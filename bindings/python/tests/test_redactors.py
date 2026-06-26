"""Built-in secret_redactor."""

from flowd import secret_redactor


def test_scrubs_known_key_shapes():
    r = secret_redactor()
    out = r(b"key=sk-ant-abcDEF012345678901234567 aws=AKIAABCDEFGHIJKLMNOP")
    assert b"sk-ant-" not in out
    assert b"AKIA" not in out
    assert b"[REDACTED]" in out


def test_returns_none_when_nothing_matches():
    assert secret_redactor()(b"just a normal trace value") is None


def test_bearer_token():
    out = secret_redactor()(b"Authorization: Bearer ya29.A0ARrdaM-some-token_value")
    assert b"ya29" not in out


def test_extra_pattern_str_and_bytes():
    r = secret_redactor(r"tok_live_[a-z0-9]+", rb"cust_secret_[0-9]+")
    out = r(b"a tok_live_abc123 and cust_secret_999 here")
    assert b"tok_live_abc123" not in out
    assert b"cust_secret_999" not in out


def test_custom_replacement():
    out = secret_redactor(replacement=b"X")(b"sk-ant-abcDEF012345678901234567")
    assert out == b"X"
