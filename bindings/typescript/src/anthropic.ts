/**
 * A ready-made Claude provider for Flow's `model` tools.
 *
 * `effect model("X")` declares that a tool is implemented by a model named X.
 * This provider serves every such tool against Claude with no host code: it
 * derives the structured-output schema from the tool's declared output type,
 * reports token usage into the trace, and maps each logical model id to a real
 * Claude model.
 *
 *     runtime.registerProvider(anthropicProvider(ir));
 *
 * Because flowd's callbacks are synchronous, the async Anthropic SDK call is
 * bridged through a bundled worker (anthropic-worker.mjs) — an implementation
 * detail the host never sees. Needs `@anthropic-ai/sdk` at runtime.
 */

import { execFileSync } from "node:child_process";
import * as path from "node:path";

import { jsonSchemaFor, normalizeIr, type IrDocument } from "./schema.js";
import type { ProviderAdapter } from "./index.js";

export interface ClaudeRequest {
  model: string;
  system: string;
  schema: object;
  request: string;
  thinking: string | null;
  maxTokens: number;
}
export interface ClaudeResult {
  response: unknown;
  tokensIn?: number;
  tokensOut?: number;
}
/** The Claude call. Default bridges synchronously to the SDK via a subprocess;
 * inject a stub to test offline. */
export type ClaudeCall = (req: ClaudeRequest) => ClaudeResult;

export interface AnthropicProviderOptions {
  /** Real Claude model for all served ids, or a {logicalId: realModel} map. */
  model?: string | Record<string, string>;
  /** Optional system prompt per logical model id. */
  instructions?: Record<string, string>;
  thinking?: "adaptive" | null;
  maxTokens?: number;
  name?: string;
  call?: ClaudeCall;
}

// The bundled worker; override with FLOWD_ANTHROPIC_WORKER (custom provider,
// or testing the bridge without the SDK). Resolved per call so the override is
// dynamic.
function workerPath(): string {
  return process.env.FLOWD_ANTHROPIC_WORKER ?? path.join(__dirname, "..", "anthropic-worker.mjs");
}

const subprocessCall: ClaudeCall = (req) => {
  const out = execFileSync(process.execPath, [workerPath()], {
    input: JSON.stringify(req),
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
  });
  return JSON.parse(out) as ClaudeResult;
};

/** Build a provider adapter that serves every `model`-effect tool in `ir`. */
export function anthropicProvider(
  ir: IrDocument | string,
  opts: AnthropicProviderOptions = {},
): ProviderAdapter {
  const doc = normalizeIr(ir);
  const served = new Map<string, { name: string; output: string }>();
  for (const tool of doc.tools ?? []) {
    if (tool.effect.level === "model" && tool.effect.model && !served.has(tool.effect.model)) {
      served.set(tool.effect.model, { name: tool.name, output: tool.output });
    }
  }
  if (served.size === 0) {
    throw new Error("the IR declares no model() tools for Claude to serve");
  }

  const call = opts.call ?? subprocessCall;
  const realModel = (id: string): string =>
    typeof opts.model === "object" ? opts.model[id] : opts.model ?? "claude-opus-4-8";

  const ask = (id: string, request: string): ClaudeResult => {
    const tool = served.get(id)!;
    const schema = jsonSchemaFor(doc, tool.output);
    const system =
      opts.instructions?.[id] ??
      `You implement the tool '${tool.name}'. Given the input, return JSON ` +
        `matching the required output schema. Do not explain.`;
    return call({
      model: realModel(id),
      system,
      schema,
      request,
      thinking: opts.thinking ?? "adaptive",
      maxTokens: opts.maxTokens ?? 4096,
    });
  };

  return {
    name: opts.name ?? "anthropic",
    supportsModel: (id) => served.has(id),
    invoke: (id, req) => ask(id, req).response,
    invokeWithMetrics: (id, req) => {
      const r = ask(id, req);
      return {
        response: r.response,
        tokensIn: r.tokensIn ?? 0,
        tokensOut: r.tokensOut ?? 0,
        costCents: 0,
      };
    },
  };
}
