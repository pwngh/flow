"""Programmatic trace inspection: open_trace / Trace / Node."""

import glob
from pathlib import Path

import pytest

from flowd import Runtime, load_ir, open_trace


def _ir(name):
    return str(Path(__file__).parent / "fixtures" / f"{name}.ir.json")


def run_score(tmp_path, score=10):
    Score = load_ir(_ir("score"))

    class Impl(Score):
        def risk_check(self, amount):
            return {"score": score, "flagged": False}

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        rt.run(750)
    return glob.glob(str(tmp_path / "score" / "exec_*"))[0]


def test_manifest_fields(tmp_path):
    t = open_trace(run_score(tmp_path))
    assert t.flow == "score"
    assert t.status == "complete"
    assert t.input == 750
    assert t.output == 10
    assert t.ir_hash.startswith("sha256:")
    assert t.output_hash.startswith("sha256:")
    assert set(t.budget) >= {"tokens_in", "tokens_out", "cost_cents"}


def test_nodes_and_model_call(tmp_path):
    t = open_trace(run_score(tmp_path))
    kinds = {n.node_kind for n in t.nodes}
    assert {"input", "output", "model_call"} <= kinds
    mc = t.model_calls[0]
    assert mc.model == "risk-v1"
    assert mc.effect_level == "model"
    assert mc.output == {"score": 10, "flagged": False}
    assert mc.invocations[0].inputs == {"amount": 750}


def test_node_lookup(tmp_path):
    t = open_trace(run_score(tmp_path))
    assert t.node("n0").node_kind == "input"
    assert t.node("does-not-exist") is None


def test_missing_manifest_raises(tmp_path):
    with pytest.raises(FileNotFoundError):
        open_trace(tmp_path)  # empty dir, no manifest.json
