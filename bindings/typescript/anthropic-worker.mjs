#!/usr/bin/env node
/**
 * Synchronous-bridge worker for anthropicProvider (src/anthropic.ts).
 *
 * Reads a ClaudeRequest JSON on stdin ({ model, system, schema, request,
 * thinking, maxTokens }) and prints a ClaudeResult JSON ({ response, tokensIn,
 * tokensOut }). Invoked once per model call by the parent (synchronously), so
 * the async Anthropic SDK call can happen here. Needs @anthropic-ai/sdk.
 */

import Anthropic from "@anthropic-ai/sdk";

function readStdin() {
  return new Promise((resolve) => {
    let s = "";
    process.stdin.setEncoding("utf8");
    process.stdin.on("data", (c) => (s += c));
    process.stdin.on("end", () => resolve(s));
  });
}

const { model, system, schema, request, thinking, maxTokens } = JSON.parse(await readStdin());

const client = new Anthropic(); // reads ANTHROPIC_API_KEY from the environment

const params = {
  model,
  max_tokens: maxTokens,
  system,
  messages: [{ role: "user", content: request }],
  output_config: { format: { type: "json_schema", schema } },
};
if (thinking) params.thinking = { type: thinking };

const response = await client.messages.create(params);
const text = response.content.find((b) => b.type === "text").text;

process.stdout.write(
  JSON.stringify({
    response: JSON.parse(text),
    tokensIn: response.usage?.input_tokens ?? 0,
    tokensOut: response.usage?.output_tokens ?? 0,
  }),
);
