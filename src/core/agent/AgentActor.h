#ifndef DEEPLUX_AGENT_ACTOR_H
#define DEEPLUX_AGENT_ACTOR_H

#include "../platform/Platform.h"

#include <QJsonArray>
#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <functional>
#include <memory>
#include <QObject>
#include <QStringList>
#include <QUndoStack>

namespace DeepLux {

class AgentRunWaiter;
class Project;
class AgentController;

/**
 * @brief Agent Actor —— Tool API 执行器
 *
 * 将 LLM 返回的 tool calls 路由到实际的 Manager API。
 * 工程内编辑通过 QUndoStack 包装；新建/打开工程会清空撤销历史。
 *
 * 安全约束：
 * - 只能调用预注册的 ToolSchema 白名单工具
 * - 不能执行 bash / system / QProcess
 */
class DEEPLUX_API AgentActor : public QObject {
    Q_OBJECT

public:
    explicit AgentActor(QObject* parent = nullptr);
    ~AgentActor() override;

    // 执行单个 tool call
    QJsonObject executeTool(const QString& toolName, const QJsonObject& params);
    QJsonObject userPermissionPreflight(const QString& toolName, const QJsonObject& params) const;

    // 纯检查已发布的 ProjectFlow selector；不读取或改变工程/运行态。
    QJsonObject flowSelectorPreflight(const QString& toolName, const QJsonObject& params) const;

    /**
     * @brief 批次中的单个工具调用（阶段1 §7：callId 贯通，保证每个原始 call 恰有一个结果）
     */
    struct ToolInvocation {
        QString callId; // LLM tool_call 原始 id；缺失时由调用方生成稳定占位
        QString tool;
        QJsonObject params;
        QJsonObject cameraTargetSnapshot = {}; // confirmation-only; not part of public tool parameters/schema
        QJsonObject folderDeleteTargetAuthorization = {}; // per confirmed execution call
        QJsonObject variableSnapshot = {};                // confirmation-only; never sent to or echoed from the LLM
        QJsonObject dataSourceImportSnapshot = {};        // per-call confirmed source identity/content snapshot
        QJsonObject annotationExportSnapshot = {};        // per-call confirmed session and destination snapshot
        QJsonObject annotationTransformSnapshot = {};     // per-call confirmed source and new-session snapshot
    };

    using BatchGate = std::function<QString(int index, const QString& toolName, const QJsonObject& params)>;

    // 批量执行（支持 macro undo）；callId 贯穿结果，遇错停止后未执行项补
    // SKIPPED_DUE_TO_PREVIOUS_ERROR 结果，参数校验失败同样保留全部 callId
    QJsonObject executeTools(const QList<ToolInvocation>& tools, const QString& macroName);

    // 兼容入口：无 callId 的旧调用方（callId 自动生成 "auto_N"）
    QJsonObject executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName);
    QJsonObject executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName,
                             const BatchGate& gate);
    static bool resultSucceeded(const QJsonObject& result);
    static QString resultFailureCode(const QJsonObject& result);
    QStringList supportedTools() const;
    bool toolUndoable(const QString& toolName) const;
    using ViewportImageProvider = std::function<QImage(QString* error)>;
    void setViewportImageProvider(ViewportImageProvider provider);
    void abortRunningTool();

    QUndoStack* undoStack() const {
        return m_undoStack;
    }
    bool undoLastAction();
    bool undoBlockedByBarrier() const;

    /**
     * @brief 用户知识库目录（appData/knowledge），不存在时自动创建。
     *
     * 用户通过 Agent 设置 → 知识库 添加的文档存放在此，
     * read_documentation 工具检索时与内置文档一视同仁。
     */
    static QString userKnowledgeDir();

    /**
     * @brief 全部知识库文档路径：内置（qrc）在前，用户目录（*.md、*.markdown、*.txt）在后。
     */
    static QStringList knowledgeDocumentPaths();
    static QJsonArray listKnowledgeDocuments();
    static QJsonObject previewKnowledgeDocument(const QString& documentId);
    static QJsonObject addKnowledgeDocument(const QString& sourcePath);
    static QJsonObject removeKnowledgeDocument(const QString& documentId);

    // MainWindow 调用：确认 GUI 当前流程已同步至共享 RunEngine 后记录快照。
    void noteRunEngineSynchronized(const Project* project);

signals:
    void toolExecuted(const QString& toolName, const QJsonObject& result);
    void toolError(const QString& toolName, const QString& error);
    void runEngineSyncRequested();
    void modulePositionSyncRequested(const QString& instanceId);
    void positionUndoExpired(const QString& instanceId, bool redo);

private:
    friend class AgentController;

    struct ToolHandler {
        std::function<QJsonObject(const QJsonObject&)> fn;
        bool mutatesProject = false;
        bool undoable = false;
    };
    struct RunWaitResult {
        bool started = false;
        bool finished = false;
        bool timedOut = false;
        bool aborted = false;
        bool pausedAtBreakpoint = false;
        bool pausedByUser = false;
        bool startFailed = false;
        QString startError;
        QString runId;
        bool reentrant = false;
        bool rejectedStart = false;
        bool ok = false;
        int elapsedMs = 0;
        int totalRuns = 0;
        int successRuns = 0;
        QString failedModule;
        QString error;
    };
    void buildHandlers();
    QJsonObject executeToolInternal(const QString& toolName, const QJsonObject& params);

    void bindConfirmedResumeSnapshot(const QString& runId, const QString& sessionId, int state,
                                     int lifecycleGeneration);
    void clearConfirmedResumeSnapshot();
    QJsonObject executeToolWithCameraTarget(
        const QString& toolName, const QJsonObject& params, const QJsonObject& cameraTargetSnapshot,
        const QJsonObject& folderDeleteTargetAuthorization = {}, const QJsonObject& variableSnapshot = {},
        const QJsonObject& dataSourceImportSnapshot = {}, const QString& callId = {},
        const QJsonObject& annotationExportSnapshot = {}, const QJsonObject& annotationTransformSnapshot = {});
    QJsonObject executeToolWithCameraTargetInternal(
        const QString& toolName, const QJsonObject& params, const QJsonObject& cameraTargetSnapshot,
        const QJsonObject& folderDeleteTargetAuthorization, const QJsonObject& variableSnapshot,
        const QJsonObject& dataSourceImportSnapshot, const QString& callId,
        const QJsonObject& annotationExportSnapshot, const QJsonObject& annotationTransformSnapshot);
    QJsonObject resumeFlowForSnapshot(const QString& runId, const QString& sessionId, int state,
                                      int lifecycleGeneration, const QJsonObject& folderDeleteTargetAuthorization = {});

    void pushUndoCommand(QUndoCommand* command);
    void establishUndoBarrier();
    QJsonObject createProject(const QJsonObject& params);
    QJsonObject addModule(const QJsonObject& params);
    QJsonObject removeModule(const QJsonObject& params);
    QJsonObject moveModule(const QJsonObject& params);
    QJsonObject setModulePosition(const QJsonObject& params);
    QJsonObject renameModule(const QJsonObject& params);
    QJsonObject setModuleEnabled(const QJsonObject& params);
    QJsonObject setBreakpoint(const QJsonObject& params);
    QJsonObject setModuleNote(const QJsonObject& params);
    QJsonObject setParam(const QJsonObject& params);
    QJsonObject setModuleParams(const QJsonObject& params);
    QJsonObject resetModuleParams(const QJsonObject& params);
    QJsonObject editModuleParams(const QJsonObject& params, bool reset, bool legacySingle);
    QJsonObject connectModules(const QJsonObject& params);
    QJsonObject disconnectModules(const QJsonObject& params);
    QJsonObject runFlow(const QJsonObject& params, const QJsonObject& folderDeleteTargetAuthorization);
    QJsonObject stepOnce(const QJsonObject& params, const QJsonObject& folderDeleteTargetAuthorization);
    QJsonObject resetStep(const QJsonObject& params);
    QJsonObject validateFlow(const QJsonObject& params);
    QJsonObject stopFlow(const QJsonObject& params);
    QJsonObject getFlowState(const QJsonObject& params);
    QJsonObject getAvailablePlugins(const QJsonObject& params);
    QJsonObject listCameras(const QJsonObject& params);
    QJsonObject getCameraDiscoveryTargets(const QJsonObject& params);
    QJsonObject discoverCameras(const QJsonObject& params);
    QJsonObject getCameraDiscoveryOperation(const QJsonObject& params);
    QJsonObject getCameraDetails(const QJsonObject& params);
    QJsonObject getCameraOperation(const QJsonObject& params);
    QJsonObject setCameraParameter(const QJsonObject& params);
    QJsonObject listCommunications(const QJsonObject& params);
    QJsonObject loadModulePlugin(const QJsonObject& params);
    QJsonObject unloadModulePlugin(const QJsonObject& params);
    QJsonObject connectCamera(const QJsonObject& params, const QJsonObject& cameraTargetSnapshot = {});
    QJsonObject disconnectCamera(const QJsonObject& params, const QJsonObject& cameraTargetSnapshot = {});
    QJsonObject saveProject(const QJsonObject& params);
    QJsonObject getModuleParamsSchema(const QJsonObject& params);
    QJsonObject getRunResults(const QJsonObject& params);
    QJsonObject getModuleOutput(const QJsonObject& params);
    QJsonObject waitForRun(const QJsonObject& params);
    QJsonObject openProject(const QJsonObject& params);
    QJsonObject pauseFlow(const QJsonObject& params);
    QJsonObject resumeFlow(const QJsonObject& params, const QJsonObject& folderDeleteTargetAuthorization);
    QJsonObject readDocumentation(const QJsonObject& params);
    QJsonObject importDataSource(const QJsonObject& params, const QJsonObject& confirmedSnapshot,
                                 const QString& callId);
    QJsonObject getAnnotationSession(const QJsonObject& params);
    QJsonObject validateWorkflow(const QJsonObject& params);
    QJsonObject getNodeRuntimeOutputs(const QJsonObject& params);
    QJsonObject getExecutionLogs(const QJsonObject& params);
    QJsonObject tuneNodeAndVerify(const QJsonObject& params);
    QJsonObject captureViewportImage(const QJsonObject& params);
    QJsonObject exportNodeOutput(const QJsonObject& params);
    bool ensureEngineLoaded(QString* error, bool forceReload = false);
    bool engineMatchesProject(const Project* project) const;
    RunWaitResult runAndWait(const QString& mode, int timeoutMs, bool forceReload,
                             const QJsonObject& authorization = {});
    // SAM 批3：Agent 工具化 SAM（共享 SamPredictor 同步预测核心）
    QJsonObject samPredict(const QJsonObject& params);
    QJsonObject samAnnotate(const QJsonObject& params);

    QUndoStack* m_undoStack;
    QString m_batchUndoMacroName;
    bool m_batchUndoMacroEligible = false;
    bool m_batchUndoMacroOpen = false;
    bool m_positionUndoExpired = false;
    bool m_moduleOrderUndoExpired = false;
    bool m_parameterUndoExpired = false;
    bool m_runtimeFieldUndoExpired = false;
    int m_latestUndoBarrierIndex = -1;
    bool m_confirmedResumeSnapshotValid = false;
    QString m_confirmedResumeRunId;
    QString m_confirmedResumeSessionId;
    int m_confirmedResumeState = 0;
    int m_confirmedResumeLifecycleGeneration = 0;
    QMap<QString, QJsonObject> m_schemaCache; // 插件加载/卸载时失效（评审 P2-7）
    QString m_stepProjectId;
    QByteArray m_stepProjectFingerprint;
    QHash<QString, ToolHandler> m_handlers;
    std::unique_ptr<AgentRunWaiter> m_runWaiter;
    ViewportImageProvider m_viewportImageProvider;
    bool m_engineProjectDirty = true;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_ACTOR_H
