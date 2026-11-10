#include "FlowExecutionService.h"

#include "common/Logger.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <exception>

namespace DeepLux {

namespace {

// T2.5：收尾段故障注入钩子。存放于 .cpp 私有测试状态，避免向
// FlowExecutionService 新增数据成员（类布局/ABI 不变）；读写统一由该实例的
// m_mutex 保护（服务为单例）。生产构建不会设置，恒为空。
std::function<void()> g_jobTeardownHookForTest;

// T2.5 P1：workerLoop() 骨架故障注入钩子。与收尾钩子同样存放于 .cpp 私有测试
// 状态（不新增数据成员，类布局/ABI 不变），由 m_mutex 保护。该钩子在
// workerLoop() 的每轮循环入口（持锁快照、锁外调用）执行；抛出即等价于
// workerLoop() 自身（取队列/加锁）抛出 C++ 异常，用于验证线程入口兜底后的
// 线程重建与命令清理路径。生产构建不会设置，恒为空。
std::function<void()> g_workerLoopFaultHookForTest;

} // namespace

/// 常驻工作线程：run() 内阻塞在 QWaitCondition 的命令循环，不使用事件循环。
class FlowExecutionService::WorkerThread : public QThread {
public:
    explicit WorkerThread(FlowExecutionService* service) : m_service(service) {}

protected:
    void run() override {
        // T2.5 P1：workerLoop() 除返回外，还可能把「出错时正在处理的命令」通过 out 参数
        // 交回给入口兜底。出队后、runJob() 尚未置 m_job 的窗口里发生异常时，m_job 仍是
        // None，仅凭它判断会漏报终态信号（jobFailed 不发出）。
        JobType faultyJob = JobType::None;
        try {
            m_service->workerLoop(&faultyJob);
        } catch (const std::exception& e) {
            // T2.5 最后一道保险：runJob() 负责 job 级 C++ 异常；此处只兜住 workerLoop()
            // 自身（取队列/加锁）的异常，绝不允许其逃出 QThread::run() 触发 std::terminate()。
            m_service->handleWorkerLoopFault(faultyJob, QString::fromUtf8(e.what()));
        } catch (...) {
            m_service->handleWorkerLoopFault(faultyJob, QStringLiteral("unknown_worker_exception"));
        }
        // T2.1 整改：workerLoop() 已返回、其 QMutexLocker 已释放；在线程真正结束前
        // 执行退出屏障。屏障等待不持有 m_mutex，不会阻塞 shutdown() 取锁。
        try {
            m_service->runWorkerExitBarrierForTest();
        } catch (...) {
            // 退出屏障是测试钩子：其异常同样不得逃出 QThread::run()。
        }
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
        joinWorkerThread(thread);
    }
}

void FlowExecutionService::joinWorkerThread(WorkerThread* thread) {
    if (!thread) {
        return;
    }
    const int deadline = m_shutdownDeadlineMs.load(std::memory_order_acquire);
    if (!thread->wait(static_cast<unsigned long>(qMax(0, deadline)))) {
        m_shutdownDeadlineHits.fetch_add(1, std::memory_order_relaxed);
        Logger::instance().warning(QString("Flow execution worker did not exit within %1 ms during shutdown "
                                           "(state=%2, job=%3); continuing to wait without detach")
                                       .arg(deadline)
                                       .arg(static_cast<int>(state()))
                                       .arg(static_cast<int>(currentJob())),
                                   "Service");
        // No detach/terminate: the worker may still touch this service and its modules,
        // so keep waiting until it really exits before the object is destroyed.
        thread->wait();
    }
    delete thread;
}

bool FlowExecutionService::isActive() const {
    QMutexLocker locker(&m_mutex);
    return state() != State::Idle || m_pendingJob != JobType::None || currentJob() != JobType::None;
}

bool FlowExecutionService::startSingle() {
    return enqueue(JobType::Single);
}

bool FlowExecutionService::startCycle() {
    return enqueue(JobType::Cycle);
}

bool FlowExecutionService::stepOnce() {
    return enqueue(JobType::Step);
}

bool FlowExecutionService::enqueue(JobType job) {
    // T2.5 P1：先把「已自行退出但尚未回收」的 worker 线程摘下来，在**锁外** join 并
    // delete，再回到锁内重建。worker 线程在 workerLoop() 自身抛出后由 run() 返回而结束，
    // 此时 m_thread 仍非空；若无条件复用，新命令会被写进一个已死线程的等待队列，
    // 启动握手只能超时撤回，服务从此无法再执行任务。回收必须在锁外：join 会阻塞到线程
    // 真正退出，持锁 join 会卡死所有服务入口。
    // 这里只摘指针、不提前放弃 join 语义——旧线程一定由 joinWorkerThread() 等它真正
    // 退出后 delete，绝不 detach/terminate。
    WorkerThread* retired = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        // 判定用 isFinished() 而非 !isRunning()：Qt 保证 run() 返回之后 isFinished() 才为
        // true，此后该线程绝不会再消费命令队列，可以安全回收；isRunning() 为 false 只说明
        // 「尚未开始或已结束」，不足以排除线程仍会启动并抢走命令。
        if (m_thread && m_thread->isFinished()) {
            retired = m_thread;
            m_thread = nullptr;
        }
    }
    if (retired) {
        joinWorkerThread(retired);
    }

    QMutexLocker locker(&m_mutex);
    // 设计 §4.1：非 Idle 时启动/单步直接拒绝，不排队；Paused 只接受 continueRun()/stop()。
    if (state() != State::Idle || m_pendingJob != JobType::None || currentJob() != JobType::None) {
        return false;
    }
    // shutdown() 销毁线程后会置 quit 标志；此处按需重建线程并复位该标志。
    ensureThreadLocked();
    m_pendingJob = job;
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
    return m_startSettled && m_startOk;
}

void FlowExecutionService::setStartHandshakeHookForTest(std::function<void()> hook) {
    QMutexLocker locker(&m_mutex);
    m_testStartHook = std::move(hook);
}

void FlowExecutionService::clearStartHandshakeHookForTest() {
    QMutexLocker locker(&m_mutex);
    m_testStartHook = nullptr;
}

void FlowExecutionService::setShutdownDeadlineForTest(int msecs) {
    m_shutdownDeadlineMs.store(qMax(0, msecs), std::memory_order_release);
}

int FlowExecutionService::shutdownDeadlineHitCountForTest() const {
    return m_shutdownDeadlineHits.load(std::memory_order_acquire);
}

void FlowExecutionService::setWorkerExitBarrierForTest(std::function<void()> hook) {
    QMutexLocker locker(&m_mutex);
    m_testWorkerExitBarrier = std::move(hook);
}

void FlowExecutionService::clearWorkerExitBarrierForTest() {
    QMutexLocker locker(&m_mutex);
    m_testWorkerExitBarrier = nullptr;
}

void FlowExecutionService::setJobTeardownHookForTest(std::function<void()> hook) {
    QMutexLocker locker(&m_mutex);
    g_jobTeardownHookForTest = std::move(hook);
}

void FlowExecutionService::clearJobTeardownHookForTest() {
    QMutexLocker locker(&m_mutex);
    g_jobTeardownHookForTest = nullptr;
}

void FlowExecutionService::setWorkerLoopFaultHookForTest(std::function<void()> hook) {
    QMutexLocker locker(&m_mutex);
    g_workerLoopFaultHookForTest = std::move(hook);
}

void FlowExecutionService::clearWorkerLoopFaultHookForTest() {
    QMutexLocker locker(&m_mutex);
    g_workerLoopFaultHookForTest = nullptr;
}

void FlowExecutionService::runWorkerExitBarrierForTest() {
    std::function<void()> hook;
    {
        QMutexLocker locker(&m_mutex);
        hook = m_testWorkerExitBarrier;
    }
    // 关键：调用时不持有服务互斥锁；屏障可以长时间等待而不阻塞任何服务入口。
    if (hook) {
        hook();
    }
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

bool FlowExecutionService::continueRun() {
    QMutexLocker locker(&m_mutex);
    if (state() != State::Paused || m_pendingJob != JobType::None) {
        return false;
    }
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
        joinWorkerThread(thread);
    }
    return true;
}

void FlowExecutionService::workerLoop(JobType* faultyJob) {
    for (;;) {
        JobType job = JobType::None;
        if (faultyJob) {
            *faultyJob = JobType::None;
        }
        {
            QMutexLocker locker(&m_mutex);
            while (!m_quitRequested && m_pendingJob == JobType::None) {
                m_wake.wait(&m_mutex);
            }
            if (m_quitRequested) {
                return;
            }
            job = m_pendingJob;
            m_pendingJob = JobType::None;
            // 新 job 的清零：停止/暂停/继续标志属于上一次 job 的窗口语义。
            m_stopRequested = false;
            m_pauseRequested = false;
            m_resumeRequested = false;
        }
        if (job == JobType::None) {
            continue;
        }
        // T2.5 P1：出队即记录本次命令，供入口兜底在「runJob() 尚未置 m_job」的窗口里
        // 正确判断出错的 job 并上报终态信号（否则会漏报 jobFailed）。
        if (faultyJob) {
            *faultyJob = job;
        }
        // T2.5 P1 测试注入：workerLoop() 骨架故障。位于「命令已出队、runJob() 尚未
        // 进入」的骨架位置，抛出后必须由 WorkerThread::run() 的入口兜底收敛，而
        // 不是穿出 QThread::run()。锁内只做快照，钩子在**不持有 m_mutex** 时执行；
        // 该钩子每轮最多调用一次，生产构建恒为空。
        {
            std::function<void()> loopFaultHook;
            {
                QMutexLocker locker(&m_mutex);
                loopFaultHook = g_workerLoopFaultHookForTest;
            }
            if (loopFaultHook) {
                loopFaultHook();
            }
        }
        runJob(job);
    }
}

void FlowExecutionService::runJob(JobType job) {
    RunEngine& engine = RunEngine::instance();
    // 从出队起即对外可见当前 job（isActive()/stop() 覆盖“已出队、租约未建立”的启动窗口）。
    // 每个 job 分配单调序号：等待方用它区分“本次 job 的完成信号”与上一次 job 的排队信号。
    const quint64 jobSerial = m_jobSerialSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    m_currentJobSerial.store(jobSerial, std::memory_order_release);
    m_job.store(static_cast<int>(job), std::memory_order_release);

    bool stopped = false;
    bool failed = false;
    bool rejected = false;
    bool leaseAcquired = false;
    bool startSettled = false;
    bool terminalEmitted = false;
    QString error;
    RunEngine::JobToken token;
    const auto mergeError = [&error](const QString& extra) {
        error = error.isEmpty() ? extra : QStringLiteral("%1; %2").arg(error, extra);
    };

    // T2.5：启动段纳入保护。此前 startHook()/beginExternalJob()/setDisplayJobId()/
    // commitState() 均在 try 之外，其抛出的 C++ 异常会经 workerLoop() 逃出
    // QThread::run() 触发 std::terminate()（进程 abort，且 startX() 等不到启动握手）。
    // 承诺范围仅限 Service 自有代码的 C++ 异常；槽函数抛异常属 Qt 未定义行为，不在其内。
    try {
        // B7 测试注入：在启动协议最前面执行（生产构建恒为空），用于确定性验证 GUI 侧握手超时。
        std::function<void()> startHook;
        {
            QMutexLocker locker(&m_mutex);
            startHook = m_testStartHook;
        }
        if (startHook) {
            startHook();
        }

        QString reason;
        if (!engine.beginExternalJob(&token, &reason)) {
            rejected = true;
            m_job.store(static_cast<int>(JobType::None), std::memory_order_release);
            m_failedJobSerial.store(jobSerial, std::memory_order_release);
            error = reason.isEmpty() ? QStringLiteral("engine_busy") : reason;
            Logger::instance().warning(QString("Flow job rejected: %1").arg(error), "Service");
            settleStart(false); // 启动握手：让 startX() 立即返回 false
            startSettled = true;
            emit jobFailed(job, error);
            terminalEmitted = true;
            return;
        }
        leaseAcquired = true;
        // 后台执行（设计 §7.1）：job 启动时把 jobId 注入 RunEngine 的显示帧上下文，
        // 帧身份不依赖每轮会重置的全局 frameId；job 结束时清空。
        engine.setDisplayJobId(jobSerial);
        {
            QMutexLocker locker(&m_mutex);
            m_leaseToken = token;
        }
        commitState(State::Running, job);
        settleStart(true); // job 租约已建立、引擎 busy，startX() 可安全返回 true
        startSettled = true;
    } catch (const std::exception& e) {
        const QString what = QString::fromUtf8(e.what());
        const QString fault = what.isEmpty() ? QStringLiteral("service_startup_exception")
                                             : QStringLiteral("service_startup_exception: %1").arg(what);
        if (rejected) {
            mergeError(fault); // 拒绝已成立：保留拒绝原因，不把该 job 改判为“启动异常”
        } else {
            failed = true;
            error = fault;
        }
    } catch (...) {
        if (rejected) {
            mergeError(QStringLiteral("service_startup_exception"));
        } else {
            failed = true;
            error = QStringLiteral("service_startup_exception");
        }
    }

    if (!startSettled) {
        // 启动未落定：立即让 startX() 返回 false，不让其等到握手超时后按“已出队”请求停止。
        settleStart(false);
        startSettled = true;
    }
    if (rejected) {
        // 拒绝分支在 Logger/上报处抛异常：该 job 仍按“拒绝”收尾，不进入主体与收尾段。
        if (!terminalEmitted) {
            emit jobFailed(job, error);
        }
        return;
    }

    // 主体：与既有语义一致，异常转 failed，不改变既有错误文本（引擎层已先兜一层）。
    if (!failed) {
        try {
            switch (job) {
            case JobType::Single:
                runSingleJob(token, stopped, failed, error);
                break;
            case JobType::Cycle:
                runCycleJob(token, stopped, failed, error);
                break;
            case JobType::Step:
                runStepJob(token, stopped, failed, error);
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
    }

    // T2.5：释放租约、清显示 jobId 同样纳入保护。此前这两步在 try 之外，一旦抛出
    // （如容器分配失败），Service 会停在 Running、租约不释放，后续 startX() 被永久拒绝。
    // 收尾异常后的尽力释放：std::exception 与其它异常（catch(...)）都必须走同一条恢复路径，
    // 否则 state 虽回落 Idle，引擎仍 busy，下一任务被永久拒绝。
    const auto bestEffortTeardownRelease = [&engine, &token, leaseAcquired]() {
        // endExternalJob() 对重复/过期 token 返回 false，不会二次生效。
        try {
            if (leaseAcquired) {
                engine.endExternalJob(token);
            }
        } catch (...) {
        }
        try {
            engine.clearDisplayJobId();
        } catch (...) {
        }
    };
    try {
        // T2.5 测试注入：收尾段故障注入。每个 job 只在收尾入口执行一次；下方的
        // catch/恢复分支不再调用，重试路径不得二次触发该钩子。钩子在**不持有 m_mutex**
        // 时执行（锁内只做快照）。
        std::function<void()> teardownHook;
        {
            QMutexLocker locker(&m_mutex);
            teardownHook = g_jobTeardownHookForTest;
        }
        if (teardownHook) {
            teardownHook();
        }
        engine.endExternalJob(token);
        // 后台执行（设计 §7.1）：job 结束/停止时清空显示帧上下文（帧槽位保留最后一帧供叠加使用）。
        engine.clearDisplayJobId();
    } catch (const std::exception& e) {
        const QString what = QString::fromUtf8(e.what());
        mergeError(what.isEmpty() ? QStringLiteral("service_teardown_exception")
                                  : QStringLiteral("service_teardown_exception: %1").arg(what));
        failed = true;
        bestEffortTeardownRelease();
    } catch (...) {
        mergeError(QStringLiteral("service_teardown_exception"));
        failed = true;
        // 非 std::exception 异常：同样必须释放租约并清显示 jobId。只把 state 置回 Idle
        // 不算恢复完成——引擎仍 busy 时后续 startX() 会被永久拒绝。
        bestEffortTeardownRelease();
    }

    // 资源与状态复位：无论启动/主体/收尾是否异常都无条件执行，且不调用可能抛异常的外部代码。
    {
        QMutexLocker locker(&m_mutex);
        m_leaseToken = RunEngine::JobToken{};
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
        try {
            Logger::instance().warning(QString("Flow job failed: %1").arg(error), "Service");
        } catch (...) {
            // 日志失败不得阻断终态上报（此处若再抛，异常会逃出 worker 线程）。
        }
        emit jobFailed(job, error);
    } else {
        emit jobFinished(job, stopped);
    }
}

void FlowExecutionService::handleWorkerLoopFault(JobType faultyJob, const QString& what) {
    // runJob() 已收敛 job 级异常；走到这里说明 workerLoop() 自身（取队列/加锁）出错。
    // 目标：不允许异常逃出 QThread::run()，把 Service 复位到可再次启动的 Idle，并让
    // 后续启动能够重新拉起线程继续执行。
    //
    // T2.5 P1：出错的 job 必须由 workerLoop() 通过 faultyJob 交回，而不是读 m_job——
    // m_job 在 runJob() 内才置位，「命令已出队、runJob() 尚未进入」的窗口里它仍是 None，
    // 只按 m_job 判断会漏报 jobFailed，调用方拿到 startX()==false 却没有任何终态信号。
    const quint64 serial = m_currentJobSerial.load(std::memory_order_acquire);
    const JobType job =
        (faultyJob != JobType::None) ? faultyJob : static_cast<JobType>(m_job.load(std::memory_order_acquire));
    const QString message = what.isEmpty() ? QStringLiteral("service_worker_exception")
                                           : QStringLiteral("service_worker_exception: %1").arg(what);
    m_job.store(static_cast<int>(JobType::None), std::memory_order_release);
    m_finishedJobSerial.store(serial, std::memory_order_release);
    if (job != JobType::None) {
        m_failedJobSerial.store(serial, std::memory_order_release);
    }
    {
        QMutexLocker locker(&m_mutex);
        m_leaseToken = RunEngine::JobToken{};
        m_stopRequested = false;
        m_pauseRequested = false;
        m_resumeRequested = false;
        // T2.5 P1：清除残留命令。handler 也可能在「命令已入队但尚未出队」的窗口被调用，
        // 残留的 m_pendingJob 会让 enqueue() 以 pending != None 永久拒绝后续启动——只复位
        // state 不足以恢复。出错的那条命令不能继续执行，必须就地丢弃。
        m_pendingJob = JobType::None;
        if (!m_startSettled) {
            // 启动握手尚未落定：立即放行 startX()，避免其等到握手超时。
            m_startOk = false;
            m_startSettled = true;
        }
        m_wake.wakeAll();
    }
    commitState(State::Idle, JobType::None);
    try {
        Logger::instance().error(QString("Flow worker fault: %1").arg(message), "Service");
    } catch (...) {
    }
    if (job != JobType::None) {
        emit jobFailed(job, message);
    }
}

void FlowExecutionService::runSingleJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error) {
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
    if (!engine.runOnceForJob(token, &reason)) {
        if (reason == QStringLiteral("external_job_cancelled")) {
            stopped = true;
        } else {
            failed = true;
            error = reason.isEmpty() ? QStringLiteral("start_rejected") : reason;
        }
        return;
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

void FlowExecutionService::runCycleJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error) {
    RunEngine& engine = RunEngine::instance();
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
        if (!engine.runOnceForJob(token, &reason)) {
            if (reason == QStringLiteral("external_job_cancelled")) {
                stopped = true;
                return;
            }
            failed = true;
            error = reason.isEmpty() ? QStringLiteral("start_rejected") : reason;
            return;
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
        // 循环节拍：上一轮结束立即下一轮，仅让出 CPU（设计 §5.3 #1）。
        QThread::yieldCurrentThread();
    }
}

void FlowExecutionService::runStepJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error) {
    RunEngine& engine = RunEngine::instance();
    if (stopRequested()) {
        stopped = true;
        return;
    }
    QString reason;
    if (!engine.stepOnceForJob(token, &reason)) {
        if (reason == QStringLiteral("external_job_cancelled")) {
            stopped = true;
        } else {
            failed = true;
            error = reason.isEmpty() ? QStringLiteral("start_rejected") : reason;
        }
        return;
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
        if (!RunEngine::instance().resumeForJob(token, &resumeReason)) {
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
