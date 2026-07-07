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
    if (state() == RunState::Running) {
        return;
    }

    m_runMode.store(static_cast<int>(RunMode::RunOnce), std::memory_order_release);
    Logger::instance().info(tr("Starting single run"), "Run");
    executeRun();
}

void RunEngine::start() {
    if (state() == RunState::Running) {
        return;
    }

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
    QWriteLocker locker(&m_moduleLock);
    if (module && !m_modules.contains(module)) {
        m_modules.append(module);
        QString key = runtimeModuleName(module);
        m_moduleMap[key] = module;
        Logger::instance().debug(QString("Module added to engine: %1").arg(key), "Run");
    }
}

bool RunEngine::loadProject(Project* project, ModuleFactory factory) {
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
    QWriteLocker locker(&m_moduleLock);
    qDeleteAll(m_ownedModules);
    m_ownedModules.clear();
    m_modules.clear();
    m_moduleMap.clear();
    m_executionOrder.clear();
    locker.unlock();
    clearOutputs();
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
    QMutexLocker locker(&m_outputMutex);
    m_outputMap.clear();
}

ImageData RunEngine::lastOutput() const {
    QMutexLocker locker(&m_lastOutputMutex);
    return m_lastOutput;
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
            emit runFinished(result);
            return;
        }
    }

    m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    emit stateChanged(state());
    emit runStarted();

    m_runStartTime = QDateTime::currentDateTime();

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

        currentModule = getNextModule(currentModule, m_lastExecuteResult);
        m_currentModuleName = currentModule;
    }

    int elapsedMs = m_runStartTime.msecsTo(QDateTime::currentDateTime());
    bool allSuccess = (state() != RunState::Stopped);
    updateStatistics(allSuccess, elapsedMs);

    RunResult result;
    result.success = allSuccess;
    result.errorCode = allSuccess ? 0 : -1;
    result.errorMessage = QString();
    result.elapsedMs = elapsedMs;
    result.finishedTime = QDateTime::currentDateTime();

    emit runFinished(result);

    if (runMode() == RunMode::RunOnce) {
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
        emit stateChanged(state());
    }
}

void RunEngine::executeModule(const QString& moduleName, ImageData& pipelineData) {
    ModuleBase* module = getModule(moduleName);
    if (!module) {
        Logger::instance().error(QString("Module not found: %1").arg(moduleName), "Run");
        m_lastExecuteResult = false;
        return;
    }

    emit moduleStarted(moduleName);

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
    QElapsedTimer moduleTimer;
    moduleTimer.start();
    bool success = module->execute(pipelineData, output);
    const qint64 elapsedMs = moduleTimer.elapsed();
    Logger::instance().info(QString("Module finished: %1 success=%2 elapsed=%3 ms")
                                .arg(moduleName)
                                .arg(success ? "true" : "false")
                                .arg(elapsedMs),
                            "Run");

    // 如果执行成功，将当前输出传递为下一个模块的输入
    if (success) {
        pipelineData = output;
        {
            QMutexLocker locker(&m_lastOutputMutex);
            m_lastOutput = output;
        }
    }

    m_lastExecuteResult = success;

    emit moduleFinished(moduleName, success);

    if (!success) {
        Logger::instance().error(QString("Module execution failed: %1").arg(moduleName), "Run");
    }
}

QString RunEngine::getNextModule(const QString& currentModule, bool lastResult) {
    ModuleBase* current = getModule(currentModule);
    if (!current)
        return QString();

    ControlFlowType flowType = current->flowControlType();

    switch (flowType) {
    case ControlFlowType::Conditional:
        // 条件为 false 时跳过整个分支（找 ConditionalElse 或 ConditionalEnd）
        if (!lastResult) {
            return findSiblingByFlowType(currentModule, ControlFlowType::ConditionalElse);
        }
        break;

    case ControlFlowType::ConditionalElse:
        if (!lastResult) {
            // 继续找下一个 ConditionalElse 或 ConditionalEnd
            return findSiblingByFlowType(currentModule, ControlFlowType::ConditionalElse);
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
        if (m_loopIndices[currentModule] < loopCount) {
            if (lastResult) {
                // 查找对应的 LoopEnd
                return findSiblingByFlowType(currentModule, ControlFlowType::LoopEnd);
            }
        }
        m_loopIndices.remove(currentModule);
        break;
    }

    case ControlFlowType::LoopEnd:
        // 循环结束，跳回对应的 Loop 入口
        return findPreviousByFlowType(currentModule, ControlFlowType::Loop);

    case ControlFlowType::StopLoop:
        if (lastResult) {
            return findSiblingByFlowType(currentModule, ControlFlowType::LoopEnd);
        }
        break;

    case ControlFlowType::While: {
        int maxIter = current->currentParams().value("maxIterations").toInt(100);
        if (lastResult && m_loopIndices[currentModule] < maxIter) {
            // 查找对应的 WhileEnd
            return findSiblingByFlowType(currentModule, ControlFlowType::WhileEnd);
        }
        m_loopIndices.remove(currentModule);
        break;
    }

    case ControlFlowType::WhileEnd:
        // While 循环结束，跳回对应的 While 入口
        return findPreviousByFlowType(currentModule, ControlFlowType::While);

    case ControlFlowType::Sequential:
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

    QMap<QString, QStringList> adjacency;
    QMap<QString, int> indegree;
    for (const QString& id : moduleIds) {
        indegree[id] = 0;
    }

    for (const ModuleConnection& conn : project->connections()) {
        if (!moduleIdSet.contains(conn.fromModuleId)) {
            error = tr("Connection references missing source module: %1").arg(conn.fromModuleId);
            return false;
        }
        if (!moduleIdSet.contains(conn.toModuleId)) {
            error = tr("Connection references missing target module: %1").arg(conn.toModuleId);
            return false;
        }
        if (!adjacency[conn.fromModuleId].contains(conn.toModuleId)) {
            adjacency[conn.fromModuleId].append(conn.toModuleId);
            indegree[conn.toModuleId]++;
        }
    }

    QStringList ready;
    for (const QString& id : moduleIds) {
        if (indegree.value(id) == 0) {
            ready.append(id);
        }
    }

    while (!ready.isEmpty()) {
        const QString id = ready.takeFirst();
        m_executionOrder.append(id);
        for (const QString& next : adjacency.value(id)) {
            indegree[next]--;
            if (indegree[next] == 0) {
                ready.append(next);
            }
        }
    }

    if (m_executionOrder.size() != moduleIds.size()) {
        m_executionOrder.clear();
        error = tr("Project module connections contain a cycle");
        return false;
    }

    return true;
}

QString RunEngine::findSiblingByFlowType(const QString& currentModule, ControlFlowType targetType) {
    int currentIndex = getModuleIndex(currentModule);
    if (currentIndex < 0)
        return QString();

    for (int i = currentIndex + 1; i < m_modules.size(); ++i) {
        if (m_modules[i]->flowControlType() == targetType) {
            return runtimeModuleName(m_modules[i]);
        }
    }
    return QString();
}

QString RunEngine::findPreviousByFlowType(const QString& currentModule, ControlFlowType targetType) {
    int currentIndex = getModuleIndex(currentModule);
    if (currentIndex < 0)
        return QString();

    for (int i = currentIndex - 1; i >= 0; --i) {
        if (m_modules[i]->flowControlType() == targetType) {
            return runtimeModuleName(m_modules[i]);
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
    m_loopIndices.clear();
}

} // namespace DeepLux
