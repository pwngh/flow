"""flowd.serve helpers: stores, idempotent mutations, suspension park/resume."""

import json
import threading
import time

import pytest

from flowd import Runtime, Suspension, load_ir
from flowd.serve import FileStore, MemoryStore, Pending, idempotent


def test_file_store_round_trip(tmp_path):
    s = FileStore(tmp_path / "kv")
    assert s.get("missing") is None
    s.put("k", b"v1")
    assert s.get("k") == b"v1"
    s.put("k", b"v2")  # atomic overwrite
    assert s.get("k") == b"v2"
    s.delete("k")
    assert s.get("k") is None


def test_file_store_add_is_exclusive_and_atomic(tmp_path):
    """Eight threads race FileStore.add on one key: exactly one claims it, and a
    reader only ever sees the complete value (never the empty-create window)."""
    s = FileStore(tmp_path / "kv")
    results: list = []
    start = threading.Barrier(8)

    def claim(i):
        start.wait()
        results.append(s.add("k", json.dumps({"by": i}).encode()))

    threads = [threading.Thread(target=claim, args=(i,)) for i in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert results.count(True) == 1            # exactly one writer won
    assert "by" in json.loads(s.get("k"))      # value is complete, not empty


def test_idempotent_runs_side_effect_once_per_key():
    store = MemoryStore()
    sends: list[tuple] = []

    class Host:
        @idempotent(store)
        def notify(self, email, band):
            sends.append((email, band))
            return {"sent": True}

    h = Host()
    # same inputs twice -> side effect once, recorded result replayed
    assert h.notify(email="a@b", band="LOW") == {"sent": True}
    assert h.notify(email="a@b", band="LOW") == {"sent": True}
    assert sends == [("a@b", "LOW")]
    # different inputs -> a new side effect
    h.notify(email="c@d", band="HIGH")
    assert len(sends) == 2


def test_idempotent_business_key_dedups_across_inputs():
    store = MemoryStore()
    sends: list = []

    class Host:
        @idempotent(store, key=lambda **kw: f"order:{kw['order_id']}")
        def charge(self, order_id, amount):
            sends.append(amount)
            return {"charged": amount}

    h = Host()
    assert h.charge(order_id="o1", amount=100) == {"charged": 100}
    # same business key, different amount -> still deduped (the first wins)
    assert h.charge(order_id="o1", amount=999) == {"charged": 100}
    assert sends == [100]


def test_idempotent_concurrent_duplicates_fire_once():
    """Eight threads racing the same key: the side effect fires exactly once and
    every caller gets the recorded result (the atomic claim, not just the lock)."""
    store = MemoryStore()
    sends: list = []
    sends_lock = threading.Lock()

    class Host:
        @idempotent(store)
        def charge(self, amount):
            time.sleep(0.05)  # widen the race window
            with sends_lock:
                sends.append(amount)
            return {"ok": amount}

    h = Host()
    results: list = []
    start = threading.Barrier(8)

    def call():
        start.wait()
        results.append(h.charge(amount=100))

    threads = [threading.Thread(target=call) for _ in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    assert results == [{"ok": 100}] * 8   # all callers see the same result
    assert sends == [100]                 # ... but the side effect fired once


def test_pending_resume_only_what_was_parked():
    """resume rejects a token this store never parked — the control that keeps a
    webhook from being pointed at an arbitrary path."""
    pending = Pending(MemoryStore())

    class FakeRuntime:
        def resume(self, token, decision):  # never reached
            return decision

    with pytest.raises(KeyError):
        pending.resume(FakeRuntime(), "/some/arbitrary/path", {"ok": True})


def test_pending_park_and_cross_runtime_resume(tmp_path, ir):
    """A suspension parked by one runtime resumes from a fresh one — the
    process-crossing path a webhook handler takes."""
    Gate = load_ir(ir("gate"))

    class Impl(Gate):
        def await_human_approval(self, prompt):
            return None  # runtime intercepts

    pending = Pending(FileStore(tmp_path / "pending"))
    traces = str(tmp_path / "traces")

    with Runtime(Impl(), trace_dir=traces) as rt:
        out = rt.run("approve transfer?")
        assert isinstance(out, Suspension)
        token = out.token
        pending.park(token, context={"application_id": "app_42"})

    # token survives the runtime; context is recoverable for correlation
    assert pending.get(token)["context"] == {"application_id": "app_42"}

    # a *fresh* runtime resumes it, then the parked entry is cleared
    decision = {"approver": "compliance@example.com", "ok": True}
    with Runtime(Impl(), trace_dir=traces) as rt2:
        final = pending.resume(rt2, token, decision)
    assert final == decision
    assert pending.get(token) is None

    # resumed exactly once: a second resume of the same token is rejected
    with Runtime(Impl(), trace_dir=traces) as rt3:
        with pytest.raises(KeyError):
            pending.resume(rt3, token, decision)
