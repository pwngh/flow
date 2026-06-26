# Serving Flow in production

A Flow run is **atomic**: it completes with a full trace, or — on a crash
mid-run — leaves nothing (the trace writer accumulates in memory and seals at the
end). That single property is what makes the production model small and boring:

> **one Runtime per unit of work · at-least-once delivery · idempotent mutations
> · suspend/resume for waits · traces to durable storage.**

There is no orchestration server to adopt and no per-step checkpointing. You wire
Flow into the request/worker/queue infrastructure you already run; `flowd.serve`
makes the per-run plumbing first-class, and these three scripts are runnable
references — copy one and swap in your framework.

| Deployment shape | Scaffold | What it shows |
|---|---|---|
| Request / response (HTTP, RPC, Lambda) | [`request_response.py`](request_response.py) | one Runtime per request; a retried POST can't double-send |
| Task / job worker (SQS, PubSub, Celery, Kafka) | [`worker.py`](worker.py) | at-least-once redelivery made safe by idempotent mutations |
| Webhook / human-in-the-loop (approval callback, scheduler) | [`webhook_resume.py`](webhook_resume.py) | a flow suspends, parks a token, and resumes from a fresh process |

```sh
python worker.py              # flow ran 2x; emails actually sent 1x -> idempotency held
python request_response.py    # then: curl -s localhost:8080/onboard -d '{"id":"cust_42",...}'
python webhook_resume.py intake && python webhook_resume.py serve   # then curl /resume
```

## The four moving parts

**1 · One Runtime per unit of work.** `libflowd` is non-reentrant per handle, so a
`Runtime` belongs to one request/job at a time. Create it, run, dispose (the
context manager, or `using`/`close()` in TS). Don't share a handle across threads.

**2 · At-least-once + atomic runs = re-run on failure.** Your queue already
redelivers; because a crashed run leaves no partial state, recovery is just
"run the whole flow again." No reconciliation, no resume-from-node-7.

**3 · Idempotent mutations** close the retry hole. flowd guarantees *replay*
never re-invokes a level-3 tool; `@idempotent(store)` closes the *retry* side, so
a re-run replays the recorded result instead of firing the side effect twice:

```python
class Host(Onboard):
    @idempotent(FileStore("state/idem"))      # default key: hash of the inputs
    def notify_applicant(self, email, band):
        send_email(email, f"risk band: {band}")
        return True
```

Pass `key=lambda **inputs: ...` for a business key (an order id, an email
message-id) when you want to dedup across runs, not just identical retries.

**4 · Suspend/resume for waits that outlive a process.** A suspending tool returns
a `Suspension`; `Pending` parks its token durably so the process can exit, and a
later webhook resumes from a fresh Runtime — a separate, linked execution that
never mutates the suspended trace.

```python
out = rt.run(application)
if isinstance(out, Suspension):
    pending.park(out.token, context={"application_id": app_id})   # process may exit
# ... later, in the webhook: ...
out = pending.resume(fresh_rt, token, decision)
```

## Operating notes

- **Traces are your audit log and replay source.** `trace_dir` is just a path —
  point it at a mounted durable volume, or sync the sealed `<flow>/<exec>/` dir
  to object storage after each run. `flowd replay` / `flowd diff` (or the
  in-process `Runtime.replay` / `diff_traces`) re-run a recorded decision against
  a new model without re-firing any side effect.
- **Back the stores with real infra.** `MemoryStore`/`FileStore` are the defaults;
  implement `Store`'s three methods over Redis/DynamoDB for multi-host idempotency
  and parking.
- **Security: a suspension token is a path to the trace.** Don't accept the raw
  token from an untrusted webhook caller — map an opaque correlation id to it in
  your `Pending` store and look it up server-side. And the trace dir must be
  durable + shared between the suspending and resuming processes.

## Where this model stops

This covers request/response, job workers, webhooks, and most agent tasks. It is
**not** durable execution (Temporal/Restate/DBOS): there is no mid-run
checkpoint, so a crash near the end of a long, expensive run re-burns the whole
run on retry. If your workload is *long, expensive, and non-suspendable* — a
12-minute, 40-model-call agent that can't tolerate re-running — wrap flowd in a
checkpointing engine, or insert suspension points as explicit checkpoints. For
everything else, atomic-run + at-least-once + idempotency is the simpler, sturdier
choice.
