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

namespace DeepLux {

namespace {
QString runtimeModuleName(ModuleBase* module) {
    if (!module) {
        return QString();
    }
    return module->instanceName().isEmpty() ? module->name() : module->instanceName();
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
    Logger::instance().info("Run engine initialized", "Run");
}

RunEngine::~RunEngine() {
    stop();
    clearModuleTree();
    clearModules();
}

void RunEngine::setCycleMode(bool enabled) {
    m_runMode.store(static_cast<int>(enabled ? RunMode::RunCycle : RunMode::None), std::memory_order_release);
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
        buildModuleTree();
        QReadLocker locker(&m_moduleLock);
        m_stepCurrentModuleName =
            m_executionOrder.isEmpty() ? runtimeModuleName(m_modules.first()) : m_executionOrder.first();
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
    m_currentModuleName = moduleToRun;
    executeModule(moduleToRun, m_stepPipelineData);

    const bool success = m_lastExecuteResult;
    if (success) {
        m_stepCurrentModuleName = getNextModule(moduleToRun, m_lastControlResult);
        m_currentModuleName = m_stepCurrentModuleName;
        if (m_stepCurrentModuleName.isEmpty()) {
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
    {
        QMutexLocker locker(&m_breakpointMutex);
        m_breakpointFlag = false;
        m_continueFlag = false;
        m_breakpointCondition.wakeAll();
    }
    if (m_cancellationToken) {
        m_cancellationToken->cancel();
    }
    resetStepState();

    emit stateChanged(state());
    emit cycleStopped();

    Logger::instance().info(tr("Run stopped"), "Run");
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
    QWriteLocker locker(&m_moduleLock);
    for (int i = 0; i < m_modules.size(); ++i) {
        ModuleBase* module = m_modules[i];
        if (module->id() == moduleId || runtimeModuleName(module) == moduleId) {
            m_moduleMap.remove(runtimeModuleName(module));
            m_modules.removeAt(i);
            if (m_ownedModules.removeOne(module)) {
                delete module;
            }
            break;
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
    locker.unlock();
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

    if (m_executing.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    emit stateChanged(state());
    emit runStarted();

    m_runStartTime = QDateTime::currentDateTime();
    clearModuleOutputs();

    if (m_cancellationToken) {
        m_cancellationToken->reset();
    }

    buildModuleTree();

    QString currentModule;
    {
        QReadLocker locker(&m_moduleLock);
        currentModule = m_executionOrder.isEmpty() ? runtimeModuleName(m_modules.first()) : m_executionOrder.first();
    }
    m_currentModuleName = currentModule;
    ImageData pipelineData;
    bool allSuccess = true;
    QString firstError;

    while (!currentModule.isEmpty() && state() == RunState::Running) {
        if (m_cancellationToken && m_cancellationToken->isCancelledFast()) {
            stop();
            break;
        }

        {
            QMutexLocker locker(&m_breakpointMutex);
            while (m_breakpointFlag && !m_continueFlag && state() == RunState::Running) {
                m_breakpointCondition.wait(&m_breakpointMutex);
            }
            m_continueFlag = false;
        }

        executeModule(currentModule, pipelineData);
        if (!m_lastExecuteResult) {
            allSuccess = false;
            if (firstError.isEmpty()) {
                firstError = m_lastModuleError;
            }
        }

        currentModule = getNextModule(currentModule, m_lastControlResult);
        m_currentModuleName = currentModule;
    }

    int elapsedMs = m_runStartTime.msecsTo(QDateTime::currentDateTime());
    allSuccess = allSuccess && state() != RunState::Stopped;
    updateStatistics(allSuccess, elapsedMs);

    RunResult result;
    result.success = allSuccess;
    result.errorCode = allSuccess ? 0 : -1;
    result.errorMessage = allSuccess ? QString() : (firstError.isEmpty() ? tr("Flow execution failed") : firstError);
    result.elapsedMs = elapsedMs;
    result.finishedTime = QDateTime::currentDateTime();

    m_executing.store(false, std::memory_order_release);
    emit runFinished(result);

    if (runMode() == RunMode::RunOnce) {
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
        emit stateChanged(state());
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
    bool success = module->execute(pipelineData, output);
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

    QMap<QString, QString> adjacency;
    QMap<QString, int> indegree;
    for (const QString& id : moduleIds) {
        indegree[id] = 0;
    }
    QSet<QString> connectedModules;

    for (const ModuleConnection& conn : project->connections()) {
        if (!moduleIdSet.contains(conn.fromModuleId)) {
            error = tr("Connection references missing source module: %1").arg(conn.fromModuleId);
            return false;
        }
        if (!moduleIdSet.contains(conn.toModuleId)) {
            error = tr("Connection references missing target module: %1").arg(conn.toModuleId);
            return false;
        }
        if (adjacency.contains(conn.fromModuleId) && adjacency.value(conn.fromModuleId) != conn.toModuleId) {
            error = tr("Flow branches are not supported yet: %1 has multiple outputs").arg(conn.fromModuleId);
            return false;
        }
        if (indegree.value(conn.toModuleId) > 0 && adjacency.value(conn.fromModuleId) != conn.toModuleId) {
            error = tr("Flow merges are not supported yet: %1 has multiple inputs").arg(conn.toModuleId);
            return false;
        }
        if (adjacency.value(conn.fromModuleId) != conn.toModuleId) {
            adjacency[conn.fromModuleId] = conn.toModuleId;
            indegree[conn.toModuleId]++;
        }
        connectedModules.insert(conn.fromModuleId);
        connectedModules.insert(conn.toModuleId);
    }

    QStringList ready;
    for (const QString& id : connectedModules) {
        if (indegree.value(id) == 0) {
            ready.append(id);
        }
    }

    if (ready.size() != 1) {
        error = tr("Connected modules must form one linear flow");
        return false;
    }

    QSet<QString> visited;
    QString current = ready.first();
    while (!current.isEmpty() && !visited.contains(current)) {
        visited.insert(current);
        m_executionOrder.append(current);
        current = adjacency.value(current);
    }

    if (!current.isEmpty() || visited != connectedModules) {
        m_executionOrder.clear();
        error = tr("Connected modules contain a cycle or disconnected chains");
        return false;
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

void RunEngine::onBreakpointHit() {
    QMutexLocker locker(&m_breakpointMutex);
    m_breakpointFlag = true;
    m_breakpointCondition.wakeOne();
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
