"""Shared pytest setup for the flowd binding tests.

Makes the source package importable (so the suite runs straight from a
checkout, not only after `pip install -e .`) and ensures the cffi
extension is built before any test constructs a Runtime.
"""

import subprocess
import sys
from pathlib import Path

import pytest

PKG_ROOT = Path(__file__).resolve().parents[1]  # bindings/python
FIXTURES = Path(__file__).parent / "fixtures"

sys.path.insert(0, str(PKG_ROOT))


def pytest_configure(config):
    try:
        import flowd._flowd  # noqa: F401
    except Exception:
        # Build the static-linked extension (needs cffi + a C toolchain
        # and flowd/libflowd.a present).
        subprocess.run(
            [sys.executable, "-m", "flowd._cffi_build"],
            cwd=PKG_ROOT,
            check=True,
        )


@pytest.fixture
def ir():
    """Return a function mapping a fixture name to its .ir.json path."""
    return lambda name: str(FIXTURES / f"{name}.ir.json")
