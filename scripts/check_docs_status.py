#!/usr/bin/env python3
"""Validate the live status snapshot and require it in implementation PRs.

Local Markdown link targets are checked; heading fragments are intentionally not.
"""

import argparse
from datetime import date
import re
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
STATUS = "docs/STATUS.md"
IMPLEMENTATION_PREFIXES = ("src/", "tests/", "tools/", "scripts/", ".github/workflows/")


def requires_status_update(paths):
    return any(
        path.startswith(IMPLEMENTATION_PREFIXES)
        or path == "CMakeLists.txt"
        or path.startswith("cmake/")
        for path in paths
    )


def validate_status(text):
    errors = []
    fields = {}
    for field in ("Date", "Source revision", "Verification", "Evidence"):
        match = re.search(rf"^- {re.escape(field)}: (.+)$", text, re.MULTILINE)
        if not match:
            errors.append(f"missing or invalid `{field}` field")
        else:
            fields[field] = match.group(1).strip()

    revision_fields = re.findall(r"^- Source revision: (.+)$", text, re.MULTILINE)
    if len(revision_fields) > 1:
        errors.append("`Source revision` must appear exactly once")

    if "Date" in fields:
        try:
            date.fromisoformat(fields["Date"][:10])
        except ValueError:
            errors.append("`Date` must contain a valid YYYY-MM-DD date")

    if "Source revision" in fields:
        revisions = re.findall(r"\b[0-9a-f]{7,40}\b", fields["Source revision"], re.I)
        if not revisions:
            errors.append("`Source revision` must identify a hexadecimal Git revision")
        for revision in revisions:
            result = subprocess.run(
                ["git", "cat-file", "-e", f"{revision}^{{commit}}"],
                cwd=ROOT,
                capture_output=True,
            )
            if result.returncode != 0:
                errors.append(f"`Source revision` does not resolve to a commit: {revision}")

    if "Verification" in fields:
        verification = fields["Verification"]
        if not re.search(r"`[^`\n]+`", verification) or not re.search(
            r"\b(pass(?:ed)?|fail(?:ed)?|not run|not tested|blocked)\b", verification, re.I
        ):
            errors.append("`Verification` must include a command and an explicit result")

    if "Evidence" in fields and not re.search(r"\[[^]]+\]\([^)]+\)", fields["Evidence"]):
        errors.append("`Evidence` must include a Markdown link to a report")

    if not re.search(r"^- OPEN-BE-01: (OPEN|CLOSED)\b", text, re.MULTILINE):
        errors.append("`OPEN-BE-01` must have an explicit OPEN or CLOSED status")

    for target in re.findall(r"\[[^]]+\]\(([^)]+)\)", text):
        if "://" in target or target.startswith("#"):
            continue
        path = (ROOT / "docs" / target.split("#", 1)[0]).resolve()
        if not path.is_relative_to(ROOT.resolve()) or not path.is_file():
            errors.append(f"status link does not resolve: {target}")
    return errors


def changed_paths(base):
    result = subprocess.run(
        ["git", "diff", "--name-only", f"{base}...HEAD"],
        cwd=ROOT,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.splitlines()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", help="base commit for the same-PR status check")
    args = parser.parse_args()

    status_path = ROOT / STATUS
    errors = validate_status(status_path.read_text(encoding="utf-8")) if status_path.is_file() else ["docs/STATUS.md is missing"]
    if args.base:
        paths = changed_paths(args.base)
        if requires_status_update(paths) and STATUS not in paths:
            errors.append("implementation/build/CI changes require docs/STATUS.md in the same PR")

    if errors:
        print("Documentation status check failed:")
        for error in errors:
            print(f"- {error}")
        return 1
    print("Documentation status check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
