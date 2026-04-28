#pragma once

#include <QObject>
#include <QTimer>
#include <QList>
#include <QDateTime>
#include <QMap>
#include <QStack>
#include <QMutex>
#include <QWaitCondition>
#include <QSet>

#include "deeplux/ControlFlowType.h"
#include "model/ImageData.h"

namespace DeepLux {
class CancellationToken;
}

namespace DeepLux {

class ModuleBase;

/**
 * @brief 运行状态
 */
enum class RunState {
    Idle,       // 空闲
    Running,    // 运行中
    Paused,     // 已暂停
    Stopped     // 已停止
};

/**
 * @brief 运行模式
 */
enum class RunMode {
    None,
    RunOnce,    // 单次运行
    RunCycle    // 连续运行
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
class ModuleTreeNode
{
public:
    QString moduleName;
    ModuleTreeNode* parent = nullptr;
    QList<ModuleTreeNode*> children;

    ModuleTreeNode(const QString& name = QString()) : moduleName(name) {}
};

/**
 * @brief 流程运行引擎
 */
class RunEngine : public QObject
{
    Q_OBJECT

public:
    static RunEngine& instance();

    // 运行状态
    RunState state() const { return m_state; }
    bool isRunning() const { return m_state == RunState::Running; }
    bool isPaused() const { return m_state == RunState::Paused; }
    bool isStopped() const { return m_state == RunState::Stopped; }

    // 运行模式
    RunMode runMode() const { return m_runMode; }
    bool isCycleMode() const { return m_runMode == RunMode::RunCycle; }
    void setCycleMode(bool enabled);

    // 运行控制
    Q_INVOKABLE void runOnce();
    Q_INVOKABLE void start();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void stop();

    // 模块管理
    void addModule(ModuleBase* module);
    void removeModule(const QString& moduleId);
    void clearModules();
    QList<ModuleBase*> modules() const { return m_modules; }
    ModuleBase* getModule(const QString& moduleName) const;
    int getModuleIndex(const QString& moduleName) const;

    // 输出管理
    void setOutput(const QString& moduleName, const QString& varName, const QVariant& value);
    QVariant getOutput(const QString& moduleName, const QString& varName) const;
    bool hasOutput(const QString& moduleName, const QString& varName) const;
    void clearOutputs();

    // 流水线输出（供 UI 在 moduleFinished 后查询显示数据）
    const ImageData& lastOutput() const { return m_lastOutput; }

    // 运行统计
    int totalRuns() const { return m_totalRuns; }
    int successRuns() const { return m_successRuns; }
    int failedRuns() const { return m_failedRuns; }
    int lastElapsedMs() const { return m_lastElapsedMs; }

    // 断点控制
    void setBreakpoint(const QString& moduleName, bool enabled);
    bool hasBreakpoint(const QString& moduleName) const;
    void setContinueFlag(bool flag) { m_continueFlag = flag; }
    void setBreakpointFlag(bool flag) { m_breakpointFlag = flag; }
    QWaitCondition& breakpointCondition() { return m_breakpointCondition; }
    QMutex& breakpointMutex() { return m_breakpointMutex; }

    // 取消令牌
    CancellationToken* cancellationToken() { return m_cancellationToken; }
    void requestCancellation();

signals:
    void stateChanged(RunState state);
    void runStarted();
    void runFinished(const RunResult& result);
    void cycleStarted();
    void cycleStopped();
    void moduleStarted(const QString& moduleId);
    void moduleFinished(const QString& moduleId, bool success);
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
    void updateStatistics(bool success, int elapsedMs);
    void reset();

    void buildModuleTree();
    void clearModuleTree();
    QString getNextModule(const QString& currentModule, bool lastResult);
    QString getNextSequentialModule(const QString& currentModule);
    QString findSiblingByFlowType(const QString& currentModule, ControlFlowType targetType);
    QString findPreviousByFlowType(const QString& currentModule, ControlFlowType targetType);

    RunState m_state = RunState::Idle;
    RunMode m_runMode = RunMode::None;
    QTimer* m_cycleTimer = nullptr;
    QList<ModuleBase*> m_modules;
    QMap<QString, ModuleBase*> m_moduleMap;

    // 模块树结构
    QMap<QString, ModuleTreeNode*> m_moduleTreeNodes;
    ModuleTreeNode* m_rootNode = nullptr;
    QStack<ModuleTreeNode*> m_nodeStack;

    // 循环索引
    QMap<QString, int> m_loopIndices;

    // 输出映射
    QMap<QString, QMap<QString, QVariant>> m_outputMap;

    // 断点控制
    QSet<QString> m_breakpoints;
    bool m_breakpointFlag = false;
    bool m_continueFlag = false;
    QMutex m_breakpointMutex;
    QWaitCondition m_breakpointCondition;

    // 统计
    int m_totalRuns = 0;
    int m_successRuns = 0;
    int m_failedRuns = 0;
    int m_lastElapsedMs = 0;
    QDateTime m_runStartTime;

    QString m_currentModuleName;
    bool m_lastExecuteResult = true;
    ImageData m_lastOutput;   // 最后一个模块的输出，供 UI 显示

    CancellationToken* m_cancellationToken = nullptr;
};

} // namespace DeepLux