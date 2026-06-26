"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { Runtime, loadIr, FlowdError } = require("../dist/index.js");

const FIX = path.join(__dirname, "fixtures");
const ir = (n) => path.join(FIX, `${n}.ir.json`);
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "flowdts_"));

const triageImpl = (rec) => ({
  risk_check: ({ amount }) => {
    if (rec) rec.push(amount);
    return { score: Math.floor(amount / 100), flagged: amount > 1000 };
  },
});

test("loadIr introspects tools and flows", () => {
  const info = loadIr(ir("triage"));
  assert.deepEqual(info.tools, [{ name: "risk_check", level: "model" }]);
  assert.deepEqual(info.flows, ["triage"]);
});

test("run() with a scalar input computes each tier", () => {
  const rt = new Runtime(ir("triage"), { traceDir: tmp(), tools: triageImpl() });
  assert.equal(rt.run(50), 1);
  assert.equal(rt.run(750), 2);
  assert.equal(rt.run(1500), 3);
  rt.close();
});

test("model callback receives named args; output is parsed", () => {
  const seen = [];
  const rt = new Runtime(ir("triage"), { traceDir: tmp(), tools: triageImpl(seen) });
  const out = rt.run(750);
  assert.equal(typeof out, "number");
  assert.deepEqual(seen, [750]);
  rt.close();
});

test("record input + deterministic & mutation tools + construct output", () => {
  const calls = [];
  const rt = new Runtime(ir("onboard"), {
    traceDir: tmp(),
    tools: {
      credit_check: ({ id }) => { calls.push(["credit", id]); return id === "ok" ? 750 : 100; },
      record_decision: ({ id, approved }) => { calls.push(["record", id, approved]); return true; },
    },
  });
  assert.deepEqual(rt.run({ id: "ok", amount: 9000 }), { approved: true, score: 750 });
  assert.deepEqual(rt.run({ id: "no", amount: 1 }), { approved: false, score: 100 });
  assert.deepEqual(calls, [
    ["credit", "ok"], ["record", "ok", true],
    ["credit", "no"], ["record", "no", false],
  ]);
  rt.close();
});

test("run_named with an unknown flow throws R155", () => {
  const rt = new Runtime(ir("triage"), { traceDir: tmp(), tools: triageImpl() });
  assert.throws(() => rt.run(10, { flow: "nope" }), (e) => e instanceof FlowdError && e.code === "R155");
  rt.close();
});

test("a throwing callback surfaces as FlowdError", () => {
  const rt = new Runtime(ir("triage"), {
    traceDir: tmp(),
    tools: { risk_check: () => { throw new Error("boom"); } },
  });
  assert.throws(() => rt.run(750), FlowdError);
  rt.close();
});

test("an async (Promise-returning) callback is rejected", () => {
  const rt = new Runtime(ir("triage"), {
    traceDir: tmp(),
    tools: { risk_check: async () => ({ score: 0, flagged: false }) },
  });
  assert.throws(() => rt.run(750), FlowdError);
  rt.close();
});

test("a missing tool implementation is rejected at construction", () => {
  assert.throws(() => new Runtime(ir("onboard"), { traceDir: tmp(), tools: { credit_check: () => 1 } }), FlowdError);
});

test("unsupported ir_version is rejected", () => {
  const doc = JSON.parse(fs.readFileSync(ir("triage"), "utf8"));
  doc.ir_version = "2.0";
  assert.throws(() => new Runtime(JSON.stringify(doc), { traceDir: tmp(), tools: triageImpl() }), /ir_version/);
});

test("a closed Runtime rejects further calls", () => {
  const rt = new Runtime(ir("triage"), { traceDir: tmp(), tools: triageImpl() });
  rt.close();
  assert.throws(() => rt.run(750), /closed/);
});

test("a trace directory is written per execution", () => {
  const td = tmp();
  const rt = new Runtime(ir("triage"), { traceDir: td, tools: triageImpl() });
  rt.run(750);
  rt.close();
  const manifests = fs.readdirSync(path.join(td, "triage"))
    .filter((d) => fs.existsSync(path.join(td, "triage", d, "manifest.json")));
  assert.equal(manifests.length, 1);
});

test("Symbol.dispose closes the runtime", () => {
  let handle;
  {
    const rt = new Runtime(ir("triage"), { traceDir: tmp(), tools: triageImpl() });
    handle = rt;
    rt[Symbol.dispose]();
  }
  assert.throws(() => handle.run(750), /closed/);
});
