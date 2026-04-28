#ifndef DEEPLUX_TOOL_SCHEMA_H
#define DEEPLUX_TOOL_SCHEMA_H

#include <QString>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>

namespace DeepLux {

/**
 * @brief Tool 参数定义
 */
struct ToolParam
{
    QString name;
    QString type;           // "string", "integer", "number", "boolean", "enum", "array", "object"
    QString description;
    bool required = false;
    QJsonValue defaultValue;
    QStringList enumValues; // for "enum" type

    ToolParam() = default;
    ToolParam(const QString& n, const QString& t, const QString& desc,
              bool req = false, const QJsonValue& def = QJsonValue(),
              const QStringList& enums = QStringList())
        : name(n), type(t), description(desc), required(req), defaultValue(def), enumValues(enums) {}

    QJsonObject toJson() const;
};

/**
 * @brief Tool 定义
 *
 * C++ 强类型定义，运行时生成 OpenAI Function Calling / Claude Tool Use 格式的 JSON。
 */
struct ToolDefinition
{
    QString name;
    QString description;
    QList<ToolParam> params;
    bool dangerous = false; // 是否标记为危险操作（如 delete, save）

    QJsonObject toOpenAIFunction() const;
    QJsonObject toClaudeTool() const;
    QJsonObject toJsonSchema() const;
};

/**
 * @brief Tool Schema 注册表
 *
 * 所有可用工具在编译期注册，运行时动态生成 LLM API 格式。
 */
class ToolSchema
{
public:
    static ToolSchema& instance();

    void registerTool(const ToolDefinition& tool);
    QList<ToolDefinition> allTools() const;
    ToolDefinition findTool(const QString& name) const;
    bool hasTool(const QString& name) const;

    QJsonArray toOpenAIFunctions() const;
    QJsonArray toClaudeTools() const;

    // 注册 DeepLux 内置工具
    void registerDefaultTools();

private:
    ToolSchema() = default;
    QList<ToolDefinition> m_tools;
};

} // namespace DeepLux

#endif // DEEPLUX_TOOL_SCHEMA_H
