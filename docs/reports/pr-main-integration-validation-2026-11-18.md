# PR Main Integration Validation

Date: 2026-11-18

Branch: `feature/pr-main-merge-display-fixes-20261118`

Base: `origin/main` at `09303861786b732372827a0955bc7c4a4db25481`

## Local checks

- `cmake --build build-codex-agent -j2` — passed.
- `ctest --test-dir build-codex-agent --output-on-failure` — passed, 132/132 tests, 0 failures. Three QtTest skips were accepted by the repository allowlist: the native SAM image case (SAM disabled), the macOS-only binary-discovery case, and a worker-thread log case that requires a debug build.
- `python3 scripts/check_ctest_skips.py build-codex-agent/Testing/Temporary/LastTest.log` — passed; all three skips are allowlisted.
- Changed C++ format check (`clang-format --dry-run --Werror`) and `git diff --check origin/main` — passed.
- CI helper tests, plugin-affinity lint, and plugin ABI export scan — passed; ABI exports verified for 66/66 built plugins.

The first full CTest attempt had one intermittent PLCWrite preview assertion failure. The complete PLCWrite test passed 13/13 on isolated rerun, and the subsequent full CTest passed 132/132. The failure did not reproduce; timing or test-state sensitivity is not fully ruled out.

## Hosted checks and remaining scope

GitHub Actions has not run for this PR yet. Windows, macOS, physical cameras/PLC/serial devices, and production model/provider behavior are not covered by this Linux Qt 5.15.3 run.
