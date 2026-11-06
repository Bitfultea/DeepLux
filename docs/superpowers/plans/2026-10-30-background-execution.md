# 后台执行线程与 UI 刷新收口实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把单次/循环/单步/断点恢复统一到专用后台执行线程，按 `execution.affinity` 分流模块，并用 50ms latest-wins 合并大图刷新，同时保持停止/取消、CLI 同步路径和插件 ABI 不变。

**Architecture:** 新增 `FlowExecutionService` 持有常驻工作线程，通过 job 租约与所有者 token 调用 `RunEngine` 的同步执行核心；`RunEngine` 在串行与并行入口按 `execution.affinity` 分流，G​​UI 模块回 GUI 线程执行。UI 只消费排队信号，并由 `ImageRefreshCoalescer` 从不可变 `DisplayFrame` 刷新大图。

**Tech Stack:** C++17、Qt 5.15/6、Qt Test、CMake、现有 OpenCV 插件体系。

**Spec:** `docs/superpowers/specs/2026-10-30-background-execution-design.md`

---

## 执行纪律

- 从当前 `main` 新建分支 `feature/background-execution`，不直接改 main。
- 每批一个独立提交；实现与测试同批提交。
- 竞态测试必须用确定性屏障（`QSemaphore`、`QWaitCondition`、`std::promise/future`、测试钩子）；`QTest::qWait`/`msleep` 仅作防挂死超时，不用于构造顺序。
- 性能验收只覆盖已审计 `worker` 的流程（至少 FindCircle）；未迁移 `gui` 长模块不纳入响应性通过范围。
- 每批报告：修改文件、行为变化、测试命令/结果、未测项。
- 改动 C++ 提交前执行 `clang-format -i <files>`；最终 `git diff --check`。

---

## 批次总览

| 批次 | 内容 | 独立提交 |
|---|---|---|
| B0 | 基线、分支、建目录 | 无代码提交 |
| B1 | `execution.affinity` 解析 + 串行/并行分流 | `feat(engine): route modules by execution affinity` |
| B2 | `ExecutionOutcome` + RAII 收尾 + 原生循环/stop 优先 | `fix(engine): finalize completed, paused and stopped runs safely` |
| B3 | job 租约 + 所有者 token + job 级取消标志 | `feat(engine): add external job lease and owned run APIs` |
| B4 | 用户暂停模块边界协议 | `feat(engine): support resumable user pause at module boundary` |
| B5 | `FlowExecutionService` | `feat(engine): add background flow execution service` |
| B6 | Agent 接入 | `feat(agent): run flows through the background execution service` |
| B7 | UI 运行/循环/单步/关闭接入 | `feat(ui): run and step flows asynchronously` |
| B8 | `DisplayFrame` + 合并器 + 视图保持 | `feat(ui): coalesce display frames and preserve viewport view` |
| B9A | 图像处理类插件审计 | `perf(plugins): run audited image plugins on the worker thread` |
| B9B | 检测/几何类插件审计 | `perf(plugins): run audited detection and geometry plugins on the worker thread` |
| B9C | 变量/逻辑/系统类插件审计 | `perf(plugins): run audited logic and variable plugins on the worker thread` |
| B10 | 全量回归、性能验收、文档收口 | `docs: record background execution acceptance` |

---

# B0：基线与分支

**Files:** 无代码修改。

- [ ] **Step 1: 记录基线**

```bash
cd /home/charles/Data/Repo/DeepLux
git rev-parse HEAD
git status --short
grep -E "CMAKE_HOME_DIRECTORY|CMAKE_BUILD_TYPE|DEEPLUX_QT_PATH" build/CMakeCache.txt
git check-ref-format --branch feature/background-execution
```

Expected：记录 HEAD；工作区除未跟踪文档外干净；`CMAKE_HOME_DIRECTORY=/home/charles/Data/Repo/DeepLux`。

- [ ] **Step 2: 建分支**

```bash
git checkout -b feature/background-execution
```

- [ ] **Step 3: 基线构建与全量测试**

```bash
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

Expected：构建通过；全量测试通过。若基线失败，停止并记录，不得带病进入 B1。

---

# B1：模块执行亲和性

**修改范围**
- 新增 `src/core/engine/ExecutionAffinity.h`：唯一的 `enum class ExecutionAffinity { Gui, Worker }`。
- `src/core/manager/PluginManager.h/.cpp`：`PluginInfo.executionAffinity`，解析 `execution.affinity`。
- `src/core/engine/RunEngine.h/.cpp`：亲和性缓存、串行分流、并行入口过滤。
- 新增 `tests/test_executionaffinity.cpp`。
- `tests/CMakeLists.txt`。

### Task B1.1：解析 `execution.affinity`

**Files:**
- Modify: `src/core/manager/PluginManager.h`
- Modify: `src/core/manager/PluginManager.cpp`
- Modify: `tests/test_pluginmanager.cpp`

- [ ] **Step 1: 写失败测试**

在 `tests/test_pluginmanager.cpp` 增加：

```cpp
void testExecutionAffinityDefaultsToGui() {
    PluginInfo info;
    QCOMPARE(info.executionAffinity, ExecutionAffinity::Gui);
}

void testExecutionAffinityParsesWorker() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pluginDir = dir.filePath(QStringLiteral("AffinityProbe"));
    QVERIFY(QDir().mkpath(pluginDir));
    QFile meta(QDir(pluginDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(meta.open(QIODevice::WriteOnly));
    meta.write(R"({
        "id":"com.deeplux.test.affinity","name":"AffinityProbe","version":"1.0.0",
        "category":"test","description":"affinity probe","author":"tests",
        "execution":{"threadSafe":false,"blocking":false,"affinity":"worker"},
        "ports":{"inputs":[],"outputs":[]}
    })");
    meta.close();

    PluginInfo info;
    QVERIFY(PluginManager::instance().loadPluginMetadata(
        QDir(pluginDir).filePath(QStringLiteral("metadata.json")), info));
    QCOMPARE(info.executionAffinity, ExecutionAffinity::Worker);
}

void testExecutionAffinityUnknownFallsBackToGui() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pluginDir = dir.filePath(QStringLiteral("AffinityUnknown"));
    QVERIFY(QDir().mkpath(pluginDir));
    QFile meta(QDir(pluginDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(meta.open(QIODevice::WriteOnly));
    meta.write(R"({
        "id":"com.deeplux.test.affinityunknown","name":"AffinityUnknown","version":"1.0.0",
        "category":"test","description":"unknown affinity probe","author":"tests",
        "execution":{"threadSafe":false,"blocking":false,"affinity":"future-mode"},
        "ports":{"inputs":[],"outputs":[]}
    })");
    meta.close();

    PluginInfo info;
    QVERIFY(PluginManager::instance().loadPluginMetadata(
        QDir(pluginDir).filePath(QStringLiteral("metadata.json")), info));
    QCOMPARE(info.executionAffinity, ExecutionAffinity::Gui);
}
```

- [ ] **Step 2: 运行测试确认失败**

```bash
cmake --build build -j4 --target test_pluginmanager
./build/bin/test_pluginmanager testExecutionAffinityDefaultsToGui
```

Expected：编译失败（`ExecutionAffinity` 未定义）。

- [ ] **Step 3: 最小实现**

`src/core/engine/ExecutionAffinity.h`（并加入 `src/core/CMakeLists.txt` 的 `CORE_HEADERS`）：

```cpp
#pragma once
namespace DeepLux {
enum class ExecutionAffinity { Gui, Worker };
}
```

`PluginManager.h` 增加 `#include "../engine/ExecutionAffinity.h"`，并修改：

```cpp
struct PluginInfo {
    // ...
    bool threadSafe = false;
    bool blocking = false;
    ExecutionAffinity executionAffinity = ExecutionAffinity::Gui;
};
```

`PluginManager.cpp` 解析：

```cpp
const QString affinity = execution.value("affinity").toString("gui").trimmed().toLower();
if (affinity == QStringLiteral("worker"))
    info.executionAffinity = ExecutionAffinity::Worker;
else {
    if (!affinity.isEmpty() && affinity != QStringLiteral("gui"))
        qWarning() << "Unknown execution.affinity, defaulting to gui:" << affinity << info.name;
    info.executionAffinity = ExecutionAffinity::Gui;
}
```

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_pluginmanager
ctest --test-dir build -R test_pluginmanager --output-on-failure
```

Expected：PASS。

### Task B1.2：RunEngine 串行分流

**Files:**
- Modify: `src/core/engine/RunEngine.h`
- Modify: `src/core/engine/RunEngine.cpp`
- Create: `tests/test_executionaffinity.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试**

`tests/test_executionaffinity.cpp`：

```cpp
#define DEEPLUX_BUILD_TESTS 1
#include "core/base/ModuleBase.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "manager/ProjectManager.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>

using namespace DeepLux;

class AffinityProbeModule : public ModuleBase {
public:
    AffinityProbeModule() {
        m_moduleId = QStringLiteral("AffinityProbe");
        m_name = QStringLiteral("AffinityProbe");
    }
    std::atomic<Qt::HANDLE> executedOn{nullptr};
    QSemaphore entered;
protected:
    bool process(const ImageData& input, ImageData& output) override {
        executedOn.store(QThread::currentThreadId());
        entered.release();
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override { return nullptr; }
};

class TestExecutionAffinity : public QObject {
    Q_OBJECT
private slots:
    void init() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
    }

    void testGuiAffinityRunsOnGuiThread() {
        AffinityProbeModule module;
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(
            QStringLiteral("AffinityProbe"), ExecutionAffinity::Gui);

        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(&context, [&]() {
            RunEngine::instance().runOnce();
        }, Qt::QueuedConnection);
        worker.start();

        QVERIFY(module.entered.tryAcquire(1, 2000));
        QTRY_VERIFY(!RunEngine::instance().isBusy());
        QCOMPARE(module.executedOn.load(), QThread::currentThreadId());
        worker.quit();
        QVERIFY(worker.wait(2000));
    }

    void testWorkerAffinityRunsOnCallingWorkerThread() {
        AffinityProbeModule module;
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(
            QStringLiteral("AffinityProbe"), ExecutionAffinity::Worker);

        QThread worker;
        QObject context;
        context.moveToThread(&worker);
        QMetaObject::invokeMethod(&context, [&]() {
            RunEngine::instance().runOnce();
        }, Qt::QueuedConnection);
        worker.start();

        QVERIFY(module.entered.tryAcquire(1, 2000));
        QTRY_VERIFY(!RunEngine::instance().isBusy());
        QVERIFY(module.executedOn.load() != QThread::currentThreadId());
        worker.quit();
        QVERIFY(worker.wait(2000));
    }

    void testParallelRejectsGuiAffinityWithoutDeadlock() {
        AffinityProbeModule module;
        RunEngine::instance().addModule(&module);
        RunEngine::instance().setModuleExecutionAffinityForTest(
            QStringLiteral("AffinityProbe"), ExecutionAffinity::Gui);

        ExecutionResult result;
        QSemaphore done;
        std::thread caller([&]() {
            result = RunEngine::instance().executeParallel({QStringLiteral("AffinityProbe")}, {});
            done.release();
        });
        QVERIFY(done.tryAcquire(1, 2000));
        caller.join();
        QCOMPARE(result.success, false);
        QVERIFY(result.errorCode == ExecError::Processing);
        QVERIFY(result.userMessage.contains(QStringLiteral("gui_affinity_not_parallelizable")));
    }
};

QTEST_MAIN(TestExecutionAffinity)
#include "test_executionaffinity.moc"
```

- [ ] **Step 2: 注册测试并确认失败**

`tests/CMakeLists.txt` 的 `TEST_SOURCES` 增加 `test_executionaffinity.cpp`。

```bash
cmake --build build -j4 --target test_executionaffinity
./build/bin/test_executionaffinity testGuiAffinityRunsOnGuiThread
```

Expected：编译失败（`setModuleExecutionAffinityForTest` 未定义）。

- [ ] **Step 3: 实现缓存与分流**

`RunEngine.h` 增加 `#include "ExecutionAffinity.h"` 和：

```cpp
DEEPLUX_TEST_VISIBILITY : void setModuleExecutionAffinityForTest(
    const QString& moduleName, ExecutionAffinity affinity);
```

`RunEngine.cpp`：

```cpp
namespace {
ExecutionAffinity affinityForModule(ModuleBase* module) {
    if (!module) return ExecutionAffinity::Gui;
    const PluginInfo info = PluginManager::instance().pluginInfo(module->moduleId());
    return info.executionAffinity;
}
}
```

在 `executeModule` 的 `module->execute(...)` 调用处：

```cpp
const ExecutionAffinity affinity = m_executionAffinity.value(moduleName, affinityForModule(module));
ExecutionResult execResult;
auto executeInline = [&]() {
    execResult = module->execute(portInputs, portOutputs, execCtx);
};
if (affinity == ExecutionAffinity::Worker || QThread::currentThread() == QCoreApplication::instance()->thread()) {
    executeInline();
} else {
    QCoreApplication* app = QCoreApplication::instance();
    if (!app || !QMetaObject::invokeMethod(app, executeInline, Qt::BlockingQueuedConnection)) {
        execResult = ExecutionResult::fail(ExecError::Processing,
                                           tr("GUI-thread module dispatch failed: %1").arg(moduleName),
                                           QStringLiteral("affinity_dispatch_failed"));
    }
}
```

`setModuleExecutionAffinityForTest` 写 `m_executionAffinity[moduleName]`。新增成员：

```cpp
QHash<QString, ExecutionAffinity> m_executionAffinity;
```

`addModule/removeModule/loadProject/clearModules` 时同步插入或移除缓存项；测试钩子优先于 PluginManager 查询。这样模块热替换不会沿用旧 affinity。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_executionaffinity
ctest --test-dir build -R test_executionaffinity --output-on-failure
```

Expected：三条测试 PASS。

### Task B1.3：并行入口过滤

**Files:**
- Modify: `src/core/engine/RunEngine.cpp`
- Modify: `src/core/engine/RunEngine.h`
- Modify: `tests/test_executionaffinity.cpp`

- [ ] **Step 1: 写失败测试**

增加：

```cpp
void testWorkerAffinityIsParallelEligible() {
    AffinityProbeModule module;
    RunEngine::instance().addModule(&module);
    RunEngine::instance().setModuleExecutionAffinityForTest(
        QStringLiteral("AffinityProbe"), ExecutionAffinity::Worker);
    QVERIFY(RunEngine::instance().isParallelEligibleForTest(QStringLiteral("AffinityProbe")));
}

void testGuiAffinityIsNotParallelEligible() {
    AffinityProbeModule module;
    RunEngine::instance().addModule(&module);
    RunEngine::instance().setModuleExecutionAffinityForTest(
        QStringLiteral("AffinityProbe"), ExecutionAffinity::Gui);
    QVERIFY(!RunEngine::instance().isParallelEligibleForTest(QStringLiteral("AffinityProbe")));
}
```

- [ ] **Step 2: 确认失败**

```bash
cmake --build build -j4 --target test_executionaffinity
```

Expected：`isParallelEligibleForTest` 未定义。

- [ ] **Step 3: 实现**

`RunEngine.h` 增加测试访问器：

```cpp
DEEPLUX_TEST_VISIBILITY : bool isParallelEligibleForTest(const QString& moduleName) const;
```

`RunEngine.cpp`：

```cpp
bool RunEngine::isParallelEligibleForTest(const QString& moduleName) const {
    ModuleBase* module = getModule(moduleName);
    return module && isParallelEligible(module);
}

bool RunEngine::isParallelEligible(ModuleBase* module) const {
    if (!module) return false;
    if (m_executionAffinity.value(runtimeModuleName(module), affinityForModule(module)) !=
        ExecutionAffinity::Worker)
        return false;
    return module->isThreadSafe() && !module->isBlocking() && !hasBreakpoint(runtimeModuleName(module));
}
```

`executeParallel` 进入池前：

```cpp
for (const QString& name : validNames) {
    ModuleBase* mod = getModule(name);
    if (affinityForModule(mod) != ExecutionAffinity::Worker) {
        return ExecutionResult::fail(ExecError::Processing,
                                     tr("gui_affinity_not_parallelizable: %1").arg(name),
                                     QStringLiteral("gui_affinity_not_parallelizable"));
    }
}
```

`executeBatchParallel` 的候选筛选改为调用 `isParallelEligible(module)`（现有条件 + affinity）。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_executionaffinity test_runengine
ctest --test-dir build -R "test_executionaffinity|test_runengine" --output-on-failure
```

Expected：PASS。若现有并行测试因 gui 默认被拒而失败，先在测试模块上设置 `Worker` affinity；不得放宽产品代码。

- [ ] **Step 5: 提交 B1**

```bash
clang-format -i src/core/manager/PluginManager.h src/core/manager/PluginManager.cpp \
  src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp tests/test_executionaffinity.cpp
git add src/core/engine/ExecutionAffinity.h \
  src/core/manager/PluginManager.h src/core/manager/PluginManager.cpp \
  src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp \
  tests/test_executionaffinity.cpp tests/test_pluginmanager.cpp \
  src/core/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(engine): route modules by execution affinity"
```

---

# B2：执行收尾结果与异常安全

**修改范围**
- `src/core/engine/RunEngine.h/.cpp`：`ExecutionOutcome`、`ExecutionFinalizer`、Completed 终态按 `RunMode`、stop 优先。
- 新增 `tests/test_runenginefinalizer.cpp`。
- `tests/CMakeLists.txt`。

### Task B2.1：Completed 终态与 stop 优先

**Files:**
- Modify: `src/core/engine/RunEngine.h`
- Modify: `src/core/engine/RunEngine.cpp`
- Create: `tests/test_runenginefinalizer.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试**

测试使用现有 `ModuleBase` 探针（可复用 `test_executionaffinity` 的 `AffinityProbeModule`，但在本文件重新定义以免跨 TU）：

```cpp
class BlockingProbe : public ModuleBase {
public:
    BlockingProbe() { m_moduleId = QStringLiteral("BlockingProbe"); m_name = QStringLiteral("BlockingProbe"); }
    QSemaphore entered;
    QSemaphore release;
protected:
    bool process(const ImageData& input, ImageData& output) override {
        entered.release();
        release.acquire();
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override { return nullptr; }
};

void testNativeCycleKeepsRunningAfterRound() {
    BlockingProbe probe;
    RunEngine::instance().addModule(&probe);
    RunEngine::instance().setParallelThreadCount(1);
    RunEngine::instance().start();
    QVERIFY(probe.entered.tryAcquire(1, 2000));
    probe.release.release();
    QTRY_VERIFY(probe.entered.tryAcquire(1, 2000));
    QCOMPARE(RunEngine::instance().state(), RunEngine::RunState::Running);
    RunEngine::instance().stop();
    probe.release.release();
    QTRY_VERIFY(!RunEngine::instance().isBusy());
}

void testStopWinsInFinalizeWindow() {
    BlockingProbe probe;
    RunEngine::instance().addModule(&probe);
    RunEngine::instance().setFinalizeHookForTest([&]() {
        RunEngine::instance().stop();
    });
    std::thread runner([]() { RunEngine::instance().runOnce(); });
    QVERIFY(probe.entered.tryAcquire(1, 2000));
    probe.release.release();
    runner.join();
    QTRY_VERIFY(!RunEngine::instance().isBusy());
    QCOMPARE(RunEngine::instance().state(), RunEngine::RunState::Stopped);
    RunEngine::instance().clearFinalizeHookForTest();
}
```

- [ ] **Step 2: 确认失败**

```bash
cmake --build build -j4 --target test_runenginefinalizer
```

Expected：`setFinalizeHookForTest` 未定义。

- [ ] **Step 3: 实现**

`RunEngine.h`：

```cpp
enum class ExecutionOutcome { Completed, Paused, Stopped, Failed };
DEEPLUX_TEST_VISIBILITY : void setFinalizeHookForTest(std::function<void()> hook);
DEEPLUX_TEST_VISIBILITY : void clearFinalizeHookForTest();
```

`RunEngine.cpp` 使用 `qScopeGuard`（Qt 5.15 可用），在成员函数内直接调用私有收尾：

```cpp
#include <QScopeGuard>

ExecutionOutcome outcome = ExecutionOutcome::Failed;
const auto finalizer = qScopeGuard([&]() { finalizeExecution(outcome); });
// ...
outcome = ExecutionOutcome::Completed; // 或 Paused / Stopped
```

`finalizeExecution`：

```cpp
void RunEngine::finalizeExecution(ExecutionOutcome outcome) {
    if (m_testFinalizeHook) m_testFinalizeHook();
    QMutexLocker locker(&m_lifecycleMutex);
    ++m_lifecycleGeneration;
    clearBreakpointPauseState();
    m_executing.store(false, std::memory_order_release);
    if (outcome == ExecutionOutcome::Paused) {
        m_state.store(static_cast<int>(RunState::Paused), std::memory_order_release);
        return;
    }
    const bool stopped = m_stopPending || state() == RunState::Stopped;
    m_stopPending = false;
    if (outcome == ExecutionOutcome::Stopped || stopped) {
        m_state.store(static_cast<int>(RunState::Stopped), std::memory_order_release);
    } else if (outcome == ExecutionOutcome::Completed && runMode() == RunMode::RunCycle) {
        m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    } else {
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    }
}
```

原 `finalizeRunTail()` 调用点改为在作用域末尾设置 outcome；Paused 路径不得调用 `finalizeRunTail()`。删除重复的旧收尾调用，确保每个执行段只收尾一次；`finalizeExecution` 内部记录 `m_finalizedGeneration` 防御重复调用。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_runenginefinalizer test_runengine
ctest --test-dir build -R "test_runenginefinalizer|test_runengine" --output-on-failure
```

Expected：PASS。

### Task B2.2：引擎层故障注入

**Files:**
- Modify: `src/core/engine/RunEngine.h/.cpp`
- Modify: `tests/test_runenginefinalizer.cpp`

- [ ] **Step 1: 写失败测试**

```cpp
void testExceptionReleasesLeaseAndAllowsNextRun() {
    AffinityProbeModule probe; // 非阻塞探针，返回即完成
    RunEngine::instance().addModule(&probe);
    RunEngine::instance().setExecutionFaultInjectionForTest([]() {
        throw std::runtime_error("injected engine fault");
    });
    RunEngine::instance().runOnce();
    QVERIFY(!RunEngine::instance().isBusy());
    RunEngine::instance().clearExecutionFaultInjectionForTest();

    QSignalSpy finished(&RunEngine::instance(), &RunEngine::runFinished);
    RunEngine::instance().runOnce();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
}
```

- [ ] **Step 2: 确认失败**

Expected：`setExecutionFaultInjectionForTest` 未定义。

- [ ] **Step 3: 实现**

在 `executeAcquiredRun` 的外层加：

```cpp
ExecutionFinalizer finalizer(this, ExecutionOutcome::Failed);
try {
    if (m_testFaultInjection) m_testFaultInjection();
    // ... 原有执行逻辑；成功/暂停/停止时 finalizer.setOutcome(...)
} catch (const std::exception& e) {
    // 记录 errorOccurred；outcome 保持 Failed
    Logger::instance().error(QString::fromUtf8(e.what()), "Run");
} catch (...) {
    Logger::instance().error(tr("Unknown engine exception"), "Run");
}
```

测试钩子只允许测试构建注入一次；不得影响生产路径。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_runenginefinalizer
ctest --test-dir build -R test_runenginefinalizer --output-on-failure
```

Expected：PASS；异常后 `isBusy()==false`。

- [ ] **Step 5: 提交 B2**

```bash
clang-format -i src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp tests/test_runenginefinalizer.cpp
git add src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp \
  tests/test_runenginefinalizer.cpp tests/CMakeLists.txt
git commit -m "fix(engine): finalize completed, paused and stopped runs safely"
```

---

# B3：Job 租约、所有者入口与取消标志

**修改范围**
- `src/core/engine/RunEngine.h/.cpp`：`JobToken`、external job 租约、`*ForJob` 入口、job 级取消标志。
- 新增 `tests/test_runenginejoblease.cpp`。
- `tests/CMakeLists.txt`。

### Task B3.1：job 租约与所有者入口

- [ ] **Step 1: 写失败测试**

```cpp
void testOwnerCanRunSingleAndStepInsideLease() {
    AffinityProbeModule probe;
    RunEngine::instance().addModule(&probe);
    RunEngine::JobToken token;
    QString reason;
    QVERIFY(RunEngine::instance().beginExternalJob(&token, &reason));
    QVERIFY(RunEngine::instance().runOnceForJob(token, &reason));
    QTRY_VERIFY(!RunEngine::instance().isExecuting());
    QVERIFY(RunEngine::instance().stepOnceForJob(token, &reason));
    QVERIFY(RunEngine::instance().endExternalJob(token));
}

void testNonOwnerEntriesRejectedDuringLease() {
    AffinityProbeModule probe;
    RunEngine::instance().addModule(&probe);
    RunEngine::JobToken token;
    QVERIFY(RunEngine::instance().beginExternalJob(&token, nullptr));
    QString reason;
    QVERIFY(!RunEngine::instance().runOnceChecked(&reason));
    QCOMPARE(reason, QStringLiteral("external_job_active"));
    QVERIFY(!RunEngine::instance().stepOnce());
    QVERIFY(!RunEngine::instance().runOnceAsync());
    QVERIFY(!RunEngine::instance().loadProject(nullptr));
    RunEngine::instance().endExternalJob(token);
}
```

- [ ] **Step 2: 确认失败**

Expected：`JobToken`/`beginExternalJob` 未定义。

- [ ] **Step 3: 实现**

`RunEngine.h`：

```cpp
struct JobToken {
    quint64 id = 0;
    quint64 salt = 0;
    bool isValid() const { return id != 0; }
    bool operator==(const JobToken& other) const { return id == other.id && salt == other.salt; }
};

bool beginExternalJob(JobToken* token, QString* reason);
bool endExternalJob(const JobToken& token);
bool runOnceForJob(const JobToken& token, QString* reason);
bool stepOnceForJob(const JobToken& token, QString* reason);
bool resumeForJob(const JobToken& token, QString* reason);
bool pauseForJob(const JobToken& token);
bool isExternalJobActiveForTest() const;
bool runOnceChecked(QString* reason);
```

实现要点：

- `beginExternalJob` 在 `m_lifecycleMutex` 内检查 `m_executing/m_maintenance/state Running/Paused`；生成 token（`QUuid` 或随机盐 + 计数器）；设置 `m_externalJobActive=true`、owner、清除 cancel flag。
- `isBusy()` 增加 `m_externalJobActive`。
- `*ForJob` 在 `tryBeginForRun` 前校验 token；不匹配返回 false + `external_job_owner_mismatch`。
- 公开 `runOnceChecked/stepOnce/runOnceAsync/start/pause/resume` 在 external job 活跃时返回/拒绝。
- `endExternalJob` 校验 token；清理 owner 与 cancel flag；返回是否成功。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_runenginejoblease
ctest --test-dir build -R test_runenginejoblease --output-on-failure
```

Expected：PASS。

### Task B3.2：job 级取消标志与窗口屏障

- [ ] **Step 1: 写失败测试**

```cpp
void testStopBetweenRoundsPreventsNextRun() {
    AffinityProbeModule probe;
    RunEngine::instance().addModule(&probe);
    RunEngine::JobToken token;
    QVERIFY(RunEngine::instance().beginExternalJob(&token, nullptr));

    // 在 Service 检查停止之后、下一轮启动之前注入 stop
    QSemaphore reached;
    RunEngine::instance().setBeforeJobStartHookForTest([&]() {
        reached.release();
        RunEngine::instance().stop();
    });
    QString reason;
    const bool started = RunEngine::instance().runOnceForJob(token, &reason);
    QVERIFY(reached.tryAcquire(1, 2000));
    QVERIFY(!started);
    QCOMPARE(reason, QStringLiteral("external_job_cancelled"));

    RunEngine::instance().clearBeforeJobStartHookForTest();
    QVERIFY(RunEngine::instance().endExternalJob(token));

    RunEngine::JobToken next;
    QVERIFY(RunEngine::instance().beginExternalJob(&next, nullptr));
    QVERIFY(RunEngine::instance().runOnceForJob(next, nullptr));
    RunEngine::instance().endExternalJob(next);
}
```

- [ ] **Step 2: 确认失败**

Expected：`setBeforeJobStartHookForTest` 未定义，或第二 token 仍被旧取消阻止。

- [ ] **Step 3: 实现**

- `m_externalJobCancelRequested` 在 `stop()` 内、`m_lifecycleMutex` 下设置（仅 external job 活跃时）。
- `*ForJob` 在锁内检查：若 cancel flag 为真，返回 false + `external_job_cancelled`；不得 reset token。
- `endExternalJob` 清理 cancel flag；`beginExternalJob` 初始化 false。
- 测试钩子 `setBeforeJobStartHookForTest` 在 `*ForJob` 的启动协议内、完成停止检查之后调用；生产为空。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_runenginejoblease
ctest --test-dir build -R test_runenginejoblease --output-on-failure
```

Expected：PASS。

- [ ] **Step 5: 提交 B3**

```bash
clang-format -i src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp tests/test_runenginejoblease.cpp
git add src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp \
  tests/test_runenginejoblease.cpp tests/CMakeLists.txt
git commit -m "feat(engine): add external job lease and owned run APIs"
```

---

# B4：用户暂停模块边界协议

**修改范围**
- `src/core/engine/RunEngine.h/.cpp`：`PauseReason`、暂停上下文、`pauseForJob`、`resumeForJob`。
- 新增 `tests/test_runengineuserpause.cpp`。
- `tests/CMakeLists.txt`。

- [ ] **Step 1: 写失败测试**

```cpp
void testPauseResumeExecutesEachDownstreamModuleOnce() {
    // A -> B -> C，三个 Probe 模块用连接/顺序构造；A 阻塞
    // 启动 runOnceForJob；A 运行中 pauseForJob；A 释放后应停在 B 前
    // QCOMPARE(state, Paused); QCOMPARE(B.count, 0)
    // resumeForJob；B/C 各执行一次，最终 Completed
}
```

使用 `BlockingProbe` 与 `QSemaphore` 构造顺序，不用 sleep。

- [ ] **Step 2: 确认失败**

Expected：`pauseForJob` 不可恢复或状态错误。

- [ ] **Step 3: 实现**

- `PauseReason { None, Breakpoint, User }`；`m_pauseReason`。
- `pauseForJob` 设置 `m_userPauseRequested`；执行循环在模块边界检查并 `setInnerPauseLocked`，设置 reason=User。
- `tryBeginForRun(Resume)` 接受 `m_pauseResumeModule` 非空且 reason ∈ {User, Breakpoint}；仅 Breakpoint 设置 `m_skipBreakpointOnce`。
- Paused 收尾保留上下文、不发 runFinished（B2 finalizer）。
- `stop()` 清除 pause request/reason/context。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_runengineuserpause test_runenginefinalizer
ctest --test-dir build -R "test_runengineuserpause|test_runenginefinalizer" --output-on-failure
```

Expected：PASS；B/C 各执行一次，暂停期间无 runFinished。

- [ ] **Step 5: 提交 B4**

```bash
clang-format -i src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp tests/test_runengineuserpause.cpp
git add src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp \
  tests/test_runengineuserpause.cpp tests/CMakeLists.txt
git commit -m "feat(engine): support resumable user pause at module boundary"
```

---

# B5：FlowExecutionService

**修改范围**
- 新增 `src/core/engine/FlowExecutionService.h/.cpp`。
- `src/core/CMakeLists.txt` 加入源/头。
- 新增 `tests/test_flowexecutionservice.cpp`。
- `tests/CMakeLists.txt`。

### Task B5.1：线程、状态与 job 租约

- [ ] **Step 1: 写失败测试**

测试文件内定义探针：

```cpp
class ProbeModule : public ModuleBase {
public:
    ProbeModule() { m_moduleId = QStringLiteral("ServiceProbe"); m_name = QStringLiteral("ServiceProbe"); }
    std::atomic<Qt::HANDLE> threadId{nullptr};
    std::atomic_int runs{0};
protected:
    bool process(const ImageData& input, ImageData& output) override {
        threadId.store(QThread::currentThreadId());
        ++runs;
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override { return nullptr; }
};

void testSingleRunsOnWorkerThreadAndFinishes() {
    ProbeModule probe; // 记录线程 id
    RunEngine::instance().addModule(&probe);
    FlowExecutionService& service = FlowExecutionService::instance();
    QSignalSpy finished(&service, &FlowExecutionService::jobFinished);
    QVERIFY(service.startSingle());
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
    QVERIFY(probe.threadId != QThread::currentThreadId());
    QCOMPARE(service.state(), FlowExecutionService::State::Idle);
}

void testStateIsVisibleAcrossThreads() {
    // 在工作线程启动 job，主线程轮询 state() 直到 Running；用 QSemaphore 屏障，不 sleep
}
```

- [ ] **Step 2: 确认失败**

Expected：`FlowExecutionService` 不存在。

- [ ] **Step 3: 实现**

`FlowExecutionService.h` 按 spec 4.1 定义；`FlowExecutionService.cpp`：

- 成员 `QThread m_thread`（自定义 `run()` 或 worker 对象）、`QMutex m_mutex`、`QWaitCondition m_wake`、`std::atomic<State> m_state`、`std::atomic<JobType> m_job`、`QQueue<Command>`。
- `startSingle/startCycle/stepOnce/continueRun`：锁内校验 Idle/Paused；设置 job；`m_wake.wakeOne()`。
- 工作循环：`beginExternalJob` → 执行 job → `endExternalJob` → `jobFinished`。
- 所有信号从工作线程发出；`state()/currentJob()` 原子读取。
- `stop()`：锁内置 cancel、engine.stop、wakeAll；不 join。
- `shutdown()`：仅 Idle 时 quit/wait；否则 stop 并返回 false。

- [ ] **Step 4: 运行测试**

```bash
cmake --build build -j4 --target test_flowexecutionservice
ctest --test-dir build -R test_flowexecutionservice --output-on-failure
```

Expected：PASS。

### Task B5.2：Single/Step/Cycle/Resume 与停止/关闭

- [ ] **Step 1: 写失败测试**

覆盖：
- Single、Step 一步、Cycle 至少两轮（用计数探针 + `QSignalSpy`，`QTRY` 等计数）。
- Cycle 轮间 `isBusy()==true`，load/clear 被拒。
- Stop 立即返回、无下一轮、最终 Idle。
- `shutdown()` 非 Idle 返回 false；Idle 返回 true。
- 异常插件：`jobFailed`/`jobFinished` 且引擎可再次运行。

- [ ] **Step 2: 确认失败并实现**

按 spec 5.1/5.2 实现命令队列与 job 循环；Cycle 使用 `runOnceForJob` 循环，轮间检查 cancel/pause。

- [ ] **Step 3: 运行测试**

```bash
cmake --build build -j4 --target test_flowexecutionservice
ctest --test-dir build -R test_flowexecutionservice --output-on-failure
```

Expected：PASS。

- [ ] **Step 4: 提交 B5**

```bash
clang-format -i src/core/engine/FlowExecutionService.h src/core/engine/FlowExecutionService.cpp \
  tests/test_flowexecutionservice.cpp
git add src/core/engine/FlowExecutionService.h src/core/engine/FlowExecutionService.cpp \
  src/core/CMakeLists.txt tests/test_flowexecutionservice.cpp tests/CMakeLists.txt
git commit -m "feat(engine): add background flow execution service"
```

---

# B6：Agent 接入

**修改范围**
- `src/core/agent/AgentActor.cpp/.h`、`AgentRunWaiter.cpp/.h`（如需要）。
- `tests/test_agentactor.cpp`、`tests/test_agentclosedloop.cpp`。

- [ ] **Step 1: 写失败测试**

- `run_flow once wait=true`：执行在非 GUI 线程；完成/失败/超时/cancel。
- `run_flow once wait=false`：立即 started，随后完成。
- `run_flow mode=cycle`：立即返回 started 且 note 含 “cycle mode ignores wait”；停止后无新轮。
- `pause_flow/resume_flow`：A→B→C 暂停恢复。
- `stop_flow`：Service job 活跃时走 Service stop。

- [ ] **Step 2: 实现**

`AgentActor::runFlow`：
```cpp
if (mode == "cycle") {
    const bool started = FlowExecutionService::instance().startCycle();
    return started ? QJsonObject{{"status","started"},{"mode","cycle"},{"async",true},
                                 {"note","cycle mode ignores wait"}}
                   : QJsonObject{{"success",false},{"code","already_running"},{"error","Flow is already running"}};
}
if (!wait) return startSingle() ? started-result : already_running;
return runAndWait(...); // start callback -> FlowExecutionService::startSingle()
```
`runAndWait` 的超时/中止调用 `FlowExecutionService::instance().stop()`。
`stopFlow` 优先 Service::stop；`pauseFlow/resumeFlow` 调用 Service。

- [ ] **Step 3: 运行测试**

```bash
cmake --build build -j4 --target test_agentactor test_agentclosedloop
ctest --test-dir build -R "test_agentactor|test_agentclosedloop" --output-on-failure
```

Expected：PASS；Cycle 兼容语义不变。

- [ ] **Step 4: 提交 B6**

```bash
clang-format -i src/core/agent/AgentActor.cpp src/core/agent/AgentActor.h \
  tests/test_agentactor.cpp tests/test_agentclosedloop.cpp
git add src/core/agent/AgentActor.cpp src/core/agent/AgentActor.h \
  tests/test_agentactor.cpp tests/test_agentclosedloop.cpp
git commit -m "feat(agent): run flows through the background execution service"
```

---

# B7：UI 异步接入

**修改范围**
- `src/ui/views/MainWindow.cpp/.h`：运行/循环/单步/停止/继续/关闭。
- `tests/test_mainwindow.cpp`、`tests/ui_capture_mainwindow.cpp`（如需要）。

- [ ] **Step 1: 写失败测试**

- 点击运行后按钮状态立即更新，GUI 定时器在长模块执行期间仍触发。
- 单步异步：一次点击只执行一个模块。
- Cycle 停止后无新轮，终态正确。
- Service 非 Idle 时 `closeEvent` 被 ignore；`jobFinished` 后再关闭。
- 断点继续按钮走 Service。

用确定性屏障：模块进入后释放 `QSemaphore`，主线程启动 16ms 定时器并计数；不使用 sleep 构造顺序。

- [ ] **Step 2: 实现**

- `onRunOnce/onRunCycle/onStepRun/onStop` 改调 Service。
- 删除 `runFinished → singleShot(1, executeFlowOnce)`。
- `closeEvent` 按 Service 状态 stop + ignore；`jobFinished` 后再 close。
- `runFinished` 只更新标签/叠加；Cycle 轮结束不重启。

- [ ] **Step 3: 运行测试**

```bash
cmake --build build -j4 --target test_mainwindow ui_capture_mainwindow
ctest --test-dir build -R "test_mainwindow|ui_capture_mainwindow" --output-on-failure
```

Expected：PASS；截图门禁通过。

- [ ] **Step 4: 提交 B7**

```bash
clang-format -i src/ui/views/MainWindow.cpp src/ui/views/MainWindow.h tests/test_mainwindow.cpp
git add src/ui/views/MainWindow.cpp src/ui/views/MainWindow.h tests/test_mainwindow.cpp
git commit -m "feat(ui): run and step flows asynchronously"
```

---

# B8：DisplayFrame、合并器与视图保持

**修改范围**
- 新增 `src/core/engine/DisplayFrame.h`（或 `src/core/display/DisplayFrame.h`）。
- `RunEngine.h/.cpp`：显示帧捕获、`jobId` 注入、`moduleDataSnapshot`。
- 新增 `src/ui/display/ImageRefreshCoalescer.h/.cpp`。
- `src/ui/display/DisplayManager.*`、`ViewportWidget.*`、`HImageWidget.*`、`MainWindow.cpp`。
- 新增 `tests/test_imagerefreshcoalescer.cpp`；修改 `test_mainwindow`、`ui_capture_mainwindow`。

### Task B8.1：DisplayFrame 快照

- [ ] **Step 1: 写失败测试**

- `setDisplayJobId` 后发布的帧 `jobId/runId/frameId/moduleId` 正确。
- `moduleDataSnapshot` 与同轮输出一致；下一轮开始后旧帧仍是旧数据。
- GUI 延迟消费 A 帧、worker 已 B 轮：A 帧快照不包含 B 数据。
- 旧 job 帧被 coalescer 丢弃。

- [ ] **Step 2: 实现**

`DisplayFrame` 按 spec 7.1 定义；`RunEngine` 在模块成功后生成图像帧，`runFinished` 前生成带 `moduleDataSnapshot` 的最终帧；`setDisplayJobId(quint64)`/`clearDisplayJobId()`。深拷贝 QImage；`moduleDataSnapshot` 复制 `allData()`，不含大图。

### Task B8.2：合并器

- [ ] **Step 1: 写失败测试**

```cpp
static DisplayFrame frameWith(quint64 id) {
    DisplayFrame frame;
    frame.jobId = 1;
    frame.runId = QStringLiteral("run-1");
    frame.frameId = id;
    frame.moduleId = QStringLiteral("probe");
    frame.viewportId = QStringLiteral("main");
    frame.image = QImage(4, 4, QImage::Format_RGB32);
    return frame;
}

void testLatestWinsWithinThrottleWindow() {
    QSemaphore rendered;
    int renders = 0;
    quint64 lastRenderedFrameId = 0;
    ImageRefreshCoalescer coalescer;
    coalescer.setRenderCallback([&](const DisplayFrame& frame) {
        ++renders;
        lastRenderedFrameId = frame.frameId;
        rendered.release();
    });
    for (quint64 i = 1; i <= 100; ++i)
        coalescer.schedule(frameWith(i));
    QVERIFY(rendered.tryAcquire(1, 500));
    QCOMPARE(renders, 1);
    QCOMPARE(lastRenderedFrameId, 100);
}
```

另测：
- 连续通知不重置时点：5ms 间隔持续 500ms，渲染次数约 10（允许 ±2），不要求精确。
- `forceFlush()` 渲染最新；`(jobId,runId)` 不匹配的帧丢弃。
- 新 job 到来清空旧 pending。

- [ ] **Step 2: 实现**

`ImageRefreshCoalescer`：每视口 `PendingFrame`，50ms 定时器，`lastRender` 时间；`schedule` 只在距上次渲染 <50ms 时更新槽位；到点渲染最新；`forceFlush` 立即渲染。

### Task B8.3：视图保持与叠加快照

- [ ] **Step 1: 写失败测试**

- 后台刷新 `preserveView=true`：zoom/offset 不变。
- 首次显示/resize/显式 fit：仍 fit。
- `updateMeasurementResultOnOverlay` 只读 `DisplayFrame.moduleDataSnapshot`；不调用 `RunEngine::moduleOutput`（可用测试注入/断言）。
- 删除 `moduleFinished → displayImage` 路径。

- [ ] **Step 2: 实现**

`HImageWidget::setImage(const QImage&, bool preserveView)`；`ViewportWidget/DisplayManager` 传递参数；MainWindow 只由 coalescer 调用 `displayImage`，并消费帧内快照。

- [ ] **Step 3: 运行测试**

```bash
cmake --build build -j4 --target test_imagerefreshcoalescer test_mainwindow ui_capture_mainwindow
ctest --test-dir build -R "test_imagerefreshcoalescer|test_mainwindow|ui_capture_mainwindow" --output-on-failure
```

Expected：PASS；截图门禁通过；连续通知测试符合频率上限。

- [ ] **Step 4: 提交 B8**

```bash
clang-format -i src/core/engine/DisplayFrame.h src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp \
  src/ui/display/ImageRefreshCoalescer.h src/ui/display/ImageRefreshCoalescer.cpp \
  src/ui/display/DisplayManager.cpp src/ui/display/DisplayManager.h \
  src/ui/widgets/ViewportWidget.cpp src/ui/widgets/ViewportWidget.h \
  src/ui/widgets/HImageWidget.cpp src/ui/widgets/HImageWidget.h \
  src/ui/views/MainWindow.cpp tests/test_imagerefreshcoalescer.cpp tests/test_mainwindow.cpp
git add src/core/engine/DisplayFrame.h src/core/engine/RunEngine.h src/core/engine/RunEngine.cpp \
  src/ui/display/ImageRefreshCoalescer.h src/ui/display/ImageRefreshCoalescer.cpp \
  src/ui/display/DisplayManager.cpp src/ui/display/DisplayManager.h \
  src/ui/widgets/ViewportWidget.cpp src/ui/widgets/ViewportWidget.h \
  src/ui/widgets/HImageWidget.cpp src/ui/widgets/HImageWidget.h \
  src/ui/views/MainWindow.cpp src/ui/views/MainWindow.h \
  tests/test_imagerefreshcoalescer.cpp tests/test_mainwindow.cpp \
  src/core/CMakeLists.txt src/ui/CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(ui): coalesce display frames and preserve viewport view"
```

---

# B9：插件审计与 worker 标记

**原则**
- 每批先审计、再改 metadata、再跑一致性冒烟测试。
- 不按目录批量放行；不通过审计的保持 `gui`。
- 每批独立提交和验收。

**通用审计步骤（每批重复）**

- [ ] 检查 `process()` 是否含：`QTimer`、`QEventLoop`、`QTcpSocket`、`QSerialPort`、`QProcess`、`QCoreApplication::processEvents`、`QWidget/QPixmap/QPainter/QOpenGL`、CameraManager。
- [ ] 检查是否有未加锁全局可变状态。
- [ ] 两个实例在 worker 线程执行，输出与 GUI 线程执行一致。
- [ ] 只有全部通过才在 `metadata.json` 加 `"affinity":"worker"`。

### B9A：图像处理类

**候选（逐个审计，不预设全部通过）**：`Blob`、`CropImage`、`PerProcessing`、`JigsawPuzzle`、`SaveImage`、`LoadPointCloud`、`PreProcessing3D`。

**排除**：`GrabImage`、`DisplayData`、`ShowImage`、`ImageScript`（事件循环/脚本状态）。

- [ ] **Step 1: 为每个候选增加一致性测试**

在 `tests/test_pluginparametercontracts.cpp` 或新建 `tests/test_workerpluginconsistency.cpp`：构造输入图像，分别在 GUI 线程与 `std::thread` 调用同一个插件实例的 `execute`，比较输出端口与关键结果。

- [ ] **Step 2: 运行**

```bash
cmake --build build -j4 --target test_workerpluginconsistency
ctest --test-dir build -R test_workerpluginconsistency --output-on-failure
```

- [ ] **Step 3: 改 metadata**

仅对通过者加：

```json
"execution": { "affinity": "worker" }
```

- [ ] **Step 4: 回归**

```bash
cmake --build build -j4
ctest --test-dir build -R "test_(blob|crop|perprocessing|jigsaw|saveimage|loadpointcloud|preprocessing3d)" --output-on-failure
```

- [ ] **Step 5: 提交 B9A**

```bash
git add src/plugins/image_processing/*/metadata.json tests/test_workerpluginconsistency.cpp tests/CMakeLists.txt
git commit -m "perf(plugins): run audited image plugins on the worker thread"
```

### B9B：检测/几何类

**候选**：`FindCircle`、`MeasureCircle`、`MeasureRect`、`MeasureLine`、`MeasureGap`、`FitCircle`、`FitLine`、`FitEllipse`、`FitPlane`、`DistancePL`、`DistancePP`、`LinesDistance`、`PointSurfaceDistance`、`ColorRecognition`、`QRCode`、`RegionStats`、`DefectDetection`、`EdgeDefectDetection`、`JiErHanDefectsDet`、`NPointCalibration`、`GapMeasure3D`、`FreeformSurface`。

- [ ] **Step 1: 静态审计**：逐插件检查 `process()` 是否含 `QTimer/QEventLoop/QTcpSocket/QSerialPort/QProcess/QCoreApplication::processEvents/QWidget/QPixmap/QPainter/QOpenGL/CameraManager`，以及未加锁全局状态。
- [ ] **Step 2: 一致性测试**：在 `test_workerpluginconsistency.cpp` 为每个候选添加 GUI/worker 两线程输出一致性用例。
- [ ] **Step 3: 运行**：`cmake --build build -j4 --target test_workerpluginconsistency && ctest --test-dir build -R test_workerpluginconsistency --output-on-failure`。
- [ ] **Step 4: 标记**：仅通过者写 `"execution": { "affinity": "worker" }`。
- [ ] **Step 5: 回归**：`cmake --build build -j4 && ctest --test-dir build -R "test_(findcircle|fitcircle|measurecircle|measurerect|measureline|measuregap|fitline|fitellipse|fitplane|distancepl|distancepp|linesdistance|pointsurfacedistance|colorrecognition|qrcode|regionstats)" --output-on-failure`。
- [ ] **Step 6: 提交** `perf(plugins): run audited detection and geometry plugins on the worker thread`。
- [ ] `FindCircle` 必须优先审计；未通过则保持 `gui` 并记录原因。

### B9C：变量/逻辑/系统类

**候选**：`CreateString`、`SplitString`、`StrFormatPlugin`、`VarDefinePlugin`、`VarSetPlugin`、`MathPlugin`、`Condition`、`If`、`Loop`、`While`、`StopWhile`、`TimeSlice`、`DataCheck`、`SystemTime`、`WriteText`、`Folder`、`SaveData`。

**排除**：`Delay`（processEvents）、`Parallel`（线程池/共享状态，需单独审计）、`QueueIn/QueueOut`（共享队列，需单独审计）、`TableOutPut`（UI/表状态）。

- [ ] **Step 1: 静态审计**：逐插件检查 `process()` 是否含 `QTimer/QEventLoop/QTcpSocket/QSerialPort/QProcess/QCoreApplication::processEvents/QWidget/QPixmap/QPainter/QOpenGL/CameraManager`，以及未加锁全局状态。
- [ ] **Step 2: 一致性测试**：在 `test_workerpluginconsistency.cpp` 为每个候选用例添加 GUI/worker 输出一致性断言。
- [ ] **Step 3: 运行**：`cmake --build build -j4 --target test_workerpluginconsistency && ctest --test-dir build -R test_workerpluginconsistency --output-on-failure`。
- [ ] **Step 4: 标记**：仅通过者写 `"execution": { "affinity": "worker" }`。
- [ ] **Step 5: 回归**：`cmake --build build -j4 && ctest --test-dir build -R "test_(expressionengine|timeslice|variable|system|logic)" --output-on-failure`。
- [ ] **Step 6: 提交** `perf(plugins): run audited logic and variable plugins on the worker thread`。

---

# B10：性能验收、全量回归与文档

**性能验收仅覆盖已审计 worker 流程**
- 至少 `FindCircle` 流程；不得用未迁移 `gui` 长模块宣称响应性。
- 用确定性屏障：模块进入后触发 `QSemaphore`；主线程启动 16ms `QTimer`，在 worker 执行期间计数；断言 tick 数 > 0 且停止按钮槽可执行；不使用 sleep 构造顺序。

- [ ] **Step 1: 写性能/响应性测试**

在 `tests/test_mainwindow.cpp` 增加 `testBackgroundExecutionKeepsUiResponsive`：加载已审计 FindCircle 验收工程，点击运行，等待模块进入屏障，16ms 定时器计数，等待完成；断言 tick 数、最终结果与 `isBusy()==false`。

- [ ] **Step 2: 运行定向测试**

```bash
cmake --build build -j4
ctest --test-dir build -R "test_mainwindow|test_agentactor|test_flowexecutionservice|test_runengine|test_imagerefreshcoalescer|test_workerpluginconsistency" --output-on-failure
```

- [ ] **Step 3: 隔离全量**

```bash
TEST_HOME=$(mktemp -d /tmp/deeplux-bg-exec.XXXXXX)
env HOME="$TEST_HOME" bash scripts/sync-plugins.sh build
env -u DEEPLUX_APP_DATA_DIR HOME="$TEST_HOME" QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build --output-on-failure -j4
git diff --check
git status --short
```

Expected：全量通过；实际通过数写入报告，不硬编码历史数字。

- [ ] **Step 4: 文档**

更新：
- `docs/architecture.md`（执行线程/亲和性/刷新链路）。
- `docs/plugins.md`（`execution.affinity` 语义与审计状态）。
- 新增 `docs/reports/background-execution-acceptance.md`：记录各批提交、测试命令/结果、已审计 worker 模块清单、未验证项（Windows、真实相机/通讯、TSan、长时稳定性）。

- [ ] **Step 5: 提交 B10**

```bash
git add docs/architecture.md docs/plugins.md docs/reports/background-execution-acceptance.md tests/test_mainwindow.cpp
git commit -m "docs: record background execution acceptance"
```

---

## 最终验收清单

- [ ] `RunEngine::runOnce/stepOnce/start` 同步语义与 CLI 回归通过。
- [ ] 单次/循环/单步/断点恢复均通过 `FlowExecutionService` 执行，UI 不阻塞。
- [ ] `execution.affinity` 串行与并行入口全部遵守；无 worker→QWidget。
- [ ] job 租约覆盖整个 job；Cycle 轮间工程修改被拒；所有者/非所有者入口符合规则。
- [ ] 用户暂停 A→B→C 语义通过；Paused 不发 runFinished。
- [ ] 异常注入后租约释放、状态回落、后续可运行。
- [ ] 原生 `start()` 循环至少两轮；收尾窗口 stop 获胜。
- [ ] 图像 50ms 频率上限 + latest-wins；测量叠加只用同帧快照；旧 job 不污染新工程。
- [ ] 性能验收只覆盖已审计 worker 流程（至少 FindCircle）。
- [ ] 全量 CTest、截图门禁、`git diff --check` 通过。
- [ ] Windows/真实相机/通讯/TSan 未验证项已记录，未写成通过。
