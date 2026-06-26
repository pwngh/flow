# flowd (Python) — onboarding example

A canonical host implementation against `onboarding.flow`: a loan-onboarding
decision exercising all three non-pure effect levels —

- `fetch_credit` **deterministic** — an external lookup (here in-memory),
- `assess_risk` **model** — LLM underwriting served by `anthropic_provider`
  with no host code (it derives the structured-output schema from the IR and
  reports token usage into the trace), guarded by `secret_redactor` and backed
  by a deterministic offline fallback,
- `notify_applicant` **mutation** — the side effect (sends an email).

It also shows `impl_version`, the context-manager lifecycle, and 3-way run
handling (value / `Suspension` / raised `FlowdError`).

## Run

```
pip install "flowd[anthropic]"   # the extra adds the Claude path
python onboarding.py             # offline: underwriting via heuristic
ANTHROPIC_API_KEY=sk-ant-... python onboarding.py   # model step -> Claude
```

## Files

```
onboarding.flow      # the Flow source
onboarding.ir.json   # compiled IR (flowc onboarding.flow -o onboarding.ir.json)
onboarding.py        # the host: tool impls + anthropic_provider + secret_redactor
```
