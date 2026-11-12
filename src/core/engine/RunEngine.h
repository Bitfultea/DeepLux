#pragma once

#include "../platform/Platform.h"
#include "DisplayFrame.h"
#include "ExecutionAffinity.h"
#include "deeplux/ControlFlowType.h"
#include "deeplux/DataContract.h"
#include "model/ImageData.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>
#include <QStack>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <functional>
#include <optional>

namespace DeepLux {
class CancellationToken;
}

namespace DeepLux {

class ModuleBase;
class Project;
struct ModuleInstance;
struct ModuleConnection;

/**
 * @brief 运行状态
 */
enum class RunState {
    Idle,    // 空闲
    Running, // 运行中
    Paused,  // 已暂停
    Stopped  // 已停止
};

/**
 * @brief 运行模式
 */
enum class RunMode {
    None,
    RunOnce, // 单次运行
    RunCycle // 连续运行
};

/**
 * @brief 单次执行段收尾结果（后台执行设计 §5.3）
 *
 * - Completed：正常完成；终态按 RunMode 决定（RunOnce→Idle，原生 RunCycle→保持 Running）。
 * - Paused：模块边界暂停，只释放本段执行权并保留续跑上下文，不发 runFinished。
 * - Stopped：stop 优先（并发 stop 不得被 Completed 覆盖）。
 * - Failed：失败/异常，清理暂停上下文并发出失败结果。
 */
enum class ExecutionOutcome { Completed, Paused, Stopped, Failed };

/**
 * @brief 模块边界暂停原因（后台执行设计 §5.3）
 *
 * - None：未暂停；Breakpoint：内层断点命中；User：pauseForJob 的用户暂停请求。
 *   isPausedAtBreakpoint() 仍只在 Breakpoint 时为真；UI/Service 用 pauseReason() 决定提示。
 */
enum class PauseReason { None, Breakpoint, User };

/**
 * @brief 运行结果
 */
struct RunResult {
    bool success;
    int errorCode;
    QString errorMessage;
    int elapsedMs;
    QDateTime finishedTime;
};

/**
 * @brief 模块树节点 - 用于流程控制
 */
class ModuleTreeNode {
public:
    QString moduleName;
    ModuleTreeNode* parent = nullptr;
    QList<ModuleTreeNode*> children;

    ModuleTreeNode(const QString& name = QString()) : moduleName(name) {}
};

/**
 * @brief 流程运行引擎
 */
class DEEPLUX_API RunEngine : public QObject {
    Q_OBJECT

public:
    static RunEngine& instance();

    using ModuleFactory = std::function<ModuleBase*(const ModuleInstance&)>;

    // 阶6 九轮：取执行权的明确意图；tryBeginForRun 在锁内按意图校验预期状态。
    // Step：单步，fresh 由 m_stepCurrentModuleName 是否为空在锁内判定，连续单步
    // 共享同一 runId/累计状态（不每步重置）。
    enum class RunIntent { Single, CycleTick, Resume, Step };

    /**
     * @brief 外部 job 租约 token（后台执行设计 §4.2/§5.3）
     *
     * 单调 id + 进程内随机盐，不可伪造；FlowExecutionService 的 job 在工作线程内持有，
     * 只有 token 匹配的 *ForJob 入口可绕过 external job 活跃的忙检查。
     */
    struct JobToken {
        quint64 id = 0;
        quint64 salt = 0;
        bool isValid() const {
            return id != 0;
        }
        bool operator==(const JobToken& other) const {
            return id == other.id && salt == other.salt;
        }
        bool operator!=(const JobToken& other) const {
            return !(*this == other);
        }
    };

    // job 租约与所有者入口（后台执行设计 §4.2/§5.3）
    bool beginExternalJob(JobToken* token, QString* reason = nullptr);
    bool endExternalJob(const JobToken& token);
    bool runOnceForJob(const JobToken& token, QString* reason = nullptr);
    bool stepOnceForJob(const JobToken& token, QString* reason = nullptr);
    bool resumeForJob(const JobToken& token, QString* reason = nullptr);
    bool pauseForJob(const JobToken& token);
    bool runOnceChecked(QString* reason = nullptr);

    /**
     * @brief 执行段 RAII 收尾器（后台执行设计 §5.3）
     *
     * 正常返回、提前 return、异常都会收尾恰好一次；默认 outcome=Failed（异常安全）。
     * Paused 必须显式 finish 提交，不能被析构默认结果覆盖；终态提交与 stop 判定在
     * 同一 m_lifecycleMutex 临界区内完成，保证并发 stop 优先。
     */
    class ExecutionFinalizer {
    public:
        // clearControlGraph：完整运行收尾清理控制图/循环索引；单步收尾必须保留跨步状态。
        // resetStepStateOnStop：单步收尾在 stop 获胜时重置单步态（与 stop 判定同一临界区）。
        ExecutionFinalizer(RunEngine* engine, ExecutionOutcome outcome, bool clearControlGraph = true,
                           bool resetStepStateOnStop = false);
        ~ExecutionFinalizer();
        ExecutionFinalizer(const ExecutionFinalizer&) = delete;
        ExecutionFinalizer& operator=(const ExecutionFinalizer&) = delete;

        void setOutcome(ExecutionOutcome outcome) {
            m_outcome = outcome;
        }
        void finish(ExecutionOutcome outcome);

    private:
        RunEngine* m_engine = nullptr;
        ExecutionOutcome m_outcome = ExecutionOutcome::Failed;
        bool m_clearControlGraph = true;
        bool m_resetStepStateOnStop = false;
        bool m_done = false;
    };

    // 运行状态
    RunState state() const {
        return static_cast<RunState>(m_state.load(std::memory_order_acquire));
    }
    bool isRunning() const {
        return state() == RunState::Running;
    }
    bool isPaused() const {
        return state() == RunState::Paused;
    }
    bool isStopped() const {
        return state() == RunState::Stopped;
    }
    bool isExecuting() const {
        return m_executing.load(std::memory_order_acquire);
    }
    bool isBusy() const {
        // 阶6 五轮：维护租约期间也视为 busy，使外部能观察到维护占用。
        // 后台执行（设计 §4.2）：external job 租约从 job 启动到结束（含 Cycle 两轮之间）
        // 始终为 busy，工程加载/清空/切换被拒绝。
        return isExecuting() || m_maintenance.load(std::memory_order_acquire) ||
               m_externalJobActive.load(std::memory_order_acquire) || state() == RunState::Running ||
               state() == RunState::Paused;
    }

    // 运行模式
    RunMode runMode() const {
        return static_cast<RunMode>(m_runMode.load(std::memory_order_acquire));
    }
    bool isCycleMode() const {
        return runMode() == RunMode::RunCycle;
    }
    void setCycleMode(bool enabled);

    // 运行控制
    Q_INVOKABLE void runOnce();
    bool runOnceAsync();
    Q_INVOKABLE bool stepOnce();
    Q_INVOKABLE void resetStepState();
    Q_INVOKABLE void start();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    bool isPausedAtBreakpoint() const;
    // 后台执行（设计 §5.3）：暂停原因（None/Breakpoint/User），UI/Service 用其决定提示。
    PauseReason pauseReason() const;

    // 模块管理
    void addModule(ModuleBase* module);
    bool loadProject(Project* project, ModuleFactory factory = ModuleFactory());
    void removeModule(const QString& moduleId);
    void clearModules();
    QList<ModuleBase*> modules() const;
    ModuleBase* getModule(const QString& moduleName) const;
    int getModuleIndex(const QString& moduleName) const;
    bool buildExecutionOrder(const Project* project, QString& error);

    // 阶段 3.1：运行前校验（必需输入/连接类型），返回错误列表（含节点+端口定位）
    QStringList validateFlow() const;

    // 阶段 4.8：结构化运行前校验（validateFlow 的唯一实现来源，消息文案逐字保持）
    struct FlowIssue {
        QString code;    // "missing_required_input" | "disabled_output_bypass_conflict"
        QString node;    // 运行名
        QString port;    // 可为空
        QString message; // 与 validateFlow() 完全一致的中文文案
    };
    QList<FlowIssue> validateFlowIssues() const;

    // 输出管理
    void setOutput(const QString& moduleName, const QString& varName, const QVariant& value);
    QVariant getOutput(const QString& moduleName, const QString& varName) const;
    bool hasOutput(const QString& moduleName, const QString& varName) const;
    void clearOutputs();

    // 流水线输出（供 UI 在 moduleFinished 后查询显示数据）
    ImageData lastOutput() const;
    // 最近一次产生输出的模块名（供 UI 追溯结果归属，避免多支路错配）
    QString lastOutputModuleName() const;
    ImageData moduleOutput(const QString& moduleName) const;
    void invalidateModuleOutput(const QString& moduleName);

    // 阶段 4.8：只读快照（前置条件：!isBusy()；GUI 线程调用）。
    // isBusy() 为真时返回空/默认值并 qWarning，禁止与工作线程写 m_nodeOutputs 竞争。
    struct ModuleRuntimeInfo {
        bool valid = false;
        bool success = false;
        bool skipped = false;
        int elapsedMs = 0;
        QString errorMessage;
        QDateTime finishedAt;
    };
    QMap<QString, ModuleRuntimeInfo> moduleRuntimeSnapshot() const;
    PortValueMap nodePortOutputs(const QString& moduleName) const;
    qint64 lastFrameId() const;

    // 后台执行（设计 §7.1/§7.5）：显示帧捕获开关。由 UI 在窗口可见时开启、不可见/最小化时
    // 关闭；关闭时工作线程不做快照与合并调度（零额外开销）。
    void setDisplayCaptureEnabled(bool enabled);
    bool isDisplayCaptureEnabled() const {
        return m_displayCaptureEnabled.load(std::memory_order_acquire);
    }
    // Service 在 job 启动时注入 jobId、结束时清空（设计 §7.1）；注入同时清空旧帧槽位，
    // 旧 job 的迟到帧不会再从槽位被取走。
    void setDisplayJobId(quint64 jobId);
    void clearDisplayJobId();
    quint64 displayJobId() const {
        return m_displayJobId.load(std::memory_order_acquire);
    }
    // 每视口最新帧槽位（只保留最新一帧；读出的帧是深拷贝后的不可变快照）。
    DisplayFrame latestDisplayFrame(const QString& viewportId = QStringLiteral("main")) const;
    void clearLatestDisplayFrames();

    // L2 测试注入：向 m_nodeOutputs 写入自定义端口值（生产构建不可见）
    DEEPLUX_TEST_VISIBILITY : void setNodePortOutputsForTest(const QString& moduleName, const PortValueMap& outputs);
    // B1 测试观察：排序分析与控制图元数据（生产构建不可见）。
    DEEPLUX_TEST_VISIBILITY : QStringList executionOrderForTest() const;
    DEEPLUX_TEST_VISIBILITY : QList<ModuleConnection> controlEdgesForTest() const;
    DEEPLUX_TEST_VISIBILITY : QVariantMap loopMetadataForTest() const; // back/reentrant/regions
    // 后台执行（设计 §6.1/§6.3）测试注入：覆盖某运行名的执行亲和性与并行资格判定；
    // 亲和性缓存本身由 addModule/removeModule/loadProject/clearModules 维护（生产构建不可见）。
    DEEPLUX_TEST_VISIBILITY : void setModuleExecutionAffinityForTest(const QString& moduleName,
                                                                     ExecutionAffinity affinity);
    DEEPLUX_TEST_VISIBILITY : bool isParallelEligibleForTest(const QString& moduleName) const;
    // 后台执行（设计 §5.3）测试注入：收尾窗口并发 stop 的确定性钩子（生产构建不可见）。
    DEEPLUX_TEST_VISIBILITY : void setFinalizeHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearFinalizeHookForTest();
#ifdef DEEPLUX_BUILD_TESTS
    DEEPLUX_TEST_VISIBILITY : void setBeforeFinalDisplayFrameHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearBeforeFinalDisplayFrameHookForTest();
#endif
    // 后台执行（设计 §5.3）测试注入：执行段故障注入（验证异常路径 finalizer 释放租约）。
    DEEPLUX_TEST_VISIBILITY : void setExecutionFaultInjectionForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearExecutionFaultInjectionForTest();
    // 后台执行（设计 §4.2/§5.3）测试注入：job 租约活跃查询与 *ForJob 窗口屏障钩子。
    DEEPLUX_TEST_VISIBILITY : bool isExternalJobActiveForTest() const;
    DEEPLUX_TEST_VISIBILITY : void setBeforeJobStartHookForTest(std::function<void()> hook);
    DEEPLUX_TEST_VISIBILITY : void clearBeforeJobStartHookForTest();
    // T2.2：GUI 回投超时注入与迟到回调观测（生产不可达）。
    DEEPLUX_TEST_VISIBILITY : void setGuiDispatchTimeoutForTest(int msecs);
    DEEPLUX_TEST_VISIBILITY : QString lastGuiDispatchErrorCodeForTest() const;
    DEEPLUX_TEST_VISIBILITY : int guiDispatchLateCallbackDropCountForTest() const;
    DEEPLUX_TEST_VISIBILITY : int guiDispatchTimeoutHitCountForTest() const;
    DEEPLUX_TEST_VISIBILITY : int guiDispatchRunningWaitEnteredCountForTest() const;

public:
    // 运行统计
    int totalRuns() const;
    int successRuns() const;
    int failedRuns() const;
    int lastElapsedMs() const;

    // 并行执行（阶段 3.3）
    void setParallelThreadCount(int n);
    int parallelThreadCount() const;
    /// 并发执行一组独立模块；任一失败取消同组并返回首个错误+诊断。
    ExecutionResult executeParallel(const QStringList& moduleNames, const PortValueMap& sharedInput);
    /// 上一次 executeParallel 观测到的最大并发度（用于证明真实并发）。
    int lastParallelMaxConcurrency() const {
        return m_lastParallelMaxConcurrency.load(std::memory_order_acquire);
    }

    // 断点控制
    void setBreakpoint(const QString& moduleName, bool enabled);
    bool hasBreakpoint(const QString& moduleName) const;
    void setModuleDisabled(const QString& moduleName, bool disabled);
    bool isModuleDisabled(const QString& moduleName) const;

    // 取消令牌
    CancellationToken* cancellationToken() {
        return m_cancellationToken;
    }
    void requestCancellation();

signals:
    void stateChanged(RunState state);
    void runStarted();
    void runFinished(const RunResult& result);
    void cycleStarted();
    void cycleStopped();
    void moduleStarted(const QString& moduleId);
    void moduleFinished(const QString& moduleId, bool success, int elapsedMs);
    void moduleSkipped(const QString& moduleId); // 未激活分支，显示 Skipped 非失败
    void breakpointHit(const QString& moduleId); // 阶段 B 复核: 断点真实命中
    // 后台执行（设计 §5.3）：模块边界暂停（断点/用户）提交后发出，附暂停原因。
    void pauseHit(const QString& moduleId, PauseReason reason);
    // 后台执行（设计 §7.1）：轻量显示帧通知——不把大图作为信号参数排队；
    // 接收方按 (viewportId, jobId, runId) 取 latestDisplayFrame() 并丢弃身份不匹配的迟到帧。
    void displayFrameReady(const QString& viewportId, quint64 jobId, const QString& runId);
    void errorOccurred(const QString& error);
    void outputChanged(const QString& moduleName, const QString& varName, const QVariant& value);

public slots:
    void onTimerTick();

private:
    RunEngine();
    ~RunEngine();

    void executeRun(RunIntent intent);
    bool executeRunAsync(RunIntent intent);
    // 后台执行（设计 §4.2）：公开 stepOnce() 是无 token 包装；stepOnceForJob 走 owner 校验后
    // 复用同一推进实现（token=null 表示公开入口，需拒绝 external job 活跃时的调用）。
    bool stepOnceImpl(const JobToken* token, QString* rejectReason = nullptr);
    // 后台执行（设计 §4.2/§5.3）：校验 job 所有权与 job 级取消标志。调用方必须持有
    // m_lifecycleMutex（tryBeginForRun 在状态提交的同一临界区内调用，与 stop() 严格串行）。
    bool validateJobStartLocked(const JobToken& token, QString* reason);
    void executeAcquiredRun(bool resuming, const QString& resumeModule, const ImageData& resumeData, RunMode mode);
    // 后台执行（设计 §5.3）：执行主体与异常兜底分离；finalizer 在所有路径上收尾恰好一次。
    void executeAcquiredRunImpl(bool resuming, const QString& resumeModule, const ImageData& resumeData, RunMode mode,
                                ExecutionFinalizer& finalizer);
    void reportExecutionFault(ExecutionFinalizer& finalizer, const QString& message);
    // 阶段 D1: 显式控制图执行（激活队列）
    void executeRunWithControlGraph(ImageData& pipelineData);
    void executeBatchParallel(const QStringList& batch, ImageData& pipelineData);
    PortValueMap collectModuleInputs(const QString& moduleName, const ImageData& pipelineData) const;
    void initializeControlQueue();
    void activateControlSuccessors(const QString& moduleName);
    void tryActivateSuccessor(const QString& sourceModule, const QString& targetModule, bool controlTriggered,
                              const QString& sourcePort = QString(), const QString& targetPort = QString());
    bool dataInputsReady(const QString& moduleName) const;
    void clearLoopIterationOutputs(const QString& loopModule);
    void emitInactiveControlModules();
    void clearControlQueue();
    void executeRunLegacy(ImageData& pipelineData, bool& allSuccess, QString& firstError);
    void executeModule(const QString& moduleName, ImageData& pipelineData);
    // 阶段 B 复核: 完整运行与单步共用的"执行或禁用旁路"入口；含断点命中。
    void executeOrBypassModule(const QString& moduleName, ImageData& pipelineData);
    PortValueMap collectBypass(const QString& moduleName);
    void clearModuleOutputs();
    void clearBreakpointPauseState();
    // 后台执行（设计 §7.1）：在执行线程内发布显示帧（含 QImage 深拷贝与最终快照）。
    // T2.9：runId 由调用线程快照后传入，本函数不再读 m_runId（池线程安全边界）。
    void publishDisplayFrame(const QString& moduleId, const ImageData& image, quint64 frameId, const QString& runId);
    std::optional<DisplayFrame> snapshotFinalDisplayFrame(const QString& runId) const;
    void publishFinalDisplayFrame(std::optional<DisplayFrame> frame);
    QMap<QString, QVariantMap> displayModuleDataSnapshot() const;
    void updateStatistics(bool success, int elapsedMs);
    // 阶段 4.8：记录/清理模块运行快照（写入点集中在模块执行结束与旁路分支）
    void recordModuleRuntime(const QString& moduleName, const ModuleRuntimeInfo& info);
    void reset();
    void stopCycleTimer();

    void buildModuleTree();
    void clearModuleTree();
    QString getNextModule(const QString& currentModule, bool lastResult);
    QString getNextSequentialModule(const QString& currentModule);
    QString findSiblingByFlowType(const QString& currentModule, ControlFlowType targetType);
    QString findPreviousByFlowType(const QString& currentModule, ControlFlowType targetType);
    std::atomic<int> m_state{static_cast<int>(RunState::Idle)};
    std::atomic<int> m_runMode{static_cast<int>(RunMode::None)};
    std::atomic_bool m_executing{false};
    // 阶6 复核（四轮）：唯一排他生命周期租约。执行（runOnce/onTimerTick/resume/
    // stepOnce）、维护（add/remove/clear/load）、公开并行原语（executeParallel）
    // 共用 m_lifecycleMutex 下的 m_executing/m_maintenance 租约，互斥且 check-then-act
    // 原子。tryBeginExecution() 在同一临界区完成"取执行权+runMode/state 提交+token
    // 重置+每运行重置"，消除启动窗口；stop() 同锁发布停止。m_stopPending/m_maintenance
    // 仅在 m_lifecycleMutex 内读写。
    // 阶6 五轮：递归锁——clearBreakpointPauseState() 自身加锁，而 stop()/
    // finalizeExecution() 等已在锁内调用它，需可重入。
    mutable QRecursiveMutex m_lifecycleMutex;
    bool m_stopPending = false;
    std::atomic_bool m_maintenance{false};
    // 阶6 十一轮：生命周期序号，每次状态转换（stop/pause/取权/收尾）自增；
    // start/resume 提交时记录 gen，发通知前重验证 gen+状态，stop 介入则跳过通知。
    std::atomic<int> m_lifecycleGeneration{0};
    // 后台执行（设计 §5.3）：最近一次收尾的生命周期世代，防御重复收尾。
    int m_finalizedGeneration = -1;
    // 后台执行（设计 §5.3）测试注入：收尾窗口钩子/执行段故障注入（生产构建不可达）。
    std::function<void()> m_testFinalizeHook;
    std::function<void()> m_testFaultInjection;
    // 后台执行（设计 §4.2/§5.3）测试注入：*ForJob 启动协议内的窗口屏障钩子（生产构建不可达）。
    std::function<void()> m_testBeforeJobStartHook;
    // 后台执行（设计 §4.2/§5.3）：external job 租约、owner token 与 job 级取消标志。
    // m_externalJobActive 供 isBusy() 无锁快照；其余字段仅在 m_lifecycleMutex 内读写。
    std::atomic_bool m_externalJobActive{false};
    JobToken m_externalJobOwner;
    quint64 m_externalJobSeq = 0;
    quint64 m_jobSalt = 0;
    bool m_externalJobCancelRequested = false;
    // 后台执行（设计 §5.3）：用户暂停请求（模块边界生效）。
    std::atomic_bool m_userPauseRequested{false};
    // 后台执行（设计 §5.3）：暂停原因（None/Breakpoint/User），与暂停上下文同锁维护。
    PauseReason m_pauseReason = PauseReason::None;
    RunMode m_pauseRunMode = RunMode::RunOnce; // 暂停前模式，恢复时还原（P1-2 八轮）
    // T2.2：GUI 回投超时、世代与迟到回调丢弃计数。世代变化使尚未开始的回调整体作废。
    std::atomic<int> m_guiDispatchTimeoutMs{5000};
    std::atomic<quint64> m_dispatchGeneration{0};
    std::atomic<int> m_guiDispatchLateDrops{0};
    std::atomic<int> m_guiDispatchTimeoutHits{0};
    std::atomic<int> m_guiDispatchRunningWaitHits{0};
    mutable QMutex m_guiDispatchMutex;
    QString m_lastGuiDispatchErrorCode;
    // 阶6 八轮：恢复判定+暂停数据转移+取执行权+意图校验在同一临界区原子完成。
    // 后台执行（设计 §4.2/§5.3）：jobToken 非空时，owner/job 级取消标志校验与状态提交
    // 在同一把 m_lifecycleMutex 临界区完成（窗口钩子亦在其中），使 stop() 与本次启动严格
    // 串行：stop 先到 → 取消标志拒绝；启动先提交 → stop 置 m_stopPending 取消本轮。
    bool tryBeginForRun(RunIntent intent, bool& resuming, QString& resumeModule, ImageData& resumeData,
                        RunMode& outMode, const JobToken* jobToken = nullptr, QString* rejectReason = nullptr);
    // 阶6 七轮：正常结束在单一临界区内完成清理+最终状态提交+执行权释放。
    // 后台执行（设计 §5.3）：唯一收尾提交点；Completed 终态按 RunMode 决定，stop 优先。
    void finalizeExecution(ExecutionOutcome outcome, bool clearControlGraph = true, bool resetStepStateOnStop = false);
    bool tryAcquireLease();
    void releaseLease();
    bool tryAcquireMaintenance();
    void releaseMaintenance();
    void clearModulesLocked();
    void addModuleLocked(ModuleBase* module);
    bool lifecycleBusyLocked();
    // 后台执行（设计 §6.1）：运行名 -> 执行亲和性缓存；缓存项随模块增删/工程加载同步维护。
    ExecutionAffinity executionAffinityFor(const QString& moduleName, ModuleBase* module) const;
    void cacheExecutionAffinity(ModuleBase* module);
    void forgetExecutionAffinity(const QString& moduleName);
    // 后台执行（设计 §6.3）：worker 亲和 + 线程安全 + 非阻塞 + 无断点才允许进入并行池。
    bool isParallelEligible(ModuleBase* module) const;
    // 阶6 五轮：内层断点命中在生命周期锁内写暂停态（stop() 同锁清理，无 UB）。
    // 后台执行（设计 §5.3）：用户暂停复用同一暂停上下文，用 PauseReason 区分原因。
    void setInnerPauseLocked(const QString& mod, const ImageData& data, PauseReason reason);
    // 后台执行（设计 §5.3）：模块边界检查并消费用户暂停请求（锁内 check-then-act）。
    bool takeUserPauseRequest(const QString& moduleName, const ImageData& data);
    QTimer* m_cycleTimer = nullptr;
    QList<ModuleBase*> m_modules;
    QList<ModuleBase*> m_ownedModules;
    QMap<QString, ModuleBase*> m_moduleMap;
    QStringList m_executionOrder;
    QList<ModuleConnection> m_connections;  // 阶段 3.1 边集合（端口路由）
    QList<ModuleConnection> m_controlEdges; // 阶段 C: 控制边（不参与数据 DAG）
    QStringList m_controlQueue;
    QSet<QString> m_controlActivated;
    QSet<QString> m_controlProcessed;
    QMap<QString, QSet<QString>> m_triggeredControlEdges; // 目标 -> 已触发的具体控制边
    QSet<QString> m_loopBackEdges;
    QSet<QString> m_reentrantModules;
    QMap<QString, QSet<QString>> m_loopRegions;
    // 当前帧每节点端口输出缓存（flowId 现阶段为 main）；新帧清除
    QMap<QString, PortValueMap> m_nodeOutputs;
    // 阶段 4.8：最近一次运行的模块状态快照（m_runtimeInfoMutex 保护）
    QMap<QString, ModuleRuntimeInfo> m_moduleRuntimeInfo;
    mutable QMutex m_runtimeInfoMutex;
    QSet<QString> m_disabledModules; // 阶段 B: 禁用节点（旁路/Skipped）
    mutable QReadWriteLock m_moduleLock;
    // 后台执行（设计 §6.1）：execution.affinity 缓存（m_affinityMutex 保护）
    mutable QMutex m_affinityMutex;
    QHash<QString, ExecutionAffinity> m_executionAffinity;

    // 后台执行（设计 §7.1）：显示帧捕获上下文与每视口最新帧槽位（只保留最新一帧）。
    // m_displayCaptureEnabled/m_displayJobId 供跨线程无锁快照；槽位由 m_displayFrameMutex 保护。
    std::atomic_bool m_displayCaptureEnabled{false};
    std::atomic<quint64> m_displayJobId{0};
    mutable QMutex m_displayFrameMutex;
    QMap<QString, DisplayFrame> m_latestDisplayFrames;

    // 模块树结构
    QMap<QString, ModuleTreeNode*> m_moduleTreeNodes;
    ModuleTreeNode* m_rootNode = nullptr;
    QStack<ModuleTreeNode*> m_nodeStack;

    // 循环索引
    QMap<QString, int> m_loopIndices;
    QStringList m_activeLoops;

    // 输出映射
    QMap<QString, QMap<QString, QVariant>> m_outputMap;
    mutable QMutex m_outputMutex;

    // 断点控制
    // 断点集合（m_breakpointMutex 保护）
    QSet<QString> m_breakpoints;
    mutable QMutex m_breakpointMutex;
    // 阶段 B 复核: 非阻塞断点暂停/恢复
    QString m_pauseResumeModule;
    ImageData m_pausePipelineData;
    QDateTime m_breakpointPausedAt;
    bool m_pausedAtBreakpoint = false;
    bool m_skipBreakpointOnce = false;
    bool m_runAllSuccess = true;
    QString m_runFirstError;

    // 统计
    int m_totalRuns = 0;
    int m_successRuns = 0;
    int m_failedRuns = 0;
    int m_lastElapsedMs = 0;
    mutable QMutex m_statsMutex;
    QDateTime m_runStartTime;

    QString m_currentModuleName;
    QString m_stepCurrentModuleName;
    ImageData m_stepPipelineData;
    bool m_lastExecuteResult = true;
    bool m_lastControlResult = true;
    QString m_lastModuleError;
    ImageData m_lastOutput;
    QString m_lastOutputModuleName;
    QMap<QString, ImageData> m_moduleOutputs;
    mutable QMutex m_lastOutputMutex;

    CancellationToken* m_cancellationToken = nullptr;

    // ABI v2 执行上下文：一次运行的 ID 与递增帧号
    QString m_runId;
    std::atomic<qint64> m_runSeq{0}; // 阶6 复核：runId 单调序号，保证每次运行唯一
    std::atomic<qint64> m_frameId{0};

    // 阶段 3.3 受控并行线程池
    QThreadPool m_runPool;
    QThreadPool m_parallelPool;
    int m_parallelThreads = 1;
    std::atomic<int> m_lastParallelMaxConcurrency{0};
    mutable QMutex m_parallelMutex;
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::RunResult)
Q_DECLARE_METATYPE(DeepLux::PauseReason)
