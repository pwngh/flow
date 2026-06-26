"""Derive a JSON Schema from an IR type.

A model tool's output type is declared in the IR; turning it into a JSON
Schema lets a provider constrain an LLM to return exactly that shape (so the
flow never rejects a malformed model output). Pure, dependency-free, and
provider-agnostic.
"""

from __future__ import annotations

import json
from typing import Any


def _normalize_ir(ir: Any) -> dict:
    """Accept an IR as a dict, a load_ir() class/instance (``_flowd_ir``), a
    path, or a JSON string; return the parsed dict."""
    if isinstance(ir, dict):
        return ir
    got = getattr(ir, "_flowd_ir", None)
    if isinstance(got, dict):
        return got
    if isinstance(ir, str):
        if ir.lstrip().startswith("{"):
            return json.loads(ir)
        with open(ir, "r", encoding="utf-8") as fh:
            return json.load(fh)
    raise TypeError(f"cannot read an IR from {type(ir).__name__}")


_PRIMITIVES = {
    "string": {"type": "string"},
    "int": {"type": "integer"},
    "float": {"type": "number"},
    "bool": {"type": "boolean"},
}


def json_schema_for(ir: Any, type_str: str) -> dict:
    """Return a JSON Schema for the IR type named by ``type_str``
    (e.g. ``"RiskDecision"``, ``"int"``, ``"[Customer]"``)."""
    return _schema(type_str, {t["name"]: t for t in _normalize_ir(ir).get("types", [])})


def _schema(type_str: str, types: dict) -> dict:
    if type_str in _PRIMITIVES:
        return dict(_PRIMITIVES[type_str])
    if type_str.startswith("[") and type_str.endswith("]"):
        return {"type": "array", "items": _schema(type_str[1:-1], types)}

    decl = types.get(type_str)
    if decl is None:
        return {"type": "object"}  # unknown named type -> permissive

    if decl.get("kind") == "record":
        props = {f["name"]: _schema(f["type"], types) for f in decl.get("fields", [])}
        return {
            "type": "object",
            "properties": props,
            "required": list(props),
            "additionalProperties": False,
        }
    if decl.get("kind") == "sum":
        # The runtime encodes a sum value as {"variant": Name, "fields": {...}}.
        arms = []
        for v in decl.get("variants", []):
            fprops = {f["name"]: _schema(f["type"], types) for f in v.get("fields", [])}
            arms.append({
                "type": "object",
                "properties": {
                    "variant": {"const": v["name"]},
                    "fields": {
                        "type": "object",
                        "properties": fprops,
                        "required": list(fprops),
                        "additionalProperties": False,
                    },
                },
                "required": ["variant", "fields"],
                "additionalProperties": False,
            })
        return {"anyOf": arms}
    return {"type": "object"}
