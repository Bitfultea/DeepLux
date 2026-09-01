#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"

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

/// 阶段 5：确定性假 LLM——按固定脚本逐轮返回 tool_call，最终返回纯文本收尾。
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
        QTimer::singleShot(0, this, [this, turn]() {
            AgentResponse resp;
            resp.success = true;
            if (turn < scriptedTurns.size()) {
                resp.content = QStringLiteral("脚本轮次 %1").arg(turn + 1);
                resp.toolCalls = scriptedTurns.value(turn);
            } else {
                resp.content = finalContent;
            }
            emit responseReceived(resp);
        });
    }
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
    bool installPlugin(const QString& pluginRoot, const QString& pluginName) const;
    QTemporaryDir m_tempDir;
};

void TestAgentEndToEnd::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", m_tempDir.filePath("appdata").toLocal8Bit());

    const QString pluginRoot = QDir(m_tempDir.path()).filePath("plugins");
    QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GrabImage")), "install GrabImage");
    QVERIFY2(installPlugin(pluginRoot, QStringLiteral("FindCircle")), "install FindCircle");

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

bool TestAgentEndToEnd::installPlugin(const QString& pluginRoot, const QString& pluginName) const {
    QDir root(pluginRoot);
    if (!root.mkpath(pluginName))
        return false;

    QDir pluginDir(root.filePath(pluginName));
    const QString srcRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../src/plugins");
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
    const QString libSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + QString("/../lib/lib%1Plugin.so").arg(pluginName));
    if (metadataSrc.isEmpty() || !QFileInfo::exists(libSrc))
        return false;

    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath(QString("lib%1Plugin.so").arg(pluginName)));
    return QFile::copy(metadataSrc, pluginDir.filePath("metadata.json")) &&
           QFile::copy(libSrc, pluginDir.filePath(QString("lib%1Plugin.so").arg(pluginName)));
}

void TestAgentEndToEnd::testFakeLlmBuildsConnectsRunsAndReadsResult() {
    // 阶段 5 验收：Agent 用确定性假 LLM 完成
    // “创建 GrabImage → FindCircle → 连接 → 运行 → 读取结果”完整闭环，不访问外部模型。
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project != nullptr);

    const QString imagePath =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../tests/acceptance/data/circle_640x480.png");
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
        // 轮 5：读取运行结果
        QJsonArray{
            makeToolCall("c7", "get_run_results", QJsonObject{}),
        },
    };

    AgentController& controller = AgentController::instance();
    ILLMClient* oldClient = controller.llmClient();
    controller.setLLMClient(&fake);
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

    QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
    QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);

    controller.sendUserMessage(QStringLiteral("创建找圆流程：GrabImage 供图给 FindCircle，运行一次并读取结果"));

    // 闭环结束：5 轮工具 + 1 轮纯文本收尾 = 6 次 LLM 请求，状态回到 Idle
    QTRY_VERIFY_WITH_TIMEOUT(controller.state() == AgentController::AgentState::Idle && fake.received.size() == 6,
                             20000);
    QCOMPARE(errorSpy.count(), 0);

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

    // 4) 7 次工具调用全部真实执行（动作日志可审计）
    QCOMPARE(logSpy.count(), 7);

    // 5) get_run_results 的结果作为 tool 消息回传进对话，可读取成功统计
    const AgentConversation& lastCtx = fake.received.last();
    QVERIFY(!lastCtx.messages.isEmpty());
    const AgentMessage& lastMsg = lastCtx.messages.last();
    QCOMPARE(lastMsg.role, QString("tool"));
    const QJsonObject stats = QJsonDocument::fromJson(lastMsg.content.toUtf8()).object();
    QVERIFY2(stats.value("successRuns").toInt() >= 1,
             qPrintable(QString("get_run_results must report success, got: %1").arg(lastMsg.content)));
    QCOMPARE(stats.value("failedRuns").toInt(), 0);

    controller.setLLMClient(oldClient);
}

QTEST_MAIN(TestAgentEndToEnd)
#include "test_agente2e.moc"
