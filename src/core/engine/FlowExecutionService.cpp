#include "FlowExecutionService.h"

#include "common/Logger.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QThread>
#include <atomic>
#include <exception>
#include <thread>

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

// T2.5 P1 第六次整改：有界回收进入 `thread->wait()` 之前的注入钩子。同样存放于
// .cpp 私有测试状态（不新增数据成员），由 m_mutex 保护、锁外调用。仅用于把回收
// 调用稳定停在「借用已生效、等待尚未开始」的位置，以确定性构造并发 shutdown()
// 的窗口。生产构建不会设置，恒为空。
std::function<void()> g_reclaimWaitHookForTest;

/// 测试观测状态（与其余测试钩子一样放 .cpp 匿名空间，**不新增类数据成员**）：
/// 仍在作用域内的 ReclaimBorrow 个数。用全局原子是因为守卫在多个线程上并存；
/// 只在测试观测接口里读取，生产路径只做一次 ++/--。
std::atomic_int g_liveReclaimGuards{0};

// 测试注入点：在「移交指针 + 登记借用」的同一临界区**之内**触发（**持 m_mutex**）。
// 用于验证 shutdown() 不可能观察到「线程可见但借用数为零」的中间态。
// 实现中不得再取 m_mutex、不得阻塞等待需要本锁的线程。生产恒为空。
std::function<void()> g_inCriticalSectionHookForTest;

} // namespace

/// 常驻工作线程：run() 内阻塞在 QWaitCondition 的命令循环，不使用事件循环。
class FlowExecutionService::WorkerThread : public QThread {
public:
    explicit WorkerThread(FlowExecutionService* service) : m_service(service) {}

    /// T2.5 P1（第二轮回）：线程是否已进入不可再消费命令的退出阶段。
    ///
    /// 这个标记与 isFinished() 的区别就是本轮 P1 的核心：jobFailed 是在 run() 内部
    /// （handleWorkerLoopFault）发出的，那一刻 run() 还没返回、isFinished() 仍为 false，
    /// 调用方收到通知后立即重试就会复用这个再也不会消费队列的线程。标记在 workerLoop()
    /// 返回之后、任何可能通知调用方的时机之前成立，因此覆盖整个「通知已发出、线程尚未
    /// 结束」的窗口。放在 WorkerThread 自身（.cpp 内定义的 private 嵌套类），
    /// FlowExecutionService 的类布局与 ABI 不变。
    bool isRetiring() const {
        return m_retiring.load(std::memory_order_acquire);
    }

protected:
    void run() override {
        // T2.5 P1（第二轮回）：一旦 workerLoop() 交出控制权（无论正常返回还是抛出），
        // 本线程就再也不会消费命令队列。**必须在任何可能通知调用方的动作之前**置位：
        // jobFailed 正是在下面的 catch 分支（handleWorkerLoopFault）里发出的，那一刻
        // run() 尚未返回、isFinished() 仍为 false。调用方收到通知后立即重试时，单靠
        // isFinished() 会跳过回收、把命令交给一个再也不会消费队列的线程，最终只能等
        // 握手超时（独立复现：标准异常 19/200、非标准 3/200、诊断复跑 28/200）。
        // isRetiring() 使 enqueue() 在这个窗口内就能识别「线程不可再用」，并等到本线程
        // 真正退出（join）后再重建。
        //
        // T2.5 P1：workerLoop() 除返回外，还可能把「出错时正在处理的命令」通过 out 参数
        // 交回给入口兜底。出队后、runJob() 尚未置 m_job 的窗口里发生异常时，m_job 仍是
        // None，仅凭它判断会漏报终态信号（jobFailed 不发出）。
        JobType faultyJob = JobType::None;
        try {
            m_service->workerLoop(&faultyJob);
        } catch (const std::exception& e) {
            // 置位后再通知：保证「收到 jobFailed 立即重试」一定看到 retiring。
            m_retiring.store(true, std::memory_order_release);
            // T2.5 最后一道保险：runJob() 负责 job 级 C++ 异常；此处只兜住 workerLoop()
            // 自身（取队列/加锁）的异常，绝不允许其逃出 QThread::run() 触发 std::terminate()。
            m_service->handleWorkerLoopFault(faultyJob, QString::fromUtf8(e.what()));
        } catch (...) {
            m_retiring.store(true, std::memory_order_release);
            m_service->handleWorkerLoopFault(faultyJob, QStringLiteral("unknown_worker_exception"));
        }
        // 正常返回路径（shutdown()/析构已请求退出）同样置位；此时线程指针已被摘走，
        // 不会影响任何入队判定。
        m_retiring.store(true, std::memory_order_release);
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
    std::atomic_bool m_retiring{false};
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
    // T2.5 P1 第三轮（2026-11-11 二次复核 P1）：必须覆盖**所有**未退出的 worker——
    // 常驻的 m_thread 与启动路径有界回收移交的 m_retiringThreads。只取 m_thread 会漏掉
    // 后者，析构可能在仍在 run() 的 worker 之下销毁它访问的成员与模块。
    QList<WorkerThread*> retiring;
    {
        QMutexLocker locker(&m_mutex);
        detachLiveWorkerThreadsLocked(retiring);
    }
    for (WorkerThread* thread : retiring) {
        joinWorkerThread(thread); // 锁外 join：持锁等待会卡死所有入口
    }
}

void FlowExecutionService::registerReclaimBorrowLocked(WorkerThread* thread) {
    // 调用方必须已持有 m_mutex。本函数与 beginInflightReclaimLocked() 必须在
    // **同一个临界区**内一并调用——见 ReclaimBorrow 的注释。
    if (thread) {
        ++m_activeReclaimBorrows[thread];
    }
}

FlowExecutionService::ReclaimBorrow::ReclaimBorrow(FlowExecutionService* service, WorkerThread* thread,
                                                   Registration mode)
    : m_service(service), m_thread(thread) {
    if (!m_service || !m_thread) {
        return;
    }
    if (mode == Registration::RegisterNow) {
        QMutexLocker locker(&m_service->m_mutex);
        m_service->registerReclaimBorrowLocked(m_thread);
    }
    // AdoptAlreadyRegistered：借用已由持有锁的调用方登记，这里只接管归还责任。
    m_ownsBorrow = true;
    // 观测标志必须在「借用已生效」之后置位：它代表「本守卫确实在守护一个 worker」，
    // 早于借用生效置位会把「还没登记」误报成「正在使用」。
    m_alive.store(true, std::memory_order_release);
    g_liveReclaimGuards.fetch_add(1, std::memory_order_acq_rel);
}

FlowExecutionService::ReclaimBorrow::~ReclaimBorrow() {
    if (!m_service || !m_thread || !m_ownsBorrow) {
        return;
    }
    // 析构点覆盖本函数的一切出口（正常 return、早退、异常），因此借用的归还时刻
    // 严格晚于 `thread->wait()` 的返回时刻——这正是 shutdown() 需要的前提。
    //
    // 观测标志先于计数表撤销：本守卫即将不再使用 worker，此刻起后续观测不应再把它
    // 算作「正在使用」。撤销顺序不影响生产语义（两者都不解引用 m_thread）。
    m_alive.store(false, std::memory_order_release);
    g_liveReclaimGuards.fetch_sub(1, std::memory_order_acq_rel);
    // 归还只操作本服务的内部计数表，**不触碰 m_thread 指向的对象**：
    // 此刻该 worker 可能已被 delete，任何解引用都是未定义行为。
    QMutexLocker locker(&m_service->m_mutex);
    auto it = m_service->m_activeReclaimBorrows.find(m_thread);
    if (it != m_service->m_activeReclaimBorrows.end()) {
        if (--it.value() <= 0) {
            m_service->m_activeReclaimBorrows.erase(it);
        }
    }
    m_service->m_borrowWake.wakeAll();
}

void FlowExecutionService::waitForReclaimBorrowsReturned() {
    // 调用方不持锁，且必须已在锁内停止接受新的回收借用
    // （enqueue() 看到 m_reclaimAborted 即拒绝启动，不再产生新借用）。
    //
    // **这里的等待与启动路径的 StartHandshakeTimeoutMs 不是一回事**：启动预算是
    // 「GUI 允许多久拿到明确的失败答复」，所以必须有界；这里是析构/shutdown 的
    // 生命周期同步，语义上必须等到「无人再使用该对象」为止，因此和
    // joinWorkerThread() 一样允许长期等待。二者共用同一个数字会造成「为了不超预算
    // 而放行悬空引用」的错误取舍。
    QMutexLocker locker(&m_mutex);
    while (!m_activeReclaimBorrows.isEmpty()) {
        m_borrowWake.wait(&m_mutex);
    }
}

bool FlowExecutionService::retireWorkerThreadBounded(WorkerThread* thread, int budgetMs, bool borrowRegistered) {
    if (!thread) {
        return true;
    }

    // T2.5 P1 第七次整改（2026-11-11 第六段复核 P1）：借用必须「与移交同一临界区登记」。
    //
    // 第六次整改在本函数内部才 New 出 ReclaimBorrow 去登记借用，而指针早已在
    // enqueue() 的锁内被移入 m_inflightReclaim 并解锁——两步之间 shutdown() 会看到
    // 借用数为零，接管并 delete；本函数随后才登记借用并 `thread->wait()`，
    // 仍然访问已释放对象（评审：FlowExecutionService.cpp:413 与 :191）。
    //
    // 因此现在由调用方（enqueue()）**在摘入 m_inflightReclaim 的同一临界区内**
    // 调 registerReclaimBorrowLocked() 登记；本函数只**接管**这笔已登记的借用，
    // 不再自行登记。borrowRegistered==false 仅用于直接调用本函数且确实没有
    // 先登记借用的路径（当前不存在，保留作为显式断言）。
    //
    // 所有对 `thread` 的使用都限制在下面的 borrow 块内；离开该块即归还借用，
    // 之后本函数**绝不**再解引用 `thread`。
    Q_ASSERT_X(borrowRegistered, "retireWorkerThreadBounded",
               "the reclaim borrow must be registered inside the same critical section that "
               "moved the worker into m_inflightReclaim");

    bool finished = false;
    bool owned = false;
    {
        ReclaimBorrow borrow(this, thread,
                             borrowRegistered ? ReclaimBorrow::Registration::AdoptAlreadyRegistered
                                              : ReclaimBorrow::Registration::RegisterNow);
        // 测试注入点（生产恒为空）：把回收调用停在「借用已生效、等待尚未开始」处。
        runReclaimWaitHookForTest();
        finished = thread->wait(static_cast<unsigned long>(qMax(0, budgetMs)));

        // 关键：**唯一回收者判定必须在锁内、按「是否仍在 inflight 列表」进行**。
        // 不能依赖等待之前采样的 m_reclaimAborted 快照——等待期间 shutdown() 可能已经把
        // 这条线程收归自己、join 完并 delete 掉，此时本线程再碰 thread 就是 use-after-free。
        // inflight 列表是「本线程是否仍被授权处置它」的唯一权威事实：
        //   - 仍在列表里 → 所有权仍在本线程；
        //   - 已被移出   → 所有权已转交 shutdown()/析构，本线程立刻返回、绝不触碰指针。
        QMutexLocker locker(&m_mutex);
        owned = m_inflightReclaim.removeOne(thread);
        if (owned && !finished) {
            // 未退出且仍归本线程：所有权移交 Service 的 retiring 列表。
            m_retiringThreads.append(thread);
        }
        m_reclaimWake.wakeAll(); // 唤醒可能在 m_reclaimWake 上等待的收尾者
    }
    // —— 借用已在此处归还。此后 thread 指针一律按「可能已被 delete」对待。——

    if (!owned) {
        // shutdown()/析构已接管；不 delete、不登记、不再触碰该指针。
        // 归还借用会让 shutdown() 的 waitForReclaimBorrowsReturned() 得以继续，
        // 于是它在真正 delete 之前就已经等到本线程离开 wait()。
        return false;
    }
    if (finished) {
        // 已退出、所有权仍在本线程：唯一删除者。
        delete thread;
        return true;
    }
    // 预算内未退出：**不阻塞调用线程**，但也**不交出所有权**。
    //
    // 为什么不是 joinWorkerThread()：那条路径在诊断阈值之后会无条件 thread->wait()，
    // 无限期阻塞——挂在 GUI 线程的 startSingle() 上就会绕过 StartHandshakeTimeoutMs
    // 的 100ms 上限，把「启动失败可预期、可拒绝」变成「GUI 卡住」。
    //
    // 为什么不能 detach 或直接 delete：worker 仍在 run() 内并可能访问本服务与模块。
    //
    // **2026-11-11 二次复核 P1（第三轮）**：此前这里用 detached std::thread 把等待者
    // 挪到后台，worker 既不在 m_thread、也不在任何 Service 侧结构里——所有权随 detach
    // 丢失，shutdown() 漏等并立即返回 true。
    //
    // **2026-11-11 四次复核 P1（本轮）**：第三轮改成「等完再登记」仍有窗口——登记发生
    // 在本函数末尾，而等待发生在它之前，两者之间并发 shutdown() 依然看不到这个 worker
    // （评审独立复现：shutdownOk=1、returnedBeforeWaitReleased=1、
    // oldWorkerRunningAtReturn=1）。因此现在**把在途标记提前到等待之前**：调用方
    // （enqueue()）已在锁内调 beginInflightReclaimLocked()，本函数的等待一开始就处于
    // 「shutdown() 可见」的状态。
    m_shutdownDeadlineHits.fetch_add(1, std::memory_order_relaxed);
    Logger::instance().warning(QString("Flow start rejected: previous worker still exiting after %1 ms; "
                                       "ownership retained by the service (inflight/retiring), job not enqueued")
                                   .arg(budgetMs),
                               "Service");
    return false;
}

void FlowExecutionService::beginInflightReclaimLocked(WorkerThread* thread) {
    // 调用方必须已持有 m_mutex。这一步与「摘除 m_thread」在同一次加锁内完成，
    // 因此不存在「线程已不可见、但尚未登记」的中间状态。
    if (thread) {
        m_inflightReclaim.append(thread);
    }
}

int FlowExecutionService::detachLiveWorkerThreadsLocked(QList<WorkerThread*>& retiring) {
    // 调用方必须已持有 m_mutex。
    //
    // T2.5 P1 第六次整改（2026-11-11 第五段复核 P1）：这里只做**所有权收归**，
    // 不再假设「移出 inflight 即可删除」。在途线程可能正被某个回收调用借用在
    // `thread->wait()` 内——删除权与使用权是两件事。真正的删除由调用方在锁外先
    // waitForReclaimBorrowsReturned()、再 joinWorkerThread() 完成。
    int count = 0;
    // 1) 在途回收中的线程：全部交回调用方，并置中止信号，使后续启动被拒绝
    //    （enqueue() 入口检查 m_reclaimAborted），保证不再产生新的回收借用。
    if (!m_inflightReclaim.isEmpty()) {
        m_reclaimAborted = true;
        for (WorkerThread* thread : m_inflightReclaim) {
            if (thread) {
                m_retiringThreads.append(thread);
            }
        }
        m_inflightReclaim.clear();
        m_reclaimWake.wakeAll(); // 让仍阻塞在有界 wait 的回收线程尽快返回
    }
    if (!m_retiringThreads.isEmpty()) {
        retiring.append(m_retiringThreads);
        count += m_retiringThreads.size();
        m_retiringThreads.clear();
    }
    if (m_thread) {
        retiring.append(m_thread);
        ++count;
        m_thread = nullptr;
    }
    return count;
}

void FlowExecutionService::joinWorkerThread(WorkerThread* thread) {
    if (!thread) {
        return;
    }
    // T2.5 P1 第六次整改（2026-11-11 第五段复核 P1）：**删除前必须先等借用归还**。
    //
    // 走到这里的线程可能正被某个回收调用在 retireWorkerThreadBounded() 内借用在
    // `thread->wait()` 上。仅凭「本函数是唯一删除者」就 delete 会与那次活跃的同对象
    // wait 并发——删除者唯一，不等于对象已无人使用。这里等的只是**回收调用返回**
    // （不是 worker 退出），所以不会与启动路径的 100ms 预算混为一谈。
    waitForReclaimBorrowsReturned();
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
    // T2.5 P1：把「故障线程退出/回收」与「下一任务入队」协调起来。
    //
    // 背景（第二轮回，2026-11-10 复核）：jobFailed 是在 worker 线程的 run() 内部发出的，
    // 收到通知后立即重试时旧线程往往还没结束（isFinished() 仍为 false）。此前只用一次
    // isFinished() 快照判断是否回收，会跳过回收、把命令写进一个再也不会消费队列的线程，
    // 最终只能等到握手超时失败（独立复现：标准异常 19/200、非标准 3/200、复跑 28/200）。
    //
    // 现在的做法：循环「摘指针 → 锁外有界回收 → 回顶部重判」，直到拿到一个真正可用
    // （既未 retiring、也未结束）的线程或确实完成了重建。判定不再是一次快照，而是
    // 与线程自标记的 retiring 状态配合，覆盖「通知已发出、线程尚未退出」的整个窗口。
    //
    // **2026-11-11 复核 P1 整改**：本循环的等待必须与启动预算是同一个界。此前调用
    // joinWorkerThread()，它在诊断阈值之后会无条件 thread->wait()，可无限延长——
    // 且发生在下面的 StartHandshakeTimeoutMs 计时**之前**，等于绕过启动上限把
    // GUI 线程卡死（评审独立观测：退出延迟 500ms → startSingle() 阻塞 500ms，超过
    // 100ms 预算）。现在改为有界回收，预算用尽即**明确拒绝**本次启动：
    //   - 不返回 true：调用方不会以为已启动；
    //   - 不入队：m_pendingJob 从不置位，绝不会「迟到执行」；
    //   - 不阻塞：旧线程留在本服务持有的 retiring 列表里（不 detach、不 terminate，
    //     join 语义保留），由 shutdown()/析构或下一次启动回收；
    //   - 旧线程退出后再次调用即可成功——无需手动 shutdown()。
    QElapsedTimer startTimer;
    startTimer.start();

    // T2.5 P1 第三轮（2026-11-11 二次复核 P1）：上一轮被拒绝时留下的旧 worker 仍由本服务
    // 持有（m_retiringThreads），它们已不再消费命令（retiring/isFinished）。
    //
    // **它们不参与本次启动的预算判定**。理由：能消费命令的是即将（重建的）新线程，这些旧
    // 线程只是在退出途中，它们退出与否**不决定本次启动能否成功**。把它们纳入启动预算会
    // 产生链式拒绝——前一次尝试预算耗尽留下一个 retiring 线程，下一次启动又要在它身上
    // 耗掉整份预算（上一轮窗口用例实测 restartOk 从 >=5 掉到 2/10、3/10）。这与「旧线程
    // 退出后再次调用即可成功、无需手动 shutdown()」的验收口径直接冲突。
    //
    // 因此这里只做**零等待的顺手清理**（已结束的直接 delete），其余原样保留所有权，
    // 由 shutdown()/析构无条件 join —— 这正是本轮 P1 要求的「生命周期管理不丢」。
    {
        QList<WorkerThread*> finished;
        {
            QMutexLocker locker(&m_mutex);
            // 第四次整改：shutdown()/析构正在进行收尾（已置中止位）时**不得**删除任何
            // retiring 线程——它可能已被收归到 shutdown 的局部列表里，两边会各删一次。
            if (!m_reclaimAborted) {
                for (int i = m_retiringThreads.size() - 1; i >= 0; --i) {
                    WorkerThread* thread = m_retiringThreads.at(i);
                    // 第六次整改：仍被回收调用借用的线程**不得**在这里删除。
                    // 解锁后执行 delete 时，对方可能仍在其 `thread->wait()` 内；
                    // 交给 shutdown()/析构在 waitForReclaimBorrowsReturned() 之后处理。
                    if (thread && m_activeReclaimBorrows.contains(thread)) {
                        continue;
                    }
                    if (!thread || thread->isFinished()) {
                        if (thread) {
                            finished.append(thread);
                        }
                        m_retiringThreads.removeAt(i);
                    }
                }
            }
        }
        if (!finished.isEmpty()) {
            for (WorkerThread* thread : finished) {
                delete thread; // 已真正结束，无需等待
            }
        }
    }

    for (;;) {
        WorkerThread* retired = nullptr;
        bool borrowRegistered = false;
        {
            QMutexLocker locker(&m_mutex);
            // 第四次整改：shutdown()/析构正在收归 worker 时，启动必须明确拒绝。
            // 否则两个入口会同时对同一批线程做 join/delete。
            if (m_reclaimAborted) {
                return false;
            }
            // 线程不可再用：要么已自标记 retiring（run() 即将返回，绝不再消费队列），
            // 要么已经真正结束（isFinished()）。两者都要求先回收再重建。
            if (m_thread && (m_thread->isRetiring() || m_thread->isFinished())) {
                retired = m_thread;
                m_thread = nullptr;
                // **关键（第四/第七次整改）**：以下三件事必须在**同一次加锁**内完成，
                // 顺序是「移交指针 → 标记在途 → 登记借用」：
                //   1. m_thread 置空（指针移交出常驻槽）；
                //   2. beginInflightReclaimLocked()（shutdown 可见）；
                //   3. registerReclaimBorrowLocked()（shutdown 知道有人正在用它）。
                // 只要 2、3 之间或 1、3 之间留出解锁空档，shutdown() 就能看到
                // 「可见但借用数为零」的状态，接管并删除；本线程随后才登记借用并
                // thread->wait()，仍会访问已释放对象（第六段复核 P1）。
                beginInflightReclaimLocked(retired);
                registerReclaimBorrowLocked(retired);
                borrowRegistered = true;
                // 测试注入点（生产恒为空）：此刻仍**持有 m_mutex**，借用已登记。
                // 任何并发 shutdown() 都必须等本临界区结束才能继续——这正是要证明的
                // 不变量：不存在「线程已可见、借用数为零」的可观察状态。
                if (g_inCriticalSectionHookForTest) {
                    g_inCriticalSectionHookForTest();
                }
            }
        }
        if (!retired) {
            break; // 没有待回收线程：走正常入队
        }
        // 锁外有界回收：剩余预算内等它真正退出则 delete；否则所有权移交 retiring 并拒绝本次启动。
        //
        // 预算已耗尽的特例：此时若把线程交给 retireWorkerThreadBounded()，它会以 0ms 等待
        // 失败并**登记进 retiring**——但这次尝试反正要拒绝，线程也大概率马上退出。留下
        // 「无预算却登记」的尾巴会让下一次启动先替它耗预算，形成链式拒绝。故此时把线程
        // 直接留在 retiring（仍由本服务持有，绝不丢所有权）。
        //
        // 注意：此时它已在 inflight 列表里、且已登记借用，必须先摘下再转入 retiring，
        // 否则同一条线程会同时出现在两个列表中（shutdown 会 join 两次、delete 两次）。
        const int reclaimBudget = StartHandshakeTimeoutMs - static_cast<int>(startTimer.elapsed());
        if (reclaimBudget <= 0) {
            // 借用已在上面的临界区内登记；这里用 AdoptAlreadyRegistered 接管它，
            // 使 shutdown() 的 waitForReclaimBorrowsReturned() 能覆盖本路径。
            ReclaimBorrow borrow(this, retired, ReclaimBorrow::Registration::AdoptAlreadyRegistered);
            QMutexLocker locker(&m_mutex);
            const bool moved = m_inflightReclaim.removeOne(retired);
            if (moved) {
                m_retiringThreads.append(retired);
            }
            m_reclaimWake.wakeAll();
            return false; // 明确拒绝，不入队；所有权仍在服务侧
        }
        if (!retireWorkerThreadBounded(retired, reclaimBudget, borrowRegistered)) {
            return false;
        }
        // 回收完成后再回到循环顶部重新判定：期间若又发生自退出，仍会被识别。
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
    // 2026-11-11 P1：计时器与上面的回收共用一个（startTimer），回收已花掉的预算从
    // 握手中扣除——总等待仍不超过 StartHandshakeTimeoutMs，而不是两段各自 100ms。
    while (!m_startSettled) {
        const int remaining = StartHandshakeTimeoutMs - static_cast<int>(startTimer.elapsed());
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

void FlowExecutionService::setReclaimWaitHookForTest(std::function<void()> hook) {
    QMutexLocker locker(&m_mutex);
    g_reclaimWaitHookForTest = std::move(hook);
}

void FlowExecutionService::clearReclaimWaitHookForTest() {
    QMutexLocker locker(&m_mutex);
    g_reclaimWaitHookForTest = nullptr;
}

void FlowExecutionService::runReclaimWaitHookForTest() {
    std::function<void()> hook;
    {
        QMutexLocker locker(&m_mutex);
        hook = g_reclaimWaitHookForTest;
    }
    // 锁外调用：钩子会阻塞等待外部放行，持锁调用会卡死所有服务入口。
    if (hook) {
        hook();
    }
}

int FlowExecutionService::activeReclaimBorrowCountForTest() const {
    QMutexLocker locker(&m_mutex);
    int total = 0;
    for (auto it = m_activeReclaimBorrows.constBegin(); it != m_activeReclaimBorrows.constEnd(); ++it) {
        total += it.value();
    }
    return total;
}

int FlowExecutionService::liveReclaimGuardCountForTest() const {
    // 不取 m_mutex：这是守卫作用域的原子计数，与计数表无关。
    // 正因为它**不**是 shutdown() 的等待条件，才能区分「shutdown 等了」与「shutdown 没等」。
    return g_liveReclaimGuards.load(std::memory_order_acquire);
}

int FlowExecutionService::inflightWithoutBorrowCountForTest() const {
    // 直接判据：inflight 里每条线程都必须已有活跃借用。
    // 若借用登记被推迟到锁外（缺陷态），锁内就能抓到「在 inflight、借用为 0」的瞬间。
    QMutexLocker locker(&m_mutex);
    int violations = 0;
    for (WorkerThread* thread : m_inflightReclaim) {
        if (!thread) {
            continue;
        }
        if (!m_activeReclaimBorrows.contains(thread) || m_activeReclaimBorrows.value(thread) <= 0) {
            ++violations;
        }
    }
    return violations;
}

void FlowExecutionService::setInCriticalSectionHookForTest(std::function<void()> hook) {
    QMutexLocker locker(&m_mutex);
    g_inCriticalSectionHookForTest = std::move(hook);
}

void FlowExecutionService::clearInCriticalSectionHookForTest() {
    QMutexLocker locker(&m_mutex);
    g_inCriticalSectionHookForTest = nullptr;
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
    QList<WorkerThread*> retiring;
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
            // T2.5 P1 第三轮（2026-11-11 二次复核 P1）：Idle 分支同样必须覆盖启动路径
            // 有界回收移交的旧 worker。只取 m_thread 会在「拒绝启动后立即 shutdown」时
            // 漏等仍在 run() 的旧 worker，令 shutdown() 在 shutdownMs=0 的情况下返回 true，
            // 破坏「返回 true 即可安全销毁 worker 所访问对象」的保证。
            m_quitRequested = true;
            m_wake.wakeAll();
            detachLiveWorkerThreadsLocked(retiring);
        }
    }
    if (nonIdle) {
        RunEngine::instance().stop();
        return false;
    }
    // T2.5 P1 第六次整改（2026-11-11 第五段复核 P1）：在删除任何 worker 之前，
    // 先等**活跃回收调用**把借用归还。
    //
    // 此时 m_reclaimAborted 已置为 true（见上面的 detachLiveWorkerThreadsLocked），
    // 新的启动会被 enqueue() 入口直接拒绝，因此不会再有新的借用产生——「禁止新启动」
    // 与「等借用归还」的先后关系正是本函数能安全终止的前提。
    //
    // 这是**生命周期等待**，与启动路径的 StartHandshakeTimeoutMs 预算无关：后者是
    // 「GUI 允许多久拿到明确失败答复」，前者是「对象何时真的可以销毁」。共用一个数字
    // 会导致「为不超预算而放行悬空引用」。因此这里无界等待，和 joinWorkerThread() 一致。
    waitForReclaimBorrowsReturned();
    for (WorkerThread* thread : retiring) {
        joinWorkerThread(thread); // 锁外 join：持锁等待会卡死所有入口
    }
    // 第四次整改：收尾完成后清除中止位，让 Service 恢复可用（shutdown 后可再次启动）。
    {
        QMutexLocker locker(&m_mutex);
        m_reclaimAborted = false;
        m_reclaimWake.wakeAll();
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
