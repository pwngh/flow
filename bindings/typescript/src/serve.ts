/**
 * flowd/serve — production helpers for hosting a Flow runtime.
 *
 * A Flow run is atomic: it either completes with a full trace, or — on a crash
 * mid-run — leaves nothing (the trace writer accumulates in memory and seals at
 * the end; see flow-provenance(7)). So the production model is small and boring:
 *
 *   - one Runtime per unit of work (a request, a job),
 *   - at-least-once delivery from whatever is in front of you (a queue, a retry),
 *   - idempotent mutations so a re-run can't double-fire a side effect, and
 *   - suspend/resume for waits that outlive a process (human approval, a webhook).
 *
 * These helpers make that plumbing first-class; the deployment *shape* — HTTP,
 * webhook, queue worker — stays yours. Nothing here imports a web framework or a
 * queue: a Store is the only seam, so you back idempotency and parked suspensions
 * with a file (the defaults), or with Redis/DynamoDB/S3 by implementing four
 * methods. (The mirror of the Python binding's `flowd.serve`.)
 */

import * as fs from "node:fs";
import * as path from "node:path";
import * as crypto from "node:crypto";

// `Suspension` is a runtime value (Pending.resume does `instanceof`); `Runtime`
// and `ToolImpl` are types only. TS emits named imports as lazy property reads,
// so the index <-> serve re-export cycle resolves fine.
import { Suspension, type Runtime, type ToolImpl } from "./index.js";

// ---------------------------------------------------------------------------
// A tiny pluggable key/value store. Implement these four methods over Redis,
// DynamoDB, or an object store to take any of the helpers below to production.
// `add` is the atomic primitive the concurrency-safe paths rely on: it must set
// the key only if absent and report whether it did (a compare-and-set / SETNX).
// ---------------------------------------------------------------------------
export interface Store {
  get(key: string): Buffer | null;
  put(key: string, value: Buffer): void;
  /** Set iff absent; return true iff *this* call set it (compare-and-set). */
  add(key: string, value: Buffer): boolean;
  delete(key: string): void;
}

/**
 * In-process. Node runs JS on a single thread, so each method is naturally
 * atomic, including `add`. Not shared across processes — a parked suspension
 * here is invisible to another process, so use FileStore (or a networked store)
 * when the resumer is separate.
 */
export class MemoryStore implements Store {
  private readonly d = new Map<string, Buffer>();

  get(key: string): Buffer | null {
    return this.d.get(key) ?? null;
  }
  put(key: string, value: Buffer): void {
    this.d.set(key, value);
  }
  add(key: string, value: Buffer): boolean {
    if (this.d.has(key)) return false;
    this.d.set(key, value);
    return true;
  }
  delete(key: string): void {
    this.d.delete(key);
  }
}

/**
 * One file per key under `root` (the key is hashed for a safe filename). `put`
 * writes a uniquely-named temp file and renames it on top, so a reader never
 * sees a half file and concurrent writers don't clobber a shared temp; `add`
 * hard-links the temp onto the target — `link` is atomic and fails if the target
 * exists, so the file only ever becomes visible already-full (no empty window a
 * create-then-write would leave). Durable and shareable on a common volume —
 * enough for a single host or an NFS/EFS mount (hard-link CAS is unreliable on
 * older NFS; use a networked store with real CAS for multi-host).
 */
export class FileStore implements Store {
  private readonly root: string;

  constructor(root: string) {
    this.root = root;
    fs.mkdirSync(this.root, { recursive: true });
  }

  private pathFor(key: string): string {
    const h = crypto.createHash("sha256").update(key, "utf8").digest("hex");
    return path.join(this.root, `${h}.json`);
  }

  get(key: string): Buffer | null {
    try {
      return fs.readFileSync(this.pathFor(key));
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code === "ENOENT") return null;
      throw e;
    }
  }

  put(key: string, value: Buffer): void {
    const p = this.pathFor(key);
    const tmp = `${p}.${crypto.randomUUID()}.tmp`; // unique per writer
    fs.writeFileSync(tmp, value);
    fs.renameSync(tmp, p); // atomic on POSIX
  }

  add(key: string, value: Buffer): boolean {
    const p = this.pathFor(key);
    const tmp = `${p}.${crypto.randomUUID()}.tmp`;
    fs.writeFileSync(tmp, value);
    try {
      fs.linkSync(tmp, p); // atomic create-if-absent; throws EEXIST if present
      return true;
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code === "EEXIST") return false;
      throw e;
    } finally {
      try {
        fs.unlinkSync(tmp);
      } catch {
        /* temp already gone */
      }
    }
  }

  delete(key: string): void {
    try {
      fs.unlinkSync(this.pathFor(key));
    } catch (e) {
      if ((e as NodeJS.ErrnoException).code !== "ENOENT") throw e;
    }
  }
}

// ---------------------------------------------------------------------------
// Idempotent mutations
// ---------------------------------------------------------------------------

// Tool callbacks run synchronously (libflowd blocks on each), so the rare
// idempotency-contention wait below is synchronous too — block the thread
// without busy-spinning. Reuses one buffer; no one ever notifies it.
const SLEEP_BUF = new Int32Array(new SharedArrayBuffer(4));
function sleepSync(ms: number): void {
  Atomics.wait(SLEEP_BUF, 0, 0, ms);
}

/** Deterministic JSON (keys sorted at every level) — for the default dedup key. */
function stableStringify(v: unknown): string {
  if (v === null || typeof v !== "object") return JSON.stringify(v) ?? "null";
  if (Array.isArray(v)) return "[" + v.map(stableStringify).join(",") + "]";
  const o = v as Record<string, unknown>;
  return (
    "{" +
    Object.keys(o)
      .sort()
      .map((k) => JSON.stringify(k) + ":" + stableStringify(o[k]))
      .join(",") +
    "}"
  );
}

export interface IdempotentOptions {
  /** Logical business key (an order id, a message-id) that dedups across runs,
   * not just identical-input retries. Default: the tool name + a hash of inputs. */
  key?: (args: Record<string, unknown>) => string;
  /** Name for the default key + diagnostics (anonymous tool fns have none). */
  name?: string;
  /** How long a loser waits for the winner's result before throwing (ms). */
  waitMs?: number;
  /** Poll interval while waiting (ms). */
  pollMs?: number;
}

/**
 * Wrap a level-3 (`mutation`) tool so a repeat invocation with the same key
 * returns the first result instead of re-firing the side effect.
 *
 * This is what makes at-least-once delivery safe: if a job is redelivered and the
 * whole flow re-runs, the mutation it already performed becomes a no-op that
 * replays the recorded result, so no second email is sent / charge made. (flowd
 * already guarantees replay never re-invokes a mutation; this closes the *retry*
 * side of the same hole — see flow-effects(7).)
 *
 *     const store = new FileStore("state/idem");
 *     const tools = {
 *       notify_applicant: idempotent(store, ({ email, band }) => {  // default key: a hash of inputs
 *         sendEmail(email as string, band as string);
 *         return true;
 *       }),
 *     };
 *
 * For a logical business key (an order id, an email message-id) pass `key`; that
 * dedups across runs, not just retries of identical inputs.
 *
 * Concurrency: the key is *claimed* atomically (`Store.add`) before the side
 * effect fires, so a concurrent duplicate — in another process sharing the store
 * — does not double-fire; the loser waits up to `waitMs` for the winner's result.
 * The common case (sequential at-least-once retry) never waits. A holder that
 * crashes mid-call leaves the key claimed but unfinished; give the store a TTL,
 * or clear stuck claims, if that edge matters to you.
 */
export function idempotent(
  store: Store,
  fn: ToolImpl,
  opts: IdempotentOptions = {},
): ToolImpl {
  const name = opts.name ?? (fn.name || "tool");
  const waitMs = opts.waitMs ?? 10_000;
  const pollMs = opts.pollMs ?? 50;

  const decode = (buf: Buffer): { done: boolean; result?: unknown } =>
    JSON.parse(buf.toString("utf8")) as { done: boolean; result?: unknown };

  return (args: Record<string, unknown>): unknown => {
    const k = opts.key
      ? opts.key(args)
      : `${name}:${crypto.createHash("sha256").update(stableStringify(args)).digest("hex")}`;

    const seen = store.get(k);
    if (seen) {
      const rec = decode(seen);
      if (rec.done) return rec.result;
    }

    // Claim the key. The winner fires the side effect exactly once.
    if (store.add(k, Buffer.from('{"done":false}'))) {
      const result = fn(args);
      store.put(k, Buffer.from(JSON.stringify({ done: true, result: result ?? null })));
      return result;
    }

    // Lost the claim: another caller is in flight — wait for its result.
    const deadline = Date.now() + waitMs;
    while (Date.now() < deadline) {
      const cur = store.get(k);
      if (cur) {
        const rec = decode(cur);
        if (rec.done) return rec.result;
      }
      sleepSync(pollMs);
    }
    throw new Error(
      `idempotent(${name}): another call holds key ${k} but did not finish within ${waitMs}ms`,
    );
  };
}

// ---------------------------------------------------------------------------
// Park & resume a suspended run
// ---------------------------------------------------------------------------

interface Parked {
  token: string;
  context: Record<string, unknown>;
}

/**
 * Durable parking for suspended runs.
 *
 * A suspending tool makes `Runtime.run` return a `Suspension`; its token is a
 * handle to the suspended trace on disk, so resumption can happen later, in a
 * *different process* (a webhook, a scheduler) from a fresh Runtime built on the
 * same IR — flowd writes a new, linked trace and leaves the suspended one
 * untouched (flow-suspension(7)).
 *
 *     const pending = new Pending(new FileStore("pending"));
 *
 *     const out = rt.run(application);
 *     if (out instanceof Suspension) {
 *       pending.park(out.token, { applicationId });
 *       return; // the process is free to exit; the token is durable
 *     }
 *
 *     // ... later, in the webhook handler, on a fresh runtime: ...
 *     const final = pending.resume(rt, token, decision); // final value, or re-parks
 *
 * `park` stores the token plus any `context` the resumer needs to correlate the
 * callback. `resume` only proceeds for a token this store actually parked (so a
 * webhook can't be tricked into resuming an arbitrary path), claims the resume
 * atomically so a duplicate delivery can't resume twice, clears the parked entry
 * on completion, and re-parks if the run suspends again.
 */
export class Pending {
  constructor(private readonly store: Store) {}

  private static key(token: string): string {
    return "pending:" + token;
  }

  park(token: string, context?: Record<string, unknown>): string {
    this.store.put(
      Pending.key(token),
      Buffer.from(JSON.stringify({ token, context: context ?? {} })),
    );
    return token;
  }

  get(token: string): Parked | null {
    const raw = this.store.get(Pending.key(token));
    return raw ? (JSON.parse(raw.toString("utf8")) as Parked) : null;
  }

  resume(runtime: Runtime, token: string, decision: unknown): unknown {
    const parked = this.get(token);
    if (parked === null) throw new Error("no parked suspension for this token");
    // Claim the resume so a duplicate webhook delivery can't double-resume.
    if (!this.store.add("resuming:" + token, Buffer.from("1"))) {
      throw new Error("suspension is already being resumed");
    }
    let out: unknown;
    try {
      out = runtime.resume(token, decision);
    } catch (e) {
      this.store.delete("resuming:" + token); // let a corrected decision retry
      throw e;
    }
    this.store.delete(Pending.key(token));
    if (out instanceof Suspension) {
      // Suspended again: re-park under the new token, carrying the context.
      this.park(out.token, parked.context);
    }
    return out;
  }
}
