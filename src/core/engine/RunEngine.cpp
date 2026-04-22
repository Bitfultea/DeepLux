#include "RunEngine.h"
#include "base/ModuleBase.h"
#include "common/Logger.h"
#include "common/CancellationToken.h"
#include <QDebug>
#include <QMutexLocker>

namespace DeepLux {

RunEngine& RunEngine::instance()
{
    static RunEngine instance;
    return instance;
}

RunEngine::RunEngine()
    : QObject(nullptr)
    , m_cycleTimer(new QTimer(this))
    , m_cancellationToken(new CancellationToken(this))
{
    m_cycleTimer->setInterval(100);
    connect(m_cycleTimer, &QTimer::timeout, this, &RunEngine::onTimerTick);
    Logger::instance().info("Run engine initialized", "Run");
}

RunEngine::~RunEngine()
{
    stop();
    clearModuleTree();
    clearModules();
}

void RunEngine::setCycleMode(bool enabled)
{
    if (enabled) {
        m_runMode = RunMode::RunCycle;
    } else {
        m_runMode = RunMode::None;
    }
}

void RunEngine::runOnce()
{
    if (m_state == RunState::Running) {
        return;
    }

    m_runMode = RunMode::RunOnce;
    Logger::instance().info(tr("Starting single run"), "Run");
    executeRun();
}

void RunEngine::start()
{
    if (m_state == RunState::Running) {
        return;
    }

    m_runMode = RunMode::RunCycle;
    m_state = RunState::Running;
    emit stateChanged(m_state);
    emit cycleStarted();

    Logger::instance().info(tr("Starting continuous run"), "Run");
    m_cycleTimer->start();
}

void RunEngine::pause()
{
    if (m_state != RunState::Running) {
        return;
    }

    m_state = RunState::Paused;
    m_cycleTimer->stop();
    emit stateChanged(m_state);

    Logger::instance().info(tr("Run paused"), "Run");
}

void RunEngine::resume()
{
    if (m_state != RunState::Paused) {
        return;
    }

    m_state = RunState::Running;
    m_cycleTimer->start();
    emit stateChanged(m_state);

    Logger::instance().info(tr("Run resumed"), "Run");
}

void RunEngine::stop()
{
    m_cycleTimer->stop();
    m_state = RunState::Stopped;
    m_runMode = RunMode::None;
    m_breakpointFlag = false;
    m_continueFlag = false;
    m_breakpointCondition.wakeOne();
    if (m_cancellationToken) {
        m_cancellationToken->cancel();
    }

    emit stateChanged(m_state);
    emit cycleStopped();

    Logger::instance().info(tr("Run stopped"), "Run");
}

void RunEngine::requestCancellation()
{
    if (m_cancellationToken) {
        m_cancellationToken->cancel();
    }
    stop();
}

void RunEngine::addModule(ModuleBase* module)
{
    if (module && !m_modules.contains(module)) {
        m_modules.append(module);
        m_moduleMap[module->name()] = module;
        Logger::instance().debug(QString("Module added to engine: %1").arg(module->name()), "Run");
    }
}

void RunEngine::removeModule(const QString& moduleId)
{
    for (int i = 0; i < m_modules.size(); ++i) {
        if (m_modules[i]->id() == moduleId) {
            m_moduleMap.remove(m_modules[i]->name());
            m_modules.removeAt(i);
            break;
        }
    }
}

void RunEngine::clearModules()
{
    m_modules.clear();
    m_moduleMap.clear();
    clearOutputs();
}

ModuleBase* RunEngine::getModule(const QString& moduleName) const
{
    return m_moduleMap.value(moduleName, nullptr);
}

int RunEngine::getModuleIndex(const QString& moduleName) const
{
    for (int i = 0; i < m_modules.size(); ++i) {
        if (m_modules[i]->name() == moduleName) {
            return i;
        }
    }
    return -1;
}

void RunEngine::setOutput(const QString& moduleName, const QString& varName, const QVariant& value)
{
    m_outputMap[moduleName][varName] = value;
    emit outputChanged(moduleName, varName, value);
}

QVariant RunEngine::getOutput(const QString& moduleName, const QString& varName) const
{
    if (m_outputMap.contains(moduleName) && m_outputMap[moduleName].contains(varName)) {
        return m_outputMap[moduleName][varName];
    }
    return QVariant();
}

bool RunEngine::hasOutput(const QString& moduleName, const QString& varName) const
{
    return m_outputMap.contains(moduleName) && m_outputMap[moduleName].contains(varName);
}

void RunEngine::clearOutputs()
{
    m_outputMap.clear();
}

void RunEngine::onTimerTick()
{
    if (m_state == RunState::Running && m_runMode == RunMode::RunCycle) {
        executeRun();
    }
}

void RunEngine::executeRun()
{
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

    m_state = RunState::Running;
    emit stateChanged(m_state);
    emit runStarted();

    m_runStartTime = QDateTime::currentDateTime();

    // Reset cancellation token for this run cycle
    if (m_cancellationToken) {
        m_cancellationToken->reset();
    }

    // 构建模块树
    buildModuleTree();

    // 从第一个模块开始执行，维护流水线数据
    QString currentModule = m_modules.first()->name();
    m_currentModuleName = currentModule;
    ImageData pipelineData;

    while (!currentModule.isEmpty() && m_state == RunState::Running) {
        // 检查是否已请求取消
        if (m_cancellationToken && m_cancellationToken->isCancelledFast()) {
            stop();
            break;
        }

        // 检查是否需要暂停
        {
            QMutexLocker locker(&m_breakpointMutex);
            if (m_breakpointFlag && !m_continueFlag) {
                locker.unlock();
                m_breakpointCondition.wait(&m_breakpointMutex);
            }
        }

        // 执行当前模块，传递流水线数据
        executeModule(currentModule, pipelineData);

        // 获取下一个模块
        currentModule = getNextModule(currentModule, m_lastExecuteResult);
        m_currentModuleName = currentModule;
    }

    int elapsedMs = m_runStartTime.msecsTo(QDateTime::currentDateTime());
    bool allSuccess = (m_state != RunState::Stopped);
    updateStatistics(allSuccess, elapsedMs);

    RunResult result;
    result.success = allSuccess;
    result.errorCode = allSuccess ? 0 : -1;
    result.errorMessage = QString();
    result.elapsedMs = elapsedMs;
    result.finishedTime = QDateTime::currentDateTime();

    emit runFinished(result);

    // 单次运行后重置状态
    if (m_runMode == RunMode::RunOnce) {
        m_state = RunState::Idle;
        emit stateChanged(m_state);
    }
}

void RunEngine::executeModule(const QString& moduleName, ImageData& pipelineData)
{
    ModuleBase* module = getModule(moduleName);
    if (!module) {
        Logger::instance().error(QString("Module not found: %1").arg(moduleName), "Run");
        m_lastExecuteResult = false;
        return;
    }

    emit moduleStarted(moduleName);

    // 处理循环索引
    if (isLoopStart(moduleName)) {
        if (!m_loopIndices.contains(moduleName)) {
            m_loopIndices[moduleName] = 0;
        } else {
            m_loopIndices[moduleName]++;
        }
    }

    // 执行模块，将上一个模块的输出作为当前模块的输入
    ImageData output;
    bool success = module->execute(pipelineData, output);

    // 如果执行成功，将当前输出传递为下一个模块的输入
    if (success) {
        pipelineData = output;
    }

    m_lastExecuteResult = success;

    emit moduleFinished(moduleName, success);

    if (!success) {
        Logger::instance().error(QString("Module execution failed: %1").arg(moduleName), "Run");
    }
}

QString RunEngine::getNextModule(const QString& currentModule, bool lastResult)
{
    // 逻辑模块处理
    if (currentModule.startsWith("如果") || currentModule.startsWith("条件分支")) {
        if (!lastResult) {
            // 条件为false，跳转到下一个 否则如果/否则/结束
            return findSiblingModule(currentModule, "否则如果");
        }
    }
    else if (currentModule.startsWith("否则如果")) {
        if (!lastResult) {
            return findSiblingModule(currentModule, "否则如果");
        }
    }
    else if (currentModule.startsWith("否则")) {
        // 执行完否则后，跳转到结束
        return findSiblingModule(currentModule, "结束");
    }
    else if (currentModule.startsWith("结束") || currentModule.startsWith("条件分支结束")) {
        // 条件分支结束，返回父级流程
        if (m_nodeStack.isEmpty()) {
            return getNextSequentialModule(currentModule);
        }
    }
    else if (isLoopStart(currentModule)) {
        ModuleBase* module = getModule(currentModule);
        int loopCount = module ? module->currentParams().value("loopCount").toInt(10) : 10;
        if (module && m_loopIndices[currentModule] < loopCount) {
            if (lastResult) {
                return findSiblingModule(currentModule, "循环结束");
            }
        }
        m_loopIndices.remove(currentModule);
    }
    else if (isLoopEnd(currentModule)) {
        // 循环结束，跳回循环开始
        return findSiblingModule(currentModule, "循环开始");
    }
    else if (currentModule.startsWith("停止循环")) {
        if (lastResult) {
            // 跳转到循环结束
            return findSiblingModule(currentModule, "循环结束");
        }
    }

    return getNextSequentialModule(currentModule);
}

QString RunEngine::getNextSequentialModule(const QString& currentModule)
{
    int index = getModuleIndex(currentModule);
    if (index >= 0 && index < m_modules.size() - 1) {
        return m_modules[index + 1]->name();
    }
    return QString();
}

QString RunEngine::findSiblingModule(const QString& currentModule, const QString& suffix)
{
    int currentIndex = getModuleIndex(currentModule);
    if (currentIndex < 0) return QString();

    for (int i = currentIndex + 1; i < m_modules.size(); ++i) {
        QString name = m_modules[i]->name();
        if (name.startsWith(suffix)) {
            return name;
        }
    }
    return QString();
}

bool RunEngine::isLogicModule(const QString& moduleName) const
{
    return moduleName.startsWith("如果") ||
           moduleName.startsWith("否则") ||
           moduleName.startsWith("条件分支") ||
           moduleName.startsWith("结束");
}

bool RunEngine::isLoopStart(const QString& moduleName) const
{
    return moduleName.startsWith("循环开始");
}

bool RunEngine::isLoopEnd(const QString& moduleName) const
{
    return moduleName.startsWith("循环结束");
}

bool RunEngine::isIfBranch(const QString& moduleName) const
{
    return moduleName.startsWith("如果");
}

bool RunEngine::isIfEnd(const QString& moduleName) const
{
    return moduleName.startsWith("结束") || moduleName.startsWith("条件分支结束");
}

bool RunEngine::isElseIf(const QString& moduleName) const
{
    return moduleName.startsWith("否则如果");
}

bool RunEngine::isElse(const QString& moduleName) const
{
    return moduleName.startsWith("否则");
}

void RunEngine::buildModuleTree()
{
    clearModuleTree();
    m_rootNode = new ModuleTreeNode("Root");

    for (ModuleBase* module : m_modules) {
        QString name = module->name();
        ModuleTreeNode* node = new ModuleTreeNode(name);
        m_moduleTreeNodes[name] = node;

        // 处理嵌套逻辑
        if (isLoopStart(name) || isIfBranch(name)) {
            if (!m_nodeStack.isEmpty()) {
                node->parent = m_nodeStack.top();
                m_nodeStack.top()->children.append(node);
            } else {
                node->parent = m_rootNode;
                m_rootNode->children.append(node);
            }
            m_nodeStack.push(node);
        }
        else if (isLoopEnd(name) || isIfEnd(name)) {
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
        }
        else {
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

void RunEngine::clearModuleTree()
{
    qDeleteAll(m_moduleTreeNodes);
    m_moduleTreeNodes.clear();
    delete m_rootNode;
    m_rootNode = nullptr;
    m_nodeStack.clear();
    m_loopIndices.clear();
}

void RunEngine::setBreakpoint(const QString& moduleName, bool enabled)
{
    if (enabled) {
        m_breakpoints.insert(moduleName);
    } else {
        m_breakpoints.remove(moduleName);
    }
}

bool RunEngine::hasBreakpoint(const QString& moduleName) const
{
    return m_breakpoints.contains(moduleName);
}

void RunEngine::onBreakpointHit()
{
    QMutexLocker locker(&m_breakpointMutex);
    m_breakpointFlag = true;
    m_breakpointCondition.wakeOne();
}

void RunEngine::updateStatistics(bool success, int elapsedMs)
{
    m_totalRuns++;
    m_lastElapsedMs = elapsedMs;

    if (success) {
        m_successRuns++;
    } else {
        m_failedRuns++;
    }
}

void RunEngine::reset()
{
    m_totalRuns = 0;
    m_successRuns = 0;
    m_failedRuns = 0;
    m_lastElapsedMs = 0;
    m_state = RunState::Idle;
    clearOutputs();
    m_loopIndices.clear();
}

} // namespace DeepLux