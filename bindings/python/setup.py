"""Build shim: compile the cffi extension that static-links libflowd.a.

All metadata lives in pyproject.toml; this file exists only to pass
``cffi_modules`` to setuptools, which cffi's build integration reads to
build flowd._flowd from flowd/_cffi_build.py during install.
"""

from setuptools import setup

setup(cffi_modules=["flowd/_cffi_build.py:ffibuilder"])
