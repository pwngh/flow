#!/bin/sh
# Editable install of the flowd Python binding.
#
# Builds the cffi extension (flowd._flowd), which static-links
# flowd/libflowd.a — so this needs a C compiler, flowd/libflowd.a built
# (run ../../build.sh at the repo root first), and linkable libcrypto +
# libcurl (on macOS, Homebrew openssl@3 + curl; override via
# FLOWD_OPENSSL_LIBDIR / FLOWD_CURL_LIBDIR).
#
# --break-system-packages lets this work on an externally-managed
# (PEP 668) system Python such as Homebrew's; it is harmless inside a
# virtualenv. Any extra arguments are forwarded to pip.

set -eu

python3 -m pip install --editable . --break-system-packages "$@"
