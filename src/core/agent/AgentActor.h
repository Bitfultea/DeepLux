#ifndef DEEPLUX_AGENT_ACTOR_H
#define DEEPLUX_AGENT_ACTOR_H

#include "../platform/Platform.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QUndoStack>
#include <functional>
#include <memory>

namespace DeepLux {

class AgentRunWaiter;
class Project;

/**
 * @brief Agent Actor —— Tool API 执行器
 *
 * 将 LLM 返回的 tool calls 通过 QHash 注册表路由到实际的处理函数（见 buildHandlers()），
 * 工程内编辑通过 QUndoStack 包装；新建/打开工程会清空撤销历史。
 *
 * 安全约束：
 * - 只能调用预注册的 ToolSchema 白名单工具（schema 有、实现无 → tool_not_registered）
 * - 不能执行 bash / system / QProcess
 * - 等待类工具（run_flow(wait=true)、tune_node_and_verify）使用 AgentRunWaiter 的嵌套
 *   事件循环，不阻塞 GUI
 */
class DEEPLUX_API AgentActor : public QObject {
    Q_OBJECT

public:
    explicit AgentActor(QObject* parent = nullptr);
    ~AgentActor() override;

    // 执行单个 tool call（对外唯一入口：结果附带内部字段 _elapsedMs）
    QJsonObject executeTool(const QString& toolName, const QJsonObject& params);

    // 批量执行（支持 macro undo）
    QJsonObject executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName);

    QUndoStack* undoStack() const {
        return m_undoStack;
    }

    /**
     * @brief 已注册处理器的工具名集合（按名排序，稳定顺序便于断言）。
     *
     * 契约测试用：必须与 ToolSchema::allTools() 的名称集合相等。
     */
    QStringList supportedTools() const;

    /**
     * @brief 该工具是否压入 QUndoStack（审计 undoable 的唯一事实来源；未注册 → false）
     */
    bool toolUndoable(const QString& toolName) const;

    /**
     * @brief 宿主注入：视口图像提供者（MainWindow 启动时设置；未设置时失败关闭）
     *
     * 回调返回空 QImage 时可通过 error 给出可读原因；工具层仍会回退到
     * RunEngine::lastOutput()。
     */
    using ViewportImageProvider = std::function<QImage(QString* error)>;
    void setViewportImageProvider(ViewportImageProvider provider);

    /**
     * @brief 中止正在等待的运行（由 AgentController::abortCurrentTurn 调用，幂等）
     */
    void abortRunningTool();

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

signals:
    void toolExecuted(const QString& toolName, const QJsonObject& result);
    void toolError(const QString& toolName, const QString& error);

private:
    // 派发注册表条目：fn + 引擎忙门禁/审计所需元数据
    struct ToolHandler {
        std::function<QJsonObject(const QJsonObject&)> fn;
        bool mutatesProject = false; // 引擎 busy 时拒绝（对齐基线 mutatesFlow 集合 + tune）
        bool undoable = false;       // 是否压入 QUndoStack
    };

    // 等待类工具的原始结果（run_flow(wait=true) 与 tune 共用）
    struct RunWaitResult {
        bool started = false;
        bool finished = false;
        bool timedOut = false;
        bool aborted = false;
        bool pausedAtBreakpoint = false;
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

    /// 真实执行体（不含耗时字段）：executeTool 统一在返回前附带 "_elapsedMs"
    QJsonObject executeToolInternal(const QString& toolName, const QJsonObject& params);

    // --- 注册表覆盖的 25 个工具实现 ---
    // R2 测试可见性：create_project 的失败分支只能在"引擎忙"时触达，而
    // executeTool 的 mutatesProject 门禁会先返回 engine_busy；测试需直接调用
    // handler 才能断言 project_create_failed（测试构建下 public，生产仍 private）。
    DEEPLUX_TEST_VISIBILITY : QJsonObject createProject(const QJsonObject& params);
    QJsonObject addModule(const QJsonObject& params);
    QJsonObject removeModule(const QJsonObject& params);
    QJsonObject setParam(const QJsonObject& params);
    QJsonObject connectModules(const QJsonObject& params);
    QJsonObject disconnectModules(const QJsonObject& params);
    QJsonObject runFlow(const QJsonObject& params);
    QJsonObject stopFlow(const QJsonObject& params);
    QJsonObject getFlowState(const QJsonObject& params);
    QJsonObject getAvailablePlugins(const QJsonObject& params);
    QJsonObject saveProject(const QJsonObject& params);
    QJsonObject getModuleParamsSchema(const QJsonObject& params);
    QJsonObject getRunResults(const QJsonObject& params);
    QJsonObject openProject(const QJsonObject& params);
    QJsonObject pauseFlow(const QJsonObject& params);
    QJsonObject resumeFlow(const QJsonObject& params);
    QJsonObject readDocumentation(const QJsonObject& params);
    // SAM 批3：Agent 工具化 SAM（共享 SamPredictor 同步预测核心）
    QJsonObject samPredict(const QJsonObject& params);
    QJsonObject samAnnotate(const QJsonObject& params);
    // 第一迭代新增：闭环 / 观测工具
    QJsonObject validateWorkflow(const QJsonObject& params);
    QJsonObject getNodeRuntimeOutputs(const QJsonObject& params);
    QJsonObject getExecutionLogs(const QJsonObject& params);
    QJsonObject tuneNodeAndVerify(const QJsonObject& params);
    QJsonObject captureViewportImage(const QJsonObject& params);
    QJsonObject exportNodeOutput(const QJsonObject& params);

    // --- 内部辅助 ---
    /// 确保引擎持有当前工程；forceReload=false 且模块集合一致时复用（保住运行快照）
    bool ensureEngineLoaded(QString* error, bool forceReload = false);
    bool engineMatchesProject(const Project* project) const;
    /// 启动一次单次运行并等待完成（run_flow(wait=true) 与 tune 的唯一等待入口）
    RunWaitResult runAndWait(const QString& mode, int timeoutMs, bool forceReload);

    QUndoStack* m_undoStack;
    QMap<QString, QJsonObject> m_schemaCache; // 插件加载/卸载时失效（评审 P2-7）
    QHash<QString, ToolHandler> m_handlers;
    std::unique_ptr<AgentRunWaiter> m_runWaiter;
    ViewportImageProvider m_viewportImageProvider;
    bool m_engineProjectDirty = true; // 工程被 Agent 修改后，下次需要重载引擎
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_ACTOR_H
