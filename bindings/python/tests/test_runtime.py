"""Runtime: execution, marshalling, registration, errors, lifecycle."""

import pytest

from flowd import FlowdError, FlowdRegistrationError, Runtime, load_ir


def make_triage(record=None):
    Triage = load_ir(_ir("triage"))

    class Impl(Triage):
        def risk_check(self, amount):
            if record is not None:
                record.append(amount)
            return {"score": amount // 100, "flagged": amount > 1000}

    return Impl


def _ir(name):
    from pathlib import Path

    return str(Path(__file__).parent / "fixtures" / f"{name}.ir.json")


@pytest.mark.parametrize("inp,expected", [(50, 1), (750, 2), (1500, 3)])
def test_run_scalar_input(tmp_path, inp, expected):
    with Runtime(make_triage()(), trace_dir=str(tmp_path)) as rt:
        assert rt.run(inp) == expected


def test_model_callback_receives_named_args(tmp_path):
    seen = []
    with Runtime(make_triage(seen)(), trace_dir=str(tmp_path)) as rt:
        rt.run(750)
    assert seen == [750]


def test_run_record_input_and_construct_output(tmp_path, ir):
    Onboard = load_ir(ir("onboard"))

    class Impl(Onboard):
        def credit_check(self, id):
            return 750 if id == "ok" else 100

        def record_decision(self, id, approved):
            return True

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        assert rt.run({"id": "ok", "amount": 9000}) == {"approved": True, "score": 750}
        assert rt.run({"id": "no", "amount": 1}) == {"approved": False, "score": 100}


def test_deterministic_and_mutation_tools_register(tmp_path, ir):
    """Non-model effect levels register via flowd_register_tool."""
    Onboard = load_ir(ir("onboard"))
    calls = []

    class Impl(Onboard):
        def credit_check(self, id):
            calls.append(("credit", id))
            return 800

        def record_decision(self, id, approved):
            calls.append(("record", id, approved))
            return True

    with Runtime(Impl(), trace_dir=str(tmp_path)) as rt:
        rt.run({"id": "x", "amount": 1})
    assert calls == [("credit", "x"), ("record", "x", True)]


def test_run_named_unknown_flow_raises(tmp_path):
    with Runtime(make_triage()(), trace_dir=str(tmp_path)) as rt:
        with pytest.raises(FlowdError) as exc:
            rt.run(10, flow="nope")
    assert exc.value.code == "R155"


def test_callback_exception_becomes_flowderror(tmp_path, ir):
    Triage = load_ir(ir("triage"))

    class Boom(Triage):
        def risk_check(self, amount):
            raise ValueError("kaboom")

    with Runtime(Boom(), trace_dir=str(tmp_path)) as rt:
        with pytest.raises(FlowdError):
            rt.run(750)


def test_noncallable_override_rejected(tmp_path, ir):
    Triage = load_ir(ir("triage"))

    class Bad(Triage):
        risk_check = None  # satisfies abc (name present) but not callable

    with pytest.raises(FlowdRegistrationError):
        Runtime(Bad(), trace_dir=str(tmp_path))


def test_impl_version_accepted(tmp_path):
    # Registration succeeds with an impl_version (the runtime records it;
    # the current libflowd build does not yet persist it to traces).
    impl = make_triage()()
    impl.impl_version = "v9"
    with Runtime(impl, trace_dir=str(tmp_path), impl_version="v9") as rt:
        assert rt.run(750) == 2


def test_output_is_parsed_not_a_string(tmp_path):
    with Runtime(make_triage()(), trace_dir=str(tmp_path)) as rt:
        assert isinstance(rt.run(750), int)


def test_closed_runtime_raises(tmp_path):
    rt = Runtime(make_triage()(), trace_dir=str(tmp_path))
    rt.close()
    with pytest.raises(FlowdError, match="closed"):
        rt.run(750)


def test_context_manager_closes(tmp_path):
    with Runtime(make_triage()(), trace_dir=str(tmp_path)) as rt:
        rt.run(750)
    with pytest.raises(FlowdError, match="closed"):
        rt.run(750)


def test_trace_written(tmp_path):
    with Runtime(make_triage()(), trace_dir=str(tmp_path)) as rt:
        rt.run(750)
    manifests = list((tmp_path / "triage").glob("*/manifest.json"))
    assert len(manifests) == 1
