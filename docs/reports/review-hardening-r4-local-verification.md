# Review Hardening R4 Local Verification Snapshot

- Date: 2026-11-13
- Branch / commit: `feature/review-hardening` / `86d335a`
- Scope: clean Linux build and full CTest at the committed source tree; not the complete R4 acceptance gate.

## Environment

- Linux, GCC 11.4.0, Qt 5.15.3, OpenCV 4.8.1, ONNX Runtime 1.23.2.
- `DEEPLUX_NATIVE_SAM=ON`, `ENABLE_HYMSON3D=OFF`, `BUILD_TESTS=ON`, Debug.
- CMake was asked for Qt 6, which was unavailable; configuration emitted a warning and selected Qt 5.15.3.
- CMake-generated `plugin-facts.json`: 69 source metadata files and 66 packaged plugin targets. The 69-file catalog includes platform-specific camera alternatives and optional plugins.

## Commands and Results

A fresh build directory outside the checkout was configured with the environment's Qt, OpenCV, Eigen, and ONNX Runtime SDKs. The SDK paths are intentionally omitted because they are machine-local.

```bash
cmake --build "$BUILD_DIR" -j4
HOME="$TEST_HOME" cmake --build "$BUILD_DIR" --target sync-plugins
HOME="$TEST_HOME" TMPDIR="$TMP_DIR" XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  QT_QPA_PLATFORM=offscreen ctest --test-dir "$BUILD_DIR" --output-on-failure -j4
```

- Clean configure: passed; Qt 6 fallback warning as described above.
- Full build: passed.
- Plugin sync: 66 prepared and copied into the isolated temporary HOME; the real user plugin directory was not used.
- CTest registration: 98 top-level tests.
- Full CTest: **98/98 passed**, 60.43 seconds.

The final CTest run used an environment that allowed the local Unix/TCP sockets required by Agent and MCP tests. An earlier restricted attempt was not counted: it both denied local socket creation and set a global `DEEPLUX_APP_DATA_DIR`, which invalidated a path-default test. The accepted run used only an isolated HOME and did not set that override.

## Not Covered

- Windows/MSVC and Qt 6 execution, including hosted CI results.
- ASan/UBSan and TSan acceptance gates.
- A clean `BUILD_TESTS=OFF` export scan for test-only symbols in the core library and release artifacts.
- Relocated deployment smoke with its exact expected module manifest.
- Real target-machine, camera/PLC, or industrial-image acceptance.

This snapshot is evidence for the local build and test slice only. It does not close the R4 acceptance gate or the D1–D5 documentation/governance work.
