/**
 * Derive a JSON Schema from an IR type.
 *
 * A model tool's output type is declared in the IR; turning it into a JSON
 * Schema lets a provider constrain an LLM to return exactly that shape (so the
 * flow never rejects a malformed model output). Pure and provider-agnostic.
 */

import * as fs from "node:fs";

interface IrField {
  name: string;
  type: string;
}
interface IrType {
  name: string;
  kind: "record" | "sum";
  fields?: IrField[];
  variants?: { name: string; fields: IrField[] }[];
}
export interface IrDocument {
  ir_version?: string;
  types?: IrType[];
  tools?: {
    name: string;
    input?: IrField[];
    output: string;
    effect: { level: string; model?: string };
  }[];
  flows?: { name: string }[];
}

/** Accept an IR as a parsed document, a path, or a JSON string. */
export function normalizeIr(ir: IrDocument | string): IrDocument {
  if (typeof ir !== "string") return ir;
  const raw = ir.trimStart().startsWith("{") ? ir : fs.readFileSync(ir, "utf8");
  return JSON.parse(raw) as IrDocument;
}

const PRIMITIVES: Record<string, object> = {
  string: { type: "string" },
  int: { type: "integer" },
  float: { type: "number" },
  bool: { type: "boolean" },
};

/** Return a JSON Schema for the IR type named by `typeStr`. */
export function jsonSchemaFor(ir: IrDocument | string, typeStr: string): object {
  const doc = normalizeIr(ir);
  const types = new Map((doc.types ?? []).map((t) => [t.name, t]));
  return schemaFor(typeStr, types);
}

function schemaFor(typeStr: string, types: Map<string, IrType>): object {
  if (typeStr in PRIMITIVES) return { ...PRIMITIVES[typeStr] };
  if (typeStr.startsWith("[") && typeStr.endsWith("]")) {
    return { type: "array", items: schemaFor(typeStr.slice(1, -1), types) };
  }
  const decl = types.get(typeStr);
  if (!decl) return { type: "object" }; // unknown named type -> permissive

  if (decl.kind === "record") {
    const props: Record<string, object> = {};
    for (const f of decl.fields ?? []) props[f.name] = schemaFor(f.type, types);
    return {
      type: "object",
      properties: props,
      required: Object.keys(props),
      additionalProperties: false,
    };
  }
  if (decl.kind === "sum") {
    // The runtime encodes a sum value as { variant, fields }.
    const arms = (decl.variants ?? []).map((v) => {
      const fprops: Record<string, object> = {};
      for (const f of v.fields) fprops[f.name] = schemaFor(f.type, types);
      return {
        type: "object",
        properties: {
          variant: { const: v.name },
          fields: { type: "object", properties: fprops, required: Object.keys(fprops), additionalProperties: false },
        },
        required: ["variant", "fields"],
        additionalProperties: false,
      };
    });
    return { anyOf: arms };
  }
  return { type: "object" };
}
