"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");

const { secretRedactor } = require("../dist/index.js");

test("scrubs known key shapes", () => {
  const out = secretRedactor()(Buffer.from("k=sk-ant-abcDEF012345678901234567 aws=AKIAABCDEFGHIJKLMNOP"));
  const s = out.toString();
  assert.ok(!s.includes("sk-ant-"));
  assert.ok(!s.includes("AKIA"));
  assert.ok(s.includes("[REDACTED]"));
});

test("returns null when nothing matches", () => {
  assert.equal(secretRedactor()(Buffer.from("a normal trace value")), null);
});

test("extra RegExp", () => {
  const out = secretRedactor(/tok_live_[a-z0-9]+/g)(Buffer.from("a tok_live_abc123 here"));
  assert.ok(!out.toString().includes("tok_live_abc123"));
});

test("custom replacement", () => {
  const out = secretRedactor({ replacement: "X" })(Buffer.from("sk-ant-abcDEF012345678901234567"));
  assert.equal(out.toString(), "X");
});
