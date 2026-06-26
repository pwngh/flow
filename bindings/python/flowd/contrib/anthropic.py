"""A ready-made Claude provider for Flow's ``model`` tools.

``effect model("X")`` in a flow declares that a tool is implemented by a model
named ``X``. This provider serves every such tool against Claude with **no host
code**: it derives the structured-output schema from the tool's declared output
type (so the model can't return a malformed value), reports token usage into
the trace, and maps each logical model id to a real Claude model.

    from flowd.contrib.anthropic import anthropic_provider

    rt.register_provider(anthropic_provider(ir))            # one line
    rt.register_provider(anthropic_provider(
        ir,
        model="claude-opus-4-8",
        instructions={"claude-underwriter-v1": "You are a conservative underwriter."},
    ))

Needs the ``anthropic`` SDK (``pip install "flowd[anthropic]"``).
"""

from __future__ import annotations

import json
from typing import Any, Optional

from ..schema import _normalize_ir, json_schema_for


def anthropic_provider(
    ir: Any,
    *,
    model: str | dict = "claude-opus-4-8",
    instructions: Optional[dict] = None,
    thinking: Optional[str] = "adaptive",
    max_tokens: int = 4096,
    client: Any = None,
    name: str = "anthropic",
):
    """Return a provider adapter (for ``Runtime.register_provider``) that serves
    every ``model``-effect tool in ``ir`` against Claude.

    ``model`` is the real Claude model for all served ids, or a
    ``{logical_id: real_model}`` mapping. ``instructions`` optionally overrides
    the system prompt per logical id. ``client`` injects an Anthropic client
    (defaults to ``anthropic.Anthropic()``); pass a stub to test offline.
    """
    doc = _normalize_ir(ir)
    served: dict[str, dict] = {}
    for tool in doc.get("tools", []):
        effect = tool.get("effect", {})
        if effect.get("level") == "model" and effect.get("model"):
            served.setdefault(effect["model"], tool)  # first tool wins per id
    if not served:
        raise ValueError("the IR declares no model() tools for Claude to serve")

    if client is None:
        import anthropic  # lazy: only needed when actually invoking

        client = anthropic.Anthropic()
    instructions = instructions or {}

    def _real_model(model_id: str) -> str:
        return model[model_id] if isinstance(model, dict) else model

    class _AnthropicProvider:
        def __init__(self):
            self.name = name

        def supports_model(self, model_id: str) -> bool:
            return model_id in served

        def _ask(self, model_id: str, request_json: str):
            tool = served[model_id]
            schema = json_schema_for(doc, tool["output"])
            system = instructions.get(model_id) or (
                f"You implement the tool '{tool['name']}'. Given the input, "
                f"return JSON matching the required output schema. Do not explain."
            )
            kwargs = dict(
                model=_real_model(model_id),
                max_tokens=max_tokens,
                system=system,
                messages=[{"role": "user", "content": request_json}],
                output_config={"format": {"type": "json_schema", "schema": schema}},
            )
            if thinking:
                kwargs["thinking"] = {"type": thinking}
            resp = client.messages.create(**kwargs)
            text = next(b.text for b in resp.content if b.type == "text")
            return json.loads(text), getattr(resp, "usage", None)

        def invoke(self, model_id: str, request_json: str):
            value, _ = self._ask(model_id, request_json)
            return value

        def invoke_with_metrics(self, model_id: str, request_json: str):
            value, usage = self._ask(model_id, request_json)
            return {
                "response": value,
                "tokens_in": getattr(usage, "input_tokens", 0) if usage else 0,
                "tokens_out": getattr(usage, "output_tokens", 0) if usage else 0,
                "cost_cents": 0.0,
            }

    return _AnthropicProvider()
