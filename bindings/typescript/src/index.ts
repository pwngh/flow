/**
 * TypeScript binding for the Flow runtime.
 *
 * Thin wrapper over libflowd's C ABI via a native N-API addon
 * (static-links flowd/libflowd.a). Public surface is `Runtime` (runs
 * flows and writes traces), `loadIr` (IR introspection), `Suspension`,
 * and the error types. Concrete, fully-typed tool/model contracts come
 * from a generated `.ts` file (see `flowd-codegen`), which wraps this
 * `Runtime`; there is no `flowc --emit=ts` flag. See the README.
 *
 * Tool/model methods are invoked synchronously and must return a value
 * (not a Promise); see the README's Concurrency section.
 */

import * as fs from "node:fs";
import * as path from "node:path";

export {
  Trace,
  TraceDiff,
  openTrace,
  diffTraces,
  type Node,
  type Invocation,
  type NodeChange,
} from "./trace.js";

export { jsonSchemaFor, normalizeIr, type IrDocument } from "./schema.js";
export { secretRedactor } from "./redactors.js";
export {
  anthropicProvider,
  type AnthropicProviderOptions,
  type ClaudeCall,
  type ClaudeRequest,
  type ClaudeResult,
} from "./anthropic.js";

// ---- native addon loader -------------------------------------------

interface RunResult {
  status: "ok" | "suspended" | "error";
  value?: string;
  token?: string;
  error?: string;
}

interface Native {
  loadIr(irJson: string): unknown;
  registerTool(
    rt: unknown, name: string, level: number, signature: string,
    fn: (argsJson: string) => string, implVersion: string | null,
  ): number;
  registerModel(
    rt: unknown, name: string, signature: string,
    fn: (argsJson: string) => string, implVersion: string | null,
  ): number;
  setRedactor(rt: unknown, fn: ((data: Buffer) => Buffer | null) | null): void;
  registerProvider(
    rt: unknown, name: string,
    supports: (modelId: string) => boolean,
    invoke: (modelId: string, requestJson: string) => string,
    metrics:
      | ((modelId: string, requestJson: string) => {
          response: string; tokensIn: number; tokensOut: number; costCents: number;
        })
      | null,
  ): number;
  run(rt: unknown, inputJson: string, traceDir: string, flow: string | null): RunResult;
  resume(rt: unknown, token: string, decisionJson: string): RunResult;
  replay(
    rt: unknown, flow: string, originalTraceDir: string, newTraceDir: string,
    newModelId: string | null,
  ): RunResult;
  destroy(rt: unknown): void;
}

function loadNative(): Native {
  const candidates = [
    path.join(__dirname, "..", "build", "Release", "flowd_native.node"),
    path.join(__dirname, "..", "build", "Debug", "flowd_native.node"),
  ];
  for (const c of candidates) {
    if (fs.existsSync(c)) {
      // eslint-disable-next-line @typescript-eslint/no-var-requires
      return require(c) as Native;
    }
  }
  throw new FlowdError(
    "native addon flowd_native.node not found; build it with " +
      "`npm install` (or `npx node-gyp rebuild`), which static-links " +
      "flowd/libflowd.a",
  );
}

let _native: Native | null = null;
function native(): Native {
  if (_native === null) _native = loadNative();
  return _native;
}

// ---- errors --------------------------------------------------------

export class FlowdError extends Error {
  readonly code?: string;
  constructor(message: string, code?: string) {
    super(code ? `${code}: ${message}` : message);
    this.name = "FlowdError";
    this.code = code;
  }
}

export class FlowdRegistrationError extends FlowdError {
  constructor(message: string, code?: string) {
    super(message, code);
    this.name = "FlowdRegistrationError";
  }
}

const REGISTER_CODES: Record<number, string> = {
  150: "malformed registration (NULL name or implementation)",
  151: "R151 effect-level mismatch with the IR",
  152: "R152 no tool of this name in the IR",
  153: "R153 signature mismatch with the IR",
};

function errorFromJson(text: string | undefined): FlowdError {
  if (!text) return new FlowdError("flow execution failed");
  try {
    const obj = JSON.parse(text) as { id?: string; message?: string };
    return new FlowdError(obj.message ?? text, obj.id);
  } catch {
    return new FlowdError(text);
  }
}

// ---- IR types ------------------------------------------------------

const EFFECT_LEVEL: Record<string, number> = {
  pure: 0,
  deterministic: 1,
  model: 2,
  mutation: 3,
};

interface IrToolInput {
  name: string;
  type: string;
}
interface IrTool {
  name: string;
  input?: IrToolInput[];
  output: string;
  effect: { level: string; model?: string };
}
interface IrFlow {
  name: string;
}
interface IrDoc {
  ir_version: string;
  tools?: IrTool[];
  flows?: IrFlow[];
}

function readIr(irPathOrJson: string): { raw: string; doc: IrDoc } {
  const raw = irPathOrJson.trimStart().startsWith("{")
    ? irPathOrJson
    : fs.readFileSync(irPathOrJson, "utf8");
  const doc = JSON.parse(raw) as IrDoc;
  if (typeof doc.ir_version !== "string" || !doc.ir_version.startsWith("1.")) {
    throw new FlowdError(
      `unsupported IR ir_version ${String(doc.ir_version)}; this binding supports 1.x`,
    );
  }
  return { raw, doc };
}

function signatureOf(tool: IrTool): string {
  const params = (tool.input ?? []).map((p) => p.type).join(", ");
  return `(${params}) -> ${tool.output}`;
}

/** Introspect an IR document: its tool names+levels and flow names. */
export function loadIr(irPathOrJson: string): {
  tools: { name: string; level: string }[];
  flows: string[];
} {
  const { doc } = readIr(irPathOrJson);
  return {
    tools: (doc.tools ?? []).map((t) => ({ name: t.name, level: t.effect.level })),
    flows: (doc.flows ?? []).map((f) => f.name),
  };
}

// ---- public runtime surface ----------------------------------------

export type ToolImpl = (args: Record<string, unknown>) => unknown;

/** A model provider adapter for the gateway (the preferred model path). */
export interface ProviderAdapter {
  /** Provider id recorded in traces, e.g. "openai". */
  name: string;
  /** Whether this adapter serves the given model id. */
  supportsModel(modelId: string): boolean;
  /** Invoke the model. `request` is the tool's input as a canonical JSON
   * string; return a JSON string or a JSON-serializable value. */
  invoke(modelId: string, request: string): unknown;
  /** Optional v2 path: report provider token/cost metrics. When present, the
   * gateway prefers it over `invoke` and rolls the usage into the run budget
   * and the trace's model_call nodes. */
  invokeWithMetrics?(
    modelId: string,
    request: string,
  ): { response: unknown; tokensIn?: number; tokensOut?: number; costCents?: number };
}

export interface RuntimeOptions {
  /** Implementation function per IR tool name (snake_case). Omit for a
   * tool-less runtime (replay / inspection only) — see `Runtime.open`. */
  tools?: Record<string, ToolImpl>;
  /** Trace root; one traces/<flow>/<exec>/ dir per run. Default "traces". */
  traceDir?: string;
  /** Recorded with every registration (host-chosen). */
  implVersion?: string;
}

export class Suspension {
  constructor(
    readonly token: string,
    private readonly runtime: Runtime,
  ) {}

  /** Resume with a decision matching the suspended tool's output type. */
  resume(decision: unknown): unknown {
    return this.runtime.resume(this.token, decision);
  }
}

export class Runtime {
  private handle: unknown;
  private readonly traceDir: string;
  private readonly flows: string[] = [];
  private closed = false;

  constructor(irPathOrJson: string, opts: RuntimeOptions = {}) {
    const { raw, doc } = readIr(irPathOrJson);
    this.traceDir = opts.traceDir ?? "traces";
    this.handle = native().loadIr(raw);
    const ver = opts.implVersion ?? null;
    this.flows = (doc.flows ?? []).map((f) => f.name);

    const tools = opts.tools;
    if (!tools) return; // tool-less runtime (replay / inspection)

    for (const tool of doc.tools ?? []) {
      const method = tools[tool.name];
      if (typeof method !== "function") {
        this.close();
        throw new FlowdRegistrationError(
          `tool '${tool.name}' is declared in the IR but no implementation ` +
            `function was provided in opts.tools`,
        );
      }
      const fn = this.wrap(tool.name, method);
      const sig = signatureOf(tool);
      const rc =
        tool.effect.level === "model"
          ? native().registerModel(this.handle, tool.name, sig, fn, ver)
          : native().registerTool(
              this.handle, tool.name, EFFECT_LEVEL[tool.effect.level],
              sig, fn, ver,
            );
      if (rc !== 0) {
        this.close();
        throw new FlowdRegistrationError(
          `registering '${tool.name}': ${REGISTER_CODES[rc] ?? `code ${rc}`}`,
          String(rc),
        );
      }
    }
  }

  /**
   * Open a tool-less runtime for replay and inspection — registers no tool
   * implementations. `irPathOrJson` is a path to an .ir.json file or the IR
   * JSON itself. Use the constructor with `tools` when you need to run flows.
   */
  static open(irPathOrJson: string, opts?: { traceDir?: string }): Runtime {
    return new Runtime(irPathOrJson, { traceDir: opts?.traceDir });
  }

  /** Build the (argsJson) => resultJson function the native layer calls. */
  private wrap(name: string, method: ToolImpl): (argsJson: string) => string {
    return (argsJson: string): string => {
      const args = JSON.parse(argsJson) as Record<string, unknown>;
      const result = method(args);
      if (result != null && typeof (result as { then?: unknown }).then === "function") {
        throw new FlowdError(
          `tool '${name}' returned a Promise; tool/model methods must be ` +
            `synchronous (async requires the worker-thread path, not implemented)`,
        );
      }
      return JSON.stringify(result ?? null);
    };
  }

  /** Install a redactor: (bytes) => Buffer to rewrite, or null to leave as-is. */
  setRedactor(fn: ((data: Buffer) => Buffer | null) | null): void {
    this.checkOpen();
    native().setRedactor(this.handle, fn);
  }

  /**
   * Register a model provider adapter (the gateway's preferred path).
   * Adapters are tried in registration order; the first whose
   * `supportsModel` returns true serves a model call, taking precedence
   * over any per-model `tools` method.
   */
  registerProvider(adapter: ProviderAdapter): void {
    this.checkOpen();
    const toJson = (v: unknown) => (typeof v === "string" ? v : JSON.stringify(v ?? null));
    const invoke = (modelId: string, requestJson: string): string =>
      toJson(adapter.invoke(modelId, requestJson));

    const withMetrics = adapter.invokeWithMetrics;
    const metrics = withMetrics
      ? (modelId: string, requestJson: string) => {
          const m = withMetrics.call(adapter, modelId, requestJson);
          return {
            response: toJson(m.response),
            tokensIn: m.tokensIn ?? 0,
            tokensOut: m.tokensOut ?? 0,
            costCents: m.costCents ?? 0,
          };
        }
      : null;

    const rc = native().registerProvider(
      this.handle,
      adapter.name,
      (modelId: string) => adapter.supportsModel(modelId),
      invoke,
      metrics,
    );
    if (rc !== 0) {
      throw new FlowdRegistrationError(
        `registerProvider: ${REGISTER_CODES[rc] ?? `code ${rc}`}`,
        String(rc),
      );
    }
  }

  /** Run a flow. Returns its output, a Suspension, or throws FlowdError. */
  run(input: unknown, opts?: { flow?: string }): unknown {
    this.checkOpen();
    const r = native().run(
      this.handle,
      JSON.stringify(input ?? null),
      this.traceDir,
      opts?.flow ?? null,
    );
    return this.finish(r);
  }

  /** Resume a suspended flow. Writes a new trace; the original is untouched. */
  resume(token: string, decision: unknown): unknown {
    this.checkOpen();
    const r = native().resume(this.handle, token, JSON.stringify(decision ?? null));
    return this.finish(r);
  }

  /**
   * Re-execute a recorded trace, writing a new one, and return its output.
   * `model` undefined/null restores every node from the original trace
   * (verifies determinism; a mutation is never re-invoked); a model id
   * re-invokes only the model nodes via the gateway against that model with
   * the recorded inputs — register a provider that serves it first. `flow`
   * selects the flow (defaults to the IR's sole flow). Pair with
   * `diffTraces(originalTraceDir, newTraceDir)` to see what changed.
   */
  replay(
    originalTraceDir: string,
    newTraceDir: string,
    opts?: { model?: string; flow?: string },
  ): unknown {
    this.checkOpen();
    const r = native().replay(
      this.handle,
      opts?.flow ?? this.soleFlow(),
      originalTraceDir,
      newTraceDir,
      opts?.model ?? null,
    );
    return this.finish(r);
  }

  private soleFlow(): string {
    if (this.flows.length === 1) return this.flows[0];
    throw new FlowdError("the IR declares multiple flows; pass { flow } to replay()");
  }

  private finish(r: RunResult): unknown {
    if (r.status === "ok") return JSON.parse(r.value as string);
    if (r.status === "suspended") return new Suspension(r.token as string, this);
    throw errorFromJson(r.error);
  }

  private checkOpen(): void {
    if (this.closed) throw new FlowdError("Runtime is closed");
  }

  /** Release the native flowd_runtime handle (calls flowd_destroy). */
  close(): void {
    if (!this.closed && this.handle) {
      native().destroy(this.handle);
    }
    this.closed = true;
  }

  [Symbol.dispose](): void {
    this.close();
  }
}
