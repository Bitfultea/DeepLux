#include "RunEngine.h"

#include "base/ModuleBase.h"
#include "common/CancellationToken.h"
#include "common/Logger.h"
#include "interface/IModule.h"
#include "manager/GlobalVarManager.h"
#include "manager/PluginManager.h"
#include "model/Project.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QThread>
#include <QWaitCondition>
#include <algorithm>
#include <functional>
#include <exception>
#include <limits>

namespace DeepLux {

namespace {
QString runtimeModuleName(ModuleBase* module) {
    if (!module) {
        return QString();
    }
    return module->instanceName().isEmpty() ? module->name() : module->instanceName();
}

QString runtimePluginId(ModuleBase* module) {
    if (!module)
        return QString();
    const QVariant declaredPluginId = module->property("deeplux.pluginId");
    return declaredPluginId.isValid() ? declaredPluginId.toString() : module->moduleId();
}

bool authorizationRequiresFreshResume(const QJsonObject& authorization) {
    if (!authorization.value(QStringLiteral("fileOutputs")).toObject().isEmpty())
        return true;
    const QJsonObject folders = authorization.value(QStringLiteral("folders")).toObject();
    for (auto it = folders.begin(); it != folders.end(); ++it) {
        const QString operation = it.value().toObject().value(QStringLiteral("operation")).toString();
        if (operation == QLatin1String("Create") || operation == QLatin1String("Delete"))
            return true;
    }
    return false;
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

// 后台执行（设计 §6.1）：未注册/测试模块默认 gui；已注册模块读 metadata 解析结果。
ExecutionAffinity affinityForModule(ModuleBase* module) {
    if (!module) {
        return ExecutionAffinity::Gui;
    }
    const PluginInfo info = PluginManager::instance().pluginInfo(module->moduleId());
    return info.executionAffinity;
}

QString connectionKey(const ModuleConnection& connection) {
    return connection.fromModuleId + QLatin1Char('\x1f') + connection.toModuleId + QLatin1Char('\x1f') +
           connection.fromPort + QLatin1Char('\x1f') + connection.toPort;
}

QByteArray projectDigest(const Project* project) {
    return project ? QCryptographicHash::hash(QJsonDocument(project->toJson()).toJson(QJsonDocument::Compact),
                                              QCryptographicHash::Sha1)
                   : QByteArray();
}
} // namespace

RunEngine& RunEngine::instance() {
    static RunEngine instance;
    return instance;
}

RunEngine::RunEngine() : QObject(nullptr), m_cancellationToken(new CancellationToken(this)) {
    qRegisterMetaType<RunResult>("RunResult");
    qRegisterMetaType<RunResult>("DeepLux::RunResult");
    connect(
        this, &RunEngine::moduleStarted, this,
        [this](const QString& moduleId) {
            QMutexLocker locker(&m_runObservationMutex);
            if (!m_runObservation.finished)
                m_runObservation.currentModule = moduleId;
        },
        Qt::DirectConnection);
    connect(
        this, &RunEngine::moduleSkipped, this,
        [this](const QString& moduleId) {
            {
                QMutexLocker locker(&m_runObservationMutex);
                m_runPortOutputs.remove(moduleId);
            }
            invalidateModuleOutput(moduleId);
        },
        Qt::DirectConnection);
    connect(
        this, &RunEngine::stateChanged, this,
        [this](RunState runState) {
            QMutexLocker locker(&m_runObservationMutex);
            if (!m_runObservation.runId.isEmpty() && !m_runObservation.finished && runState != RunState::Idle)
                m_runObservation.state = runState;
            if (!m_cycleSessionObservation.runId.isEmpty() && !m_cycleSessionObservation.finished) {
                m_cycleSessionObservation.state = runState;
                if (runState == RunState::Stopped) {
                    m_cycleSessionObservation.finished = true;
                    m_cycleSessionObservation.result.success = false;
                    m_cycleSessionObservation.result.errorCode = ExecError::Cancelled;
                    m_cycleSessionObservation.result.finishedTime = QDateTime::currentDateTime();
                }
            }
        },
        Qt::DirectConnection);
    connect(
        this, &RunEngine::runFinished, this, [this](const RunResult&) { clearActiveAgentFolderDeleteAuthorization(); },
        Qt::DirectConnection);
    connect(
        this, &RunEngine::runFinished, this,
        [this](const RunResult& result) {
            if (result.runId.isEmpty())
                return;
            QMutexLocker locker(&m_runObservationMutex);
            RunObservation finished =
                m_runObservation.runId == result.runId
                    ? m_runObservation
                    : (m_lastFinishedRun.runId == result.runId ? m_lastFinishedRun : RunObservation{});
            if (finished.runId.isEmpty())
                return;
            finished.runId = result.runId;
            finished.result = result;
            if (!finished.finished)
                finished.state = state();
            finished.finished = true;
            m_lastFinishedRun = finished;
            if (m_runObservation.runId == result.runId)
                m_runObservation = finished;
            if (m_cycleSessionObservation.runId == finished.sessionId && !m_cycleSessionObservation.finished &&
                state() != RunState::Running && state() != RunState::Paused) {
                m_cycleSessionObservation.finished = true;
                m_cycleSessionObservation.result = result;
                m_cycleSessionObservation.state = state();
            }
        },
        Qt::DirectConnection);
    // 后台执行（设计 §5.3）：PauseReason 在首次跨线程 queued 连接（Service/UI 的 pauseHit）
    // 之前完成注册，兼容 Qt5.15 的非自动注册行为。
    qRegisterMetaType<PauseReason>("PauseReason");
    qRegisterMetaType<PauseReason>("DeepLux::PauseReason");
    // 后台执行（设计 §4.2）：进程内随机盐，使 job token 不可伪造。
    m_jobSalt = QRandomGenerator::global()->generate64();
    m_cycleTimer = new QTimer(this);
    m_cycleTimer->setInterval(100);
    connect(m_cycleTimer, &QTimer::timeout, this, &RunEngine::onTimerTick);
    m_runPool.setMaxThreadCount(1);
    setParallelThreadCount(0);
    Logger::instance().info("Run engine initialized", "Run");
}

RunEngine::~RunEngine() {
    stop();
    m_runPool.waitForDone();
    m_parallelPool.waitForDone();
    releaseVariableExecutionLease();
    clearModuleTree();
    clearModules();
}

void RunEngine::setCycleMode(bool enabled) {
    m_runMode.store(static_cast<int>(enabled ? RunMode::RunCycle : RunMode::None), std::memory_order_release);
}

void RunEngine::clearActiveAgentFolderDeleteAuthorization() {
    QMutexLocker locker(&m_agentFolderDeleteAuthorizationMutex);
    m_activeAgentFolderDeleteAuthorization = {};
}

void RunEngine::clearActiveAgentFolderDeleteAuthorizationForPause() {
    QMutexLocker locker(&m_agentFolderDeleteAuthorizationMutex);
    const QJsonObject authorization = !m_activeAgentFolderDeleteAuthorization.isEmpty()
                                          ? m_activeAgentFolderDeleteAuthorization
                                          : m_cycleAgentFolderDeleteAuthorization;
    if (authorizationRequiresFreshResume(authorization)) {
        m_pausedAgentFolderDeleteConfirmationRequired = true;
        m_pausedAgentFolderDeleteAuthorizationId =
            authorization.value(QStringLiteral("executionAuthorizationId")).toString();
    }
    m_activeAgentFolderDeleteAuthorization = {};
}

QJsonObject RunEngine::folderDeleteAuthorizationForModule(const QString& moduleId) const {
    QMutexLocker locker(&m_agentFolderDeleteAuthorizationMutex);
    if (m_activeAgentFolderDeleteAuthorization.isEmpty())
        return {};

    const QJsonObject folders = m_activeAgentFolderDeleteAuthorization.value(QStringLiteral("folders")).toObject();
    const QJsonObject folder = folders.value(moduleId).toObject();
    QJsonObject authorization = m_activeAgentFolderDeleteAuthorization;
    authorization.remove(QStringLiteral("folders"));
    authorization.insert(QStringLiteral("active"), true);
    authorization.insert(QStringLiteral("authorized"), folders.contains(moduleId));
    authorization.insert(QStringLiteral("authorizedModuleId"), moduleId);
    authorization.insert(QStringLiteral("approvedOperation"), folder.value(QStringLiteral("operation")));
    authorization.insert(QStringLiteral("approvedConfiguredPath"), folder.value(QStringLiteral("configuredPath")));
    authorization.insert(QStringLiteral("targetExistsAtApproval"),
                         folder.value(QStringLiteral("targetExistsAtApproval")));
    authorization.insert(QStringLiteral("resolvedTarget"), folder.value(QStringLiteral("resolvedTarget")));
    return authorization;
}

QJsonObject RunEngine::fileOutputAuthorizationForModule(const QString& moduleId) const {
    QMutexLocker locker(&m_agentFolderDeleteAuthorizationMutex);
    if (m_activeAgentFolderDeleteAuthorization.isEmpty())
        return {};

    const QJsonObject fileOutputs =
        m_activeAgentFolderDeleteAuthorization.value(QStringLiteral("fileOutputs")).toObject();
    const QJsonObject fileOutput = fileOutputs.value(moduleId).toObject();
    QJsonObject authorization = m_activeAgentFolderDeleteAuthorization;
    authorization.remove(QStringLiteral("fileOutputs"));
    authorization.insert(QStringLiteral("active"), true);
    authorization.insert(QStringLiteral("authorized"), fileOutputs.contains(moduleId));
    authorization.insert(QStringLiteral("authorizedModuleId"), moduleId);
    authorization.insert(QStringLiteral("approvedPluginId"), fileOutput.value(QStringLiteral("pluginId")));
    authorization.insert(QStringLiteral("approvedParams"), fileOutput.value(QStringLiteral("params")));
    authorization.insert(QStringLiteral("approvedConfiguredPath"), fileOutput.value(QStringLiteral("configuredPath")));
    authorization.insert(QStringLiteral("resolvedTarget"), fileOutput.value(QStringLiteral("resolvedTarget")));
    return authorization;
}

void RunEngine::stopCycleTimer() {
    QTimer* timer = nullptr;
    {
        QMutexLocker locker(&m_lifecycleMutex);
        timer = m_cycleTimer;
    }
    if (!timer)
        return;
    if (QThread::currentThread() == timer->thread())
        timer->stop();
    else
        QMetaObject::invokeMethod(timer, "stop", Qt::QueuedConnection);
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
    // 阶6 复核（四轮 P0-1）：公开并行原语与执行/维护共享排他租约——取租约期间
    // clearModules/loadProject 被拒绝（无 UAF），且不再无条件重置全局 token 撤销
    // 他人停止；token 重置在租约临界区内（此时无其它运行）。
    if (!tryAcquireLease()) {
        return ExecutionResult::fail(ExecError::Processing, tr("executeParallel: engine busy or stop pending"),
                                     QStringLiteral("lease"));
    }

    // 阶6 复核（三轮）：始终生成自己的局部 runId（不读 m_runId），连续独立调用不共享。
    const QString parallelRunId = QString::number(QDateTime::currentMSecsSinceEpoch()) + QLatin1Char('-') +
                                  QString::number(m_runSeq.fetch_add(1, std::memory_order_relaxed));
    CancellationToken* const parallelToken = m_cancellationToken;

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

    // 后台执行（设计 §6.3）：并行池只允许 worker 亲和模块。GUI 线程可能正同步等待线程池，
    // 池线程统一 BlockingQueuedConnection 回 GUI 会互等死锁；因此进入池前过滤，存在 gui
    // 亲和时直接返回结构化错误，由调用方（如 Parallel 插件）回退串行。
    for (const QString& name : validNames) {
        if (executionAffinityFor(name, getModule(name)) != ExecutionAffinity::Worker) {
            releaseLease();
            return ExecutionResult::fail(ExecError::Processing, tr("gui_affinity_not_parallelizable: %1").arg(name),
                                         QStringLiteral("gui_affinity_not_parallelizable"));
        }
    }

    for (const QString& name : validNames) {
        ModuleBase* mod = getModule(name);
        // 同一实例同刻仅一次：每个任务持有独立输入/输出副本
        // 每任务独立输入/输出副本（in/out 均为局部值）；sharedInput 只读。
        m_parallelPool.start([this, mod, name, parallelRunId, parallelToken, &sharedInput, &running, &maxConcurrent,
                              &errMutex, &haveError, &firstError, &doneMutex, &doneCond, &finished]() {
            const int cur = ++running;
            int expected = maxConcurrent.load();
            while (cur > expected && !maxConcurrent.compare_exchange_weak(expected, cur)) {
            }

            PortValueMap in = sharedInput;
            PortValueMap out;
            ExecutionContext ctx;
            ctx.runId = parallelRunId;
            ctx.frameId = m_frameId.load(std::memory_order_relaxed);
            ctx.cancellationToken = parallelToken;
            const ExecutionResult r = mod->execute(in, out, ctx);
            --running;

            if (!r.success) {
                QMutexLocker locker(&errMutex);
                if (!haveError) {
                    haveError = true;
                    firstError = r;
                }
                if (parallelToken)
                    parallelToken->cancel(); // 取消同组剩余任务
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
    releaseLease();

    if (haveError)
        return firstError;
    return ExecutionResult::ok();
}

void RunEngine::runOnce() {
    if (isBusy()) {
        return;
    }

    QString reason;
    runOnceChecked(&reason);
}

bool RunEngine::runOnceChecked(QString* reason) {
    // 后台执行（设计 §4.2）：非所有者入口——external job 活跃时一律拒绝并给出原因。
    if (reason) {
        reason->clear();
    }
    if (m_externalJobActive.load(std::memory_order_acquire)) {
        if (reason) {
            *reason = QStringLiteral("external_job_active");
        }
        return false;
    }
    if (isBusy()) {
        if (reason) {
            *reason = QStringLiteral("engine_busy");
        }
        return false;
    }

    Logger::instance().info(tr("Starting single run"), "Run");
    // 阶6 八轮：Single 意图在锁内校验 Idle/Stopped。
    bool resuming = false;
    QString resumeModule;
    ImageData resumeData;
    RunMode mode = RunMode::RunOnce;
    if (!tryBeginForRun(RunIntent::Single, resuming, resumeModule, resumeData, mode)) {
        if (reason) {
            *reason = QStringLiteral("start_rejected");
        }
        return false;
    }
    executeAcquiredRun(resuming, resumeModule, resumeData, mode);
    return true;
}

bool RunEngine::runOnceAsync(QString* acceptedRunId, const QJsonObject& agentFolderAuthorization) {
    if (m_externalJobActive.load(std::memory_order_acquire) || isBusy())
        return false;
    if (acceptedRunId)
        acceptedRunId->clear();
    if (!executeRunAsync(RunIntent::Single, acceptedRunId, agentFolderAuthorization))
        return false;
    Logger::instance().info(tr("Starting single run"), "Run");
    return true;
}

bool RunEngine::stepOnce() {
    return stepOnce({});
}

bool RunEngine::stepOnce(const QJsonObject& authorization) {
    return stepOnceImpl(nullptr, authorization);
}

bool RunEngine::stepOnceForJob(const JobToken& token, QString* reason) {
    return stepOnceImpl(&token, {}, reason);
}

bool RunEngine::stepOnceForJob(const JobToken& token, const QJsonObject& authorization,
                               const QString& requestedRunId, const QString& sessionId, int maxCycles,
                               QString* acceptedRunId, QString* reason) {
    return stepOnceImpl(&token, authorization, reason, acceptedRunId, sessionId, requestedRunId, maxCycles);
}

bool RunEngine::stepOnceImpl(const JobToken* token, const QJsonObject& authorization, QString* rejectReason,
                             QString* acceptedRunId, const QString& sessionId, const QString& requestedRunId,
                             int maxCycles) {
    if (acceptedRunId)
        acceptedRunId->clear();
    if (!token && (m_externalJobActive.load(std::memory_order_acquire) || isBusy()))
        return false;

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
    result.runId = m_runId;
            result.errorCode = -1;
            result.errorMessage = tr("No modules to run");
            result.elapsedMs = 0;
            result.finishedTime = QDateTime::currentDateTime();
            emit runFinished(result);
            return false;
        }
    }

    // 阶6 复核（五轮 P1-3）：单步使用与完整运行相同的原子启动协议
    // （tryBeginExecution 在锁内提交 runMode/state/token），stop() 无法插在取权与
    // 提交之间；取权后才修改单步/控制状态。
    bool ignResuming = false;
    QString ignModule;
    ImageData ignData;
    RunMode ignMode = RunMode::RunOnce;
    if (!tryBeginForRun(RunIntent::Step, ignResuming, ignModule, ignData, ignMode, {}, {}, 0, authorization,
                        token, rejectReason, sessionId, requestedRunId, maxCycles)) {
        if (rejectReason && rejectReason->isEmpty())
            *rejectReason = QStringLiteral("start_rejected");
        return false;
    }
    if (acceptedRunId)
        *acceptedRunId = m_runId;
    // 后台执行（设计 §5.3）：单步与完整运行共用同一 RAII 收尾器（终态 Idle/Stopped，
    // Paused 不适用于单步），保证提前返回/异常也释放执行权；单步必须保留跨步控制图状态。
    ExecutionFinalizer finalizer(this, ExecutionOutcome::Failed, /*clearControlGraph=*/false,
                                 /*resetStepStateOnStop=*/true);
    emit stateChanged(state());
    emit runStarted();

    if (m_stepCurrentModuleName.isEmpty()) {
        clearOutputs();
        clearModuleOutputs();
        {
            QMutexLocker nodeOutputsLocker(&m_lifecycleMutex);
            m_nodeOutputs.clear();
        }
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
        finalizer.finish(ExecutionOutcome::Failed);
        releaseVariableExecutionLease();
        return false;
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
    result.runId = m_runId;
    result.success = success;
    result.errorCode = success ? 0 : -1;
    result.errorMessage =
        success ? QString()
                : (m_lastModuleError.isEmpty() ? tr("Step execution failed: %1").arg(moduleToRun) : m_lastModuleError);
    result.elapsedMs = elapsedMs;
    result.finishedTime = QDateTime::currentDateTime();
    if (!success) {
        const RunObservation failedObservation = runObservation(m_runId);
        result.failedModule = failedObservation.result.failedModule;
        result.failedPort = failedObservation.result.failedPort;
        result.diagnostics = failedObservation.result.diagnostics;
        if (failedObservation.result.errorCode != 0)
            result.errorCode = failedObservation.result.errorCode;
    }
    finalizer.finish(ExecutionOutcome::Completed);
    recordCompletedObservation(result, state());
    publishFinalDisplayFrame();
    emit runFinished(result);
    emit stateChanged(state());
    releaseVariableExecutionLease();
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
    startCycle(nullptr);
}

bool RunEngine::startCycle(QString* acceptedSessionId, int intervalMs, bool runFirstIterationImmediately,
                           bool scheduleNextAfterRunFinished, bool deferFirstIteration, int maxCycles,
                           const QJsonObject& agentFolderAuthorization) {
    if (acceptedSessionId)
        acceptedSessionId->clear();
    if (maxCycles < 0)
        return false;
    if (!agentFolderAuthorization.isEmpty() &&
        (maxCycles <= 0 ||
         agentFolderAuthorization.value(QStringLiteral("executionAuthorizationId")).toString().isEmpty() ||
         agentFolderAuthorization.value(QStringLiteral("confirmationId")).toString().isEmpty() ||
         agentFolderAuthorization.value(QStringLiteral("callId")).toString().isEmpty()))
        return false;
    QString startedSessionId;
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
            return false;
        }
    }

    // 阶6 复核（五轮）：检查+runMode/state 提交+resetStepState 全在同一临界区，
    // stop() 无法与清理交错；定时器仅在提交后仍为 Running 时启动（stop 介入则不启）。
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (m_externalJobActive.load(std::memory_order_acquire) || m_executing.load(std::memory_order_acquire) ||
            m_maintenance.load(std::memory_order_acquire) || m_stopPending || m_cycleFailureNotificationPending ||
            state() == RunState::Running || state() == RunState::Paused) {
            return false;
        }
        if (!GlobalVarManager::instance().beginExecutionLease())
            return false;
        m_variableExecutionLeaseHeld.store(true, std::memory_order_release);
        m_runMode.store(static_cast<int>(RunMode::RunCycle), std::memory_order_release);
        m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
        m_cycleMaxIterations = maxCycles;
        m_cycleAgentFolderDeleteAuthorization = agentFolderAuthorization;
        m_cycleSessionId = QString::number(QDateTime::currentMSecsSinceEpoch()) + QLatin1Char('-') +
                           QString::number(m_runSeq.fetch_add(1, std::memory_order_relaxed));
        startedSessionId = m_cycleSessionId;
        {
            QMutexLocker observationLocker(&m_runObservationMutex);
            m_cycleSessionObservation = RunObservation{};
            m_cycleSessionObservation.runId = m_cycleSessionId;
            m_cycleSessionObservation.sessionId = m_cycleSessionId;
            m_cycleSessionObservation.state = RunState::Running;
            m_cycleSessionObservation.maxCycles = maxCycles;
            m_cycleSessionObservation.startedTime = QDateTime::currentDateTime();
            m_cycleSessionObservation.project = m_loadedProject;
            m_cycleSessionObservation.projectFingerprint = m_loadedProjectFingerprint;
        }
        resetStepState();
    }
    // Each session owns its timer callback identity, so an already-posted timeout from a
    // stopped session cannot be mistaken for a tick in a newly-started session.
    auto* timer = new QTimer(this);
    timer->setObjectName(QStringLiteral("RunEngineCycleTimer"));
    timer->setInterval(qMax(1, intervalMs));
    timer->setSingleShot(scheduleNextAfterRunFinished);
    connect(timer, &QTimer::timeout, this, [this, startedSessionId]() { executeCycleTick(startedSessionId); });

    // 阶6 九轮：定时器启动+cycleStarted+日志与状态提交同转换；stop 介入则不启动、
    // 不发出误导信号/日志。
    bool started = false;
    int gen = 0;
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (state() == RunState::Running && m_cycleSessionId == startedSessionId && runMode() == RunMode::RunCycle) {
            if (m_cycleTimer) {
                m_cycleTimer->stop();
                m_cycleTimer->deleteLater();
            }
            m_cycleTimer = timer;
            m_scheduleCycleAfterRunFinished = scheduleNextAfterRunFinished;
            m_cycleFirstIterationDeferred = deferFirstIteration;
            m_cycleIntervalMs = qMax(1, intervalMs);
            if (!deferFirstIteration && (!scheduleNextAfterRunFinished || !runFirstIterationImmediately))
                timer->start();
            gen = m_lifecycleGeneration;
            started = true;
        }
    }
    if (!started) {
        QMutexLocker locker(&m_lifecycleMutex);
        if (m_cycleSessionId == startedSessionId)
            m_cycleAgentFolderDeleteAuthorization = {};
        timer->deleteLater();
        releaseVariableExecutionLease();
        return false;
    }
    // 阶6 十一轮：发通知前重验证 generation+状态；stop 介入则不逆序发 cycleStarted/日志。
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (m_lifecycleGeneration != gen || state() != RunState::Running) {
            return false;
        }
    }
    if (acceptedSessionId)
        *acceptedSessionId = startedSessionId;
    emit stateChanged(state());
    emit cycleStarted();
    Logger::instance().info(tr("Starting continuous run"), "Run");
    if (runFirstIterationImmediately)
        executeCycleTick(startedSessionId);
    return true;
}

bool RunEngine::continueDeferredCycle(const QString& sessionId) {
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (sessionId.isEmpty() || sessionId != m_cycleSessionId || runMode() != RunMode::RunCycle ||
            m_state.load(std::memory_order_acquire) == static_cast<int>(RunState::Stopped))
            return false;
        if (!m_cycleFirstIterationDeferred)
            return false;
        m_cycleFirstIterationDeferred = false;
        if (state() == RunState::Paused)
            return true;
        if (state() != RunState::Running)
            return false;
    }
    executeCycleTick(sessionId);
    return true;
}

void RunEngine::pause() {
    // 阶6 复核（四轮）：Running→Paused 检查+提交在同一临界区。
    {
        QMutexLocker locker(&m_lifecycleMutex);
        // 后台执行（设计 §4.2/§5.3）：external job 活跃时公开 pause() 拒绝；用户暂停走
        // pauseForJob() 的模块边界协议。
        if (m_externalJobActive.load(std::memory_order_acquire)) {
            return;
        }
        if (state() != RunState::Running) {
            return;
        }
        ++m_lifecycleGeneration; // 阶6 十一轮
        m_state.store(static_cast<int>(RunState::Paused), std::memory_order_release);
        clearActiveAgentFolderDeleteAuthorizationForPause();
    }
    stopCycleTimer();
    emit stateChanged(state());

    Logger::instance().info(tr("Run paused"), "Run");
}

void RunEngine::resume() {
    if (m_externalJobActive.load(std::memory_order_acquire))
        return;
    resumeExpected(resumeSnapshot());
}

RunResumeSnapshot RunEngine::resumeSnapshot() const {
    QMutexLocker lifecycleLocker(&m_lifecycleMutex);
    QMutexLocker observationLocker(&m_runObservationMutex);
    RunResumeSnapshot snapshot;
    snapshot.state = state();
    snapshot.lifecycleGeneration = m_lifecycleGeneration;
    const RunObservation& observation =
        (!m_cycleSessionObservation.runId.isEmpty() && !m_cycleSessionObservation.finished) ? m_cycleSessionObservation
                                                                                            : m_runObservation;
    snapshot.runId = observation.runId;
    snapshot.sessionId = observation.sessionId;
    snapshot.project = observation.project;
    snapshot.projectFingerprint = observation.projectFingerprint;
    return snapshot;
}

bool RunEngine::resumeExpected(const RunResumeSnapshot& expected, const QJsonObject& agentFolderAuthorization) {
    if (m_externalJobActive.load(std::memory_order_acquire))
        return false;
    if (expected.state != RunState::Paused)
        return false;

    bool pausedAtBreakpoint = false;
    bool deferred = false;
    int generation = 0;
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (state() != RunState::Paused || m_lifecycleGeneration != expected.lifecycleGeneration ||
            m_executing.load(std::memory_order_acquire) || m_maintenance.load(std::memory_order_acquire))
            return false;
        if (!expected.sessionId.isEmpty()) {
            if (runMode() != RunMode::RunCycle || m_cycleSessionId != expected.sessionId ||
                expected.runId != expected.sessionId)
                return false;
        } else if (runMode() == RunMode::RunCycle || m_runId != expected.runId || expected.runId.isEmpty()) {
            return false;
        }
        pausedAtBreakpoint = m_pausedAtBreakpoint && !m_pauseResumeModule.isEmpty();
        if (m_pausedAgentFolderDeleteConfirmationRequired) {
            const QString authorizationId =
                agentFolderAuthorization.value(QStringLiteral("executionAuthorizationId")).toString();
            if (authorizationId.isEmpty() || authorizationId == m_pausedAgentFolderDeleteAuthorizationId ||
                agentFolderAuthorization.value(QStringLiteral("confirmationId")).toString().isEmpty() ||
                agentFolderAuthorization.value(QStringLiteral("callId")).toString().isEmpty())
                return false;
        }
        if (!pausedAtBreakpoint) {
            if (m_pausedAgentFolderDeleteConfirmationRequired) {
                if (runMode() == RunMode::RunCycle)
                    m_cycleAgentFolderDeleteAuthorization = agentFolderAuthorization;
                else {
                    QMutexLocker authorizationLocker(&m_agentFolderDeleteAuthorizationMutex);
                    m_activeAgentFolderDeleteAuthorization = agentFolderAuthorization;
                }
                m_pausedAgentFolderDeleteConfirmationRequired = false;
                m_pausedAgentFolderDeleteAuthorizationId.clear();
            }
            // 状态/身份/epoch 与 Running 转换在同一生命周期锁内复验。
            m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
            deferred = m_cycleFirstIterationDeferred;
            if (!deferred && m_cycleTimer)
                m_cycleTimer->start();
            generation = m_lifecycleGeneration;
        }
    }

    if (pausedAtBreakpoint) {
        // tryBeginForRun 再次在同一生命周期锁内验证 session/run/epoch 后才取得执行权。
        return executeRun(RunIntent::Resume, expected.sessionId, expected.runId, expected.lifecycleGeneration,
                          agentFolderAuthorization);
    }

    Q_UNUSED(deferred);
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (m_lifecycleGeneration != generation || state() != RunState::Running)
            return false;
    }
    emit stateChanged(state());
    Logger::instance().info(tr("Run resumed"), "Run");
    return true;
}

bool RunEngine::tryBeginForRun(RunIntent intent, bool& resuming, QString& resumeModule, ImageData& resumeData,
                               RunMode& outMode, const QString& expectedCycleSessionId,
                               const QString& expectedResumeRunId, int expectedLifecycleGeneration,
                               const QJsonObject& agentFolderAuthorization, const JobToken* jobToken,
                               QString* rejectReason, const QString& sessionId, const QString& requestedRunId,
                               int maxCycles) {
    // 阶6 八轮（P1-1）：意图+预期状态校验、恢复判定、暂停数据转移、取执行权全部在
    // 同一临界区。stop() 后旧 resume/旧 tick 因预期状态不再满足而被拒绝，不会重启。
    // 后台执行（设计 §4.2/§5.3，t6 F1）：job 所有权/job 级取消标志校验、启动窗口钩子、
    // 复核与状态提交全部在本临界区内（递归锁允许钩子内 stop() 重入），不再存在
    // "复检解锁 → 另锁启动"的两段窗口：stop() 与该次启动严格串行。
    if (rejectReason) {
        rejectReason->clear();
    }
    QMutexLocker locker(&m_lifecycleMutex);
    if (jobToken) {
        if (!validateJobStartLocked(*jobToken, rejectReason)) {
            return false;
        }
        // 设计 §5.3 窗口屏障：钩子在"完成停止/owner/取消标志校验之后、状态提交之前"执行。
        // 钩子内 stop() 经递归锁重入并置 job 级取消标志；随后的同锁复核拒绝本次启动。
        if (m_testBeforeJobStartHook) {
            m_testBeforeJobStartHook();
        }
        if (!validateJobStartLocked(*jobToken, rejectReason)) {
            return false;
        }
    }
    if (m_executing.load(std::memory_order_acquire) || m_maintenance.load(std::memory_order_acquire)) {
        if (rejectReason) {
            *rejectReason = QStringLiteral("engine_busy");
        }
        return false;
    }
    if (m_stopPending) {
        m_stopPending = false;
        if (rejectReason) {
            *rejectReason = jobToken ? QStringLiteral("external_job_cancelled") : QStringLiteral("stopped");
        }
        return false;
    }
    if (jobToken && !agentFolderAuthorization.isEmpty() &&
        ((!sessionId.isEmpty() && maxCycles <= 0) ||
         agentFolderAuthorization.value(QStringLiteral("executionAuthorizationId")).toString().isEmpty() ||
         agentFolderAuthorization.value(QStringLiteral("confirmationId")).toString().isEmpty() ||
         agentFolderAuthorization.value(QStringLiteral("callId")).toString().isEmpty())) {
        if (rejectReason)
            *rejectReason = QStringLiteral("authorization_invalid");
        return false;
    }
    const RunState st = state();
    // 后台执行（设计 §5.3）：暂停上下文可由断点或用户暂停建立；恢复时按原因决定是否
    // 跳过一次断点（m_skipBreakpointOnce 只对 Breakpoint 设置，保证下游模块恰执行一次）。
    const bool paused = !m_pauseResumeModule.isEmpty() &&
                        (m_pauseReason == PauseReason::Breakpoint || m_pauseReason == PauseReason::User);
    bool fresh = false;
    switch (intent) {
    case RunIntent::Single:
        if (st != RunState::Idle && st != RunState::Stopped) {
            return false;
        }
        resuming = false;
        outMode = RunMode::RunOnce;
        fresh = true;
        break;
    case RunIntent::Step:
        // 阶6 九轮（P1-1）：单步 fresh 由 step 位置判定——同一流程连续单步共享
        // runId/累计状态，仅首步（无 step 位置）重置上下文。
        if (st != RunState::Idle && st != RunState::Stopped) {
            return false;
        }
        resuming = false;
        outMode = RunMode::RunOnce;
        fresh = m_stepCurrentModuleName.isEmpty();
        break;
    case RunIntent::CycleTick:
        if (st != RunState::Running || runMode() != RunMode::RunCycle ||
            (!expectedCycleSessionId.isEmpty() && expectedCycleSessionId != m_cycleSessionId) ||
            agentFolderAuthorization != m_cycleAgentFolderDeleteAuthorization ||
            (m_cycleMaxIterations <= 0 && !agentFolderAuthorization.isEmpty())) {
            return false;
        }
        resuming = false;
        outMode = RunMode::RunCycle;
        fresh = true;
        break;
    case RunIntent::Resume:
        if (st != RunState::Paused || !paused ||
            (expectedLifecycleGeneration != 0 && m_lifecycleGeneration != expectedLifecycleGeneration) ||
            (!expectedCycleSessionId.isEmpty() &&
             (runMode() != RunMode::RunCycle || m_cycleSessionId != expectedCycleSessionId)) ||
            (expectedCycleSessionId.isEmpty() &&
             (runMode() == RunMode::RunCycle || expectedResumeRunId.isEmpty() || m_runId != expectedResumeRunId))) {
            return false;
        }
        if (intent == RunIntent::Resume && m_pausedAgentFolderDeleteConfirmationRequired) {
            const QString authorizationId =
                agentFolderAuthorization.value(QStringLiteral("executionAuthorizationId")).toString();
            if (authorizationId.isEmpty() || authorizationId == m_pausedAgentFolderDeleteAuthorizationId ||
                agentFolderAuthorization.value(QStringLiteral("confirmationId")).toString().isEmpty() ||
                agentFolderAuthorization.value(QStringLiteral("callId")).toString().isEmpty())
                return false;
        }
        if (!expectedCycleSessionId.isEmpty() && !m_cycleAgentFolderDeleteAuthorization.isEmpty() &&
            !agentFolderAuthorization.isEmpty())
            m_cycleAgentFolderDeleteAuthorization = agentFolderAuthorization;
        resuming = true;
        outMode = m_pauseRunMode; // 阶6 八轮（P1-2）：还原暂停前模式
        resumeModule = m_pauseResumeModule;
        resumeData = m_pausePipelineData;
        if (m_breakpointPausedAt.isValid()) {
            const qint64 pausedMs = m_breakpointPausedAt.msecsTo(QDateTime::currentDateTime());
            if (pausedMs > 0) {
                m_runStartTime = m_runStartTime.addMSecs(pausedMs);
            }
        }
        m_pauseResumeModule.clear();
        m_pausePipelineData = ImageData();
        m_breakpointPausedAt = QDateTime();
        m_pausedAtBreakpoint = false;
        m_skipBreakpointOnce = (m_pauseReason == PauseReason::Breakpoint);
        m_pauseReason = PauseReason::None;
        m_userPauseRequested.store(false, std::memory_order_release);
        break;
    }
    if (intent == RunIntent::Single || intent == RunIntent::Step) {
        if (!GlobalVarManager::instance().beginExecutionLease())
            return false;
        m_variableExecutionLeaseHeld.store(true, std::memory_order_release);
    } else if (!m_variableExecutionLeaseHeld.load(std::memory_order_acquire)) {
        return false;
    }
    ++m_lifecycleGeneration; // 阶6 十一轮：取权是一次生命周期转换
    m_executing.store(true, std::memory_order_release);
    m_runMode.store(static_cast<int>(outMode), std::memory_order_release);
    m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    if (m_cancellationToken) {
        m_cancellationToken->reset();
    }
    if (fresh) {
        m_runStartTime = QDateTime::currentDateTime();
        m_runAllSuccess = true;
        m_runFirstError.clear();
        m_runId = requestedRunId.isEmpty()
                      ? QString::number(QDateTime::currentMSecsSinceEpoch()) + QLatin1Char('-') +
                            QString::number(m_runSeq.fetch_add(1, std::memory_order_relaxed))
                      : requestedRunId;
        m_frameId.store(0, std::memory_order_release);
        m_lastParallelMaxConcurrency.store(0, std::memory_order_release);
        QMutexLocker observationLocker(&m_runObservationMutex);
        m_runObservation = RunObservation{};
        m_runObservation.runId = m_runId;
        m_runObservation.sessionId = !sessionId.isEmpty() ? sessionId
                                                          : (outMode == RunMode::RunCycle ? m_cycleSessionId : QString());
        m_runObservation.maxCycles = maxCycles;
        m_runObservation.state = RunState::Running;
        m_runObservation.startedTime = m_runStartTime;
        m_runObservation.project = m_loadedProject;
        m_runObservation.projectFingerprint = m_loadedProjectFingerprint;
        m_runPortOutputs.clear();
        if (!sessionId.isEmpty()) {
            if (m_cycleSessionObservation.sessionId != sessionId) {
                m_cycleSessionObservation = RunObservation{};
                m_cycleSessionObservation.runId = sessionId;
                m_cycleSessionObservation.sessionId = sessionId;
                m_cycleSessionObservation.maxCycles = maxCycles;
                m_cycleSessionObservation.startedTime = m_runStartTime;
                m_cycleSessionObservation.project = m_loadedProject;
                m_cycleSessionObservation.projectFingerprint = m_loadedProjectFingerprint;
            }
            m_cycleSessionObservation.state = RunState::Running;
            m_cycleSessionObservation.lastIterationRunId = m_runId;
        } else if (outMode == RunMode::RunCycle) {
            m_cycleSessionObservation.lastIterationRunId = m_runId;
        }
    } else {
        QMutexLocker observationLocker(&m_runObservationMutex);
        m_runObservation.state = RunState::Running;
    }
    {
        QMutexLocker authorizationLocker(&m_agentFolderDeleteAuthorizationMutex);
        m_activeAgentFolderDeleteAuthorization = agentFolderAuthorization;
    }
    if (intent == RunIntent::Resume && m_pausedAgentFolderDeleteConfirmationRequired) {
        m_pausedAgentFolderDeleteConfirmationRequired = false;
        m_pausedAgentFolderDeleteAuthorizationId.clear();
    }
    return true;
}

void RunEngine::recordCompletedObservation(const RunResult& result, RunState finalState) {
    QMutexLocker observationLocker(&m_runObservationMutex);
    if (m_runObservation.runId != result.runId)
        return;
    m_runObservation.result = result;
    m_runObservation.finished = true;
    m_runObservation.state = finalState;
    m_lastFinishedRun = m_runObservation;
    {
        QMutexLocker valueLocker(&m_outputMutex);
        QMutexLocker imageLocker(&m_lastOutputMutex);
        m_lastFinishedValues = m_outputMap;
        m_lastFinishedImages = m_moduleOutputs;
        m_lastFinishedPortOutputs = m_runPortOutputs;
    }
    if (m_cycleSessionObservation.runId == m_runObservation.sessionId) {
        m_cycleSessionObservation.state = finalState;
        m_cycleSessionObservation.lastCompletedRunId = result.runId;
        if (result.success) {
            m_cycleSessionObservation.lastSuccessfulRunId = result.runId;
            ++m_cycleSessionObservation.completedCycles;
            if (m_cycleSessionObservation.maxCycles > 0 &&
                m_cycleSessionObservation.completedCycles >= m_cycleSessionObservation.maxCycles) {
                m_cycleSessionObservation.finished = true;
                m_cycleSessionObservation.state = finalState;
                m_cycleSessionObservation.result = result;
            }
        } else if (m_cycleSessionObservation.maxCycles > 0) {
            m_cycleSessionObservation.finished = true;
            m_cycleSessionObservation.state = RunState::Stopped;
            m_cycleSessionObservation.result = result;
        }
    }
}

RunEngine::ExecutionFinalizer::ExecutionFinalizer(RunEngine* engine, ExecutionOutcome outcome, bool clearControlGraph,
                                                  bool resetStepStateOnStop)
    : m_engine(engine), m_outcome(outcome), m_clearControlGraph(clearControlGraph),
      m_resetStepStateOnStop(resetStepStateOnStop) {}

RunEngine::ExecutionFinalizer::~ExecutionFinalizer() {
    finish(m_outcome);
}

void RunEngine::ExecutionFinalizer::finish(ExecutionOutcome outcome) {
    m_outcome = outcome;
    if (m_done)
        return;
    m_done = true;
    if (m_engine)
        m_engine->finalizeExecution(outcome, m_clearControlGraph, m_resetStepStateOnStop);
}

void RunEngine::finalizeExecution(ExecutionOutcome outcome, bool clearControlGraph, bool resetStepStateOnStop) {
    if (m_testFinalizeHook)
        m_testFinalizeHook();
    QMutexLocker locker(&m_lifecycleMutex);
    if (m_finalizedGeneration == m_lifecycleGeneration)
        return;
    ++m_lifecycleGeneration;
    m_finalizedGeneration = m_lifecycleGeneration;
    if (outcome == ExecutionOutcome::Paused) {
        if (m_stopPending || state() == RunState::Stopped) {
            m_stopPending = false;
            clearBreakpointPauseState();
            m_executing.store(false, std::memory_order_release);
            m_state.store(static_cast<int>(RunState::Stopped), std::memory_order_release);
        } else {
            m_pauseRunMode = runMode();
            m_userPauseRequested.store(false, std::memory_order_release);
            m_executing.store(false, std::memory_order_release);
            m_state.store(static_cast<int>(RunState::Paused), std::memory_order_release);
        }
        return;
    }
    if (clearControlGraph)
        clearControlQueue();
    clearBreakpointPauseState();
    const bool stopped = m_stopPending || state() == RunState::Stopped || outcome == ExecutionOutcome::Stopped;
    m_stopPending = false;
    if (stopped && resetStepStateOnStop)
        resetStepState();
    m_executing.store(false, std::memory_order_release);
    if (stopped) {
        m_state.store(static_cast<int>(RunState::Stopped), std::memory_order_release);
    } else if (outcome == ExecutionOutcome::Completed && runMode() == RunMode::RunCycle) {
        m_state.store(static_cast<int>(RunState::Running), std::memory_order_release);
    } else {
        if (outcome == ExecutionOutcome::Failed && runMode() == RunMode::RunCycle) {
            stopCycleTimer();
            m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
        }
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    }
}

void RunEngine::finalizeRunTail(const RunResult& result) {
    ExecutionFinalizer finalizer(this, ExecutionOutcome::Completed);
    finalizer.finish(ExecutionOutcome::Completed);
    recordCompletedObservation(result, state());
    releaseVariableExecutionLease();
}

void RunEngine::setFinalizeHookForTest(std::function<void()> hook) {
    m_testFinalizeHook = std::move(hook);
}

void RunEngine::clearFinalizeHookForTest() {
    m_testFinalizeHook = nullptr;
}

void RunEngine::setExecutionFaultInjectionForTest(std::function<void()> hook) {
    m_testFaultInjection = std::move(hook);
}

void RunEngine::clearExecutionFaultInjectionForTest() {
    m_testFaultInjection = nullptr;
}

bool RunEngine::tryAcquireLease() {
    // 公开并行原语 executeParallel 的排他租约：与执行/维护互斥，且**循环运行/
    // 断点暂停期间拒绝**（P1-4 隔离语义）；取租约时重置 token（此时无其它运行）。
    // 后台执行（设计 §4.2）：external job 活跃时公开并行原语同样拒绝（只有所有者可执行）。
    QMutexLocker locker(&m_lifecycleMutex);
    if (m_externalJobActive.load(std::memory_order_acquire) || m_executing.load(std::memory_order_acquire) ||
        m_maintenance.load(std::memory_order_acquire) || state() == RunState::Running || state() == RunState::Paused) {
        return false;
    }
    if (m_stopPending) {
        m_stopPending = false;
        return false;
    }
    if (!GlobalVarManager::instance().beginExecutionLease())
        return false;
    m_variableExecutionLeaseHeld.store(true, std::memory_order_release);
    m_executing.store(true, std::memory_order_release);
    if (m_cancellationToken) {
        m_cancellationToken->reset();
    }
    return true;
}

void RunEngine::releaseLease() {
    {
        QMutexLocker locker(&m_lifecycleMutex);
        m_stopPending = false;
        m_executing.store(false, std::memory_order_release);
    }
    releaseVariableExecutionLease();
}

bool RunEngine::tryAcquireMaintenance() {
    // 模块维护租约：与执行/其它维护互斥；持有期间 tryBeginExecution 拒绝，
    // 保证维护期间无运行启动（消除 check-then-act 悬空指针/混合工程）。
    // 后台执行（设计 §4.2）：external job 租约期间 loadProject/addModule/removeModule/
    // clearModules 一律拒绝（ProjectManager 的工程切换因此被挡）。
    QMutexLocker locker(&m_lifecycleMutex);
    if (m_externalJobActive.load(std::memory_order_acquire) || m_executing.load(std::memory_order_acquire) ||
        m_maintenance.load(std::memory_order_acquire) || state() == RunState::Running || state() == RunState::Paused) {
        return false;
    }
    m_maintenance = true;
    return true;
}

RunEngine::ProjectMutationResult RunEngine::withProjectMutationLease(const std::function<bool()>& mutation) {
    if (!mutation || !tryAcquireMaintenance())
        return ProjectMutationResult::Busy;
    bool applied = false;
    try {
        applied = mutation();
    } catch (...) {
        releaseMaintenance();
        throw;
    }
    releaseMaintenance();
    return applied ? ProjectMutationResult::Applied : ProjectMutationResult::Rejected;
}

void RunEngine::releaseMaintenance() {
    QMutexLocker locker(&m_lifecycleMutex);
    // 阶6 七轮（P2-6）：维护期间收到的停止请求在此补做清理，避免被丢弃。
    if (m_stopPending) {
        ++m_lifecycleGeneration; // 阶6 十一轮：维护期停止补清理是一次转换
        m_stopPending = false;
        clearBreakpointPauseState();
        resetStepState();
    }
    m_maintenance = false;
}

void RunEngine::setInnerPauseLocked(const QString& mod, const ImageData& data, PauseReason reason) {
    // 阶6 五轮：内层只在此锁内写暂停态；stop() 同锁清理，消除 QString/ImageData
    // 并发读清空 UB。后台执行（设计 §5.3）：用户暂停复用同一暂停上下文，用 reason 区分。
    QMutexLocker locker(&m_lifecycleMutex);
    m_pauseResumeModule = mod;
    m_pausePipelineData = data;
    m_breakpointPausedAt = QDateTime::currentDateTime();
    m_pauseReason = reason;
    // isPausedAtBreakpoint() 仍只在断点原因时为真（UI/Service 用 pauseReason 判提示）。
    m_pausedAtBreakpoint = (reason == PauseReason::Breakpoint);
    if (reason == PauseReason::User) {
        m_userPauseRequested.store(false, std::memory_order_release);
    }
}

void RunEngine::finalizeAbortedRun(RunMode mode, const RunResult& result, bool retainCycleSession) {
    // 阶6 五轮：空模块/校验失败早退的原子收尾——在锁内判定是否已有 stop()，
    // 不覆盖 Stopped；仅无停止时复位 Idle，防状态卡死/停止丢失。
    QMutexLocker locker(&m_lifecycleMutex);
    ++m_lifecycleGeneration; // 阶6 十一轮：中止收尾是一次生命周期转换
    m_finalizedGeneration = m_lifecycleGeneration;
    const bool wasStopped = (state() == RunState::Stopped) || m_stopPending;
    clearControlQueue();
    clearBreakpointPauseState();
    m_stopPending = false;
    if (mode == RunMode::RunCycle && !retainCycleSession) {
        stopCycleTimer();
        m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
    }
    const RunState resultState =
        wasStopped ? RunState::Stopped : (retainCycleSession ? RunState::Running : RunState::Idle);
    m_state.store(static_cast<int>(resultState), std::memory_order_release);
    recordCompletedObservation(result, state());
    m_executing.store(false, std::memory_order_release);
}

bool RunEngine::retainsCycleSessionAfterFailure() {
    QMutexLocker locker(&m_lifecycleMutex);
    return m_scheduleCycleAfterRunFinished;
}

bool RunEngine::takeUserPauseRequest(const QString& moduleName, const ImageData& data) {
    // 设计 §5.3：模块边界检查并消费用户暂停请求（check-then-act 在同一临界区）。
    QMutexLocker locker(&m_lifecycleMutex);
    if (!m_userPauseRequested.load(std::memory_order_acquire)) {
        return false;
    }
    setInnerPauseLocked(moduleName, data, PauseReason::User);
    return true;
}

bool RunEngine::lifecycleBusyLocked() {
    QMutexLocker locker(&m_lifecycleMutex);
    return m_executing.load(std::memory_order_acquire) || m_maintenance.load(std::memory_order_acquire) ||
           state() == RunState::Running || state() == RunState::Paused;
}

void RunEngine::stop() {
    stopCycleTimer();
    bool finishPausedRun = false;
    bool stoppedCycle = false;
    RunResult pausedCancellation{};

    // 阶6 复核（二轮）：不在取锁前按 Stopped 提前返回（否则会丢失对"已声明开始、
    // 尚未取执行权"运行的停止请求）。状态转换与清理决策全部在生命周期锁内：
    // 有执行权持有者时置 m_stopPending 令其自行退出/放弃；否则就地清理。
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (m_cycleFailureNotificationPending)
            return;
        const bool stoppingCycle = runMode() == RunMode::RunCycle;
        m_cycleAgentFolderDeleteAuthorization = {};
        finishPausedRun = state() == RunState::Paused && !m_executing.load(std::memory_order_acquire);
        ++m_lifecycleGeneration; // 阶6 十一轮：停止是一次生命周期转换
        m_state.store(static_cast<int>(RunState::Stopped), std::memory_order_release);
        m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
        m_pausedAgentFolderDeleteConfirmationRequired = false;
        m_pausedAgentFolderDeleteAuthorizationId.clear();
        if (m_cancellationToken) {
            m_cancellationToken->cancel();
        }
        // 后台执行（设计 §5.3）：job 级取消标志覆盖 Cycle 两轮之间的窗口，旧 job 不能清除它；
        // 同时清除用户暂停请求，防止旧 resume 重启。
        if (m_externalJobActive.load(std::memory_order_acquire)) {
            m_externalJobCancelRequested = true;
        }
        m_userPauseRequested.store(false, std::memory_order_release);
        if (m_executing.load(std::memory_order_acquire)) {
            m_stopPending = true;
        } else if (m_maintenance.load(std::memory_order_acquire)) {
            // 阶6 七轮（P2-6）：维护期间不并发清理，但保留 stopPending，由
            // releaseMaintenance() 补做停止清理（addModule 等路径不自行清理）。
            m_stopPending = true;
        } else {
            m_stopPending = false;
            clearBreakpointPauseState();
            resetStepState();
        }
        if (finishPausedRun) {
            {
                QMutexLocker observationLocker(&m_runObservationMutex);
                if (!m_runObservation.runId.isEmpty() && !m_runObservation.finished) {
                    pausedCancellation.runId = m_runObservation.runId;
                    pausedCancellation.success = false;
                    pausedCancellation.errorCode = ExecError::Cancelled;
                    pausedCancellation.errorMessage = tr("Run cancelled while paused");
                    pausedCancellation.elapsedMs = m_runObservation.startedTime.msecsTo(QDateTime::currentDateTime());
                    pausedCancellation.finishedTime = QDateTime::currentDateTime();
                }
            }
            if (!pausedCancellation.runId.isEmpty())
                recordCompletedObservation(pausedCancellation, RunState::Stopped);
        }
        if (stoppingCycle) {
            {
                QMutexLocker observationLocker(&m_runObservationMutex);
                if (m_cycleSessionObservation.runId == m_cycleSessionId && !m_cycleSessionObservation.finished) {
                    m_cycleSessionObservation.finished = true;
                    m_cycleSessionObservation.state = RunState::Stopped;
                    m_cycleSessionObservation.result.runId = m_cycleSessionObservation.runId;
                    m_cycleSessionObservation.result.success = false;
                    m_cycleSessionObservation.result.errorCode = ExecError::Cancelled;
                    m_cycleSessionObservation.result.errorMessage = tr("Cycle session cancelled by user");
                    m_cycleSessionObservation.result.finishedTime = QDateTime::currentDateTime();
                }
            }
            m_cycleSessionId.clear();
            m_cycleAgentFolderDeleteAuthorization = {};
            stoppedCycle = true;
        }
    }

    if (stoppedCycle)
        emit cycleStopped();
    if (!pausedCancellation.runId.isEmpty())
        emit runFinished(pausedCancellation);
    emit stateChanged(state());

    if (!m_executing.load(std::memory_order_acquire))
        releaseVariableExecutionLease();

    Logger::instance().info(tr("Run stopped"), "Run");
}

bool RunEngine::isPausedAtBreakpoint() const {
    QMutexLocker locker(&m_lifecycleMutex);
    return m_pausedAtBreakpoint;
}

QString RunEngine::cycleSessionId() const {
    QMutexLocker locker(&m_lifecycleMutex);
    return m_cycleSessionId;
}

bool RunEngine::refreshCycleProjectSnapshot(const QString& sessionId, Project* project) {
    if (!project)
        return false;
    QMutexLocker lifecycleLocker(&m_lifecycleMutex);
    if (sessionId.isEmpty() || sessionId != m_cycleSessionId || runMode() != RunMode::RunCycle ||
        (state() != RunState::Running && state() != RunState::Paused))
        return false;
    QMutexLocker observationLocker(&m_runObservationMutex);
    if (!m_cycleSessionObservation.runId.isEmpty() && m_cycleSessionObservation.runId == sessionId) {
        m_cycleSessionObservation.project = project;
        m_cycleSessionObservation.projectFingerprint = projectDigest(project);
    }
    if (m_runObservation.sessionId == sessionId) {
        m_runObservation.project = project;
        m_runObservation.projectFingerprint = projectDigest(project);
    }
    m_loadedProject = project;
    m_loadedProjectFingerprint = projectDigest(project);
    return true;
}

PauseReason RunEngine::pauseReason() const {
    QMutexLocker locker(&m_lifecycleMutex);
    return m_pauseReason;
}

void RunEngine::requestCancellation() {
    if (m_cancellationToken) {
        m_cancellationToken->cancel();
    }
    stop();
}

// ---------------------------------------------------------------------------
// 后台执行（设计 §4.2/§5.3）：external job 租约、所有者入口与 job 级取消标志
// ---------------------------------------------------------------------------

bool RunEngine::beginExternalJob(JobToken* token, QString* reason) {
    if (reason) {
        reason->clear();
    }
    QMutexLocker locker(&m_lifecycleMutex);
    if (m_externalJobActive.load(std::memory_order_acquire)) {
        if (reason) {
            *reason = QStringLiteral("external_job_active");
        }
        return false;
    }
    // 设计 §4.2：beginExternalJob() 在引擎已执行/维护时失败；由 Service 显式上报。
    if (m_executing.load(std::memory_order_acquire) || m_maintenance.load(std::memory_order_acquire) ||
        state() == RunState::Running || state() == RunState::Paused) {
        if (reason) {
            *reason = QStringLiteral("engine_busy");
        }
        return false;
    }

    JobToken fresh;
    fresh.id = ++m_externalJobSeq;
    fresh.salt = m_jobSalt;
    m_externalJobOwner = fresh;
    m_externalJobCancelRequested = false; // 新 job 才允许复位 job 级取消标志
    m_externalJobActive.store(true, std::memory_order_release);
    if (token) {
        *token = fresh;
    }
    return true;
}

bool RunEngine::endExternalJob(const JobToken& token) {
    QMutexLocker locker(&m_lifecycleMutex);
    if (!m_externalJobActive.load(std::memory_order_acquire) || !token.isValid() || !(token == m_externalJobOwner)) {
        return false; // 重复/过期/伪造 token 无效
    }
    m_externalJobActive.store(false, std::memory_order_release);
    m_externalJobOwner = JobToken{};
    m_externalJobCancelRequested = false;
    m_userPauseRequested.store(false, std::memory_order_release);
    // t6 O1：租约结束不得留下“无恢复上下文、无法再 resume”的 Paused 引擎；
    // 正常路径下 Service 会在结束租约前恢复/停止，此分支只做防御性清理。
    if (state() == RunState::Paused) {
        clearBreakpointPauseState();
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    }
    return true;
}

bool RunEngine::validateJobStartLocked(const JobToken& token, QString* reason) {
    // 调用方必须持有 m_lifecycleMutex（tryBeginForRun 在状态提交的同一临界区内调用）。
    if (!m_externalJobActive.load(std::memory_order_acquire)) {
        if (reason) {
            *reason = QStringLiteral("external_job_inactive");
        }
        return false;
    }
    if (!token.isValid() || !(token == m_externalJobOwner)) {
        if (reason) {
            *reason = QStringLiteral("external_job_owner_mismatch");
        }
        return false;
    }
    if (m_externalJobCancelRequested || m_stopPending) {
        // job 级取消标志：旧 job 不能清除它，必须等 endExternalJob 后重新 begin 才能复位。
        if (reason) {
            *reason = QStringLiteral("external_job_cancelled");
        }
        return false;
    }
    return true;
}

bool RunEngine::runOnceForJob(const JobToken& token, QString* reason) {
    bool resuming = false;
    QString resumeModule;
    ImageData resumeData;
    RunMode mode = RunMode::RunOnce;
    if (!tryBeginForRun(RunIntent::Single, resuming, resumeModule, resumeData, mode, {}, {}, 0, {}, &token, reason)) {
        if (reason && reason->isEmpty()) {
            *reason = QStringLiteral("start_rejected");
        }
        return false;
    }
    executeAcquiredRun(resuming, resumeModule, resumeData, mode);
    if (!isExecuting() && state() != RunState::Paused)
        releaseVariableExecutionLease();
    return true;
}

bool RunEngine::runOnceForJob(const JobToken& token, const QJsonObject& authorization,
                              const QString& requestedRunId, const QString& sessionId, int maxCycles,
                              QString* acceptedRunId, QString* reason) {
    if (acceptedRunId)
        acceptedRunId->clear();
    bool resuming = false;
    QString resumeModule;
    ImageData resumeData;
    RunMode mode = RunMode::RunOnce;
    if (!tryBeginForRun(RunIntent::Single, resuming, resumeModule, resumeData, mode, {}, {}, 0, authorization,
                        &token, reason, sessionId, requestedRunId, maxCycles)) {
        if (reason && reason->isEmpty())
            *reason = QStringLiteral("start_rejected");
        return false;
    }
    if (acceptedRunId)
        *acceptedRunId = m_runId;
    executeAcquiredRun(resuming, resumeModule, resumeData, mode);
    if (!isExecuting() && state() != RunState::Paused)
        releaseVariableExecutionLease();
    return true;
}

bool RunEngine::resumeForJob(const JobToken& token, QString* reason) {
    bool resuming = false;
    QString resumeModule;
    ImageData resumeData;
    RunMode mode = RunMode::RunOnce;
    if (!tryBeginForRun(RunIntent::Resume, resuming, resumeModule, resumeData, mode, {}, m_runId, 0, {}, &token,
                        reason)) {
        if (reason && reason->isEmpty()) {
            *reason = QStringLiteral("not_paused");
        }
        return false;
    }
    executeAcquiredRun(resuming, resumeModule, resumeData, mode);
    if (!isExecuting() && state() != RunState::Paused)
        releaseVariableExecutionLease();
    return true;
}

bool RunEngine::resumeForJob(const JobToken& token, const QJsonObject& authorization,
                             QString* acceptedRunId, QString* reason) {
    if (acceptedRunId)
        acceptedRunId->clear();
    bool resuming = false;
    QString resumeModule;
    ImageData resumeData;
    RunMode mode = RunMode::RunOnce;
    QString expectedRunId;
    {
        QMutexLocker locker(&m_runObservationMutex);
        expectedRunId = m_runObservation.runId;
    }
    if (!tryBeginForRun(RunIntent::Resume, resuming, resumeModule, resumeData, mode, {}, expectedRunId, 0,
                        authorization, &token, reason)) {
        if (reason && reason->isEmpty())
            *reason = QStringLiteral("not_paused");
        return false;
    }
    if (acceptedRunId)
        *acceptedRunId = m_runId;
    executeAcquiredRun(resuming, resumeModule, resumeData, mode);
    if (!isExecuting() && state() != RunState::Paused)
        releaseVariableExecutionLease();
    return true;
}

bool RunEngine::pauseForJob(const JobToken& token) {
    QMutexLocker locker(&m_lifecycleMutex);
    if (!m_externalJobActive.load(std::memory_order_acquire) || !token.isValid() || !(token == m_externalJobOwner) ||
        m_externalJobCancelRequested) {
        return false;
    }
    if (!m_executing.load(std::memory_order_acquire) && state() != RunState::Running) {
        return false; // 无运行可暂停
    }
    // 设计 §5.3：不在此改状态，只登记请求；执行循环在模块边界响应。
    // t6 O3：这里不是生命周期状态提交（状态由执行循环在模块边界/收尾时提交），
    // 不递增 m_lifecycleGeneration。
    m_userPauseRequested.store(true, std::memory_order_release);
    return true;
}

bool RunEngine::isExternalJobActiveForTest() const {
    return m_externalJobActive.load(std::memory_order_acquire);
}

void RunEngine::setBeforeJobStartHookForTest(std::function<void()> hook) {
    m_testBeforeJobStartHook = std::move(hook);
}

void RunEngine::clearBeforeJobStartHookForTest() {
    m_testBeforeJobStartHook = nullptr;
}

void RunEngine::addModuleLocked(ModuleBase* module) {
    QWriteLocker locker(&m_moduleLock);
    if (module && !m_modules.contains(module)) {
        m_modules.append(module);
        QString key = runtimeModuleName(module);
        m_moduleMap[key] = module;
        cacheExecutionAffinity(module); // 后台执行（设计 §6.1）：随模块加入缓存亲和性
        Logger::instance().debug(QString("Module added to engine: %1").arg(key), "Run");
    }
}

void RunEngine::addModule(ModuleBase* module) {
    // 阶6 复核（四轮 P0-2）：检查与修改在同一维护租约内原子完成。
    if (!tryAcquireMaintenance()) {
        Logger::instance().warning(tr("Cannot add modules while the flow is running"), "Run");
        return;
    }
    addModuleLocked(module);
    releaseMaintenance();
}

bool RunEngine::loadProject(Project* project, ModuleFactory factory) {
    // 阶6 复核（四轮 P0-2）：整个加载过程持有维护租约，stop()+清空+建模块+添加
    // 期间无运行启动、无并发维护，杜绝混合工程状态与悬空模块指针。
    if (!tryAcquireMaintenance()) {
        emit errorOccurred(tr("Cannot load a project while the flow is running"));
        return false;
    }
    stop();
    clearModulesLocked();

    if (!project) {
        releaseMaintenance();
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
                pluginManager.destroyModule(module);
                return nullptr;
            }
            return moduleBase;
        };
    }

    m_disabledModules.clear();
    for (const ModuleInstance& inst : project->modules()) {
        ModuleBase* module = factory(inst);
        if (!module) {
            clearModulesLocked();
            releaseMaintenance();
            emit errorOccurred(tr("Cannot create module: %1").arg(inst.moduleId));
            return false;
        }

        module->setProperty("deeplux.pluginId", inst.moduleId);
        module->setInstanceName(inst.id);
        module->setParams(inst.params);
        if (!module->initialize()) {
            if (!PluginManager::instance().destroyModule(module))
                delete module;
            clearModulesLocked();
            releaseMaintenance();
            emit errorOccurred(tr("Cannot initialize module: %1").arg(inst.moduleId));
            return false;
        }

        m_ownedModules.append(module);
        addModuleLocked(module);

        // 阶段 B: 断点/禁用同步
        if (inst.breakpoint)
            setBreakpoint(inst.id, true);
        if (!inst.enabled)
            m_disabledModules.insert(inst.id);
    }

    QString orderError;
    if (!buildExecutionOrder(project, orderError)) {
        clearModulesLocked();
        releaseMaintenance();
        emit errorOccurred(orderError);
        return false;
    }

    {
        QMutexLocker observationLocker(&m_runObservationMutex);
        m_loadedProject = project;
        m_loadedProjectFingerprint = projectDigest(project);
    }

    releaseMaintenance();
    return true;
}

void RunEngine::removeModule(const QString& moduleId) {
    // 阶6 复核（四轮 P0-2）：检查与删除同租约原子，删除期间无运行持有模块指针。
    if (!tryAcquireMaintenance()) {
        Logger::instance().warning(tr("Cannot remove modules while the flow is running"), "Run");
        return;
    }
    QString removedName;
    // L5-I1/I3：先取 lifecycle 再取 moduleLock（全局锁序），保证 m_nodeOutputs 写受保护
    QMutexLocker lifecycleLocker(&m_lifecycleMutex);
    QWriteLocker locker(&m_moduleLock);
    for (int i = 0; i < m_modules.size(); ++i) {
        ModuleBase* module = m_modules[i];
        if (module->id() == moduleId || runtimeModuleName(module) == moduleId) {
            removedName = runtimeModuleName(module);
            m_moduleMap.remove(removedName);
            m_modules.removeAt(i);
            m_disabledModules.remove(removedName);
            forgetExecutionAffinity(removedName); // 后台执行（设计 §6.1）：热替换不沿用旧亲和性
            m_nodeOutputs.remove(removedName);
            m_executionOrder.removeAll(removedName); // 从拓扑序移除，不清空剩余 DAG
            {
                QMutexLocker bl(&m_breakpointMutex);
                m_breakpoints.remove(removedName);
            }
            if (m_ownedModules.removeOne(module)) {
                if (!PluginManager::instance().destroyModule(module))
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
        // 阶6 五轮：暂停态写入加锁，与锁内读取串行化。
        {
            QMutexLocker locker(&m_lifecycleMutex);
            if (m_pauseResumeModule == removedName) {
                m_pauseResumeModule.clear();
                m_pausedAtBreakpoint = false;
            }
        }
    }
    releaseMaintenance();
}

void RunEngine::clearModulesLocked() {
    QMutexLocker lifecycleLocker(&m_lifecycleMutex);
    QWriteLocker locker(&m_moduleLock);
    for (ModuleBase* module : m_ownedModules) {
        if (!PluginManager::instance().destroyModule(module))
            delete module;
    }
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
    {
        // 后台执行（设计 §6.1）：清空模块时同步清空亲和性缓存，避免重载工程沿用旧值。
        QMutexLocker affinityLocker(&m_affinityMutex);
        m_executionAffinity.clear();
    }
    {
        QMutexLocker runtimeLocker(&m_runtimeInfoMutex);
        m_moduleRuntimeInfo.clear();
    }
    clearBreakpointPauseState();
    locker.unlock();
    {
        QMutexLocker bl(&m_breakpointMutex);
        m_breakpoints.clear();
    }
    clearOutputs();
    resetStepState();
    QMutexLocker observationLocker(&m_runObservationMutex);
    m_loadedProject.clear();
    m_loadedProjectFingerprint.clear();
}

void RunEngine::clearModules() {
    // 阶6 复核（四轮 P0-2）：检查与清空同维护租约原子完成。
    if (!tryAcquireMaintenance()) {
        Logger::instance().warning(tr("Cannot clear modules while a module is executing"), "Run");
        return;
    }
    clearModulesLocked();
    releaseMaintenance();
}

QList<ModuleBase*> RunEngine::modules() const {
    QReadLocker locker(&m_moduleLock);
    return m_modules;
}

ExecutionAffinity RunEngine::executionAffinityFor(const QString& moduleName, ModuleBase* module) const {
    {
        QMutexLocker locker(&m_affinityMutex);
        const auto it = m_executionAffinity.constFind(moduleName);
        if (it != m_executionAffinity.constEnd()) {
            return it.value();
        }
    }
    return affinityForModule(module);
}

void RunEngine::cacheExecutionAffinity(ModuleBase* module) {
    if (!module) {
        return;
    }
    const QString key = runtimeModuleName(module);
    if (key.isEmpty()) {
        return;
    }
    const ExecutionAffinity affinity = affinityForModule(module);
    QMutexLocker locker(&m_affinityMutex);
    m_executionAffinity.insert(key, affinity);
}

void RunEngine::forgetExecutionAffinity(const QString& moduleName) {
    QMutexLocker locker(&m_affinityMutex);
    m_executionAffinity.remove(moduleName);
}

void RunEngine::setModuleExecutionAffinityForTest(const QString& moduleName, ExecutionAffinity affinity) {
    QMutexLocker locker(&m_affinityMutex);
    m_executionAffinity.insert(moduleName, affinity);
}

bool RunEngine::isParallelEligibleForTest(const QString& moduleName) const {
    ModuleBase* module = getModule(moduleName);
    return module && isParallelEligible(module);
}

bool RunEngine::isParallelEligible(ModuleBase* module) const {
    if (!module) {
        return false;
    }
    // 后台执行（设计 §6.3）：worker 亲和 + 线程安全 + 非阻塞 + 无断点才可进入并行池。
    if (executionAffinityFor(runtimeModuleName(module), module) != ExecutionAffinity::Worker) {
        return false;
    }
    return module->isThreadSafe() && !module->isBlocking() && !hasBreakpoint(runtimeModuleName(module));
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
    // 阶6 五轮：暂停态清理自身加锁（递归锁，允许已持锁调用者重入），
    // 与 isPausedAtBreakpoint()/resume()/executeRun 的锁内读取串行化。
    QMutexLocker locker(&m_lifecycleMutex);
    m_pauseResumeModule.clear();
    m_pausePipelineData = ImageData();
    m_breakpointPausedAt = QDateTime();
    m_pausedAtBreakpoint = false;
    m_skipBreakpointOnce = false;
    // 后台执行（设计 §5.3）：清理暂停原因与用户暂停请求，防止旧 resume 重启。
    m_pauseReason = PauseReason::None;
    m_userPauseRequested.store(false, std::memory_order_release);
}

ImageData RunEngine::lastOutput() const {
    QMutexLocker locker(&m_lastOutputMutex);
    return m_lastOutput;
}

QString RunEngine::lastOutputModuleName() const {
    QMutexLocker locker(&m_lastOutputMutex);
    return m_lastOutputModuleName;
}

ImageData RunEngine::moduleOutput(const QString& moduleName) const {
    QMutexLocker locker(&m_lastOutputMutex);
    return m_moduleOutputs.value(moduleName);
}

RunEngine::CurrentImageInputSnapshot
RunEngine::currentExecutionModuleImageInput(const QString& moduleName, const QString& upstreamSourceModuleName,
                                            ImageData& image) const {
    image = ImageData();

    ImageData pipelineData;
    QMap<QString, PortValueMap> nodeOutputs;
    QString stepCursor;
    QString pausedModule;
    QString runId;
    RunState contextState = RunState::Idle;
    int lifecycleGeneration = 0;
    bool pausedContext = false;
    {
        QMutexLocker lifecycleLocker(&m_lifecycleMutex);
        const bool stepContext = !m_stepCurrentModuleName.isEmpty() && !m_executing.load() && !m_maintenance.load();
        pausedContext =
            state() == RunState::Paused && m_pausedAtBreakpoint && !m_executing.load() && !m_maintenance.load();
        if (!stepContext && !pausedContext)
            return CurrentImageInputSnapshot::NoContext;
        pipelineData = pausedContext ? m_pausePipelineData : m_stepPipelineData;
        stepCursor = m_stepCurrentModuleName;
        pausedModule = m_pauseResumeModule;
        runId = m_runId;
        contextState = state();
        lifecycleGeneration = m_lifecycleGeneration.load();
        // A step/paused context is quiescent here; lifecycle locking prevents a new
        // execution or maintenance lease until this consistent output snapshot is copied.
        nodeOutputs = m_nodeOutputs;
    }
    const bool upstreamExecuted = nodeOutputs.contains(upstreamSourceModuleName);

    PortValueMap inputs;
    bool targetFound = false;
    {
        QReadLocker moduleLocker(&m_moduleLock);
        ModuleBase* module = m_moduleMap.value(moduleName, nullptr);
        if (module) {
            targetFound = true;
            bool imageInputIsMultiple = false;
            for (const PortSpec& input : module->inputPorts()) {
                if (input.id == QLatin1String("image")) {
                    imageInputIsMultiple = input.multiple;
                    break;
                }
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
            if (incoming.isEmpty()) {
                inputs.insert(QStringLiteral("image"), QVariant::fromValue(pipelineData));
            } else if (!imageInputIsMultiple) {
                for (const ModuleConnection& connection : incoming) {
                    if (connection.toPort != QLatin1String("image"))
                        continue;
                    const PortValueMap upstream = nodeOutputs.value(connection.fromModuleId);
                    if (upstream.contains(connection.fromPort))
                        inputs.insert(QStringLiteral("image"), upstream.value(connection.fromPort));
                }
            }
        }
    }

    const QVariant imageInput = inputs.value(QStringLiteral("image"));
    const bool inputAvailable = imageInput.canConvert<ImageData>() && !imageInput.value<ImageData>().isEmpty();
    if (inputAvailable)
        image = imageInput.value<ImageData>();

    {
        QMutexLocker lifecycleLocker(&m_lifecycleMutex);
        const bool unchangedContext =
            lifecycleGeneration == m_lifecycleGeneration.load() && !m_executing.load() && !m_maintenance.load() &&
            state() == contextState &&
            (pausedContext ? (m_pausedAtBreakpoint && m_pauseResumeModule == pausedModule && m_runId == runId)
                           : (!m_stepCurrentModuleName.isEmpty() && m_stepCurrentModuleName == stepCursor));
        if (!unchangedContext || !targetFound) {
            image = ImageData();
            return CurrentImageInputSnapshot::ContextChanged;
        }
    }
    if (!upstreamExecuted)
        return CurrentImageInputSnapshot::UpstreamNotExecuted;
    if (!inputAvailable)
        return CurrentImageInputSnapshot::NotMaterialized;
    return CurrentImageInputSnapshot::Available;
}

bool RunEngine::runOutputSnapshot(const QString& runId, const QString& moduleName, ImageData& image,
                                  QMap<QString, QVariant>& values, PortValueMap& ports) const {
    QMutexLocker runLocker(&m_runObservationMutex);
    const bool current =
        m_runObservation.runId == runId && m_runObservation.finished && m_runObservation.result.success;
    const bool recent =
        m_lastFinishedRun.runId == runId && m_lastFinishedRun.finished && m_lastFinishedRun.result.success;
    if (runId.isEmpty() || (!current && !recent))
        return false;
    if (current) {
        QMutexLocker valueLocker(&m_outputMutex);
        QMutexLocker imageLocker(&m_lastOutputMutex);
        values = m_outputMap.value(moduleName);
        image = m_moduleOutputs.value(moduleName);
        ports = m_runPortOutputs.value(moduleName);
    } else {
        values = m_lastFinishedValues.value(moduleName);
        image = m_lastFinishedImages.value(moduleName);
        ports = m_lastFinishedPortOutputs.value(moduleName);
    }
    const QMap<QString, QVariant> imageData = image.allData();
    for (auto it = imageData.cbegin(); it != imageData.cend(); ++it)
        if (!values.contains(it.key()))
            values.insert(it.key(), it.value());
    return true;
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

RunObservation RunEngine::runObservation(const QString& runId) const {
    QMutexLocker locker(&m_runObservationMutex);
    if (!m_cycleSessionObservation.runId.isEmpty() &&
        (runId == m_cycleSessionObservation.runId || (runId.isEmpty() && !m_cycleSessionObservation.finished)))
        return m_cycleSessionObservation;
    if ((runId.isEmpty() || runId == m_runObservation.runId) && !m_runObservation.runId.isEmpty())
        return m_runObservation;
    if (runId == m_lastFinishedRun.runId)
        return m_lastFinishedRun;
    return {};
}

RunResult RunEngine::lastRunResult() const {
    QMutexLocker locker(&m_runObservationMutex);
    return m_lastFinishedRun.result;
}

QString RunEngine::currentModuleName() const {
    QMutexLocker locker(&m_runObservationMutex);
    return m_runObservation.finished ? QString() : m_runObservation.currentModule;
}

RunExecutionCandidates RunEngine::runExecutionCandidates() const {
    RunExecutionCandidates result;
    QMutexLocker lifecycleLocker(&m_lifecycleMutex);
    {
        QMutexLocker observationLocker(&m_runObservationMutex);
        result.project = m_loadedProject.data();
        result.projectFingerprint = m_loadedProjectFingerprint;
    }
    if (m_executing.load(std::memory_order_acquire) || m_maintenance.load(std::memory_order_acquire))
        return result;

    QReadLocker moduleLocker(&m_moduleLock);
    QStringList order = m_executionOrder;
    if (order.isEmpty()) {
        for (ModuleBase* module : m_modules)
            order.append(runtimeModuleName(module));
    }
    if (order.isEmpty()) {
        result.complete = true;
        return result;
    }

    for (const QString& moduleId : order) {
        if (!m_disabledModules.contains(moduleId))
            result.moduleIds.append(moduleId);
    }
    result.complete = true;
    result.mayRepeat = !m_loopBackEdges.isEmpty();
    for (const QString& moduleId : result.moduleIds) {
        ModuleBase* module = m_moduleMap.value(moduleId, nullptr);
        if (module && (module->flowControlType() == ControlFlowType::Loop ||
                       module->flowControlType() == ControlFlowType::While || m_reentrantModules.contains(moduleId))) {
            result.mayRepeat = true;
            break;
        }
    }
    return result;
}

RunExecutionCandidates RunEngine::stepExecutionCandidates() const {
    RunExecutionCandidates result;
    QMutexLocker lifecycleLocker(&m_lifecycleMutex);
    {
        QMutexLocker observationLocker(&m_runObservationMutex);
        result.project = m_loadedProject.data();
        result.projectFingerprint = m_loadedProjectFingerprint;
    }
    if (m_executing.load(std::memory_order_acquire) || m_maintenance.load(std::memory_order_acquire))
        return result;

    QReadLocker moduleLocker(&m_moduleLock);
    QStringList order = m_executionOrder;
    if (order.isEmpty()) {
        for (ModuleBase* module : m_modules)
            order.append(runtimeModuleName(module));
    }
    if (order.isEmpty()) {
        result.complete = false;
        return result;
    }

    QString target = m_stepCurrentModuleName;
    if (target.isEmpty() && !m_controlEdges.isEmpty()) {
        QSet<QString> hasIncoming;
        for (const ModuleConnection& connection : m_connections) {
            if (!m_loopBackEdges.contains(connectionKey(connection)))
                hasIncoming.insert(connection.toModuleId);
        }
        for (const QString& moduleId : order) {
            if (!hasIncoming.contains(moduleId)) {
                target = moduleId;
                break;
            }
        }
    } else if (target.isEmpty()) {
        target = order.first();
    }

    if (target.isEmpty() || !m_moduleMap.contains(target))
        return result;
    if (!m_disabledModules.contains(target))
        result.moduleIds.append(target);
    result.complete = true;
    return result;
}

RunExecutionCandidates RunEngine::resumeExecutionCandidates() const {
    RunExecutionCandidates result;
    QMutexLocker lifecycleLocker(&m_lifecycleMutex);
    {
        QMutexLocker observationLocker(&m_runObservationMutex);
        result.project = m_loadedProject.data();
        result.projectFingerprint = m_loadedProjectFingerprint;
    }
    if (state() != RunState::Paused || !m_pausedAtBreakpoint || m_pauseResumeModule.isEmpty() ||
        m_executing.load(std::memory_order_acquire) || m_maintenance.load(std::memory_order_acquire))
        return result;

    QReadLocker moduleLocker(&m_moduleLock);
    const QString start = m_pauseResumeModule;
    if (!m_moduleMap.contains(start))
        return result;

    if (!m_controlEdges.isEmpty()) {
        QSet<QString> visited;
        QSet<QString> active;
        std::function<void(const QString&)> visit = [&](const QString& moduleId) {
            if (active.contains(moduleId)) {
                result.mayRepeat = true;
                return;
            }
            if (visited.contains(moduleId))
                return;
            visited.insert(moduleId);
            active.insert(moduleId);
            if (!m_disabledModules.contains(moduleId))
                result.moduleIds.append(moduleId);
            for (const ModuleConnection& edge : m_controlEdges) {
                if (edge.fromModuleId != moduleId)
                    continue;
                if (m_loopBackEdges.contains(connectionKey(edge)))
                    result.mayRepeat = true;
                visit(edge.toModuleId);
            }
            active.remove(moduleId);
        };
        visit(start);
        result.complete = true;
        return result;
    }

    QStringList order = m_executionOrder;
    if (order.isEmpty()) {
        for (ModuleBase* module : m_modules)
            order.append(runtimeModuleName(module));
    }
    const int startIndex = order.indexOf(start);
    if (startIndex < 0)
        return result;
    QSet<QString> candidates;
    for (int i = startIndex; i < order.size(); ++i) {
        candidates.insert(order.at(i));
        if (!m_disabledModules.contains(order.at(i)))
            result.moduleIds.append(order.at(i));
    }
    if (!m_loopBackEdges.isEmpty()) {
        for (const ModuleConnection& edge : m_connections) {
            if (!m_loopBackEdges.contains(connectionKey(edge)) || !candidates.contains(edge.fromModuleId))
                continue;
            result.mayRepeat = true;
            if (!m_disabledModules.contains(edge.toModuleId) && !result.moduleIds.contains(edge.toModuleId))
                result.moduleIds.append(edge.toModuleId);
        }
    }
    for (const QString& moduleId : result.moduleIds) {
        ModuleBase* module = m_moduleMap.value(moduleId, nullptr);
        if (module &&
            (module->flowControlType() == ControlFlowType::Loop ||
             module->flowControlType() == ControlFlowType::While ||
             module->flowControlType() == ControlFlowType::LoopEnd ||
             module->flowControlType() == ControlFlowType::WhileEnd || m_reentrantModules.contains(moduleId))) {
            result.mayRepeat = true;
            break;
        }
    }
    result.complete = true;
    return result;
}

QString RunEngine::lastRunId() const {
    QMutexLocker locker(&m_runObservationMutex);
    if (!m_cycleSessionObservation.runId.isEmpty() && !m_cycleSessionObservation.finished)
        return m_cycleSessionObservation.runId;
    return m_runObservation.runId;
}

void RunEngine::onTimerTick() {
    // 周期模式保持既有定时器时序；Agent 单次运行单独使用 runOnceAsync()。
    QString sessionId;
    {
        QMutexLocker locker(&m_lifecycleMutex);
        sessionId = m_cycleSessionId;
    }
    executeCycleTick(sessionId);
}

void RunEngine::executeCycleTick(const QString& sessionId) {
    if (sessionId.isEmpty())
        return;
    QJsonObject agentFolderAuthorization;
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (sessionId != m_cycleSessionId)
            return;
        agentFolderAuthorization = m_cycleAgentFolderDeleteAuthorization;
    }
    executeRun(RunIntent::CycleTick, sessionId, QString(), 0, agentFolderAuthorization);
}

bool RunEngine::executeRun(RunIntent intent, const QString& expectedCycleSessionId, const QString& expectedResumeRunId,
                           int expectedLifecycleGeneration, const QJsonObject& agentFolderAuthorization) {
    bool resuming = false;
    QString resumeModule;
    ImageData resumeData;
    RunMode mode = RunMode::RunOnce;
    if (!tryBeginForRun(intent, resuming, resumeModule, resumeData, mode, expectedCycleSessionId, expectedResumeRunId,
                        expectedLifecycleGeneration, agentFolderAuthorization))
        return false;
    executeAcquiredRun(resuming, resumeModule, resumeData, mode);
    if (state() != RunState::Running && state() != RunState::Paused && !isExecuting())
        releaseVariableExecutionLease();
    return true;
}

bool RunEngine::executeRunAsync(RunIntent intent, QString* acceptedRunId, const QJsonObject& agentFolderAuthorization) {
    bool resuming = false;
    QString resumeModule;
    ImageData resumeData;
    RunMode mode = RunMode::RunOnce;
    if (!tryBeginForRun(intent, resuming, resumeModule, resumeData, mode, QString(), QString(), 0,
                        agentFolderAuthorization))
        return false;
    if (acceptedRunId)
        *acceptedRunId = m_runId; // 执行租约已取得；下一次运行此时不能覆盖 ID
    m_runPool.start([this, resuming, resumeModule, resumeData, mode]() {
        executeAcquiredRun(resuming, resumeModule, resumeData, mode);
        if (state() != RunState::Running && state() != RunState::Paused && !isExecuting())
            releaseVariableExecutionLease();
    });
    return true;
}

void RunEngine::releaseVariableExecutionLease() {
    if (!m_variableExecutionLeaseHeld.exchange(false, std::memory_order_acq_rel))
        return;
    GlobalVarManager::instance().endExecutionLease();
}

void RunEngine::executeAcquiredRun(bool resuming, const QString& resumeModule, const ImageData& resumeData,
                                   RunMode mode) {
    ExecutionFinalizer finalizer(this, ExecutionOutcome::Failed);
    try {
        executeAcquiredRunImpl(resuming, resumeModule, resumeData, mode, finalizer);
    } catch (const std::exception& error) {
        reportExecutionFault(finalizer, QString::fromUtf8(error.what()));
    } catch (...) {
        reportExecutionFault(finalizer, tr("Unknown engine exception"));
    }
}

void RunEngine::reportExecutionFault(ExecutionFinalizer& finalizer, const QString& message) {
    const QString error = message.trimmed().isEmpty() ? tr("Unknown engine exception") : message.trimmed();
    RunResult result;
    result.runId = m_runId;
    result.success = false;
    result.errorCode = -1;
    result.errorMessage = error;
    result.elapsedMs = m_runStartTime.isValid() ? m_runStartTime.msecsTo(QDateTime::currentDateTime()) : 0;
    result.finishedTime = QDateTime::currentDateTime();
    Logger::instance().error(error, "Run");
    finalizer.finish(ExecutionOutcome::Failed);
    recordCompletedObservation(result, state());
    releaseVariableExecutionLease();
    emit errorOccurred(error);
    emit runFinished(result);
    emit stateChanged(state());
}

void RunEngine::executeAcquiredRunImpl(bool resuming, const QString& resumeModule, const ImageData& resumeData,
                                       RunMode mode, ExecutionFinalizer& finalizer) {
    if (m_testFaultInjection)
        m_testFaultInjection();

    const QString acceptedRunId = m_runId;
    QString cycleSessionId;
    if (mode == RunMode::RunCycle) {
        QMutexLocker observationLocker(&m_runObservationMutex);
        cycleSessionId = m_runObservation.sessionId;
    }
    if (!resuming) {
        {
            QReadLocker locker(&m_moduleLock);
            if (m_modules.isEmpty()) {
                RunResult result;
                result.runId = acceptedRunId;
                result.success = false;
                result.errorCode = -1;
                result.errorMessage = tr("No modules to run");
                result.elapsedMs = 0;
                result.finishedTime = QDateTime::currentDateTime();
                // 阶6 五轮：原子收尾，不覆盖 stop() 的 Stopped。
                const bool boundedCycle = mode == RunMode::RunCycle && m_cycleMaxIterations > 0;
                const bool retainCycle =
                    mode == RunMode::RunCycle && retainsCycleSessionAfterFailure() && !boundedCycle;
                finalizeAbortedRun(mode, result, retainCycle);
                if (boundedCycle) {
                    finishBoundedCycleAfterFailure(cycleSessionId, result);
                    emit stateChanged(state());
                    emit runFinished(result);
                    emit cycleStopped();
                    QMutexLocker locker(&m_lifecycleMutex);
                    m_cycleFailureNotificationPending = false;
                } else if (mode == RunMode::RunCycle && !retainCycle) {
                    emit cycleStopped();
                    emit stateChanged(state());
                    emit runFinished(result);
                } else {
                    emit stateChanged(state());
                    emit runFinished(result);
                }
                if (retainCycle)
                    scheduleCycleTimerAfterRunFinished(cycleSessionId);
                return;
            }
        }

        QString failedModule;
        QString failedPort;
        const QStringList validationErrors = validateFlow(&failedModule, &failedPort);
        if (!validationErrors.isEmpty()) {
            RunResult result;
            result.runId = acceptedRunId;
            result.success = false;
            result.errorCode = ExecError::TypeMismatch;
            result.errorMessage = validationErrors.join(QStringLiteral("\n"));
            result.failedModule = failedModule;
            result.failedPort = failedPort;
            result.diagnostics = result.errorMessage;
            result.elapsedMs = 0;
            result.finishedTime = QDateTime::currentDateTime();
            emit errorOccurred(result.errorMessage);
            // 阶6 七轮（P0-2）：仅一次原子终结（不再先 endExecutionCleanup 重复释放）。
            const bool boundedCycle = mode == RunMode::RunCycle && m_cycleMaxIterations > 0;
            const bool retainCycle = mode == RunMode::RunCycle && retainsCycleSessionAfterFailure() && !boundedCycle;
            finalizeAbortedRun(mode, result, retainCycle);
            if (boundedCycle) {
                finishBoundedCycleAfterFailure(cycleSessionId, result);
                emit stateChanged(state());
                emit runFinished(result);
                emit cycleStopped();
                QMutexLocker locker(&m_lifecycleMutex);
                m_cycleFailureNotificationPending = false;
            } else if (mode == RunMode::RunCycle && !retainCycle) {
                emit cycleStopped();
                emit stateChanged(state());
                emit runFinished(result);
            } else {
                emit stateChanged(state());
                emit runFinished(result);
            }
            if (retainCycle)
                scheduleCycleTimerAfterRunFinished(cycleSessionId);
            return;
        }

        emit stateChanged(state());
        emit runStarted();

        clearBreakpointPauseState();
        clearOutputs();
        {
            QMutexLocker nodeOutputsLocker(&m_lifecycleMutex);
            m_nodeOutputs.clear();
        }
        buildModuleTree();
    } else {
        // 阶6 七轮：恢复分支的暂停数据已由 tryBeginForRun 在锁内转移，此处仅通知。
        emit stateChanged(state());
    }

    QString currentModule;
    ImageData pipelineData;
    if (resuming) {
        currentModule = resumeModule;
        pipelineData = resumeData;
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

    // 阶6 七轮：恢复目标模块若已不存在（残留暂停态），不得对其解引用；按中止收尾。
    if (resuming && !getModule(currentModule)) {
        RunResult result;
        result.runId = acceptedRunId;
        result.success = false;
        result.errorCode = -1;
        result.errorMessage = tr("Resume target module no longer exists");
        result.elapsedMs = 0;
        result.finishedTime = QDateTime::currentDateTime();
        const bool retainCycle = mode == RunMode::RunCycle && retainsCycleSessionAfterFailure();
        finalizeAbortedRun(mode, result, retainCycle);
        emit stateChanged(state());
        emit runFinished(result);
        if (retainCycle)
            scheduleCycleTimerAfterRunFinished(cycleSessionId);
        return;
    }

    // 阶段 D1: 显式控制图存在时走激活队列；否则走 legacy flowControlType 调度
    if (!m_controlEdges.isEmpty()) {
        executeRunWithControlGraph(pipelineData);
    } else {
        executeRunLegacy(pipelineData, m_runAllSuccess, m_runFirstError);
    }

    // 阶6 复核（二轮）：内层只报告"命中断点"，暂停态在此于生命周期锁内提交；
    // 内层不提前暴露 m_executing=false。若命中断点同时 stop() 并发到达
    // （m_stopPending），放弃暂停、走正常结束路径，避免数据竞争与错误恢复。
    PauseReason pauseReason = PauseReason::None;
    {
        QMutexLocker locker(&m_lifecycleMutex);
        pauseReason = m_pauseReason;
    }
    if (pauseReason != PauseReason::None) {
        QString pauseModule;
        {
            QMutexLocker locker(&m_lifecycleMutex);
            pauseModule = m_pauseResumeModule;
        }
        finalizer.finish(ExecutionOutcome::Paused);
        if (state() == RunState::Paused) {
            clearActiveAgentFolderDeleteAuthorizationForPause();
            {
                QMutexLocker observationLocker(&m_runObservationMutex);
                if (m_runObservation.runId == acceptedRunId && !m_runObservation.finished)
                    m_runObservation.currentModule = pauseModule;
            }
            emit stateChanged(state());
            emit pauseHit(pauseModule, pauseReason);
            if (pauseReason == PauseReason::Breakpoint)
                emit breakpointHit(pauseModule);
            return;
        }
    }

    const int elapsedMs = m_runStartTime.msecsTo(QDateTime::currentDateTime());
    const bool allSuccess = m_runAllSuccess && state() != RunState::Stopped;
    updateStatistics(allSuccess, elapsedMs);

    RunResult result;
    result.runId = acceptedRunId;
    result.success = allSuccess;
    result.errorCode = allSuccess ? 0 : -1;
    result.errorMessage =
        allSuccess ? QString() : (m_runFirstError.isEmpty() ? tr("Flow execution failed") : m_runFirstError);
    if (!allSuccess) {
        QMutexLocker locker(&m_runObservationMutex);
        result.failedModule = m_runObservation.result.failedModule;
        result.failedPort = m_runObservation.result.failedPort;
        result.diagnostics = m_runObservation.result.diagnostics;
        if (m_runObservation.result.errorCode != 0)
            result.errorCode = m_runObservation.result.errorCode;
    }
    result.elapsedMs = elapsedMs;
    result.finishedTime = QDateTime::currentDateTime();

    // 阶6 七轮（P0-1）：清理+最终状态+释放执行权同一临界区。
    finalizeRunTail(result);
    const bool stoppedCycleAfterFailure =
        mode == RunMode::RunCycle && !result.success && stopCycleAfterRuntimeFailure(cycleSessionId);
    publishFinalDisplayFrame();
    emit runFinished(result);
    emit stateChanged(state());
    if (stoppedCycleAfterFailure) {
        emit cycleStopped();
        QMutexLocker locker(&m_lifecycleMutex);
        m_cycleFailureNotificationPending = false;
    } else if (mode == RunMode::RunCycle && result.success) {
        if (finishBoundedCycleAtLimit(cycleSessionId, result)) {
            emit cycleStopped();
        } else {
            scheduleCycleTimerAfterRunFinished(cycleSessionId);
        }
    }

}

bool RunEngine::stopCycleAfterRuntimeFailure(const QString& sessionId) {
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (sessionId.isEmpty() || sessionId != m_cycleSessionId || runMode() != RunMode::RunCycle ||
            state() != RunState::Running)
            return false;
        ++m_lifecycleGeneration;
        m_scheduleCycleAfterRunFinished = false;
        m_cycleFirstIterationDeferred = false;
        m_cycleFailureNotificationPending = true;
        m_cycleSessionId.clear();
        m_cycleAgentFolderDeleteAuthorization = {};
        m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    }
    stopCycleTimer();
    return true;
}

bool RunEngine::finishBoundedCycleAfterFailure(const QString& sessionId, const RunResult& result) {
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (m_cycleMaxIterations <= 0 || sessionId.isEmpty() || sessionId != m_cycleSessionId)
            return false;
        {
            QMutexLocker observationLocker(&m_runObservationMutex);
            if (m_cycleSessionObservation.runId != sessionId)
                return false;
            m_cycleSessionObservation.finished = true;
            m_cycleSessionObservation.state = RunState::Idle;
            m_cycleSessionObservation.result = result;
        }
        ++m_lifecycleGeneration;
        m_scheduleCycleAfterRunFinished = false;
        m_cycleFirstIterationDeferred = false;
        m_cycleFailureNotificationPending = true;
        m_cycleSessionId.clear();
        m_cycleAgentFolderDeleteAuthorization = {};
        m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    }
    stopCycleTimer();
    return true;
}

void RunEngine::scheduleCycleTimerAfterRunFinished(const QString& sessionId) {
    QMutexLocker locker(&m_lifecycleMutex);
    if (!m_scheduleCycleAfterRunFinished || sessionId.isEmpty() || sessionId != m_cycleSessionId ||
        state() != RunState::Running || !m_cycleTimer)
        return;
    m_cycleTimer->start(m_cycleIntervalMs);
}

bool RunEngine::finishBoundedCycleAtLimit(const QString& sessionId, const RunResult& result) {
    {
        QMutexLocker locker(&m_lifecycleMutex);
        if (m_cycleMaxIterations <= 0 || sessionId.isEmpty() || sessionId != m_cycleSessionId ||
            runMode() != RunMode::RunCycle || state() != RunState::Running)
            return false;

        QMutexLocker observationLocker(&m_runObservationMutex);
        if (m_cycleSessionObservation.completedCycles < m_cycleMaxIterations)
            return false;
        m_cycleSessionObservation.finished = true;
        m_cycleSessionObservation.state = RunState::Idle;
        m_cycleSessionObservation.result = result;
        ++m_lifecycleGeneration;
        m_scheduleCycleAfterRunFinished = false;
        m_cycleFirstIterationDeferred = false;
        m_cycleSessionId.clear();
        m_cycleAgentFolderDeleteAuthorization = {};
        m_runMode.store(static_cast<int>(RunMode::None), std::memory_order_release);
        m_state.store(static_cast<int>(RunState::Idle), std::memory_order_release);
    }
    stopCycleTimer();
    emit stateChanged(RunState::Idle);
    return true;
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
            // 阶6 复核（二轮）：内层只记录"命中断点"数据，不提前释放执行权、
            // 不置 Paused；由外层 executeRun 在生命周期锁内统一提交，
            // 避免 stop() 在内外层之间并发清除暂停数据。
            setInnerPauseLocked(mod, pipelineData, PauseReason::Breakpoint);
            return;
        }

        // 后台执行（设计 §5.3）：用户暂停请求在模块边界生效——保存下一待执行模块与
        // 流水线数据，由外层统一提交 Paused；不得在模块执行中途打断。
        if (takeUserPauseRequest(mod, pipelineData)) {
            return;
        }

        // 仅并发执行明确声明线程安全、非阻塞、启用且没有断点的就绪模块。
        // G-fix4: blocking 模块（文件 I/O 等）不进并行批次，避免占满线程池。
        // 后台执行（设计 §6.3）：并行批次资格额外要求执行亲和性为 worker（gui 模块留在
        // 串行路径，由 executeModule 的亲和性分流处理）。
        QStringList batch;
        batch.append(m_controlQueue.takeFirst());
        if (m_reentrantModules.contains(mod))
            m_controlActivated.remove(mod);
        ModuleBase* firstModule = getModule(mod);
        const bool canBuildParallelBatch =
            parallelThreadCount() > 1 && isParallelEligible(firstModule) && !isModuleDisabled(mod);
        while (canBuildParallelBatch && !m_controlQueue.isEmpty()) {
            const QString nextMod = m_controlQueue.first();
            ModuleBase* nextModule = getModule(nextMod);
            if (!nextModule || isModuleDisabled(nextMod) || !isParallelEligible(nextModule))
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
        int errorCode = 0;
        QString diagnostics;
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

    // 阶6：runId/token 在调用线程快照后按值进入工作任务；池线程不得读取
    // 可能并发变化的成员字符串（m_runId）。pipelineData 按值捕获为每任务
    // 独立副本，collectModuleInputs 对其与 m_nodeOutputs 均只读；批次结果
    // 仅在下方等待结束后由调用线程写回共享状态（只读边界）。
    const QString batchRunId = m_runId;
    CancellationToken* const batchToken = m_cancellationToken;

    for (const QString& mod : batch) {
        ModuleBase* module = getModule(mod);
        if (!module) {
            ParallelResult missing;
            missing.error = tr("Parallel module is unavailable: %1").arg(mod);
            results.insert(mod, missing);
            ++finished;
            continue;
        }

        m_parallelPool.start([this, mod, module, batchRunId, batchToken, pipelineData, &results, &resultMutex, &cond,
                              &finished, &running, &maxConcurrent]() {
            ParallelResult result;
            QElapsedTimer timer;
            timer.start();
            module->setProperty(AgentFolderDeleteAuthorizationProperty,
                                folderDeleteAuthorizationForModule(module->instanceName()));
            module->setProperty(AgentFileOutputAuthorizationProperty,
                                fileOutputAuthorizationForModule(module->instanceName()));
            emit moduleStarted(mod);

            if (batchToken && batchToken->isCancelledFast()) {
                result.error = tr("Parallel module cancelled before execution: %1").arg(mod);
            } else {
                const int cur = ++running;
                int expected = maxConcurrent.load();
                while (cur > expected && !maxConcurrent.compare_exchange_weak(expected, cur)) {
                }

                module->setCancellationToken(batchToken);
                const PortValueMap portInputs = collectModuleInputs(mod, pipelineData);
                PortValueMap portOutputs;
                ExecutionContext context;
                context.runId = batchRunId;
                context.frameId = m_frameId.fetch_add(1, std::memory_order_relaxed);
                context.timestampMs = QDateTime::currentMSecsSinceEpoch();
                context.runMode = module->flowControlType();
                context.cancellationToken = batchToken;
                const ExecutionResult execution = module->execute(portInputs, portOutputs, context);

                result.success = execution.success;
                result.outputs = portOutputs;
                result.error = execution.userMessage;
                result.errorCode = execution.errorCode;
                result.diagnostics = execution.diagnostics;
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
                if (!result.success && batchToken)
                    batchToken->cancel();
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
            {
                QMutexLocker locker(&m_runObservationMutex);
                m_runPortOutputs[mod] = pr.outputs;
            }
            {
                QMutexLocker nodeOutputsLocker(&m_lifecycleMutex);
                m_nodeOutputs[mod] = pr.outputs;
            }
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
            recordModuleRuntime(
                mod, ModuleRuntimeInfo{true, true, false, pr.elapsedMs, QString(), QDateTime::currentDateTime()});
            emit moduleFinished(mod, true, pr.elapsedMs);
            activateControlSuccessors(mod);
        } else {
            {
                QMutexLocker locker(&m_runObservationMutex);
                m_runPortOutputs.remove(mod);
                if (m_runObservation.result.failedModule.isEmpty()) {
                    m_runObservation.result.failedModule = mod;
                    m_runObservation.result.errorCode = pr.errorCode;
                    m_runObservation.result.diagnostics = pr.diagnostics;
                    if (pr.diagnostics.startsWith(QLatin1String("port=")))
                        m_runObservation.result.failedPort = pr.diagnostics.section(QLatin1Char(' '), 0, 0).mid(5);
                }
            }
            m_lastExecuteResult = false;
            m_lastModuleError = pr.error.isEmpty() ? tr("Parallel module failed: %1").arg(mod) : pr.error;
            m_runAllSuccess = false;
            if (m_runFirstError.isEmpty())
                m_runFirstError = m_lastModuleError;
            recordModuleRuntime(mod, ModuleRuntimeInfo{true, false, false, pr.elapsedMs, m_lastModuleError,
                                                       QDateTime::currentDateTime()});
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
    QMutexLocker nodeOutputsLocker(&m_lifecycleMutex);
    for (const QString& moduleName : m_loopRegions.value(loopModule)) {
        m_nodeOutputs.remove(moduleName);
        m_triggeredControlEdges.remove(moduleName);
        {
            QMutexLocker locker(&m_runObservationMutex);
            m_runPortOutputs.remove(moduleName);
        }
        invalidateModuleOutput(moduleName);
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
        if (!m_controlProcessed.contains(moduleName)) {
            emit moduleSkipped(moduleName);
            recordModuleRuntime(moduleName,
                                ModuleRuntimeInfo{true, true, true, 0, QString(), QDateTime::currentDateTime()});
        }
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
            // 阶6 复核（二轮）：内层只记录"命中断点"，不提前置 Paused/释放执行权；
            // 由外层 executeRun 在生命周期锁内提交。
            setInnerPauseLocked(currentModule, pipelineData, PauseReason::Breakpoint);
            return;
        }

        // 后台执行（设计 §5.3）：用户暂停请求在模块边界生效（见控制图路径同款协议）。
        if (takeUserPauseRequest(currentModule, pipelineData)) {
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
                for (int i = curIdx + 1; i < nextIdx; ++i) {
                    emit moduleSkipped(order[i]);
                    recordModuleRuntime(
                        order[i], ModuleRuntimeInfo{true, true, true, 0, QString(), QDateTime::currentDateTime()});
                }
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

    module->setProperty(AgentFolderDeleteAuthorizationProperty,
                        folderDeleteAuthorizationForModule(module->instanceName()));
    module->setProperty(AgentFileOutputAuthorizationProperty, fileOutputAuthorizationForModule(module->instanceName()));
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
    // 后台执行（设计 §6.2）：worker 亲和或当前已在 GUI 线程时内联执行；否则用
    // BlockingQueuedConnection 回到 GUI 线程执行，避免 gui 模块跨线程使用 QObject 亲和资源。
    // 已在 GUI 线程时直接执行可避免自等待死锁（CLI/测试同步路径）。
    ExecutionResult execResult;
    const ExecutionAffinity affinity = executionAffinityFor(moduleName, module);
    auto executeInline = [&]() { execResult = module->execute(portInputs, portOutputs, execCtx); };
    QCoreApplication* app = QCoreApplication::instance();
    if (affinity == ExecutionAffinity::Worker || !app || QThread::currentThread() == app->thread()) {
        executeInline();
    } else if (!QMetaObject::invokeMethod(app, executeInline, Qt::BlockingQueuedConnection)) {
        // 无事件循环/正在退出时不得静默跳过：模块按失败处理并记录（设计 §6.2）。
        const QString dispatchError = tr("GUI-thread module dispatch failed: %1").arg(moduleName);
        Logger::instance().warning(dispatchError, "Run");
        execResult =
            ExecutionResult::fail(ExecError::Processing, dispatchError, QStringLiteral("affinity_dispatch_failed"));
    }
    bool success = execResult.success;
    if (success) {
        QMutexLocker nodeOutputsLocker(&m_lifecycleMutex);
        m_nodeOutputs[moduleName] = portOutputs; // 缓存每端口输出供下游/单步使用
        QMutexLocker locker(&m_runObservationMutex);
        m_runPortOutputs[moduleName] = portOutputs;
    } else {
        QMutexLocker locker(&m_runObservationMutex);
        m_runPortOutputs.remove(moduleName);
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
    // design §13 #3（t26 F2）：gui 亲和模块单次执行超过 100ms 时告警，
    // 提示按审计流程将该模块迁移到 worker（不阻断执行，仅让现场可定位）。
    if (affinity == ExecutionAffinity::Gui && elapsedMs > 100) {
        Logger::instance().warning(
            QStringLiteral("gui 模块 %1 耗时 %2 ms，建议审计迁移到 worker").arg(moduleName).arg(elapsedMs), "Run");
    }

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
        // 后台执行（设计 §7.1）：模块成功且输出含 image 端口时，在执行线程内发布深拷贝帧。
        publishDisplayFrame(moduleName, output, static_cast<quint64>(execCtx.frameId));
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
        locker.unlock();
        QMutexLocker observationLocker(&m_runObservationMutex);
        if (m_runObservation.result.failedModule.isEmpty()) {
            m_runObservation.result.failedModule = moduleName;
            m_runObservation.result.errorCode = execResult.errorCode;
            m_runObservation.result.diagnostics =
                execResult.diagnostics.isEmpty() ? m_lastModuleError : execResult.diagnostics;
            if (execResult.diagnostics.startsWith(QLatin1String("port=")))
                m_runObservation.result.failedPort = execResult.diagnostics.section(QLatin1Char(' '), 0, 0).mid(5);
        }
    }

    m_lastExecuteResult = success;

    {
        ModuleRuntimeInfo runtimeInfo;
        runtimeInfo.valid = true;
        runtimeInfo.success = success;
        runtimeInfo.skipped = false;
        runtimeInfo.elapsedMs = static_cast<int>(elapsedMs);
        runtimeInfo.errorMessage = success ? QString() : m_lastModuleError;
        runtimeInfo.finishedAt = QDateTime::currentDateTime();
        recordModuleRuntime(moduleName, runtimeInfo);
    }

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
        {
            QMutexLocker nodeOutputsLocker(&m_lifecycleMutex);
            m_nodeOutputs[moduleName] = bypass;
        }
        if (bypass.contains(QStringLiteral("image"))) {
            QMutexLocker locker(&m_lastOutputMutex);
            m_moduleOutputs[moduleName] = bypass.value(QStringLiteral("image")).value<ImageData>();
        }
        emit moduleSkipped(moduleName);
        recordModuleRuntime(moduleName,
                            ModuleRuntimeInfo{true, true, true, 0, QString(), QDateTime::currentDateTime()});
        m_lastExecuteResult = true; // 旁路不算失败
        m_lastControlResult = true;
        return;
    }

    executeModule(moduleName, pipelineData);
}

QList<RunEngine::FlowIssue> RunEngine::validateFlowIssues() const {
    QList<FlowIssue> issues;
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
                FlowIssue issue;
                issue.code = QStringLiteral("missing_required_input");
                issue.node = id;
                issue.port = in.id;
                issue.message = tr("节点 %1 的必需输入端口 %2 无上游连接").arg(id, in.id);
                issues.append(issue);
            }
        }

        // 阶段 B 复核: 禁用节点无法旁路的输出端口若被下游依赖，运行前报错（含类型校验）
        if (m_disabledModules.contains(id)) {
            const QSet<QString> bypassable = bypassableOutputPorts(inPorts, outPorts);
            for (const ModuleConnection& conn : m_connections) {
                if (conn.fromModuleId != id || conn.edgeType == QLatin1String("control"))
                    continue;
                if (!bypassable.contains(conn.fromPort)) {
                    FlowIssue issue;
                    issue.code = QStringLiteral("disabled_output_bypass_conflict");
                    issue.node = id;
                    issue.port = conn.fromPort;
                    issue.message = tr("禁用节点 %1 无法旁路输出端口 %2，但被下游依赖").arg(id, conn.fromPort);
                    issues.append(issue);
                }
            }
        }
    }
    return issues;
}

QStringList RunEngine::validateFlow(QString* failedModule, QString* failedPort) const {
    if (failedModule)
        failedModule->clear();
    if (failedPort)
        failedPort->clear();
    QStringList errors;
    for (const FlowIssue& issue : validateFlowIssues()) {
        errors.append(issue.message);
        if (failedModule && failedModule->isEmpty()) {
            *failedModule = issue.node;
            if (failedPort)
                *failedPort = issue.port;
        }
    }
    return errors;
}

QStringList RunEngine::validateProjectFlow(const Project* project) {
    if (!project)
        return {tr("No project opened")};
    if (!tryAcquireMaintenance())
        return {tr("Flow is running; validation is unavailable")};

    QStringList errors;
    {
        QList<ModuleBase*> runtimeModules;
        {
            QReadLocker locker(&m_moduleLock);
            runtimeModules = m_modules;
        }
        const QList<ModuleInstance> projectModules = project->modules();
        bool runtimeMatchesProject = runtimeModules.size() == projectModules.size();
        for (int i = 0; runtimeMatchesProject && i < projectModules.size(); ++i) {
            runtimeMatchesProject = runtimeModules[i] && runtimeModuleName(runtimeModules[i]) == projectModules[i].id &&
                                    runtimePluginId(runtimeModules[i]) == projectModules[i].moduleId;
        }
        if (!runtimeMatchesProject) {
            errors.append(
                tr("Runtime modules are out of sync with the current project; synchronize before validation"));
        } else if (runtimeModules.isEmpty()) {
            errors.append(tr("Flow contains no modules"));
        } else {
            // buildExecutionOrder mutates several caches. Keep that mutation isolated in a scratch engine while
            // reusing the already initialized runtime modules; it does not initialize plugins or execute modules.
            RunEngine validator;
            for (int i = 0; i < runtimeModules.size(); ++i) {
                ModuleBase* module = runtimeModules[i];
                validator.addModule(module);
                if (!projectModules[i].enabled)
                    validator.setModuleDisabled(projectModules[i].id, true);
            }

            QString graphError;
            if (!validator.buildExecutionOrder(project, graphError) && !graphError.isEmpty())
                errors.append(graphError);
            errors.append(validator.validateFlow());
        }
    }
    releaseMaintenance();
    return errors;
}

bool RunEngine::synchronizeProjectModules(Project* project, const QList<ModuleBase*>& modules, QString& error) {
    error.clear();
    if (!project) {
        error = tr("No project opened");
        return false;
    }
    if (!tryAcquireMaintenance()) {
        error = tr("Cannot synchronize modules while the flow is running");
        return false;
    }

    const QList<ModuleInstance> projectModules = project->modules();
    QSet<QString> instanceIds;
    QSet<ModuleBase*> modulePointers;
    bool validInput = modules.size() == projectModules.size();
    for (int i = 0; validInput && i < modules.size(); ++i) {
        ModuleBase* module = modules[i];
        const ModuleInstance& instance = projectModules[i];
        validInput = module && module->isInitialized() && module->instanceName() == instance.id &&
                     runtimePluginId(module) == instance.moduleId && !instanceIds.contains(instance.id) &&
                     !modulePointers.contains(module) && !m_ownedModules.contains(module);
        if (validInput) {
            instanceIds.insert(instance.id);
            modulePointers.insert(module);
        }
    }
    if (!validInput) {
        error = tr("GUI modules do not match the current project");
        releaseMaintenance();
        return false;
    }

    clearModulesLocked();
    for (int i = 0; i < modules.size(); ++i) {
        ModuleBase* module = modules[i];
        const ModuleInstance& instance = projectModules[i];
        addModuleLocked(module);
        if (!instance.enabled)
            m_disabledModules.insert(instance.id);
        if (instance.breakpoint) {
            QMutexLocker breakpointLocker(&m_breakpointMutex);
            m_breakpoints.insert(instance.id);
        }
    }

    if (!buildExecutionOrder(project, error)) {
        clearModulesLocked();
        releaseMaintenance();
        return false;
    }
    {
        QMutexLocker observationLocker(&m_runObservationMutex);
        m_loadedProject = project;
        m_loadedProjectFingerprint = projectDigest(project);
    }
    releaseMaintenance();
    return true;
}

void RunEngine::recordModuleRuntime(const QString& moduleName, const ModuleRuntimeInfo& info) {
    QMutexLocker locker(&m_runtimeInfoMutex);
    m_moduleRuntimeInfo.insert(moduleName, info);
}

QMap<QString, RunEngine::ModuleRuntimeInfo> RunEngine::moduleRuntimeSnapshot() const {
    if (isBusy()) {
        qWarning() << "[RunEngine] moduleRuntimeSnapshot called while the engine is busy; returning empty snapshot";
        return QMap<QString, ModuleRuntimeInfo>();
    }
    QMutexLocker locker(&m_runtimeInfoMutex);
    return m_moduleRuntimeInfo;
}

void RunEngine::setNodePortOutputsForTest(const QString& moduleName, const PortValueMap& outputs) {
    QMutexLocker locker(&m_lifecycleMutex);
    m_nodeOutputs[moduleName] = outputs;
}

PortValueMap RunEngine::nodePortOutputs(const QString& moduleName) const {
    if (isBusy()) {
        qWarning() << "[RunEngine] nodePortOutputs called while the engine is busy; returning empty map";
        return PortValueMap();
    }
    QMutexLocker locker(&m_lifecycleMutex);
    return m_nodeOutputs.value(moduleName);
}

qint64 RunEngine::lastFrameId() const {
    if (isBusy()) {
        qWarning() << "[RunEngine] lastFrameId called while the engine is busy; returning 0";
        return 0;
    }
    QMutexLocker locker(&m_lifecycleMutex);
    return m_frameId.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// 后台执行（设计 §7.1/§7.3/§7.5）：执行线程显示帧快照与每视口最新帧槽位
// ---------------------------------------------------------------------------

void RunEngine::setDisplayCaptureEnabled(bool enabled) {
    m_displayCaptureEnabled.store(enabled, std::memory_order_release);
}

void RunEngine::setDisplayJobId(quint64 jobId) {
    m_displayJobId.store(jobId, std::memory_order_release);
    // 新 job：清空旧帧槽位，旧 job 的迟到通知不会再从槽位取到可渲染帧。
    clearLatestDisplayFrames();
}

void RunEngine::clearDisplayJobId() {
    m_displayJobId.store(0, std::memory_order_release);
}

DisplayFrame RunEngine::latestDisplayFrame(const QString& viewportId) const {
    QMutexLocker locker(&m_displayFrameMutex);
    return m_latestDisplayFrames.value(viewportId);
}

void RunEngine::clearLatestDisplayFrames() {
    QMutexLocker locker(&m_displayFrameMutex);
    m_latestDisplayFrames.clear();
}

QMap<QString, QVariantMap> RunEngine::displayModuleDataSnapshot() const {
    // 设计 §7.1：run 结束时一次性抓取各模块 allData() 的不可变副本（不含大图）。
    QMap<QString, QVariantMap> snapshot;
    QMutexLocker locker(&m_lastOutputMutex);
    for (auto it = m_moduleOutputs.constBegin(); it != m_moduleOutputs.constEnd(); ++it) {
        const QVariantMap data = it.value().allData();
        if (!data.isEmpty()) {
            snapshot.insert(it.key(), data);
        }
    }
    return snapshot;
}

void RunEngine::publishDisplayFrame(const QString& moduleId, const ImageData& image, quint64 frameId) {
    if (!m_displayCaptureEnabled.load(std::memory_order_acquire)) {
        return;
    }
    QImage frameImage = image.toQImage();
    if (frameImage.isNull()) {
        return;
    }
    // 设计 §7.1：在执行线程内完成深拷贝，帧脱离 ImageData/cv::Mat 的共享缓冲。
    frameImage = frameImage.copy();

    DisplayFrame frame;
    frame.jobId = m_displayJobId.load(std::memory_order_acquire);
    frame.runId = m_runId;
    frame.frameId = frameId;
    frame.moduleId = moduleId;
    frame.viewportId = QStringLiteral("main"); // 本轮仅覆盖主视口（viewportId 作为键保留）
    frame.image = frameImage;
    {
        QMutexLocker locker(&m_displayFrameMutex);
        m_latestDisplayFrames.insert(frame.viewportId, frame);
    }
    emit displayFrameReady(frame.viewportId, frame.jobId, frame.runId);
}

void RunEngine::publishFinalDisplayFrame() {
    if (!m_displayCaptureEnabled.load(std::memory_order_acquire)) {
        return;
    }
    DisplayFrame frame;
    frame.jobId = m_displayJobId.load(std::memory_order_acquire);
    frame.runId = m_runId;
    frame.viewportId = QStringLiteral("main");
    frame.moduleDataSnapshot = displayModuleDataSnapshot();
    {
        QMutexLocker locker(&m_displayFrameMutex);
        const auto it = m_latestDisplayFrames.constFind(frame.viewportId);
        if (it != m_latestDisplayFrames.constEnd() && it.value().runId == frame.runId &&
            it.value().jobId == frame.jobId) {
            // 沿用本轮最后一帧的图像与身份；只补充 run 级结果快照。
            frame.frameId = it.value().frameId;
            frame.moduleId = it.value().moduleId;
            frame.image = it.value().image;
        }
        m_latestDisplayFrames.insert(frame.viewportId, frame);
    }
    // 即使本轮没有 2D 图像，也发布带身份的快照帧：UI 据此只更新/清除叠加，不改变画面。
    emit displayFrameReady(frame.viewportId, frame.jobId, frame.runId);
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

QString RunEngine::inferEdgeType(const ModuleConnection& connection, const QList<PortSpec>& sourceOutputs,
                                 const QList<PortSpec>& targetInputs) {
    const QString sourcePortId = connection.fromPort.isEmpty() ? QStringLiteral("image") : connection.fromPort;
    const QString targetPortId = connection.toPort.isEmpty() ? QStringLiteral("image") : connection.toPort;
    const PortSpec* output = findPort(sourceOutputs, sourcePortId);
    const PortSpec* input = findPort(targetInputs, targetPortId);
    const bool sourceIsControl = sourcePortId == QLatin1String("next") || (output && output->control);
    const bool targetIsControl = targetPortId == QLatin1String("control") || (input && input->control);
    return sourceIsControl || targetIsControl ? QStringLiteral("control") : QStringLiteral("data");
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
            conn.edgeType = inferEdgeType(conn, sourcePorts, targetPorts);
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
