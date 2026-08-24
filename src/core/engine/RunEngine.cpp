#include "RunEngine.h"

#include "base/ModuleBase.h"
#include "common/CancellationToken.h"
#include "common/Logger.h"
#include "interface/IModule.h"
#include "manager/PluginManager.h"
#include "model/Project.h"

#include <QDebug>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <algorithm>
#include <limits>

namespace DeepLux {

namespace {
QString runtimeModuleName(ModuleBase* module) {
    if (!module) {
        return QString();
    }
    return module->instanceName().isEmpty() ? module->name() : module->instanceName();
}

const PortSpec* findPort(const QList<PortSpec>& ports, const QString& id) {
    for (const PortSpec& port : ports) {
        if (port.id == id)
            return &port;
    }
    return nullptr;
}

bool portTypesCompatible(DataType outputType, DataType inputType) {
    return outputType == DataType::Any || inputType == DataType::Any || outputType == inputType;
}

QString connectionKey(const ModuleConnection& connection) {
    return connection.fromModuleId + QLatin1Char('\x1f') + connection.toModuleId + QLatin1Char('\x1f') +
           connection.fromPort + QLatin1Char('\x1f') + connection.toPort;
}
} // namespace

RunEngine& RunEngine::instance() {
    static RunEngine instance;
    return instance;
}

RunEngine::RunEngine()
    : QObject(nullptr), m_cycleTimer(new QTimer(this)), m_cancellationToken(new CancellationToken(this)) {
    m_cycleTimer->setInterval(100);
    connect(m_cycleTimer, &QTimer::timeout, this, &RunEngine::onTimerTick);
    setParallelThreadCount(0);
    Logger::instance().info("Run engine initialized", "Run");
}

RunEngine::~RunEngine() {
    stop();
    m_parallelPool.waitForDone();
    clearModuleTree();
    clearModules();
}

void RunEngine::setCycleMode(bool enabled) {
    m_runMode.store(static_cast<int>(enabled ? RunMode::RunCycle : RunMode::None), std::memory_order_release);
}

void RunEngine::setParallelThreadCount(int n) {
    // 默认上限 max(1, cores-1)
    const int cap = qMax(1, QThread::idealThreadCount() - 1);
    int threads = (n <= 0) ? cap : qBound(1, n, qMax(1, QThread::idealThreadCount()));
    QMutexLocker locker(&m_parallelMutex);
    m_parallelThreads = threads;
    m_parallelPool.setMaxThreadCount(threads);
}

int RunEngine::parallelThreadCount() const {
    QMutexLocker locker(&m_parallelMutex);
    return m_parallelThreads;
}

ExecutionResult RunEngine::executeParallel(const QStringList& names, const PortValueMap& sharedInput) {
    if (m_cancellationToken)
        m_cancellationToken->reset();

    std::atomic<int> running{0};
    std::atomic<int> maxConcurrent{0};
    QMutex errMutex;
    bool haveError = false;
    ExecutionResult firstError;
    QMutex doneMutex;
    QWaitCondition doneCond;
    int finished = 0;

    QStringList validNames;
    for (const QString& name : names)
        if (getModule(name))
            validNames.append(name);

    for (const QString& name : validNames) {
        ModuleBase* mod = getModule(name);
        // 同一实例同刻仅一次：每个任务持有独立输入/输出副本
        m_parallelPool.start([this, mod, name, &sharedInput, &running, &maxConcurrent, &errMutex, &haveError,
                              &firstError, &doneMutex, &doneCond, &finished]() {
            const int cur = ++running;
            int expected = maxConcurrent.load();
            while (cur > expected && !maxConcurrent.compare_exchange_weak(expected, cur)) {
            }

            PortValueMap in = sharedInput;
            PortValueMap out;
            ExecutionContext ctx;
            ctx.runId = m_runId;
            ctx.frameId = m_frameId.load(std::memory_order_relaxed);
            ctx.cancellationToken = m_cancellationToken;
            const ExecutionResult r = mod->execute(in, out, ctx);
            --running;

            if (!r.success) {
                QMutexLocker locker(&errMutex);
                if (!haveError) {
                    haveError = true;
                    firstError = r;
                }
                if (m_cancellationToken)
                    m_cancellationToken->cancel(); // 取消同组剩余任务
            }

            QMutexLocker doneLocker(&doneMutex);
            ++finished;
            doneCond.wakeAll();
        });
    }

    {
        QMutexLocker locker(&doneMutex);
        while (finished < validNames.size())
            doneCond.wait(&doneMutex);
    }

    m_lastParallelMaxConcurrency.store(maxConcurrent.load(), std::memory_order_release);

    if (haveError)
        return firstError;
    return ExecutionResult::ok();
}

void RunEngine::runOnce() {
    if (isBusy()) {
        return;
    }

    resetStepState();
    m_runMode.store(static_cast<int>(RunMode::RunOnce), std::memory_order_release);
    Logger::instance().info(tr("Starting single run"), "Run");
    executeRun();
}

bool RunEngine::stepOnce() {
    if (isBusy()) {
        return false;
    }

    const QStringList validationErrors = validateFlow();
    if (!validationErrors.isEmpty()) {
        emit errorOccurred(validationErrors.join(QStringLiteral("\n")));
        return false;
    }

    {
        QReadLocker locker(&m_moduleLock);
        if (m_modules.isEmpty()) {
            RunResult result;
            result.success = false;
            result.errorCode = -1;
            result.errorMessage = tr("No modules to run");
            result.elapsedMs = 0;
            result.finishedTime = QDateTime::currentDateTime();
            emit runFinished(result);
            return false;
        }
    }

    if (m_stepCurrentModuleName.isEmpty()) {
        clearModuleOutputs();
        m_nodeOutputs.clear();
        buildModuleTree();
        if (!m_controlEdges.isEmpty()) {
            initializeControlQueue();
            m_stepCurrentModuleName = m_controlQueue.value(0);
        } else {
            QReadLocker locker(&m_moduleLock);
            m_stepCurrentModuleName =
                m_executionOrder.isEmpty() ? runtimeModuleName(m_modules.first()) : m_executionOrder.first();
        }
        m_stepPipelineData = ImageData();
    }

    if (m_stepCurrentModuleName.isEmpty()) {
        resetStepState();
        return false;
    }

    m_executing.store(true, std::memory_order_release);
    m_runMode.store(static_cast<int>(RunMode::RunOnce), std::memory_order_release);
    m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    emit stateChanged(state());
    emit runStarted();

    m_runStartTime = QDateTime::currentDateTime();
    if (m_cancellationToken) {
        m_cancellationToken->reset();
    }

    const QString moduleToRun = m_stepCurrentModuleName;
    const bool usesControlGraph = !m_controlEdges.isEmpty();
    if (usesControlGraph && !m_controlQueue.isEmpty()) {
        m_controlQueue.takeFirst();
        if (m_reentrantModules.contains(moduleToRun))
            m_controlActivated.remove(moduleToRun);
    }
    m_currentModuleName = moduleToRun;
    // 阶段 B 复核: 单步与完整运行共用"执行或禁用旁路"入口
    executeOrBypassModule(moduleToRun, m_stepPipelineData);

    const bool success = m_lastExecuteResult;
    if (success) {
        if (usesControlGraph) {
            m_controlProcessed.insert(moduleToRun);
            activateControlSuccessors(moduleToRun);
            m_stepCurrentModuleName = m_controlQueue.value(0);
        } else {
            m_stepCurrentModuleName = getNextModule(moduleToRun, m_lastControlResult);
        }
        m_currentModuleName = m_stepCurrentModuleName;
        if (m_stepCurrentModuleName.isEmpty()) {
            if (usesControlGraph) {
                emitInactiveControlModules();
            }
            resetStepState();
        }
    } else {
        resetStepState();
    }

    const int elapsedMs = m_runStartTime.msecsTo(QDateTime::currentDateTime());
    updateStatistics(success, elapsedMs);

    RunResult result;
    result.success = success;
    result.errorCode = success ? 0 : -1;
    result.errorMessage =
        success ? QString()
                : (m_lastModuleError.isEmpty() ? tr("Step execution failed: %1").arg(moduleToRun) : m_lastModuleError);
    result.elapsedMs = elapsedMs;
    result.finishedTime = QDateTime::currentDateTime();
    m_executing.store(false, std::memory_order_release);
    emit runFinished(result);

    m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    emit stateChanged(state());
    return true;
}

void RunEngine::resetStepState() {
    m_stepCurrentModuleName.clear();
    m_stepPipelineData = ImageData();
    m_currentModuleName.clear();
    m_loopIndices.clear();
    clearControlQueue();
}

void RunEngine::start() {
    if (isBusy()) {
        return;
    }
    {
        QReadLocker locker(&m_moduleLock);
        if (m_modules.isEmpty()) {
            RunResult result;
            result.success = false;
            result.errorCode = -1;
            result.errorMessage = tr("No modules to run");
            result.elapsedMs = 0;
            result.finishedTime = QDateTime::currentDateTime();
            emit errorOccurred(result.errorMessage);
            emit runFinished(result);
            return;
        }
    }

    resetStepState();
    m_runMode.store(static_cast<int>(RunMode::RunCycle), std::memory_order_release);
    m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    emit stateChanged(state());
    emit cycleStarted();

    Logger::instance().info(tr("Starting continuous run"), "Run");
    m_cycleTimer->start();
}

void RunEngine::pause() {
    if (state() != RunState::Running) {
        return;
    }

    m_state.store(static_cast<int>(RunState::Paused), std::memory_order_release);
    m_cycleTimer->stop();
    emit stateChanged(state());

    Logger::instance().info(tr("Run paused"), "Run");
}

void RunEngine::resume() {
    if (state() != RunState::Paused) {
        return;
    }

    if (m_pausedAtBreakpoint) {
        executeRun();
        return;
    }

    m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    m_cycleTimer->start();
    emit stateChanged(state());

    Logger::instance().info(tr("Run resumed"), "Run");
}

void RunEngine::stop() {
    if (state() == RunState::Stopped) {
        return;
    }

    m_cycleTimer->stop();
    m_state.store(static_cast<int>(RunState::Stopped), std::memory_order_release);
    m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
    clearBreakpointPauseState();
    if (m_cancellationToken) {
        m_cancellationToken->cancel();
    }
    resetStepState();

    emit stateChanged(state());
    emit cycleStopped();

    Logger::instance().info(tr("Run stopped"), "Run");
}

bool RunEngine::isPausedAtBreakpoint() const {
    return m_pausedAtBreakpoint;
}

void RunEngine::requestCancellation() {
    if (m_cancellationToken) {
        m_cancellationToken->cancel();
    }
    stop();
}

void RunEngine::addModule(ModuleBase* module) {
    if (isBusy()) {
        Logger::instance().warning(tr("Cannot add modules while the flow is running"), "Run");
        return;
    }
    QWriteLocker locker(&m_moduleLock);
    if (module && !m_modules.contains(module)) {
        m_modules.append(module);
        QString key = runtimeModuleName(module);
        m_moduleMap[key] = module;
        Logger::instance().debug(QString("Module added to engine: %1").arg(key), "Run");
    }
}

bool RunEngine::loadProject(Project* project, ModuleFactory factory) {
    if (isBusy()) {
        emit errorOccurred(tr("Cannot load a project while the flow is running"));
        return false;
    }
    stop();
    clearModules();

    if (!project) {
        emit errorOccurred(tr("No project to load"));
        return false;
    }

    if (!factory) {
        factory = [](const ModuleInstance& inst) -> ModuleBase* {
            PluginManager& pluginManager = PluginManager::instance();
            if (!pluginManager.isPluginLoaded(inst.moduleId) && !pluginManager.loadPlugin(inst.moduleId, 5000)) {
                return nullptr;
            }

            IModule* module = pluginManager.createModule(inst.moduleId);
            ModuleBase* moduleBase = qobject_cast<ModuleBase*>(module);
            if (!moduleBase) {
                delete module;
                return nullptr;
            }
            return moduleBase;
        };
    }

    m_disabledModules.clear();
    for (const ModuleInstance& inst : project->modules()) {
        ModuleBase* module = factory(inst);
        if (!module) {
            clearModules();
            emit errorOccurred(tr("Cannot create module: %1").arg(inst.moduleId));
            return false;
        }

        module->setInstanceName(inst.id);
        module->setParams(inst.params);
        if (!module->initialize()) {
            delete module;
            clearModules();
            emit errorOccurred(tr("Cannot initialize module: %1").arg(inst.moduleId));
            return false;
        }

        m_ownedModules.append(module);
        addModule(module);

        // 阶段 B: 断点/禁用同步
        if (inst.breakpoint)
            setBreakpoint(inst.id, true);
        if (!inst.enabled)
            m_disabledModules.insert(inst.id);
    }

    QString orderError;
    if (!buildExecutionOrder(project, orderError)) {
        clearModules();
        emit errorOccurred(orderError);
        return false;
    }

    return true;
}

void RunEngine::removeModule(const QString& moduleId) {
    if (isBusy()) {
        Logger::instance().warning(tr("Cannot remove modules while the flow is running"), "Run");
        return;
    }
    QString removedName;
    QWriteLocker locker(&m_moduleLock);
    for (int i = 0; i < m_modules.size(); ++i) {
        ModuleBase* module = m_modules[i];
        if (module->id() == moduleId || runtimeModuleName(module) == moduleId) {
            removedName = runtimeModuleName(module);
            m_moduleMap.remove(removedName);
            m_modules.removeAt(i);
            m_disabledModules.remove(removedName);
            m_nodeOutputs.remove(removedName);
            m_executionOrder.removeAll(removedName); // 从拓扑序移除，不清空剩余 DAG
            {
                QMutexLocker bl(&m_breakpointMutex);
                m_breakpoints.remove(removedName);
            }
            if (m_ownedModules.removeOne(module)) {
                delete module;
            }
            break;
        }
    }
    // 用解析后的名称清理连接，避免插件ID vs 实例名不匹配
    for (int i = m_connections.size() - 1; i >= 0; --i) {
        if (m_connections[i].fromModuleId == removedName || m_connections[i].toModuleId == removedName) {
            m_connections.removeAt(i);
        }
    }
    for (int i = m_controlEdges.size() - 1; i >= 0; --i) {
        if (m_controlEdges[i].fromModuleId == removedName || m_controlEdges[i].toModuleId == removedName) {
            m_controlEdges.removeAt(i);
        }
    }
    // 清除已删节点的运行期输出和单步/暂停状态
    if (!removedName.isEmpty()) {
        m_triggeredControlEdges.remove(removedName);
        m_reentrantModules.remove(removedName);
        m_loopRegions.remove(removedName);
        for (auto region = m_loopRegions.begin(); region != m_loopRegions.end(); ++region)
            region->remove(removedName);
        invalidateModuleOutput(removedName);
        resetStepState();
        if (m_pauseResumeModule == removedName) {
            m_pauseResumeModule.clear();
            m_pausedAtBreakpoint = false;
        }
    }
}

void RunEngine::clearModules() {
    if (isBusy()) {
        Logger::instance().warning(tr("Cannot clear modules while a module is executing"), "Run");
        return;
    }
    QWriteLocker locker(&m_moduleLock);
    qDeleteAll(m_ownedModules);
    m_ownedModules.clear();
    m_modules.clear();
    m_moduleMap.clear();
    m_executionOrder.clear();
    m_disabledModules.clear();
    m_connections.clear();
    m_controlEdges.clear();
    m_loopBackEdges.clear();
    m_reentrantModules.clear();
    m_loopRegions.clear();
    m_nodeOutputs.clear();
    clearBreakpointPauseState();
    locker.unlock();
    {
        QMutexLocker bl(&m_breakpointMutex);
        m_breakpoints.clear();
    }
    clearOutputs();
    resetStepState();
}

QList<ModuleBase*> RunEngine::modules() const {
    QReadLocker locker(&m_moduleLock);
    return m_modules;
}

ModuleBase* RunEngine::getModule(const QString& moduleName) const {
    QReadLocker locker(&m_moduleLock);
    return m_moduleMap.value(moduleName, nullptr);
}

int RunEngine::getModuleIndex(const QString& moduleName) const {
    QReadLocker locker(&m_moduleLock);
    for (int i = 0; i < m_modules.size(); ++i) {
        if (runtimeModuleName(m_modules[i]) == moduleName) {
            return i;
        }
    }
    return -1;
}

void RunEngine::setOutput(const QString& moduleName, const QString& varName, const QVariant& value) {
    {
        QMutexLocker locker(&m_outputMutex);
        m_outputMap[moduleName][varName] = value;
    }
    emit outputChanged(moduleName, varName, value);
}

QVariant RunEngine::getOutput(const QString& moduleName, const QString& varName) const {
    QMutexLocker locker(&m_outputMutex);
    if (m_outputMap.contains(moduleName) && m_outputMap[moduleName].contains(varName)) {
        return m_outputMap[moduleName][varName];
    }
    return QVariant();
}

bool RunEngine::hasOutput(const QString& moduleName, const QString& varName) const {
    QMutexLocker locker(&m_outputMutex);
    return m_outputMap.contains(moduleName) && m_outputMap[moduleName].contains(varName);
}

void RunEngine::clearOutputs() {
    {
        QMutexLocker locker(&m_outputMutex);
        m_outputMap.clear();
    }
    clearModuleOutputs();
}

void RunEngine::clearModuleOutputs() {
    QMutexLocker locker(&m_lastOutputMutex);
    m_lastOutput = ImageData();
    m_lastOutputModuleName.clear();
    m_moduleOutputs.clear();
}

void RunEngine::clearBreakpointPauseState() {
    m_pauseResumeModule.clear();
    m_pausePipelineData = ImageData();
    m_breakpointPausedAt = QDateTime();
    m_pausedAtBreakpoint = false;
    m_skipBreakpointOnce = false;
}

ImageData RunEngine::lastOutput() const {
    QMutexLocker locker(&m_lastOutputMutex);
    return m_lastOutput;
}

ImageData RunEngine::moduleOutput(const QString& moduleName) const {
    QMutexLocker locker(&m_lastOutputMutex);
    return m_moduleOutputs.value(moduleName);
}

void RunEngine::invalidateModuleOutput(const QString& moduleName) {
    {
        QMutexLocker locker(&m_outputMutex);
        m_outputMap.remove(moduleName);
    }
    QMutexLocker locker(&m_lastOutputMutex);
    m_moduleOutputs.remove(moduleName);
    if (m_lastOutputModuleName == moduleName) {
        m_lastOutput = ImageData();
        m_lastOutputModuleName.clear();
    }
}

int RunEngine::totalRuns() const {
    QMutexLocker locker(&m_statsMutex);
    return m_totalRuns;
}

int RunEngine::successRuns() const {
    QMutexLocker locker(&m_statsMutex);
    return m_successRuns;
}

int RunEngine::failedRuns() const {
    QMutexLocker locker(&m_statsMutex);
    return m_failedRuns;
}

int RunEngine::lastElapsedMs() const {
    QMutexLocker locker(&m_statsMutex);
    return m_lastElapsedMs;
}

void RunEngine::onTimerTick() {
    if (state() == RunState::Running && runMode() == RunMode::RunCycle) {
        executeRun();
    }
}

void RunEngine::executeRun() {
    const bool resuming = m_pausedAtBreakpoint && !m_pauseResumeModule.isEmpty();

    if (!resuming) {
        {
            QReadLocker locker(&m_moduleLock);
            if (m_modules.isEmpty()) {
                RunResult result;
                result.success = false;
                result.errorCode = -1;
                result.errorMessage = tr("No modules to run");
                result.elapsedMs = 0;
                result.finishedTime = QDateTime::currentDateTime();
                if (runMode() == RunMode::RunCycle) {
                    m_cycleTimer->stop();
                    m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
                    m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
                    emit stateChanged(state());
                    emit cycleStopped();
                }
                emit runFinished(result);
                return;
            }
        }

        const QStringList validationErrors = validateFlow();
        if (!validationErrors.isEmpty()) {
            RunResult result;
            result.success = false;
            result.errorCode = ExecError::TypeMismatch;
            result.errorMessage = validationErrors.join(QStringLiteral("\n"));
            result.finishedTime = QDateTime::currentDateTime();
            emit errorOccurred(result.errorMessage);
            if (runMode() == RunMode::RunCycle) {
                m_cycleTimer->stop();
                m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
                m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
                emit stateChanged(state());
                emit cycleStopped();
            }
            emit runFinished(result);
            return;
        }

        if (m_executing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
        emit stateChanged(state());
        emit runStarted();

        m_runStartTime = QDateTime::currentDateTime();
        m_runAllSuccess = true;
        m_runFirstError.clear();
        clearBreakpointPauseState();
        clearModuleOutputs();
        m_runId = QString::number(QDateTime::currentMSecsSinceEpoch());
        m_frameId.store(0, std::memory_order_release);
        m_lastParallelMaxConcurrency.store(0, std::memory_order_release);
        m_nodeOutputs.clear();

        if (m_cancellationToken) {
            m_cancellationToken->reset();
        }

        buildModuleTree();
    } else {
        if (m_executing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        if (m_breakpointPausedAt.isValid()) {
            const qint64 pausedMs = m_breakpointPausedAt.msecsTo(QDateTime::currentDateTime());
            if (pausedMs > 0) {
                m_runStartTime = m_runStartTime.addMSecs(pausedMs);
            }
        }
        m_pausedAtBreakpoint = false;
        m_skipBreakpointOnce = true;
        m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
        emit stateChanged(state());
    }

    QString currentModule;
    ImageData pipelineData;
    if (resuming) {
        currentModule = m_pauseResumeModule;
        pipelineData = m_pausePipelineData;
        m_pauseResumeModule.clear();
        m_pausePipelineData = ImageData();
        m_breakpointPausedAt = QDateTime();
    } else if (!m_controlEdges.isEmpty()) {
        initializeControlQueue();
        currentModule = m_controlQueue.value(0);
    } else {
        {
            QReadLocker locker(&m_moduleLock);
            currentModule =
                m_executionOrder.isEmpty() ? runtimeModuleName(m_modules.first()) : m_executionOrder.first();
        }
    }
    m_currentModuleName = currentModule;

    // 阶段 D1: 显式控制图存在时走激活队列；否则走 legacy flowControlType 调度
    if (!m_controlEdges.isEmpty()) {
        executeRunWithControlGraph(pipelineData);
    } else {
        executeRunLegacy(pipelineData, m_runAllSuccess, m_runFirstError);
    }

    // 断点暂停时不发射 runFinished（运行未结束，仅暂停）
    if (m_pausedAtBreakpoint) {
        return;
    }

    const int elapsedMs = m_runStartTime.msecsTo(QDateTime::currentDateTime());
    const bool allSuccess = m_runAllSuccess && state() != RunState::Stopped;
    updateStatistics(allSuccess, elapsedMs);

    RunResult result;
    result.success = allSuccess;
    result.errorCode = allSuccess ? 0 : -1;
    result.errorMessage =
        allSuccess ? QString() : (m_runFirstError.isEmpty() ? tr("Flow execution failed") : m_runFirstError);
    result.elapsedMs = elapsedMs;
    result.finishedTime = QDateTime::currentDateTime();

    m_executing.store(false, std::memory_order_release);
    clearControlQueue();
    clearBreakpointPauseState();
    emit runFinished(result);

    if (runMode() == RunMode::RunOnce) {
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
        emit stateChanged(state());
    }
}

// ---------------------------------------------------------------------------
// 阶段 D1: 显式控制图执行（激活队列）
// ---------------------------------------------------------------------------

void RunEngine::executeRunWithControlGraph(ImageData& pipelineData) {
    while (!m_controlQueue.isEmpty()) {
        if (state() != RunState::Running)
            break;
        if (m_cancellationToken && m_cancellationToken->isCancelledFast()) {
            stop();
            break;
        }

        const QString mod = m_controlQueue.first();

        // 断点
        if (m_skipBreakpointOnce) {
            m_skipBreakpointOnce = false;
        } else if (hasBreakpoint(mod)) {
            m_pauseResumeModule = mod;
            m_pausePipelineData = pipelineData;
            m_breakpointPausedAt = QDateTime::currentDateTime();
            m_pausedAtBreakpoint = true;
            m_state.store(static_cast<int>(RunState::Paused), std::memory_order_release);
            m_executing.store(false, std::memory_order_release);
            emit stateChanged(state());
            emit breakpointHit(mod);
            return;
        }

        // 仅并发执行明确声明线程安全、非阻塞、启用且没有断点的就绪模块。
        // G-fix4: blocking 模块（文件 I/O 等）不进并行批次，避免占满线程池。
        QStringList batch;
        batch.append(m_controlQueue.takeFirst());
        if (m_reentrantModules.contains(mod))
            m_controlActivated.remove(mod);
        ModuleBase* firstModule = getModule(mod);
        const bool canBuildParallelBatch = parallelThreadCount() > 1 && firstModule && firstModule->isThreadSafe() &&
                                           !firstModule->isBlocking() && !isModuleDisabled(mod);
        while (canBuildParallelBatch && !m_controlQueue.isEmpty()) {
            const QString nextMod = m_controlQueue.first();
            ModuleBase* nextModule = getModule(nextMod);
            if (!nextModule || !nextModule->isThreadSafe() || nextModule->isBlocking() || isModuleDisabled(nextMod) ||
                hasBreakpoint(nextMod))
                break;
            batch.append(m_controlQueue.takeFirst());
            if (m_reentrantModules.contains(nextMod))
                m_controlActivated.remove(nextMod);
        }

        if (batch.size() == 1) {
            executeOrBypassModule(mod, pipelineData);
            m_controlProcessed.insert(mod);
            if (!m_lastExecuteResult) {
                m_runAllSuccess = false;
                if (m_runFirstError.isEmpty())
                    m_runFirstError = m_lastModuleError;
            } else {
                activateControlSuccessors(mod);
            }
        } else {
            executeBatchParallel(batch, pipelineData);
        }
        m_currentModuleName = m_controlQueue.value(0);
    }

    if (m_controlQueue.isEmpty() && state() == RunState::Running)
        emitInactiveControlModules();
}

PortValueMap RunEngine::collectModuleInputs(const QString& moduleName, const ImageData& pipelineData) const {
    QReadLocker locker(&m_moduleLock);
    ModuleBase* module = m_moduleMap.value(moduleName, nullptr);
    if (!module)
        return {};

    QSet<QString> multiPorts;
    for (const PortSpec& spec : module->inputPorts()) {
        if (spec.multiple && !spec.control)
            multiPorts.insert(spec.id);
    }

    QList<ModuleConnection> incoming;
    for (const ModuleConnection& connection : m_connections) {
        if (connection.toModuleId == moduleName && connection.edgeType != QLatin1String("control"))
            incoming.append(connection);
    }

    QHash<QString, int> executionRank;
    for (int i = 0; i < m_executionOrder.size(); ++i)
        executionRank.insert(m_executionOrder[i], i);
    std::stable_sort(incoming.begin(), incoming.end(),
                     [&executionRank](const ModuleConnection& a, const ModuleConnection& b) {
                         return executionRank.value(a.fromModuleId, std::numeric_limits<int>::max()) <
                                executionRank.value(b.fromModuleId, std::numeric_limits<int>::max());
                     });

    PortValueMap inputs;
    QMap<QString, QVariantList> multiValues;
    for (const ModuleConnection& connection : incoming) {
        const PortValueMap upstream = m_nodeOutputs.value(connection.fromModuleId);
        if (!upstream.contains(connection.fromPort))
            continue;
        if (multiPorts.contains(connection.toPort))
            multiValues[connection.toPort].append(upstream.value(connection.fromPort));
        else
            inputs.insert(connection.toPort, upstream.value(connection.fromPort));
    }
    for (auto it = multiValues.constBegin(); it != multiValues.constEnd(); ++it)
        inputs.insert(it.key(), it.value());

    if (incoming.isEmpty())
        inputs.insert(QStringLiteral("image"), QVariant::fromValue(pipelineData));
    return inputs;
}

void RunEngine::executeBatchParallel(const QStringList& batch, ImageData& pipelineData) {
    struct ParallelResult {
        bool success = false;
        QString error;
        PortValueMap outputs;
        bool controlResult = false;
        int elapsedMs = 0;
    };

    QMap<QString, ParallelResult> results;
    std::atomic<int> running{0};
    std::atomic<int> maxConcurrent{0};
    QMutex resultMutex;
    QWaitCondition cond;
    int finished = 0;
    const int total = batch.size();

    for (const QString& mod : batch) {
        ModuleBase* module = getModule(mod);
        if (!module) {
            ParallelResult missing;
            missing.error = tr("Parallel module is unavailable: %1").arg(mod);
            results.insert(mod, missing);
            ++finished;
            continue;
        }

        m_parallelPool.start(
            [this, mod, module, pipelineData, &results, &resultMutex, &cond, &finished, &running, &maxConcurrent]() {
                ParallelResult result;
                QElapsedTimer timer;
                timer.start();
                emit moduleStarted(mod);

                if (m_cancellationToken && m_cancellationToken->isCancelledFast()) {
                    result.error = tr("Parallel module cancelled before execution: %1").arg(mod);
                } else {
                    const int cur = ++running;
                    int expected = maxConcurrent.load();
                    while (cur > expected && !maxConcurrent.compare_exchange_weak(expected, cur)) {
                    }

                    module->setCancellationToken(m_cancellationToken);
                    const PortValueMap portInputs = collectModuleInputs(mod, pipelineData);
                    PortValueMap portOutputs;
                    ExecutionContext context;
                    context.runId = m_runId;
                    context.frameId = m_frameId.fetch_add(1, std::memory_order_relaxed);
                    context.timestampMs = QDateTime::currentMSecsSinceEpoch();
                    context.runMode = module->flowControlType();
                    context.cancellationToken = m_cancellationToken;
                    const ExecutionResult execution = module->execute(portInputs, portOutputs, context);

                    result.success = execution.success;
                    result.outputs = portOutputs;
                    result.error = execution.userMessage;
                    if (result.success && portOutputs.contains(QStringLiteral("image"))) {
                        const ImageData output = portOutputs.value(QStringLiteral("image")).value<ImageData>();
                        const QMap<QString, QVariant> values = output.allData();
                        const ControlFlowType type = module->flowControlType();
                        if (type == ControlFlowType::Conditional || type == ControlFlowType::ConditionalElse) {
                            if (values.contains(QStringLiteral("if_result")))
                                result.controlResult = values.value(QStringLiteral("if_result")).toBool();
                            else if (values.contains(QStringLiteral("condition_result")))
                                result.controlResult = values.value(QStringLiteral("condition_result")).toBool();
                        } else if (type == ControlFlowType::While && values.contains(QStringLiteral("while_result"))) {
                            result.controlResult = values.value(QStringLiteral("while_result")).toBool();
                        } else if (type == ControlFlowType::StopLoop &&
                                   values.contains(QStringLiteral("stop_while_requested"))) {
                            result.controlResult = values.value(QStringLiteral("stop_while_requested")).toBool();
                        }
                    }
                    --running;
                }
                result.elapsedMs = static_cast<int>(timer.elapsed());

                {
                    QMutexLocker locker(&resultMutex);
                    results.insert(mod, result);
                    ++finished;
                    if (!result.success && m_cancellationToken)
                        m_cancellationToken->cancel();
                    cond.wakeAll();
                }
            });
    }

    // 等待全部完成
    {
        QMutexLocker locker(&resultMutex);
        while (finished < total)
            cond.wait(&resultMutex);
    }

    m_lastParallelMaxConcurrency.store(maxConcurrent.load(), std::memory_order_release);

    // 应用结果到共享状态
    for (const QString& mod : batch) {
        const ParallelResult pr = results.value(mod);
        m_controlProcessed.insert(mod);

        if (pr.success) {
            m_nodeOutputs[mod] = pr.outputs;
            if (pr.outputs.contains(QStringLiteral("image"))) {
                ImageData out = pr.outputs.value(QStringLiteral("image")).value<ImageData>();
                pipelineData = out;
                {
                    QMutexLocker locker(&m_lastOutputMutex);
                    m_lastOutput = out;
                    m_lastOutputModuleName = mod;
                    m_moduleOutputs[mod] = out;
                }
            }
            m_lastExecuteResult = true;
            m_lastControlResult = pr.controlResult;
            emit moduleFinished(mod, true, pr.elapsedMs);
            activateControlSuccessors(mod);
        } else {
            m_lastExecuteResult = false;
            m_lastModuleError = pr.error.isEmpty() ? tr("Parallel module failed: %1").arg(mod) : pr.error;
            m_runAllSuccess = false;
            if (m_runFirstError.isEmpty())
                m_runFirstError = m_lastModuleError;
            emit moduleFinished(mod, false, pr.elapsedMs);
        }
    }
}

void RunEngine::initializeControlQueue() {
    clearControlQueue();
    // 结构化回边不构成入口入度；普通前驱边仍然构成入度。
    QSet<QString> hasIncoming;
    for (const ModuleConnection& conn : m_connections) {
        if (m_loopBackEdges.contains(connectionKey(conn)))
            continue;
        hasIncoming.insert(conn.toModuleId);
    }

    QStringList order = m_executionOrder;
    if (order.isEmpty()) {
        QReadLocker locker(&m_moduleLock);
        for (ModuleBase* module : m_modules)
            order.append(runtimeModuleName(module));
    }
    for (const QString& moduleName : order) {
        if (!hasIncoming.contains(moduleName)) {
            m_controlQueue.append(moduleName);
            m_controlActivated.insert(moduleName);
        }
    }
}

void RunEngine::activateControlSuccessors(const QString& moduleName) {
    ModuleBase* module = getModule(moduleName);
    if (!module)
        return;

    const ControlFlowType ft = module->flowControlType();
    const bool isConditional = ft == ControlFlowType::Conditional;
    const bool isLoop = ft == ControlFlowType::Loop;
    const bool isWhile = ft == ControlFlowType::While;
    const bool isStopLoop = ft == ControlFlowType::StopLoop;

    const auto activateControl = [this](const ModuleConnection& edge) {
        tryActivateSuccessor(edge.fromModuleId, edge.toModuleId, true, edge.fromPort, edge.toPort);
    };

    if (ft == ControlFlowType::Parallel && module->currentParams().contains(QStringLiteral("parallelCount")))
        setParallelThreadCount(module->currentParams().value(QStringLiteral("parallelCount")).toInt());

    if ((isLoop || isWhile) && isModuleDisabled(moduleName)) {
        for (const ModuleConnection& edge : m_controlEdges) {
            if (edge.fromModuleId == moduleName && edge.fromPort == QLatin1String("done"))
                activateControl(edge);
        }
        m_loopIndices.remove(moduleName);
        m_activeLoops.removeAll(moduleName);
        return;
    }

    if (isLoop) {
        const int loopCount = module->currentParams().value("loopCount").toInt(0);
        const int iter = m_loopIndices.value(moduleName, 0);
        if (iter < loopCount) {
            clearLoopIterationOutputs(moduleName);
            for (const ModuleConnection& edge : m_controlEdges) {
                if (edge.fromModuleId == moduleName && edge.fromPort == QLatin1String("body"))
                    activateControl(edge);
            }
        } else {
            for (const ModuleConnection& edge : m_controlEdges) {
                if (edge.fromModuleId == moduleName && edge.fromPort == QLatin1String("done"))
                    activateControl(edge);
            }
            m_loopIndices.remove(moduleName);
            m_activeLoops.removeAll(moduleName);
        }
        return;
    }

    if (isWhile) {
        const int maxIter = module->currentParams().value("maxIterations").toInt(100);
        const int iter = m_loopIndices.value(moduleName, 0);
        if (m_lastControlResult && iter < maxIter) {
            clearLoopIterationOutputs(moduleName);
            for (const ModuleConnection& edge : m_controlEdges) {
                if (edge.fromModuleId == moduleName && edge.fromPort == QLatin1String("body"))
                    activateControl(edge);
            }
        } else {
            for (const ModuleConnection& edge : m_controlEdges) {
                if (edge.fromModuleId == moduleName && edge.fromPort == QLatin1String("done"))
                    activateControl(edge);
            }
            m_loopIndices.remove(moduleName);
            m_activeLoops.removeAll(moduleName);
        }
        return;
    }

    if (isStopLoop) {
        for (const ModuleConnection& edge : m_controlEdges) {
            if (edge.fromModuleId == moduleName && edge.fromPort == QLatin1String("stop"))
                activateControl(edge);
        }
        if (!m_activeLoops.isEmpty()) {
            const QString stoppedLoop = m_activeLoops.takeLast();
            m_loopIndices.remove(stoppedLoop);
        }
        return;
    }

    for (const ModuleConnection& connection : m_controlEdges) {
        if (connection.fromModuleId != moduleName)
            continue;
        if (!isConditional || (connection.fromPort == QLatin1String("true") && m_lastControlResult) ||
            (connection.fromPort == QLatin1String("false") && !m_lastControlResult)) {
            activateControl(connection);
        }
    }
    if (!isConditional && !isStopLoop) {
        for (const ModuleConnection& connection : m_connections) {
            if (connection.fromModuleId == moduleName && connection.edgeType != QLatin1String("control"))
                tryActivateSuccessor(moduleName, connection.toModuleId, false);
        }
    }
}

void RunEngine::tryActivateSuccessor(const QString& sourceModule, const QString& targetModule, bool controlTriggered,
                                     const QString& sourcePort, const QString& targetPort) {
    const bool canRepeat = m_reentrantModules.contains(sourceModule) && m_reentrantModules.contains(targetModule);
    if (m_controlActivated.contains(targetModule) || (m_controlProcessed.contains(targetModule) && !canRepeat))
        return;

    if (controlTriggered) {
        ModuleConnection triggered;
        triggered.fromModuleId = sourceModule;
        triggered.toModuleId = targetModule;
        triggered.fromPort = sourcePort;
        triggered.toPort = targetPort;
        m_triggeredControlEdges[targetModule].insert(connectionKey(triggered));
    }

    QMap<QString, QList<ModuleConnection>> incomingByPort;
    for (const ModuleConnection& edge : m_controlEdges) {
        if (edge.toModuleId == targetModule)
            incomingByPort[edge.toPort].append(edge);
    }

    ModuleBase* target = getModule(targetModule);
    const QList<PortSpec> inputPorts = target ? target->inputPorts() : QList<PortSpec>{};
    const QSet<QString> triggeredEdges = m_triggeredControlEdges.value(targetModule);
    for (auto it = incomingByPort.constBegin(); it != incomingByPort.constEnd(); ++it) {
        const PortSpec* port = findPort(inputPorts, it.key());
        const bool requireAll = port && port->control && port->multiple && port->joinPolicy == ControlJoinPolicy::All;
        int triggeredCount = 0;
        for (const ModuleConnection& edge : it.value()) {
            if (triggeredEdges.contains(connectionKey(edge)))
                ++triggeredCount;
        }
        if (requireAll) {
            if (triggeredCount != it.value().size())
                return;
        } else if (triggeredCount == 0) {
            return;
        }
    }

    if (!dataInputsReady(targetModule))
        return;

    m_triggeredControlEdges.remove(targetModule);
    m_controlActivated.insert(targetModule);
    m_controlQueue.append(targetModule);
}

bool RunEngine::dataInputsReady(const QString& moduleName) const {
    ModuleBase* module = getModule(moduleName);
    if (!module)
        return false;

    // 统计每个 toPort 的上游连接数与已就绪数
    QMap<QString, int> totalUpstream;
    QMap<QString, int> readyUpstream;
    for (const ModuleConnection& connection : m_connections) {
        if (connection.toModuleId != moduleName || connection.edgeType == QLatin1String("control"))
            continue;
        totalUpstream[connection.toPort]++;
        if (m_nodeOutputs.contains(connection.fromModuleId) &&
            m_nodeOutputs.value(connection.fromModuleId).contains(connection.fromPort))
            readyUpstream[connection.toPort]++;
    }

    for (const PortSpec& input : module->inputPorts()) {
        if (!input.required || input.control)
            continue;
        const int total = totalUpstream.value(input.id, 0);
        const int ready = readyUpstream.value(input.id, 0);
        if (total == 0 || (input.multiple && ready != total) || (!input.multiple && ready == 0))
            return false;
    }
    return true;
}

void RunEngine::clearLoopIterationOutputs(const QString& loopModule) {
    for (const QString& moduleName : m_loopRegions.value(loopModule)) {
        m_nodeOutputs.remove(moduleName);
        m_triggeredControlEdges.remove(moduleName);
    }
}

void RunEngine::emitInactiveControlModules() {
    QStringList moduleNames;
    {
        QReadLocker locker(&m_moduleLock);
        for (ModuleBase* module : m_modules)
            moduleNames.append(runtimeModuleName(module));
    }
    for (const QString& moduleName : moduleNames) {
        if (!m_controlProcessed.contains(moduleName))
            emit moduleSkipped(moduleName);
    }
}

void RunEngine::clearControlQueue() {
    m_controlQueue.clear();
    m_controlActivated.clear();
    m_controlProcessed.clear();
    m_triggeredControlEdges.clear();
    m_loopIndices.clear();
    m_activeLoops.clear();
}

void RunEngine::executeRunLegacy(ImageData& pipelineData, bool& allSuccess, QString& firstError) {
    // 阶段 D3: legacy 路径——无控制边时使用 flowControlType 调度
    // 暂时内联原逻辑（后续 D3 合并）
    QString currentModule = m_currentModuleName;

    while (!currentModule.isEmpty() && state() == RunState::Running) {
        if (m_cancellationToken && m_cancellationToken->isCancelledFast()) {
            stop();
            break;
        }

        if (m_skipBreakpointOnce) {
            m_skipBreakpointOnce = false;
        } else if (hasBreakpoint(currentModule)) {
            m_pauseResumeModule = currentModule;
            m_pausePipelineData = pipelineData;
            m_breakpointPausedAt = QDateTime::currentDateTime();
            m_pausedAtBreakpoint = true;
            m_state.store(static_cast<int>(RunState::Paused), std::memory_order_release);
            m_executing.store(false, std::memory_order_release);
            emit stateChanged(state());
            emit breakpointHit(currentModule);
            return;
        }

        executeOrBypassModule(currentModule, pipelineData);
        if (!m_lastExecuteResult) {
            allSuccess = false;
            if (firstError.isEmpty())
                firstError = m_lastModuleError;
        }

        // 阶段 3.2: 控制节点前向跳过时标记 Skipped
        const QString nextModule = getNextModule(currentModule, m_lastControlResult);
        if (!nextModule.isEmpty()) {
            QStringList order = m_executionOrder;
            if (order.isEmpty()) {
                QReadLocker locker(&m_moduleLock);
                QString cur = m_modules.isEmpty() ? QString() : runtimeModuleName(m_modules.first());
                QSet<QString> seen;
                while (!cur.isEmpty() && !seen.contains(cur)) {
                    seen.insert(cur);
                    order.append(cur);
                    cur = getNextSequentialModule(cur);
                }
            }
            const int curIdx = order.indexOf(currentModule);
            const int nextIdx = order.indexOf(nextModule);
            if (curIdx >= 0 && nextIdx > curIdx + 1) {
                for (int i = curIdx + 1; i < nextIdx; ++i)
                    emit moduleSkipped(order[i]);
            }
        }
        currentModule = nextModule;
        m_currentModuleName = currentModule;
    }
}

void RunEngine::executeModule(const QString& moduleName, ImageData& pipelineData) {
    m_lastModuleError.clear();
    m_lastControlResult = false;
    ModuleBase* module = getModule(moduleName);
    if (!module) {
        m_lastModuleError = tr("Module not found: %1").arg(moduleName);
        Logger::instance().error(m_lastModuleError, "Run");
        m_lastExecuteResult = false;
        return;
    }

    emit moduleStarted(moduleName);
    module->setCancellationToken(m_cancellationToken);

    // 处理循环索引（基于 flowControlType，不再基于名称）
    ControlFlowType flowType = module->flowControlType();
    if (flowType == ControlFlowType::Loop || flowType == ControlFlowType::While) {
        if (!m_loopIndices.contains(moduleName)) {
            m_loopIndices[moduleName] = 0;
            m_activeLoops.append(moduleName);
        } else {
            m_loopIndices[moduleName]++;
        }
    }

    // 执行模块，将上一个模块的输出作为当前模块的输入
    ImageData output;
    QString moduleError;
    QMutex moduleErrorMutex;
    const QMetaObject::Connection errorConnection = connect(
        module, &IModule::errorOccurred, this,
        [&moduleError, &moduleErrorMutex](const QString& error) {
            QMutexLocker locker(&moduleErrorMutex);
            if (moduleError.isEmpty()) {
                moduleError = error;
            }
        },
        Qt::DirectConnection);
    QElapsedTimer moduleTimer;
    moduleTimer.start();
    // ABI v2：强类型端口执行，携带执行上下文
    const PortValueMap portInputs = collectModuleInputs(moduleName, pipelineData);
    PortValueMap portOutputs;
    ExecutionContext execCtx;
    execCtx.runId = m_runId;
    execCtx.frameId = m_frameId.fetch_add(1, std::memory_order_relaxed);
    execCtx.timestampMs = QDateTime::currentMSecsSinceEpoch();
    execCtx.runMode = module->flowControlType();
    execCtx.cancellationToken = m_cancellationToken;
    const ExecutionResult execResult = module->execute(portInputs, portOutputs, execCtx);
    bool success = execResult.success;
    if (success) {
        m_nodeOutputs[moduleName] = portOutputs; // 缓存每端口输出供下游/单步使用
    }
    if (success && portOutputs.contains(QStringLiteral("image"))) {
        output = portOutputs.value(QStringLiteral("image")).value<ImageData>();
    }
    if (!success && !execResult.userMessage.isEmpty()) {
        QMutexLocker locker(&moduleErrorMutex);
        if (moduleError.isEmpty()) {
            moduleError = execResult.userMessage;
        }
    }
    disconnect(errorConnection);
    const qint64 elapsedMs = moduleTimer.elapsed();
    Logger::instance().info(QString("Module finished: %1 success=%2 elapsed=%3 ms")
                                .arg(moduleName)
                                .arg(success ? "true" : "false")
                                .arg(elapsedMs),
                            "Run");

    // 如果执行成功，将当前输出传递为下一个模块的输入
    if (success) {
        pipelineData = output;
        m_lastControlResult = true;
        const QMap<QString, QVariant> values = output.allData();
        if (flowType == ControlFlowType::Conditional || flowType == ControlFlowType::ConditionalElse) {
            if (values.contains(QStringLiteral("if_result"))) {
                m_lastControlResult = values.value(QStringLiteral("if_result")).toBool();
            } else if (values.contains(QStringLiteral("condition_result"))) {
                m_lastControlResult = values.value(QStringLiteral("condition_result")).toBool();
            }
        } else if (flowType == ControlFlowType::While && values.contains(QStringLiteral("while_result"))) {
            m_lastControlResult = values.value(QStringLiteral("while_result")).toBool();
        } else if (flowType == ControlFlowType::StopLoop && values.contains(QStringLiteral("stop_while_requested"))) {
            m_lastControlResult = values.value(QStringLiteral("stop_while_requested")).toBool();
        }
        {
            QMutexLocker locker(&m_lastOutputMutex);
            m_lastOutput = output;
            m_lastOutputModuleName = moduleName;
            m_moduleOutputs[moduleName] = output;
        }
    } else {
        {
            QMutexLocker locker(&moduleErrorMutex);
            m_lastModuleError = moduleError.trimmed();
        }
        if (m_lastModuleError.isEmpty()) {
            m_lastModuleError = tr("Module execution failed: %1").arg(moduleName);
        }
        output.setData(QStringLiteral("error"), m_lastModuleError);
        QMutexLocker locker(&m_lastOutputMutex);
        m_moduleOutputs[moduleName] = output;
    }

    m_lastExecuteResult = success;

    emit moduleFinished(moduleName, success, static_cast<int>(elapsedMs));

    if (!success) {
        Logger::instance().error(QString("Module execution failed: %1: %2").arg(moduleName, m_lastModuleError), "Run");
    }
}

namespace {
// 可旁路输出端口 = image 载体 + 同名且类型兼容的输入/输出端口对
QSet<QString> bypassableOutputPorts(const QList<PortSpec>& inPorts, const QList<PortSpec>& outPorts) {
    QSet<QString> r;
    r.insert(QStringLiteral("image"));
    for (const PortSpec& out : outPorts)
        for (const PortSpec& in : inPorts)
            if (in.id == out.id && portTypesCompatible(in.type, out.type))
                r.insert(out.id);
    return r;
}
} // namespace

PortValueMap RunEngine::collectBypass(const QString& moduleName) {
    QReadLocker locker(&m_moduleLock);
    ModuleBase* dm = m_moduleMap.value(moduleName, nullptr);
    const QList<PortSpec> inPorts = dm ? dm->inputPorts() : QList<PortSpec>{};
    const QList<PortSpec> outPorts = dm ? dm->outputPorts() : QList<PortSpec>{};
    const QSet<QString> bypassable = bypassableOutputPorts(inPorts, outPorts);

    // 按目标端口 toPort 收集上游值
    PortValueMap received;
    for (const ModuleConnection& conn : m_connections) {
        if (conn.toModuleId != moduleName || conn.edgeType == QLatin1String("control"))
            continue;
        const PortValueMap uo = m_nodeOutputs.value(conn.fromModuleId);
        if (uo.contains(conn.fromPort))
            received.insert(conn.toPort, uo.value(conn.fromPort));
    }

    PortValueMap bypass;
    for (const PortSpec& out : outPorts) {
        if (bypassable.contains(out.id) && received.contains(out.id) &&
            portValueMatchesType(received.value(out.id), out.type)) {
            bypass.insert(out.id, received.value(out.id));
        }
    }
    if (!bypass.contains(QStringLiteral("image")) && received.contains(QStringLiteral("image")))
        bypass.insert(QStringLiteral("image"), received.value(QStringLiteral("image")));
    return bypass;
}

void RunEngine::executeOrBypassModule(const QString& moduleName, ImageData& pipelineData) {
    if (isModuleDisabled(moduleName)) {
        // 禁用节点不执行、不产生旧结果；可旁路端口原样传递
        const PortValueMap bypass = collectBypass(moduleName);
        m_nodeOutputs[moduleName] = bypass;
        if (bypass.contains(QStringLiteral("image"))) {
            QMutexLocker locker(&m_lastOutputMutex);
            m_moduleOutputs[moduleName] = bypass.value(QStringLiteral("image")).value<ImageData>();
        }
        emit moduleSkipped(moduleName);
        m_lastExecuteResult = true; // 旁路不算失败
        m_lastControlResult = true;
        return;
    }

    executeModule(moduleName, pipelineData);
}

QStringList RunEngine::validateFlow() const {
    QStringList errors;
    QReadLocker locker(&m_moduleLock);
    for (ModuleBase* module : m_modules) {
        const QString id = runtimeModuleName(module);
        const QList<PortSpec> inPorts = module->inputPorts();
        const QList<PortSpec> outPorts = module->outputPorts();
        for (const PortSpec& in : inPorts) {
            if (!in.required || in.id == QLatin1String("image"))
                continue;
            bool supplied = false;
            for (const ModuleConnection& conn : m_connections) {
                if (conn.toModuleId != id || conn.toPort != in.id)
                    continue;
                supplied = true;
                break;
            }
            if (!supplied) {
                errors << tr("节点 %1 的必需输入端口 %2 无上游连接").arg(id, in.id);
            }
        }

        // 阶段 B 复核: 禁用节点无法旁路的输出端口若被下游依赖，运行前报错（含类型校验）
        if (m_disabledModules.contains(id)) {
            const QSet<QString> bypassable = bypassableOutputPorts(inPorts, outPorts);
            for (const ModuleConnection& conn : m_connections) {
                if (conn.fromModuleId != id || conn.edgeType == QLatin1String("control"))
                    continue;
                if (!bypassable.contains(conn.fromPort)) {
                    errors << tr("禁用节点 %1 无法旁路输出端口 %2，但被下游依赖").arg(id, conn.fromPort);
                }
            }
        }
    }
    return errors;
}

QString RunEngine::getNextModule(const QString& currentModule, bool lastResult) {
    ModuleBase* current = getModule(currentModule);
    if (!current)
        return QString();

    ControlFlowType flowType = current->flowControlType();

    switch (flowType) {
    case ControlFlowType::Conditional:
        if (!lastResult) {
            const QString elseModule = findSiblingByFlowType(currentModule, ControlFlowType::ConditionalElse);
            if (!elseModule.isEmpty()) {
                return elseModule;
            }
            const QString endModule = findSiblingByFlowType(currentModule, ControlFlowType::ConditionalEnd);
            if (!endModule.isEmpty()) {
                return getNextSequentialModule(endModule);
            }
            const QString bodyModule = getNextSequentialModule(currentModule);
            return bodyModule.isEmpty() ? QString() : getNextSequentialModule(bodyModule);
        }
        break;

    case ControlFlowType::ConditionalElse:
        if (!lastResult) {
            const QString elseModule = findSiblingByFlowType(currentModule, ControlFlowType::ConditionalElse);
            if (!elseModule.isEmpty()) {
                return elseModule;
            }
            const QString endModule = findSiblingByFlowType(currentModule, ControlFlowType::ConditionalEnd);
            return endModule.isEmpty() ? QString() : getNextSequentialModule(endModule);
        }
        break;

    case ControlFlowType::ConditionalEnd:
        // 条件分支结束，返回父级流程
        if (m_nodeStack.isEmpty()) {
            return getNextSequentialModule(currentModule);
        }
        break;

    case ControlFlowType::Loop: {
        int loopCount = current->currentParams().value("loopCount").toInt(10);
        if (lastResult && m_loopIndices[currentModule] < loopCount) {
            return getNextSequentialModule(currentModule);
        }
        m_loopIndices.remove(currentModule);
        const QString endModule = findSiblingByFlowType(currentModule, ControlFlowType::LoopEnd);
        if (!endModule.isEmpty()) {
            return getNextSequentialModule(endModule);
        }
        const QString bodyModule = getNextSequentialModule(currentModule);
        return bodyModule.isEmpty() ? QString() : getNextSequentialModule(bodyModule);
    }

    case ControlFlowType::LoopEnd:
        // 循环结束，跳回对应的 Loop 入口
        return findPreviousByFlowType(currentModule, ControlFlowType::Loop);

    case ControlFlowType::StopLoop: {
        if (lastResult) {
            QString endModule = findSiblingByFlowType(currentModule, ControlFlowType::LoopEnd);
            const QString whileEnd = findSiblingByFlowType(currentModule, ControlFlowType::WhileEnd);
            const auto executionIndex = [this](const QString& name) {
                return m_executionOrder.isEmpty() ? getModuleIndex(name) : m_executionOrder.indexOf(name);
            };
            if (endModule.isEmpty() || (!whileEnd.isEmpty() && executionIndex(whileEnd) < executionIndex(endModule))) {
                endModule = whileEnd;
            }
            return endModule.isEmpty() ? QString() : getNextSequentialModule(endModule);
        }
        break;
    }

    case ControlFlowType::While: {
        int maxIter = current->currentParams().value("maxIterations").toInt(100);
        if (lastResult && m_loopIndices[currentModule] < maxIter) {
            return getNextSequentialModule(currentModule);
        }
        m_loopIndices.remove(currentModule);
        const QString endModule = findSiblingByFlowType(currentModule, ControlFlowType::WhileEnd);
        if (!endModule.isEmpty()) {
            return getNextSequentialModule(endModule);
        }
        const QString bodyModule = getNextSequentialModule(currentModule);
        return bodyModule.isEmpty() ? QString() : getNextSequentialModule(bodyModule);
    }

    case ControlFlowType::WhileEnd:
        // While 循环结束，跳回对应的 While 入口
        return findPreviousByFlowType(currentModule, ControlFlowType::While);

    case ControlFlowType::Sequential:
        for (auto it = m_loopIndices.constBegin(); it != m_loopIndices.constEnd(); ++it) {
            ModuleBase* loop = getModule(it.key());
            if (!loop || getNextSequentialModule(it.key()) != currentModule) {
                continue;
            }
            const ControlFlowType loopType = loop->flowControlType();
            const ControlFlowType endType =
                loopType == ControlFlowType::Loop ? ControlFlowType::LoopEnd : ControlFlowType::WhileEnd;
            if ((loopType == ControlFlowType::Loop || loopType == ControlFlowType::While) &&
                findSiblingByFlowType(it.key(), endType).isEmpty()) {
                return it.key();
            }
        }
        break;
    default:
        break;
    }

    return getNextSequentialModule(currentModule);
}

QString RunEngine::getNextSequentialModule(const QString& currentModule) {
    if (!m_executionOrder.isEmpty()) {
        int index = m_executionOrder.indexOf(currentModule);
        if (index >= 0 && index < m_executionOrder.size() - 1) {
            return m_executionOrder[index + 1];
        }
        return QString();
    }

    int index = getModuleIndex(currentModule);
    if (index >= 0 && index < m_modules.size() - 1) {
        return runtimeModuleName(m_modules[index + 1]);
    }
    return QString();
}

bool RunEngine::buildExecutionOrder(const Project* project, QString& error) {
    m_executionOrder.clear();
    m_controlEdges.clear();
    m_loopBackEdges.clear();
    m_reentrantModules.clear();
    m_loopRegions.clear();
    m_connections = project ? project->connections() : QList<ModuleConnection>{};
    if (!project || project->connections().isEmpty()) {
        return true;
    }

    QStringList moduleIds;
    QSet<QString> moduleIdSet;
    for (ModuleBase* module : m_modules) {
        QString id = runtimeModuleName(module);
        moduleIds.append(id);
        moduleIdSet.insert(id);
    }

    QMap<QString, ModuleBase*> modulesById;
    QMap<QString, int> indegree;
    for (const QString& id : moduleIds) {
        indegree[id] = 0;
        modulesById.insert(id, m_moduleMap.value(id));
    }
    QMap<QString, QStringList> adjacency;
    QMap<QString, int> inputCounts;
    QSet<QString> uniqueEdges;

    for (ModuleConnection& conn : m_connections) {
        if (!moduleIdSet.contains(conn.fromModuleId)) {
            error = tr("Connection references missing source module: %1").arg(conn.fromModuleId);
            return false;
        }
        if (!moduleIdSet.contains(conn.toModuleId)) {
            error = tr("Connection references missing target module: %1").arg(conn.toModuleId);
            return false;
        }
        if (conn.edgeType == QLatin1String("pending")) {
            error = tr("Connection %1 -> %2 requires manual port mapping").arg(conn.fromModuleId, conn.toModuleId);
            return false;
        }

        // 阶段 C: 显式控制边契约——推断边类型并校验端口兼容
        if (conn.fromPort.isEmpty())
            conn.fromPort = QStringLiteral("image");
        if (conn.toPort.isEmpty())
            conn.toPort = QStringLiteral("image");

        ModuleBase* source = modulesById.value(conn.fromModuleId);
        ModuleBase* target = modulesById.value(conn.toModuleId);
        const QList<PortSpec> sourcePorts = source ? source->outputPorts() : QList<PortSpec>{};
        const QList<PortSpec> targetPorts = target ? target->inputPorts() : QList<PortSpec>{};
        const PortSpec* output = findPort(sourcePorts, conn.fromPort);
        const PortSpec* input = findPort(targetPorts, conn.toPort);
        if (!source || !target) {
            error = tr("Connection references an unavailable runtime module");
            return false;
        }
        // 隐式控制端口：next（所有节点的隐式控制输出）和 control（隐式控制输入）
        const bool fromIsImplicitControl = (conn.fromPort == QLatin1String("next"));
        const bool toIsImplicitControl = (conn.toPort == QLatin1String("control"));
        const bool fromIsControl = fromIsImplicitControl || (output && output->control);
        const bool toIsControl = toIsImplicitControl || (input && input->control);

        // 推断边类型
        if (conn.edgeType.isEmpty()) {
            conn.edgeType = (fromIsControl || toIsControl) ? QStringLiteral("control") : QStringLiteral("data");
        }

        if (conn.edgeType == QLatin1String("data")) {
            // 数据边：两端必须是非控制端口
            if (fromIsControl) {
                error = tr("Data edge %1.%2 targets a control port").arg(conn.fromModuleId, conn.fromPort);
                return false;
            }
            if (toIsControl) {
                error = tr("Data edge targets control port %1.%2").arg(conn.toModuleId, conn.toPort);
                return false;
            }
        } else if (conn.edgeType == QLatin1String("control")) {
            // 控制边：源必须是控制输出，目标必须是控制输入
            if (!fromIsControl) {
                error = tr("Control edge source %1.%2 is not a control port").arg(conn.fromModuleId, conn.fromPort);
                return false;
            }
            if (!toIsControl) {
                error = tr("Control edge target %1.%2 is not a control port").arg(conn.toModuleId, conn.toPort);
                return false;
            }
        } else {
            error = tr("Unknown edge type '%1' on connection %3.%4 -> %5.%6")
                        .arg(conn.edgeType, conn.fromModuleId, conn.fromPort, conn.toModuleId, conn.toPort);
            return false;
        }

        // 端口存在性校验（隐式端口跳过）
        if (!fromIsImplicitControl && !sourcePorts.isEmpty() && !output) {
            error = tr("Node %1 has no output port %2").arg(conn.fromModuleId, conn.fromPort);
            return false;
        }
        if (!toIsImplicitControl && !targetPorts.isEmpty() && !input) {
            error = tr("Node %1 has no input port %2").arg(conn.toModuleId, conn.toPort);
            return false;
        }
        // 数据边类型兼容（控制边不做数据类型校验）
        if (conn.edgeType == QLatin1String("data") && output && input &&
            !portTypesCompatible(output->type, input->type)) {
            error = tr("Connection %1.%2 (%3) is incompatible with %4.%5 (%6)")
                        .arg(conn.fromModuleId, conn.fromPort, dataTypeName(output->type), conn.toModuleId, conn.toPort,
                             dataTypeName(input->type));
            return false;
        }

        const QString edgeKey = connectionKey(conn);
        if (uniqueEdges.contains(edgeKey)) {
            error = tr("Duplicate connection %1.%2 -> %3.%4")
                        .arg(conn.fromModuleId, conn.fromPort, conn.toModuleId, conn.toPort);
            return false;
        }
        uniqueEdges.insert(edgeKey);

        if (conn.edgeType == QLatin1String("control")) {
            // 控制边不参与数据 DAG 拓扑排序，只记录
            m_controlEdges.append(conn);
            continue;
        }

        // 数据边：参与拓扑排序
        const QString inputKey = conn.toModuleId + QLatin1Char('\x1f') + conn.toPort;
        if (++inputCounts[inputKey] > 1 && input && !input->multiple) {
            error =
                tr("Input port %1.%2 does not accept multiple upstream connections").arg(conn.toModuleId, conn.toPort);
            return false;
        }
        adjacency[conn.fromModuleId].append(conn.toModuleId);
        ++indegree[conn.toModuleId];
    }

    QStringList ready;
    for (const QString& id : moduleIds) {
        if (indegree.value(id) == 0) {
            ready.append(id);
        }
    }

    while (!ready.isEmpty()) {
        const QString current = ready.takeFirst();
        m_executionOrder.append(current);
        for (const QString& downstream : adjacency.value(current)) {
            if (--indegree[downstream] == 0)
                ready.append(downstream);
        }
    }

    if (m_executionOrder.size() != moduleIds.size()) {
        m_executionOrder.clear();
        error = tr("Data-flow graph contains a cycle");
        return false;
    }

    struct CombinedEdge {
        QString target;
        bool control = false;
    };
    QMap<QString, QList<CombinedEdge>> combinedAdj;
    for (const ModuleConnection& connection : m_connections) {
        combinedAdj[connection.fromModuleId].append(
            {connection.toModuleId, connection.edgeType == QLatin1String("control")});
    }

    const auto reachable = [&combinedAdj](QStringList pending, const QString& excludedLoopTarget) {
        QSet<QString> reached;
        while (!pending.isEmpty()) {
            const QString current = pending.takeFirst();
            if (reached.contains(current))
                continue;
            reached.insert(current);
            for (const CombinedEdge& edge : combinedAdj.value(current)) {
                if (edge.control && edge.target == excludedLoopTarget)
                    continue;
                pending.append(edge.target);
            }
        }
        return reached;
    };

    QMap<QString, ControlFlowType> flowTypes;
    for (ModuleBase* module : m_modules)
        flowTypes.insert(runtimeModuleName(module), module->flowControlType());

    // 回边必须回到 Loop/While，且源节点只能从该循环的 body 分支到达。
    for (const ModuleConnection& candidate : m_controlEdges) {
        const ControlFlowType targetType = flowTypes.value(candidate.toModuleId, ControlFlowType::Sequential);
        if (candidate.fromModuleId == candidate.toModuleId ||
            (targetType != ControlFlowType::Loop && targetType != ControlFlowType::While)) {
            continue;
        }

        QStringList bodyStarts;
        QStringList doneStarts;
        for (const ModuleConnection& edge : m_controlEdges) {
            if (edge.fromModuleId != candidate.toModuleId)
                continue;
            if (edge.fromPort == QLatin1String("body"))
                bodyStarts.append(edge.toModuleId);
            else if (edge.fromPort == QLatin1String("done"))
                doneStarts.append(edge.toModuleId);
        }
        if (reachable(bodyStarts, candidate.toModuleId).contains(candidate.fromModuleId) &&
            !reachable(doneStarts, candidate.toModuleId).contains(candidate.fromModuleId)) {
            m_loopBackEdges.insert(connectionKey(candidate));
        }
    }

    QMap<QString, QStringList> reducedAdj;
    QMap<QString, QStringList> reducedReverse;
    QMap<QString, int> combinedIndegree;
    for (const QString& moduleId : moduleIds)
        combinedIndegree.insert(moduleId, 0);
    for (const ModuleConnection& connection : m_connections) {
        if (m_loopBackEdges.contains(connectionKey(connection)))
            continue;
        reducedAdj[connection.fromModuleId].append(connection.toModuleId);
        reducedReverse[connection.toModuleId].append(connection.fromModuleId);
        ++combinedIndegree[connection.toModuleId];
    }

    ready.clear();
    for (const QString& moduleId : moduleIds) {
        if (combinedIndegree.value(moduleId) == 0)
            ready.append(moduleId);
    }
    int visitedCount = 0;
    while (!ready.isEmpty()) {
        const QString current = ready.takeFirst();
        ++visitedCount;
        for (const QString& downstream : reducedAdj.value(current)) {
            if (--combinedIndegree[downstream] == 0)
                ready.append(downstream);
        }
    }
    if (visitedCount != moduleIds.size()) {
        error = tr("Control or combined graph contains an unstructured cycle");
        return false;
    }

    const auto reachableIn = [](const QMap<QString, QStringList>& graph, QStringList pending) {
        QSet<QString> reached;
        while (!pending.isEmpty()) {
            const QString current = pending.takeFirst();
            if (reached.contains(current))
                continue;
            reached.insert(current);
            pending.append(graph.value(current));
        }
        return reached;
    };
    for (const ModuleConnection& backEdge : m_controlEdges) {
        if (!m_loopBackEdges.contains(connectionKey(backEdge)))
            continue;
        QStringList bodyStarts;
        for (const ModuleConnection& edge : m_controlEdges) {
            if (edge.fromModuleId == backEdge.toModuleId && edge.fromPort == QLatin1String("body"))
                bodyStarts.append(edge.toModuleId);
        }
        const QSet<QString> bodyReach = reachableIn(reducedAdj, bodyStarts);
        const QSet<QString> reachesBackSource = reachableIn(reducedReverse, {backEdge.fromModuleId});
        m_reentrantModules.insert(backEdge.toModuleId);
        for (const QString& moduleId : bodyReach) {
            if (reachesBackSource.contains(moduleId)) {
                m_reentrantModules.insert(moduleId);
                m_loopRegions[backEdge.toModuleId].insert(moduleId);
            }
        }
    }

    return true;
}

QString RunEngine::findSiblingByFlowType(const QString& currentModule, ControlFlowType targetType) {
    QStringList order = m_executionOrder;
    if (order.isEmpty()) {
        QReadLocker locker(&m_moduleLock);
        for (ModuleBase* module : m_modules) {
            order.append(runtimeModuleName(module));
        }
    }

    const int currentIndex = order.indexOf(currentModule);
    if (currentIndex < 0)
        return QString();

    ModuleBase* current = getModule(currentModule);
    if (!current)
        return QString();
    const ControlFlowType opener = current->flowControlType();
    const ControlFlowType matchingEnd = opener == ControlFlowType::Conditional ? ControlFlowType::ConditionalEnd
                                        : opener == ControlFlowType::Loop      ? ControlFlowType::LoopEnd
                                        : opener == ControlFlowType::While     ? ControlFlowType::WhileEnd
                                                                               : ControlFlowType::Sequential;
    int depth = 0;
    for (int i = currentIndex + 1; i < order.size(); ++i) {
        ModuleBase* candidate = getModule(order[i]);
        if (!candidate)
            continue;
        const ControlFlowType type = candidate->flowControlType();
        if (matchingEnd != ControlFlowType::Sequential && type == opener) {
            ++depth;
            continue;
        }
        if (matchingEnd != ControlFlowType::Sequential && type == matchingEnd) {
            if (depth == 0) {
                return type == targetType ? order[i] : QString();
            }
            --depth;
            continue;
        }
        if (depth == 0 && type == targetType) {
            return order[i];
        }
    }
    return QString();
}

QString RunEngine::findPreviousByFlowType(const QString& currentModule, ControlFlowType targetType) {
    QStringList order = m_executionOrder;
    if (order.isEmpty()) {
        QReadLocker locker(&m_moduleLock);
        for (ModuleBase* module : m_modules) {
            order.append(runtimeModuleName(module));
        }
    }

    const int currentIndex = order.indexOf(currentModule);
    if (currentIndex < 0)
        return QString();

    ModuleBase* current = getModule(currentModule);
    if (!current)
        return QString();
    const ControlFlowType closer = current->flowControlType();
    int depth = 0;
    for (int i = currentIndex - 1; i >= 0; --i) {
        ModuleBase* candidate = getModule(order[i]);
        if (!candidate)
            continue;
        const ControlFlowType type = candidate->flowControlType();
        if (type == closer) {
            ++depth;
        } else if (type == targetType) {
            if (depth == 0) {
                return order[i];
            }
            --depth;
        }
    }
    return QString();
}

void RunEngine::buildModuleTree() {
    clearModuleTree();
    m_rootNode = new ModuleTreeNode("Root");

    for (ModuleBase* module : m_modules) {
        QString name = runtimeModuleName(module);
        ModuleTreeNode* node = new ModuleTreeNode(name);
        m_moduleTreeNodes[name] = node;

        ControlFlowType flowType = module->flowControlType();

        // 流程入口类型：压栈
        if (flowType == ControlFlowType::Loop || flowType == ControlFlowType::While ||
            flowType == ControlFlowType::Conditional) {
            if (!m_nodeStack.isEmpty()) {
                node->parent = m_nodeStack.top();
                m_nodeStack.top()->children.append(node);
            } else {
                node->parent = m_rootNode;
                m_rootNode->children.append(node);
            }
            m_nodeStack.push(node);
        }
        // 流程出口类型：出栈
        else if (flowType == ControlFlowType::LoopEnd || flowType == ControlFlowType::WhileEnd ||
                 flowType == ControlFlowType::ConditionalEnd) {
            if (!m_nodeStack.isEmpty()) {
                m_nodeStack.pop();
            }
            if (!m_nodeStack.isEmpty()) {
                node->parent = m_nodeStack.top();
                m_nodeStack.top()->children.append(node);
            } else {
                node->parent = m_rootNode;
                m_rootNode->children.append(node);
            }
        } else {
            if (!m_nodeStack.isEmpty()) {
                node->parent = m_nodeStack.top();
                m_nodeStack.top()->children.append(node);
            } else {
                node->parent = m_rootNode;
                m_rootNode->children.append(node);
            }
        }
    }
}

void RunEngine::clearModuleTree() {
    qDeleteAll(m_moduleTreeNodes);
    m_moduleTreeNodes.clear();
    delete m_rootNode;
    m_rootNode = nullptr;
    m_nodeStack.clear();
    m_loopIndices.clear();
}

void RunEngine::setBreakpoint(const QString& moduleName, bool enabled) {
    QMutexLocker locker(&m_breakpointMutex);
    if (enabled) {
        m_breakpoints.insert(moduleName);
    } else {
        m_breakpoints.remove(moduleName);
    }
}

bool RunEngine::hasBreakpoint(const QString& moduleName) const {
    QMutexLocker locker(&m_breakpointMutex);
    return m_breakpoints.contains(moduleName);
}

void RunEngine::setModuleDisabled(const QString& moduleName, bool disabled) {
    QWriteLocker locker(&m_moduleLock);
    if (disabled) {
        m_disabledModules.insert(moduleName);
    } else {
        m_disabledModules.remove(moduleName);
    }
}

bool RunEngine::isModuleDisabled(const QString& moduleName) const {
    QReadLocker locker(&m_moduleLock);
    return m_disabledModules.contains(moduleName);
}

void RunEngine::updateStatistics(bool success, int elapsedMs) {
    QMutexLocker locker(&m_statsMutex);
    m_totalRuns++;
    m_lastElapsedMs = elapsedMs;

    if (success) {
        m_successRuns++;
    } else {
        m_failedRuns++;
    }
}

void RunEngine::reset() {
    {
        QMutexLocker locker(&m_statsMutex);
        m_totalRuns = 0;
        m_successRuns = 0;
        m_failedRuns = 0;
        m_lastElapsedMs = 0;
    }
    m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    clearOutputs();
    resetStepState();
}

} // namespace DeepLux
