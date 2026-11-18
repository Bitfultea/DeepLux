# Troubleshooting

## CMake selects the wrong Qt

Configure from a clean build directory and explicitly select the intended Qt package or toolchain. For example, set `DEEPLUX_QT_PATH` as described in the root [README](../README.md) or select the Qt CMake package directory. Do not reuse a build directory configured for another Qt major version or compiler.

## Native SAM is unavailable

Native SAM is enabled by default and requires the ONNX Runtime SDK at configure/build time. Set `ONNXRUNTIME_DIR` to the SDK root containing `include/onnxruntime_c_api.h` and the platform library. At runtime, configure a valid native model package as described in the [SAM deployment guide](sam_native/stage5_packaging.md). A successful application build does not prove that a model package is installed or that CUDA is available.

## A plugin is missing or rejected

Build and synchronize plugins with `cmake --build build --target sync-plugins`. Check the application log for the resolved plugin path and ABI error code. A missing or mismatched ABI export requires rebuilding the plugin against the current application/toolchain; do not bypass the loader check or copy an older binary over the bundled plugin.

When testing locally, use a temporary `HOME` or `DEEPLUX_APP_DATA_DIR` so the test plugin directory is isolated from user-installed plugins.

## CTest fails to create a local socket

Some Agent/MCP tests use local Unix sockets. Run them in an environment that permits local socket creation. If the sandbox denies socket operations, record that environmental failure separately and rerun in an authorized test environment; do not relabel it as a product test pass.

## Build reports `No space left on device`

Check the temporary filesystem and build directory before attributing the failure to source code. On systems with a small `/tmp` tmpfs, set `TMPDIR` to a filesystem with adequate space and retry from a clean generated build directory.

## Hardware-dependent behavior differs

Camera, PLC, serial, CUDA, and vendor SDK behavior requires the corresponding runtime and hardware. A unit test, CPU inference run, or offscreen GUI test is not a substitute for the target-device acceptance recorded in [project status](STATUS.md) and the relevant acceptance report.
