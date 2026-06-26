# flowd (TypeScript)

Run Flow programs from Node, type-checked end to end. `flowc` compiles a `.flow`
to a JSON IR; `flowd-codegen` turns that IR into a typed contract; this binding
runs the flow and records a provenance trace you can replay against a different
model and diff. A thin N-API addon that static-links `libflowd`. **Status:
working.**

## The whole idea, in one flow

Here is `onboarding.flow` — a loan decision that touches all three effect levels
(a deterministic lookup, a model judgment, a side effect):

```flow
type Applicant    = { id: string, email: string, amount: int }
type CreditReport = { score: int, history_years: int }
type RiskDecision = { approve: bool, band: string, rationale: string }

tool fetch_credit(id: string) -> CreditReport
  effect deterministic
tool assess_risk(report: CreditReport, amount: int) -> RiskDecision
  effect model("claude-underwriter-v1")
tool notify_applicant(email: string, band: string) -> bool
  effect mutation

flow onboard(Applicant) -> RiskDecision {
  report   = fetch_credit(it.id)
  decision = assess_risk(report, it.amount)
  sent     = notify_applicant(it.email, decision.band)
  decision
}
```

Generate a typed contract from the compiled IR, then implement the leaf tools.
The `OnboardTools` interface makes the type checker enforce every method's input
and output against the IR — that is the whole reason the contract is *generated*
rather than synthesized at runtime (TypeScript erases types).

```sh
npx flowd-codegen onboarding.ir.json > onboarding.flow.ts
```

```typescript
import { Onboard, type OnboardTools } from "./onboarding.flow.js";

const impl: OnboardTools = {
  fetchCredit: ({ id }) => BUREAU[id] ?? { score: 600, history_years: 0 },     // deterministic
  assessRisk: ({ report, amount }) => {                                        // model (fallback)
    const band = report.score >= 720 ? "LOW" : report.score >= 640 ? "MEDIUM" : "HIGH";
    return { approve: report.score >= 640, band, rationale: "..." };
  },
  notifyApplicant: ({ email, band }) => { sendEmail(email, band); return true; }, // mutation
};

const onboard = new Onboard(impl, { traceDir: "traces" });
const decision = onboard.run({ id: "cust_42", email: "alice@example.com", amount: 15000 });
// decision : RiskDecision  ->  { approve: true, band: "LOW", rationale: "..." }
onboard.close();   // the generated wrapper is close()-based; Runtime also supports `using`
```

`examples/onboarding.ts` is the full runnable host. Prefer to skip codegen? Drive
the runtime directly, keying `tools` by the IR's snake_case names:

```typescript
import { Runtime } from "flowd";

using rt = new Runtime("onboarding.ir.json", {        // a path or an IR JSON string
  traceDir: "traces",
  tools: {
    fetch_credit:     ({ id }) => ({ score: 760, history_years: 9 }),
    assess_risk:      ({ report, amount }) => ({ approve: true, band: "LOW", rationale: "..." }),
    notify_applicant: ({ email, band }) => true,
  },
});
const decision = rt.run({ id: "cust_42", email: "alice@example.com", amount: 15000 });
```

## What it recorded

That `run` wrote `traces/onboard/<exec>/`. Every node captures the exact inputs
and output of a step, so a reader can reconstruct *why* the decision came out the
way it did. The model step:

```json
// traces/onboard/<exec>/nodes/n2.json
{
  "node_kind": "model_call",
  "model": "claude-underwriter-v1",
  "invocations": [{
    "inputs": { "report": { "score": 760, "history_years": 9 }, "amount": 15000 },
    "output": { "inline": { "approve": true, "band": "LOW", "rationale": "..." } }
  }]
}
```

The manifest alongside it carries `ir_hash` / `input_hash` / `output_hash`,
status, and a token/cost budget. Read the JSON directly, or `openTrace(dir)` for
a `Trace` with `.input`, `.output`, `.status`, `.budget`, `.modelCalls`, and
`.nodes`.

## Serve the `model()` tools with Claude — no adapter code

The `assessRisk` heuristic above is a fallback. Register the bundled provider and
the same flow runs that step against Claude — which derives its structured-output
schema *from the IR's declared `RiskDecision` type*, so it can't hand back a
malformed value, and reports tokens/cost into the trace:

```typescript
import { secretRedactor, anthropicProvider } from "flowd";   // npm i @anthropic-ai/sdk

onboard.setRedactor(secretRedactor());                       // keep key-shaped bytes out of traces
onboard.registerProvider(anthropicProvider("onboarding.ir.json"));
```

Two lines — `anthropicProvider` even hides the synchronous bridge to the async
SDK (a bundled worker), so you write no `async` plumbing. The credential lives in
the provider (the SDK reads `ANTHROPIC_API_KEY` from the environment), never in
flow inputs. `jsonSchemaFor(ir, type)` exposes the schema derivation on its own.

## Replay a past decision against a new model, and diff

This is the reason for recording everything: re-run a decision against a
different model, with the inputs captured the first time, and see what moved.

```typescript
import { Runtime, diffTraces } from "flowd";

using rt = Runtime.open("onboarding.ir.json");               // tool-less: replay + inspect only
rt.registerProvider(anthropicProvider("onboarding.ir.json"));
rt.replay("traces/onboard/<exec>", "replays", { model: "claude-opus-4-8" });

console.log(diffTraces("traces/onboard/<exec>", "replays/onboard/<exec>").summary());
// -> which nodes changed, old -> new
```

`replay(orig, new, { model })` restores every node from the trace when `model` is
omitted (a `mutation` is never re-invoked); a model id re-invokes only the model
nodes, with the recorded inputs.

## Human-in-the-loop: suspend and resume

A suspending tool makes `run` return a `Suspension` instead of a value. Hand the
decision back once the wait is satisfied:

```typescript
const out = onboard.run(applicant);
if (out instanceof Suspension) {
  out.resume({ approver: "ops", decision: "approve", /* ... */ });
}
```

Resume is a *separate* execution: it writes a new trace linked via `resumed_from`
while the suspended trace stays `suspended`, untouched. v1 ships one suspending
tool, the built-in `await_human_approval`.

## Install & test

```sh
./build.sh                                  # at repo root — builds flowd/libflowd.a first
cd bindings/typescript && npm install       # builds the native addon (gypfile)
npm run build                               # tsc -> dist/ (incl. the flowd-codegen CLI)
npm test                                    # node --test over test/
```

Needs Node 20+, a C compiler, and `libflowd.a`. On macOS the addon links Homebrew
`openssl@3` (libcrypto) and `curl` (libcurl); override the `-L` paths in
`binding.gyp` for other layouts. `npm install` ships no prebuilt binary, so a
consumer rebuilds from source.

## Notes & caveats

- **Tool methods are synchronous** — they must *return* a value, not a Promise (a
  returned Promise throws a clear error). `libflowd`'s callbacks are synchronous
  C functions and `flowd_run` blocks on each; the async worker-thread path is
  documented but not yet wired. Do async I/O outside the flow, or use
  `anthropicProvider`, which hides a synchronous bridge to the async SDK.
- **snake_case ↔ camelCase.** Codegen emits the camelCase method and registers
  under the IR's name (`flowd_register_tool` matches verbatim; a mismatch is
  `R152`). Keep the transform bijective so a `credit_check`/`creditCheck`
  collision can't silently break registration.
- **No signature validation** (`R153` reserved, unchecked), so a stale generated
  file whose *types* drifted from a newer IR isn't caught at runtime; the binding
  does validate the loaded IR's `ir_version` against the supported 1.x range.
  `implVersion` is threaded through registration but not yet persisted into
  traces.
- **One `Runtime` per handle, non-reentrant.** `run` / `resume` must not overlap
  (with or without `{ flow }`); `close()` frees it, and `Runtime` implements
  `Symbol.dispose` so `using rt = new Runtime(...)` works. The provider pool is
  capped at 8 per process.
- **Selecting a flow, marshalling I/O.** `run` executes the IR's first flow;
  `run(input, { flow: "name" })` selects another. A bare implicit `it` scalar
  takes a value, a record takes an object, a multi-parameter flow an object keyed
  by parameter name.

## Layout

```
src/
  index.ts       Runtime (run/replay/open), loadIr, Suspension, errors
  trace.ts       openTrace, Trace, diffTraces
  schema.ts      jsonSchemaFor (IR type -> JSON Schema)
  redactors.ts   secretRedactor (built-in trace redactor)
  anthropic.ts   anthropicProvider (serve model() tools via Claude)
  codegen.ts     flowd-codegen: emits a typed .ts contract from an IR
  native.c       N-API addon; static-links libflowd.a
examples/        onboarding.ts — a runnable host
test/            node --test suite + compiled fixture IRs
```
