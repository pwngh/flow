"""load_ir: ABC synthesis, naming, and ir_version validation."""

import json

import pytest

from flowd import FlowdError, load_ir


def test_returns_abc_with_one_abstractmethod_per_tool(ir):
    Triage = load_ir(ir("triage"))
    assert Triage.__name__ == "Triage"
    assert Triage.__abstractmethods__ == frozenset({"risk_check"})


def test_class_name_derived_from_filename(ir):
    assert load_ir(ir("onboard")).__name__ == "Onboard"
    assert load_ir(ir("gate")).__name__ == "Gate"


def test_all_tools_become_abstractmethods(ir):
    Onboard = load_ir(ir("onboard"))
    assert Onboard.__abstractmethods__ == frozenset(
        {"credit_check", "record_decision"}
    )


def test_abc_blocks_partial_implementation(ir):
    Triage = load_ir(ir("triage"))

    class Partial(Triage):
        pass

    with pytest.raises(TypeError, match="risk_check"):
        Partial()


def test_unsupported_ir_version_rejected(tmp_path, ir):
    doc = json.load(open(ir("triage")))
    doc["ir_version"] = "2.0"
    bad = tmp_path / "future.ir.json"
    bad.write_text(json.dumps(doc))
    with pytest.raises(FlowdError, match="ir_version"):
        load_ir(str(bad))
