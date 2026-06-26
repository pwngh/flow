"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const { generate } = require("../dist/codegen.js");

const FIX = path.join(__dirname, "fixtures");
const readIr = (n) => fs.readFileSync(path.join(FIX, `${n}.ir.json`), "utf8");
const DIST_INDEX = path.resolve(__dirname, "..", "dist", "index.js");

test("generate() emits typed interfaces, a Tools type, and a class", () => {
  const out = generate(readIr("onboard"));
  // record types -> interfaces with mapped TS types
  assert.match(out, /export interface Customer \{[\s\S]*id: string;[\s\S]*amount: number;[\s\S]*\}/);
  assert.match(out, /export interface Decision \{[\s\S]*approved: boolean;[\s\S]*score: number;[\s\S]*\}/);
  // camelCase Tools interface
  assert.match(out, /export interface OnboardTools \{/);
  assert.match(out, /creditCheck\(args: \{ id: string \}\): number;/);
  assert.match(out, /recordDecision\(args: \{ id: string; approved: boolean \}\): boolean;/);
  // wrapper class + camelCase -> snake_case registration
  assert.match(out, /export class Onboard \{/);
  assert.match(out, /"credit_check": \(a\) => impl\.creditCheck/);
  assert.match(out, /"record_decision": \(a\) => impl\.recordDecision/);
});

test("generate() maps sum types to a discriminated union", () => {
  const out = generate(readIr("gate"));
  assert.match(out, /export interface Approval \{[\s\S]*approver: string;[\s\S]*ok: boolean;[\s\S]*\}/);
});

test("generate() rejects an unsupported ir_version", () => {
  const doc = JSON.parse(readIr("triage"));
  doc.ir_version = "2.0";
  assert.throws(() => generate(JSON.stringify(doc)), /ir_version/);
});

test("the generated contract transpiles and drives the runtime", () => {
  // Point the generated import at the built dist so require() resolves.
  const tsSource = generate(readIr("onboard"), DIST_INDEX);
  const ts = require("typescript");
  const js = ts.transpileModule(tsSource, {
    compilerOptions: { module: ts.ModuleKind.CommonJS, target: ts.ScriptTarget.ES2022 },
  }).outputText;

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cgrun_"));
  const modPath = path.join(dir, "onboard.flow.cjs");
  fs.writeFileSync(modPath, js);

  const { Onboard } = require(modPath);
  const td = fs.mkdtempSync(path.join(os.tmpdir(), "cgtr_"));
  const onboard = new Onboard(
    {
      creditCheck: ({ id }) => (id === "cust_42" ? 750 : 100),
      recordDecision: () => true,
    },
    { traceDir: td, implVersion: "cg-1" },
  );
  assert.deepEqual(onboard.run({ id: "cust_42", amount: 5000 }), { approved: true, score: 750 });
  onboard.close();
});
