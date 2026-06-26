#!/usr/bin/env python3
"""Webhook resume — a flow suspends for human approval; a webhook resumes it.

The long-wait pattern. A suspending tool (here the built-in await_human_approval)
makes the run return a Suspension; we park its token durably and the process is
free to exit. Whenever the human acts — minutes or days later — a webhook builds
a *fresh* Runtime and resumes from the token. That resume is a separate, linked
execution; the suspended trace is never mutated (flow-suspension(7)).

Two entrypoints share a durable Pending store and trace dir:

    python webhook_resume.py intake          # runs the flow, parks a token, prints it
    python webhook_resume.py serve           # runs the webhook on :8081
    curl -s localhost:8081/resume -d \
      '{"token":"<paste token>","decision":{"approver":"ops","ok":true}}'
    # -> {"status":"complete","result":{"approver":"ops","ok":true}}

Maps to: a Slack/Jira approval callback, a Stripe/GitHub webhook, a scheduler
firing a timer.

SECURITY: resume only proceeds for a token THIS app parked — Pending.resume
rejects anything not in its store, so a caller can't point it at an arbitrary
path. Still, prefer mapping an opaque correlation id to the token server-side
rather than echoing the raw token to clients, and keep the trace dir durable and
shared between the suspending and resuming processes.
"""

import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from flowd import Runtime, Suspension, load_ir
from flowd.serve import FileStore, Pending

HERE = Path(__file__).resolve().parent
Gate = load_ir(str(HERE / "gate.ir.json"))
TRACES = str(HERE / "traces")
pending = Pending(FileStore(HERE / "state" / "pending"))


class Host(Gate):
    def await_human_approval(self, prompt):
        return None  # the runtime intercepts this; the body is never called


def intake():
    """Run the flow until it suspends, then park the token and return."""
    with Runtime(Host(), trace_dir=TRACES) as rt:
        out = rt.run("approve wire transfer of $15,000?")
        if isinstance(out, Suspension):
            pending.park(out.token, context={"reason": "wire approval"})
            print("parked. resume with:\n")
            print(f"  curl -s localhost:8081/resume -d "
                  f"'{json.dumps({'token': out.token, 'decision': {'approver': 'ops', 'ok': True}})}'")
        else:
            print("completed without suspending:", out)


def resume(token: str, decision):
    # A fresh Runtime — exactly what a webhook process has. The token (a handle
    # to the suspended trace) carries the state.
    with Runtime(Host(), trace_dir=TRACES) as rt:
        return pending.resume(rt, token, decision)


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        try:
            n = int(self.headers.get("content-length", 0))
            req = json.loads(self.rfile.read(n) or b"{}")
            out = resume(req["token"], req["decision"])
            if isinstance(out, Suspension):
                payload = {"status": "suspended_again", "token": out.token}
            else:
                payload = {"status": "complete", "result": out}
            code = 200
        except KeyError:
            code, payload = 400, {"error": "body needs {token, decision}"}
        except Exception as e:
            code, payload = 422, {"error": str(e)}
        data = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "intake"
    if cmd == "intake":
        intake()
    elif cmd == "serve":
        print("webhook on http://localhost:8081  (POST /resume)")
        ThreadingHTTPServer(("127.0.0.1", 8081), Handler).serve_forever()
    else:
        sys.exit("usage: webhook_resume.py [intake|serve]")
