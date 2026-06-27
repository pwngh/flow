# flowd (Python)

Run Flow programs from Python. `flowc` compiles a `.flow` to a JSON IR; this
binding loads that IR, lets you write the leaf tools in Python, runs the flow,
and records a provenance trace of every step — which you can then replay against
a different model and diff. A thin `cffi` wrapper that static-links `libflowd`.
**Status: working.**

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

`load_ir` reads the compiled IR and hands you back an abstract class with **one
method per tool**. You implement the leaves; the flow's wiring runs in the
engine. Miss a method and `abc` refuses to instantiate.

```python
from flowd import load_ir, Runtime

Onboard = load_ir("onboarding.ir.json")

class Host(Onboard):
    def fetch_credit(self, id):                       # deterministic
        return BUREAU.get(id, {"score": 600, "history_years": 0})

    def assess_risk(self, report, amount):            # model (heuristic fallback)
        band = "LOW" if report["score"] >= 720 else "MEDIUM" if report["score"] >= 640 else "HIGH"
        return {"approve": report["score"] >= 640, "band": band, "rationale": "..."}

    def notify_applicant(self, email, band):          # mutation
        send_email(email, f"risk band: {band}")
        return True

with Runtime(Host(), trace_dir="traces") as rt:
    decision = rt.run({"id": "cust_42", "email": "alice@example.com", "amount": 15000})
    # -> {"approve": True, "band": "LOW", "rationale": "..."}
```

`examples/onboarding.py` is the full runnable version. `python onboarding.py`.

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
status, and a token/cost budget. Read the JSON directly, or `open_trace(dir)`
for a `Trace` with `.input`, `.output`, `.status`, `.budget`, `.model_calls`,
and `.nodes`.

## Serve the `model()` tools with Claude — no adapter code

The `assess_risk` heuristic above is a fallback. Register the bundled provider
and the same flow runs that step against Claude — which derives its
structured-output schema *from the IR's declared `RiskDecision` type*, so it
can't hand back a malformed value, and reports tokens/cost into the trace:

```python
from flowd import secret_redactor
from flowd.contrib.anthropic import anthropic_provider   # pip install "flowd[anthropic]"

rt.set_redactor(secret_redactor())                       # keep key-shaped bytes out of traces
rt.register_provider(anthropic_provider("onboarding.ir.json"))
```

Two lines, no `async` plumbing. The credential lives in the provider (the SDK
reads `ANTHROPIC_API_KEY` from the environment), never in flow inputs.
`json_schema_for(ir, type)` exposes the schema derivation on its own.

## Replay a past decision against a new model, and diff

This is the reason for recording everything: re-run a decision against a
different model, with the inputs captured the first time, and see what moved.

```python
from flowd import Runtime, diff_traces

rt = Runtime.open("onboarding.ir.json")                  # tool-less: replay + inspect only
rt.register_provider(anthropic_provider("onboarding.ir.json"))
rt.replay("traces/onboard/<exec>", "replays", model="claude-opus-4-8")

print(diff_traces("traces/onboard/<exec>", "replays/onboard/<exec>").summary())
# -> which nodes changed, old -> new
```

`replay(orig, new, model=None)` restores every node from the trace when `model`
is `None` (a `mutation` is never re-invoked); a model id re-invokes only the
model nodes, with the recorded inputs.

## Human-in-the-loop: suspend and resume

A suspending tool makes `run` return a `Suspension` instead of a value. Hand the
decision back once the wait is satisfied:

```python
out = rt.run(applicant)
if isinstance(out, Suspension):
    out = rt.resume(out.token, {"approver": "ops", "decision": "approve", ...})
```

Resume is a *separate* execution: it writes a new trace linked via `resumed_from`
while the suspended trace stays `suspended`, untouched. v1 ships one suspending
tool, the built-in `await_human_approval`.

## Serving in production

A Flow run is atomic, so the production model is small: **one Runtime per unit of
work, at-least-once delivery, idempotent mutations, suspend/resume for waits.**
`flowd.serve` makes that plumbing first-class — pluggable `Store`s, an
`@idempotent` wrapper so a re-run can't double-fire a mutation, and `Pending` to
park and resume suspensions across processes:

```python
from flowd import Runtime, Suspension
from flowd.serve import FileStore, idempotent, Pending

idem    = FileStore("state/idem")            # any Store: MemoryStore, FileStore, or your own over Redis/Dynamo
pending = Pending(FileStore("state/pending"))

class Host(Onboard):
    @idempotent(idem)                        # claims the key atomically — an at-least-once retry can't re-send
    def notify_applicant(self, email, band):
        send_email(email, f"risk band: {band}"); return True

def handle(application):                     # request or worker: run, and park if it pauses for a human
    with Runtime(Host(), trace_dir="traces") as rt:
        out = rt.run(application)
        if isinstance(out, Suspension):
            pending.park(out.token, context={"id": application["id"]})
            return {"status": "pending", "token": out.token}    # safe to exit; the token is durable
        return out

def on_approval(token, decision):            # webhook, a different process: resume on the same IR
    with Runtime(Host(), trace_dir="traces") as rt:
        return pending.resume(rt, token, decision)              # the final value, or re-parks if it suspends again
```

`examples/serving/` has runnable references for the common shapes — request/response
(`request_response.py`), a job worker (`worker.py`), and a webhook that resumes a
suspended run (`webhook_resume.py`) — plus the operational model and its limits
(it is not durable execution; see `examples/serving/README.md`).

## Install & test

```sh
./build.sh                            # at repo root — builds flowd/libflowd.a first
cd bindings/python && ./install.sh    # editable install (pip install -e .)
pip install -e ".[test]" && pytest    # the suite runs against compiled fixture IRs
```

Needs Python 3.10+, a C compiler, and `libflowd.a`. libcrypto + libcurl must be
linkable (macOS defaults to Homebrew `openssl@3` / `curl`; override with
`FLOWD_OPENSSL_LIBDIR` / `FLOWD_CURL_LIBDIR`). This is cffi API/static mode, so
`cffi 1.16+` is both a build and a runtime dependency.

## Notes & caveats

- **Synthesis checks names, not signatures.** `abc` enforces that every tool
  method exists — not its arity, types, or return type — and today's `libflowd`
  accepts a tool's `signature` but does not validate it (`R153` is reserved,
  unchecked). Effect levels come from the IR, not the method bodies. Want
  conformance checks? Compare each method's `inspect.signature` to the IR
  yourself.
- **`impl_version`** (set on the class or via `Runtime(..., impl_version=...)`)
  is threaded through registration, but the current `libflowd` build does not
  yet persist it into trace artifacts.
- **One `Runtime` per thread.** `libflowd` is non-reentrant per handle: `run` /
  `run_named` / `resume` must not overlap, and providers/redactors must outlive
  the handle (the binding keeps them alive). Prefer the context manager — GC
  alone is non-deterministic.
- **Selecting a flow, marshalling I/O.** `run` executes the IR's first flow;
  `run(input, flow="name")` selects another. A bare implicit `it` scalar takes a
  scalar (`run(16)`), a record takes a dict, a multi-parameter flow a dict keyed
  by parameter name.

## Layout

```
flowd/
  __init__.py            load_ir, Runtime, Suspension, FlowdError, secret_redactor
  trace.py               open_trace, Trace, diff_traces
  schema.py              json_schema_for (IR type -> JSON Schema)
  redactors.py           secret_redactor (built-in trace redactor)
  serve.py               idempotent, Pending, Store/MemoryStore/FileStore (production helpers)
  contrib/anthropic.py   anthropic_provider (serve model() tools via Claude)
examples/                onboarding.py — a runnable host
  serving/               request/response, worker, webhook — runnable references
tests/                   pytest suite + compiled fixture IRs
```
