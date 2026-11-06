#include "TestPluginRegistry.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"

#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>
#include <cmath>
#include <optional>

using namespace DeepLux;

namespace {

QJsonObject makeToolCall(const QString& id, const QString& name, const QJsonObject& args) {
    return QJsonObject{{"id", id}, {"type", "function"}, {"name", name}, {"arguments", args}};
}

/// 确定性假 LLM——只按请求上下文返回脚本化 tool_call，不观察/等待 RunEngine。
/// 不发起任何网络请求；每轮请求的完整上下文记录在 received 中供断言。
class ScriptedLLMClient : public ILLMClient {
    Q_OBJECT

public:
    explicit ScriptedLLMClient(QObject* parent = nullptr) : ILLMClient(parent) {}

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}

    QList<QJsonArray> scriptedTurns; // 每一轮的 tool_calls
    QString finalContent = QStringLiteral("已完成");
    QList<AgentConversation> received;

    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) override {
        Q_UNUSED(tools);
        received.append(ctx);
        const int turn = received.size() - 1;
        QJsonArray calls = scriptedTurns.value(turn);
        if (turn == scriptedTurns.size()) {
            // The model reads the runId from the tool result and asks the production
            // controller to wait; the fake itself never polls RunEngine or sleeps.
            for (auto it = ctx.messages.crbegin(); it != ctx.messages.crend(); ++it) {
                if (it->role != QLatin1String("tool"))
                    continue;
                const QJsonObject toolResult = QJsonDocument::fromJson(it->content.toUtf8()).object();
                const QString runId = toolResult.value(QStringLiteral("runId")).toString();
                if (!runId.isEmpty()) {
                    calls.append(makeToolCall(QStringLiteral("c7"), QStringLiteral("wait_for_run"),
                                              QJsonObject{{QStringLiteral("runId"), runId}}));
                    break;
                }
            }
        } else if (turn == scriptedTurns.size() + 1) {
            for (auto it = ctx.messages.crbegin(); it != ctx.messages.crend(); ++it) {
                if (it->role != QLatin1String("tool") || it->toolCallId != QLatin1String("c7"))
                    continue;
                const QJsonObject waitResult = QJsonDocument::fromJson(it->content.toUtf8()).object();
                calls.append(makeToolCall(
                    QStringLiteral("c8"), QStringLiteral("get_module_output"),
                    QJsonObject{{QStringLiteral("runId"), waitResult.value(QStringLiteral("runId")).toString()},
                                {QStringLiteral("instanceId"), QStringLiteral("findcircle_1")},
                                {QStringLiteral("key"), QStringLiteral("circle_radius")}}));
                break;
            }
        }

        QTimer::singleShot(0, this, [this, turn, calls, requestId = ctx.requestId]() {
            AgentResponse resp;
            resp.requestId = requestId;
            resp.success = true;
            if (turn < scriptedTurns.size()) {
                resp.content = QStringLiteral("脚本轮次 %1").arg(turn + 1);
                resp.toolCalls = calls;
            } else if (turn == scriptedTurns.size()) {
                resp.content = QStringLiteral("Waiting for the production run result.");
                resp.toolCalls = calls;
            } else if (turn == scriptedTurns.size() + 1) {
                resp.content = QStringLiteral("Read the actual measured radius.");
                resp.toolCalls = calls;
            } else {
                resp.content = finalContent;
            }
            emit responseReceived(resp);
        });
    }
};

/// 作用域守卫：无论断言提前返回还是异常，析构时都无条件恢复单例控制器原来的
/// LLM 客户端与权限级别，避免控制器继续指向已析构的栈上假客户端（悬空指针）。
class ScopedLLMClient {
public:
    ScopedLLMClient(AgentController& controller, ILLMClient* fake)
        : m_controller(controller), m_previousClient(controller.llmClient()),
          m_previousLevel(controller.permissionLevel()) {
        m_controller.setLLMClient(fake);
    }
    ~ScopedLLMClient() {
        m_controller.setLLMClient(m_previousClient);
        m_controller.setPermissionLevel(m_previousLevel);
    }
    ScopedLLMClient(const ScopedLLMClient&) = delete;
    ScopedLLMClient& operator=(const ScopedLLMClient&) = delete;

private:
    AgentController& m_controller;
    ILLMClient* m_previousClient;
    AgentController::PermissionLevel m_previousLevel;
};

} // namespace

class TestAgentEndToEnd : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void testFakeLlmBuildsConnectsRunsAndReadsResult();

private:
    bool installPlugin(const QString& pluginRoot, const QString& pluginName, const QString& libSrc) const;
    QTemporaryDir m_tempDir;
};

void TestAgentEndToEnd::initTestCase() {
    UserManager::instance().login(QStringLiteral("developer"), QString());
    QVERIFY(m_tempDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", m_tempDir.filePath("appdata").toLocal8Bit());

    const QString pluginRoot = QDir(m_tempDir.path()).filePath("plugins");
    QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(AGENTE2E_PLUGIN_GrabImage)),
             "install GrabImage");
    QVERIFY2(installPlugin(pluginRoot, QStringLiteral("FindCircle"), QStringLiteral(AGENTE2E_PLUGIN_FindCircle)),
             "install FindCircle");

    PluginManager::instance().shutdown();
    PluginManager::instance().addPluginPath(pluginRoot);
    QVERIFY2(PluginManager::instance().initialize(), "PluginManager initialize");
    QVERIFY2(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")), "load GrabImage");
    QVERIFY2(PluginManager::instance().loadPlugin(QStringLiteral("FindCircle")), "load FindCircle");

    QVERIFY2(AgentController::instance().initialize(), "AgentController initialize");
}

void TestAgentEndToEnd::cleanupTestCase() {
    AgentController::instance().shutdown();
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
}

void TestAgentEndToEnd::cleanup() {
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
    AgentController::instance().clearConversation();
}

bool TestAgentEndToEnd::installPlugin(const QString& pluginRoot, const QString& pluginName,
                                      const QString& libSrc) const {
    QDir root(pluginRoot);
    if (!root.mkpath(pluginName))
        return false;

    QDir pluginDir(root.filePath(pluginName));
    const QString srcRoot = QDir(DeepLuxTest::testSourceDir()).filePath(QStringLiteral("src/plugins"));
    const QStringList pluginDomains = {
        QStringLiteral("geometry"), QStringLiteral("image_processing"), QStringLiteral("detection"),
        QStringLiteral("logic"),    QStringLiteral("system"),           QStringLiteral("communication"),
        QStringLiteral("variable"), QStringLiteral("calibration"),      QStringLiteral("hymson3d"),
    };
    QString metadataSrc;
    for (const QString& domain : pluginDomains) {
        const QString candidate = QDir(srcRoot).filePath(QString("%1/%2/metadata.json").arg(domain, pluginName));
        if (QFileInfo::exists(candidate)) {
            metadataSrc = candidate;
            break;
        }
    }
    // 插件库路径由 CMake $<TARGET_FILE:...> 注入，不拼接库名（跨平台）
    if (metadataSrc.isEmpty() || !QFileInfo::exists(libSrc))
        return false;

    const QString destLibName = QFileInfo(libSrc).fileName();
    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath(destLibName));
    return QFile::copy(metadataSrc, pluginDir.filePath("metadata.json")) &&
           QFile::copy(libSrc, pluginDir.filePath(destLibName));
}

void TestAgentEndToEnd::testFakeLlmBuildsConnectsRunsAndReadsResult() {
    // Agent 用确定性假 LLM 完成
    // “创建 GrabImage → FindCircle → 连接 → 运行 → 读取结果”完整闭环，不访问外部模型。
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    const QString imagePath = DeepLuxTest::testDataPath(QStringLiteral("circle_640x480.png"));
    QVERIFY2(QFileInfo::exists(imagePath), qPrintable(imagePath));

    ScriptedLLMClient fake;
    fake.scriptedTurns = {
        // 轮 1：创建两个模块
        QJsonArray{
            makeToolCall("c1", "add_module", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "grab_1"}}),
            makeToolCall("c2", "add_module", QJsonObject{{"plugin", "FindCircle"}, {"instanceName", "findcircle_1"}}),
        },
        // 轮 2：配置 GrabImage 从固定验收图像取图
        QJsonArray{
            makeToolCall("c3", "set_param",
                         QJsonObject{{"instanceId", "grab_1"}, {"key", "grabSource"}, {"value", "Path"}}),
            makeToolCall("c4", "set_param",
                         QJsonObject{{"instanceId", "grab_1"}, {"key", "filePath"}, {"value", imagePath}}),
        },
        // 轮 3：连接
        QJsonArray{
            makeToolCall("c5", "connect_modules", QJsonObject{{"fromId", "grab_1"}, {"toId", "findcircle_1"}}),
        },
        // 轮 4：运行一次
        QJsonArray{
            makeToolCall("c6", "run_flow", QJsonObject{{"mode", "once"}}),
        },
    };

    AgentController& controller = AgentController::instance();
    // 作用域守卫保证任何断言提前返回时都恢复原客户端，不留下悬空指针
    ScopedLLMClient scopedClient(controller, &fake);
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

    QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
    QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
    QSignalSpy runFinishedSpy(&RunEngine::instance(), &RunEngine::runFinished);
    QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);

    // 阶段1 §5（F13）：run_flow 属执行入口，Autopilot 也必须确认——
    // 脚本化测试模拟用户批准（本流程应恰好触发一次确认）
    QSignalSpy confirmSpy(&controller, &AgentController::toolsPendingConfirmation);
    QObject::connect(
        &controller, &AgentController::toolsPendingConfirmation, &controller,
        [&controller](const QJsonArray&) { controller.confirmPendingTools(controller.pendingConfirmationId()); });

    controller.sendUserMessage(QStringLiteral("创建找圆流程：GrabImage 供图给 FindCircle，运行一次并读取结果"));

    // 闭环结束：创建/运行、等待、读取测量输出，再纯文本收尾。
    QTRY_VERIFY_WITH_TIMEOUT(controller.state() == AgentController::AgentState::Idle && fake.received.size() == 7,
                             20000);
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(runFinishedSpy.count(), 1);
    QCOMPARE(confirmSpy.count(), 1); // 仅 run_flow 触发确认

    // 1) 流程结构由假 LLM 工具调用建立：2 模块 + 1 连接
    QVERIFY2(project->findModule("grab_1") != nullptr, "grab_1 must exist");
    QVERIFY2(project->findModule("findcircle_1") != nullptr, "findcircle_1 must exist");
    QCOMPARE(project->connections().size(), 1);
    QCOMPARE(project->connections().first().fromModuleId, QString("grab_1"));
    QCOMPARE(project->connections().first().toModuleId, QString("findcircle_1"));

    // 2) set_param 真实写入了取图参数
    const std::optional<ModuleInstance> grab = project->moduleById("grab_1");
    QVERIFY(grab.has_value());
    QCOMPARE(grab->params.value("grabSource").toString(), QString("Path"));
    QCOMPARE(grab->params.value("filePath").toString(), imagePath);

    // 3) run_flow 真实执行成功并产出检测结果（固定验收图：圆心 320,240 半径 100）
    QVERIFY2(RunEngine::instance().successRuns() >= 1, "run_flow must produce a successful run");
    QCOMPARE(RunEngine::instance().failedRuns(), 0);
    const ImageData out = RunEngine::instance().moduleOutput(QStringLiteral("findcircle_1"));
    QVERIFY2(out.hasData("circle_radius"), "FindCircle must produce circle_radius");
    QVERIFY2(std::abs(out.data("circle_radius").toDouble() - 100.0) < 3.0, "circle_radius off");
    QVERIFY2(std::abs(out.data("circle_center_x").toDouble() - 320.0) < 3.0, "circle_center_x off");
    QVERIFY2(std::abs(out.data("circle_center_y").toDouble() - 240.0) < 3.0, "circle_center_y off");

    // 4) 工具调用及 pending/终态两次 wait_for_run 审计均保留。
    QCOMPARE(logSpy.count(), 9);
    QVERIFY(logSpy.first().first().canConvert<AgentActionLogEntry>());
    QCOMPARE(logSpy.first().first().value<AgentActionLogEntry>().action, QStringLiteral("add_module"));
    QStringList waitAuditResults;
    for (const QList<QVariant>& row : logSpy) {
        const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
        if (entry.action == QLatin1String("wait_for_run"))
            waitAuditResults.append(entry.result);
    }
    QCOMPARE(waitAuditResults.size(), 2);
    QVERIFY(waitAuditResults.first().contains(QStringLiteral("pending")));
    QCOMPARE(waitAuditResults.last(), QStringLiteral("success"));

    // 5) wait_for_run 由生产控制器等待，并将最终运行统计作为 tool 消息回传模型
    const AgentConversation& lastCtx = fake.received.last();
    QVERIFY(!lastCtx.messages.isEmpty());
    const AgentMessage* waitMessage = nullptr;
    const AgentMessage* outputMessage = nullptr;
    for (const AgentMessage& message : lastCtx.messages) {
        if (message.role != QLatin1String("tool"))
            continue;
        if (message.toolCallId == QLatin1String("c7"))
            waitMessage = &message;
        else if (message.toolCallId == QLatin1String("c8"))
            outputMessage = &message;
    }
    QVERIFY(waitMessage);
    QVERIFY(outputMessage);
    const QJsonObject stats = QJsonDocument::fromJson(waitMessage->content.toUtf8()).object();
    QCOMPARE(stats.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
    QVERIFY(!stats.value(QStringLiteral("isBusy")).toBool());
    QVERIFY2(stats.value("successRuns").toInt() >= 1,
             qPrintable(QString("wait_for_run must report success, got: %1").arg(waitMessage->content)));
    QCOMPARE(stats.value("failedRuns").toInt(), 0);
    const QJsonObject measuredOutput = QJsonDocument::fromJson(outputMessage->content.toUtf8()).object();
    QCOMPARE(measuredOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(measuredOutput.value(QStringLiteral("key")).toString(), QStringLiteral("circle_radius"));
    QVERIFY(std::abs(measuredOutput.value(QStringLiteral("value")).toDouble() - 100.0) < 3.0);

    QString finalReply;
    for (const QList<QVariant>& row : responseSpy) {
        const QString reply = row.first().toString();
        if (reply.contains(QStringLiteral("get_module_output")))
            finalReply = reply;
    }
    QVERIFY(!finalReply.isEmpty());
    QVERIFY(finalReply.contains(QStringLiteral("get_module_output")));
    QVERIFY(finalReply.contains(QStringLiteral("callId=c8")));
    QVERIFY(finalReply.contains(QStringLiteral("findcircle_1")));
    QVERIFY(finalReply.contains(QStringLiteral("circle_radius")));
    const QRegularExpression valuePattern(QStringLiteral("\\\"value\\\":([0-9.eE+-]+)"));
    const QRegularExpressionMatch valueMatch = valuePattern.match(finalReply);
    QVERIFY2(valueMatch.hasMatch(), qPrintable(finalReply));
    QVERIFY(std::abs(valueMatch.captured(1).toDouble() - measuredOutput.value(QStringLiteral("value")).toDouble()) <
            1e-6);
    QVERIFY(!controller.conversationHistory().isEmpty());
    QCOMPARE(controller.conversationHistory().last().content, finalReply);
    // scopedClient 析构时自动恢复原 LLM 客户端与权限级别
}

QTEST_MAIN(TestAgentEndToEnd)
#include "test_agente2e.moc"
