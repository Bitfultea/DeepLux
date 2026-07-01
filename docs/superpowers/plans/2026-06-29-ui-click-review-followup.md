# UI Click Review Follow-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Keep the confirmed left workflow layout and polish the real clicked UI states found during screenshot review.

**Architecture:** Preserve `ProcessTabWidget` as the left-side workflow panel with `流程`, `画布`, and `数据源`. Update tests to encode that product decision, improve theme/readability through existing Qt stylesheet hooks, and extend the UI screenshot tool so future reviews capture clicked states instead of only startup states.

**Tech Stack:** C++17, Qt Widgets, Qt Test, CMake, offscreen Qt screenshots.

---

### Task 1: Align Layout Tests With Confirmed Three-Tab Workflow Panel

**Files:**
- Modify: `tests/test_mainwindow.cpp`

- [x] **Step 1: Change the layout test name and expectations**

Replace `testMainWindowLayoutKeepsInspectorAndSettingsOutOfWorkflowTabs` with a test that asserts:
- the main toolbar still contains only high-frequency actions
- `ProcessTabWidget` contains `流程`, `画布`, and `数据源`
- `ProcessTabWidget` does not contain `属性`
- `DataSourcePanel` is present inside `ProcessTabWidget`
- `LogDock` keeps a compact minimum height

- [x] **Step 2: Run the target test and confirm the old failure is gone**

Run:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/test_mainwindow -v2
```

Expected: no failure about `数据源` being in `ProcessTabWidget`.

### Task 2: Improve Dark Theme Readability and Narrow Workflow Tabs

**Files:**
- Modify: `src/ui/views/MainWindow.cpp`
- Modify: `src/ui/widgets/ViewportWidget.cpp`
- Modify: `src/ui/widgets/AgentChatPanel.cpp`

- [x] **Step 1: Add UI assertions for theme and tab fit**

Extend the main-window layout test to click `切换主题` and verify:
- the main toolbar action widgets have a non-empty dark theme stylesheet with readable foreground color
- the workflow tab bar has three tabs and no tab-scroll buttons at 1024x700 when the confirmed three tabs are visible

- [x] **Step 2: Tighten workflow tab styling**

In `MainWindow::applyTheme`, reduce `m_processTabWidget` tab padding from the current wide spacing to a compact value that fits `流程 / 画布 / 数据源` at 1024 width.

- [x] **Step 3: Make dark toolbars readable**

Ensure dark theme styles for `QToolBar QToolButton` and `ViewportWidget` toolbar buttons include explicit `color`, hover color, and selected/pressed contrast.

- [x] **Step 4: Localize Agent chat input text**

Change `Ask the Agent...  (Enter to send, Shift+Enter for new line)` to a concise Chinese placeholder that fits the bottom panel.

### Task 3: Extend Screenshot Capture to Click Through Real UI States

**Files:**
- Modify: `tests/ui_capture_mainwindow.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `scripts/capture-ui-screenshots.sh`

- [x] **Step 1: Add Qt Test dependency to screenshot target**

Link `ui_capture_mainwindow` with `Qt::Test` so it can dispatch stable mouse clicks through `QTest`.

- [x] **Step 2: Capture clicked states**

Extend `ui_capture_mainwindow` to save:
- `01-initial-1024.png`
- `02-process-canvas-tab.png`
- `03-process-datasource-tab.png`
- `04-bottom-terminal-tab.png`
- `05-bottom-agent-chat-tab.png`
- `06-bottom-agent-log-tab.png`
- `07-theme-toggle.png`
- `08-tool-panel-closed.png`
- existing static 1440 and 1024 overview screenshots

- [x] **Step 3: Update script output**

Make `scripts/capture-ui-screenshots.sh` list all generated screenshots with `find` instead of hard-coded two-file output.

### Task 4: Verify With Tests and Screenshots

**Files:**
- Modify after evidence: `tests/TEST_REPORT.md` if test counts or coverage notes change

- [x] **Step 1: Build changed targets**

Run:

```bash
cmake --build build --target test_mainwindow ui_capture_mainwindow -j4
```

Expected: build completes.

- [x] **Step 2: Run targeted test**

Run:

```bash
QT_QPA_PLATFORM=offscreen ./build/bin/test_mainwindow -v2
```

Expected: all `TestMainWindow` tests pass.

- [x] **Step 3: Generate screenshots**

Run:

```bash
bash scripts/capture-ui-screenshots.sh build /tmp/deeplux-ui-click-review-final
```

Expected: overview and clicked-state PNGs are generated.

- [x] **Step 4: Review screenshots**

Open at least:
- `/tmp/deeplux-ui-click-review-final/01-initial-1024.png`
- `/tmp/deeplux-ui-click-review-final/03-process-datasource-tab.png`
- `/tmp/deeplux-ui-click-review-final/07-theme-toggle.png`
- `/tmp/deeplux-ui-click-review-final/08-tool-panel-closed.png`

Expected: three left workflow tabs remain, dark theme text is readable, and closed-tool-panel state does not crop tabs incoherently.

- [x] **Step 5: Run final checks**

Run:

```bash
ctest --test-dir build -R "test_(mainwindow|himagewidget)" --output-on-failure
git diff --check
```

Expected: targeted tests pass and diff check reports no whitespace errors.
