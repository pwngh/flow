"""Provider-adapter gateway dispatch and the redactor secrets hook."""

import glob
import os

import pytest

from flowd import FlowdError, Runtime, load_ir


def test_provider_serves_model_ahead_of_method(tmp_path, ir):
    Triage = load_ir(ir("triage"))
    served = []

    class Impl(Triage):
        def risk_check(self, amount):
            raise AssertionError("model method must not run when a provider serves it")

    class Stub:
        name = "stub"

        def supports_model(self, model_id):
            return model_id == "risk-v1"

        def invoke(self, model_id, request_json):
            served.append((model_id, request_json))
            return {"score": 1, "flagged": False}

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        rt.register_provider(Stub())
        assert rt.run(750) == 2
    assert served == [("risk-v1", '{"amount":750}')]


def test_provider_without_name_rejected(tmp_path, ir):
    Triage = load_ir(ir("triage"))

    class Impl(Triage):
        def risk_check(self, amount):
            return {"score": 0, "flagged": False}

    class NoName:
        def supports_model(self, m):
            return True

        def invoke(self, m, r):
            return {}

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        with pytest.raises(FlowdError):
            rt.register_provider(NoName())


def _onboard_runtime(tmp_path, ir):
    Onboard = load_ir(ir("onboard"))

    class Impl(Onboard):
        def credit_check(self, id):
            return 800

        def record_decision(self, id, approved):
            return True

    return Runtime(Impl(), trace_dir=str(tmp_path))


def _trace_blob(trace_dir):
    files = glob.glob(os.path.join(trace_dir, "onboard", "**", "*.json"), recursive=True)
    return " ".join(open(f).read() for f in files)


def test_redactor_scrubs_persisted_bytes(tmp_path, ir):
    def redactor(data: bytes):
        return data.replace(b"cust_42", b"[REDACTED]") if b"cust_42" in data else None

    with _onboard_runtime(tmp_path, ir) as rt:
        rt.set_redactor(redactor)
        rt.run({"id": "cust_42", "amount": 1})

    blob = _trace_blob(str(tmp_path))
    assert "cust_42" not in blob
    assert "[REDACTED]" in blob


def test_redactor_none_leaves_bytes_untouched(tmp_path, ir):
    with _onboard_runtime(tmp_path, ir) as rt:
        rt.set_redactor(lambda data: None)  # never rewrite
        rt.run({"id": "cust_42", "amount": 1})
    assert "cust_42" in _trace_blob(str(tmp_path))


def test_set_redactor_none_clears(tmp_path, ir):
    with _onboard_runtime(tmp_path, ir) as rt:
        rt.set_redactor(lambda data: b"X")
        rt.set_redactor(None)  # cleared; no exception, bytes pass through
        assert rt.run({"id": "cust_42", "amount": 1}) == {"approved": True, "score": 800}
