"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { Runtime } = require("../dist/index.js");

const FIX = path.join(__dirname, "fixtures");
const ir = (n) => path.join(FIX, `${n}.ir.json`);
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "flowdts_"));

function walk(p) {
  return fs.statSync(p).isDirectory()
    ? fs.readdirSync(p).flatMap((c) => walk(path.join(p, c)))
    : [p];
}
const traceBlob = (td) =>
  walk(path.join(td, "onboard"))
    .filter((f) => f.endsWith(".json"))
    .map((f) => fs.readFileSync(f, "utf8"))
    .join(" ");

test("a provider adapter serves the model ahead of the per-model method", () => {
  const served = [];
  const rt = new Runtime(ir("triage"), {
    traceDir: tmp(),
    tools: { risk_check: () => { throw new Error("method must not run when a provider serves it"); } },
  });
  rt.registerProvider({
    name: "stub",
    supportsModel: (m) => m === "risk-v1",
    invoke: (modelId, request) => { served.push([modelId, request]); return { score: 1, flagged: false }; },
  });
  assert.equal(rt.run(750), 2);
  assert.deepEqual(served, [["risk-v1", '{"amount":750}']]);
  rt.close();
});

const onboardRuntime = (td) =>
  new Runtime(ir("onboard"), {
    traceDir: td,
    tools: { credit_check: () => 800, record_decision: () => true },
  });

test("a redactor scrubs secret bytes from persisted traces", () => {
  const td = tmp();
  const rt = onboardRuntime(td);
  rt.setRedactor((buf) =>
    buf.includes("cust_42") ? Buffer.from(buf.toString().replace(/cust_42/g, "[RED]")) : null,
  );
  rt.run({ id: "cust_42", amount: 1 });
  rt.close();
  const blob = traceBlob(td);
  assert.ok(!blob.includes("cust_42"));
  assert.ok(blob.includes("[RED]"));
});

test("a redactor returning null leaves bytes untouched", () => {
  const td = tmp();
  const rt = onboardRuntime(td);
  rt.setRedactor(() => null);
  rt.run({ id: "cust_42", amount: 1 });
  rt.close();
  assert.ok(traceBlob(td).includes("cust_42"));
});

test("setRedactor(null) clears a prior redactor", () => {
  const rt = onboardRuntime(tmp());
  rt.setRedactor(() => Buffer.from("X"));
  rt.setRedactor(null);
  assert.deepEqual(rt.run({ id: "cust_42", amount: 1 }), { approved: true, score: 800 });
  rt.close();
});
