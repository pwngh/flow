# flowd (TypeScript) — onboarding example

A canonical host against `onboarding.flow`, built on the generated, fully-typed
`Onboard` contract. Exercises all three non-pure effect levels —

- `fetchCredit` **deterministic** — an external lookup (here in-memory),
- `assessRisk` **model** — LLM underwriting served by a Claude provider
  adapter, with a deterministic offline fallback,
- `notifyApplicant` **mutation** — the side effect (sends an email).

It also shows `implVersion`, a redactor over the trace, the disposable
lifecycle, and 3-way run handling.

The model step is served by `anthropicProvider`, which derives the
structured-output schema from the IR and hides the synchronous bridge to the
async SDK — the host writes no adapter code. `secretRedactor` keeps credentials
out of the trace.

## Run

```
# in the binding (..): npm install        # builds the native addon
npx flowd-codegen onboarding.ir.json > onboarding.flow.ts   # (already generated here)
npm install @anthropic-ai/sdk             # only for the Claude path
tsc onboarding.ts onboarding.flow.ts --module commonjs --target es2022 \
    --moduleResolution node --esModuleInterop
node onboarding.js                        # offline: underwriting via heuristic
ANTHROPIC_API_KEY=sk-ant-... node onboarding.js   # model step -> Claude
```

## Files

```
onboarding.flow         # the Flow source
onboarding.ir.json      # compiled IR
onboarding.flow.ts      # generated typed contract (flowd-codegen)
onboarding.ts           # the host: typed tool impls + anthropicProvider + secretRedactor
```
