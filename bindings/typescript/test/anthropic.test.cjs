"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { Runtime, openTrace, anthropicProvider } = require("../dist/index.js");

const ir = (n) => path.join(__dirname, "fixtures", `${n}.ir.json`);
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "tsan_"));
const execDir = (root) => {
  const d = path.join(root, "score");
  return path.join(d, fs.readdirSync(d).find((x) => x.startsWith("exec_")));
};

// A stub Claude call (no SDK / network).
function stub(text = { score: 7, flagged: true }, usage = { tokensIn: 11, tokensOut: 5 }) {
  const calls = [];
  const call = (req) => {
    calls.push(req);
    return { response: text, ...usage };
  };
  return { call, calls };
}

function runScore(root, provider) {
  const rt = new Runtime(ir("score"), {
    traceDir: root,
    tools: { risk_check: () => { throw new Error("provider must serve the model"); } },
  });
  rt.registerProvider(provider);
  const out = rt.run(750);
  rt.close();
  return out;
}

test("serves a model tool with zero adapter code", () => {
  const s = stub();
  assert.equal(runScore(tmp(), anthropicProvider(ir("score"), { call: s.call })), 7);
});

test("derives the structured-output schema from the IR", () => {
  const s = stub();
  runScore(tmp(), anthropicProvider(ir("score"), { call: s.call }));
  const schema = s.calls[0].schema;
  assert.deepEqual(Object.keys(schema.properties), ["score", "flagged"]);
  assert.equal(schema.additionalProperties, false);
  assert.equal(s.calls[0].model, "claude-opus-4-8");
});

test("per-model real model + instructions", () => {
  const s = stub();
  runScore(tmp(), anthropicProvider(ir("score"), {
    call: s.call,
    model: { "risk-v1": "claude-haiku-4-5" },
    instructions: { "risk-v1": "Be terse." },
  }));
  assert.equal(s.calls[0].model, "claude-haiku-4-5");
  assert.equal(s.calls[0].system, "Be terse.");
});

test("metrics flow into the trace", () => {
  const s = stub();
  const root = tmp();
  runScore(root, anthropicProvider(ir("score"), { call: s.call }));
  const t = openTrace(execDir(root));
  assert.equal(t.budget.tokens_in, 11);
  assert.equal(t.budget.tokens_out, 5);
});

test("throws when the IR has no model tools", () => {
  const doc = { ir_version: "1.0", types: [], tools: [
    { name: "t", input: [], output: "int", effect: { level: "deterministic" } },
  ], flows: [] };
  assert.throws(() => anthropicProvider(doc, { call: stub().call }), /no model/);
});

// Exercise the DEFAULT subprocess bridge (no `call` override) against a fake
// worker, so the execFileSync round-trip is covered without the real SDK.
test("the subprocess bridge round-trips through the worker", () => {
  const root = tmp();
  const worker = path.join(root, "fake-worker.mjs");
  fs.writeFileSync(
    worker,
    'let s="";process.stdin.on("data",c=>s+=c);' +
      'process.stdin.on("end",()=>{const r=JSON.parse(s);' +
      'process.stdout.write(JSON.stringify({response:{score:3,flagged:false},tokensIn:2,tokensOut:1,_model:r.model}));});',
  );
  process.env.FLOWD_ANTHROPIC_WORKER = worker;
  try {
    const out = runScore(root, anthropicProvider(ir("score"))); // no call override -> real bridge
    assert.equal(out, 3);
    const t = openTrace(execDir(root));
    assert.equal(t.budget.tokens_in, 2);
  } finally {
    delete process.env.FLOWD_ANTHROPIC_WORKER;
  }
});
