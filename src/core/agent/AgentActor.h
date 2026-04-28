#ifndef DEEPLUX_AGENT_ACTOR_H
#define DEEPLUX_AGENT_ACTOR_H

#include <QObject>
#include <QJsonObject>
#include <QUndoStack>
#include <QPointer>

namespace DeepLux {

class Project;

/**
 * @brief Agent Actor —— Tool API 执行器
 *
 * 将 LLM 返回的 tool calls 路由到实际的 Manager API。
 * 所有写操作通过 QUndoStack 包装，支持撤销。
 *
 * 安全约束：
 * - 只能调用预注册的 ToolSchema 白名单工具
 * - 不能执行 bash / system / QProcess
 */
class AgentActor : public QObject
{
    Q_OBJECT

public:
    explicit AgentActor(QObject* parent = nullptr);
    ~AgentActor() override;

    // 执行单个 tool call
    QJsonObject executeTool(const QString& toolName, const QJsonObject& params);

    // 批量执行（支持 macro undo）
    QJsonObject executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName);

    QUndoStack* undoStack() const { return m_undoStack; }

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

    QUndoStack* m_undoStack;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_ACTOR_H
