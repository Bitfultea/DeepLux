#ifndef DEEPLUX_TOOL_SCHEMA_H
#define DEEPLUX_TOOL_SCHEMA_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>

namespace DeepLux {

/**
 * @brief Tool 参数定义
 */
struct ToolParam {
    QString name;
    QString type; // "string", "integer", "number", "boolean", "enum", "array", "object"
    QString description;
    bool required = false;
    QJsonValue defaultValue;
    QStringList enumValues; // for "enum" type

    ToolParam() = default;
    ToolParam(const QString& n, const QString& t, const QString& desc, bool req = false,
              const QJsonValue& def = QJsonValue(), const QStringList& enums = QStringList())
        : name(n), type(t), description(desc), required(req), defaultValue(def), enumValues(enums) {}

    QJsonObject toJson() const;
};

/**
 * @brief Tool 定义
 *
 * C++ 强类型定义，运行时生成 OpenAI Function Calling / Claude Tool Use 格式的 JSON。
 *
 * 字段顺序约束（C++17 位置聚合初始化）：既有 `registerTool({..., true})` 注册点依赖
 * `dangerous` 位于第 4 位。**不得**把 `readOnly` 插到 `dangerous` 之前，否则
 * remove_module / save_project / sam_annotate 会被静默降级为只读
 * （详见 docs/agent-iteration1-design.md §3.1）。新增工具一律用下方工厂函数注册。
 */
struct ToolDefinition {
    QString name;
    QString description;
    QList<ToolParam> params;
    bool dangerous = false; // 第 4 位：破坏性/写外部资源/不可撤销（保持既有位置语义）
    bool readOnly = false;  // 只观察：不改变工程状态、不写外部持久资源
    int timeoutMs = 0;      // 单次调用允许的最长等待；0 表示使用 ToolSchema::kDefaultToolTimeoutMs

    QJsonObject toOpenAIFunction() const;
    QJsonObject toClaudeTool() const;
    QJsonObject toJsonSchema() const;
};

/**
 * @brief 工具元数据工厂（契约单源化的唯一注册入口）
 *
 * 不变式：dangerous ⇒ !readOnly；三个工厂在实现里 Q_ASSERT 该不变式。
 * 既有 19 个工具保留位置聚合写法，新增工具必须走工厂，避免字段顺序漂移。
 */
ToolDefinition makeReadOnlyTool(const QString& name, const QString& description, const QList<ToolParam>& params = {},
                                int timeoutMs = 0);
ToolDefinition makeWriteTool(const QString& name, const QString& description, const QList<ToolParam>& params = {},
                             int timeoutMs = 0);
ToolDefinition makeDangerousTool(const QString& name, const QString& description, const QList<ToolParam>& params = {},
                                 int timeoutMs = 0);

/**
 * @brief Tool Schema 注册表
 *
 * 所有可用工具在编译期注册，运行时动态生成 LLM API 格式。
 */
class ToolSchema {
public:
    /// 未显式声明 timeoutMs 的工具使用的等待上限（30s）
    static constexpr int kDefaultToolTimeoutMs = 30000;

    static ToolSchema& instance();

    void registerTool(const ToolDefinition& tool);
    QList<ToolDefinition> allTools() const;
    ToolDefinition findTool(const QString& name) const;
    bool hasTool(const QString& name) const;

    QJsonArray toOpenAIFunctions() const;
    QJsonArray toClaudeTools() const;

    /// timeoutMs > 0 ? timeoutMs : kDefaultToolTimeoutMs
    static int effectiveTimeoutMs(const ToolDefinition& tool);

    // 注册 DeepLux 内置工具
    void registerDefaultTools();

private:
    ToolSchema() = default;
    QList<ToolDefinition> m_tools;
};

} // namespace DeepLux

#endif // DEEPLUX_TOOL_SCHEMA_H
