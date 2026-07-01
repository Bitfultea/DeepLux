# Plugin Optimization Phases 2-4 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Continue after plugin foundation Phase 1 by improving high-value algorithm plugins, flow/variable/system plugins, and broad plugin audit coverage in controlled TDD batches.

**Architecture:** Use existing Qt Test targets for plugin-specific behavior where they already exist, and add focused contract tests for cross-plugin expectations. Keep changes scoped to one plugin family at a time; do not change `IModule` ABI in these phases.

**Tech Stack:** C++17, Qt Test, CMake, DeepLux `ModuleBase`, runtime plugin `.so` libraries.

---

### Task 1: Algorithm Plugin Parameter Validation

**Files:**
- Modify: `tests/test_fitline.cpp`
- Modify: `tests/test_fitcircle.cpp`
- Modify: `tests/test_measureline.cpp`
- Modify: `src/plugins/geometry/FitLine/FitLinePlugin.cpp`
- Modify: `src/plugins/geometry/FitCircle/FitCirclePlugin.cpp`
- Modify: `src/plugins/detection/MeasureLine/MeasureLinePlugin.cpp`

- [x] **Step 1: Write failing tests**

Add tests that invalid parameters are rejected:

```cpp
QString error;
QJsonObject params = plugin.defaultParams();
params["threshold"] = -1.0;
QVERIFY(!plugin.validateParams(params, error));
QVERIFY(!error.isEmpty());
```

For `FitLine`, also reject unknown `fitMethod`.
For `FitCircle`, reject `minRadius > maxRadius`, non-positive `iterations`, and non-positive `threshold`.
For `MeasureLine`, reject `minLength > maxLength`, `minAngle > maxAngle`, and non-positive `threshold`.

- [x] **Step 2: Verify red**

Run: `cmake --build build --target test_fitline test_fitcircle test_measureline -j4 && ctest --test-dir build -R "test_(fitline|fitcircle|measureline)" --output-on-failure`

Expected: new validation tests fail because current implementations accept invalid params.

- [x] **Step 3: Implement minimal validation**

Update `doValidateParams` in each plugin to check only the invalid cases listed above and return actionable Chinese error strings.

- [x] **Step 4: Verify green**

Run the same command. Expected: all three targets pass.

### Task 2: Algorithm Plugin Input Error Robustness

**Files:**
- Modify: `tests/test_distancepp.cpp`
- Modify: `tests/test_distancepl.cpp`
- Modify: `src/plugins/geometry/DistancePP/DistancePPPlugin.cpp`
- Modify: `src/plugins/geometry/DistancePL/DistancePLPlugin.cpp`

- [x] **Step 1: Write failing tests**

Add tests where points/lines are present but malformed:

```cpp
ImageData input, output;
input.setData("point1", QVariantList{1.0});
input.setData("point2", QPointF(2.0, 3.0));
QVERIFY(!plugin.execute(input, output));
```

For `DistancePL`, include a malformed line with fewer than two points.

- [x] **Step 2: Verify red**

Run: `cmake --build build --target test_distancepp test_distancepl -j4 && ctest --test-dir build -R "test_(distancepp|distancepl)" --output-on-failure`

Expected: malformed values are accepted or converted to zeros.

- [x] **Step 3: Implement minimal parsing guards**

Reject variant lists that do not contain required numeric coordinates or line endpoints. Emit existing-style Chinese error messages and return `false`.

- [x] **Step 4: Verify green**

Run the same command. Expected: both targets pass.

### Task 3: Flow, Variable, and System Plugin Behavior Tests

**Files:**
- Create: `tests/test_variable_system_plugins.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `src/plugins/variable/MathPlugin/MathPlugin.cpp`
- Modify: `src/plugins/variable/SplitString/SplitStringPlugin.cpp`
- Modify: `src/plugins/system/SystemTime/SystemTimePlugin.cpp`

- [x] **Step 1: Write failing tests**

Add tests for:

```cpp
// Math: division by zero returns false and emits an error.
// SplitString: missing delimiter is rejected by validateParams.
// SystemTime: empty timeFormat is rejected by validateParams.
```

- [x] **Step 2: Verify red**

Run: `cmake --build build --target test_variable_system_plugins -j4 && ctest --test-dir build -R test_variable_system_plugins --output-on-failure`

Expected: validation or execution tests fail on current behavior.

- [x] **Step 3: Implement minimal behavior**

Add validation and explicit error paths without changing plugin public API.

- [x] **Step 4: Verify green**

Run the same command. Expected: `test_variable_system_plugins` passes.

### Task 4: Expand Plugin Contract Audit

**Files:**
- Modify: `tests/test_plugincontracts.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: plugins that fail clone parameter preservation.

- [x] **Step 1: Expand representative plugin list**

Add one buildable plugin from each family:

```cpp
"Blob", "PerProcessing", "Matching", "MeasureRect", "Folder", "SaveData", "CreateString", "Math", "SplitString"
```

- [x] **Step 2: Verify red**

Run: `cmake --build build --target test_plugincontracts -j4 && ctest --test-dir build -R test_plugincontracts --output-on-failure`

Expected: failures show concrete clone/metadata/config gaps.

- [x] **Step 3: Fix only low-risk contract gaps**

For param-bearing plugins with `cloneImpl`, copy `currentParams()` into the clone. For missing non-null config widgets on param-bearing plugins, add a minimal widget.

- [x] **Step 4: Verify green**

Run the same command. Expected: `test_plugincontracts` passes.

### Task 5: Full Regression and Reporting

**Files:**
- Modify: `tests/TEST_REPORT.md`

- [x] **Step 1: Build all**

Run: `cmake --build build -j4`

Expected: exit code 0.

- [x] **Step 2: Run all tests**

Run: `ctest --test-dir build --output-on-failure`

Expected: all tests pass.

- [x] **Step 3: Check whitespace**

Run: `git diff --check`

Expected: no output.

- [x] **Step 4: Update report**

Update test count, add coverage notes for algorithm validation, variable/system plugin behavior, and expanded plugin contracts.
