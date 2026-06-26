"""Ready-made trace redactors.

``set_redactor`` takes a ``bytes -> bytes | None`` callback; writing one by
hand for every host is boilerplate. ``secret_redactor`` returns a callback that
scrubs common credential shapes (Anthropic / OpenAI / AWS keys, bearer tokens)
from persisted trace bytes — layer 3 of the secrets pattern, batteries included.
"""

from __future__ import annotations

import re

# Common credential shapes. Bytes patterns (the redactor receives bytes).
_DEFAULT_PATTERNS = [
    rb"sk-ant-[A-Za-z0-9_-]{20,}",        # Anthropic API key
    rb"sk-[A-Za-z0-9]{20,}",              # OpenAI-style key
    rb"AKIA[0-9A-Z]{16}",                 # AWS access key id
    rb"ghp_[A-Za-z0-9]{20,}",             # GitHub personal access token
    rb"(?i)bearer\s+[A-Za-z0-9._\-]+",    # bearer tokens in headers/errors
]


def secret_redactor(*extra_patterns, replacement: bytes = b"[REDACTED]"):
    """Return a redactor that replaces known credential shapes with
    ``replacement``. Pass extra ``str``/``bytes`` regexes to scrub host-specific
    secrets too; returns ``None`` (leave untouched) when nothing matched.

        rt.set_redactor(secret_redactor())
        rt.set_redactor(secret_redactor(rb"tok_live_[a-z0-9]+"))
    """
    patterns = list(_DEFAULT_PATTERNS)
    for p in extra_patterns:
        patterns.append(p.encode("utf-8") if isinstance(p, str) else p)
    compiled = [re.compile(p) for p in patterns]

    def redact(data: bytes):
        out = data
        for rx in compiled:
            out = rx.sub(replacement, out)
        return out if out != data else None

    return redact
