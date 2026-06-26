/**
 * Ready-made trace redactors.
 *
 * `setRedactor` takes a `(Buffer) => Buffer | null` callback; writing one by
 * hand for every host is boilerplate. `secretRedactor` returns a callback that
 * scrubs common credential shapes (Anthropic / OpenAI / AWS keys, GitHub
 * tokens, bearer tokens) from persisted trace bytes.
 */

const DEFAULT_PATTERNS = [
  /sk-ant-[A-Za-z0-9_-]{20,}/g, // Anthropic API key
  /sk-[A-Za-z0-9]{20,}/g, // OpenAI-style key
  /AKIA[0-9A-Z]{16}/g, // AWS access key id
  /ghp_[A-Za-z0-9]{20,}/g, // GitHub personal access token
  /[Bb]earer\s+[A-Za-z0-9._-]+/g, // bearer tokens
];

/**
 * Return a redactor that replaces known credential shapes with `replacement`
 * (default `[REDACTED]`). Pass extra `RegExp`s to scrub host-specific secrets.
 * Returns `null` (leave untouched) when nothing matched.
 *
 *   runtime.setRedactor(secretRedactor());
 *   runtime.setRedactor(secretRedactor(/tok_live_[a-z0-9]+/g));
 */
export function secretRedactor(
  ...args: (RegExp | { replacement?: string })[]
): (data: Buffer) => Buffer | null {
  let replacement = "[REDACTED]";
  const extra: RegExp[] = [];
  for (const a of args) {
    if (a instanceof RegExp) extra.push(a);
    else if (a && typeof a === "object" && typeof a.replacement === "string") replacement = a.replacement;
  }
  const patterns = [...DEFAULT_PATTERNS, ...extra].map(
    (p) => new RegExp(p.source, p.flags.includes("g") ? p.flags : p.flags + "g"),
  );

  return (data: Buffer): Buffer | null => {
    const text = data.toString("utf8");
    let out = text;
    for (const rx of patterns) out = out.replace(rx, replacement);
    return out === text ? null : Buffer.from(out, "utf8");
  };
}
