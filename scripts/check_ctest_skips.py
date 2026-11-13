#!/usr/bin/env python3
"""Fail when QtTest reports a skip not documented in the CI allowlist."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / "scripts" / "ctest-skip-allowlist.json"
SKIP_RE = re.compile(r"^SKIP\s*:\s*([A-Za-z_]\w*::[A-Za-z_]\w*)\(\)\s*(.*)$", re.MULTILINE)


def check(log_text: str, allowlist: dict[str, dict[str, object]]) -> tuple[list[str], int]:
    errors: list[str] = []
    skips = list(SKIP_RE.finditer(log_text))
    for match in skips:
        test_id, reason = match.groups()
        entry = allowlist.get(test_id)
        phrases = entry.get("reason_contains", []) if entry else []
        if not any(phrase in reason for phrase in phrases):
            errors.append(f"unapproved skip: {test_id}: {reason}")
    return errors, len(skips)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <CTest LastTest.log>", file=sys.stderr)
        return 2
    log_path = Path(sys.argv[1])
    if not log_path.is_file():
        print(f"CTest log not found: {log_path}", file=sys.stderr)
        return 2
    allowlist = json.loads(ALLOWLIST.read_text(encoding="utf-8"))
    errors, count = check(log_path.read_text(encoding="utf-8", errors="replace"), allowlist)
    for error in errors:
        print(error, file=sys.stderr)
    if errors:
        return 1
    print(f"QSKIP gate passed: {count} skip(s), all explicitly allowlisted.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
