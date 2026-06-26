/**
 * Read and diff Flow execution traces.
 *
 * A trace is the durable, content-addressed record of one flow run — every
 * node's inputs, outputs, model identities, token/cost metrics, and timing
 * (flow-trace(5)). This turns a trace directory into an inspectable object
 * graph and diffs two traces node-by-node, making the record/replay/diff loop
 * usable from TypeScript.
 */

import * as fs from "node:fs";
import * as path from "node:path";

function resolveValue(value: unknown, valuesDir: string): unknown {
  if (value && typeof value === "object" && !Array.isArray(value)) {
    const o = value as Record<string, unknown>;
    const keys = Object.keys(o);
    if (keys.length === 1) {
      if ("inline" in o) return o.inline;
      if ("hash" in o) {
        const digest = String(o.hash).split(":").pop();
        return JSON.parse(fs.readFileSync(path.join(valuesDir, `${digest}.json`), "utf8"));
      }
    }
  }
  return value;
}

export interface Invocation {
  inputs: Record<string, unknown> | null;
  output: unknown;
}

export interface Node {
  nodeId: string;
  nodeKind: string; // input | output | tool_call | model_call | subflow_call | suspension
  effectLevel: string; // pure | deterministic | model | mutation
  invocations: Invocation[];
  tool?: string;
  provider?: string;
  model?: string;
  modelVersion?: string;
  tokensIn: number;
  tokensOut: number;
  costCents: number;
  elapsedMs: number;
  errors: unknown | null;
  /** The node's produced value (last invocation's output). */
  output: unknown;
  failed: boolean;
}

interface Manifest {
  execution_id?: string;
  flow?: string;
  status?: string;
  ir_hash?: string;
  input_hash?: string;
  output_hash?: string;
  elapsed_ms?: number;
  budget_summary?: { tokens_in: number; tokens_out: number; cost_cents: number };
}

function numKey(id: string): number {
  return id[0] === "n" && /^\d+$/.test(id.slice(1)) ? Number(id.slice(1)) : Infinity;
}

function parseNode(raw: any, valuesDir: string): Node {
  const invocations: Invocation[] = (raw.invocations ?? []).map((inv: any) => {
    let inputs: Record<string, unknown> | null = inv.inputs ?? null;
    if (inputs && typeof inputs === "object") {
      inputs = Object.fromEntries(
        Object.entries(inputs).map(([k, v]) => [k, resolveValue(v, valuesDir)]),
      );
    }
    return {
      inputs,
      output: "output" in inv ? resolveValue(inv.output, valuesDir) : null,
    };
  });
  return {
    nodeId: raw.node_id,
    nodeKind: raw.node_kind,
    effectLevel: raw.effect_level ?? "",
    invocations,
    tool: raw.tool,
    provider: raw.provider,
    model: raw.model,
    modelVersion: raw.model_version,
    tokensIn: raw.tokens_in ?? 0,
    tokensOut: raw.tokens_out ?? 0,
    costCents: raw.cost_cents ?? 0,
    elapsedMs: raw.elapsed_ms ?? 0,
    errors: raw.errors ?? null,
    output: invocations.length ? invocations[invocations.length - 1].output : null,
    failed: (raw.errors ?? null) !== null,
  };
}

export class Trace {
  readonly dir: string;
  readonly manifest: Manifest;
  readonly nodes: Node[];
  private readonly byId: Map<string, Node>;

  constructor(directory: string) {
    this.dir = directory;
    const manifestPath = path.join(directory, "manifest.json");
    if (!fs.existsSync(manifestPath)) {
      throw new Error(`no manifest.json under ${directory} (R301)`);
    }
    this.manifest = JSON.parse(fs.readFileSync(manifestPath, "utf8"));

    const valuesDir = path.join(directory, "values");
    const nodesDir = path.join(directory, "nodes");
    this.nodes = fs.existsSync(nodesDir)
      ? fs
          .readdirSync(nodesDir)
          .filter((f) => f.endsWith(".json"))
          .map((f) => parseNode(JSON.parse(fs.readFileSync(path.join(nodesDir, f), "utf8")), valuesDir))
          .sort((a, b) => numKey(a.nodeId) - numKey(b.nodeId))
      : [];
    this.byId = new Map(this.nodes.map((n) => [n.nodeId, n]));
  }

  get executionId(): string { return this.manifest.execution_id ?? ""; }
  get flow(): string { return this.manifest.flow ?? ""; }
  get status(): string { return this.manifest.status ?? ""; }
  get irHash(): string { return this.manifest.ir_hash ?? ""; }
  get inputHash(): string { return this.manifest.input_hash ?? ""; }
  get outputHash(): string { return this.manifest.output_hash ?? ""; }
  get elapsedMs(): number { return this.manifest.elapsed_ms ?? 0; }
  /** Run totals: { tokens_in, tokens_out, cost_cents }. */
  get budget() { return this.manifest.budget_summary ?? { tokens_in: 0, tokens_out: 0, cost_cents: 0 }; }
  get modelCalls(): Node[] { return this.nodes.filter((n) => n.nodeKind === "model_call"); }
  get input(): unknown { return this.nodes.find((n) => n.nodeKind === "input")?.output ?? null; }
  get output(): unknown { return this.nodes.find((n) => n.nodeKind === "output")?.output ?? null; }

  node(nodeId: string): Node | undefined { return this.byId.get(nodeId); }
  diff(other: Trace): TraceDiff { return diffTraces(this, other); }
}

export function openTrace(directory: string): Trace {
  return new Trace(directory);
}

// ---- diff ------------------------------------------------------------------

export interface NodeChange {
  nodeId: string;
  nodeKind: string;
  outputBefore: unknown;
  outputAfter: unknown;
  modelBefore?: string;
  modelAfter?: string;
  outputChanged: boolean;
  modelChanged: boolean;
}

export class TraceDiff {
  constructor(
    readonly changes: NodeChange[],
    readonly onlyInA: string[],
    readonly onlyInB: string[],
    readonly outputHashBefore: string,
    readonly outputHashAfter: string,
  ) {}

  get outputChanged(): boolean {
    return this.outputHashBefore !== this.outputHashAfter;
  }

  get hasChanges(): boolean {
    return this.changes.length > 0 || this.onlyInA.length > 0 || this.onlyInB.length > 0;
  }

  summary(): string {
    if (!this.hasChanges) return "traces identical";
    const lines = [
      `${this.changes.length} node(s) changed; output ${this.outputChanged ? "changed" : "unchanged"}`,
    ];
    for (const c of this.changes) {
      const bits: string[] = [];
      if (c.outputChanged) bits.push(`output ${JSON.stringify(c.outputBefore)} -> ${JSON.stringify(c.outputAfter)}`);
      if (c.modelChanged) bits.push(`model ${c.modelBefore} -> ${c.modelAfter}`);
      lines.push(`  ${c.nodeId} (${c.nodeKind}): ${bits.join("; ")}`);
    }
    if (this.onlyInA.length) lines.push(`  only in A: ${this.onlyInA.join(", ")}`);
    if (this.onlyInB.length) lines.push(`  only in B: ${this.onlyInB.join(", ")}`);
    return lines.join("\n");
  }
}

export function diffTraces(a: string | Trace, b: string | Trace): TraceDiff {
  const ta = a instanceof Trace ? a : openTrace(a);
  const tb = b instanceof Trace ? b : openTrace(b);
  const idsA = new Set(ta.nodes.map((n) => n.nodeId));
  const idsB = new Set(tb.nodes.map((n) => n.nodeId));

  const changes: NodeChange[] = [];
  for (const id of [...idsA].filter((x) => idsB.has(x)).sort((x, y) => numKey(x) - numKey(y))) {
    const na = ta.node(id)!;
    const nb = tb.node(id)!;
    const outputChanged = JSON.stringify(na.output) !== JSON.stringify(nb.output);
    const modelChanged = na.model !== nb.model;
    if (outputChanged || modelChanged) {
      changes.push({
        nodeId: id,
        nodeKind: na.nodeKind,
        outputBefore: na.output,
        outputAfter: nb.output,
        modelBefore: na.model,
        modelAfter: nb.model,
        outputChanged,
        modelChanged,
      });
    }
  }
  return new TraceDiff(
    changes,
    [...idsA].filter((x) => !idsB.has(x)).sort((x, y) => numKey(x) - numKey(y)),
    [...idsB].filter((x) => !idsA.has(x)).sort((x, y) => numKey(x) - numKey(y)),
    ta.outputHash,
    tb.outputHash,
  );
}
