#!/usr/bin/env python3
"""Guard plugin-sync jobs against writing to a developer or runner profile."""

from __future__ import annotations

from pathlib import Path

import yaml

EXPECTED_SYNC_JOBS = {
    "ubuntu-qt5": "qt5",
    "windows-msvc": "windows",
    "windows-msvc-ctest": "windows-ctest",
    "linux-asan-ubsan": "asan",
    "linux-tsan": "tsan",
    "ubuntu-qt6": "qt6",
}


def assert_sync_verification(name: str, step: dict[str, str]) -> None:
    command = step.get("run", "").strip()
    expected = "python scripts/verify_ci_plugin_sync.py --build-dir build --plugin FitCirclePlugin"
    expected_unix = "python3 scripts/verify_ci_plugin_sync.py --build-dir build --plugin FitCirclePlugin"
    assert command in {expected, expected_unix}, f"{name}: post-sync step must invoke the behavior-tested verifier"


def main() -> int:
    mainwindow_wrapper = Path(__file__).resolve().with_name("run-mainwindow-test-isolated.sh")
    wrapper_text = mainwindow_wrapper.read_text(encoding="utf-8")
    assert 'env -u DEEPLUX_APP_DATA_DIR HOME="$test_home" bash "$project_dir/scripts/sync-plugins.sh"' in wrapper_text, (
        "isolated MainWindow plugin sync must ignore the parent app-data override"
    )
    assert 'env -u DEEPLUX_APP_DATA_DIR HOME="$test_home" "$test_binary" "$@"' in wrapper_text, (
        "isolated MainWindow test must use the same temporary HOME as plugin sync"
    )

    workflow_path = Path(__file__).resolve().parents[1] / ".github/workflows/ci.yml"
    workflow = yaml.load(workflow_path.read_text(encoding="utf-8"), Loader=yaml.BaseLoader)
    jobs = workflow["jobs"]
    sync_jobs = set()
    verifier_tests = [step for step in jobs["plugin-affinity-lint"].get("steps", [])
                      if step.get("name") == "Test plugin-sync verifier"]
    assert len(verifier_tests) == 1, "CI must run the plugin-sync verifier behavior tests exactly once"
    assert verifier_tests[0].get("run", "").strip() == "python3 scripts/test_ci_plugin_sync_verifier.py"
    isolation_tests = [step for step in jobs["plugin-affinity-lint"].get("steps", [])
                       if step.get("name") == "Test plugin isolation configuration"]
    assert len(isolation_tests) == 1, "CI must run plugin-isolation behavior tests exactly once"
    assert isolation_tests[0].get("run", "").strip() == "python3 scripts/test_ci_plugin_isolation_config.py"

    for name, job in jobs.items():
        steps = job.get("steps", [])
        sync_indices = [i for i, step in enumerate(steps) if "sync-plugins" in step.get("run", "")]
        assert len(sync_indices) <= 1, f"{name}: expected at most one plugin-sync step"
        for sync_index in sync_indices:
            assert name not in sync_jobs, f"{name}: duplicate plugin-sync job"
            sync_jobs.add(name)
            isolation_indices = [
                i for i, step in enumerate(steps)
                if step.get("name") == "Isolate plugin data directory"
            ]
            preceding = [i for i in isolation_indices if i < sync_index]
            assert preceding, f"{name}: plugin sync has no preceding isolation step"
            isolate = steps[preceding[-1]]
            command = isolate.get("run", "").strip()
            name_arg = EXPECTED_SYNC_JOBS[name]
            expected_python = "python" if name.startswith("windows-") else "python3"
            assert command == f"{expected_python} scripts/configure_ci_plugin_isolation.py --name {name_arg}", (
                f"{name}: isolation step must invoke the behavior-tested platform helper"
            )

            verify_indices = [
                i for i, step in enumerate(steps)
                if i > sync_index and "Verify isolated plugin sync" in step.get("name", "")
            ]
            assert verify_indices, f"{name}: plugin sync has no post-sync isolation verification"
            assert_sync_verification(name, steps[verify_indices[0]])

    assert sync_jobs == set(EXPECTED_SYNC_JOBS), (
        f"plugin-sync job set changed: missing={sorted(set(EXPECTED_SYNC_JOBS) - sync_jobs)}, "
        f"unexpected={sorted(sync_jobs - set(EXPECTED_SYNC_JOBS))}"
    )
    print(f"Plugin-sync isolation configuration passed for {len(sync_jobs)} job(s): {', '.join(sorted(sync_jobs))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
