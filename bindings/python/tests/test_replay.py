"""Replay (model-versioned and same-model), diff, and provider metrics."""

import glob
from pathlib import Path

from flowd import Runtime, diff_traces, load_ir, open_trace


def _ir(name):
    return str(Path(__file__).parent / "fixtures" / f"{name}.ir.json")


def base_run(tmp_path, score=10):
    Score = load_ir(_ir("score"))

    class Impl(Score):
        def risk_check(self, amount):
            return {"score": score, "flagged": False}

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        out = rt.run(750)
    return out, glob.glob(str(tmp_path / "score" / "exec_*"))[0]


class V2:
    name = "stub-v2"

    def supports_model(self, model_id):
        return model_id == "risk-v2"

    def invoke(self, model_id, request_json):
        return {"score": 99, "flagged": True}


def test_model_versioned_replay_changes_output(tmp_path):
    base_out, base = base_run(tmp_path)
    assert base_out == 10
    rep_root = str(tmp_path / "rep")
    with Runtime.open(_ir("score"), trace_dir=str(tmp_path)) as rt:
        rt.register_provider(V2())
        rep_out = rt.replay(base, rep_root, model="risk-v2")
    assert rep_out == 99
    rep_exec = glob.glob(f"{rep_root}/score/exec_*")[0]
    d = diff_traces(base, rep_exec)
    assert d.output_changed
    model_changes = [c for c in d.changes if c.node_kind == "model_call"]
    assert model_changes and model_changes[0].model_before == "risk-v1"
    assert model_changes[0].model_after == "risk-v2"


def test_same_model_replay_is_identical(tmp_path):
    base_out, base = base_run(tmp_path)
    sm_root = str(tmp_path / "sm")
    with Runtime.open(_ir("score"), trace_dir=str(tmp_path)) as rt:
        sm_out = rt.replay(base, sm_root, model=None)  # restore-only
    assert sm_out == base_out
    sm_exec = glob.glob(f"{sm_root}/score/exec_*")[0]
    assert not diff_traces(base, sm_exec)  # no changes


def test_open_accepts_path_and_json_string(tmp_path):
    raw = open(_ir("score")).read()
    with Runtime.open(raw) as rt:        # JSON string
        assert rt._sole_flow() == "score"
    with Runtime.open(_ir("score")) as rt:  # path
        assert rt._sole_flow() == "score"


def test_provider_metrics_recorded_in_trace(tmp_path):
    _, base = base_run(tmp_path)

    class Metered:
        name = "metered"

        def supports_model(self, model_id):
            return model_id == "risk-v1"

        def invoke(self, model_id, request_json):
            return {"score": 5, "flagged": False}

        def invoke_with_metrics(self, model_id, request_json):
            return {
                "response": {"score": 5, "flagged": False},
                "tokens_in": 42,
                "tokens_out": 7,
                "cost_cents": 0.13,
            }

    out_root = str(tmp_path / "m")
    with Runtime.open(_ir("score"), trace_dir=str(tmp_path)) as rt:
        rt.register_provider(Metered())
        rt.replay(base, out_root, model="risk-v1")  # re-invoke via the gateway
    t = open_trace(glob.glob(f"{out_root}/score/exec_*")[0])
    assert t.budget["tokens_in"] == 42
    assert t.budget["tokens_out"] == 7
    assert t.model_calls[0].tokens_in == 42
    assert t.model_calls[0].cost_cents == 0.13
