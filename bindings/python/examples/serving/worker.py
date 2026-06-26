#!/usr/bin/env python3
"""Job worker — pull a job, run a flow, ack, with at-least-once redelivery made
safe by idempotent mutations.

This is the heart of the production story. A real queue (SQS, PubSub, Kafka,
Celery) delivers *at least once*: a worker that crashes after performing a side
effect but before ack'ing will see the job again. Because a Flow run is atomic
(a crash mid-run leaves no trace), recovery is simply "run the whole flow
again" — and the @idempotent mutation makes that safe: the flow re-runs, but the
side effect it already performed replays its recorded result instead of firing
twice.

This script models that by delivering the SAME job twice and showing the flow
ran twice while the email went out once.

    python worker.py
    # ... job j1 -> {...}   (acked)        x2
    # flow ran 2x; emails actually sent 1x  -> idempotency held: True

Maps to: an SQS consumer, a Celery/RQ task, a Kafka consumer group. Replace the
in-memory queue with your broker; keep "one Runtime per job + idempotent
mutations + ack on success".
"""

import json
import queue
from pathlib import Path

from flowd import FlowdError, Runtime, Suspension, load_ir
from flowd.serve import FileStore, Pending, idempotent

HERE = Path(__file__).resolve().parent
Onboard = load_ir(str(HERE.parent / "onboarding.ir.json"))

BUREAU = {"cust_42": {"score": 760, "history_years": 9}}
_idem = FileStore(HERE / "state" / "idem")
_pending = Pending(FileStore(HERE / "state" / "pending"))

SENDS: list[str] = []   # side effects that actually happened
RUNS: list[str] = []    # flow executions (may exceed SENDS, by design)


class Host(Onboard):
    def fetch_credit(self, id):
        return BUREAU.get(id, {"score": 600, "history_years": 0})

    def assess_risk(self, report, amount):
        band = "LOW" if report["score"] >= 720 else "MEDIUM" if report["score"] >= 640 else "HIGH"
        return {"approve": report["score"] >= 640, "band": band, "rationale": "heuristic"}

    @idempotent(_idem)
    def notify_applicant(self, email, band):
        SENDS.append(email)
        print(f"  [email] -> {email}: band {band}")
        return True


def process(job: dict):
    """Run one job to completion (or suspension). One Runtime per job."""
    RUNS.append(job["id"])
    with Runtime(Host(), trace_dir=str(HERE / "traces")) as rt:
        out = rt.run(job["input"])
    if isinstance(out, Suspension):
        # A suspending flow parks here and is resumed out-of-band (see
        # webhook_resume.py); for this onboarding flow it never happens.
        _pending.park(out.token, context={"job_id": job["id"]})
        return {"status": "parked", "token": out.token}
    return {"status": "complete", "decision": out}


def main():
    q: queue.Queue = queue.Queue()
    job = {"id": "j1",
           "input": {"id": "cust_42", "email": "alice@example.com", "amount": 15000}}
    # Deliver the SAME job twice to model an at-least-once redelivery.
    q.put(job)
    q.put(job)

    while not q.empty():
        j = q.get()
        try:
            result = process(j)
            print(f"job {j['id']} -> {json.dumps(result)}   (acked)")
        except FlowdError as e:
            # Defensive: this onboarding flow never errors, but in general a flow
            # error means don't ack — let the broker redeliver (which the
            # idempotent mutation keeps safe).
            print(f"job {j['id']} failed ({e.code}); leaving for redelivery")

    print(f"\nflow ran {len(RUNS)}x; emails actually sent {len(SENDS)}x  "
          f"-> idempotency held: {len(SENDS) == 1}")


if __name__ == "__main__":
    main()
