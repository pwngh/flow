"""IR type -> JSON Schema derivation."""

from pathlib import Path

from flowd import json_schema_for


def _ir(name):
    return str(Path(__file__).parent / "fixtures" / f"{name}.ir.json")


def test_primitives():
    assert json_schema_for(_ir("score"), "int") == {"type": "integer"}
    assert json_schema_for(_ir("score"), "string") == {"type": "string"}
    assert json_schema_for(_ir("score"), "bool") == {"type": "boolean"}
    assert json_schema_for(_ir("score"), "float") == {"type": "number"}


def test_record():
    assert json_schema_for(_ir("score"), "RiskReport") == {
        "type": "object",
        "properties": {"score": {"type": "integer"}, "flagged": {"type": "boolean"}},
        "required": ["score", "flagged"],
        "additionalProperties": False,
    }


def test_record_with_string_and_nested(tmp_path):
    # onboarding fixture has Customer{id:string, amount:int}, RiskDecision{...}
    assert json_schema_for(_ir("onboard"), "Customer") == {
        "type": "object",
        "properties": {"id": {"type": "string"}, "amount": {"type": "integer"}},
        "required": ["id", "amount"],
        "additionalProperties": False,
    }


def test_list():
    # synthesize an IR with a list-typed field at the call site
    ir = {"ir_version": "1.0", "types": [
        {"name": "Bag", "kind": "record", "fields": [{"name": "items", "type": "[string]"}]}
    ]}
    assert json_schema_for(ir, "Bag")["properties"]["items"] == {
        "type": "array", "items": {"type": "string"}
    }


def test_unknown_named_type_is_permissive():
    assert json_schema_for({"ir_version": "1.0", "types": []}, "Mystery") == {"type": "object"}
