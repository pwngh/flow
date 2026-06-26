"""Suspension and resume against the built-in await_human_approval."""

import json

from flowd import Runtime, Suspension, load_ir


def gate_runtime(tmp_path, ir):
    Gate = load_ir(ir("gate"))

    class Impl(Gate):
        def await_human_approval(self, prompt):
            return None  # runtime intercepts; this is never actually called

    return Runtime(Impl(), trace_dir=str(tmp_path))


def test_run_returns_suspension_with_token(tmp_path, ir):
    with gate_runtime(tmp_path, ir) as rt:
        result = rt.run("approve transfer?")
        assert isinstance(result, Suspension)
        assert isinstance(result.token, str) and result.token


def test_original_trace_is_suspended(tmp_path, ir):
    with gate_runtime(tmp_path, ir) as rt:
        rt.run("approve?")
    manifest = next((tmp_path / "gate").glob("*/manifest.json"))
    assert json.loads(manifest.read_text())["status"] == "suspended"


def test_resume_completes_with_decision(tmp_path, ir):
    with gate_runtime(tmp_path, ir) as rt:
        susp = rt.run("approve?")
        decision = {"approver": "compliance@example.com", "ok": True}
        assert susp.resume(decision) == decision


def test_resume_writes_a_new_trace(tmp_path, ir):
    with gate_runtime(tmp_path, ir) as rt:
        susp = rt.run("approve?")
        susp.resume({"approver": "a@b", "ok": False})
    # original (suspended) + resumed = two execution dirs
    execs = list((tmp_path / "gate").glob("exec_*"))
    assert len(execs) == 2
