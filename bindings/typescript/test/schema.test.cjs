"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const path = require("node:path");

const { jsonSchemaFor } = require("../dist/index.js");

const ir = (n) => path.join(__dirname, "fixtures", `${n}.ir.json`);

test("primitives", () => {
  assert.deepEqual(jsonSchemaFor(ir("score"), "int"), { type: "integer" });
  assert.deepEqual(jsonSchemaFor(ir("score"), "string"), { type: "string" });
  assert.deepEqual(jsonSchemaFor(ir("score"), "bool"), { type: "boolean" });
  assert.deepEqual(jsonSchemaFor(ir("score"), "float"), { type: "number" });
});

test("record", () => {
  assert.deepEqual(jsonSchemaFor(ir("score"), "RiskReport"), {
    type: "object",
    properties: { score: { type: "integer" }, flagged: { type: "boolean" } },
    required: ["score", "flagged"],
    additionalProperties: false,
  });
});

test("list", () => {
  const doc = { ir_version: "1.0", types: [
    { name: "Bag", kind: "record", fields: [{ name: "items", type: "[string]" }] },
  ] };
  assert.deepEqual(jsonSchemaFor(JSON.stringify(doc), "Bag").properties.items, {
    type: "array", items: { type: "string" },
  });
});

test("unknown named type is permissive", () => {
  assert.deepEqual(jsonSchemaFor({ ir_version: "1.0", types: [] }, "Mystery"), { type: "object" });
});
