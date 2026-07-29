#pragma once

#include "deeplux/ControlFlowType.h"
#include "model/ImageData.h"

#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QReadWriteLock>
#include <QSet>
#include <QStack>
#include <QTimer>
#include <QWaitCondition>
#include <atomic>
#include <functional>

namespace DeepLux {
class CancellationToken;
}

namespace DeepLux {

class ModuleBase;
class Project;
struct ModuleInstance;

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
class RunEngine : public QObject {
    Q_OBJECT

public:
    static RunEngine& instance();

    using ModuleFactory = std::function<ModuleBase*(const ModuleInstance&)>;

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
        return isExecuting() || state() == RunState::Running || state() == RunState::Paused;
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
    Q_INVOKABLE bool stepOnce();
    Q_INVOKABLE void resetStepState();
    Q_INVOKABLE void start();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();

    // 模块管理
    void addModule(ModuleBase* module);
    bool loadProject(Project* project, ModuleFactory factory = ModuleFactory());
    void removeModule(const QString& moduleId);
    void clearModules();
    QList<ModuleBase*> modules() const;
    ModuleBase* getModule(const QString& moduleName) const;
    int getModuleIndex(const QString& moduleName) const;
    bool buildExecutionOrder(const Project* project, QString& error);

    // 输出管理
    void setOutput(const QString& moduleName, const QString& varName, const QVariant& value);
    QVariant getOutput(const QString& moduleName, const QString& varName) const;
    bool hasOutput(const QString& moduleName, const QString& varName) const;
    void clearOutputs();

    // 流水线输出（供 UI 在 moduleFinished 后查询显示数据）
    ImageData lastOutput() const;
    ImageData moduleOutput(const QString& moduleName) const;
    void invalidateModuleOutput(const QString& moduleName);

    // 运行统计
    int totalRuns() const;
    int successRuns() const;
    int failedRuns() const;
    int lastElapsedMs() const;

    // 断点控制
    void setBreakpoint(const QString& moduleName, bool enabled);
    bool hasBreakpoint(const QString& moduleName) const;
    void setContinueFlag(bool flag) {
        QMutexLocker locker(&m_breakpointMutex);
        m_continueFlag = flag;
        if (flag) {
            m_breakpointCondition.wakeOne();
        }
    }
    void setBreakpointFlag(bool flag) {
        QMutexLocker locker(&m_breakpointMutex);
        m_breakpointFlag = flag;
        if (!flag) {
            m_breakpointCondition.wakeOne();
        }
    }
    QWaitCondition& breakpointCondition() {
        return m_breakpointCondition;
    }
    QMutex& breakpointMutex() {
        return m_breakpointMutex;
    }

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
    void errorOccurred(const QString& error);
    void outputChanged(const QString& moduleName, const QString& varName, const QVariant& value);

public slots:
    void onTimerTick();

private slots:
    void onBreakpointHit();

private:
    RunEngine();
    ~RunEngine();

    void executeRun();
    void executeModule(const QString& moduleName, ImageData& pipelineData);
    void clearModuleOutputs();
    void updateStatistics(bool success, int elapsedMs);
    void reset();

    void buildModuleTree();
    void clearModuleTree();
    QString getNextModule(const QString& currentModule, bool lastResult);
    QString getNextSequentialModule(const QString& currentModule);
    QString findSiblingByFlowType(const QString& currentModule, ControlFlowType targetType);
    QString findPreviousByFlowType(const QString& currentModule, ControlFlowType targetType);
    std::atomic<int> m_state{static_cast<int>(RunState::Idle)};
    std::atomic<int> m_runMode{static_cast<int>(RunMode::None)};
    std::atomic_bool m_executing{false};
    QTimer* m_cycleTimer = nullptr;
    QList<ModuleBase*> m_modules;
    QList<ModuleBase*> m_ownedModules;
    QMap<QString, ModuleBase*> m_moduleMap;
    QStringList m_executionOrder;
    mutable QReadWriteLock m_moduleLock;

    // 模块树结构
    QMap<QString, ModuleTreeNode*> m_moduleTreeNodes;
    ModuleTreeNode* m_rootNode = nullptr;
    QStack<ModuleTreeNode*> m_nodeStack;

    // 循环索引
    QMap<QString, int> m_loopIndices;

    // 输出映射
    QMap<QString, QMap<QString, QVariant>> m_outputMap;
    mutable QMutex m_outputMutex;

    // 断点控制
    QSet<QString> m_breakpoints;
    bool m_breakpointFlag = false;
    bool m_continueFlag = false;
    mutable QMutex m_breakpointMutex;
    QWaitCondition m_breakpointCondition;

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
};

} // namespace DeepLux
