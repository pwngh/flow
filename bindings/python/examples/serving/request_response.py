#!/usr/bin/env python3
"""Request/response — run a flow per HTTP request.

The canonical shape: a synchronous endpoint. One Runtime per request (handles
are non-reentrant), the mutation made idempotent so a client retry of the same
POST can't double-send, and the recorded trace left on disk for later audit.

    python request_response.py
    curl -s localhost:8080/onboard \
      -d '{"id":"cust_42","email":"alice@example.com","amount":15000}'
    # -> {"status":"complete","decision":{"approve":true,"band":"LOW",...}}

Maps to: a FastAPI/Flask/Express route, a Lambda behind API Gateway, a gRPC
handler. Swap http.server for your framework; the Runtime-per-request body is
the part that matters.
"""

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from flowd import FlowdError, Runtime, Suspension, load_ir
from flowd.serve import FileStore, idempotent

HERE = Path(__file__).resolve().parent
Onboard = load_ir(str(HERE.parent / "onboarding.ir.json"))

BUREAU = {
    "cust_42": {"score": 760, "history_years": 9},
    "cust_99": {"score": 590, "history_years": 2},
}
# A durable idempotency store shared across requests (and processes): a repeated
# POST with the same inputs replays the first result instead of re-sending.
_idem = FileStore(HERE / "state" / "idem")


class Host(Onboard):
    def fetch_credit(self, id):
        return BUREAU.get(id, {"score": 600, "history_years": 0})

    def assess_risk(self, report, amount):
        band = "LOW" if report["score"] >= 720 else "MEDIUM" if report["score"] >= 640 else "HIGH"
        return {"approve": report["score"] >= 640, "band": band,
                "rationale": f"score {report['score']}, requested {amount}"}

    @idempotent(_idem)
    def notify_applicant(self, email, band):
        print(f"  [email] -> {email}: risk band {band}", flush=True)
        return True


def onboard(body: dict):
    # One Runtime per request — the handle is non-reentrant, so never share it
    # across concurrent requests. ThreadingHTTPServer gives each its own.
    with Runtime(Host(), trace_dir=str(HERE / "traces")) as rt:
        out = rt.run(body)
        if isinstance(out, Suspension):           # this flow never suspends, but
            return 202, {"status": "suspended", "token": out.token}  # be honest
        return 200, {"status": "complete", "decision": out}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            n = int(self.headers.get("content-length", 0))
            body = json.loads(self.rfile.read(n) or b"{}")
            code, payload = onboard(body)
        except FlowdError as e:                    # a flow error -> 422 with the R-code
            code, payload = 422, {"error": e.message, "code": e.code}
        except Exception as e:                     # bad JSON / bad input -> 400
            code, payload = 400, {"error": str(e)}
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    print("serving on http://localhost:8080  (POST /onboard)")
    ThreadingHTTPServer(("127.0.0.1", 8080), Handler).serve_forever()
