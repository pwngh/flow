"""Read and diff Flow execution traces.

A trace is the durable, content-addressed record of one flow run — every
node's inputs, outputs, model identities, token/cost metrics, and timing
(see flow-trace(5)). This module turns a trace directory into an inspectable
object graph and diffs two traces node-by-node, which is what makes the
record/replay/diff loop usable from Python:

    base    = Runtime(Impl()).run(x)            # records traces/<flow>/<exec>/
    replay  = Runtime.open(ir).replay(base_dir, new_dir, model="gpt-5")
    delta   = diff_traces(base_dir, new_dir)    # what changed, and where
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional


def _resolve(value: Any, values_dir: Path) -> Any:
    """Resolve a trace value reference: {"inline": v} | {"hash": "sha256:.."}."""
    # Safe only because the trace writer wraps every value in exactly one of
    # {inline} / {hash}: a one-key dict here is a reference, never a user value.
    # If that wrapping ever loosens, a real {"hash": ..} payload would be
    # misread as a content address.
    if isinstance(value, dict) and len(value) == 1:
        if "inline" in value:
            return value["inline"]
        if "hash" in value:
            digest = str(value["hash"]).split(":", 1)[-1]
            blob = values_dir / f"{digest}.json"
            return json.loads(blob.read_text())
    return value


@dataclass
class Invocation:
    """One row of input a node processed (a direct call has exactly one)."""

    inputs: Optional[dict]
    output: Any


@dataclass
class Node:
    """One node's execution record."""

    node_id: str
    node_kind: str          # input | output | tool_call | model_call | subflow_call | suspension
    effect_level: str       # pure | deterministic | model | mutation
    invocations: list[Invocation]
    tool: Optional[str] = None
    provider: Optional[str] = None
    model: Optional[str] = None
    model_version: Optional[str] = None
    tokens_in: int = 0
    tokens_out: int = 0
    cost_cents: float = 0.0
    elapsed_ms: int = 0
    errors: Optional[dict] = None
    raw: dict = field(default_factory=dict, repr=False)

    @property
    def output(self) -> Any:
        """The node's produced value (last invocation's output)."""
        return self.invocations[-1].output if self.invocations else None

    @property
    def failed(self) -> bool:
        return self.errors is not None


class Trace:
    """A parsed trace directory (manifest + nodes + content-addressed values)."""

    def __init__(self, directory: str | Path):
        self.dir = Path(directory)
        manifest_path = self.dir / "manifest.json"
        if not manifest_path.is_file():
            raise FileNotFoundError(f"no manifest.json under {self.dir} (R301)")
        self.manifest: dict = json.loads(manifest_path.read_text())

        values_dir = self.dir / "values"
        self.nodes: list[Node] = []
        nodes_dir = self.dir / "nodes"
        if nodes_dir.is_dir():
            for path in sorted(nodes_dir.glob("*.json"), key=lambda p: _id_sort(p.stem)):
                self.nodes.append(_parse_node(json.loads(path.read_text()), values_dir))
        self._by_id = {n.node_id: n for n in self.nodes}

    # -- manifest fields ---------------------------------------------------
    @property
    def execution_id(self) -> str:
        return self.manifest.get("execution_id", "")

    @property
    def flow(self) -> str:
        return self.manifest.get("flow", "")

    @property
    def status(self) -> str:
        return self.manifest.get("status", "")

    @property
    def ir_hash(self) -> str:
        return self.manifest.get("ir_hash", "")

    @property
    def input_hash(self) -> str:
        return self.manifest.get("input_hash", "")

    @property
    def output_hash(self) -> str:
        return self.manifest.get("output_hash", "")

    @property
    def elapsed_ms(self) -> int:
        return self.manifest.get("elapsed_ms", 0)

    @property
    def budget(self) -> dict:
        """Run totals: {'tokens_in', 'tokens_out', 'cost_cents'}."""
        return self.manifest.get("budget_summary", {})

    @property
    def model_calls(self) -> list[Node]:
        return [n for n in self.nodes if n.node_kind == "model_call"]

    @property
    def input(self) -> Any:
        n = next((n for n in self.nodes if n.node_kind == "input"), None)
        return n.output if n else None

    @property
    def output(self) -> Any:
        n = next((n for n in self.nodes if n.node_kind == "output"), None)
        return n.output if n else None

    def node(self, node_id: str) -> Optional[Node]:
        return self._by_id.get(node_id)

    def __repr__(self) -> str:
        return (
            f"Trace({self.flow}/{self.execution_id} status={self.status} "
            f"nodes={len(self.nodes)} budget={self.budget})"
        )


def _parse_node(raw: dict, values_dir: Path) -> Node:
    invocations = []
    for inv in raw.get("invocations", []):
        inputs = inv.get("inputs")
        if isinstance(inputs, dict):
            inputs = {k: _resolve(v, values_dir) for k, v in inputs.items()}
        output = _resolve(inv["output"], values_dir) if "output" in inv else None
        invocations.append(Invocation(inputs=inputs, output=output))
    return Node(
        node_id=raw["node_id"],
        node_kind=raw["node_kind"],
        effect_level=raw.get("effect_level", ""),
        invocations=invocations,
        tool=raw.get("tool"),
        provider=raw.get("provider"),
        model=raw.get("model"),
        model_version=raw.get("model_version"),
        tokens_in=raw.get("tokens_in", 0),
        tokens_out=raw.get("tokens_out", 0),
        cost_cents=raw.get("cost_cents", 0.0),
        elapsed_ms=raw.get("elapsed_ms", 0),
        errors=raw.get("errors"),
        raw=raw,
    )


def open_trace(directory: str | Path) -> Trace:
    """Open a trace directory (the traces/<flow>/<execution_id>/ produced by a run)."""
    return Trace(directory)


# -- diff -------------------------------------------------------------------

@dataclass
class NodeChange:
    node_id: str
    node_kind: str
    output_before: Any = None
    output_after: Any = None
    model_before: Optional[str] = None
    model_after: Optional[str] = None

    @property
    def output_changed(self) -> bool:
        return self.output_before != self.output_after

    @property
    def model_changed(self) -> bool:
        return self.model_before != self.model_after


@dataclass
class TraceDiff:
    """Node-by-node difference between two traces (same flow, e.g. a run and
    its model-versioned replay)."""

    changes: list[NodeChange] = field(default_factory=list)
    only_in_a: list[str] = field(default_factory=list)
    only_in_b: list[str] = field(default_factory=list)
    output_hash_before: str = ""
    output_hash_after: str = ""

    @property
    def output_changed(self) -> bool:
        return self.output_hash_before != self.output_hash_after

    def __bool__(self) -> bool:
        return bool(self.changes or self.only_in_a or self.only_in_b)

    def summary(self) -> str:
        if not self:
            return "traces identical"
        lines = [f"{len(self.changes)} node(s) changed; "
                 f"output {'changed' if self.output_changed else 'unchanged'}"]
        for c in self.changes:
            bits = []
            if c.output_changed:
                bits.append(f"output {c.output_before!r} -> {c.output_after!r}")
            if c.model_changed:
                bits.append(f"model {c.model_before} -> {c.model_after}")
            lines.append(f"  {c.node_id} ({c.node_kind}): {'; '.join(bits)}")
        if self.only_in_a:
            lines.append(f"  only in A: {', '.join(self.only_in_a)}")
        if self.only_in_b:
            lines.append(f"  only in B: {', '.join(self.only_in_b)}")
        return "\n".join(lines)

    def __repr__(self) -> str:
        return f"TraceDiff(changes={len(self.changes)}, output_changed={self.output_changed})"


def diff_traces(a: str | Path | Trace, b: str | Path | Trace) -> TraceDiff:
    """Diff two traces node-by-node, by node id."""
    ta = a if isinstance(a, Trace) else open_trace(a)
    tb = b if isinstance(b, Trace) else open_trace(b)

    ids_a = {n.node_id for n in ta.nodes}
    ids_b = {n.node_id for n in tb.nodes}
    diff = TraceDiff(
        only_in_a=sorted(ids_a - ids_b, key=_id_sort),
        only_in_b=sorted(ids_b - ids_a, key=_id_sort),
        output_hash_before=ta.output_hash,
        output_hash_after=tb.output_hash,
    )
    for node_id in sorted(ids_a & ids_b, key=_id_sort):
        na, nb = ta.node(node_id), tb.node(node_id)
        change = NodeChange(
            node_id=node_id,
            node_kind=na.node_kind,
            output_before=na.output,
            output_after=nb.output,
            model_before=na.model,
            model_after=nb.model,
        )
        if change.output_changed or change.model_changed:
            diff.changes.append(change)
    return diff


def _id_sort(node_id: str):
    return (0, int(node_id[1:])) if node_id[1:].isdigit() else (1, node_id)
