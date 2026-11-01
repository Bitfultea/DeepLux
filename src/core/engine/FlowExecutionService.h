#pragma once

#include "../platform/Platform.h"
#include "RunEngine.h"

#include <QMutex>
#include <QObject>
#include <QThread>
#include <QWaitCondition>
#include <atomic>

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
    // 等待 job 真正收尾（Idle 且无 pending/currentJob）。runFinished 早于 Service
    // endExternalJob，wait=true 的调用方用它在返回前恢复“引擎已空闲”语义。
    bool waitForIdle(int timeoutMs);

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
    void workerLoop();
    void runJob(JobType job);

    enum class ResumeOutcome { Resumed, Stopped, Failed };
    // 模块边界暂停后等待 continueRun()/stop()；Resumed 时已恢复被暂停段。
    ResumeOutcome waitForResumeOrStop(const RunEngine::JobToken& token, QString& error);

    void runSingleJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error);
    void runCycleJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error);
    void runStepJob(const RunEngine::JobToken& token, bool& stopped, bool& failed, QString& error);

    bool stopRequested() const;
    bool pauseRequested() const;
    bool quitRequested() const;
    void commitState(State state, JobType job);
    // 启动握手：enqueue 等待工作者完成“取得 job 租约并提交 Running / 启动失败”，
    // 保证 startX() 返回 true 时 isBusy() 已为真（兼容 Agent/UI 的“started 即忙”语义）。
    void settleStart(bool ok);

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    WorkerThread* m_thread = nullptr;
    bool m_quitRequested = false;
    bool m_stopRequested = false;
    bool m_pauseRequested = false;
    bool m_resumeRequested = false;
    bool m_startSettled = true;
    bool m_startOk = false;
    JobType m_pendingJob = JobType::None;
    RunEngine::JobToken m_leaseToken;
    std::atomic<int> m_state{static_cast<int>(State::Idle)};
    std::atomic<int> m_job{static_cast<int>(JobType::None)};
    std::atomic_bool m_lastJobStopped{false};
    std::atomic<quint64> m_jobSerialSeq{0};
    std::atomic<quint64> m_currentJobSerial{0};
    std::atomic<quint64> m_finishedJobSerial{0};
    std::atomic<quint64> m_failedJobSerial{0};
};

} // namespace DeepLux
