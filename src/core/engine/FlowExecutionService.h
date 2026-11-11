#pragma once

#include "../platform/Platform.h"
#include "RunEngine.h"

#include <QList>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <functional>

namespace DeepLux {

/**
 * @brief 后台流程执行服务（后台执行设计 §4.1/§5.1/§5.2/§5.3）
 *
 * - 持有应用级常驻工作线程，空闲时阻塞在 QWaitCondition，不忙等、不为每次运行创建线程。
 * - 串行执行 Single/Cycle/Step/Resume；命令不排队（非 Idle 时启动请求直接拒绝）。
 * - 整个 job 期间持有 RunEngine 的 external job 租约（含 Cycle 两轮之间），因此
 *   isBusy() 在整个 job 期间为真，工程加载/清空被拒绝。
 * - 所有信号（jobStateChanged/jobFinished/jobFailed）从工作线程发出；状态用原子快照
 *   供跨线程读取（设计 §4.1）。
 * - 不直接修改 RunEngine 内部成员，只调用其公开同步入口（*ForJob/stop/pauseForJob）。
 */
class DEEPLUX_API FlowExecutionService : public QObject {
    Q_OBJECT

public:
    enum class JobType { None, Single, Cycle, Step, Resume };
    enum class State { Idle, Running, Paused, Stopping };

    static FlowExecutionService& instance();

    // 启动入口（设计 §4.1）：仅 Idle 接受；Paused 只接受 continueRun() 或 stop()。
    bool startSingle();
    bool startCycle();
    bool stepOnce();
    bool continueRun();
    bool pauseRun();
    // 任何状态下安全、幂等、立即返回；覆盖空闲/暂停/循环轮间等待。
    void stop();
    // 仅 Idle 时销毁线程并返回 true；非 Idle 时先 stop 并返回 false（调用方在
    // jobFinished 后重试，见设计 §5.3 #7）。
    bool shutdown();

    State state() const {
        return static_cast<State>(m_state.load(std::memory_order_acquire));
    }
    JobType currentJob() const {
        return static_cast<JobType>(m_job.load(std::memory_order_acquire));
    }
    // 是否有 job（含已入队尚未启动的命令）；供 Agent/UI 选择 stop 路径。
    bool isActive() const;
    // 最近一次 job 是否以停止结束（Service 侧结果分类）。
    bool lastJobStopped() const {
        return m_lastJobStopped.load(std::memory_order_acquire);
    }
    // job 序号（单调递增）：等待方可据此忽略上一次 job 尚在队列中的 jobFinished/jobFailed。
    quint64 currentJobSerial() const {
        return m_currentJobSerial.load(std::memory_order_acquire);
    }
    quint64 lastFinishedJobSerial() const {
        return m_finishedJobSerial.load(std::memory_order_acquire);
    }
    quint64 lastFailedJobSerial() const {
        return m_failedJobSerial.load(std::memory_order_acquire);
    }

    // B7：启动握手在 GUI 线程上的最长等待。超时后撤回尚未出队的命令（已出队则请求停止），
    // 保证 closeEvent/启动路径不被拖住，且不会留下“稍后执行”的 pending job。
    static constexpr int StartHandshakeTimeoutMs = 100;

    // 后台执行（B7）测试注入：worker 在 beginExternalJob 之前的启动协议内执行该钩子，
    // 用于确定性验证启动握手的等待上限与超时语义（生产构建该钩子恒为空）。
    DEEPLUX_TEST_VISIBILITY : void setStartHandshakeHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearStartHandshakeHookForTest();

    // T2.1：worker 退出等待的诊断阈值（默认 10s）。仅用于告警，不作为强杀或 detach 依据。
    // 注入短值便于测试确定性地触发诊断路径。
    DEEPLUX_TEST_VISIBILITY : void setShutdownDeadlineForTest(int msecs);
    DEEPLUX_TEST_VISIBILITY : int shutdownDeadlineHitCountForTest() const;

    // T2.1 整改：worker 退出屏障。workerLoop() 返回后、QThread::run() 结束前由工作线程
    // 调用；钩子在**不持有 m_mutex** 时执行，使 NF 能确定性保证「诊断先于 worker 退出、
    // shutdown 等 worker 真正退出后才返回」。生产构建该钩子恒为空。
    DEEPLUX_TEST_VISIBILITY : void setWorkerExitBarrierForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearWorkerExitBarrierForTest();

    // T2.5：收尾段故障注入。普通 C++ 回调，锁内快照、锁外调用；每个 job 只在收尾入口
    // 调用一次，恢复/重试路径不得再次触发。钩子存储在 .cpp 的私有测试状态中，**不新增
    // 类数据成员**，FlowExecutionService 的类布局/ABI 保持不变；DEEPLUX_TEST_VISIBILITY
    // 只控制访问权限。生产构建不会设置该钩子，恒为空。
    DEEPLUX_TEST_VISIBILITY : void setJobTeardownHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearJobTeardownHookForTest();

    // T2.5 P1：workerLoop() 骨架故障注入。普通 C++ 回调，锁内快照、锁外调用；每轮
    // 循环入口最多调用一次。钩子同样存放于 .cpp 的私有测试状态，**不新增类数据成员**，
    // 类布局/ABI 保持不变。抛出即等价于 workerLoop() 自身（取队列/加锁）抛出 C++ 异常，
    // 用于验证线程入口兜底后的命令清理与线程安全重建。生产构建不会设置，恒为空。
    DEEPLUX_TEST_VISIBILITY : void setWorkerLoopFaultHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearWorkerLoopFaultHookForTest();

signals:
    void jobStateChanged(FlowExecutionService::State state, FlowExecutionService::JobType job);
    void jobFinished(FlowExecutionService::JobType job, bool stopped);
    void jobFailed(FlowExecutionService::JobType job, const QString& error);

private:
    class WorkerThread;

    FlowExecutionService();
    ~FlowExecutionService() override;

    // 命令槽（设计 §4.1：不排队，深度 1）：锁内校验 + 入队 + 唤醒工作线程。
    bool enqueue(JobType job);
    void ensureThreadLocked();
    // faultyJob（可空）在每轮出队后写入当前命令，供 run() 的入口兜底判断出错的 job。
    void workerLoop(JobType* faultyJob = nullptr);
    void runJob(JobType job);
    // T2.5：workerLoop() 自身 C++ 异常的最后一道保险（job 级异常已由 runJob() 收敛）。
    // 由 WorkerThread::run() 调用；负责把 Service 复位到可再次启动的 Idle 并上报 jobFailed。
    // faultyJob 由 workerLoop() 在出队时记录（出错时正在处理的命令）；仅在 m_job 尚未置位
    // 的窗口里依赖它，避免漏报终态信号。
    void handleWorkerLoopFault(JobType faultyJob, const QString& what);

    enum class ResumeOutcome { Resumed, Stopped, Failed };
    // 模块边界暂停后等待 continueRun()/stop()；Resumed 时已恢复被暂停段。
    ResumeOutcome waitForResumeOrStop(const RunEngine::JobToken& token, QString& error);

    void runSingleJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error);
    void runCycleJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error);
    void runStepJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error);

    bool stopRequested() const;
    bool pauseRequested() const;
    bool quitRequested() const;
    // T2.1 整改：锁内快照退出屏障钩子、锁外调用；由 WorkerThread::run() 在退出前调用。
    void runWorkerExitBarrierForTest();
    void commitState(State state, JobType job);
    // 启动握手：enqueue 等待工作者完成“取得 job 租约并提交 Running / 启动失败”，
    // 保证 startX() 返回 true 时 isBusy() 已为真（兼容 Agent/UI 的“started 即忙”语义）。
    void settleStart(bool ok);
    // T2.1：有界诊断等待后继续 join；绝不 detach/terminate，保证 worker 访问的对象
    // 一定 outlive 线程。**仅用于析构/shutdown 等允许长期等待的收尾路径。**
    void joinWorkerThread(WorkerThread* thread);
    // T2.5 P1 二次整改（2026-11-11 复核 P1）：启动路径专用的**有界**回收。
    //
    // 与 joinWorkerThread() 的区别：本函数在 budgetMs 用尽时放弃等待并返回 false，
    // 由调用方明确拒绝本次启动；线程指针**仍由本服务持有**（见 m_retiringThreads），
    // 等它真正退出后再 delete，既不 detach 也不泄漏，更不阻塞 GUI。
    // 返回 true 表示旧线程已真正退出并被 delete。
    bool retireWorkerThreadBounded(WorkerThread* thread, int budgetMs);

    /// T2.5 P1 第三轮（2026-11-11 二次复核 P1）：把当前仍未退出的 worker 全部摘到
    /// `retiring` 中交回调用方，供 shutdown()/析构在**锁外**逐个 joinWorkerThread()。
    /// 覆盖 m_thread 与所有由有界回收移交的旧 worker —— 后者此前被交给 detached
    /// 回收线程，脱离了 Service 的所有权，导致 shutdown() 漏等、提前报告成功。
    void detachLiveWorkerThreadsLocked(QList<WorkerThread*>& retiring);

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    WorkerThread* m_thread = nullptr;
    /// 启动路径有界回收**移交**给本服务的旧 worker：已不再消费命令、尚未真正退出。
    /// 由 m_mutex 保护；shutdown()/析构必须覆盖它们，否则「shutdown 成功后可以销毁
    /// worker 所访问对象」的保证不成立。
    QList<WorkerThread*> m_retiringThreads;
    bool m_quitRequested = false;
    bool m_stopRequested = false;
    bool m_pauseRequested = false;
    bool m_resumeRequested = false;
    bool m_startSettled = true;
    bool m_startOk = false;
    // B7：启动握手超时诊断/测试钩子（钩子仅在持有本地副本时于工作线程执行）。
    std::function<void()> m_testStartHook;
    // T2.1 整改：worker 退出屏障（仅测试注入）；为空时生产行为不变。
    std::function<void()> m_testWorkerExitBarrier;
    JobType m_pendingJob = JobType::None;
    RunEngine::JobToken m_leaseToken;
    std::atomic<int> m_state{static_cast<int>(State::Idle)};
    std::atomic<int> m_job{static_cast<int>(JobType::None)};
    // T2.1：worker 退出诊断阈值与命中计数（默认 10s；仅告警，不触发强杀/detach）。
    std::atomic<int> m_shutdownDeadlineMs{10000};
    std::atomic<int> m_shutdownDeadlineHits{0};
    std::atomic_bool m_lastJobStopped{false};
    std::atomic<quint64> m_jobSerialSeq{0};
    std::atomic<quint64> m_currentJobSerial{0};
    std::atomic<quint64> m_finishedJobSerial{0};
    std::atomic<quint64> m_failedJobSerial{0};
};

} // namespace DeepLux
