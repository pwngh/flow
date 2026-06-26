"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { Runtime, openTrace } = require("../dist/index.js");

const FIX = path.join(__dirname, "fixtures");
const ir = (n) => path.join(FIX, `${n}.ir.json`);
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "tstr_"));
const execDir = (root) => {
  const d = path.join(root, "score");
  return path.join(d, fs.readdirSync(d).find((x) => x.startsWith("exec_")));
};

function runScore(root, score = 10) {
  const rt = new Runtime(ir("score"), {
    traceDir: root,
    tools: { risk_check: () => ({ score, flagged: false }) },
  });
  rt.run(750);
  rt.close();
  return execDir(root);
}

test("trace manifest fields", () => {
  const t = openTrace(runScore(tmp()));
  assert.equal(t.flow, "score");
  assert.equal(t.status, "complete");
  assert.equal(t.input, 750);
  assert.equal(t.output, 10);
  assert.ok(t.irHash.startsWith("sha256:"));
  assert.ok(t.outputHash.startsWith("sha256:"));
});

test("nodes and the model_call carry resolved values", () => {
  const t = openTrace(runScore(tmp()));
  const kinds = new Set(t.nodes.map((n) => n.nodeKind));
  assert.ok(kinds.has("input") && kinds.has("output") && kinds.has("model_call"));
  const mc = t.modelCalls[0];
  assert.equal(mc.model, "risk-v1");
  assert.equal(mc.effectLevel, "model");
  assert.deepEqual(mc.output, { score: 10, flagged: false });
  assert.deepEqual(mc.invocations[0].inputs, { amount: 750 });
});

test("node lookup by id", () => {
  const t = openTrace(runScore(tmp()));
  assert.equal(t.node("n0").nodeKind, "input");
  assert.equal(t.node("does-not-exist"), undefined);
});

test("missing manifest throws", () => {
  assert.throws(() => openTrace(tmp()), /manifest/);
});
