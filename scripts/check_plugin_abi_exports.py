#!/usr/bin/env python3
"""Check that every built production plugin defines both ABI exports itself."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


SYMBOLS = {"deeplux_plugin_abi_version", "deeplux_plugin_abi_fingerprint"}


def find_dumpbin():
    found = shutil.which("dumpbin")
    if found:
        return found
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
        "Microsoft Visual Studio/Installer/vswhere.exe"
    )
    if vswhere.exists():
        result = subprocess.run(
            [str(vswhere), "-latest", "-products", "*", "-find", r"**\dumpbin.exe"],
            check=True,
            capture_output=True,
            text=True,
        )
        candidates = [line.strip() for line in result.stdout.splitlines() if line.strip()]
        if candidates:
            return candidates[0]
    return None


def plugin_binaries(build_dir):
    suffixes = {".dll"} if os.name == "nt" else {".so", ".dylib"}
    return sorted(
        path
        for path in build_dir.rglob("*Plugin*")
        if path.is_file()
        and path.suffix.lower() in suffixes
        and not {"tests", "abi-fixtures"}.intersection(part.lower() for part in path.parts)
    )


def exports(binary, tool):
    if os.name == "nt":
        output = subprocess.run([tool, "/nologo", "/exports", str(binary)], check=True, capture_output=True, text=True)
        return {line.split()[-1] for line in output.stdout.splitlines() if line.split()}
    flags = ["-gU"] if sys.platform == "darwin" else ["-D", "--defined-only"]
    output = subprocess.run([tool, *flags, str(binary)], check=True, capture_output=True, text=True)
    names = {line.split()[-1].split("@")[0] for line in output.stdout.splitlines() if line.split()}
    return {name.lstrip("_") for name in names} if sys.platform == "darwin" else names


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--tool", help="nm or dumpbin executable; auto-detected by default")
    args = parser.parse_args()
    tool = args.tool or (find_dumpbin() if os.name == "nt" else shutil.which("nm"))
    if not tool:
        parser.error("could not find the platform export-table tool")

    binaries = plugin_binaries(args.build_dir.resolve())
    if not binaries:
        print(f"No production plugin binaries found under {args.build_dir}", file=sys.stderr)
        return 1

    failures = []
    for binary in binaries:
        missing = SYMBOLS - exports(binary, tool)
        if missing:
            failures.append((binary, sorted(missing)))
            print(f"MISSING {binary}: {', '.join(sorted(missing))}")
        else:
            print(f"OK {binary}")
    print(f"Plugin ABI exports: {len(binaries) - len(failures)}/{len(binaries)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
