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

## Hosted CI findings and remediation

The first PR #9 run (`44277686502`) failed in three independent areas:

- Qt 6 and Windows builds rejected `PluginToolBoxTree::mimeData` because its parameter was passed by value, while Qt 6 requires a const reference.
- Ubuntu Qt 5, ASan/UBSan, and TSan jobs stopped at plugin-sync verification. CMake now stages plugins beside the build executable, but the verifier still looked under the isolated user app-data directory.
- Windows plugin staging used `Get-FileHash`, which was unavailable in the PowerShell runtime used by the runner.

The fixes use the version-correct Qt signature, verify both supported build-tree runtime layouts, and compute DLL hashes through .NET's SHA-256 implementation. The workflow labels now describe build-tree verification. The rerun passed Qt 5, Qt 6, TSan, docs-status, format, and plugin-affinity jobs. Windows then exposed another path-normalization defect: the stale-file cleanup treated the copied DLL as stale because CMake's destination contained `Release/..`; that comparison has now been corrected to compare normalized paths. Windows CTest and ASan/UBSan results were still running when this follow-up was recorded.

## Follow-up local validation — 2026-11-19

- `cmake --build build-codex-agent -j2` — passed.
- Full CTest with a temporary HOME and loopback socket access — passed, 132/132, 0 failures, 3 allowlisted skips.
- Plugin-sync verifier and isolation configuration tests — passed; the verifier also passed against the actual local build tree.
- Documentation status tests 8/8, changed-file C++ format check, whitespace check, and plugin ABI export scan 66/66 — passed.

The normalized-path correction is pending push and a new hosted run. Windows/macOS, physical devices, and production model/provider behavior are not covered by this local Linux Qt 5.15.3 run.
