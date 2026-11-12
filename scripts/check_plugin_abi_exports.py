#!/usr/bin/env python3
"""Check that every built production plugin defines both ABI exports itself."""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


SYMBOLS = {"deeplux_plugin_abi_version", "deeplux_plugin_abi_fingerprint"}
TEST_HOOK_MARKERS = ("ForTest", "faultInjection", "FaultInjection", "HandshakeHook")


def has_library_suffix(path, suffixes):
    return path.suffix.lower() in suffixes or (os.name != "nt" and sys.platform != "darwin" and ".so." in path.name)


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
    excluded = {"tests", "abi-fixtures"}
    return sorted(
        path
        for path in build_dir.rglob("*Plugin*")
        if path.is_file()
        and has_library_suffix(path, suffixes)
        and not excluded.intersection(part.lower() for part in path.relative_to(build_dir).parts)
    )


def release_binaries(build_dir):
    suffixes = {".dll", ".exe"} if os.name == "nt" else {".so", ".dylib"}
    excluded = {"tests", "abi-fixtures"}
    return sorted(
        path
        for path in build_dir.rglob("*")
        if path.is_file()
        and not path.name.lower().startswith("test_")
        and not excluded.intersection(part.lower() for part in path.relative_to(build_dir).parts)
        and (
            has_library_suffix(path, suffixes)
            or (os.name != "nt" and path.parent.name == "bin" and os.access(path, os.X_OK))
        )
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
    parser.add_argument("--check-release-no-test-hooks", action="store_true")
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

    if args.check_release_no_test_hooks:
        candidates = release_binaries(args.build_dir.resolve())
        if not candidates:
            print(f"No release binaries found under {args.build_dir}", file=sys.stderr)
            return 1
        for binary in candidates:
            test_hooks = sorted(
                name for name in exports(binary, tool) if any(marker in name for marker in TEST_HOOK_MARKERS)
            )
            if test_hooks:
                for symbol in test_hooks:
                    print(f"TEST_HOOK_EXPORT {binary}: {symbol}")
                failures.append((binary, test_hooks))
            else:
                print(f"OK {binary}: no test-hook exports")

    checks = len(binaries) + (len(candidates) if args.check_release_no_test_hooks else 0)
    print(f"ABI/release checks: {checks - len(failures)}/{checks} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
