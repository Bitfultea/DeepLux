# Repository Guidelines

## Project Structure & Module Organization

DeepLux is a C++17 Qt machine-vision application. Core application entry points live in `src/app`, shared business logic and interfaces in `src/core`, and Qt UI code in `src/ui`. Runtime modules are organized as plugins under `src/plugins/<domain>/<PluginName>/`, usually with `CMakeLists.txt`, `metadata.json`, and `*Plugin.{h,cpp}` files. Unit tests are in `tests`, design notes and CLI references are in `docs`, reusable CMake helpers are in `cmake`, and local build/deploy helpers are in `scripts`.

## Build, Test, and Development Commands

Configure a local build with Qt and tests enabled:

```bash
cmake -S . -B build -DDEEPLUX_QT_PATH=/path/to/Qt/6.6.0/gcc_64 -DBUILD_TESTS=ON
```

Build everything:

```bash
cmake --build build -j$(nproc)
```

Run tests through CTest:

```bash
ctest --test-dir build --output-on-failure
```

Sync rebuilt plugin libraries into `~/.deeplux/plugins` when testing plugin loading:

```bash
cmake --build build --target sync-plugins
```

Launch the app or CLI from the build output, for example `./build/bin/DeepLux --gui` or `./scripts/deeplux help`. Halcon Runtime is required; OpenCV and camera SDKs are optional.

## Coding Style & Naming Conventions

Use the repository `.clang-format`: LLVM base, 4 spaces, no tabs, 120-column limit, attached braces, C++17, sorted/regrouped includes. Format touched C++ files with `clang-format -i path/to/file.cpp`. Keep code inside the `DeepLux` namespace where existing modules do so. Use `PascalCase` for classes, `lowerCamelCase` for functions, and the existing `m_` prefix for member variables. Plugin classes and folders should match the local pattern, such as `FitLinePlugin`.

## Testing Guidelines

Tests use Qt Test and are registered in `tests/CMakeLists.txt`. Add new test files as `tests/test_<feature>.cpp`, add them to `TEST_SOURCES`, and define test cases as private slots named `testSomething`. Prefer focused coverage for core logic, serialization, plugin metadata, and UI view-model behavior. Run `ctest --test-dir build --output-on-failure` before submitting changes.

## Commit & Pull Request Guidelines

History uses short prefixed commits such as `fix:`, `feat:`, `refactor:`, `chore:`, and `debug:`. Keep the prefix accurate and the subject specific; avoid leaving `WIP:` commits in final branches. Pull requests should include a concise summary, affected modules, test commands and results, linked issues when applicable, and screenshots or short recordings for visible UI changes. Call out new SDK, Qt, Halcon, or plugin deployment requirements.

## Security & Configuration Tips

Do not commit build outputs, local SDK paths, logs, API keys, or machine-specific settings. Keep generated binaries in ignored directories such as `build/`; keep user plugin state under `~/.deeplux/plugins`.
