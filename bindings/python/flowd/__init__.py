"""Python binding for the Flow runtime.

Thin wrapper over libflowd's C ABI via cffi. Public surface is
``load_ir`` (returns an abstract base class derived from the IR's
tool and model declarations) and ``Runtime`` (runs flows against
a concrete subclass and writes traces). See the README for usage.
"""

from __future__ import annotations

import json
import os
from abc import ABC, abstractmethod

from .redactors import secret_redactor
from .schema import json_schema_for
from .trace import (
    Node,
    Trace,
    TraceDiff,
    diff_traces,
    open_trace,
)

__version__ = "0.1.0"

__all__ = [
    "load_ir",
    "Runtime",
    "Suspension",
    "FlowdError",
    "FlowdRegistrationError",
    "Trace",
    "Node",
    "TraceDiff",
    "open_trace",
    "diff_traces",
    "secret_redactor",
    "json_schema_for",
    "__version__",
]

# The compiled extension (flowd._flowd) static-links libflowd.a. It is
# built by flowd/_cffi_build.py (run `python -m flowd._cffi_build`, or
# `pip install` via cffi_modules). Import is deferred to a helper so an
# unbuilt extension yields a clear message rather than an ImportError at
# package import time.
_ffi = None
_lib = None


def _load_native():
    global _ffi, _lib
    if _lib is not None:
        return
    try:
        from ._flowd import ffi, lib  # type: ignore
    except ImportError as exc:  # pragma: no cover - build-time guidance
        raise FlowdError(
            "the native extension flowd._flowd is not built. Build it with "
            "`python -m flowd._cffi_build` (or `pip install .`), which "
            "static-links flowd/libflowd.a."
        ) from exc
    _ffi, _lib = ffi, lib


# Effect-level string (IR's effect.level) -> FLOWD_EFFECT_* enum value.
_EFFECT_LEVEL = {
    "pure": 0,
    "deterministic": 1,
    "model": 2,
    "mutation": 3,
}


class FlowdError(Exception):
    """A runtime error from libflowd (carries the R-code when known)."""

    def __init__(self, message, code=None):
        super().__init__(message if not code else f"{code}: {message}")
        self.code = code
        self.message = message


class FlowdRegistrationError(FlowdError):
    """flowd_register_tool/model returned a non-zero R15x/150 code."""


_REGISTER_CODES = {
    150: "malformed registration (NULL name/implementation, or a "
         "malformed adapter struct for register_provider)",
    151: "R151 effect-level mismatch with the IR",
    152: "R152 no tool of this name in the IR",
    153: "R153 signature mismatch with the IR",
}


def _camel(stem):
    return "".join(part[:1].upper() + part[1:] for part in stem.replace("-", "_").split("_"))


def _check_ir_version(ir):
    version = ir.get("ir_version", "")
    if not isinstance(version, str) or not version.startswith("1."):
        raise FlowdError(
            f"unsupported IR ir_version {version!r}; this binding supports 1.x"
        )


def _read_ir_source(ir):
    """Return the IR JSON text from a path or a JSON string."""
    if isinstance(ir, str) and ir.lstrip().startswith("{"):
        return ir
    with open(ir, "r", encoding="utf-8") as fh:
        return fh.read()


def load_ir(path):
    """Load an IR document and synthesize its abstract base class.

    Returns an ``abc.ABC`` subclass with one ``@abstractmethod`` per
    tool (and per model) declared in the IR; a host subclasses it and
    implements every method. The parsed IR and its raw bytes are
    attached to the class so ``Runtime`` reuses the single parse.
    """
    with open(path, "r", encoding="utf-8") as fh:
        raw = fh.read()
    ir = json.loads(raw)
    _check_ir_version(ir)

    namespace = {}
    for tool in ir.get("tools", []):
        name = tool["name"]

        def _stub(self, *args, _name=name, **kwargs):  # noqa: ANN001
            raise NotImplementedError(_name)

        _stub.__name__ = name
        namespace[name] = abstractmethod(_stub)

    namespace["_flowd_ir"] = ir
    namespace["_flowd_ir_json"] = raw

    stem = os.path.basename(path)
    for suffix in (".ir.json", ".json", ".ir"):
        if stem.endswith(suffix):
            stem = stem[: -len(suffix)]
            break
    cls_name = _camel(stem) or "Flow"
    return type(cls_name, (ABC,), namespace)


def _find_ir(impl):
    for klass in type(impl).__mro__:
        if "_flowd_ir" in klass.__dict__:
            return klass.__dict__["_flowd_ir"], klass.__dict__["_flowd_ir_json"]
    raise FlowdError(
        "implementation is not a subclass of a load_ir() class "
        "(no IR attached)"
    )


class Suspension:
    """A suspended execution: carries the token and resumes via its Runtime."""

    def __init__(self, token, runtime):
        self.token = token
        self._runtime = runtime

    def resume(self, decision):
        """Resume with ``decision`` (must match the suspended tool's output type)."""
        return self._runtime.resume(self.token, decision)

    def __repr__(self):
        return f"Suspension(token={self.token!r})"


class Runtime:
    """Runs flows against a concrete implementation and writes traces.

    Owns exactly one ``flowd_runtime`` handle (built once from the IR
    that ``load_ir`` already parsed). libflowd is non-reentrant per
    handle, so ``run``/``run_named``/``resume`` must not overlap on one
    Runtime; use the context-manager form or ``close()`` to release the
    handle deterministically.
    """

    def __init__(self, impl, trace_dir="traces", impl_version=None, *, _ir_source=None):
        _load_native()
        self._impl = impl
        if impl is not None:
            self._ir, raw = _find_ir(impl)
        else:
            raw = _ir_source
            self._ir = json.loads(raw)
            _check_ir_version(self._ir)
        self._trace_dir = trace_dir
        self._impl_version = impl_version or getattr(impl, "impl_version", None)
        self._keepalive = []  # ffi.callback cdata must outlive the handle
        self._closed = False

        self._rt = _lib.flowd_load_ir(raw.encode("utf-8"))
        if self._rt == _ffi.NULL:
            raise self._last_error("flowd_load_ir failed")
        try:
            if impl is not None:
                self._register_all()
        except Exception:
            self.close()
            raise

    @classmethod
    def open(cls, ir, *, trace_dir="traces"):
        """Open a tool-less runtime for replay and inspection — registers no
        tool implementations. ``ir`` is a path to an ``.ir.json`` file or the
        IR JSON itself. Use the normal constructor when you need to run flows.
        """
        return cls(None, trace_dir=trace_dir, _ir_source=_read_ir_source(ir))

    # -- registration -------------------------------------------------

    def _register_all(self):
        for tool in self._ir.get("tools", []):
            name = tool["name"]
            level = tool["effect"]["level"]
            method = getattr(self._impl, name, None)
            if not callable(method):
                raise FlowdRegistrationError(
                    f"tool '{name}' is declared in the IR but the "
                    f"implementation's '{name}' attribute is missing or not "
                    f"callable"
                )

            cb = _ffi.callback("flowd_tool_fn", self._make_callback(method))
            self._keepalive.append(cb)

            # The runtime accepts but does not validate the signature
            # (it is (void)-discarded in libflowd; reserved for a future
            # R153), so we pass NULL rather than reconstructing it.
            sig = _ffi.NULL
            ver = (
                self._impl_version.encode("utf-8")
                if self._impl_version
                else _ffi.NULL
            )

            if level == "model":
                rc = _lib.flowd_register_model(
                    self._rt, name.encode("utf-8"), sig, cb, ver, _ffi.NULL
                )
            else:
                # An IR with an unrecognized effect level would otherwise
                # raise a bare KeyError here; surface it as the binding's
                # own registration error instead.
                code = _EFFECT_LEVEL.get(level)
                if code is None:
                    raise FlowdRegistrationError(
                        f"tool {name!r} has unknown effect level {level!r}"
                    )
                rc = _lib.flowd_register_tool(
                    self._rt, name.encode("utf-8"), code, sig,
                    cb, ver, _ffi.NULL,
                )
            if rc != 0:
                raise FlowdRegistrationError(
                    f"registering '{name}': "
                    f"{_REGISTER_CODES.get(rc, f'code {rc}')}",
                    code=str(rc),
                )

    def _make_callback(self, method):
        def _cb(args_json, err_ptr, _user_ctx):
            try:
                args = json.loads(_ffi.string(args_json).decode("utf-8"))
                result = method(**args)
                out = json.dumps(result).encode("utf-8")
                # Return through the runtime's own allocator: libflowd free()s
                # this pointer, so a cffi-owned buffer (e.g. ffi.new) here would
                # be a cross-allocator free. flowd_py_strdup malloc()s into the
                # runtime's heap on purpose.
                return _lib.flowd_py_strdup(out)
            except Exception as exc:  # never let an exception cross into C
                err = json.dumps(
                    {
                        "severity": "error",
                        "id": "R155",
                        "message": f"{type(exc).__name__}: {exc}",
                    }
                ).encode("utf-8")
                err_ptr[0] = _lib.flowd_py_strdup(err)
                return _ffi.NULL

        return _cb

    # -- secrets ------------------------------------------------------

    def set_redactor(self, redactor):
        """Install a byte-rewrite redactor (layer 3 of the secrets pattern).

        ``redactor(data: bytes) -> bytes | None`` runs on every value the
        trace writer persists; return new bytes to rewrite, or ``None`` to
        leave them untouched. ``redactor=None`` clears any prior redactor.
        """
        self._check_open()
        if redactor is None:
            _lib.flowd_set_redactor(self._rt, _ffi.NULL, _ffi.NULL)
            return

        def _cb(data, length, out_len, _user_ctx):
            try:
                original = _ffi.buffer(data, length)[:]
                new = redactor(original)
                if new is None:
                    return _ffi.NULL
                buf = bytes(new)
                out_len[0] = len(buf)
                return _lib.flowd_py_memdup(buf, len(buf))
            except Exception:
                return _ffi.NULL  # on failure, leave bytes untouched

        cb = _ffi.callback("flowd_redactor_fn", _cb)
        self._keepalive.append(cb)
        _lib.flowd_set_redactor(self._rt, cb, _ffi.NULL)

    # -- providers ----------------------------------------------------

    def register_provider(self, adapter):
        """Register a model provider adapter (the gateway's preferred path).

        ``adapter`` supplies:
          - ``name``: a provider id string (e.g. ``"openai"``);
          - ``supports_model(model_id) -> bool``;
          - ``invoke(model_id, request_json: str) -> response`` (a JSON
            string or a JSON-serializable value).

        Adapters are tried in registration order; the first whose
        ``supports_model`` returns true serves each model call, taking
        precedence over any per-model method.
        """
        self._check_open()

        pname = getattr(adapter, "name", None)
        if not pname:
            raise FlowdError("provider adapter needs a 'name'")
        name_buf = _ffi.new("char[]", str(pname).encode("utf-8"))

        def _provider_name():
            return name_buf

        def _supports(model_id, _ctx):
            try:
                return 1 if adapter.supports_model(
                    _ffi.string(model_id).decode("utf-8")
                ) else 0
            except Exception:
                return 0

        def _invoke(model_id, request_json, err_msg, _ctx):
            try:
                resp = adapter.invoke(
                    _ffi.string(model_id).decode("utf-8"),
                    _ffi.string(request_json).decode("utf-8"),
                )
                if not isinstance(resp, (str, bytes)):
                    resp = json.dumps(resp)
                out = resp.encode("utf-8") if isinstance(resp, str) else resp
                return _lib.flowd_py_strdup(out)
            except Exception as exc:
                err_msg[0] = _lib.flowd_py_strdup(
                    f"{type(exc).__name__}: {exc}".encode("utf-8")
                )
                return _ffi.NULL

        cb_name = _ffi.callback("flowd_provider_name_fn", _provider_name)
        cb_supports = _ffi.callback("flowd_provider_supports_fn", _supports)
        cb_invoke = _ffi.callback("flowd_provider_invoke_fn", _invoke)

        struct = _ffi.new("flowd_provider_adapter_t *")
        struct.provider_name = cb_name
        struct.supports_model = cb_supports
        struct.invoke = cb_invoke
        struct.user_ctx = _ffi.NULL
        struct.invoke_with_metrics = _ffi.NULL

        keep = [cb_name, cb_supports, cb_invoke, name_buf, struct]

        # Optional v2 path: an adapter that also reports token/cost metrics via
        # ``invoke_with_metrics(model_id, request_json) -> {"response", "tokens_in",
        # "tokens_out", "cost_cents"}``. The gateway prefers it when set and rolls
        # the reported usage into the run's budget and the trace's model_call nodes.
        metrics_fn = getattr(adapter, "invoke_with_metrics", None)
        if callable(metrics_fn):
            def _metrics(model_id, request_json, result, _ctx):
                try:
                    m = metrics_fn(
                        _ffi.string(model_id).decode("utf-8"),
                        _ffi.string(request_json).decode("utf-8"),
                    )
                    resp = m["response"]
                    if not isinstance(resp, (str, bytes)):
                        resp = json.dumps(resp)
                    out = resp.encode("utf-8") if isinstance(resp, str) else resp
                    result.response_json = _lib.flowd_py_strdup(out)
                    result.err_msg = _ffi.NULL
                    result.tokens_in = int(m.get("tokens_in", 0))
                    result.tokens_out = int(m.get("tokens_out", 0))
                    result.cost_cents = float(m.get("cost_cents", 0.0))
                except Exception as exc:
                    result.response_json = _ffi.NULL
                    result.err_msg = _lib.flowd_py_strdup(
                        f"{type(exc).__name__}: {exc}".encode("utf-8")
                    )

            cb_metrics = _ffi.callback("flowd_provider_metrics_fn", _metrics)
            struct.invoke_with_metrics = cb_metrics
            keep.append(cb_metrics)

        # struct + callbacks + name buffer must outlive the runtime.
        self._keepalive.extend(keep)

        rc = _lib.flowd_register_provider(self._rt, struct)
        if rc != 0:
            raise FlowdRegistrationError(
                f"register_provider: {_REGISTER_CODES.get(rc, f'code {rc}')}",
                code=str(rc),
            )

    # -- execution ----------------------------------------------------

    def run(self, input, flow=None):
        """Run a flow. Returns its output, a ``Suspension``, or raises.

        ``flow`` selects a named flow in a multi-flow IR (via
        ``flowd_run_named``); omit it to run the IR's first flow.
        """
        self._check_open()
        susp = _ffi.new("char **")
        input_json = json.dumps(input).encode("utf-8")
        trace_dir = self._trace_dir.encode("utf-8")
        if flow is None:
            out = _lib.flowd_run(self._rt, input_json, trace_dir, susp)
        else:
            out = _lib.flowd_run_named(
                self._rt, flow.encode("utf-8"), input_json, trace_dir, susp
            )
        return self._finish(out, susp)

    def resume(self, token, decision):
        """Resume a suspended flow. Writes a new trace; original is untouched."""
        self._check_open()
        susp = _ffi.new("char **")
        out = _lib.flowd_resume(
            self._rt,
            token.encode("utf-8"),
            json.dumps(decision).encode("utf-8"),
            susp,
        )
        return self._finish(out, susp)

    def replay(self, original_trace_dir, new_trace_dir, *, model=None, flow=None):
        """Re-execute a recorded trace, writing a new one, and return its output.

        ``model=None`` restores every node from the original trace (verifies
        determinism; a mutation is never re-invoked). A model id re-invokes
        only the model nodes via the gateway against that model with the
        recorded inputs — register a provider that serves it first. ``flow``
        selects the flow (defaults to the IR's sole flow). Pair with
        ``diff_traces(original_trace_dir, new_trace_dir)`` to see what changed.
        """
        self._check_open()
        out = _lib.flowd_replay(
            self._rt,
            (flow or self._sole_flow()).encode("utf-8"),
            str(original_trace_dir).encode("utf-8"),
            str(new_trace_dir).encode("utf-8"),
            model.encode("utf-8") if model else _ffi.NULL,
        )
        if out == _ffi.NULL:
            raise self._last_error("replay failed")
        value = _ffi.string(out).decode("utf-8")
        _lib.free(out)
        return json.loads(value)

    def _sole_flow(self):
        flows = self._ir.get("flows", [])
        if len(flows) == 1:
            return flows[0]["name"]
        raise FlowdError(
            "the IR declares multiple flows; pass flow=<name> to replay()"
        )

    def _finish(self, out, susp):
        # flowd_run/resume return NULL for BOTH completion-failure and
        # suspension; the suspension-token out-param disambiguates.
        if out != _ffi.NULL:
            value = _ffi.string(out).decode("utf-8")
            _lib.free(out)
            return json.loads(value)
        if susp[0] != _ffi.NULL:
            token = _ffi.string(susp[0]).decode("utf-8")
            _lib.free(susp[0])
            return Suspension(token, self)
        raise self._last_error("flow execution failed")

    # -- lifecycle ----------------------------------------------------

    def _last_error(self, fallback):
        err = _lib.flowd_last_error_json(self._rt) if self._rt else _ffi.NULL
        if err == _ffi.NULL:
            return FlowdError(fallback)
        text = _ffi.string(err).decode("utf-8")
        _lib.free(err)
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            return FlowdError(text)
        return FlowdError(obj.get("message", text), code=obj.get("id"))

    def _check_open(self):
        if self._closed:
            raise FlowdError("Runtime is closed")

    def close(self):
        if not self._closed and getattr(self, "_rt", None) not in (None, _ffi.NULL):
            _lib.flowd_destroy(self._rt)
        self._rt = None
        self._closed = True

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass
