#ifndef DEEPLUX_AGENT_ACTOR_H
#define DEEPLUX_AGENT_ACTOR_H

#include "../platform/Platform.h"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QUndoStack>

namespace DeepLux {

class Project;

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

    // 批量执行（支持 macro undo）
    QJsonObject executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName);

    QUndoStack* undoStack() const {
        return m_undoStack;
    }

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
    QJsonObject createProject(const QJsonObject& params);
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

    QUndoStack* m_undoStack;
    QMap<QString, QJsonObject> m_schemaCache; // 插件加载/卸载时失效（评审 P2-7）
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_ACTOR_H
