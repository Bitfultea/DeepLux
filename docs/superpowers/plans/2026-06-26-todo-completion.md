# TODO Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to execute this checklist. Keep changes scoped to concrete TODO or incomplete behavior discovered by repository scan.

**Goal:** Replace remaining user-visible TODO/incomplete behavior with working implementations and regression coverage.

**Architecture:** Use existing Qt Test targets for UI/core behavior. For optional Hymson3D code paths that are not enabled in the default build, keep implementation static and preserve the normal build.

**Tech Stack:** C++17, Qt 5/6 compatible APIs, Qt Test, CTest, DeepLux `ImageData` metadata.

---

### Task 1: PropertyPanel Choice Params

**Files:**
- Modify: `src/ui/widgets/PropertyPanel.cpp`
- Modify: `tests/test_propertypanel.cpp`

- [x] Add a regression test for string params with sibling `_options` metadata.
- [x] Skip `_options` keys as standalone editable fields.
- [x] Render choice params as `QComboBox` and write selected values back to the module.

### Task 2: Communication Configuration and PLC Transport

**Files:**
- Modify: `src/core/communication/CommunicationManager.{h,cpp}`
- Modify: `src/ui/views/CommunicationSetView.cpp`
- Create: `tests/test_communicationmanager.cpp`
- Modify: `tests/CMakeLists.txt`

- [x] Add configuration CRUD APIs with validation.
- [x] Treat PLC as TCP/Modbus transport in the existing socket layer.
- [x] Persist Communication Settings form changes through `CommunicationManager`.
- [x] Enable TCP/PLC fields only for network types and serial fields only for serial type.
- [x] Cover config CRUD, PLC TCP transport, form apply, and type switching.

### Task 3: MainWindow TODO Entrypoints

**Files:**
- Modify: `src/ui/views/MainWindow.cpp`
- Modify: `tests/test_mainwindow.cpp`

- [x] Make recent project selection open by stable selected index, update status, log errors, and notify TerminalBridge.
- [x] Make Home clear the central display, return to FlowCanvas, clear selection, and reset process time.
- [x] Cover Home tab switching in `test_mainwindow`.

### Task 4: Hymson3D Defect Detection Input

**Files:**
- Modify: `src/plugins/hymson3d/DefectDetection/DefectDetectionPlugin.cpp`

- [x] Remove demo point-cloud generation.
- [x] Read point-cloud input from `ImageData` metadata keys `point_cloud`, `pointCloud`, or `display_data`.
- [x] Preserve input metadata and write `defect_point_cloud`, `has_defects`, and `defect_count` outputs.
- [x] Normalize defect labels without duplicating existing label arrays.

### Task 5: Verification and Reporting

**Files:**
- Modify: `tests/TEST_REPORT.md`

- [x] Run targeted regression tests.
- [x] Run full build.
- [x] Run full CTest suite.
- [x] Run `git diff --check`.
- [x] Update test report to 35 passing targets.
