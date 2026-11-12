#!/usr/bin/env python3
"""Group TSan summaries for review without suppressing or clearing findings."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RULES = ROOT / "scripts" / "tsan-shared-library-candidates.json"
SUMMARY_RE = re.compile(r"^SUMMARY: ThreadSanitizer: data race.*$", re.MULTILINE)


def classify(log_text: str, patterns: list[str]) -> tuple[list[str], list[str]]:
    candidates: list[str] = []
    unclassified: list[str] = []
    compiled = [re.compile(pattern) for pattern in patterns]
    for summary in SUMMARY_RE.findall(log_text):
        target = candidates if any(pattern.fullmatch(summary) for pattern in compiled) else unclassified
        target.append(summary)
    return candidates, unclassified


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} <raw TSan log>", file=sys.stderr)
        return 2
    log_path = Path(sys.argv[1])
    if not log_path.is_file():
        print(f"TSan log not found: {log_path}", file=sys.stderr)
        return 2
    rules = json.loads(RULES.read_text(encoding="utf-8"))
    candidates, unclassified = classify(log_path.read_text(encoding="utf-8", errors="replace"), rules["patterns"])
    print(
        f"TSan summaries: {len(candidates)} direct QtCore shared-library location(s) for manual review, "
        f"{len(unclassified)} unclassified."
    )
    for summary in candidates:
        print(f"MANUAL REVIEW: {summary}")
    for summary in unclassified:
        print(f"UNCLASSIFIED: {summary}")
    print("No report is suppressed or declared an uninstrumented-Qt false positive; inspect the full raw stacks.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
