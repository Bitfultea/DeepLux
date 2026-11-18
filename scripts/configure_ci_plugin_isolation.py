#!/usr/bin/env python3
"""Create per-job plugin data and profile directories for GitHub Actions."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil


def configure_isolation(name: str, runner_temp: Path, github_env: Path, *, is_windows: bool) -> None:
    if not name.replace("-", "").isalnum():
        raise ValueError("job name must contain only letters, digits, and hyphens")

    if is_windows:
        app_data = runner_temp / f"deeplux-appdata-{name}"
        profile = runner_temp / f"deeplux-userprofile-{name}"
        profile_key = "USERPROFILE"
    else:
        profile = runner_temp / f"deeplux-home-{name}"
        app_data = profile / ".deeplux"
        profile_key = "HOME"

    app_data.mkdir(parents=True, exist_ok=True)
    profile.mkdir(parents=True, exist_ok=True)
    shutil.rmtree(app_data / "plugins", ignore_errors=True)
    with github_env.open("a", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{profile_key}={profile}\n")
        stream.write(f"DEEPLUX_APP_DATA_DIR={app_data}\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    args = parser.parse_args()
    runner_temp = os.environ.get("RUNNER_TEMP")
    github_env = os.environ.get("GITHUB_ENV")
    if not runner_temp or not github_env:
        parser.error("RUNNER_TEMP and GITHUB_ENV must be set")
    configure_isolation(args.name, Path(runner_temp), Path(github_env), is_windows=os.name == "nt")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
