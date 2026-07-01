# UI Layout Screenshot Review Implementation Plan

> **For agentic workers:** Use this plan when continuing DeepLux Qt Widgets UI layout cleanup. Keep each batch small and verify with screenshots before continuing.

**Goal:** Improve the main window layout based on real screenshots, with a repeatable build-test-capture-review loop.

---

### Task 1: Layout Regression Coverage

- [x] Add `test_mainwindow` coverage for the main toolbar, workflow tabs, right-side Inspector, and log panel minimum height.
- [x] Add `test_himagewidget` coverage for a centered, readable empty display-state prompt.
- [x] Register the new test in CTest.

### Task 2: Main Window Layout Cleanup

- [x] Keep only high-frequency project/run/theme actions in the main toolbar.
- [x] Move `PropertyPanel` and `DataSourcePanel` out of the workflow tab widget into a right-side Inspector.
- [x] Relax tool/process/Inspector minimum widths to keep the central display usable at 1024px width.
- [x] Reduce the log panel minimum height and compress log row height.
- [x] Remove emoji from bottom log/terminal/Agent tab labels.

### Task 3: Display Empty State

- [x] Replace the low-contrast empty `Zoom` overlay with a centered empty-state prompt.
- [x] Wrap empty-state text for narrow display widths.
- [x] Shorten display toolbar labels from `自适应/实际大小` to `适应/1:1`.

### Task 4: Screenshot Review Tooling

- [x] Add `ui_capture_mainwindow` as a build target.
- [x] Add `scripts/capture-ui-screenshots.sh` to generate `1440x900` and `1024x700` screenshots.
- [x] Review screenshots after each UI batch and apply visual micro-adjustments.

### Task 5: Verification

- [x] Run targeted tests after implementation batches.
- [x] Run full build.
- [x] Run full CTest suite.
- [x] Run `git diff --check`.
- [x] Update `tests/TEST_REPORT.md` with final test count and result time.
