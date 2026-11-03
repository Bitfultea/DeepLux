#pragma once

#include "../platform/Platform.h"
#include "deeplux/ControlFlowType.h"
#include "deeplux/DataContract.h"
#include "model/ImageData.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QReadWriteLock>
#include <QSet>
#include <QStack>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <functional>

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
 * @brief 运行结果
 */
struct RunResult {
    bool success;
    int errorCode;
    QString errorMessage;
    int elapsedMs;
    QDateTime finishedTime;
    QString runId;
    QString failedModule;
    QString failedPort;
    QString diagnostics;
};

struct RunObservation {
    QString runId;
    QString sessionId;
    QString lastIterationRunId;
    QString lastCompletedRunId;
    QString lastSuccessfulRunId;
    int maxCycles = 0;
    int completedCycles = 0;
    RunState state = RunState::Idle;
    bool finished = false;
    RunResult result{};
    QString currentModule;
    QDateTime startedTime;
    QPointer<Project> project;
    QByteArray projectFingerprint;
};

struct RunResumeSnapshot {
    QString runId;
    QString sessionId;
    RunState state = RunState::Idle;
    int lifecycleGeneration = 0;
    QPointer<Project> project;
    QByteArray projectFingerprint;
};

struct RunExecutionCandidates {
    QStringList moduleIds;
    const Project* project = nullptr;
    QByteArray projectFingerprint;
    bool complete = false;
    bool mayRepeat = false;
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
    enum class ProjectMutationResult { Busy, Applied, Rejected };

    // 阶6 九轮：取执行权的明确意图；tryBeginForRun 在锁内按意图校验预期状态。
    // Step：单步，fresh 由 m_stepCurrentModuleName 是否为空在锁内判定，连续单步
    // 共享同一 runId/累计状态（不每步重置）。
    enum class RunIntent { Single, CycleTick, Resume, Step };
    enum class CurrentImageInputSnapshot { NoContext, UpstreamNotExecuted, NotMaterialized, Available, ContextChanged };

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
        return isExecuting() || m_maintenance.load(std::memory_order_acquire) || state() == RunState::Running ||
               state() == RunState::Paused;
    }

    // 运行模式
    RunMode runMode() const {
        return static_cast<RunMode>(m_runMode.load(std::memory_order_acquire));
    }
    bool isCycleMode() const {
        return runMode() == RunMode::RunCycle;
    }
    QString cycleSessionId() const;
    RunResumeSnapshot resumeSnapshot() const;
    bool resumeExpected(const RunResumeSnapshot& expected, const QJsonObject& agentFolderAuthorization = {});
    bool refreshCycleProjectSnapshot(const QString& sessionId, Project* project);
    void setCycleMode(bool enabled);

    // 运行控制
    Q_INVOKABLE void runOnce();
    bool runOnceAsync(QString* acceptedRunId = nullptr, const QJsonObject& agentFolderAuthorization = {});
    Q_INVOKABLE bool stepOnce();
    bool stepOnce(const QJsonObject& agentFolderAuthorization);
    Q_INVOKABLE void resetStepState();
    Q_INVOKABLE void start();
    bool startCycle(QString* acceptedSessionId, int intervalMs = 100, bool runFirstIterationImmediately = false,
                    bool scheduleNextAfterRunFinished = false, bool deferFirstIteration = false, int maxCycles = 0,
                    const QJsonObject& agentFolderAuthorization = {});
    bool continueDeferredCycle(const QString& sessionId);
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();
    bool isPausedAtBreakpoint() const;

    // 模块管理
    void addModule(ModuleBase* module);
    bool loadProject(Project* project, ModuleFactory factory = ModuleFactory());
    void removeModule(const QString& moduleId);
    void clearModules();
    QList<ModuleBase*> modules() const;
    ModuleBase* getModule(const QString& moduleName) const;
    int getModuleIndex(const QString& moduleName) const;
    bool buildExecutionOrder(const Project* project, QString& error);
    static QString inferEdgeType(const ModuleConnection& connection, const QList<PortSpec>& sourceOutputs,
                                 const QList<PortSpec>& targetInputs);
    // Atomically replace GUI-owned runtime modules and rebuild the project graph under one maintenance lease.
    bool synchronizeProjectModules(Project* project, const QList<ModuleBase*>& modules, QString& error);
    // Run a synchronous Project mutation while excluding execution start with the lifecycle maintenance lease.
    ProjectMutationResult withProjectMutationLease(const std::function<bool()>& mutation);

    // 阶段 3.1：运行前校验（必需输入/连接类型），返回错误列表（含节点+端口定位）
    QStringList validateFlow(QString* failedModule = nullptr, QString* failedPort = nullptr) const;
    // 对候选工程做完整运行前校验；使用临时执行图，不改写当前引擎的执行/控制图状态。
    QStringList validateProjectFlow(const Project* project);

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
    // Read image input only from the current step sequence or paused run; ContextChanged is
    // fail-closed and must never be interpreted as a file-backed input.
    CurrentImageInputSnapshot currentExecutionModuleImageInput(const QString& moduleName,
                                                               const QString& upstreamSourceModuleName,
                                                               ImageData& image) const;
    bool runOutputSnapshot(const QString& runId, const QString& moduleName, ImageData& image,
                           QMap<QString, QVariant>& values, PortValueMap& ports) const;
    void invalidateModuleOutput(const QString& moduleName);

    // 运行统计
    int totalRuns() const;
    int successRuns() const;
    int failedRuns() const;
    int lastElapsedMs() const;

    // 阶段 3：运行身份与状态查询（G3）
    RunObservation runObservation(const QString& runId = QString()) const;
    RunResult lastRunResult() const;
    QString currentModuleName() const;
    QString lastRunId() const;
    RunExecutionCandidates runExecutionCandidates() const;
    RunExecutionCandidates stepExecutionCandidates() const;
    RunExecutionCandidates resumeExecutionCandidates() const;

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
    void errorOccurred(const QString& error);
    void outputChanged(const QString& moduleName, const QString& varName, const QVariant& value);

public slots:
    void onTimerTick();

private:
    RunEngine();
    ~RunEngine();

    bool executeRun(RunIntent intent, const QString& expectedCycleSessionId = QString(),
                    const QString& expectedResumeRunId = QString(), int expectedLifecycleGeneration = 0,
                    const QJsonObject& agentFolderAuthorization = {});
    bool executeRunAsync(RunIntent intent, QString* acceptedRunId, const QJsonObject& agentFolderAuthorization = {});
    void executeAcquiredRun(bool resuming, const QString& resumeModule, const ImageData& resumeData, RunMode mode);
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
    void updateStatistics(bool success, int elapsedMs);
    void reset();
    void stopCycleTimer();
    void executeCycleTick(const QString& sessionId);
    void clearActiveAgentFolderDeleteAuthorization();
    void clearActiveAgentFolderDeleteAuthorizationForPause();
    QJsonObject folderDeleteAuthorizationForModule(const QString& moduleId) const;
    QJsonObject fileOutputAuthorizationForModule(const QString& moduleId) const;

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
    // endExecutionCleanup()/finalizeAbortedRun() 等已在锁内调用它，需可重入。
    mutable QRecursiveMutex m_lifecycleMutex;
    bool m_stopPending = false;
    std::atomic_bool m_maintenance{false};
    // 阶6 十一轮：生命周期序号，每次状态转换（stop/pause/取权/收尾）自增；
    // start/resume 提交时记录 gen，发通知前重验证 gen+状态，stop 介入则跳过通知。
    std::atomic<int> m_lifecycleGeneration{0};
    std::atomic_bool m_variableExecutionLeaseHeld{false};
    RunMode m_pauseRunMode = RunMode::RunOnce; // 暂停前模式，恢复时还原（P1-2 八轮）
    // 阶6 八轮：恢复判定+暂停数据转移+取执行权+意图校验在同一临界区原子完成。
    bool tryBeginForRun(RunIntent intent, bool& resuming, QString& resumeModule, ImageData& resumeData,
                        RunMode& outMode, const QString& expectedCycleSessionId = QString(),
                        const QString& expectedResumeRunId = QString(), int expectedLifecycleGeneration = 0,
                        const QJsonObject& agentFolderAuthorization = {});
    // 阶6 七轮：正常结束在单一临界区内完成清理+最终状态提交+执行权释放。
    void finalizeRunTail(const RunResult& result);
    void recordCompletedObservation(const RunResult& result, RunState finalState);
    bool tryAcquireLease();
    void releaseLease();
    bool tryAcquireMaintenance();
    void releaseMaintenance();
    void releaseVariableExecutionLease();
    void clearModulesLocked();
    void addModuleLocked(ModuleBase* module);
    void finalizeAbortedRun(RunMode mode, const RunResult& result, bool retainCycleSession = false);
    bool retainsCycleSessionAfterFailure();
    bool stopCycleAfterRuntimeFailure(const QString& sessionId);
    bool finishBoundedCycleAfterFailure(const QString& sessionId, const RunResult& result);
    bool finishBoundedCycleAtLimit(const QString& sessionId, const RunResult& result);
    void scheduleCycleTimerAfterRunFinished(const QString& sessionId);
    bool lifecycleBusyLocked();
    // 阶6 五轮：内层断点命中在生命周期锁内写暂停态（stop() 同锁清理，无 UB）。
    void setInnerPauseLocked(const QString& mod, const ImageData& data);
    QTimer* m_cycleTimer = nullptr;
    bool m_scheduleCycleAfterRunFinished = false;
    bool m_cycleFirstIterationDeferred = false;
    bool m_cycleFailureNotificationPending = false;
    int m_cycleIntervalMs = 100;
    int m_cycleMaxIterations = 0;
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
    QSet<QString> m_disabledModules; // 阶段 B: 禁用节点（旁路/Skipped）
    mutable QReadWriteLock m_moduleLock;

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
    RunObservation m_runObservation;
    RunObservation m_lastFinishedRun;
    RunObservation m_cycleSessionObservation;
    QMap<QString, PortValueMap> m_runPortOutputs;
    QMap<QString, PortValueMap> m_lastFinishedPortOutputs;
    QMap<QString, QMap<QString, QVariant>> m_lastFinishedValues;
    QMap<QString, ImageData> m_lastFinishedImages;
    mutable QMutex m_runObservationMutex;
    QPointer<Project> m_loadedProject;
    QByteArray m_loadedProjectFingerprint;
    QString m_cycleSessionId;
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

    mutable QMutex m_agentFolderDeleteAuthorizationMutex;
    QJsonObject m_activeAgentFolderDeleteAuthorization;
    bool m_pausedAgentFolderDeleteConfirmationRequired = false; // protected by lifecycle mutex
    QString m_pausedAgentFolderDeleteAuthorizationId;           // protected by lifecycle mutex
    QJsonObject m_cycleAgentFolderDeleteAuthorization;          // protected by lifecycle mutex; session scoped
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::RunResult)
