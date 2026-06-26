"""flowd.serve — production helpers for hosting a Flow runtime.

A Flow run is atomic: it either completes with a full trace, or — on a crash
mid-run — leaves nothing (the trace writer accumulates in memory and seals at the
end; see flow-provenance(7)). So the production model is small and boring:

  * one Runtime per unit of work (a request, a job),
  * at-least-once delivery from whatever is in front of you (a queue, a retry),
  * idempotent mutations so a re-run can't double-fire a side effect, and
  * suspend/resume for waits that outlive a process (human approval, a webhook).

These helpers make that plumbing first-class. The deployment *shape* — HTTP,
webhook, queue worker — stays yours; see examples/serving/ for runnable ones.

Nothing here imports a web framework or a queue: a Store is the only seam, so you
back idempotency and parked suspensions with a file (the defaults), or with
Redis/Dynamo/S3 by implementing four methods.
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
import time
import uuid
from pathlib import Path
from typing import Any, Callable, Optional, Protocol

from . import Runtime, Suspension

__all__ = ["Store", "MemoryStore", "FileStore", "idempotent", "Pending"]


# --------------------------------------------------------------------------
# A tiny pluggable key/value store. Implement these four methods over Redis,
# DynamoDB, or an object store to take any of the helpers below to production.
# `add` is the atomic primitive the concurrency-safe paths rely on: it must set
# the key only if absent and report whether it did (a compare-and-set / SETNX).
# --------------------------------------------------------------------------
class Store(Protocol):
    def get(self, key: str) -> Optional[bytes]: ...
    def put(self, key: str, value: bytes) -> None: ...
    def add(self, key: str, value: bytes) -> bool: ...  # set iff absent; True if set
    def delete(self, key: str) -> None: ...


class MemoryStore:
    """In-process. Every method is individually atomic (guarded by one lock),
    including `add`, so two threads racing the same key resolve correctly. Not
    shared across processes — a parked suspension here is invisible to another
    process, so use FileStore (or a networked store) when the resumer is
    separate."""

    def __init__(self) -> None:
        self._d: dict[str, bytes] = {}
        self._lock = threading.Lock()

    def get(self, key: str) -> Optional[bytes]:
        with self._lock:
            return self._d.get(key)

    def put(self, key: str, value: bytes) -> None:
        with self._lock:
            self._d[key] = value

    def add(self, key: str, value: bytes) -> bool:
        with self._lock:
            if key in self._d:
                return False
            self._d[key] = value
            return True

    def delete(self, key: str) -> None:
        with self._lock:
            self._d.pop(key, None)


class FileStore:
    """One JSON file per key under `root` (the key is hashed for a safe
    filename). `put` writes a uniquely-named temp file and renames it on, so a
    reader never sees a half file and concurrent writers don't clobber a shared
    temp; `add` uses an exclusive create (O_CREAT|O_EXCL), atomic across
    processes on a local FS. Durable and shareable on a common volume — enough
    for a single host or an NFS/EFS mount (O_EXCL is not reliable on older NFS;
    use a networked store with real CAS for multi-host)."""

    def __init__(self, root: str | os.PathLike[str]) -> None:
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def _path(self, key: str) -> Path:
        return self.root / (hashlib.sha256(key.encode("utf-8")).hexdigest() + ".json")

    def get(self, key: str) -> Optional[bytes]:
        p = self._path(key)
        return p.read_bytes() if p.exists() else None

    def put(self, key: str, value: bytes) -> None:
        p = self._path(key)
        tmp = p.with_name(f"{p.name}.{uuid.uuid4().hex}.tmp")  # unique per writer
        tmp.write_bytes(value)
        os.replace(tmp, p)  # atomic on POSIX

    def add(self, key: str, value: bytes) -> bool:
        # Write the content to a unique temp first, then hard-link it onto the
        # target: os.link is atomic and fails if the target exists, so the file
        # only ever becomes visible already-full — a concurrent get() never sees
        # the empty window an O_EXCL-create-then-write would leave.
        p = self._path(key)
        tmp = p.with_name(f"{p.name}.{uuid.uuid4().hex}.tmp")
        tmp.write_bytes(value)
        try:
            os.link(tmp, p)
            return True
        except FileExistsError:
            return False
        finally:
            tmp.unlink(missing_ok=True)

    def delete(self, key: str) -> None:
        self._path(key).unlink(missing_ok=True)


# --------------------------------------------------------------------------
# Idempotent mutations
# --------------------------------------------------------------------------
def idempotent(store: Store, *, key: Optional[Callable[..., str]] = None,
               wait: float = 10.0, poll: float = 0.05):
    """Wrap a level-3 (`mutation`) tool method so a repeat invocation with the
    same key returns the first result instead of re-firing the side effect.

    This is what makes at-least-once delivery safe: if a job is redelivered and
    the whole flow re-runs, the mutation it already performed becomes a no-op
    that replays the recorded result, so no second email is sent / charge made.
    (flowd already guarantees replay never re-invokes a mutation; this closes
    the *retry* side of the same hole — see flow-effects(7).)

        class Host(Onboard):
            @idempotent(store)                       # default key: hash of inputs
            def notify_applicant(self, email, band):
                send_email(email, f"risk band: {band}")
                return True

    The default key is the method name plus a hash of its inputs — right for
    "the same job retried." For a logical business key (an order id, an email
    message-id) pass `key=lambda **inputs: ...`; that dedups across runs, not
    just retries of identical inputs.

    Concurrency: the key is *claimed* atomically (`Store.add`) before the side
    effect fires, so a concurrent duplicate — in this process or another sharing
    the store — does not double-fire; the loser waits up to `wait` seconds for
    the winner's result. The common case (sequential at-least-once retry) never
    waits. A holder that crashes mid-call leaves the key claimed but unfinished;
    give the store a TTL, or clear stuck claims, if that edge matters to you.
    """

    def decorate(fn: Callable[..., Any]) -> Callable[..., Any]:
        name = getattr(fn, "__name__", "tool")

        def wrapper(self, **inputs):
            if key is not None:
                k = key(**inputs)
            else:
                digest = hashlib.sha256(
                    json.dumps(inputs, sort_keys=True, default=str).encode("utf-8")
                ).hexdigest()
                k = f"{name}:{digest}"

            seen = store.get(k)
            if seen is not None:
                rec = json.loads(seen)
                if rec.get("done"):
                    return rec["result"]

            # Claim the key. The winner fires the side effect exactly once.
            if store.add(k, b'{"done": false}'):
                result = fn(self, **inputs)
                store.put(k, json.dumps({"done": True, "result": result},
                                        default=str).encode("utf-8"))
                return result

            # Lost the claim: another caller is in flight — wait for its result.
            deadline = time.monotonic() + wait
            while time.monotonic() < deadline:
                seen = store.get(k)
                if seen is not None:
                    rec = json.loads(seen)
                    if rec.get("done"):
                        return rec["result"]
                time.sleep(poll)
            raise TimeoutError(
                f"idempotent({name}): another call holds key {k!r} but did not "
                f"finish within {wait}s")

        wrapper.__name__ = name
        wrapper.__doc__ = fn.__doc__
        wrapper.__wrapped__ = fn
        return wrapper

    return decorate


# --------------------------------------------------------------------------
# Park & resume a suspended run
# --------------------------------------------------------------------------
class Pending:
    """Durable parking for suspended runs.

    A suspending tool makes `Runtime.run` return a `Suspension`; its token is a
    handle to the suspended trace on disk, so resumption can happen later, in a
    *different process* (a webhook, a scheduler) from a fresh Runtime built on
    the same IR — flowd writes a new, linked trace and leaves the suspended one
    untouched (flow-suspension(7)).

        pending = Pending(FileStore("pending"))

        out = rt.run(application)
        if isinstance(out, Suspension):
            pending.park(out.token, context={"application_id": app_id})
            return  # the process is free to exit; the token is durable

        # ... later, in the webhook handler, a fresh runtime: ...
        out = pending.resume(rt, token, decision)   # final value, or re-parks

    `park` stores the token plus any `context` the resumer needs to correlate
    the callback. `resume` only proceeds for a token this store actually parked
    (so a webhook can't be tricked into resuming an arbitrary path), claims the
    resume atomically so a duplicate delivery can't resume twice, clears the
    parked entry on completion, and re-parks if the run suspends again.
    """

    def __init__(self, store: Store) -> None:
        self._store = store

    @staticmethod
    def _key(token: str) -> str:
        return "pending:" + token

    def park(self, token: str, context: Optional[dict] = None) -> str:
        self._store.put(
            self._key(token),
            json.dumps({"token": token, "context": context or {}}).encode("utf-8"),
        )
        return token

    def get(self, token: str) -> Optional[dict]:
        raw = self._store.get(self._key(token))
        return json.loads(raw) if raw is not None else None

    def resume(self, runtime: Runtime, token: str, decision: Any) -> Any:
        parked = self.get(token)
        if parked is None:
            raise KeyError("no parked suspension for this token")
        # Claim the resume so a duplicate webhook delivery can't double-resume.
        if not self._store.add("resuming:" + token, b"1"):
            raise RuntimeError("suspension is already being resumed")
        try:
            out = runtime.resume(token, decision)
        except Exception:
            self._store.delete("resuming:" + token)  # let a corrected decision retry
            raise
        self._store.delete(self._key(token))
        if isinstance(out, Suspension):  # suspended again: re-park under the new token
            self.park(out.token, context=(parked or {}).get("context"))
        return out
