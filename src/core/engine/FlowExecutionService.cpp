#include "FlowExecutionService.h"

#include "common/Logger.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <QUuid>
#include <exception>

namespace DeepLux {

/// 常驻工作线程：run() 内阻塞在 QWaitCondition 的命令循环，不使用事件循环。
class FlowExecutionService::WorkerThread : public QThread {
public:
    explicit WorkerThread(FlowExecutionService* service) : m_service(service) {}

protected:
    void run() override {
        m_service->workerLoop();
    }

private:
    FlowExecutionService* m_service = nullptr;
};

FlowExecutionService& FlowExecutionService::instance() {
    static FlowExecutionService instance;
    return instance;
}

FlowExecutionService::FlowExecutionService() : QObject(nullptr) {
    // 跨线程 queued 信号/信号转发所需的元类型（Qt5.15 亦要求显式注册）。
    qRegisterMetaType<FlowExecutionService::State>("FlowExecutionService::State");
    qRegisterMetaType<FlowExecutionService::JobType>("FlowExecutionService::JobType");
    qRegisterMetaType<PauseReason>("PauseReason");
    qRegisterMetaType<PauseReason>("DeepLux::PauseReason");
    Logger::instance().info("Flow execution service initialized", "Service");
}

FlowExecutionService::~FlowExecutionService() {
    {
        QMutexLocker locker(&m_mutex);
        m_quitRequested = true;
        m_wake.wakeAll();
    }
    WorkerThread* thread = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        thread = m_thread;
        m_thread = nullptr;
    }
    if (thread) {
        thread->wait();
        delete thread;
    }
}

bool FlowExecutionService::isActive() const {
    QMutexLocker locker(&m_mutex);
    return state() != State::Idle || m_pendingJob != JobType::None || currentJob() != JobType::None;
}

bool FlowExecutionService::startSingle() {
    return enqueue(JobType::Single);
}

bool FlowExecutionService::startSingle(JobRequest* request) {
    return enqueue(JobType::Single, request);
}

bool FlowExecutionService::startCycle() {
    return enqueue(JobType::Cycle);
}

bool FlowExecutionService::startCycle(JobRequest* request) {
    return enqueue(JobType::Cycle, request);
}

bool FlowExecutionService::stepOnce() {
    return enqueue(JobType::Step);
}

bool FlowExecutionService::stepOnce(JobRequest* request) {
    return enqueue(JobType::Step, request);
}

RunObservation FlowExecutionService::currentRunObservation() const {
    QString runId;
    {
        QMutexLocker locker(&m_mutex);
        runId = m_activeRequest.runId.isEmpty() ? m_lastRunId : m_activeRequest.runId;
    }
    return RunEngine::instance().runObservation(runId);
}

RunObservation FlowExecutionService::currentSessionObservation() const {
    QString sessionId;
    {
        QMutexLocker locker(&m_mutex);
        sessionId = m_activeRequest.sessionId.isEmpty() ? m_lastSessionId : m_activeRequest.sessionId;
    }
    return sessionId.isEmpty() ? RunEngine::instance().runObservation()
                               : RunEngine::instance().runObservation(sessionId);
}

FlowExecutionService::JobRequest FlowExecutionService::activeJobRequestSnapshot() const {
    QMutexLocker locker(&m_mutex);
    return m_activeRequest;
}

bool FlowExecutionService::enqueue(JobType job, JobRequest* request) {
    JobRequest accepted = request ? *request : JobRequest{};
    if (accepted.maxCycles < 0 ||
        (job == JobType::Cycle && !accepted.authorization.isEmpty() && accepted.maxCycles <= 0))
        return false;
    if (accepted.runId.isEmpty())
        accepted.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (job == JobType::Cycle && accepted.sessionId.isEmpty())
        accepted.sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QMutexLocker locker(&m_mutex);
    // 设计 §4.1：非 Idle 时启动/单步直接拒绝，不排队；Paused 只接受 continueRun()/stop()。
    if (state() != State::Idle || m_pendingJob != JobType::None || currentJob() != JobType::None) {
        return false;
    }
    // shutdown() 销毁线程后会置 quit 标志；此处按需重建线程并复位该标志。
    ensureThreadLocked();
    m_pendingJob = job;
    m_pendingRequest = accepted;
    m_startSettled = false;
    m_startOk = false;
    m_wake.wakeOne();

    // 启动握手：工作者取得 job 租约并提交 Running（或显式失败）后返回，保证
    // startX() 返回 true 时引擎 isBusy() 已为真（兼容 Agent/UI 的“started 即忙”语义）。
    // B7：GUI 线程等待上限固定为 StartHandshakeTimeoutMs——启动路径（含 closeEvent 期间
    // 的重试）不得被 5s 阻塞；超时后必须撤回/停止该次启动，不能留下稍后执行的 pending。
    QElapsedTimer handshakeTimer;
    handshakeTimer.start();
    while (!m_startSettled) {
        const int remaining = StartHandshakeTimeoutMs - static_cast<int>(handshakeTimer.elapsed());
        if (remaining <= 0) {
            const bool withdrawn = (m_pendingJob == job);
            if (withdrawn) {
                // 工作者尚未取走命令：直接撤回，任何模块都不会执行。
                m_pendingJob = JobType::None;
            } else {
                // 命令已被取走：请求停止，保证该 job 启动后在第一轮之前协作退出。
                m_stopRequested = true;
            }
            m_wake.wakeAll();
            Logger::instance().warning(QString("Flow start handshake timed out after %1 ms (%2)")
                                           .arg(StartHandshakeTimeoutMs)
                                           .arg(withdrawn ? "pending withdrawn" : "stop requested"),
                                       "Service");
            return false;
        }
        if (!m_wake.wait(&m_mutex, static_cast<unsigned long>(remaining))) {
            // wait 超时后回到循环顶部重新判定；剩余时间耗尽时走上面的撤回/停止分支。
        }
    }
    const bool ok = m_startSettled && m_startOk;
    if (ok && request)
        *request = accepted;
    return ok;
}

void FlowExecutionService::setStartHandshakeHookForTest(std::function<void()> hook) {
    QMutexLocker locker(&m_mutex);
    m_testStartHook = std::move(hook);
}

void FlowExecutionService::clearStartHandshakeHookForTest() {
    QMutexLocker locker(&m_mutex);
    m_testStartHook = nullptr;
}

void FlowExecutionService::settleStart(bool ok) {
    QMutexLocker locker(&m_mutex);
    m_startOk = ok;
    m_startSettled = true;
    m_wake.wakeAll();
}

void FlowExecutionService::ensureThreadLocked() {
    if (m_thread) {
        return;
    }
    // 常驻线程按需创建一次；shutdown() 销毁后下一次启动自动重建。
    m_quitRequested = false;
    m_thread = new WorkerThread(this);
    m_thread->setObjectName(QStringLiteral("FlowExecutionService"));
    m_thread->start();
}

bool FlowExecutionService::continueRun(const QJsonObject& authorization) {
    QMutexLocker locker(&m_mutex);
    if (state() != State::Paused || m_pendingJob != JobType::None) {
        return false;
    }
    if (!authorization.isEmpty())
        m_activeRequest.authorization = authorization;
    m_resumeRequested = true;
    // 防御（t11 F1）：恢复路径不得保留残留暂停请求，否则 Cycle 恢复后会立即再次进入 Paused。
    m_pauseRequested = false;
    m_wake.wakeAll();
    return true;
}

bool FlowExecutionService::pauseRun() {
    RunEngine::JobToken token;
    {
        QMutexLocker locker(&m_mutex);
        // 设计 §4.1（t11 F1）：Paused/Stopping 不接受新的暂停请求（Paused 只接受
        // continueRun()/stop()）；仅 Running，或启动窗口（Idle 且 currentJob()!=None）
        // 接受，保证“启动即暂停”能力。
        const bool startingWindow = state() == State::Idle && currentJob() != JobType::None;
        if (state() != State::Running && !startingWindow) {
            return false;
        }
        m_pauseRequested = true;
        token = m_leaseToken;
    }
    // 设计 §5.3 #4 / t6 O2：模块边界暂停请求在引擎空闲（Cycle 轮间/启动窗口）时
    // pauseForJob 返回 false 属正常语义——请求已登记，工作线程会在下一模块边界等待，
    // 不得把该 false 当成失败。
    if (token.isValid()) {
        (void) RunEngine::instance().pauseForJob(token);
    }
    m_wake.wakeAll();
    return true;
}

void FlowExecutionService::stop() {
    {
        QMutexLocker locker(&m_mutex);
        const bool hadJob = state() != State::Idle || m_pendingJob != JobType::None || currentJob() != JobType::None;
        if (!hadJob) {
            return; // 幂等：无 job 时不做任何事
        }
        m_stopRequested = true;
        m_pendingJob = JobType::None; // 尚未出队的命令直接取消
        m_wake.wakeAll();
    }
    // 安全阀：任何线程可调；租约活跃时 RunEngine::stop() 同时置 job 级取消标志，
    // 保证工作线程不会在停止后再启动下一轮（设计 §5.3 #5）。
    RunEngine::instance().stop();
}

bool FlowExecutionService::shutdown() {
    WorkerThread* thread = nullptr;
    bool nonIdle = false;
    {
        QMutexLocker locker(&m_mutex);
        nonIdle = state() != State::Idle || m_pendingJob != JobType::None || currentJob() != JobType::None;
        if (nonIdle) {
            // 非 Idle：只请求停止，不销毁仍可能访问 RunEngine 的工作线程（设计 §5.3 #7）。
            // t11 F3：该分支无条件返回 false——不得在“检查后已变 Idle”时返回 true，
            // 因为线程并未经过 wait()+delete（true 只在真实销毁路径返回）。
            m_stopRequested = true;
            m_pendingJob = JobType::None;
            m_wake.wakeAll();
        } else {
            thread = m_thread;
            m_thread = nullptr;
            m_quitRequested = true;
            m_wake.wakeAll();
        }
    }
    if (nonIdle) {
        RunEngine::instance().stop();
        return false;
    }
    if (thread) {
        thread->wait();
        delete thread;
    }
    return true;
}

void FlowExecutionService::workerLoop() {
    for (;;) {
        JobType job = JobType::None;
        JobRequest request;
        {
            QMutexLocker locker(&m_mutex);
            while (!m_quitRequested && m_pendingJob == JobType::None) {
                m_wake.wait(&m_mutex);
            }
            if (m_quitRequested) {
                return;
            }
            job = m_pendingJob;
            request = m_pendingRequest;
            m_pendingJob = JobType::None;
            // 新 job 的清零：停止/暂停/继续标志属于上一次 job 的窗口语义。
            m_stopRequested = false;
            m_pauseRequested = false;
            m_resumeRequested = false;
        }
        if (job == JobType::None) {
            continue;
        }
        runJob(job, request);
    }
}

void FlowExecutionService::runJob(JobType job, const JobRequest& request) {
    RunEngine& engine = RunEngine::instance();
    // 从出队起即对外可见当前 job（isActive()/stop() 覆盖“已出队、租约未建立”的启动窗口）。
    // 每个 job 分配单调序号：等待方用它区分“本次 job 的完成信号”与上一次 job 的排队信号。
    const quint64 jobSerial = m_jobSerialSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    m_currentJobSerial.store(jobSerial, std::memory_order_release);
    m_job.store(static_cast<int>(job), std::memory_order_release);
    {
        QMutexLocker locker(&m_mutex);
        m_activeRequest = request;
    }

    // B7 测试注入：在启动协议最前面执行（生产构建恒为空），用于确定性验证 GUI 侧握手超时。
    std::function<void()> startHook;
    {
        QMutexLocker locker(&m_mutex);
        startHook = m_testStartHook;
    }
    if (startHook) {
        startHook();
    }

    RunEngine::JobToken token;
    QString reason;
    if (!engine.beginExternalJob(&token, &reason)) {
        m_job.store(static_cast<int>(JobType::None), std::memory_order_release);
        m_failedJobSerial.store(jobSerial, std::memory_order_release);
        const QString error = reason.isEmpty() ? QStringLiteral("engine_busy") : reason;
        Logger::instance().warning(QString("Flow job rejected: %1").arg(error), "Service");
        settleStart(false); // 启动握手：让 startX() 立即返回 false
        emit jobFailed(job, error);
        return;
    }
    // 后台执行（设计 §7.1）：job 启动时把 jobId 注入 RunEngine 的显示帧上下文，
    // 帧身份不依赖每轮会重置的全局 frameId；job 结束时清空。
    engine.setDisplayJobId(jobSerial);
    {
        QMutexLocker locker(&m_mutex);
        m_leaseToken = token;
    }
    commitState(State::Running, job);
    settleStart(true); // job 租约已建立、引擎 busy，startX() 可安全返回 true

    bool stopped = false;
    bool failed = false;
    QString error;
    try {
        switch (job) {
        case JobType::Single:
            runSingleJob(token, request, stopped, failed, error);
            break;
        case JobType::Cycle:
            runCycleJob(token, request, stopped, failed, error);
            break;
        case JobType::Step:
            runStepJob(token, request, stopped, failed, error);
            break;
        case JobType::Resume:
        case JobType::None:
            failed = true;
            error = QStringLiteral("invalid_job_type");
            break;
        }
    } catch (const std::exception& e) {
        failed = true;
        error = QString::fromUtf8(e.what());
    } catch (...) {
        failed = true;
        error = QStringLiteral("unknown_service_exception");
    }

    // 收尾：释放 job 租约（同时清理 job 级取消标志与暂停上下文），再提交 Idle。
    engine.endExternalJob(token);
    // 后台执行（设计 §7.1）：job 结束/停止时清空显示帧上下文（帧槽位保留最后一帧供叠加使用）。
    engine.clearDisplayJobId();
    {
        QMutexLocker locker(&m_mutex);
        m_leaseToken = RunEngine::JobToken{};
        m_lastRunId = m_activeRequest.runId;
        m_lastSessionId = m_activeRequest.sessionId;
        m_activeRequest = JobRequest{};
        if ((m_stopRequested || m_quitRequested) && !stopped && !failed) {
            stopped = true; // 停止请求落在收尾窗口：按停止处理，不得上报成功
        }
        m_stopRequested = false;
        m_pauseRequested = false;
        m_resumeRequested = false;
    }
    m_job.store(static_cast<int>(JobType::None), std::memory_order_release);
    m_lastJobStopped.store(stopped, std::memory_order_release);
    m_finishedJobSerial.store(jobSerial, std::memory_order_release);
    if (failed) {
        m_failedJobSerial.store(jobSerial, std::memory_order_release);
    }

    if (stopped) {
        commitState(State::Stopping, job);
    }
    commitState(State::Idle, JobType::None);
    {
        // 唤醒仍在等待命令/暂停/继续的工作线程或锁内等待者（job 已收尾）。
        QMutexLocker locker(&m_mutex);
        m_wake.wakeAll();
    }
    if (failed) {
        Logger::instance().warning(QString("Flow job failed: %1").arg(error), "Service");
        emit jobFailed(job, error);
    } else {
        emit jobFinished(job, stopped);
    }
}

void FlowExecutionService::runSingleJob(const RunEngine::JobToken& token, const JobRequest& request, bool& stopped,
                                       bool& failed, QString& error) {
    RunEngine& engine = RunEngine::instance();
    if (stopRequested()) {
        stopped = true;
        return;
    }
    // 启动窗口内到达的暂停请求：在首个模块边界前等待继续/停止（设计 §5.3 #4）。
    if (pauseRequested()) {
        const ResumeOutcome outcome = waitForResumeOrStop(token, error);
        if (outcome == ResumeOutcome::Stopped) {
            stopped = true;
            return;
        }
        if (outcome == ResumeOutcome::Failed) {
            failed = true;
            return;
        }
    }

    QString reason;
    QString acceptedRunId;
    if (!engine.runOnceForJob(token, request.authorization, request.runId, request.sessionId, request.maxCycles,
                              &acceptedRunId, &reason)) {
        if (reason == QStringLiteral("external_job_cancelled")) {
            stopped = true;
        } else {
            failed = true;
            error = reason.isEmpty() ? QStringLiteral("start_rejected") : reason;
        }
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        m_activeRequest.runId = acceptedRunId;
    }
    // 模块边界暂停（断点/用户）：保持同一 job，等待 continueRun()/stop() 后恢复。
    while (engine.isPaused()) {
        const ResumeOutcome outcome = waitForResumeOrStop(token, error);
        if (outcome == ResumeOutcome::Stopped) {
            stopped = true;
            return;
        }
        if (outcome == ResumeOutcome::Failed) {
            failed = true;
            return;
        }
    }
    stopped = (engine.state() == RunState::Stopped);
}

void FlowExecutionService::runCycleJob(const RunEngine::JobToken& token, const JobRequest& request, bool& stopped,
                                       bool& failed, QString& error) {
    RunEngine& engine = RunEngine::instance();
    int completedCycles = 0;
    for (;;) {
        if (quitRequested() || stopRequested()) {
            stopped = true;
            return;
        }
        // 轮间暂停：引擎可能已 Idle，由 Service 在下一轮启动前等待（设计 §5.3 #4）。
        if (pauseRequested() && !engine.isPaused()) {
            const ResumeOutcome outcome = waitForResumeOrStop(token, error);
            if (outcome == ResumeOutcome::Stopped) {
                stopped = true;
                return;
            }
            if (outcome == ResumeOutcome::Failed) {
                failed = true;
                return;
            }
            continue;
        }

        QString reason;
        QString acceptedRunId;
        const QString requestedRunId = completedCycles == 0 ? request.runId : QString();
        const JobRequest currentRequest = activeJobRequestSnapshot();
        if (!engine.runOnceForJob(token, currentRequest.authorization, requestedRunId, currentRequest.sessionId,
                                  request.maxCycles, &acceptedRunId, &reason)) {
            if (reason == QStringLiteral("external_job_cancelled")) {
                stopped = true;
                return;
            }
            failed = true;
            error = reason.isEmpty() ? QStringLiteral("start_rejected") : reason;
            return;
        }
        {
            QMutexLocker locker(&m_mutex);
            m_activeRequest.runId = acceptedRunId;
        }
        // 单轮内命中断点/用户暂停：等待继续后恢复被暂停段（Resume 段同属当前 job）。
        while (engine.isPaused()) {
            const ResumeOutcome outcome = waitForResumeOrStop(token, error);
            if (outcome == ResumeOutcome::Stopped) {
                stopped = true;
                return;
            }
            if (outcome == ResumeOutcome::Failed) {
                failed = true;
                return;
            }
        }
        if (stopRequested()) {
            stopped = true;
            return;
        }
        const RunObservation observation = engine.runObservation(acceptedRunId);
        if (observation.finished && !observation.result.success) {
            failed = true;
            error = observation.result.errorMessage.isEmpty() ? QStringLiteral("cycle_iteration_failed")
                                                              : observation.result.errorMessage;
            return;
        }
        ++completedCycles;
        if (request.maxCycles > 0 && completedCycles >= request.maxCycles)
            return;
        // 循环节拍：上一轮结束立即下一轮，仅让出 CPU（设计 §5.3 #1）。
        if (request.intervalMs > 0)
            QThread::msleep(static_cast<unsigned long>(request.intervalMs));
        else
            QThread::yieldCurrentThread();
    }
}

void FlowExecutionService::runStepJob(const RunEngine::JobToken& token, const JobRequest& request, bool& stopped,
                                      bool& failed, QString& error) {
    RunEngine& engine = RunEngine::instance();
    if (stopRequested()) {
        stopped = true;
        return;
    }
    QString reason;
    QString acceptedRunId;
    if (!engine.stepOnceForJob(token, request.authorization, request.runId, request.sessionId, request.maxCycles,
                               &acceptedRunId, &reason)) {
        if (reason == QStringLiteral("external_job_cancelled")) {
            stopped = true;
        } else {
            failed = true;
            error = reason.isEmpty() ? QStringLiteral("start_rejected") : reason;
        }
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        m_activeRequest.runId = acceptedRunId;
    }
    stopped = (engine.state() == RunState::Stopped);
}

FlowExecutionService::ResumeOutcome FlowExecutionService::waitForResumeOrStop(const RunEngine::JobToken& token,
                                                                              QString& error) {
    const JobType pausedJob = currentJob();
    {
        QMutexLocker locker(&m_mutex);
        m_pauseRequested = false; // 请求已在模块边界生效
    }
    commitState(State::Paused, pausedJob);

    {
        QMutexLocker locker(&m_mutex);
        while (!m_resumeRequested && !m_stopRequested && !m_quitRequested) {
            m_wake.wait(&m_mutex);
        }
        if (m_stopRequested || m_quitRequested) {
            return ResumeOutcome::Stopped;
        }
        m_resumeRequested = false;
        // 防御（t11 F1）：恢复前清除任何残留暂停请求，保证恢复后不会立即再次暂停。
        m_pauseRequested = false;
    }

    // 设计 §5.2：continueRun 提交一个 Resume 段；被暂停的执行段在此恢复。
    m_job.store(static_cast<int>(JobType::Resume), std::memory_order_release);
    commitState(State::Running, JobType::Resume);
    if (RunEngine::instance().isPaused()) {
        QString resumeReason;
        QJsonObject authorization;
        {
            QMutexLocker locker(&m_mutex);
            authorization = m_activeRequest.authorization;
        }
        if (!RunEngine::instance().resumeForJob(token, authorization, nullptr, &resumeReason)) {
            m_job.store(static_cast<int>(pausedJob), std::memory_order_release);
            error = resumeReason.isEmpty() ? QStringLiteral("resume_rejected") : resumeReason;
            return ResumeOutcome::Failed;
        }
    }
    m_job.store(static_cast<int>(pausedJob), std::memory_order_release);
    return ResumeOutcome::Resumed;
}

bool FlowExecutionService::stopRequested() const {
    QMutexLocker locker(&m_mutex);
    return m_stopRequested;
}

bool FlowExecutionService::pauseRequested() const {
    QMutexLocker locker(&m_mutex);
    return m_pauseRequested;
}

bool FlowExecutionService::quitRequested() const {
    QMutexLocker locker(&m_mutex);
    return m_quitRequested;
}

void FlowExecutionService::commitState(State state, JobType job) {
    m_state.store(static_cast<int>(state), std::memory_order_release);
    emit jobStateChanged(state, job);
}

} // namespace DeepLux
