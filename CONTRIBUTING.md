# Contributing

DeepLux is a C++17/Qt application. Keep changes within the owning module, follow the repository `.clang-format`, and add focused Qt Test coverage for behavior changes.

## Local Checks

```bash
cmake -S . -B build -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
git diff --check
```

Use an isolated build and plugin data directory when validating plugin-loading changes. Do not overwrite a user's `~/.deeplux/plugins` during tests.

## Pull Request Checklist

- [ ] State the problem and user-visible behavior change.
- [ ] Keep implementation, tests, fixtures, and documentation aligned.
- [ ] Record the exact tested commit, command, environment, and result in the relevant report; distinguish local tests from CI, hardware, and field acceptance.
- [ ] Update `docs/STATUS.md` in the same pull request when live delivery status or a project gate changes.
- [ ] Update `CHANGELOG.md` for user-visible changes; keep unreleased entries under `Unreleased`.
- [ ] Include UI screenshots for visible changes and list unverified platform or hardware paths.
- [ ] Check plugin ABI implications and deployment requirements.
- [ ] Ensure CI checks pass or link an explicit, reviewed exception; do not describe a skipped or non-blocking job as passed.
