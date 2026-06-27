"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const {
  Runtime, Suspension, MemoryStore, FileStore, idempotent, Pending,
} = require("../dist/index.js");

const FIX = path.join(__dirname, "fixtures");
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "flowdserve_"));
const gateRuntime = (td) =>
  new Runtime(path.join(FIX, "gate.ir.json"), {
    traceDir: td,
    tools: { await_human_approval: () => null },
  });

// ---- Stores -------------------------------------------------------------
for (const [label, make] of [
  ["MemoryStore", () => new MemoryStore()],
  ["FileStore", () => new FileStore(tmp())],
]) {
  test(`${label}: put / get / delete round-trip`, () => {
    const s = make();
    assert.equal(s.get("k"), null);
    s.put("k", Buffer.from("v"));
    assert.equal(s.get("k").toString(), "v");
    s.delete("k");
    assert.equal(s.get("k"), null);
  });

  test(`${label}: add is compare-and-set (sets iff absent)`, () => {
    const s = make();
    assert.equal(s.add("k", Buffer.from("first")), true);
    assert.equal(s.add("k", Buffer.from("second")), false); // already present
    assert.equal(s.get("k").toString(), "first"); // loser didn't clobber the winner
  });
}

test("FileStore persists across instances on the same root", () => {
  const root = tmp();
  new FileStore(root).put("k", Buffer.from("durable"));
  assert.equal(new FileStore(root).get("k").toString(), "durable");
});

// ---- idempotent ---------------------------------------------------------
test("idempotent: the side effect fires once across at-least-once retries", () => {
  let sends = 0;
  const notify = idempotent(new MemoryStore(), (a) => {
    sends++;
    return `sent:${a.email}`;
  });
  assert.equal(notify({ email: "a@b" }), "sent:a@b");
  assert.equal(notify({ email: "a@b" }), "sent:a@b"); // replayed, not re-fired
  assert.equal(sends, 1);
});

test("idempotent: distinct inputs fire separately; a custom key dedups across them", () => {
  let n = 0;
  const store = new MemoryStore();
  const f = idempotent(store, (a) => {
    n++;
    return a.id;
  });
  f({ id: 1 });
  f({ id: 2 });
  assert.equal(n, 2); // different inputs -> two firings

  const byUser = idempotent(store, (a) => {
    n++;
    return a.id;
  }, { key: (a) => `user:${a.user}` });
  byUser({ user: "u", id: 3 });
  byUser({ user: "u", id: 4 }); // same logical key -> fires once
  assert.equal(n, 3);
});

// ---- Pending ------------------------------------------------------------
test("Pending: park records token + context; get reads it back; unknown -> null", () => {
  const p = new Pending(new MemoryStore());
  p.park("tok-1", { applicationId: 42 });
  assert.deepEqual(p.get("tok-1"), { token: "tok-1", context: { applicationId: 42 } });
  assert.equal(p.get("missing"), null);
});

test("Pending: resume an unparked token throws", () => {
  const pending = new Pending(new MemoryStore());
  const rt = gateRuntime(tmp());
  assert.throws(() => pending.resume(rt, "never-parked", {}), /no parked suspension/);
  rt.close();
});

test("Pending: park in one runtime, resume in another (same IR + trace dir), then it's cleared", () => {
  const td = tmp();
  const pending = new Pending(new FileStore(tmp()));

  // process 1: run, suspend, park, exit.
  const rt1 = gateRuntime(td);
  const s = rt1.run("approve?");
  assert.ok(s instanceof Suspension);
  pending.park(s.token, { who: "ops" });
  rt1.close();

  // process 2: a fresh runtime on the same IR resumes from the durable token.
  const decision = { approver: "ops", ok: true };
  const rt2 = gateRuntime(td);
  assert.deepEqual(pending.resume(rt2, s.token, decision), decision);
  assert.equal(pending.get(s.token), null); // cleared on completion

  // a duplicate delivery can't resume again (the parked entry is gone).
  assert.throws(() => pending.resume(rt2, s.token, decision), /no parked suspension/);
  rt2.close();
});
