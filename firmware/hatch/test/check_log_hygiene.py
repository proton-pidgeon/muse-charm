#!/usr/bin/env python3
"""Log hygiene for the Vesper firmware (task 11): no secret may reach a log.

Scans every logging call in the Vesper-owned firmware sources (firmware/hatch/*.c|h) and in
the lines the SDK patch set adds (firmware/sdk-patches/*.patch), and fails if one passes a
secret as an argument: the node credential, the claim secret, the node bearer (token), the
Authorization buffer, a header value, or the claim code (the wire doc keeps it off serial).
String literals and comments are ignored (a format string may say "token refused"); presence
checks such as ``token[0] ? "set" : "unset"`` or ``vesper_cred_present()`` are allowed.

    python3 firmware/hatch/test/check_log_hygiene.py firmware/hatch firmware/sdk-patches

It first checks itself against known-bad and known-good calls. Exit 0 when clean; 1 with one
line per offending call otherwise. Run by ``make -C firmware test``.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

LOG_CALL = re.compile(
    r"\b(ESP_LOG[EWIDV]|ESP_EARLY_LOG[EWIDV]|ESP_DRAM_LOG[EWIDV]|printf|fprintf|puts|"
    r"ets_printf|esp_rom_printf|muse_hatch_console)\s*\("
)
# An identifier with one of these as an underscore-separated part holds a secret (s_cred,
# claim_secret, node_token, auth_buf, hdrs, ...).
SECRET_PARTS = {"secret", "credential", "cred", "token", "auth", "bearer", "authline", "hdrs"}
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\s*(?:\.|->)\s*[A-Za-z_][A-Za-z0-9_]*)*")
# Calls whose name says they return no secret: x_present(), x_len(), ...
SAFE_SUFFIXES = ("_present", "_len", "_valid", "_state_name")
# A presence check right after the identifier is fine: x[0] ? ..., x[0] != ...
PRESENCE = re.compile(r"\s*\[\s*0\s*\]\s*(\?|!=|==)")
STRING = re.compile(r'"(?:\\.|[^"\\])*"' + r"|'(?:\\.|[^'\\])*'")
COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)


def calls(text: str):
    """(offset, argument text) of every logging call, parentheses matched."""
    for m in LOG_CALL.finditer(text):
        depth, i = 1, m.end()
        while i < len(text) and depth:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
            i += 1
        yield m.start(), text[m.end() : i - 1]


def offenders(args: str) -> list[str]:
    bare = STRING.sub('""', args)
    found = []
    for m in IDENT.finditer(bare):
        chain = re.split(r"\s*(?:\.|->)\s*", m.group(0))
        last = chain[-1]
        if PRESENCE.match(bare, m.end()) or last.endswith(SAFE_SUFFIXES):
            continue
        parts = {p for ident in chain for p in ident.lower().split("_")}
        claim_code = last == "code" and any("claim" in i for i in chain[:-1])
        if parts & SECRET_PARTS or claim_code or last == "value":
            found.append(m.group(0))
    return found


def scan(name: str, text: str) -> list[str]:
    text = COMMENT.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    out = []
    for off, args in calls(text):
        bad = offenders(args)
        if bad:
            line = text.count("\n", 0, off) + 1
            out.append(f"{name}:{line}: log call passes {', '.join(sorted(set(bad)))}")
    return out


def added_lines(patch: str) -> str:
    """The lines a patch adds, one blob per file (context dropped, so calls can't cross files)."""
    blobs: list[str] = []
    for line in patch.splitlines():
        if line.startswith("+++ "):
            blobs.append("")
        elif line.startswith("+") and blobs:
            blobs[-1] += line[1:] + "\n"
    return "\n\n;\n\n".join(blobs)


SELF_TEST_BAD = [
    'ESP_LOGI(TAG, "cred %s", s_cred);',
    'ESP_LOGI(TAG, "x %s", cred);',
    'ESP_LOGW(TAG, "secret %s",\n         s_claim.secret);',
    'printf("%s\\n", auth);',
    'ESP_LOGI(TAG, "code %s", s_claim.code);',
    'ESP_LOGI(TAG, "h %s", s_turn.hdrs[5].value);',
    'ESP_LOGI(TAG, "t %s", token);',
    'ESP_LOGI(TAG, "t %s", s_turn.auth);',
]
SELF_TEST_OK = [
    'ESP_LOGI(TAG, "token refused, credential %s", vesper_cred_present() ? "yes" : "no");',
    'ESP_LOGI(TAG, "%s", s.token[0] ? "node token set" : "no node token");',
    'ESP_LOGI(TAG, "turn: HTTP %d%s%s", status, code[0] ? " " : "", code);',
    '/* ESP_LOGI(TAG, "%s", secret); */',
    'ESP_LOGI(TAG, "claimed: room %s", s_claim.room);',
]


def self_test() -> bool:
    ok = all(scan("bad", t) for t in SELF_TEST_BAD) and not any(scan("ok", t) for t in SELF_TEST_OK)
    print(f"log hygiene self-test: {'ok' if ok else 'FAILED'}")
    return ok


def main(argv: list[str]) -> int:
    if not self_test():
        return 1
    problems: list[str] = []
    scanned = 0
    for root in map(Path, argv[1:]):
        for path in sorted(root.glob("*")):
            if path.suffix in {".c", ".h"}:
                problems += scan(str(path), path.read_text(encoding="utf-8"))
                scanned += 1
            elif path.suffix == ".patch":
                problems += scan(str(path) + " (added lines)", added_lines(path.read_text(encoding="utf-8")))
                scanned += 1
    for p in problems:
        print(p)
    print(f"log hygiene: {scanned} file(s), {len(problems)} offending log call(s)")
    return 1 if problems or not scanned else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
