"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { Runtime, openTrace, diffTraces } = require("../dist/index.js");

const FIX = path.join(__dirname, "fixtures");
const ir = (n) => path.join(FIX, `${n}.ir.json`);
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "tsrp_"));
const execDir = (root) => {
  const d = path.join(root, "score");
  return path.join(d, fs.readdirSync(d).find((x) => x.startsWith("exec_")));
};

function base(root, score = 10) {
  const rt = new Runtime(ir("score"), {
    traceDir: root,
    tools: { risk_check: () => ({ score, flagged: false }) },
  });
  const out = rt.run(750);
  rt.close();
  return [out, execDir(root)];
}

const v2 = {
  name: "stub-v2",
  supportsModel: (m) => m === "risk-v2",
  invoke: () => ({ score: 99, flagged: true }),
};

test("model-versioned replay re-invokes the model and changes the output", () => {
  const root = tmp();
  const [baseOut, baseDir] = base(root);
  assert.equal(baseOut, 10);

  const ro = Runtime.open(ir("score"), { traceDir: root });
  ro.registerProvider(v2);
  const repRoot = path.join(root, "rep");
  const repOut = ro.replay(baseDir, repRoot, { model: "risk-v2" });
  ro.close();

  assert.equal(repOut, 99);
  const d = diffTraces(baseDir, execDir(repRoot));
  assert.ok(d.outputChanged);
  const mc = d.changes.find((c) => c.nodeKind === "model_call");
  assert.equal(mc.modelBefore, "risk-v1");
  assert.equal(mc.modelAfter, "risk-v2");
});

test("same-model replay restores every node (identical trace)", () => {
  const root = tmp();
  const [baseOut, baseDir] = base(root);
  const ro = Runtime.open(ir("score"), { traceDir: root });
  const smRoot = path.join(root, "sm");
  const smOut = ro.replay(baseDir, smRoot, {}); // no model -> restore
  ro.close();
  assert.equal(smOut, baseOut);
  assert.ok(!diffTraces(baseDir, execDir(smRoot)).hasChanges);
});

test("provider invokeWithMetrics records tokens/cost in the trace", () => {
  const root = tmp();
  const [, baseDir] = base(root);
  const ro = Runtime.open(ir("score"), { traceDir: root });
  ro.registerProvider({
    name: "metered",
    supportsModel: (m) => m === "risk-v1",
    invoke: () => ({ score: 5, flagged: false }),
    invokeWithMetrics: () => ({
      response: { score: 5, flagged: false },
      tokensIn: 42,
      tokensOut: 7,
      costCents: 0.13,
    }),
  });
  const mRoot = path.join(root, "m");
  ro.replay(baseDir, mRoot, { model: "risk-v1" });
  ro.close();
  const t = openTrace(execDir(mRoot));
  assert.equal(t.budget.tokens_in, 42);
  assert.equal(t.budget.tokens_out, 7);
  assert.equal(t.modelCalls[0].tokensIn, 42);
  assert.equal(t.modelCalls[0].costCents, 0.13);
});

test("Runtime.open is tool-less and replays", () => {
  const root = tmp();
  const [, baseDir] = base(root);
  const ro = Runtime.open(ir("score"));
  const out = ro.replay(baseDir, path.join(root, "x"), {});
  ro.close();
  assert.equal(out, 10);
});
