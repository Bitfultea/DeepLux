#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/ToolSchema.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QtTest/QtTest>

using namespace DeepLux;

// 工具契约单源化（docs/agent-iteration1-design.md §3.5）：
// 断言 ToolSchema 是 readOnly/dangerous/timeoutMs 的唯一事实来源，并锁定
// 第一迭代 6 个新工具的元数据与参数契约。行为断言属于各工具的实现测试。
class TestAgentToolContract : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
    }

    void testToolNamesUnique() {
        QSet<QString> names;
        const QList<ToolDefinition> tools = ToolSchema::instance().allTools();
        QVERIFY(!tools.isEmpty());
        for (const ToolDefinition& tool : tools) {
            QVERIFY2(!tool.name.trimmed().isEmpty(), "tool name must not be empty");
            QVERIFY2(!names.contains(tool.name), qPrintable(QString("duplicate tool name: %1").arg(tool.name)));
            names.insert(tool.name);
        }
    }

    void testDescriptionsNonEmpty() {
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            QVERIFY2(tool.description.trimmed().size() >= 8,
                     qPrintable(QString("tool %1 has a placeholder description").arg(tool.name)));
        }
    }

    void testRequiredParamsExist() {
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            const QJsonObject parameters = tool.toOpenAIFunction()["function"].toObject()["parameters"].toObject();
            const QJsonObject properties = parameters["properties"].toObject();
            const QJsonArray required = parameters["required"].toArray();
            for (const QJsonValue& name : required) {
                QVERIFY2(
                    properties.contains(name.toString()),
                    qPrintable(
                        QString("required param %1 of %2 is missing from properties").arg(name.toString(), tool.name)));
            }
            // 双向：schema 标记 required 的参数必须出现在 OpenAI required 列表里
            for (const ToolParam& param : tool.params) {
                if (!param.required)
                    continue;
                QVERIFY2(required.contains(QJsonValue(param.name)),
                         qPrintable(QString("param %1 of %2 is required but absent from the required list")
                                        .arg(param.name, tool.name)));
            }
        }
    }

    void testParamTypesValid() {
        const QSet<QString> validTypes = {"string", "integer", "number", "boolean", "array", "object", "enum"};
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            for (const ToolParam& param : tool.params) {
                QVERIFY2(!param.name.isEmpty(), qPrintable(QString("unnamed param in %1").arg(tool.name)));
                QVERIFY2(
                    validTypes.contains(param.type),
                    qPrintable(QString("param %1 of %2 has invalid type %3").arg(param.name, tool.name, param.type)));
                QJsonObject json = param.toJson();
                if (!param.enumValues.isEmpty()) {
                    QCOMPARE(param.type, QStringLiteral("enum"));
                    QCOMPARE(json["type"].toString(), QStringLiteral("string")); // "enum" 不是合法 JSON Schema 类型
                    QVERIFY(json.contains("enum"));
                    QCOMPARE(json["enum"].toArray().size(), param.enumValues.size());
                    // 默认值必须是枚举成员
                    if (!param.defaultValue.isUndefined() && !param.defaultValue.isNull()) {
                        QVERIFY2(
                            param.enumValues.contains(param.defaultValue.toString()),
                            qPrintable(
                                QString("default of %1.%2 is not a member of its enum").arg(tool.name, param.name)));
                    }
                } else {
                    QVERIFY(!json["type"].toString().isEmpty());
                }
            }
        }
    }

    void testDangerousImpliesNotReadOnly() {
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            QVERIFY2(!(tool.dangerous && tool.readOnly),
                     qPrintable(QString("tool %1 cannot be both dangerous and readOnly").arg(tool.name)));
        }
    }

    void testTimeoutMetadataNonNegative() {
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            QVERIFY2(tool.timeoutMs >= 0, qPrintable(QString("tool %1 has negative timeoutMs").arg(tool.name)));
            const int effective = ToolSchema::effectiveTimeoutMs(tool);
            if (tool.timeoutMs > 0) {
                QCOMPARE(effective, tool.timeoutMs);
            } else {
                QCOMPARE(effective, ToolSchema::kDefaultToolTimeoutMs);
            }
        }
    }

    void testMetadataSnapshot() {
        // 设计 §3.2.3 的 25 行快照：readOnly / dangerous
        struct Expect {
            const char* name;
            bool readOnly;
            bool dangerous;
        };
        const QList<Expect> expected = {
            {"create_project", false, false},
            {"add_module", false, false},
            {"remove_module", false, true},
            {"set_param", false, false},
            {"connect_modules", false, false},
            {"disconnect_modules", false, false},
            {"run_flow", false, false},
            {"stop_flow", false, false},
            {"get_flow_state", true, false},
            {"get_available_plugins", true, false},
            {"save_project", false, true},
            {"get_module_params_schema", true, false},
            {"get_run_results", true, false},
            {"open_project", false, false},
            {"pause_flow", false, false},
            {"resume_flow", false, false},
            {"read_documentation", true, false},
            {"sam_predict", false, false},
            {"sam_annotate", false, true},
            {"validate_workflow", true, false},
            {"get_node_runtime_outputs", true, false},
            {"get_execution_logs", true, false},
            {"tune_node_and_verify", false, false},
            {"capture_viewport_image", true, false},
            {"export_node_output", true, false},
        };

        const QList<ToolDefinition> tools = ToolSchema::instance().allTools();
        QCOMPARE(tools.size(), expected.size());
        for (const Expect& e : expected) {
            const ToolDefinition tool = ToolSchema::instance().findTool(QString::fromLatin1(e.name));
            QVERIFY2(!tool.name.isEmpty(), qPrintable(QString("missing tool: %1").arg(QString::fromLatin1(e.name))));
            QCOMPARE(tool.readOnly, e.readOnly);
            QCOMPARE(tool.dangerous, e.dangerous);
        }
    }

    void testToJsonSchemaExposesMetadata() {
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            const QJsonObject schema = tool.toJsonSchema();
            QVERIFY(schema.contains("readOnly"));
            QVERIFY(schema.contains("dangerous"));
            QVERIFY(schema.contains("timeoutMs"));
            QCOMPARE(schema["readOnly"].toBool(), tool.readOnly);
            QCOMPARE(schema["dangerous"].toBool(), tool.dangerous);
            QCOMPARE(schema["timeoutMs"].toInt(), tool.timeoutMs);
        }
    }

    void testNewToolsRegistered() {
        struct Expect {
            const char* name;
            int timeoutMs;
        };
        const QList<Expect> expected = {
            {"validate_workflow", 30000},     {"get_node_runtime_outputs", 0}, {"get_execution_logs", 0},
            {"tune_node_and_verify", 300000}, {"capture_viewport_image", 0},   {"export_node_output", 0},
        };
        for (const Expect& e : expected) {
            const QString name = QString::fromLatin1(e.name);
            QVERIFY2(ToolSchema::instance().hasTool(name), qPrintable(QString("tool not registered: %1").arg(name)));
            QCOMPARE(ToolSchema::instance().findTool(name).timeoutMs, e.timeoutMs);
        }
    }

    void testRunFlowWaitContract() {
        const ToolDefinition runFlow = ToolSchema::instance().findTool(QStringLiteral("run_flow"));
        QCOMPARE(runFlow.timeoutMs, 60000);
        QVERIFY(!runFlow.readOnly);
        QVERIFY(!runFlow.dangerous);

        const ToolParam wait = paramOf(runFlow, QStringLiteral("wait"));
        QVERIFY2(!wait.name.isEmpty(), "run_flow must expose 'wait'");
        QCOMPARE(wait.type, QStringLiteral("boolean"));
        QVERIFY(!wait.required);
        QVERIFY(wait.defaultValue.isBool());
        QVERIFY(wait.defaultValue.toBool());

        const ToolParam timeoutMs = paramOf(runFlow, QStringLiteral("timeoutMs"));
        QVERIFY2(!timeoutMs.name.isEmpty(), "run_flow must expose 'timeoutMs'");
        QCOMPARE(timeoutMs.type, QStringLiteral("integer"));
        QCOMPARE(timeoutMs.defaultValue.toInt(), 60000);
    }

    void testNewToolsParams() {
        // 无参数工具
        QCOMPARE(ToolSchema::instance().findTool(QStringLiteral("validate_workflow")).params.size(), 0);
        QCOMPARE(ToolSchema::instance().findTool(QStringLiteral("capture_viewport_image")).params.size(), 0);

        // get_node_runtime_outputs
        const ToolDefinition nodeOutputs = ToolSchema::instance().findTool(QStringLiteral("get_node_runtime_outputs"));
        QCOMPARE(nodeOutputs.params.size(), 1);
        const ToolParam nodeName = paramOf(nodeOutputs, QStringLiteral("nodeName"));
        QCOMPARE(nodeName.type, QStringLiteral("string"));
        QVERIFY(nodeName.required);

        // get_execution_logs
        const ToolDefinition logs = ToolSchema::instance().findTool(QStringLiteral("get_execution_logs"));
        QCOMPARE(logs.params.size(), 4);
        const ToolParam category = paramOf(logs, QStringLiteral("category"));
        QCOMPARE(category.type, QStringLiteral("string"));
        QVERIFY(!category.required);
        const ToolParam level = paramOf(logs, QStringLiteral("level"));
        QVERIFY(!level.required);
        QCOMPARE(level.enumValues, QStringList({"debug", "info", "warning", "error", "success"}));
        QCOMPARE(level.defaultValue.toString(), QStringLiteral("debug"));
        const ToolParam keyword = paramOf(logs, QStringLiteral("keyword"));
        QCOMPARE(keyword.type, QStringLiteral("string"));
        QVERIFY(!keyword.required);
        const ToolParam limit = paramOf(logs, QStringLiteral("limit"));
        QCOMPARE(limit.type, QStringLiteral("integer"));
        QVERIFY(!limit.required);
        QCOMPARE(limit.defaultValue.toInt(), 50);

        // tune_node_and_verify
        const ToolDefinition tune = ToolSchema::instance().findTool(QStringLiteral("tune_node_and_verify"));
        QCOMPARE(tune.params.size(), 4);
        QVERIFY(paramOf(tune, QStringLiteral("nodeName")).required);
        const ToolParam parameters = paramOf(tune, QStringLiteral("parameters"));
        QCOMPARE(parameters.type, QStringLiteral("object"));
        QVERIFY(parameters.required);
        const ToolParam runMode = paramOf(tune, QStringLiteral("runMode"));
        QVERIFY(!runMode.required);
        QCOMPARE(runMode.enumValues, QStringList({"flow", "node"}));
        QCOMPARE(runMode.defaultValue.toString(), QStringLiteral("flow"));
        const ToolParam tuneTimeout = paramOf(tune, QStringLiteral("timeoutMs"));
        QCOMPARE(tuneTimeout.type, QStringLiteral("integer"));
        QCOMPARE(tuneTimeout.defaultValue.toInt(), 60000);

        // export_node_output
        const ToolDefinition exportOutput = ToolSchema::instance().findTool(QStringLiteral("export_node_output"));
        QCOMPARE(exportOutput.params.size(), 2);
        QVERIFY(paramOf(exportOutput, QStringLiteral("nodeName")).required);
        const ToolParam format = paramOf(exportOutput, QStringLiteral("format"));
        QVERIFY(!format.required);
        QCOMPARE(format.enumValues, QStringList({"png", "jpg"}));
        QCOMPARE(format.defaultValue.toString(), QStringLiteral("png"));
    }

    void testPermissionMatrix() {
        using Level = AgentController::PermissionLevel;
        // 设计 §3.2.2：Observer 只放行只读；Advisor 写/危险均待确认；Autopilot 仅危险待确认
        struct Expect {
            const char* name;
            bool observerConfirms;
            bool advisorConfirms;
            bool autopilotConfirms;
        };
        const QList<Expect> expected = {
            {"get_flow_state", false, false, false},
            {"get_available_plugins", false, false, false},
            {"read_documentation", false, false, false},
            {"validate_workflow", false, false, false},
            {"get_node_runtime_outputs", false, false, false},
            {"get_execution_logs", false, false, false},
            {"capture_viewport_image", false, false, false},
            {"export_node_output", false, false, false},
            {"add_module", true, true, false},
            {"set_param", true, true, false},
            {"run_flow", true, true, false},
            {"sam_predict", true, true, false},
            {"tune_node_and_verify", true, true, false},
            {"remove_module", true, true, true},
            {"save_project", true, true, true},
            {"sam_annotate", true, true, true},
        };
        for (const Expect& e : expected) {
            const QString name = QString::fromLatin1(e.name);
            QCOMPARE(AgentController::toolRequiresConfirmation(Level::Observer, name), e.observerConfirms);
            QCOMPARE(AgentController::toolRequiresConfirmation(Level::Advisor, name), e.advisorConfirms);
            QCOMPARE(AgentController::toolRequiresConfirmation(Level::Autopilot, name), e.autopilotConfirms);
        }

        // 未注册工具 fail closed：Observer 拒绝、Advisor/Autopilot 待确认
        QVERIFY(AgentController::toolRequiresConfirmation(Level::Observer, QStringLiteral("not_a_tool")));
        QVERIFY(AgentController::toolRequiresConfirmation(Level::Advisor, QStringLiteral("not_a_tool")));
        QVERIFY(AgentController::toolRequiresConfirmation(Level::Autopilot, QStringLiteral("not_a_tool")) == false);
    }

    void testOpenAIAndClaudeFormatStable() {
        // 元数据字段不得改变既有 Function Calling / Claude Tool Use 输出形状
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            const QJsonObject openAi = tool.toOpenAIFunction();
            QCOMPARE(openAi["type"].toString(), QStringLiteral("function"));
            const QJsonObject fn = openAi["function"].toObject();
            QCOMPARE(fn["name"].toString(), tool.name);
            QCOMPARE(fn["description"].toString(), tool.description);
            const QJsonObject parameters = fn["parameters"].toObject();
            QCOMPARE(parameters["type"].toString(), QStringLiteral("object"));
            QCOMPARE(parameters["properties"].toObject().size(), tool.params.size());
            // metadata 不进入 function 输出（只由 toJsonSchema 暴露）
            QVERIFY(!fn.contains("readOnly"));
            QVERIFY(!fn.contains("dangerous"));
            QVERIFY(!fn.contains("timeoutMs"));
            // Claude Tool Use 复用同一函数格式，必须逐字段相等
            QCOMPARE(tool.toClaudeTool(), openAi);
        }
    }

    void testBulkSchemaSerializable() {
        const QJsonArray openAi = ToolSchema::instance().toOpenAIFunctions();
        const QJsonArray claude = ToolSchema::instance().toClaudeTools();
        QCOMPARE(openAi.size(), ToolSchema::instance().allTools().size());
        QCOMPARE(claude.size(), openAi.size());
    }

    void testRegistryUndoableSet() {
        // 设计 §3.4 的 undoable 目标集合（审计事实来源 = AgentActor 派发注册表）。
        // 注：undoable ⇒ mutatesProject 的另一半由 AgentActor 内部断言/引擎忙用例覆盖，
        // mutatesProject 未作为公开 API 暴露，本测试只锁定可观察的 undoable 集合。
        const QSet<QString> undoableTools = {
            QStringLiteral("add_module"),         QStringLiteral("remove_module"),
            QStringLiteral("set_param"),          QStringLiteral("connect_modules"),
            QStringLiteral("disconnect_modules"), QStringLiteral("tune_node_and_verify"),
        };
        AgentActor actor;
        for (const ToolDefinition& tool : ToolSchema::instance().allTools()) {
            const bool expected = undoableTools.contains(tool.name);
            QVERIFY2(actor.toolUndoable(tool.name) == expected,
                     qPrintable(QStringLiteral("unexpected undoable flag for %1").arg(tool.name)));
        }
        // 工程级工具不入撤销栈（D2 修正点）
        for (const QString& name :
             {QStringLiteral("create_project"), QStringLiteral("open_project"), QStringLiteral("save_project")}) {
            QVERIFY(!actor.toolUndoable(name));
        }
        QVERIFY(!actor.toolUndoable(QStringLiteral("not_a_tool")));
    }

    void testSchemaMatchesHandlers() {
        // 设计 §3.5 第 7 条：schema 注册表与 AgentActor 派发注册表必须双向一致，
        // 防"schema 有、实现无"或"实现有、schema 无"的漂移。
        AgentActor actor;
        const QStringList handlers = actor.supportedTools();
        QStringList sorted = handlers;
        sorted.sort();
        QCOMPARE(handlers, sorted); // 稳定顺序便于断言

        QSet<QString> handlerSet;
        for (const QString& name : handlers)
            handlerSet.insert(name);
        QSet<QString> schemaSet;
        for (const ToolDefinition& tool : ToolSchema::instance().allTools())
            schemaSet.insert(tool.name);

        const QStringList missingHandlers = (schemaSet - handlerSet).values();
        QVERIFY2(missingHandlers.isEmpty(),
                 qPrintable(QStringLiteral("schema tools without handler: %1").arg(missingHandlers.join(", "))));
        const QStringList orphanHandlers = (handlerSet - schemaSet).values();
        QVERIFY2(orphanHandlers.isEmpty(),
                 qPrintable(QStringLiteral("handlers without schema: %1").arg(orphanHandlers.join(", "))));
    }

private:
    static ToolParam paramOf(const ToolDefinition& tool, const QString& name) {
        for (const ToolParam& param : tool.params) {
            if (param.name == name)
                return param;
        }
        return ToolParam();
    }
};

QTEST_MAIN(TestAgentToolContract)
#include "test_agenttoolcontract.moc"
