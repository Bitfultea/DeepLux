#pragma once

#include "../platform/Platform.h"
#include "RunEngine.h"

#include <QHash>
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

    // T2.5 P1 第六次整改：有界回收进入 `thread->wait()` **之前**的注入点。普通 C++ 回调，
    // 在回收调用线程上、**不持锁**执行；每次回收最多调用一次。用于把回收调用稳定地停在
    // 「借用已生效、等待尚未开始」的位置，使并发 shutdown() 的窗口可确定性构造，
    // 而不是靠 sleep 撞时序。钩子存放于 .cpp 私有测试状态，**不新增类数据成员**。
    DEEPLUX_TEST_VISIBILITY : void setReclaimWaitHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearReclaimWaitHookForTest();

    /// 测试观测：当前活跃的回收借用总数。
    ///
    /// 判据必须看「借用何时归还」，而不是「startSingle() 何时返回」——后者可以被
    /// 正确实现合法地推迟（例如借用在归还后又走了握手），把它当判据会误判。
    DEEPLUX_TEST_VISIBILITY : int activeReclaimBorrowCountForTest() const;

    /// 测试注入点：在「移交指针 + 登记借用」的**同一临界区之内**触发，用于验证
    /// 「shutdown() 看不到借用数为零的中间态」。回调在**持锁**状态下调用，
    /// 因此实现里不得再取 m_mutex，也不得阻塞等待其他需要 m_mutex 的线程。
    DEEPLUX_TEST_VISIBILITY : void setInCriticalSectionHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearInCriticalSectionHookForTest();

    /// 测试观测：**不变量探针** —— 若此刻 m_inflightReclaim 非空，则其中每条线程
    /// 都必须已有 ≥1 笔活跃借用。返回违规条数（0 表示不变量成立）。
    ///
    /// 这是「借用登记与指针移交同临界区」的**直接**判据，不依赖任何时序窗口：
    /// 只要存在「已移入 inflight 但借用量为 0」的瞬间，本探针就能在锁内抓到。
    DEEPLUX_TEST_VISIBILITY : int inflightWithoutBorrowCountForTest() const;

    /// 测试观测：此刻仍**在使用** worker 的回收调用个数（借用的作用域计数）。
    ///
    /// 与 activeReclaimBorrowCountForTest() 的区别是本轮 P1 的核心：
    /// - 计数表（activeReclaimBorrowCount）是 shutdown() 的**等待条件**。shutdown() 只有在
    ///   计数表归零后才会继续删除，因此在 shutdown() 返回的那一刻读它**永远**是 0——
    ///   用它判「shutdown 有没有等」，缺陷态也会全绿（第 17 章记录的无效判据）。
    /// - 本探针数的是「借用守卫是否还在作用域内」，它与 shutdown() 的等待**没有**先后
    ///   耦合假设：缺陷态下 shutdown() 不等就删，回收守卫仍在作用域内，这里必然 >0。
    DEEPLUX_TEST_VISIBILITY : int liveReclaimGuardCountForTest() const;

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

    /// 锁内快照、锁外调用回收等待钩子（见 setReclaimWaitHookForTest）。
    void runReclaimWaitHookForTest();

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
    //
    // **调用前置条件（第七次整改）**：线程必须在**锁内**登记进 m_inflightReclaim，
    // 并且**在同一临界区内**通过 registerReclaimBorrowLocked() 登记一笔回收借用；
    // 否则会出现「借用登记晚于指针移交」的空档（见 .cpp 注释）。
    // `borrowRegistered` 是该前提的显式声明，便于复核与断言。
    bool retireWorkerThreadBounded(WorkerThread* thread, int budgetMs, bool borrowRegistered);

    /// T2.5 P1 第三轮（2026-11-11 二次复核 P1）：把当前仍未退出的 worker 全部摘到
    /// `retiring` 中交回调用方，供 shutdown()/析构在**锁外**逐个 joinWorkerThread()。
    /// 覆盖 m_thread 与所有由有界回收移交的旧 worker —— 后者此前被交给 detached
    /// 回收线程，脱离了 Service 的所有权，导致 shutdown() 漏等、提前报告成功。
    /// 返回值：摘出的线程数（**不含**仍在途回收中的线程）。
    int detachLiveWorkerThreadsLocked(QList<WorkerThread*>& retiring);

    /// T2.5 P1 第四次整改（2026-11-11 复核 P1）：把某条线程**在锁内**标记为「在途回收」。
    ///
    /// 这是本轮缺陷的核心修正点：旧实现先在锁内 `m_thread = nullptr`，再到锁外
    /// `thread->wait(budget)`，最后才登记进 m_retiringThreads。这三步之间线程只存在于
    /// 调用线程的栈上，Service 可见结构里没有它——并发 shutdown() 会认为「没有未退出
    /// worker」而立即返回 true，破坏「返回 true 即可安全销毁 worker 所访问对象」。
    /// 现在把「摘除 + 标记在途」放进同一次加锁，窗口消失。
    void beginInflightReclaimLocked(WorkerThread* thread);

    /// T2.5 P1 第六次整改（2026-11-11 第五段复核 P1）：等待**所有活跃回收调用**归还借用。
    ///
    /// 第四次整改只保证了「删除者唯一」，没有保证「对象已无人使用」：回收线程可能正阻塞
    /// 在 `thread->wait()` 内部，shutdown() 却已把该指针移出 m_inflightReclaim 并 join+delete，
    /// 于是回收线程在 wait 返回后仍会经由已释放对象完成等待收尾（use-after-free）。
    ///
    /// 调用前置条件：调用方**已在锁内**调过 detachLiveWorkerThreadsLocked()（保证不会再
    /// 有新的回收借用产生），且调用方**不持有 m_mutex**。
    ///
    /// 只等待**回收调用**，不等待 worker 退出——因此不会把 shutdown 的生命周期等待与
    /// 启动路径的 100ms 预算混为一谈（详见第 17 章与第 16.6 节更正）。
    void waitForReclaimBorrowsReturned();

    /// 单个回收借用的生命周期守卫。
    ///
    /// **借用必须在 m_mutex 下、与「把线程从 m_thread 摘入 m_inflightReclaim」同一个
    /// 临界区登记**，否则两步之间 shutdown() 会看到借用数为零、接管并删除对象，
    /// 而启动线程随后才去登记借用并 `thread->wait()`——仍访问已释放对象。
    ///
    /// 因此该守卫有两种构造方式：
    ///   - `AdoptAlreadyRegistered`：借用已由持有锁的调用方登记，本守卫只负责归还；
    ///   - `RegisterNow`：在构造时自行加锁登记（仅用于调用方未持有锁的路径）。
    class ReclaimBorrow {
    public:
        enum class Registration { AdoptAlreadyRegistered, RegisterNow };

        /// 接管一笔**已经登记**的借用（调用方必须已在同一临界区内完成登记）。
        ReclaimBorrow(FlowExecutionService* service, WorkerThread* thread, Registration mode);
        ~ReclaimBorrow();
        ReclaimBorrow(const ReclaimBorrow&) = delete;
        ReclaimBorrow& operator=(const ReclaimBorrow&) = delete;

    private:
        FlowExecutionService* m_service = nullptr;
        WorkerThread* m_thread = nullptr;
        bool m_ownsBorrow = false;
        /// 本借用是否仍**在作用域内**（构造后为 true，析构时置 false）。
        ///
        /// 只作**测试观测**用途：它回答「此刻是否还有回收调用在使用这个 worker」，
        /// 而 m_activeReclaimBorrows（计数表）回答「登记的借用有几笔」。两者在
        /// shutdown() 的等待条件里是同一个事实——shutdown() 之所以能在
        /// waitForReclaimBorrowsReturned() 之后继续，恰恰是因为计数表已空；
        /// 因此用计数表判「shutdown 有没有等」必然读到 0，无法暴露缺陷。
        std::atomic_bool m_alive{false};
    };

    /// 在 m_mutex 下登记一笔回收借用（调用方必须已持有 m_mutex）。
    ///
    /// 与 beginInflightReclaimLocked() 一起，构成「移交指针 + 登记借用」的单一
    /// 临界区——这两件事分开做就会留下空档。
    void registerReclaimBorrowLocked(WorkerThread* thread);

    mutable QMutex m_mutex;
    QWaitCondition m_wake;
    /// 在途回收的等待条件（与 m_wake 分开，避免把 job 通知误唤醒/误判）。
    QWaitCondition m_reclaimWake;
    /// 等待「活跃回收调用归还借用」的收尾条件（与 m_reclaimWake 语义不同：
    /// 前者等的是**回收函数返回**，不是 worker 退出）。
    QWaitCondition m_borrowWake;
    WorkerThread* m_thread = nullptr;
    /// 启动路径有界回收**移交**给本服务的旧 worker：已不再消费命令、尚未真正退出。
    /// 由 m_mutex 保护；shutdown()/析构必须覆盖它们，否则「shutdown 成功后可以销毁
    /// worker 所访问对象」的保证不成立。
    QList<WorkerThread*> m_retiringThreads;
    /// 正在被某个线程做**有界回收**（已摘出 m_thread、正在锁外 wait）的 worker。
    ///
    /// 与 m_retiringThreads 的区别：retiring 是「回收已放弃、所有权完全交给 Service」，
    /// inflight 是「某线程正在等它，但它同样**必须**被 shutdown() 看见」。两者都计入
    /// detachLiveWorkerThreadsLocked()，只是 inflight 需要先中止等待再收归。
    ///
    /// **注意**：本列表记录的是「所有权归属」，不等于「引用计数」。移出本列表后，
    /// 回收线程可能仍在 wait 内持有借用——借用由 m_activeReclaimBorrows 单独计数。
    QList<WorkerThread*> m_inflightReclaim;
    /// **当前仍被某个回收调用借用**的 worker → 借用计数。
    ///
    /// 谁把 worker 从 m_inflightReclaim 移出、进入删除流程，就必须先等这里的计数归零，
    /// 否则会在对方 `thread->wait()` 尚未返回时 delete 掉对象。
    QHash<WorkerThread*, int> m_activeReclaimBorrows;
    /// 有线程正在 shutdown()/析构中收归 worker（用于回收线程让出所有权）。
    bool m_reclaimAborted = false;
    /// **关闭进行中 / 排他关闭权**（T2.5 P1 第八次整改；第九次整改补上排他语义）。
    ///
    /// 与 m_reclaimAborted 的区别是本轮 P1 的核心：
    /// - `m_reclaimAborted` 只在 `detachLiveWorkerThreadsLocked()` 发现**存在在途回收**
    ///   时才置位（它的用途是让回收线程让出所有权），普通 Idle 关闭路径不会置位。
    /// - 本标记在 `shutdown()` **一开始**就置位，覆盖所有关闭路径，并在
    ///   `enqueue()` 的**最终入队临界区**检查——那里才是「命令即将交给 worker」的最后
    ///   一刻。只靠入口检查不够：入口检查之后、最终入队之前，shutdown 可能已经完成
    ///   回收并置位标记。
    ///
    /// **排他性（第九次整改）**：本标记同时是「关闭权」的凭证。shutdown() 入口在
    /// 同一临界区内先检查它，已置位即说明另有一次关闭正持有旧 worker；本次调用不是
    /// owner，必须立即返回 false —— 不清除原关闭者的标记、不改动收尾状态、也不得
    /// 报告清理完成。只有 owner 才在成功结尾清除标记并恢复服务。否则第二次调用会
    /// 看到空的 retiring 列表而提前返回 true 并清标记，令新启动被接受。
    ///
    /// 否则会出现「shutdown 期间仍能启动新任务」：`ensureThreadLocked()` 会顺手把
    /// `m_quitRequested` 复位（它本来是给「shutdown 之后重新可用」准备的），
    /// 于是关闭期间的新启动既重建了 worker、又执行了任务，而 shutdown 已经返回。
    bool m_shutdownInProgress = false;
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
