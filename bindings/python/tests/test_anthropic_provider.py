"""flowd.contrib.anthropic.anthropic_provider (tested with a stub client)."""

import glob
from pathlib import Path

import pytest

from flowd import Runtime, load_ir, open_trace
from flowd.contrib.anthropic import anthropic_provider


def _ir(name):
    return str(Path(__file__).parent / "fixtures" / f"{name}.ir.json")


class _FakeUsage:
    input_tokens = 11
    output_tokens = 5


class _FakeBlock:
    def __init__(self, text):
        self.type = "text"
        self.text = text


class _FakeResp:
    def __init__(self, text):
        self.content = [_FakeBlock(text)]
        self.usage = _FakeUsage()


class FakeClient:
    """Records calls and returns canned structured output."""

    def __init__(self, text='{"score": 7, "flagged": true}'):
        self.calls = []
        self._text = text
        self.messages = self

    def create(self, **kwargs):  # stands in for client.messages.create
        self.calls.append(kwargs)
        return _FakeResp(self._text)


def test_serves_model_tool_with_zero_host_code(tmp_path):
    Score = load_ir(_ir("score"))

    class Impl(Score):
        def risk_check(self, amount):
            raise AssertionError("the provider must serve the model, not the fallback")

    fake = FakeClient()
    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        rt.register_provider(anthropic_provider(_ir("score"), client=fake))
        assert rt.run(750) == 7  # the flow returns report.score


def test_derives_schema_from_ir(tmp_path):
    fake = FakeClient()
    Score = load_ir(_ir("score"))

    class Impl(Score):
        def risk_check(self, amount):
            return {"score": 0, "flagged": False}

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        rt.register_provider(anthropic_provider(_ir("score"), client=fake))
        rt.run(750)

    schema = fake.calls[0]["output_config"]["format"]["schema"]
    assert schema["properties"].keys() == {"score", "flagged"}
    assert schema["additionalProperties"] is False
    assert fake.calls[0]["model"] == "claude-opus-4-8"


def test_per_model_real_model_and_instructions(tmp_path):
    fake = FakeClient()
    Score = load_ir(_ir("score"))

    class Impl(Score):
        def risk_check(self, amount):
            return {"score": 0, "flagged": False}

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        rt.register_provider(anthropic_provider(
            _ir("score"),
            model={"risk-v1": "claude-haiku-4-5"},
            instructions={"risk-v1": "Be terse."},
            client=fake,
        ))
        rt.run(750)

    assert fake.calls[0]["model"] == "claude-haiku-4-5"
    assert fake.calls[0]["system"] == "Be terse."


def test_metrics_flow_into_the_trace(tmp_path):
    fake = FakeClient()
    Score = load_ir(_ir("score"))

    class Impl(Score):
        def risk_check(self, amount):
            return {"score": 0, "flagged": False}

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        rt.register_provider(anthropic_provider(_ir("score"), client=fake))
        rt.run(750)

    t = open_trace(glob.glob(str(tmp_path / "score" / "exec_*"))[0])
    assert t.budget["tokens_in"] == 11   # _FakeUsage.input_tokens
    assert t.budget["tokens_out"] == 5   # _FakeUsage.output_tokens


def test_raises_when_ir_has_no_model_tools():
    # the onboarding IR's tools are deterministic + mutation only... it does
    # have a model tool, so use a synthetic IR with none.
    ir = {"ir_version": "1.0", "types": [], "tools": [
        {"name": "t", "input": [], "output": "int", "effect": {"level": "deterministic"}}
    ], "flows": []}
    with pytest.raises(ValueError, match="no model"):
        anthropic_provider(ir, client=FakeClient())
