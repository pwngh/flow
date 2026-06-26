"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { Runtime, Suspension } = require("../dist/index.js");

const FIX = path.join(__dirname, "fixtures");
const ir = (n) => path.join(FIX, `${n}.ir.json`);
const tmp = () => fs.mkdtempSync(path.join(os.tmpdir(), "flowdts_"));
const gateRuntime = (td) =>
  new Runtime(ir("gate"), { traceDir: td, tools: { await_human_approval: () => null } });

test("a suspending tool returns a Suspension with a token", () => {
  const rt = gateRuntime(tmp());
  const r = rt.run("approve?");
  assert.ok(r instanceof Suspension);
  assert.equal(typeof r.token, "string");
  assert.ok(r.token.length > 0);
  rt.close();
});

test("the original trace's manifest stays suspended", () => {
  const td = tmp();
  const rt = gateRuntime(td);
  rt.run("approve?");
  rt.close();
  const manifest = fs.readdirSync(path.join(td, "gate"))
    .map((d) => path.join(td, "gate", d, "manifest.json"))
    .find((p) => fs.existsSync(p));
  assert.equal(JSON.parse(fs.readFileSync(manifest, "utf8")).status, "suspended");
});

test("resume completes with the decision payload", () => {
  const rt = gateRuntime(tmp());
  const s = rt.run("approve?");
  const decision = { approver: "compliance@example.com", ok: true };
  assert.deepEqual(s.resume(decision), decision);
  rt.close();
});

test("resume writes a new trace, leaving the original (two exec dirs)", () => {
  const td = tmp();
  const rt = gateRuntime(td);
  const s = rt.run("approve?");
  s.resume({ approver: "a@b", ok: false });
  rt.close();
  const execs = fs.readdirSync(path.join(td, "gate")).filter((d) => d.startsWith("exec_"));
  assert.equal(execs.length, 2);
});
