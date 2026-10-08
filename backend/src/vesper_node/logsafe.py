"""Log redaction: never log tokens or keys.

A slimmed mirror of ``vesper-voice/server/src/vesper_voice/logsafe.py``. Every log record
is rewritten before it is emitted:

* node credentials (``vnc_…``) and claim secrets (``vcs_…``) by shape (task 08);
* known secret values, registered by :mod:`vesper_node.config`, are masked verbatim;
* credential fields (``Authorization: …``, ``xi-api-key: …``, ``token=…``,
  ``Bearer …``/``Token …``) and ElevenLabs/Deepgram key shapes are masked;
* ``bytes`` arguments (audio) are replaced by a ``<redacted:bytes len=N>`` placeholder.

Redaction is a backstop. The primary rule is that the code never passes transcript or reply
*text* to a logger at all (Kevin's Q4 decision: transcripts are transient). The test-suite
checks that rule against captured logs.
"""

from __future__ import annotations

import logging
import re
import threading

REDACTED = "[REDACTED]"
_MIN_SECRET_LEN = 8

_KEY_PATTERNS: tuple[re.Pattern[str], ...] = (
    re.compile(r"\bsk_[A-Fa-f0-9]{32,}\b"),  # ElevenLabs
    re.compile(r"\b[A-Fa-f0-9]{40}\b"),  # Deepgram
    re.compile(r"\b(?:sk|pk|rk)-[A-Za-z0-9_\-]{20,}"),
    re.compile(r"\bv(?:nc|cs)_[A-Za-z0-9_\-]{20,}"),  # node credential / claim secret (task 08)
)
_FIELD_PATTERN = re.compile(
    r"""(?ix)
    (?P<name> authorization | xi-api-key | x-api-key | api[_-]?key | secret | token | password
               | credential | x-vesper-node-admin | claim[_-]?code )
    (?P<sep>["']?\s*[:=]\s*["']?)
    (?:(?:bearer|token|basic)\s+)?
    (?P<value>[^\s"',;&}]+)
    """
)
_AUTH_SCHEME_PATTERN = re.compile(r"(?i)\b(bearer|token|basic)\s+[A-Za-z0-9_\-\.=+/]{8,}")

_registered: set[str] = set()
_lock = threading.Lock()


def register_secret(value: str | None) -> None:
    if value and len(value) >= _MIN_SECRET_LEN:
        with _lock:
            _registered.add(value)


def redact_text(text: str) -> str:
    if not text:
        return text
    with _lock:
        values = sorted(_registered, key=len, reverse=True)
    for value in values:
        if value in text:
            text = text.replace(value, REDACTED)
    text = _FIELD_PATTERN.sub(lambda m: f"{m.group('name')}{m.group('sep')}{REDACTED}", text)
    text = _AUTH_SCHEME_PATTERN.sub(lambda m: f"{m.group(1)} {REDACTED}", text)
    for pattern in _KEY_PATTERNS:
        text = pattern.sub(REDACTED, text)
    return text


def _sanitize_arg(value: object) -> object:
    if isinstance(value, bytes | bytearray | memoryview):
        return f"<redacted:bytes len={len(value)}>"
    if isinstance(value, str):
        return redact_text(value)
    return value


def sanitize_record(record: logging.LogRecord) -> logging.LogRecord:
    if getattr(record, "_vesper_node_redacted", False):
        return record
    if isinstance(record.args, tuple):
        record.args = tuple(_sanitize_arg(a) for a in record.args)
    try:
        message = record.getMessage()
    except Exception:  # noqa: BLE001 - malformed format string: keep the raw template
        message = str(record.msg)
    record.msg = redact_text(message)
    record.args = None
    if record.exc_info and not record.exc_text:
        record.exc_text = logging.Formatter().formatException(record.exc_info)
    if record.exc_text:
        record.exc_text = redact_text(record.exc_text)
    record.exc_info = None
    record._vesper_node_redacted = True  # type: ignore[attr-defined]
    return record


class RedactingFilter(logging.Filter):
    def filter(self, record: logging.LogRecord) -> bool:
        sanitize_record(record)
        return True


_FILTER = RedactingFilter()
_installed = False
_original_handle = logging.Handler.handle
# Loggers that can echo request URLs/headers at DEBUG.
_NOISY_LOGGERS = ("httpx", "httpcore", "hpack", "multipart")


def _redacting_handle(self: logging.Handler, record: logging.LogRecord) -> bool:
    sanitize_record(record)
    return _original_handle(self, record)


def install_log_redaction() -> None:
    """Filter on the root logger and every handler, present and future (idempotent)."""
    global _installed
    root = logging.getLogger()
    if _FILTER not in root.filters:
        root.addFilter(_FILTER)
    for handler in root.handlers:
        if _FILTER not in handler.filters:
            handler.addFilter(_FILTER)
    for name in _NOISY_LOGGERS:
        logging.getLogger(name).setLevel(logging.WARNING)
    with _lock:
        if not _installed:
            logging.Handler.handle = _redacting_handle  # type: ignore[method-assign]
            logging.captureWarnings(True)
            _installed = True
