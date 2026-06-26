#!/usr/bin/env python3
"""Canonical flowd (Python) host for onboarding.flow — batteries included.

The host writes only the real per-tool logic; the binding owns the plumbing:

  * fetch_credit     deterministic : an external lookup (here in-memory)
  * assess_risk      model         : an offline heuristic fallback; when
                                     ANTHROPIC_API_KEY is set the model step is
                                     served by Claude with *no adapter code* —
                                     anthropic_provider derives the structured-
                                     output schema from the IR and reports token
                                     usage into the trace.
  * notify_applicant mutation      : the side effect (sends an email)

secret_redactor keeps credentials out of the trace. The wiring is two lines.

    pip install "flowd[anthropic]"
    python onboarding.py
    ANTHROPIC_API_KEY=sk-ant-... python onboarding.py
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from flowd import FlowdError, Runtime, Suspension, load_ir, secret_redactor

IR_PATH = Path(__file__).with_name("onboarding.ir.json")
Onboard = load_ir(str(IR_PATH))

# A toy credit bureau. fetch_credit stays deterministic (level 1): a real impl
# would HTTP GET a bureau, with no side effects.
_BUREAU = {
    "cust_42": {"score": 760, "history_years": 9},
    "cust_99": {"score": 590, "history_years": 2},
}


class OnboardImpl(Onboard):
    impl_version = "onboarding-host-1"

    def fetch_credit(self, id: str) -> dict:
        return _BUREAU.get(id, {"score": 600, "history_years": 0})

    def assess_risk(self, report: dict, amount: int) -> dict:
        """Offline fallback; Claude serves this when a provider is registered."""
        score = report["score"]
        band = "LOW" if score >= 720 else "MEDIUM" if score >= 640 else "HIGH"
        return {
            "approve": score >= 640 and amount <= score * 40,
            "band": band,
            "rationale": f"heuristic: score {score}, requested {amount}",
        }

    def notify_applicant(self, email: str, band: str) -> bool:
        # A real impl sends email via SES/SMTP with an idempotency key — replay
        # restores level-3 nodes and never re-invokes.
        print(f"  [email] -> {email}: your application risk band is {band}")
        return True


def main() -> None:
    trace_dir = str(Path(__file__).with_name("traces"))
    with Runtime(OnboardImpl(), trace_dir=trace_dir) as rt:
        rt.set_redactor(secret_redactor())  # scrub any key-shaped bytes

        if os.environ.get("ANTHROPIC_API_KEY"):
            from flowd.contrib.anthropic import anthropic_provider

            rt.register_provider(anthropic_provider(
                str(IR_PATH),
                instructions={
                    "claude-underwriter-v1": (
                        "You are a conservative consumer-credit underwriter. Decide "
                        "whether to approve, assign a LOW/MEDIUM/HIGH band, and give a "
                        "one-sentence rationale."
                    ),
                },
            ))
            print("underwriting via Claude (claude-opus-4-8)")
        else:
            print("underwriting via heuristic (set ANTHROPIC_API_KEY for Claude)")

        result = rt.run({"id": "cust_42", "email": "alice@example.com", "amount": 15000})

        if isinstance(result, Suspension):
            print(f"suspended: {result.token}")  # this flow never suspends
            return
        print(f"decision: {json.dumps(result)}")
        print(f"trace written under {trace_dir}/onboard/")


if __name__ == "__main__":
    try:
        main()
    except FlowdError as exc:
        raise SystemExit(f"flow failed: {exc}")
