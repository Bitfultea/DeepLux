# Plugin Foundation Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unify the baseline behavior shared by DeepLux module plugins so later algorithm, flow, and communication improvements can build on stable parameter, validation, and lifecycle contracts.

**Architecture:** Start at `ModuleBase` because most module plugins inherit it. Add contract tests before changing behavior, then introduce a focused plugin contract audit test that checks metadata, build artifacts, cloning, default parameters, validation, and configuration widget creation for representative built plugins. Fix only low-risk foundational issues in this phase.

**Tech Stack:** C++17, Qt 5/6, Qt Test, CMake, DeepLux plugin `.so` libraries.

---

### Task 1: Preserve Defaults When Loading Partial Plugin Params

**Files:**
- Modify: `tests/test_modulebase.cpp`
- Modify: `src/core/base/ModuleBase.cpp`

- [ ] **Step 1: Write the failing test**

Add a test-only module with defaults:

```cpp
class DefaultParamModule : public ModuleBase
{
    Q_OBJECT

public:
    DefaultParamModule()
    {
        m_moduleId = "com.deeplux.test.default-param-module";
        m_name = "Default Param Module";
        m_category = "test";
        m_defaultParams = QJsonObject{
            {"threshold", 3.0},
            {"iterations", 100},
            {"method", "RANSAC"},
        };
        m_params = m_defaultParams;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override
    {
        Q_UNUSED(input)
        Q_UNUSED(output)
        return true;
    }

    QWidget* createConfigWidget() override { return nullptr; }
};
```

Add `testSetParamsMergesDefaults()`:

```cpp
void TestModuleBase::testSetParamsMergesDefaults()
{
    DefaultParamModule module;

    QJsonObject partial;
    partial["threshold"] = 7.25;
    module.setParams(partial);

    const QJsonObject params = module.currentParams();
    QCOMPARE(params["threshold"].toDouble(), 7.25);
    QCOMPARE(params["iterations"].toInt(), 100);
    QCOMPARE(params["method"].toString(), QString("RANSAC"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_modulebase -j4 && ctest --test-dir build -R test_modulebase --output-on-failure`

Expected: `testSetParamsMergesDefaults` fails because `iterations` and `method` are missing after `setParams(partial)`.

- [ ] **Step 3: Write minimal implementation**

In `ModuleBase::setParams`, merge incoming values over defaults:

```cpp
QJsonObject merged = m_defaultParams;
for (auto it = params.begin(); it != params.end(); ++it) {
    merged[it.key()] = it.value();
}
```

Validate and store `merged`.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_modulebase -j4 && ctest --test-dir build -R test_modulebase --output-on-failure`

Expected: `test_modulebase` passes.

### Task 2: Apply the Same Merge Contract During Deserialization

**Files:**
- Modify: `tests/test_modulebase.cpp`
- Modify: `src/core/base/ModuleBase.cpp`

- [ ] **Step 1: Write the failing test**

Add `testFromJsonMergesDefaults()`:

```cpp
void TestModuleBase::testFromJsonMergesDefaults()
{
    DefaultParamModule module;
    QJsonObject json = module.toJson();
    json["params"] = QJsonObject{{"threshold", 8.5}};

    QVERIFY(module.fromJson(json));

    const QJsonObject params = module.currentParams();
    QCOMPARE(params["threshold"].toDouble(), 8.5);
    QCOMPARE(params["iterations"].toInt(), 100);
    QCOMPARE(params["method"].toString(), QString("RANSAC"));
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_modulebase -j4 && ctest --test-dir build -R test_modulebase --output-on-failure`

Expected: `testFromJsonMergesDefaults` fails because `fromJson` replaces the full params object.

- [ ] **Step 3: Write minimal implementation**

In `ModuleBase::fromJson`, call `setParams(json["params"].toObject())` after updating metadata fields.

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_modulebase -j4 && ctest --test-dir build -R test_modulebase --output-on-failure`

Expected: `test_modulebase` passes.

### Task 3: Add a Representative Plugin Contract Audit

**Files:**
- Create: `tests/test_plugincontracts.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing contract test**

Create `tests/test_plugincontracts.cpp` that uses `PluginManager` with a temporary plugin root, copies metadata and built `.so` files for these representative plugins, loads them, creates two instances, edits one instance param, and verifies the other instance remains unchanged:

```cpp
const QStringList representativePlugins = {
    "FitLine",
    "FitCircle",
    "MeasureLine",
    "DistancePP",
    "SystemTime",
};
```

The test must verify:

```cpp
QVERIFY(module != nullptr);
QVERIFY(!module->moduleId().isEmpty());
QVERIFY(!module->name().isEmpty());
QVERIFY(!module->category().isEmpty());
QVERIFY(module->interfaceVersion() == DEEPLUX_MODULE_INTERFACE_VERSION);
QVERIFY(module->validateParams(module->currentParams(), error));
QVERIFY(module->createConfigWidget() != nullptr || module->currentParams().isEmpty());
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build build --target test_plugincontracts -j4 && ctest --test-dir build -R test_plugincontracts --output-on-failure`

Expected: initial failures identify concrete contract gaps, most likely config widgets returning `nullptr` for param-bearing modules or clone/instance isolation issues.

- [ ] **Step 3: Fix only low-risk contract gaps**

For every failure, prefer one of these minimal fixes:

```cpp
IModule* SomePlugin::cloneImpl() const
{
    auto* clone = new SomePlugin();
    clone->setParams(currentParams());
    return clone;
}
```

or return a simple non-null config widget for parameterized plugins:

```cpp
QWidget* SomePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    auto* layout = new QVBoxLayout(widget);
    layout->addWidget(new QLabel(tr("参数可在属性面板中编辑")));
    return widget;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target test_plugincontracts -j4 && ctest --test-dir build -R test_plugincontracts --output-on-failure`

Expected: `test_plugincontracts` passes.

### Task 4: Full Regression

**Files:**
- Modify: `tests/TEST_REPORT.md`

- [ ] **Step 1: Run full build**

Run: `cmake --build build -j4`

Expected: exit code 0.

- [ ] **Step 2: Run full tests**

Run: `ctest --test-dir build --output-on-failure`

Expected: all tests pass.

- [ ] **Step 3: Run whitespace check**

Run: `git diff --check`

Expected: no output.

- [ ] **Step 4: Update test report**

Update `tests/TEST_REPORT.md` with the new test count and add bullets for `test_modulebase` and `test_plugincontracts`.
