#!/usr/bin/env python3
"""Verify a synchronized plugin is an exact copy of a build artifact."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import sys


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_plugin_sync(build_dir: Path, app_data_dir: Path, plugin: str) -> None:
    names = {f"{prefix}{plugin}{suffix}" for prefix in ("", "lib") for suffix in (".dll", ".so", ".dylib")}
    build_roots = [path for path in (build_dir / "bin", build_dir / "lib") if path.is_dir()]
    built = [path for root in build_roots for path in root.rglob("*") if path.is_file() and path.name in names]
    synced = [
        path
        for path in (app_data_dir / "plugins").rglob("*")
        if path.is_file() and path.name in names
    ] if (app_data_dir / "plugins").is_dir() else []

    if not built:
        raise RuntimeError(f"no {plugin} build artifact found under {build_dir}/bin or {build_dir}/lib")
    if len(synced) != 1:
        raise RuntimeError(f"expected exactly one synchronized {plugin} library, found {len(synced)}")

    synced_hash = sha256(synced[0])
    if not any(sha256(path) == synced_hash for path in built):
        raise RuntimeError(f"synchronized {synced[0]} differs from every {plugin} build artifact")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--plugin", required=True)
    args = parser.parse_args()
    app_data = os.environ.get("DEEPLUX_APP_DATA_DIR")
    if not app_data:
        parser.error("DEEPLUX_APP_DATA_DIR is not set")
    try:
        verify_plugin_sync(args.build_dir, Path(app_data), args.plugin)
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    print(f"Verified synchronized {args.plugin} against build artifacts")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
