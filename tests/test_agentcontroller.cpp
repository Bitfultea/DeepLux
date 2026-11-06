#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QImage>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>
#include <algorithm>
#include <atomic>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#endif

// 测试需要访问 AgentController 的私有成员（m_conversationHistory, trimHistory）
// 这是 Qt 测试中的常见做法，仅用于测试文件
#define private public
#include "core/agent/AgentController.h"
#include "core/agent/AgentObserver.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/OpenAILLMClient.h"
#undef private

#include "core/agent/AgentActor.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/agent/ToolSchema.h"
#include "core/base/ModuleBase.h"
#include "core/common/Logger.h"
#include "core/common/UserManager.h"
#include "core/common/VarModel.h"
#include "core/engine/RunEngine.h"
#include "core/manager/GlobalVarManager.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Annotation.h"
#include "core/model/Project.h"
#include "project_json_test_helpers.h"

using namespace DeepLux;

class AgentBusyProbeModule : public ModuleBase {
public:
    explicit AgentBusyProbeModule(std::atomic<int>* calls = nullptr) : m_calls(calls ? calls : &m_ownedCalls) {
        m_moduleId = QStringLiteral("com.deeplux.test.agentbusy");
        m_name = QStringLiteral("AgentBusy");
    }

    int calls() const {
        return m_calls->load(std::memory_order_acquire);
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        m_calls->fetch_add(1, std::memory_order_acq_rel);
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    std::atomic<int> m_ownedCalls{0};
    std::atomic<int>* m_calls;
};

class UndoCounterCommand : public QUndoCommand {
public:
    explicit UndoCounterCommand(int* undoCount) : m_undoCount(undoCount) {}
    void undo() override {
        ++*m_undoCount;
    }
    void redo() override {}

private:
    int* m_undoCount;
};

// ========== Mock LLM Client ==========

class MockLLMClient : public ILLMClient {
    Q_OBJECT
public:
    explicit MockLLMClient(QObject* parent = nullptr) : ILLMClient(parent) {}

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}
    bool supportsImageInput() const override {
        return imageInputSupported;
    }

    AgentConversation lastCtx;
    QList<ToolDefinition> lastTools;
    bool imageInputSupported = true;
    bool toolCallingSupported = true;
    int sendCount = 0;
    int cancelCount = 0;
    bool requestCorrelationSupported = false;

    bool supportsToolCalling() const override {
        return toolCallingSupported;
    }
    bool supportsRequestCorrelation() const override {
        return requestCorrelationSupported;
    }
    int requestCount = 0;

    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) override {
        ++sendCount;
        lastCtx = ctx;
        lastTools = tools;
        ++requestCount;
    }

    void cancelRequest() override {
        ++cancelCount;
    }

    void emitResponse(const AgentResponse& resp) {
        AgentResponse correlated = resp;
        if (correlated.requestId.isEmpty())
            correlated.requestId = lastCtx.requestId;
        emit responseReceived(correlated);
        QCoreApplication::processEvents();
    }

    void emitError(const QString& error) {
        emit errorOccurred(error);
        QCoreApplication::processEvents();
    }

    void emitRequestFailed(const QString& requestId, const QString& error) {
        emit requestFailed(requestId, error);
    }

    // 模拟 OpenAILLMClient 的最终失败链路：先分类、后 errorOccurred
    void emitClassifiedError(const LLMErrorInfo& info) {
        emit errorOccurredClassified(info);
        emit errorOccurred(info.message);
        QCoreApplication::processEvents();
    }
};

// ========== Test Suite ==========

class TestAgentController : public QObject {
    Q_OBJECT

private:
    MockLLMClient* m_mock = nullptr;
    ILLMClient* m_oldClient = nullptr;
    QTemporaryDir m_settingsDir;
    QSettings::Format m_oldSettingsFormat = QSettings::defaultFormat();

private slots:
    void testAgentRequestCancellationRejectsLateResponseAfterNewRequest() {
        AgentController& controller = AgentController::instance();
        controller.setLLMClient(nullptr);
        m_mock->requestCorrelationSupported = true;
        controller.setLLMClient(m_mock);
        controller.clearConversation();
        controller.sendUserMessage(QStringLiteral("first request"));
        const QString firstRequestId = m_mock->lastCtx.requestId;
        QVERIFY(!firstRequestId.isEmpty());
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QVERIFY(controller.cancelCurrentRequest());
        QCOMPARE(m_mock->cancelCount, 1);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);

        controller.sendUserMessage(QStringLiteral("second request"));
        const QString secondRequestId = m_mock->lastCtx.requestId;
        QVERIFY(!secondRequestId.isEmpty());
        QVERIFY(firstRequestId != secondRequestId);

        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        m_mock->emitRequestFailed(firstRequestId, QStringLiteral("stale failure"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(errorSpy.count(), 0);

        AgentResponse late;
        late.requestId = firstRequestId;
        late.success = true;
        late.content = QStringLiteral("stale response");
        m_mock->emitResponse(late);
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(controller.conversationHistorySize(), 2);

        AgentResponse current;
        current.requestId = secondRequestId;
        current.success = true;
        current.content = QStringLiteral("current response");
        m_mock->emitResponse(current);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(controller.conversationHistorySize(), 3);
        QCOMPARE(controller.m_conversationHistory.last().content, QStringLiteral("current response"));
    }

    void testAgentRequestCancellationMarksPendingWaitCancelled() {
        AgentController& controller = AgentController::instance();
        controller.clearConversation();
        AgentMessage toolResult;
        toolResult.role = QStringLiteral("tool");
        toolResult.toolCallId = QStringLiteral("cancel_wait_call");
        toolResult.content = QStringLiteral("{\"status\":\"pending\",\"runId\":\"run_keep_alive\"}");
        controller.m_conversationHistory.append(toolResult);
        controller.m_pendingQueries.append({QStringLiteral("cancel_wait_call"), QStringLiteral("wait_for_run"),
                                            QJsonObject{{QStringLiteral("runId"), QStringLiteral("run_keep_alive")}}});
        controller.transitionTo(AgentController::AgentState::Executing);
        const int oldGeneration = controller.m_waitGeneration;

        QVERIFY(controller.cancelCurrentRequest());
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.m_pendingQueries.isEmpty());
        QVERIFY(controller.m_waitGeneration > oldGeneration);
        const AgentMessage& cancelledMessage = controller.m_conversationHistory.last();
        const QJsonObject cancelled = QJsonDocument::fromJson(cancelledMessage.content.toUtf8()).object();
        QCOMPARE(cancelled.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
        QCOMPARE(cancelled.value(QStringLiteral("reason")).toString(), QStringLiteral("agent_request_cancelled"));
        QVERIFY(!RunEngine::instance().isBusy());
    }

    void testFinalReplyUsesCurrentToolEvidenceAndSuppressesUnverifiedSuccess() {
        AgentController& controller = AgentController::instance();
        controller.clearConversation();

        AgentMessage priorUser;
        priorUser.role = QStringLiteral("user");
        priorUser.content = QStringLiteral("previous measurement");
        controller.m_conversationHistory.append(priorUser);
        AgentMessage priorCall;
        priorCall.role = QStringLiteral("assistant");
        priorCall.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("old_call")},
                                               {QStringLiteral("name"), QStringLiteral("get_module_output")}});
        controller.m_conversationHistory.append(priorCall);
        AgentMessage priorResult;
        priorResult.role = QStringLiteral("tool");
        priorResult.toolCallId = QStringLiteral("old_call");
        priorResult.content = QStringLiteral("{\"status\":\"available\",\"runId\":\"old_run\","
                                             "\"instanceId\":\"old_node\",\"key\":\"radius\",\"value\":999}");
        controller.m_conversationHistory.append(priorResult);

        AgentMessage currentUser;
        currentUser.role = QStringLiteral("user");
        currentUser.content = QStringLiteral("current measurement");
        controller.m_conversationHistory.append(currentUser);
        QCOMPARE(controller.finalizeTextReply(QStringLiteral("普通问答")), QStringLiteral("普通问答"));

        AgentMessage currentCall;
        currentCall.role = QStringLiteral("assistant");
        currentCall.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("current_call")},
                                                 {QStringLiteral("name"), QStringLiteral("get_module_output")}});
        controller.m_conversationHistory.append(currentCall);
        AgentMessage currentResult;
        currentResult.role = QStringLiteral("tool");
        currentResult.toolCallId = QStringLiteral("current_call");
        currentResult.content = QStringLiteral("{\"status\":\"available\",\"runId\":\"current_run\","
                                               "\"instanceId\":\"current_node\",\"key\":\"circle_radius\","
                                               "\"value\":100}");
        controller.m_conversationHistory.append(currentResult);

        const QString supportedReply = controller.finalizeTextReply(QStringLiteral("半径测量已完成。"));
        QVERIFY(supportedReply.contains(QStringLiteral("callId=current_call")));
        QVERIFY(supportedReply.contains(QStringLiteral("current_node")));
        QVERIFY(supportedReply.contains(QStringLiteral("circle_radius")));
        QVERIFY(supportedReply.contains(QStringLiteral("100")));
        QVERIFY(!supportedReply.contains(QStringLiteral("old_run")));
        QVERIFY(!supportedReply.contains(QStringLiteral("999")));

        currentResult.content = QStringLiteral("{\"status\":\"uncertain\",\"runId\":\"current_run\","
                                               "\"sideEffectStatus\":\"uncertain\"}");
        controller.m_conversationHistory.last() = currentResult;
        const QString uncertainReply = controller.finalizeTextReply(QStringLiteral("操作已成功完成。"));
        QVERIFY(uncertainReply.startsWith(QStringLiteral("本轮工具结果未能确认成功")));
        QVERIFY(uncertainReply.contains(QStringLiteral("uncertain")));
        QVERIFY(!uncertainReply.contains(QStringLiteral("操作已成功完成")));

        controller.m_conversationHistory.removeLast(); // 当前 get_module_output call 没有配对 tool message。
        const QString missingResultReply = controller.finalizeTextReply(QStringLiteral("测量成功。"));
        QVERIFY(missingResultReply.startsWith(QStringLiteral("本轮工具结果未能确认成功")));
        QVERIFY(missingResultReply.contains(QStringLiteral("MISSING_TOOL_RESULT")));
        QVERIFY(!missingResultReply.contains(QStringLiteral("测量成功。")));
        controller.m_conversationHistory.append(currentResult);

        currentResult.content = QStringLiteral("{\"status\":\"available\",\"runId\":\"current_run\","
                                               "\"instanceId\":\"current_node\",\"key\":\"circle_radius\"}");
        controller.m_conversationHistory.last() = currentResult;
        const QString missingValueReply = controller.finalizeTextReply(QStringLiteral("测量已成功。"));
        QVERIFY(missingValueReply.startsWith(QStringLiteral("本次工具结果未能确认成功")) ||
                missingValueReply.startsWith(QStringLiteral("工具返回了运行状态")));
        QVERIFY(!missingValueReply.contains(QStringLiteral("测量已成功。")));
    }

    void testDataSourceImportHeadlessAndConfirmedForSupportedFormats() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        projects.closeProject();
        Project* project = projects.newProject();
        QVERIFY(project);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imagePath = dir.filePath(QStringLiteral("import-sample.png"));
        QImage image(3, 2, QImage::Format_ARGB32);
        image.fill(qRgba(40, 80, 120, 255));
        QVERIFY(image.save(imagePath, "PNG"));

        const QString plyPath = dir.filePath(QStringLiteral("import-sample.ply"));
        QFile ply(plyPath);
        QVERIFY(ply.open(QIODevice::WriteOnly));
        const QByteArray plyBytes("ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\n"
                                  "property float z\nend_header\n0 0 0\n1 0 0\n");
        QCOMPARE(ply.write(plyBytes), plyBytes.size());
        ply.close();

#ifdef DEEPLUX_HAS_OPENCV
        const QString tiffPath = dir.filePath(QStringLiteral("import-height.tiff"));
        cv::Mat heights(2, 2, CV_32F);
        heights.at<float>(0, 0) = 1.0f;
        heights.at<float>(0, 1) = 2.0f;
        heights.at<float>(1, 0) = 3.0f;
        heights.at<float>(1, 1) = 4.0f;
        QVERIFY(cv::imwrite(tiffPath.toStdString(), heights));
#else
        QSKIP("OpenCV is required to create the TIFF height-map fixture");
#endif

        ModuleInstance grabImage;
        grabImage.id = QStringLiteral("import-must-not-configure-grabimage");
        grabImage.moduleId = QStringLiteral("GrabImage");
        grabImage.name = QStringLiteral("Existing GrabImage");
        grabImage.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                       {QStringLiteral("filePath"), QStringLiteral("preserve-me.png")}};
        project->addModule(grabImage);
        const QJsonObject originalGrabParams = project->moduleById(grabImage.id)->params;

        QCOMPARE(controller.executeDataSourceViewAction(QStringLiteral("get_data_source_view_state"), {})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("select_flow_module"),
                                   QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("headless-instance")}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("set_workspace_panel_visibility"),
                                   QJsonObject{{QStringLiteral("panel"), QStringLiteral("tools")},
                                               {QStringLiteral("visible"), true}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("set_log_level_filter"),
                                   QJsonObject{{QStringLiteral("minimumLevel"), QStringLiteral("info")}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("select_log_panel_tab"),
                                   QJsonObject{{QStringLiteral("tab"), QStringLiteral("runtime")}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("scroll_agent_chat_to_latest"), {})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("clear_runtime_logs"), {})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("list_recent_projects"), {})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(
            controller
                .handleToolCall(QStringLiteral("import_data_source"), QJsonObject{{QStringLiteral("path"), imagePath}})
                .value(QStringLiteral("errorCode"))
                .toString(),
            QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(project->dataSources().size(), 0);
        QJsonObject nonOwnerResult;
        AgentActor* actor = controller.actor();
        std::thread nonOwner([&]() {
            nonOwnerResult = actor->executeTool(QStringLiteral("import_data_source"),
                                                QJsonObject{{QStringLiteral("path"), imagePath}});
        });
        nonOwner.join();
        QCOMPARE(nonOwnerResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QCOMPARE(project->dataSources().size(), 0);

        QJsonObject busyNonOwnerResult;
        const QJsonObject beforeBusyNonOwner = project->toJson();
        const auto threadAffinityLease = RunEngine::instance().withProjectMutationLease([&]() {
            std::thread nonOwnerDuringMaintenance([&]() {
                busyNonOwnerResult = actor->executeTool(QStringLiteral("import_data_source"),
                                                        QJsonObject{{QStringLiteral("path"), imagePath}});
            });
            nonOwnerDuringMaintenance.join();
            return true;
        });
        QCOMPARE(threadAffinityLease, RunEngine::ProjectMutationResult::Applied);
        QCOMPARE(busyNonOwnerResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QCOMPARE(project->toJson(), beforeBusyNonOwner);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QJsonObject rejected = controller.handleToolCall(QStringLiteral("import_data_source"),
                                                         QJsonObject{{QStringLiteral("path"), imagePath}});
        QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(project->dataSources().size(), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        auto approveImport = [&](const QString& path) {
            const QJsonObject pending = controller.handleToolCall(QStringLiteral("import_data_source"),
                                                                  QJsonObject{{QStringLiteral("path"), path}});
            if (pending.value(QStringLiteral("status")) != QLatin1String("pending_confirmation"))
                return QJsonObject{{QStringLiteral("unexpected"), pending}};
            const QString confirmationId = pending.value(QStringLiteral("confirmation"))
                                               .toObject()
                                               .value(QStringLiteral("confirmationId"))
                                               .toString();
            if (!controller.confirmPendingTools(confirmationId))
                return QJsonObject{{QStringLiteral("unexpected"), QStringLiteral("confirmation failed")}};
            if (toolSpy.isEmpty())
                return QJsonObject{{QStringLiteral("unexpected"), QStringLiteral("tool result missing")}};
            const QJsonObject result = toolSpy.takeLast().at(1).toJsonObject();
            return result;
        };

        const QByteArray originalImageBytes = [&]() {
            QFile source(imagePath);
            if (!source.open(QIODevice::ReadOnly))
                return QByteArray();
            return source.readAll();
        }();
        QVERIFY(!originalImageBytes.isEmpty());

        QJsonObject importedImage = approveImport(imagePath);
        QCOMPARE(importedImage.value(QStringLiteral("status")).toString(), QStringLiteral("imported"));
        QVERIFY(!importedImage.contains(QStringLiteral("displayed")));
        QCOMPARE(importedImage.value(QStringLiteral("type")).toString(), QStringLiteral("image"));
        QJsonObject importedPly = approveImport(plyPath);
        QCOMPARE(importedPly.value(QStringLiteral("status")).toString(), QStringLiteral("imported"));
        QCOMPARE(importedPly.value(QStringLiteral("type")).toString(), QStringLiteral("pointcloud"));
        QJsonObject importedTiff = approveImport(tiffPath);
        QCOMPARE(importedTiff.value(QStringLiteral("status")).toString(), QStringLiteral("imported"));
        QCOMPARE(importedTiff.value(QStringLiteral("type")).toString(), QStringLiteral("pointcloud"));
        QCOMPARE(project->dataSources().size(), 3);
        QCOMPARE(project->dataSourceCollectionGeneration(), 3u);
        QVERIFY(project->isModified());
        const auto importedPlySource =
            project->findDataSource(importedPly.value(QStringLiteral("dataSourceId")).toString());
        QVERIFY(importedPlySource.has_value());
        QCOMPARE(importedPlySource->metadata.value(QStringLiteral("pointCount")).toLongLong(), 2);
        const auto importedTiffSource =
            project->findDataSource(importedTiff.value(QStringLiteral("dataSourceId")).toString());
        QVERIFY(importedTiffSource.has_value());
        QCOMPARE(importedTiffSource->metadata.value(QStringLiteral("pointCount")).toLongLong(), 4);
        QCOMPARE(project->moduleById(grabImage.id)->params, originalGrabParams);
        QCOMPARE(controller.executeDataSourceViewAction(QStringLiteral("get_data_source_view_state"), {})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("REQUIRES_GUI"));

        const quint64 generationBeforeRepeat = project->dataSourceCollectionGeneration();
        const int dataSourceSignalCount = auditSpy.count();
        const QJsonObject repeatedImage = approveImport(imagePath);
        QCOMPARE(repeatedImage.value(QStringLiteral("status")).toString(), QStringLiteral("already_imported"));
        QCOMPARE(project->dataSourceCollectionGeneration(), generationBeforeRepeat);
        QCOMPARE(project->dataSources().size(), 3);
        QVERIFY(auditSpy.count() > dataSourceSignalCount);
        const AgentActionLogEntry audit = auditSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(audit.action, QStringLiteral("import_data_source"));
        QVERIFY(!audit.undoable);

        const QString secondImagePath = dir.filePath(QStringLiteral("batch-second.png"));
        QImage secondImage(2, 2, QImage::Format_RGB32);
        secondImage.fill(qRgb(10, 20, 30));
        QVERIFY(secondImage.save(secondImagePath, "PNG"));
        const QString thirdImagePath = dir.filePath(QStringLiteral("batch-third.png"));
        QImage thirdImage(1, 3, QImage::Format_RGB32);
        thirdImage.fill(qRgb(90, 40, 15));
        QVERIFY(thirdImage.save(thirdImagePath, "PNG"));

        AgentResponse batchResponse;
        batchResponse.success = true;
        for (const auto& item : {qMakePair(QStringLiteral("batch_import_a"), secondImagePath),
                                 qMakePair(QStringLiteral("batch_import_b"), thirdImagePath)}) {
            QJsonObject call{{QStringLiteral("id"), item.first},
                             {QStringLiteral("type"), QStringLiteral("function")},
                             {QStringLiteral("name"), QStringLiteral("import_data_source")},
                             {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("path"), item.second}}}};
            batchResponse.toolCalls.append(call);
        }
        controller.sendUserMessage(QStringLiteral("Import two data sources"));
        m_mock->emitResponse(batchResponse);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        const QJsonObject confirmation = controller.pendingConfirmationPayload();
        const QString summary =
            confirmation.value(QStringLiteral("context")).toObject().value(QStringLiteral("summary")).toString();
        QVERIFY(summary.contains(QFileInfo(secondImagePath).canonicalFilePath()));
        QVERIFY(summary.contains(QFileInfo(thirdImagePath).canonicalFilePath()));
        const int batchToolCountBefore = toolSpy.count();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(toolSpy.count(), batchToolCountBefore + 2);
        QCOMPARE(toolSpy.at(batchToolCountBefore).at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("imported"));
        QCOMPARE(toolSpy.at(batchToolCountBefore + 1).at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("imported"));
        QCOMPARE(project->dataSources().size(), 5);
        QCOMPARE(project->dataSourceCollectionGeneration(), 5u);

        AgentResponse doneResponse;
        doneResponse.success = true;
        doneResponse.content = QStringLiteral("The first batch completed.");
        m_mock->emitResponse(doneResponse);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);

        const QString partialSuccessPath = dir.filePath(QStringLiteral("partial-success.png"));
        QVERIFY(secondImage.save(partialSuccessPath, "PNG"));
        const QString partialFailurePath = dir.filePath(QStringLiteral("partial-corrupt.png"));
        QFile partialFailure(partialFailurePath);
        QVERIFY(partialFailure.open(QIODevice::WriteOnly));
        const QByteArray corruptPng = QByteArrayLiteral("stable corrupt image payload");
        QCOMPARE(partialFailure.write(corruptPng), static_cast<qint64>(corruptPng.size()));
        partialFailure.close();
        const QString skippedPath = dir.filePath(QStringLiteral("must-be-skipped.png"));
        QVERIFY(thirdImage.save(skippedPath, "PNG"));

        QJsonObject stateAfterFirstImport;
        quint64 generationAfterFirstImport = 0;
        bool dirtyAfterFirstImport = false;
        QObject signalContext;
        connect(project, &Project::dataSourceAdded, &signalContext, [&](const DataSource&) {
            stateAfterFirstImport = project->toJson();
            generationAfterFirstImport = project->dataSourceCollectionGeneration();
            dirtyAfterFirstImport = project->isModified();
        });
        AgentResponse partialFailureResponse;
        partialFailureResponse.success = true;
        int partialCall = 0;
        for (const auto& item : {qMakePair(QStringLiteral("partial_import_ok"), partialSuccessPath),
                                 qMakePair(QStringLiteral("partial_import_bad"), partialFailurePath),
                                 qMakePair(QStringLiteral("partial_import_skipped"), skippedPath)}) {
            partialFailureResponse.toolCalls.append(
                QJsonObject{{QStringLiteral("id"), item.first},
                            {QStringLiteral("type"), QStringLiteral("function")},
                            {QStringLiteral("name"), QStringLiteral("import_data_source")},
                            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("path"), item.second}}}});
            ++partialCall;
        }
        QCOMPARE(partialCall, 3);
        controller.sendUserMessage(QStringLiteral("Import a valid file then stop at the corrupt one"));
        m_mock->emitResponse(partialFailureResponse);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        const int partialToolCountBefore = toolSpy.count();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(toolSpy.count(), partialToolCountBefore + 2);
        const QList<AgentMessage> history = controller.m_conversationHistory;
        auto toolResultFor = [&history](const QString& id) {
            for (const AgentMessage& message : history) {
                if (message.role == QLatin1String("tool") && message.toolCallId == id)
                    return QJsonDocument::fromJson(message.content.toUtf8()).object();
            }
            return QJsonObject();
        };
        QCOMPARE(toolResultFor(QStringLiteral("partial_import_ok")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("imported"));
        QCOMPARE(toolResultFor(QStringLiteral("partial_import_bad")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INVALID_IMAGE"));
        QCOMPARE(toolResultFor(QStringLiteral("partial_import_skipped")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(project->dataSources().size(), 6);
        QCOMPARE(project->dataSourceCollectionGeneration(), generationAfterFirstImport);
        QCOMPARE(project->toJson(), stateAfterFirstImport);
        QCOMPARE(project->isModified(), dirtyAfterFirstImport);

        const QString unrelatedFirstPath = dir.filePath(QStringLiteral("unrelated-change-first.png"));
        const QString unrelatedSecondPath = dir.filePath(QStringLiteral("unrelated-change-second.png"));
        const QString unrelatedThirdPath = dir.filePath(QStringLiteral("unrelated-change-third.png"));
        QVERIFY(secondImage.save(unrelatedFirstPath, "PNG"));
        QVERIFY(thirdImage.save(unrelatedSecondPath, "PNG"));
        QVERIFY(image.save(unrelatedThirdPath, "PNG"));
        QJsonObject stateAfterFirstAndForeignEdit;
        quint64 generationAfterFirstAndForeignEdit = 0;
        bool dirtyAfterFirstAndForeignEdit = false;
        QObject foreignEditContext;
        QObject postToolContext;
        connect(project, &Project::dataSourceAdded, &foreignEditContext,
                [project, &unrelatedFirstPath](const DataSource& source) {
                    if (source.filePath == QFileInfo(unrelatedFirstPath).canonicalFilePath())
                        project->setName(QStringLiteral("changed by unrelated synchronous listener"));
                });
        connect(controller.actor(), &AgentActor::toolExecuted, &postToolContext,
                [project, &unrelatedFirstPath, &stateAfterFirstAndForeignEdit, &generationAfterFirstAndForeignEdit,
                 &dirtyAfterFirstAndForeignEdit](const QString& tool, const QJsonObject& result) {
                    if (tool != QLatin1String("import_data_source") ||
                        result.value(QStringLiteral("status")) != QLatin1String("imported"))
                        return;
                    const auto source =
                        project->findDataSource(result.value(QStringLiteral("dataSourceId")).toString());
                    if (!source || source->filePath != QFileInfo(unrelatedFirstPath).canonicalFilePath())
                        return;
                    stateAfterFirstAndForeignEdit = project->toJson();
                    generationAfterFirstAndForeignEdit = project->dataSourceCollectionGeneration();
                    dirtyAfterFirstAndForeignEdit = project->isModified();
                });

        AgentResponse foreignEditBatch;
        foreignEditBatch.success = true;
        for (const auto& item : {qMakePair(QStringLiteral("foreign_edit_first"), unrelatedFirstPath),
                                 qMakePair(QStringLiteral("foreign_edit_second"), unrelatedSecondPath),
                                 qMakePair(QStringLiteral("foreign_edit_third"), unrelatedThirdPath)}) {
            foreignEditBatch.toolCalls.append(
                QJsonObject{{QStringLiteral("id"), item.first},
                            {QStringLiteral("type"), QStringLiteral("function")},
                            {QStringLiteral("name"), QStringLiteral("import_data_source")},
                            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("path"), item.second}}}});
        }
        const int foreignBatchToolCountBefore = toolSpy.count();
        controller.sendUserMessage(QStringLiteral("Check that an unrelated project edit expires later imports"));
        m_mock->emitResponse(foreignEditBatch);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(toolSpy.count(), foreignBatchToolCountBefore + 2);
        QVERIFY(!stateAfterFirstAndForeignEdit.isEmpty());
        const QList<AgentMessage> foreignHistory = controller.m_conversationHistory;
        auto foreignToolResultFor = [&foreignHistory](const QString& id) {
            for (const AgentMessage& message : foreignHistory) {
                if (message.role == QLatin1String("tool") && message.toolCallId == id)
                    return QJsonDocument::fromJson(message.content.toUtf8()).object();
            }
            return QJsonObject();
        };
        QCOMPARE(foreignToolResultFor(QStringLiteral("foreign_edit_first")).value(QStringLiteral("status")).toString(),
                 QStringLiteral("imported"));
        QCOMPARE(
            foreignToolResultFor(QStringLiteral("foreign_edit_second")).value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("CONFIRMATION_EXPIRED"));
        QCOMPARE(
            foreignToolResultFor(QStringLiteral("foreign_edit_third")).value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(project->dataSources().size(), 7);
        QCOMPARE(project->dataSourceCollectionGeneration(), generationAfterFirstAndForeignEdit);
        QCOMPARE(project->toJson(), stateAfterFirstAndForeignEdit);
        QCOMPARE(project->isModified(), dirtyAfterFirstAndForeignEdit);

        QFile imageAfterImport(imagePath);
        QVERIFY(imageAfterImport.open(QIODevice::ReadOnly));
        QCOMPARE(imageAfterImport.readAll(), originalImageBytes);
        imageAfterImport.close();

        const QString projectPath = dir.filePath(QStringLiteral("roundtrip.dproj"));
        QVERIFY(project->save(projectPath));
        Project reopened;
        QVERIFY(reopened.load(projectPath));
        QCOMPARE(reopened.dataSources().size(), 7);
        QCOMPARE(reopened.findDataSource(importedImage.value(QStringLiteral("dataSourceId")).toString())->filePath,
                 QFileInfo(imagePath).canonicalFilePath());
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testRegisteredImageCanBeBoundToExplicitGrabImageInstance() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        projects.closeProject();
        Project* project = projects.newProject();
        QVERIFY(project);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString imagePath = dir.filePath(QStringLiteral("binding-image.png"));
        QImage image(3, 2, QImage::Format_RGB32);
        image.fill(qRgb(30, 60, 90));
        QVERIFY(image.save(imagePath, "PNG"));

        DataSource source;
        source.id = QStringLiteral("binding-image-source");
        source.name = QFileInfo(imagePath).fileName();
        source.filePath = QFileInfo(imagePath).canonicalFilePath();
        source.type = QStringLiteral("image");
        project->addDataSource(source);

        ModuleInstance firstGrab;
        firstGrab.id = QStringLiteral("binding-grab-first");
        firstGrab.moduleId = QStringLiteral("GrabImage");
        firstGrab.name = QStringLiteral("First GrabImage");
        firstGrab.params =
            QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Camera")}, {QStringLiteral("filePath"), ""}};
        project->addModule(firstGrab);
        ModuleInstance selectedGrab = firstGrab;
        selectedGrab.id = QStringLiteral("binding-grab-selected");
        selectedGrab.name = QStringLiteral("Selected GrabImage");
        project->addModule(selectedGrab);
        const QJsonObject before = project->toJson();

        const QJsonObject listed = controller.actor()->executeTool(QStringLiteral("list_data_sources"), {});
        QCOMPARE(listed.value(QStringLiteral("total")).toInt(), 1);
        const QJsonObject listedSource = listed.value(QStringLiteral("dataSources")).toArray().first().toObject();
        QCOMPARE(listedSource.value(QStringLiteral("id")).toString(), source.id);
        QCOMPARE(listedSource.value(QStringLiteral("filePath")).toString(), source.filePath);
        QCOMPARE(listedSource.value(QStringLiteral("type")).toString(), QStringLiteral("image"));
        QVERIFY(listedSource.value(QStringLiteral("fileExists")).toBool());

        const QJsonObject flow = controller.actor()->executeTool(QStringLiteral("get_flow_state"), {});
        QJsonArray grabCandidates;
        for (const QJsonValue& module : flow.value(QStringLiteral("modules")).toArray()) {
            if (module.toObject().value(QStringLiteral("moduleId")).toString() == QLatin1String("GrabImage"))
                grabCandidates.append(module);
        }
        QCOMPARE(grabCandidates.size(), 2);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(controller.evaluateToolGate(QStringLiteral("set_module_params"), {}) ==
                AgentController::ToolGate::Allow);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(
            controller.handleToolCall(QStringLiteral("list_data_sources"), {}).value(QStringLiteral("total")).toInt(),
            1);
        QCOMPARE(controller
                     .handleToolCall(QStringLiteral("set_module_params"),
                                     QJsonObject{{QStringLiteral("instanceId"), selectedGrab.id},
                                                 {QStringLiteral("values"),
                                                  QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                                              {QStringLiteral("filePath"), source.filePath}}}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(project->toJson(), before);

        const QJsonObject bindingParams{
            {QStringLiteral("instanceId"), selectedGrab.id},
            {QStringLiteral("values"), QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Path")},
                                                   {QStringLiteral("filePath"), source.filePath}}}};
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject rejected = controller.handleToolCall(QStringLiteral("set_module_params"), bindingParams);
        QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject pending = controller.pendingConfirmationPayload();
        const QByteArray pendingJson = QJsonDocument(pending).toJson(QJsonDocument::Compact);
        QVERIFY(pendingJson.contains(selectedGrab.id.toUtf8()));
        QVERIFY(pendingJson.contains(source.filePath.toUtf8()));
        QVERIFY(pendingJson.contains(QByteArrayLiteral("Path")));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(project->toJson(), before);

        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject pendingAgain = controller.handleToolCall(QStringLiteral("set_module_params"), bindingParams);
        QCOMPARE(pendingAgain.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const int toolCountBefore = toolSpy.count();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(toolSpy.count(), toolCountBefore + 1);
        const QJsonObject result = toolSpy.last().at(1).toJsonObject();
        QVERIFY2(result.value(QStringLiteral("status")).toString() == QLatin1String("params_set"),
                 qPrintable(QJsonDocument(result).toJson(QJsonDocument::Compact)));
        QCOMPARE(result.value(QStringLiteral("instanceId")).toString(), selectedGrab.id);
        const QJsonObject effective = result.value(QStringLiteral("effectiveParams")).toObject();
        QCOMPARE(effective.value(QStringLiteral("grabSource")).toString(), QStringLiteral("Path"));
        QCOMPARE(effective.value(QStringLiteral("filePath")).toString(), source.filePath);
        QCOMPARE(project->moduleById(selectedGrab.id)->params.value(QStringLiteral("grabSource")).toString(),
                 QStringLiteral("Path"));
        QCOMPARE(project->moduleById(selectedGrab.id)->params.value(QStringLiteral("filePath")).toString(),
                 source.filePath);
        QCOMPARE(project->moduleById(firstGrab.id)->params, firstGrab.params);
        QVERIFY(project->isModified());

        QVERIFY(!auditSpy.isEmpty());
        const AgentActionLogEntry audit = auditSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(audit.action, QStringLiteral("set_module_params"));
        QVERIFY(audit.undoable);
        const QJsonObject auditParams = QJsonDocument::fromJson(audit.params.toUtf8()).object();
        QCOMPARE(auditParams.value(QStringLiteral("instanceId")).toString(), selectedGrab.id);
        QCOMPARE(auditParams.value(QStringLiteral("values")).toObject().value(QStringLiteral("grabSource")).toString(),
                 QStringLiteral("Path"));
        QCOMPARE(auditParams.value(QStringLiteral("values")).toObject().value(QStringLiteral("filePath")).toString(),
                 source.filePath);

        const QString projectPath = dir.filePath(QStringLiteral("grab-binding-roundtrip.dproj"));
        QVERIFY(project->save(projectPath));
        Project reopened;
        QVERIFY(reopened.load(projectPath));
        QCOMPARE(reopened.moduleById(selectedGrab.id)->params.value(QStringLiteral("grabSource")).toString(),
                 QStringLiteral("Path"));
        QCOMPARE(reopened.moduleById(selectedGrab.id)->params.value(QStringLiteral("filePath")).toString(),
                 source.filePath);
        QCOMPARE(reopened.moduleById(firstGrab.id)->params, firstGrab.params);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
    }

    void testDataSourceImportRejectsStaleAndInvalidInputsWithoutProjectMutation() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        projects.closeProject();
        Project* project = projects.newProject();
        QVERIFY(project);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const QString validPath = dir.filePath(QStringLiteral("drift.png"));
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(qRgb(1, 2, 3));
        QVERIFY(image.save(validPath, "PNG"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        auto confirmAndGetResult = [&](const QString& path) {
            const QJsonObject pending = controller.handleToolCall(QStringLiteral("import_data_source"),
                                                                  QJsonObject{{QStringLiteral("path"), path}});
            if (pending.value(QStringLiteral("status")) != QLatin1String("pending_confirmation"))
                return QJsonObject{{QStringLiteral("unexpected"), pending}};
            if (!controller.confirmPendingTools(controller.pendingConfirmationId()))
                return QJsonObject{{QStringLiteral("unexpected"), QStringLiteral("confirmation failed")}};
            if (toolSpy.isEmpty())
                return QJsonObject{{QStringLiteral("unexpected"), QStringLiteral("tool result missing")}};
            return toolSpy.takeLast().at(1).toJsonObject();
        };

        const QJsonObject baseline = project->toJson();
        const quint64 baselineGeneration = project->dataSourceCollectionGeneration();
        const bool baselineDirty = project->isModified();
        const QString missingPath = dir.filePath(QStringLiteral("missing.png"));
        QCOMPARE(confirmAndGetResult(missingPath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("FILE_NOT_FOUND"));

        const QString directoryPath = dir.path();
        QCOMPARE(confirmAndGetResult(directoryPath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INVALID_PATH"));

        const QString unsupportedPath = dir.filePath(QStringLiteral("unsupported.pcd"));
        QFile unsupported(unsupportedPath);
        QVERIFY(unsupported.open(QIODevice::WriteOnly));
        const QByteArray unsupportedBytes = QByteArrayLiteral("not a supported import");
        QCOMPARE(unsupported.write(unsupportedBytes), static_cast<qint64>(unsupportedBytes.size()));
        unsupported.close();
        QCOMPARE(confirmAndGetResult(unsupportedPath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("UNSUPPORTED_FORMAT"));

        const QString corruptImagePath = dir.filePath(QStringLiteral("corrupt.png"));
        QFile corruptImage(corruptImagePath);
        QVERIFY(corruptImage.open(QIODevice::WriteOnly));
        QCOMPARE(corruptImage.write(QByteArrayLiteral("not png data")), qint64(12));
        corruptImage.close();
        QCOMPARE(confirmAndGetResult(corruptImagePath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INVALID_IMAGE"));

        const QString corruptPlyPath = dir.filePath(QStringLiteral("corrupt.ply"));
        QFile corruptPly(corruptPlyPath);
        QVERIFY(corruptPly.open(QIODevice::WriteOnly));
        const QByteArray corruptPlyBytes = QByteArrayLiteral("ply\nformat ascii 1.0\n");
        QCOMPARE(corruptPly.write(corruptPlyBytes), static_cast<qint64>(corruptPlyBytes.size()));
        corruptPly.close();
        QCOMPARE(confirmAndGetResult(corruptPlyPath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INVALID_POINT_CLOUD"));

        const QString truncatedPlyPath = dir.filePath(QStringLiteral("truncated.ply"));
        QFile truncatedPly(truncatedPlyPath);
        QVERIFY(truncatedPly.open(QIODevice::WriteOnly));
        const QByteArray truncatedPlyBytes = QByteArrayLiteral(
            "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\n"
            "end_header\n0 1 2\n");
        QCOMPARE(truncatedPly.write(truncatedPlyBytes), static_cast<qint64>(truncatedPlyBytes.size()));
        truncatedPly.close();
        QCOMPARE(confirmAndGetResult(truncatedPlyPath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INVALID_POINT_CLOUD"));

        const QString corruptTiffPath = dir.filePath(QStringLiteral("corrupt.tiff"));
        QFile corruptTiff(corruptTiffPath);
        QVERIFY(corruptTiff.open(QIODevice::WriteOnly));
        QCOMPARE(corruptTiff.write(QByteArrayLiteral("not tiff data")), qint64(13));
        corruptTiff.close();
        QCOMPARE(confirmAndGetResult(corruptTiffPath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INVALID_POINT_CLOUD"));

        const int toolsBeforeStale = toolSpy.count();
        const QJsonObject stalePending = controller.handleToolCall(QStringLiteral("import_data_source"),
                                                                   QJsonObject{{QStringLiteral("path"), validPath}});
        QCOMPARE(stalePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        image.fill(qRgb(210, 130, 70));
        QVERIFY(image.save(validPath, "PNG")); // Same path, changed content/hash after approval.
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(toolSpy.count(), toolsBeforeStale);
        QCOMPARE(project->toJson(), baseline);
        QCOMPARE(project->dataSourceCollectionGeneration(), baselineGeneration);
        QCOMPARE(project->isModified(), baselineDirty);

        const QString replacement = dir.filePath(QStringLiteral("replacement.png"));
        QVERIFY(image.save(replacement, "PNG"));
        const QString resultNoProjectPath = replacement;
        projects.closeProject();
        QCOMPARE(confirmAndGetResult(resultNoProjectPath).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("NO_PROJECT"));

        project = projects.newProject();
        QVERIFY(project);
        const QJsonObject busyBaseline = project->toJson();
        const quint64 busyGeneration = project->dataSourceCollectionGeneration();
        const QJsonObject pendingBusy = controller.handleToolCall(
            QStringLiteral("import_data_source"), QJsonObject{{QStringLiteral("path"), resultNoProjectPath}});
        QCOMPARE(pendingBusy.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString pendingBusyId = controller.pendingConfirmationId();
        const QJsonObject call = controller.pendingToolCalls().first().toObject();
        const AgentActor::ToolInvocation invocation{
            call.value(QStringLiteral("id")).toString(),
            QStringLiteral("import_data_source"),
            call.value(QStringLiteral("arguments")).toObject(),
            {},
            {},
            {},
            controller.m_pendingFileTargetPaths.value(QStringLiteral("data-source-import:0")).toObject()};
        QJsonObject busyResult;
        const RunEngine::ProjectMutationResult leaseResult = RunEngine::instance().withProjectMutationLease([&]() {
            const QJsonObject batch = controller.actor()->executeTools({invocation}, QStringLiteral("busy import"));
            busyResult = batch.value(QStringLiteral("results"))
                             .toArray()
                             .first()
                             .toObject()
                             .value(QStringLiteral("result"))
                             .toObject();
            return true;
        });
        QCOMPARE(leaseResult, RunEngine::ProjectMutationResult::Applied);
        QCOMPARE(busyResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("ENGINE_BUSY"));
        QVERIFY(controller.rejectPendingTools(pendingBusyId));
        QCOMPARE(project->toJson(), busyBaseline);
        QCOMPARE(project->dataSourceCollectionGeneration(), busyGeneration);
        QVERIFY(!project->isModified());
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testGlobalVariablePermissionsConfirmationAndStaleSnapshots() {
        AgentController& controller = AgentController::instance();
        GlobalVarManager& variables = GlobalVarManager::instance();
        RunEngine::instance().stop();
        ProjectManager::instance().closeProject();
        variables.clearAllVariables();
        auto* initial = new VarModel(QStringLiteral("confirmedVar"), VarDataType::Int, 3);
        initial->setExpression(QStringLiteral("old expression must not be disclosed"));
        initial->setIsLinked(true);
        variables.addVariable(initial);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QVERIFY(controller.handleToolCall(QStringLiteral("list_global_variables"), {}).contains("variables"));
        QCOMPARE(controller
                     .handleToolCall(QStringLiteral("get_global_variable"),
                                     QJsonObject{{QStringLiteral("name"), QStringLiteral("confirmedVar")}})
                     .value(QStringLiteral("value"))
                     .toInt(),
                 3);
        QCOMPARE(controller
                     .handleToolCall(QStringLiteral("set_global_variable"),
                                     QJsonObject{{QStringLiteral("name"), QStringLiteral("confirmedVar")},
                                                 {QStringLiteral("value"), 8}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(variables.getVariableValue(QStringLiteral("confirmedVar")).toInt(), 3);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject stale = controller.handleToolCall(
            QStringLiteral("set_global_variable"),
            QJsonObject{{QStringLiteral("name"), QStringLiteral("confirmedVar")}, {QStringLiteral("value"), 9}});
        QCOMPARE(stale.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject stalePayload = stale.value(QStringLiteral("confirmation")).toObject();
        const QJsonObject staleContext = stalePayload.value(QStringLiteral("context")).toObject();
        const QString staleSummary = staleContext.value(QStringLiteral("summary")).toString();
        QVERIFY(staleSummary.contains(QStringLiteral("confirmedVar")));
        QVERIFY(staleSummary.contains(QStringLiteral("value=9")));
        QVERIFY(staleSummary.contains(QStringLiteral("保留现有 expression/link")));
        QVERIFY(!staleSummary.contains(QStringLiteral("old expression")));
        QVERIFY(!staleSummary.contains(QStringLiteral("value=3")));
        QVERIFY(variables.setVariableValue(QStringLiteral("confirmedVar"), 4));
        QVERIFY(controller.confirmPendingTools(stalePayload.value(QStringLiteral("confirmationId")).toString()));
        QCOMPARE(variables.getVariableValue(QStringLiteral("confirmedVar")).toInt(), 4);

        const QJsonObject approved = controller.handleToolCall(
            QStringLiteral("set_global_variable"),
            QJsonObject{{QStringLiteral("name"), QStringLiteral("confirmedVar")}, {QStringLiteral("value"), 9}});
        QVERIFY(controller.confirmPendingTools(approved.value(QStringLiteral("confirmation"))
                                                   .toObject()
                                                   .value(QStringLiteral("confirmationId"))
                                                   .toString()));
        QCOMPARE(variables.getVariableValue(QStringLiteral("confirmedVar")).toInt(), 9);
        QCOMPARE(variables.getVariable(QStringLiteral("confirmedVar"))->expression(),
                 QStringLiteral("old expression must not be disclosed"));
        QVERIFY(variables.getVariable(QStringLiteral("confirmedVar"))->isLinked());

        const QJsonObject staleRemove =
            controller.handleToolCall(QStringLiteral("remove_global_variable"),
                                      QJsonObject{{QStringLiteral("name"), QStringLiteral("confirmedVar")}});
        const QString staleRemoveId = staleRemove.value(QStringLiteral("confirmation"))
                                          .toObject()
                                          .value(QStringLiteral("confirmationId"))
                                          .toString();
        variables.removeVariable(QStringLiteral("confirmedVar"));
        variables.addVariable(new VarModel(QStringLiteral("confirmedVar"), VarDataType::Int, 15));
        QVERIFY(controller.confirmPendingTools(staleRemoveId));
        QVERIFY(variables.hasVariable(QStringLiteral("confirmedVar")));
        QCOMPARE(variables.getVariableValue(QStringLiteral("confirmedVar")).toInt(), 15);

        const QJsonObject create =
            controller.handleToolCall(QStringLiteral("create_global_variable"),
                                      QJsonObject{{QStringLiteral("name"), QStringLiteral("approvedCreate")},
                                                  {QStringLiteral("type"), QStringLiteral("string")},
                                                  {QStringLiteral("value"), QStringLiteral("literal")}});
        QVERIFY(controller.confirmPendingTools(create.value(QStringLiteral("confirmation"))
                                                   .toObject()
                                                   .value(QStringLiteral("confirmationId"))
                                                   .toString()));
        QCOMPARE(variables.getVariableValue(QStringLiteral("approvedCreate")).toString(), QStringLiteral("literal"));

        QVERIFY(variables.beginExecutionLease());
        const QJsonObject busy = controller.handleToolCall(
            QStringLiteral("set_global_variable"),
            QJsonObject{{QStringLiteral("name"), QStringLiteral("confirmedVar")}, {QStringLiteral("value"), 22}});
        const QString busyId =
            busy.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        QVERIFY(controller.confirmPendingTools(busyId));
        variables.endExecutionLease();
        QCOMPARE(variables.getVariableValue(QStringLiteral("confirmedVar")).toInt(), 15);
        QCOMPARE(controller.permissionLevel(), AgentController::PermissionLevel::Autopilot);
    }

    void testDataSourceReferenceConfirmationEpochAndPermissions() {
        AgentController& controller = AgentController::instance();
        RunEngine::instance().stop();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString sourcePath = directory.filePath(QStringLiteral("source.dat"));
        QFile sourceFile(sourcePath);
        QVERIFY(sourceFile.open(QIODevice::WriteOnly));
        const QByteArray original("source bytes");
        QCOMPARE(sourceFile.write(original), original.size());
        sourceFile.close();

        DataSource source;
        source.id = QStringLiteral("confirmed-source");
        source.name = QStringLiteral("confirmed source");
        source.filePath = sourcePath;
        source.type = QStringLiteral("image");
        project->addDataSource(source);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(
            controller.handleToolCall(QStringLiteral("list_data_sources"), {}).value(QStringLiteral("total")).toInt(),
            1);
        QCOMPARE(controller
                     .handleToolCall(QStringLiteral("remove_data_source_reference"),
                                     QJsonObject{{QStringLiteral("dataSourceId"), source.id}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(project->findDataSource(source.id));

        QStringList removalAuditResults;
        const QMetaObject::Connection auditConnection =
            QObject::connect(&controller, &AgentController::actionLogEntryAdded, &controller,
                             [&removalAuditResults](const AgentActionLogEntry& entry) {
                                 if (entry.action == QLatin1String("remove_data_source_reference"))
                                     removalAuditResults.append(entry.result);
                             });
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject pending = controller.handleToolCall(QStringLiteral("remove_data_source_reference"),
                                                              QJsonObject{{QStringLiteral("dataSourceId"), source.id}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject pendingPayload = pending.value(QStringLiteral("confirmation")).toObject();
        const QJsonArray sideEffects =
            pendingPayload.value(QStringLiteral("context")).toObject().value(QStringLiteral("sideEffects")).toArray();
        QCOMPARE(sideEffects.size(), 1);
        QVERIFY(sideEffects.first().toString().contains(QStringLiteral("project_change:data_source_reference")));
        QVERIFY(sideEffects.first().toString().contains(QStringLiteral("source_file=preserved")));
        const QString expiredId = pendingPayload.value(QStringLiteral("confirmationId")).toString();
        const QJsonObject beforeReload = project->toJson();
        const quint64 generationBeforeReload = project->dataSourceCollectionGeneration();
        QVERIFY(project->fromJson(beforeReload));
        QCOMPARE(project->toJson(), beforeReload);
        QVERIFY(project->dataSourceCollectionGeneration() > generationBeforeReload);
        QVERIFY(controller.confirmPendingTools(expiredId));
        QVERIFY(project->findDataSource(source.id));
        QCOMPARE(removalAuditResults.last(), QStringLiteral("expired"));

        const QJsonObject rejected = controller.handleToolCall(
            QStringLiteral("remove_data_source_reference"), QJsonObject{{QStringLiteral("dataSourceId"), source.id}});
        const QString rejectedId = rejected.value(QStringLiteral("confirmation"))
                                       .toObject()
                                       .value(QStringLiteral("confirmationId"))
                                       .toString();
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(project->findDataSource(source.id));
        QCOMPARE(removalAuditResults.last(), QStringLiteral("rejected"));

        const QJsonObject approved = controller.handleToolCall(
            QStringLiteral("remove_data_source_reference"), QJsonObject{{QStringLiteral("dataSourceId"), source.id}});
        const QString approvedId = approved.value(QStringLiteral("confirmation"))
                                       .toObject()
                                       .value(QStringLiteral("confirmationId"))
                                       .toString();
        QVERIFY(controller.confirmPendingTools(approvedId));
        QVERIFY(!project->findDataSource(source.id));
        QFile preserved(sourcePath);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
        QCOMPARE(preserved.readAll(), original);
        QCOMPARE(removalAuditResults.last(), QStringLiteral("success"));

        const QString projectPath = directory.filePath(QStringLiteral("same-project.dproj"));
        project->addDataSource(source);
        QVERIFY(project->save(projectPath));
        ProjectManager::instance().closeProject();
        project = ProjectManager::instance().openProject(projectPath);
        QVERIFY(project);
        QVERIFY(project->findDataSource(source.id));
        const quint64 openedIdentity = project->runtimeIdentity();
        QCOMPARE(project->dataSourceCollectionGeneration(), 1u);
        const QJsonObject reopenedPending = controller.handleToolCall(
            QStringLiteral("remove_data_source_reference"), QJsonObject{{QStringLiteral("dataSourceId"), source.id}});
        const QString reopenedPendingId = reopenedPending.value(QStringLiteral("confirmation"))
                                              .toObject()
                                              .value(QStringLiteral("confirmationId"))
                                              .toString();
        const QJsonObject reopenedSnapshot = project->toJson();
        ProjectManager::instance().closeProject();
        project = ProjectManager::instance().openProject(projectPath);
        QVERIFY(project);
        QCOMPARE(project->toJson(), reopenedSnapshot);
        QCOMPARE(project->dataSourceCollectionGeneration(), 1u);
        QVERIFY(project->runtimeIdentity() != openedIdentity);
        QVERIFY(controller.confirmPendingTools(reopenedPendingId));
        QVERIFY(project->findDataSource(source.id));
        QCOMPARE(removalAuditResults.last(), QStringLiteral("expired"));

        const QString batchVariableName = QStringLiteral("staleDataSourceBatchVariable");
        const QJsonArray mixedBatch{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("create-before-remove")},
                        {QStringLiteral("type"), QStringLiteral("function")},
                        {QStringLiteral("name"), QStringLiteral("create_global_variable")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("name"), batchVariableName},
                                                                  {QStringLiteral("type"), QStringLiteral("int")},
                                                                  {QStringLiteral("value"), 1}}}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("remove-after-create")},
                        {QStringLiteral("type"), QStringLiteral("function")},
                        {QStringLiteral("name"), QStringLiteral("remove_data_source_reference")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("dataSourceId"), source.id}}}}};
        controller.beginPendingConfirmation(mixedBatch, QString(), false);
        const QString mixedBatchConfirmationId = controller.pendingConfirmationId();
        const QJsonObject beforeMixedBatchReload = project->toJson();
        QVERIFY(project->fromJson(beforeMixedBatchReload));
        QCOMPARE(project->toJson(), beforeMixedBatchReload);
        QVERIFY(controller.confirmPendingTools(mixedBatchConfirmationId));
        QVERIFY(project->findDataSource(source.id));
        QVERIFY(!GlobalVarManager::instance().hasVariable(batchVariableName));
        QCOMPARE(removalAuditResults.last(), QStringLiteral("expired"));
        disconnect(auditConnection);
    }

    void testGlobalVariableAuditsRedactValuesAndRejectionIsAtomic() {
        AgentController& controller = AgentController::instance();
        GlobalVarManager& variables = GlobalVarManager::instance();
        variables.clearAllVariables();
        auto* original = new VarModel(QStringLiteral("auditVariable"), VarDataType::Int, 17);
        original->setExpression(QStringLiteral("private-expression-token"));
        original->setNote(QStringLiteral("private-note-token"));
        original->setIsLinked(true);
        variables.addVariable(original);

        QStringList auditParams;
        QStringList auditResults;
        const QMetaObject::Connection auditConnection =
            connect(&controller, &AgentController::actionLogEntryAdded, this,
                    [&auditParams, &auditResults](const AgentActionLogEntry& entry) {
                        if (entry.action == QLatin1String("create_global_variable") ||
                            entry.action == QLatin1String("set_global_variable") ||
                            entry.action == QLatin1String("remove_global_variable")) {
                            auditParams.append(entry.params);
                            auditResults.append(entry.result);
                        }
                    });

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const auto requestConfirmation = [&controller](const QString& tool, const QJsonObject& params) {
            const QJsonObject pending = controller.handleToolCall(tool, params);
            return pending.value(QStringLiteral("confirmation"))
                .toObject()
                .value(QStringLiteral("confirmationId"))
                .toString();
        };

        QString confirmationId =
            requestConfirmation(QStringLiteral("set_global_variable"),
                                QJsonObject{{QStringLiteral("name"), QStringLiteral("auditVariable")},
                                            {QStringLiteral("value"), 987654321}});
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(controller.rejectPendingTools(confirmationId));
        QCOMPARE(variables.getVariableValue(QStringLiteral("auditVariable")).toInt(), 17);
        QCOMPARE(variables.getVariable(QStringLiteral("auditVariable"))->expression(),
                 QStringLiteral("private-expression-token"));
        QCOMPARE(variables.getVariable(QStringLiteral("auditVariable"))->note(), QStringLiteral("private-note-token"));
        QVERIFY(variables.getVariable(QStringLiteral("auditVariable"))->isLinked());
        QCOMPARE(auditResults.last(), QStringLiteral("rejected"));

        confirmationId = requestConfirmation(QStringLiteral("remove_global_variable"),
                                             QJsonObject{{QStringLiteral("name"), QStringLiteral("auditVariable")}});
        QVERIFY(controller.rejectPendingTools(confirmationId));
        QVERIFY(variables.hasVariable(QStringLiteral("auditVariable")));
        QCOMPARE(variables.getVariableValue(QStringLiteral("auditVariable")).toInt(), 17);
        QCOMPARE(variables.getVariable(QStringLiteral("auditVariable"))->note(), QStringLiteral("private-note-token"));

        confirmationId =
            requestConfirmation(QStringLiteral("create_global_variable"),
                                QJsonObject{{QStringLiteral("name"), QStringLiteral("rejectedCreate")},
                                            {QStringLiteral("type"), QStringLiteral("string")},
                                            {QStringLiteral("value"), QStringLiteral("private-create-token")}});
        QVERIFY(controller.rejectPendingTools(confirmationId));
        QVERIFY(!variables.hasVariable(QStringLiteral("rejectedCreate")));

        const QJsonObject approved =
            controller.handleToolCall(QStringLiteral("set_global_variable"),
                                      QJsonObject{{QStringLiteral("name"), QStringLiteral("auditVariable")},
                                                  {QStringLiteral("value"), 246813579}});
        confirmationId = approved.value(QStringLiteral("confirmation"))
                             .toObject()
                             .value(QStringLiteral("confirmationId"))
                             .toString();
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(variables.getVariableValue(QStringLiteral("auditVariable")).toInt(), 246813579);

        QVERIFY(!auditParams.isEmpty());
        for (const QString& paramsText : auditParams) {
            QVERIFY(!paramsText.contains(QStringLiteral("987654321")));
            QVERIFY(!paramsText.contains(QStringLiteral("246813579")));
            QVERIFY(!paramsText.contains(QStringLiteral("private-create-token")));
            QVERIFY(!paramsText.contains(QStringLiteral("private-expression-token")));
            QVERIFY(!paramsText.contains(QStringLiteral("private-note-token")));
            const QJsonObject params = QJsonDocument::fromJson(paramsText.toUtf8()).object();
            QVERIFY(!params.contains(QStringLiteral("value")));
            QVERIFY(!params.contains(QStringLiteral("note")));
            QVERIFY(!params.contains(QStringLiteral("expression")));
        }
        QVERIFY(auditParams.last().contains(QStringLiteral("auditVariable")));
        QVERIFY(auditResults.contains(QStringLiteral("success")));
        disconnect(auditConnection);
    }

    void testDuplicateToolCallIdsAreNormalizedAndVariableDriftExpiresBatch() {
        AgentController& controller = AgentController::instance();
        GlobalVarManager& variables = GlobalVarManager::instance();
        variables.clearAllVariables();
        variables.addVariable(new VarModel(QStringLiteral("firstVariable"), VarDataType::Int, 1));
        variables.addVariable(new VarModel(QStringLiteral("secondVariable"), VarDataType::Int, 2));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Update both variables after review."));
        AgentResponse response;
        response.success = true;
        response.content = QStringLiteral("Reviewing both variable updates.");
        response.toolCalls = {
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("duplicate-call-id")},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("name"), QStringLiteral("set_global_variable")},
                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("name"), QStringLiteral("firstVariable")},
                                                          {QStringLiteral("value"), 11}}}},
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("duplicate-call-id")},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("name"), QStringLiteral("set_global_variable")},
                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("name"), QStringLiteral("secondVariable")},
                                                          {QStringLiteral("value"), 22}}}},
        };
        m_mock->emitResponse(response);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Confirming, 5000);

        const QJsonArray pendingCalls =
            controller.pendingConfirmationPayload().value(QStringLiteral("tools")).toArray();
        QCOMPARE(pendingCalls.size(), 2);
        const QString firstCallId = pendingCalls.at(0).toObject().value(QStringLiteral("id")).toString();
        const QString secondCallId = pendingCalls.at(1).toObject().value(QStringLiteral("id")).toString();
        QVERIFY(!firstCallId.isEmpty());
        QVERIFY(!secondCallId.isEmpty());
        QVERIFY(firstCallId != secondCallId);
        QVERIFY(variables.setVariableValue(QStringLiteral("firstVariable"), 3));

        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(variables.getVariableValue(QStringLiteral("firstVariable")).toInt(), 3);
        QCOMPARE(variables.getVariableValue(QStringLiteral("secondVariable")).toInt(), 2);
        for (const QString& callId : {firstCallId, secondCallId}) {
            int resultCount = 0;
            for (const AgentMessage& message : controller.m_conversationHistory)
                if (message.role == QLatin1String("tool") && message.toolCallId == callId)
                    ++resultCount;
            QCOMPARE(resultCount, 1);
        }
    }

    void testFailedRunDetailsReachContext() {
        AgentController& controller = AgentController::instance();
        RunResult failed;
        failed.success = false;
        failed.errorMessage = QStringLiteral("missing input");
        failed.errorCode = 42;
        failed.failedModule = QStringLiteral("module-17");
        failed.failedPort = QStringLiteral("image");
        failed.diagnostics = QStringLiteral("source port is empty");
        controller.m_observer->onRunFinished(failed);
        const QString context = controller.buildContext();
        QVERIFY(context.contains(QStringLiteral("moduleId=module-17")));
        QVERIFY(context.contains(QStringLiteral("port=image")));
        QVERIFY(context.contains(QStringLiteral("errorCode=42")));
        QVERIFY(context.contains(QStringLiteral("source port is empty")));
    }

    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        QVERIFY(m_settingsDir.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, m_settingsDir.path());
        ToolSchema::instance().registerDefaultTools();
        AgentController::instance().initialize();
    }

    void cleanupTestCase() {
        AgentController::instance().shutdown();
        QSettings::setDefaultFormat(m_oldSettingsFormat);
    }

    void init() {
        // 每个测试前彻底重置状态（状态机 + 对话历史 + client）
        UserManager::instance().login(QStringLiteral("developer"), QString());
        AgentController::instance().clearConversation();
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        // 强制回到 Idle（前一个测试可能留下非 Idle 状态）
        AgentController::instance().transitionTo(AgentController::AgentState::Idle);
        // 上下文治理：每例恢复默认上限，避免上例的测试覆盖值泄漏
        AgentController::instance().setContextLimitsForTest(128000, 6000, 24, 24);
        AgentController::instance().setTurnDeadlineMsForTest(-1);
        AgentController::instance().setTurnDeadlineConfigForTest(-1);

        m_mock = new MockLLMClient(&AgentController::instance());
        m_oldClient = AgentController::instance().llmClient();
        AgentController::instance().setLLMClient(m_mock);
    }

    void cleanup() {
        AgentController::instance().setLLMClient(m_oldClient);
        delete m_mock;
        m_mock = nullptr;
    }

    // ---------- 状态机基础 ----------

    void testAgentUndoStopsAtSuccessfulSaveBarrier() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("undo_barrier_node");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("original");
        project->addModule(instance);

        AgentActor* actor = controller.actor();
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("rename_module"),
                                   QJsonObject{{"instanceId", instance.id}, {"name", "before save"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString savePath = tempDir.filePath(QStringLiteral("saved.json"));
        QCOMPARE(actor->executeTool(QStringLiteral("save_project"), QJsonObject{{"path", savePath}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("saved"));
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("rename_module"),
                                   QJsonObject{{"instanceId", instance.id}, {"name", "after save"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));

        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy rejectedSpy(&controller, &AgentController::undoRejected);
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("before save"));
        QUndoStack* stack = actor->undoStack();
        const int indexAtBarrier = stack->index();
        const int countAtBarrier = stack->count();
        QVERIFY(actor->undoBlockedByBarrier());
        QVERIFY(!controller.undoLastAgentAction());
        QCOMPARE(stack->index(), indexAtBarrier);
        QCOMPARE(stack->count(), countAtBarrier);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("before save"));
        QVERIFY(!logSpy.isEmpty());
        QCOMPARE(logSpy.last().first().value<AgentActionLogEntry>().result, QStringLiteral("blocked_by_undo_barrier"));
        QCOMPARE(rejectedSpy.size(), 1);

        QFile file(savePath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(saved.value(QStringLiteral("modules"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("name"))
                     .toString(),
                 QStringLiteral("before save"));
    }

    void testSavePreconditionFailureDoesNotEstablishUndoBarrier() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("unsaved_undo_node");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("original");
        project->addModule(instance);

        AgentActor* actor = controller.actor();
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("rename_module"),
                                   QJsonObject{{"instanceId", instance.id}, {"name", "edited"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        const QJsonObject failed = actor->executeTool(QStringLiteral("save_project"), {});
        QCOMPARE(failed.value(QStringLiteral("errorCode")).toString(), QStringLiteral("SAVE_FAILED"));
        QVERIFY(!actor->undoBlockedByBarrier());
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("original"));

        QUndoStack* stack = actor->undoStack();
        const int baseCount = stack->count();
        const QJsonObject failedBatch =
            actor->executeTools({{QStringLiteral("before"), QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "before failed save"}}},
                                 {QStringLiteral("save"), QStringLiteral("save_project"), QJsonObject()},
                                 {QStringLiteral("after"), QStringLiteral("rename_module"),
                                  QJsonObject{{"instanceId", instance.id}, {"name", "must be skipped"}}}},
                                QStringLiteral("save-precondition-barrier-test"));
        QCOMPARE(failedBatch.value(QStringLiteral("status")).toString(), QStringLiteral("partial_failed"));
        const QJsonArray results = failedBatch.value(QStringLiteral("results")).toArray();
        QCOMPARE(results.size(), 3);
        QCOMPARE(results.at(0).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("before"));
        QCOMPARE(results.at(1).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("save"));
        QCOMPARE(results.at(1)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("SAVE_FAILED"));
        QCOMPARE(results.at(2).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("after"));
        QCOMPARE(results.at(2)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(stack->index(), baseCount);
        QVERIFY(!actor->undoBlockedByBarrier());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("before failed save"));
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("original"));
    }

    void testSaveProjectConfirmationGatesDiskWrite() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        Project* project = projects.newProject();
        QVERIFY(project);
        project->setName(QStringLiteral("Slice 36 confirmed save"));

        QTemporaryDir targetDir;
        QVERIFY(targetDir.isValid());
        const QString path = targetDir.filePath(QStringLiteral("confirmed-project.dproj"));
        QVERIFY(!QFileInfo::exists(path));

        QSettings settings;
        settings.beginGroup(QStringLiteral("ProjectManager"));
        const QStringList recentBefore = settings.value(QStringLiteral("recentProjects")).toStringList();
        settings.endGroup();
        const QStringList recentProjectsBefore = projects.recentProjects();

        int projectSavedCount = 0;
        const QMetaObject::Connection savedConnection =
            connect(&projects, &ProjectManager::projectSaved, &controller,
                    [&projectSavedCount](Project*) { ++projectSavedCount; });
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(!controller.actor()->undoBlockedByBarrier());

        QJsonObject pending =
            controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), path}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject payload = controller.pendingConfirmationPayload();
        const QJsonArray sideEffects =
            payload.value(QStringLiteral("context")).toObject().value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(std::any_of(sideEffects.cbegin(), sideEffects.cend(), [&path](const QJsonValue& effect) {
            return effect.toString() == QStringLiteral("file_write:%1").arg(path);
        }));
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(projects.currentProject(), project);
        QVERIFY(project->filePath().isEmpty());
        QCOMPARE(projects.recentProjects(), recentProjectsBefore);
        settings.sync();
        settings.beginGroup(QStringLiteral("ProjectManager"));
        QCOMPARE(settings.value(QStringLiteral("recentProjects")).toStringList(), recentBefore);
        settings.endGroup();

        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        Project::failNextSaveAfterBytesForTesting(24);
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(projects.currentProject(), project);
        QVERIFY(project->filePath().isEmpty());
        QCOMPARE(projectSavedCount, 0);
        QCOMPARE(projects.recentProjects(), recentProjectsBefore);
        settings.sync();
        settings.beginGroup(QStringLiteral("ProjectManager"));
        QCOMPARE(settings.value(QStringLiteral("recentProjects")).toStringList(), recentBefore);
        settings.endGroup();
        QVERIFY(!controller.actor()->undoBlockedByBarrier());
        QVERIFY(!audit.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("save_project"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending =
            controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), path}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(projectSavedCount, 0);
        QVERIFY(!QFileInfo::exists(path));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));

        QCOMPARE(projectSavedCount, 0);
        QVERIFY(!QFileInfo::exists(path));
        QVERIFY(project->isModified());
        QCOMPARE(project->filePath(), QString());
        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(toolExecuted.last().at(0).toString(), QStringLiteral("save_project"));
        QCOMPARE(qvariant_cast<QJsonObject>(toolExecuted.last().at(1)).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SAVE_FAILED"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));

        pending =
            controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), path}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));

        QCOMPARE(projectSavedCount, 1);
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(projects.currentProject(), project);
        QCOMPARE(project->filePath(), path);
        QFile saved(path);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        const QJsonObject savedProject = QJsonDocument::fromJson(saved.readAll()).object();
        QCOMPARE(savedProject.value(QStringLiteral("id")).toString(), project->id());
        QCOMPARE(savedProject.value(QStringLiteral("name")).toString(), QStringLiteral("Slice 36 confirmed save"));
        QCOMPARE(projects.recentProjects().first(), path);
        settings.sync();
        settings.beginGroup(QStringLiteral("ProjectManager"));
        QVERIFY(settings.value(QStringLiteral("recentProjects")).toStringList().contains(path));
        settings.endGroup();
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("save_project"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        project->setName(QStringLiteral("Advisor rejected save then retry"));
        pending =
            controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), path}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString advisorRejectedId = controller.pendingConfirmationId();
        QVERIFY(!advisorRejectedId.isEmpty());
        Project::failNextSaveAfterBytesForTesting(24);
        QVERIFY(controller.rejectPendingTools(advisorRejectedId));
        QCOMPARE(projectSavedCount, 1);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(project->isModified());

        pending =
            controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), path}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(projectSavedCount, 1);
        QVERIFY(project->isModified());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        QCOMPARE(qvariant_cast<QJsonObject>(toolExecuted.last().at(1)).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SAVE_FAILED"));

        pending =
            controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{QStringLiteral("path"), path}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(projectSavedCount, 2);
        QVERIFY(!project->isModified());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        disconnect(savedConnection);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        projects.closeProject();
    }

    void testAnnotationSessionExportUsesFreshSnapshotAndPermissionConfirmation() {
        AgentController& controller = AgentController::instance();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QImage image(8, 6, QImage::Format_RGB32);
        image.fill(Qt::blue);
        const QString imagePath = QDir(dir.path()).filePath(QStringLiteral("source.png"));
        QVERIFY(image.save(imagePath, "PNG"));
        AnnotationSession session;
        session.imagePath = imagePath;
        session.imageWidth = 8;
        session.imageHeight = 6;
        AnnotationObject object;
        object.id = QStringLiteral("export-id");
        object.label = QStringLiteral("crack");
        object.polygon = {QPointF(1, 1), QPointF(6, 1), QPointF(6, 4)};
        object.bbox = QRectF(1, 1, 5, 3);
        session.annotations = {object};
        const QString sessionPath = QDir(dir.path()).filePath(QStringLiteral("source.anno.json"));
        QString saveError;
        QVERIFY(session.save(sessionPath, &saveError));
        const QString outputPath = QDir(dir.path()).filePath(QStringLiteral("approved.json"));
        const QJsonObject params{{QStringLiteral("sessionPath"), sessionPath},
                                 {QStringLiteral("format"), QStringLiteral("labelme")},
                                 {QStringLiteral("outputPath"), outputPath}};
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy executed(controller.actor(), &AgentActor::toolExecuted);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QJsonObject denied = controller.handleToolCall(QStringLiteral("export_annotation_session"), params);
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(!QFileInfo::exists(outputPath));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("export_annotation_session"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject payload = pending.value(QStringLiteral("confirmation")).toObject();
        const QString confirmationId = payload.value(QStringLiteral("confirmationId")).toString();
        QVERIFY(!confirmationId.isEmpty());
        const QString summary =
            payload.value(QStringLiteral("context")).toObject().value(QStringLiteral("summary")).toString();
        QVERIFY(summary.contains(sessionPath));
        QVERIFY(summary.contains(outputPath));
        QVERIFY(payload.value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .first()
                    .toString()
                    .contains(outputPath));
        QVERIFY(controller.rejectPendingTools(confirmationId));
        QVERIFY(!QFileInfo::exists(outputPath));

        pending = controller.handleToolCall(QStringLiteral("export_annotation_session"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        session.annotations[0].label = QStringLiteral("changed");
        QVERIFY(session.save(sessionPath, &saveError));
        QVERIFY(controller.confirmPendingTools(staleId));
        QVERIFY(!QFileInfo::exists(outputPath));
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).result, QStringLiteral("expired"));

        const QString racedPath = QDir(dir.path()).filePath(QStringLiteral("raced.json"));
        QJsonObject racedParams = params;
        racedParams.insert(QStringLiteral("outputPath"), racedPath);
        pending = controller.handleToolCall(QStringLiteral("export_annotation_session"), racedParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QFile racedTarget(racedPath);
        QVERIFY(racedTarget.open(QIODevice::WriteOnly));
        const QByteArray racerBytes("created-after-confirmation");
        QCOMPARE(racedTarget.write(racerBytes), qint64(racerBytes.size()));
        racedTarget.close();
        QVERIFY(controller.confirmPendingTools(pending.value(QStringLiteral("confirmation"))
                                                   .toObject()
                                                   .value(QStringLiteral("confirmationId"))
                                                   .toString()));
        QFile racedCheck(racedPath);
        QVERIFY(racedCheck.open(QIODevice::ReadOnly));
        QCOMPARE(racedCheck.readAll(), racerBytes);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).result, QStringLiteral("expired"));

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QString revokedPath = QDir(dir.path()).filePath(QStringLiteral("revoked.json"));
        QJsonObject revokedParams = params;
        revokedParams.insert(QStringLiteral("outputPath"), revokedPath);
        pending = controller.handleToolCall(QStringLiteral("export_annotation_session"), revokedParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString revokedId =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(controller.confirmPendingTools(revokedId));
        QVERIFY(!QFileInfo::exists(revokedPath));
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).result, QStringLiteral("denied"));

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));

        const QString approvedPath = QDir(dir.path()).filePath(QStringLiteral("approved-fresh.json"));
        QJsonObject approvedParams = params;
        approvedParams.insert(QStringLiteral("outputPath"), approvedPath);
        pending = controller.handleToolCall(QStringLiteral("export_annotation_session"), approvedParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        QVERIFY(controller.confirmPendingTools(approvedId));
        QVERIFY(QFileInfo::exists(approvedPath));
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).result, QStringLiteral("success"));
        QVERIFY(!qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).undoable);
        QVERIFY(!executed.isEmpty());

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QString autopilotPath = QDir(dir.path()).filePath(QStringLiteral("autopilot.json"));
        QJsonObject autopilotParams = params;
        autopilotParams.insert(QStringLiteral("outputPath"), autopilotPath);
        pending = controller.handleToolCall(QStringLiteral("export_annotation_session"), autopilotParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(pending.value(QStringLiteral("confirmation"))
                                                  .toObject()
                                                  .value(QStringLiteral("confirmationId"))
                                                  .toString()));
        QVERIFY(!QFileInfo::exists(autopilotPath));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testAnnotationSessionTransformRequiresConfirmationAndPreservesSource() {
        AgentController& controller = AgentController::instance();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        AnnotationSession session;
        session.imagePath = QStringLiteral("offline-image.png");
        session.imageWidth = 20;
        session.imageHeight = 10;
        AnnotationObject object;
        object.id = QStringLiteral("transform-id");
        object.label = QStringLiteral("before");
        object.bbox = QRectF(1, 1, 5, 5);
        object.polygon = {QPointF(1, 1), QPointF(6, 1), QPointF(6, 6)};
        object.maskRle = QStringLiteral("must-not-enter-audit");
        session.annotations = {object};
        const QString sessionPath = QDir(dir.path()).filePath(QStringLiteral("source.deeplux-anno.json"));
        QString saveError;
        QVERIFY(session.save(sessionPath, &saveError));
        QFile input(sessionPath);
        QVERIFY(input.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = input.readAll();
        input.close();
        const QString outputPath = QDir(dir.path()).filePath(QStringLiteral("transformed.deeplux-anno.json"));
        const QJsonObject params{{QStringLiteral("sessionPath"), sessionPath},
                                 {QStringLiteral("outputPath"), outputPath},
                                 {QStringLiteral("operation"), QStringLiteral("set_label")},
                                 {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("transform-id")}},
                                 {QStringLiteral("label"), QStringLiteral("after")}};
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy executed(controller.actor(), &AgentActor::toolExecuted);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QJsonObject denied = controller.handleToolCall(QStringLiteral("transform_annotation_session"), params);
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(!QFileInfo::exists(outputPath));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("transform_annotation_session"), params);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject confirmation = pending.value(QStringLiteral("confirmation")).toObject();
        const QString confirmationId = confirmation.value(QStringLiteral("confirmationId")).toString();
        const QString summary =
            confirmation.value(QStringLiteral("context")).toObject().value(QStringLiteral("summary")).toString();
        QVERIFY(summary.contains(sessionPath));
        QVERIFY(summary.contains(outputPath));
        QVERIFY(summary.contains(QStringLiteral("set_label")));
        QVERIFY(controller.rejectPendingTools(confirmationId));
        QVERIFY(!QFileInfo::exists(outputPath));

        const QString racedPath = QDir(dir.path()).filePath(QStringLiteral("raced.deeplux-anno.json"));
        QJsonObject racedParams = params;
        racedParams.insert(QStringLiteral("outputPath"), racedPath);
        pending = controller.handleToolCall(QStringLiteral("transform_annotation_session"), racedParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QFile racer(racedPath);
        QVERIFY(racer.open(QIODevice::WriteOnly));
        const QByteArray racerBytes("keep-racing-file");
        QCOMPARE(racer.write(racerBytes), qint64(racerBytes.size()));
        racer.close();
        const QString racedConfirmationId =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        QVERIFY(controller.confirmPendingTools(racedConfirmationId));
        QFile racedCheck(racedPath);
        QVERIFY(racedCheck.open(QIODevice::ReadOnly));
        QCOMPARE(racedCheck.readAll(), racerBytes);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).result, QStringLiteral("expired"));

        pending = controller.handleToolCall(QStringLiteral("transform_annotation_session"), params);
        const QString approvedId =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        QVERIFY(controller.confirmPendingTools(approvedId));
        QVERIFY(QFileInfo::exists(outputPath));
        QFile output(outputPath);
        QVERIFY(output.open(QIODevice::ReadOnly));
        const AnnotationSession transformed = AnnotationSession::loadFromBytes(output.readAll(), nullptr, &saveError);
        QCOMPARE(transformed.annotations.size(), 1);
        QCOMPARE(transformed.annotations.first().label, QStringLiteral("after"));
        QFile sourceAfter(sessionPath);
        QVERIFY(sourceAfter.open(QIODevice::ReadOnly));
        QCOMPARE(sourceAfter.readAll(), originalBytes);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).result, QStringLiteral("success"));
        QVERIFY(!qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).undoable);
        QVERIFY(!executed.isEmpty());
        const QByteArray auditJson = audit.last().at(0).value<AgentActionLogEntry>().params.toUtf8();
        QVERIFY(!auditJson.contains("must-not-enter-audit"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QString autopilotOutput = QDir(dir.path()).filePath(QStringLiteral("autopilot.deeplux-anno.json"));
        QJsonObject autopilotParams = params;
        autopilotParams.insert(QStringLiteral("outputPath"), autopilotOutput);
        pending = controller.handleToolCall(QStringLiteral("transform_annotation_session"), autopilotParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(pending.value(QStringLiteral("confirmation"))
                                                  .toObject()
                                                  .value(QStringLiteral("confirmationId"))
                                                  .toString()));
        QVERIFY(!QFileInfo::exists(autopilotOutput));

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QString logoutOutput = QDir(dir.path()).filePath(QStringLiteral("logout.deeplux-anno.json"));
        QJsonObject logoutParams = params;
        logoutParams.insert(QStringLiteral("outputPath"), logoutOutput);
        pending = controller.handleToolCall(QStringLiteral("transform_annotation_session"), logoutParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString logoutId =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        UserManager::instance().logout();
        QVERIFY(controller.confirmPendingTools(logoutId));
        QVERIFY(!QFileInfo::exists(logoutOutput));
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(audit.last().at(0)).result, QStringLiteral("denied"));
        QFile sourceLogoutCheck(sessionPath);
        QVERIFY(sourceLogoutCheck.open(QIODevice::ReadOnly));
        QCOMPARE(sourceLogoutCheck.readAll(), originalBytes);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testKnowledgeDocumentWritesRequireFreshConfirmation() {
        struct RestoreAppData {
            bool wasSet = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
            QByteArray oldValue = qgetenv("DEEPLUX_APP_DATA_DIR");
            ~RestoreAppData() {
                if (wasSet)
                    qputenv("DEEPLUX_APP_DATA_DIR", oldValue);
                else
                    qunsetenv("DEEPLUX_APP_DATA_DIR");
            }
        } restore;
        struct RestoreCurrentDirectory {
            QString oldPath = QDir::currentPath();
            ~RestoreCurrentDirectory() {
                QDir::setCurrent(oldPath);
            }
        } restoreCurrentDirectory;

        AgentController& controller = AgentController::instance();
        QTemporaryDir appData;
        QVERIFY(appData.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", appData.path().toLocal8Bit());
        const QString sourceDir = QDir(appData.path()).filePath(QStringLiteral("sources"));
        QVERIFY(QDir().mkpath(sourceDir));
        const QString sourcePath = QDir(sourceDir).filePath(QStringLiteral("ConfirmedKB.md"));
        {
            QFile source(sourcePath);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QVERIFY(source.write("## secret\nKB_BODY_SECRET\n") > 0);
        }
        QVERIFY(QDir::setCurrent(sourceDir));

        const QJsonObject pendingAdd =
            controller.handleToolCall(QStringLiteral("add_knowledge_document"),
                                      QJsonObject{{QStringLiteral("sourcePath"), QStringLiteral("ConfirmedKB.md")}});
        QCOMPARE(pendingAdd.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject addPayload = pendingAdd.value(QStringLiteral("confirmation")).toObject();
        const QJsonObject addContext = addPayload.value(QStringLiteral("context")).toObject();
        const QString addSummary = addContext.value(QStringLiteral("summary")).toString();
        QVERIFY(addSummary.contains(sourcePath));
        QVERIFY(addSummary.contains(QStringLiteral("ConfirmedKB.md")));
        QVERIFY(!addSummary.contains(QStringLiteral("KB_BODY_SECRET")));
        QVERIFY(addContext.value(QStringLiteral("sideEffects")).toArray().first().toString().contains(sourcePath));

        {
            QFile changed(sourcePath);
            QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QVERIFY(changed.write("changed after preview") > 0);
        }
        QVERIFY(controller.confirmPendingTools(addPayload.value(QStringLiteral("confirmationId")).toString()));
        const QString imported = QDir(AgentActor::userKnowledgeDir()).filePath(QStringLiteral("ConfirmedKB.md"));
        QVERIFY(!QFileInfo::exists(imported));

        const QString stableSource = QDir(sourceDir).filePath(QStringLiteral("StableKB.txt"));
        {
            QFile source(stableSource);
            QVERIFY(source.open(QIODevice::WriteOnly));
            QVERIFY(source.write("stable content\n") > 0);
        }
        const QJsonObject stablePending = controller.handleToolCall(
            QStringLiteral("add_knowledge_document"), QJsonObject{{QStringLiteral("sourcePath"), stableSource}});
        QCOMPARE(stablePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString addId = stablePending.value(QStringLiteral("confirmation"))
                                  .toObject()
                                  .value(QStringLiteral("confirmationId"))
                                  .toString();
        QVERIFY(controller.confirmPendingTools(addId));
        const QString stableTarget = QDir(AgentActor::userKnowledgeDir()).filePath(QStringLiteral("StableKB.txt"));
        QVERIFY(QFileInfo::exists(stableTarget));

        const QJsonObject removePending =
            controller.handleToolCall(QStringLiteral("remove_knowledge_document"),
                                      QJsonObject{{QStringLiteral("documentId"), QStringLiteral("user:StableKB.txt")}});
        QCOMPARE(removePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject removePayload = removePending.value(QStringLiteral("confirmation")).toObject();
        const QString removeSummary =
            removePayload.value(QStringLiteral("context")).toObject().value(QStringLiteral("summary")).toString();
        QVERIFY(removeSummary.contains(stableTarget));
        QVERIFY(!removeSummary.contains(QStringLiteral("stable content")));
#ifdef Q_OS_UNIX
        QFile previousObject(stableTarget);
        QVERIFY(previousObject.open(QIODevice::ReadOnly)); // keep the unlinked inode alive while replacing it
        QVERIFY(QFile::remove(stableTarget));
        {
            QFile replacement(stableTarget);
            QVERIFY(replacement.open(QIODevice::WriteOnly));
            QCOMPARE(replacement.write("stable content\n"), qint64(15));
        }
#else
        {
            QFile changed(stableTarget);
            QVERIFY(changed.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QVERIFY(changed.write("changed target") > 0);
        }
#endif
        QVERIFY(controller.confirmPendingTools(removePayload.value(QStringLiteral("confirmationId")).toString()));
        QFile preserved(stableTarget);
        QVERIFY(preserved.open(QIODevice::ReadOnly));
#ifdef Q_OS_UNIX
        QCOMPARE(preserved.readAll(), QByteArray("stable content\n"));
#else
        QCOMPARE(preserved.readAll(), QByteArray("changed target"));
#endif

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject observerList =
            controller.handleToolCall(QStringLiteral("list_knowledge_documents"), QJsonObject());
        QVERIFY(observerList.contains(QStringLiteral("documents")));
        const QJsonObject observerPreview =
            controller.handleToolCall(QStringLiteral("preview_knowledge_document"),
                                      QJsonObject{{QStringLiteral("documentId"), QStringLiteral("user:StableKB.txt")}});
#ifdef Q_OS_UNIX
        QVERIFY(observerPreview.value(QStringLiteral("content")).toString().contains(QStringLiteral("stable content")));
#else
        QVERIFY(observerPreview.value(QStringLiteral("content")).toString().contains(QStringLiteral("changed target")));
#endif
        const QJsonObject denied =
            controller.handleToolCall(QStringLiteral("remove_knowledge_document"),
                                      QJsonObject{{QStringLiteral("documentId"), QStringLiteral("user:StableKB.txt")}});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(QFileInfo::exists(stableTarget));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testSaveProjectConfirmationBindsExplicitAndDefaultSymlinkTargets() {
#ifndef Q_OS_UNIX
        QSKIP("The symlink target fixture currently requires POSIX file symlinks");
#else
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        Project* project = projects.newProject();
        QVERIFY(project);
        project->setName(QStringLiteral("Slice 93 symlink save"));

        QTemporaryDir targetDir;
        QVERIFY(targetDir.isValid());
        const QString explicitLink = targetDir.filePath(QStringLiteral("explicit save link.dproj"));
        const QString explicitB = targetDir.filePath(QStringLiteral("explicit target B.dproj"));
        const QString explicitC = targetDir.filePath(QStringLiteral("explicit target C.dproj"));
        const auto writeSentinel = [](const QString& path, const QByteArray& bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto readBytes = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray();
            return file.readAll();
        };
        const QByteArray explicitSentinelB("explicit target B sentinel");
        const QByteArray explicitSentinelC("explicit target C sentinel");
        QVERIFY(writeSentinel(explicitB, explicitSentinelB));
        QVERIFY(writeSentinel(explicitC, explicitSentinelC));
        if (!QFile::link(explicitB, explicitLink))
            QSKIP("Unable to create POSIX symlink for save_project target test");

        int projectSavedCount = 0;
        const QMetaObject::Connection savedConnection =
            connect(&projects, &ProjectManager::projectSaved, &controller,
                    [&projectSavedCount](Project*) { ++projectSavedCount; });
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        QJsonObject pending =
            controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{"path", explicitLink}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const auto hasTarget = [](const AgentController& current, const QString& configured, const QString& target) {
            const QJsonArray effects = current.pendingConfirmationPayload()
                                           .value(QStringLiteral("context"))
                                           .toObject()
                                           .value(QStringLiteral("sideEffects"))
                                           .toArray();
            const QString expected = QStringLiteral("file_write:%1 resolved_target=%2").arg(configured, target);
            return std::any_of(effects.cbegin(), effects.cend(),
                               [&expected](const QJsonValue& effect) { return effect.toString() == expected; });
        };
        QVERIFY(hasTarget(controller, explicitLink, explicitB));
        const QString explicitOldId = controller.pendingConfirmationId();
        QVERIFY(QFile::remove(explicitLink));
        QVERIFY(QFile::link(explicitC, explicitLink));
        QVERIFY(hasTarget(controller, explicitLink, explicitB));
        QVERIFY(controller.confirmPendingTools(explicitOldId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(projectSavedCount, 0);
        QCOMPARE(readBytes(explicitB), explicitSentinelB);
        QCOMPARE(readBytes(explicitC), explicitSentinelC);
        QVERIFY(project->filePath().isEmpty());

        pending = controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{"path", explicitLink}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(hasTarget(controller, explicitLink, explicitC));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(projectSavedCount, 1);
        QCOMPARE(readBytes(explicitB), explicitSentinelB);
        QVERIFY(QFileInfo::exists(explicitC));
        QFile explicitSaved(explicitC);
        QVERIFY(explicitSaved.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(explicitSaved.readAll()).object().value(QStringLiteral("id")).toString(),
                 project->id());
        explicitSaved.close();
        QVERIFY(QFile::remove(explicitLink));

        const QString defaultLink = targetDir.filePath(QStringLiteral("default save link.dproj"));
        const QString defaultB = targetDir.filePath(QStringLiteral("default target B.dproj"));
        const QString defaultC = targetDir.filePath(QStringLiteral("default target C.dproj"));
        const QString defaultD = targetDir.filePath(QStringLiteral("default target D.dproj"));
        const QByteArray defaultSentinelB("default target B sentinel");
        const QByteArray defaultSentinelC("default target C sentinel");
        const QByteArray defaultSentinelD("default target D sentinel");
        QVERIFY(writeSentinel(defaultB, defaultSentinelB));
        QVERIFY(writeSentinel(defaultC, defaultSentinelC));
        QVERIFY(writeSentinel(defaultD, defaultSentinelD));
        QVERIFY(QFile::link(defaultB, defaultLink));
        project->setFilePath(defaultLink);
        pending = controller.handleToolCall(QStringLiteral("save_project"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(hasTarget(controller, defaultLink, defaultB));
        const QString omittedPathId = controller.pendingConfirmationId();
        QVERIFY(QFile::remove(defaultLink));
        QVERIFY(QFile::link(defaultC, defaultLink));
        QVERIFY(hasTarget(controller, defaultLink, defaultB));
        QVERIFY(controller.confirmPendingTools(omittedPathId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(projectSavedCount, 1);
        QCOMPARE(readBytes(defaultB), defaultSentinelB);
        QCOMPARE(readBytes(defaultC), defaultSentinelC);
        QCOMPARE(readBytes(defaultD), defaultSentinelD);

        pending = controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{"path", QString()}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(hasTarget(controller, defaultLink, defaultC));
        const QString emptyPathId = controller.pendingConfirmationId();
        QVERIFY(QFile::remove(defaultLink));
        QVERIFY(QFile::link(defaultD, defaultLink));
        QVERIFY(hasTarget(controller, defaultLink, defaultC));
        QVERIFY(controller.confirmPendingTools(emptyPathId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(projectSavedCount, 1);
        QCOMPARE(readBytes(defaultC), defaultSentinelC);
        QCOMPARE(readBytes(defaultD), defaultSentinelD);

        pending = controller.handleToolCall(QStringLiteral("save_project"), QJsonObject{{"path", QString()}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(hasTarget(controller, defaultLink, defaultD));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(projectSavedCount, 2);
        QCOMPARE(readBytes(defaultB), defaultSentinelB);
        QCOMPARE(readBytes(defaultC), defaultSentinelC);
        QFile defaultSaved(defaultD);
        QVERIFY(defaultSaved.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(defaultSaved.readAll()).object().value(QStringLiteral("id")).toString(),
                 project->id());
        defaultSaved.close();

        QVERIFY(QFile::remove(defaultLink));
        project->setFilePath(QString());
        for (const QJsonObject& params : {QJsonObject(), QJsonObject{{QStringLiteral("path"), QString()}}}) {
            toolExecuted.clear();
            pending = controller.handleToolCall(QStringLiteral("save_project"), params);
            QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
            const QJsonArray noTargetEffects = controller.pendingConfirmationPayload()
                                                   .value(QStringLiteral("context"))
                                                   .toObject()
                                                   .value(QStringLiteral("sideEffects"))
                                                   .toArray();
            QVERIFY(noTargetEffects.contains(QStringLiteral("file_write:<当前路径>")));
            QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
            QCOMPARE(toolExecuted.count(), 1);
            const QJsonObject missingPathResult = toolExecuted.last().at(0).toString() == QStringLiteral("save_project")
                                                      ? toolExecuted.last().at(1).toJsonObject()
                                                      : QJsonObject();
            QCOMPARE(missingPathResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("SAVE_FAILED"));
            QVERIFY(!QFileInfo::exists(defaultLink));
            QCOMPARE(projectSavedCount, 2);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        }

        disconnect(savedConnection);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        projects.closeProject();
#endif
    }

    void testProjectSwitchClearsOldUndoBarrier() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* oldProject = ProjectManager::instance().newProject();
        QVERIFY(oldProject);
        ModuleInstance oldInstance;
        oldInstance.id = QStringLiteral("old_project_node");
        oldInstance.moduleId = QStringLiteral("GrabImage");
        oldInstance.name = QStringLiteral("old original");
        oldProject->addModule(oldInstance);

        AgentActor* actor = controller.actor();
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("rename_module"),
                                   QJsonObject{{"instanceId", oldInstance.id}, {"name", "old edited"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("save_project"),
                                   QJsonObject{{"path", tempDir.filePath(QStringLiteral("old.json"))}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("saved"));
        QVERIFY(actor->undoBlockedByBarrier());

        Project* newProject = ProjectManager::instance().newProject();
        QVERIFY(newProject);
        QVERIFY(!actor->undoBlockedByBarrier());
        QCOMPARE(actor->undoStack()->count(), 0);
        ModuleInstance newInstance;
        newInstance.id = QStringLiteral("new_project_node");
        newInstance.moduleId = QStringLiteral("GrabImage");
        newInstance.name = QStringLiteral("new original");
        newProject->addModule(newInstance);
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("rename_module"),
                                   QJsonObject{{"instanceId", newInstance.id}, {"name", "new edited"}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("renamed"));
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(newProject->moduleById(newInstance.id)->name, QStringLiteral("new original"));
        QVERIFY(!actor->undoBlockedByBarrier());
    }

    void testAgentBatchSplitsUndoAtSaveBarrierAndPreservesFailedCalls() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("batch_undo_barrier_node");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("original");
        project->addModule(instance);

        AgentActor* actor = controller.actor();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString savePath = tempDir.filePath(QStringLiteral("batch.json"));
        const auto runBatch = [actor, &instance](const QString& path) {
            return actor->executeTools(
                {{QStringLiteral("before"), QStringLiteral("rename_module"),
                  QJsonObject{{"instanceId", instance.id}, {"name", "before"}}},
                 {QStringLiteral("save"), QStringLiteral("save_project"), QJsonObject{{"path", path}}},
                 {QStringLiteral("after"), QStringLiteral("rename_module"),
                  QJsonObject{{"instanceId", instance.id}, {"name", "after"}}}},
                QStringLiteral("barrier-test"));
        };

        QUndoStack* stack = actor->undoStack();
        const int baseCount = stack->count();
        const QJsonObject completed = runBatch(savePath);
        QCOMPARE(completed.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonArray completedResults = completed.value(QStringLiteral("results")).toArray();
        QCOMPARE(completedResults.size(), 3);
        QCOMPARE(completedResults.at(0).toObject().value(QStringLiteral("callId")).toString(),
                 QStringLiteral("before"));
        QCOMPARE(completedResults.at(1).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("save"));
        QCOMPARE(completedResults.at(2).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("after"));
        QCOMPARE(stack->count(), baseCount + 2);
        QFile savedFile(savePath);
        QVERIFY(savedFile.open(QIODevice::ReadOnly));
        const QJsonArray savedModules =
            QJsonDocument::fromJson(savedFile.readAll()).object().value(QStringLiteral("modules")).toArray();
        QCOMPARE(savedModules.size(), 1);
        QCOMPARE(savedModules.first().toObject().value(QStringLiteral("name")).toString(), QStringLiteral("before"));
        QVERIFY(savedModules.first().toObject().value(QStringLiteral("name")).toString() != QStringLiteral("after"));
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("before"));
        QVERIFY(!controller.undoLastAgentAction());
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("before"));

        ProjectManager::instance().closeProject();
        project = ProjectManager::instance().newProject();
        QVERIFY(project);
        instance.name = QStringLiteral("original");
        project->addModule(instance);
        const int failedBase = stack->count();
        const QJsonObject failed = runBatch(tempDir.path()); // 已存在的目录令保存实现失败。
        QCOMPARE(failed.value(QStringLiteral("status")).toString(), QStringLiteral("partial_failed"));
        const QJsonArray failedResults = failed.value(QStringLiteral("results")).toArray();
        QCOMPARE(failedResults.size(), 3);
        QCOMPARE(failedResults.at(0).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("before"));
        QCOMPARE(failedResults.at(1).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("save"));
        QCOMPARE(failedResults.at(2).toObject().value(QStringLiteral("callId")).toString(), QStringLiteral("after"));
        QCOMPARE(failedResults.at(2)
                     .toObject()
                     .value(QStringLiteral("result"))
                     .toObject()
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(stack->count(), failedBase + 1); // before 入栈；after 未执行。
        QVERIFY(actor->undoBlockedByBarrier());
        const int failedIndex = stack->index();
        QVERIFY(!controller.undoLastAgentAction());
        QCOMPARE(stack->index(), failedIndex);
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("before"));
    }

    void testInitialStateIsIdle() {
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testG4MoveModuleControllerGateAndUndoAudit() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        for (const QString& id : {QStringLiteral("first"), QStringLiteral("second")}) {
            ModuleInstance instance;
            instance.id = id;
            instance.moduleId = QStringLiteral("GrabImage");
            instance.name = id;
            project->addModule(instance);
        }
        const QJsonObject request{{"instanceId", "second"}, {"index", 0}};
        const int undoCount = controller.actor()->undoStack()->count();

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.handleToolCall("move_module", request).value("errorCode").toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(project->modules().first().id, QStringLiteral("first"));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.handleToolCall("move_module", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QCOMPARE(project->modules().first().id, QStringLiteral("first"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QCOMPARE(controller.handleToolCall("move_module", request).value("status").toString(), QStringLiteral("moved"));
        QCOMPARE(project->modules().first().id, QStringLiteral("second"));
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount + 1);
        QVERIFY(!logSpy.isEmpty());
        const AgentActionLogEntry moved = logSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(moved.action, QStringLiteral("move_module"));
        QCOMPARE(moved.result, QStringLiteral("success"));
        QVERIFY(moved.undoable);

        QCOMPARE(controller.handleToolCall("move_module", request).value("status").toString(), QStringLiteral("no_op"));
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount + 1);
        const AgentActionLogEntry noOp = logSpy.last().at(0).value<AgentActionLogEntry>();
        QVERIFY(!noOp.undoable);
        QCOMPARE(controller.handleToolCall("move_module", QJsonObject{{"instanceId", "second"}, {"index", 2}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        const AgentActionLogEntry invalid = logSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(invalid.result, QStringLiteral("error"));
        QVERIFY(!invalid.undoable);
        ProjectManager::instance().closeProject();
    }

    void testG4SetModulePositionControllerGateAndUndoAudit() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("positioned");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.posX = 5;
        instance.posY = 15;
        project->addModule(instance);
        project->setModified(false);

        const QJsonObject request{{"instanceId", instance.id}, {"x", -20}, {"y", 40}};
        const int undoCount = controller.actor()->undoStack()->count();
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.handleToolCall("set_module_position", request).value("errorCode").toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(project->moduleById(instance.id)->posX, 5);
        QVERIFY(!project->isModified());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.handleToolCall("set_module_position", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QCOMPARE(project->moduleById(instance.id)->posX, 5);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount);

        QCOMPARE(controller.handleToolCall("set_module_position", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(controller.confirmPendingTools(approvedId));
        QCOMPARE(project->moduleById(instance.id)->posX, -20);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount + 1);
        QVERIFY(!controller.confirmPendingTools(approvedId));
        QCOMPARE(project->moduleById(instance.id)->posX, -20);
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount + 1);
        controller.actor()->undoStack()->undo();
        QCOMPARE(project->moduleById(instance.id)->posX, 5);
        QCOMPARE(project->moduleById(instance.id)->posY, 15);
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount + 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QCOMPARE(controller.handleToolCall("set_module_position", request).value("status").toString(),
                 QStringLiteral("updated"));
        QCOMPARE(project->moduleById(instance.id)->posX, -20);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount + 1);
        QVERIFY(!logSpy.isEmpty());
        const AgentActionLogEntry updated = logSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(updated.action, QStringLiteral("set_module_position"));
        QCOMPARE(updated.result, QStringLiteral("success"));
        QVERIFY(updated.undoable);

        QCOMPARE(controller.handleToolCall("set_module_position", request).value("status").toString(),
                 QStringLiteral("no_op"));
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount + 1);
        const AgentActionLogEntry noOp = logSpy.last().at(0).value<AgentActionLogEntry>();
        QVERIFY(!noOp.undoable);
        ProjectManager::instance().closeProject();
    }

    void testG4ExpiredAgentPositionUndoIsAuditedAsExpired() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance instance;
        instance.id = QStringLiteral("stale_position");
        instance.moduleId = QStringLiteral("GrabImage");
        project->addModule(instance);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject request{{"instanceId", instance.id}, {"x", 10}, {"y", 20}};
        QCOMPARE(controller.handleToolCall("set_module_position", request).value("status").toString(),
                 QStringLiteral("updated"));
        ModuleInstance guiUpdated = *project->moduleById(instance.id);
        guiUpdated.posX = 30;
        guiUpdated.posY = 40;
        project->updateModule(instance.id, guiUpdated, Project::PositionChangeSource::Gui);

        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QVERIFY(!controller.undoLastAgentAction());
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        QVERIFY(!logSpy.isEmpty());
        const AgentActionLogEntry expired = logSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(expired.action, QStringLiteral("undo"));
        QCOMPARE(expired.result, QStringLiteral("expired"));
        const int undoCount = controller.actor()->undoStack()->count();
        const int staleIndex = controller.actor()->undoStack()->index();
        controller.actor()->undoStack()->redo();
        QCOMPARE(controller.actor()->undoStack()->index(), staleIndex + 1);
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        QVERIFY(!controller.undoLastAgentAction());
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount);
        QCOMPARE(controller.actor()->undoStack()->index(), staleIndex);
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        const AgentActionLogEntry repeatedExpired = logSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(repeatedExpired.action, QStringLiteral("undo"));
        QCOMPARE(repeatedExpired.result, QStringLiteral("expired"));
        controller.actor()->undoStack()->redo();
        QCOMPARE(controller.actor()->undoStack()->index(), staleIndex + 1);
        QCOMPARE(project->moduleById(instance.id)->posX, 30);
        QCOMPARE(project->moduleById(instance.id)->posY, 40);
        ProjectManager::instance().closeProject();
    }

    void testG4RenameModuleControllerGateAndUndoAudit() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("stable_id");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.name = QStringLiteral("Before");
        project->addModule(instance);
        const QJsonObject request{{"instanceId", instance.id}, {"name", "After"}};
        const int undoCount = controller.actor()->undoStack()->count();

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.handleToolCall("rename_module", request).value("errorCode").toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.handleToolCall("rename_module", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("Before"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(controller.actor()->undoStack()->count(), undoCount);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QCOMPARE(controller.handleToolCall("rename_module", request).value("status").toString(),
                 QStringLiteral("renamed"));
        QCOMPARE(project->moduleById(instance.id)->name, QStringLiteral("After"));
        QVERIFY(logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(controller.handleToolCall("rename_module", request).value("status").toString(),
                 QStringLiteral("no_op"));
        QVERIFY(!logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(controller.handleToolCall("rename_module", QJsonObject{{"instanceId", instance.id}, {"name", 7}})
                     .value("errorCode")
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QVERIFY(!logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        ProjectManager::instance().closeProject();
    }

    void testG4SetModuleEnabledControllerGateAndAudit() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("toggle_id");
        instance.moduleId = QStringLiteral("GrabImage");
        project->addModule(instance);
        const QJsonObject request{{"instanceId", instance.id}, {"enabled", false}};

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.handleToolCall("set_module_enabled", request).value("errorCode").toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(project->moduleById(instance.id)->enabled);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.handleToolCall("set_module_enabled", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(project->moduleById(instance.id)->enabled);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QCOMPARE(controller.handleToolCall("set_module_enabled", request).value("status").toString(),
                 QStringLiteral("updated"));
        QVERIFY(!project->moduleById(instance.id)->enabled);
        QVERIFY(logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(controller.handleToolCall("set_module_enabled", request).value("status").toString(),
                 QStringLiteral("no_op"));
        QVERIFY(!logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(
            controller
                .handleToolCall("set_module_enabled", QJsonObject{{"instanceId", instance.id}, {"enabled", "false"}})
                .value("errorCode")
                .toString(),
            QStringLiteral("INVALID_PARAM"));
        QVERIFY(!logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        ProjectManager::instance().closeProject();
    }

    void testG4SetBreakpointControllerGateAndAudit() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("breakpoint_id");
        instance.moduleId = QStringLiteral("GrabImage");
        project->addModule(instance);
        const QJsonObject request{{"instanceId", instance.id}, {"enabled", true}};

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.handleToolCall("set_breakpoint", request).value("errorCode").toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(!project->moduleById(instance.id)->breakpoint);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.handleToolCall("set_breakpoint", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(!project->moduleById(instance.id)->breakpoint);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QCOMPARE(controller.handleToolCall("set_breakpoint", request).value("status").toString(),
                 QStringLiteral("updated"));
        QVERIFY(project->moduleById(instance.id)->breakpoint);
        QVERIFY(logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(controller.handleToolCall("set_breakpoint", request).value("status").toString(),
                 QStringLiteral("no_op"));
        QVERIFY(!logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(
            controller.handleToolCall("set_breakpoint", QJsonObject{{"instanceId", instance.id}, {"enabled", "true"}})
                .value("errorCode")
                .toString(),
            QStringLiteral("INVALID_PARAM"));
        QVERIFY(!logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        ProjectManager::instance().closeProject();
    }

    void testG4SetModuleNoteControllerGateAndAudit() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance instance;
        instance.id = QStringLiteral("note_id");
        instance.moduleId = QStringLiteral("GrabImage");
        instance.note = QStringLiteral("before");
        project->addModule(instance);
        const QJsonObject request{{"instanceId", instance.id}, {"note", "after"}};

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.handleToolCall("set_module_note", request).value("errorCode").toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("before"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.handleToolCall("set_module_note", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("before"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        QCOMPARE(controller.handleToolCall("set_module_note", request).value("status").toString(),
                 QStringLiteral("updated"));
        QCOMPARE(project->moduleById(instance.id)->note, QStringLiteral("after"));
        QVERIFY(logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        QCOMPARE(controller.handleToolCall("set_module_note", request).value("status").toString(),
                 QStringLiteral("no_op"));
        QVERIFY(!logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        const QJsonObject clear{{"instanceId", instance.id}, {"note", QString()}};
        QCOMPARE(controller.handleToolCall("set_module_note", clear).value("status").toString(),
                 QStringLiteral("updated"));
        QVERIFY(project->moduleById(instance.id)->note.isEmpty());
        QVERIFY(logSpy.last().at(0).value<AgentActionLogEntry>().undoable);
        ProjectManager::instance().closeProject();
    }

    void testG4StepOnceRequiresExecutionConfirmation() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        QVERIFY(ProjectManager::instance().newProject() != nullptr);
        const QJsonObject request;

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.handleToolCall("step_once", request).value("errorCode").toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.handleToolCall("step_once", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QCOMPARE(controller.handleToolCall("step_once", request).value("status").toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        ProjectManager::instance().closeProject();
    }

    void testAutopilotResumeFlowConfirmationBindsPausedSessionAndProject() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        QVERIFY(PluginManager::instance().initialize());
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();

        QTemporaryDir oldTarget;
        QTemporaryDir currentTarget;
        QVERIFY(oldTarget.isValid());
        QVERIFY(currentTarget.isValid());
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance folder;
        folder.id = QStringLiteral("resume_folder_target");
        folder.moduleId = QStringLiteral("Folder");
        folder.name = QStringLiteral("Resume folder side-effect fixture");
        folder.params.insert(QStringLiteral("operation"), QStringLiteral("Delete"));
        folder.params.insert(QStringLiteral("folderPath"), oldTarget.path());
        project->addModule(folder);
        ModuleInstance plcWrite;
        plcWrite.id = QStringLiteral("resume_plc_write_target");
        plcWrite.moduleId = QStringLiteral("PLCWrite");
        plcWrite.name = QStringLiteral("Resume PLC target fixture");
        plcWrite.params.insert(QStringLiteral("ipAddress"), QStringLiteral("192.0.2.44"));
        plcWrite.params.insert(QStringLiteral("port"), 1502);
        plcWrite.params.insert(QStringLiteral("slaveId"), 7);
        plcWrite.params.insert(QStringLiteral("startAddress"), 4660);
        project->addModule(plcWrite);

        std::atomic<int> calls{0};
        const auto loadProbeProject = [&]() {
            return engine.loadProject(project,
                                      [&calls](const ModuleInstance&) { return new AgentBusyProbeModule(&calls); });
        };
        QVERIFY(loadProbeProject());
        QString firstSession;
        QVERIFY(engine.startCycle(&firstSession, 1000, false, false, false));
        engine.pause();
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(calls.load(std::memory_order_acquire), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(
            controller.handleToolCall(QStringLiteral("resume_flow"), {}).value(QStringLiteral("errorCode")).toString(),
            QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(calls.load(std::memory_order_acquire), 0);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QJsonObject advisorPending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(advisorPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(calls.load(std::memory_order_acquire), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(project->setModuleParam(folder.id, QStringLiteral("folderPath"), currentTarget.path()));
        const QJsonObject staleConfiguration = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(staleConfiguration.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("FLOW_CONFIGURATION_CHANGED"));
        QVERIFY(!controller.hasPendingTools());
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(calls.load(std::memory_order_acquire), 0);

        engine.stop();
        QVERIFY(loadProbeProject());
        QString sessionId;
        QVERIFY(engine.startCycle(&sessionId, 100, false, false, false));
        engine.pause();
        QCOMPARE(engine.state(), RunState::Paused);

        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject context =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("context")).toObject();
        const QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(std::any_of(effects.cbegin(), effects.cend(), [](const QJsonValue& effect) {
            return effect.toString() ==
                   QLatin1String("execution:resume_paused_session_may_touch_files_devices_network");
        }));
        const QString folderEffect = QStringLiteral("folder_delete:%1@%2").arg(folder.id, currentTarget.path());
        const bool hasFolderEffect =
            std::any_of(effects.cbegin(), effects.cend(),
                        [&folderEffect](const QJsonValue& effect) { return effect.toString() == folderEffect; });
        const QString missingFolderMessage =
            QStringLiteral("resume confirmation omitted configured Folder target: %1")
                .arg(QString::fromUtf8(QJsonDocument(effects).toJson(QJsonDocument::Compact)));
        QVERIFY2(hasFolderEffect, qPrintable(missingFolderMessage));
        const QString plcEffect =
            QStringLiteral("plc_write:%1@192.0.2.44:1502 slave=7 address=4660 quantity=<runtime> "
                           "end=<runtime> range=unresolved-at-confirmation valueSource=unknown:<unset>")
                .arg(plcWrite.id);
        const bool hasPlcEffect = std::any_of(effects.cbegin(), effects.cend(), [&plcEffect](const QJsonValue& effect) {
            return effect.toString() == plcEffect;
        });
        QVERIFY2(hasPlcEffect, qPrintable(QString::fromUtf8(QJsonDocument(effects).toJson(QJsonDocument::Compact))));
        const QJsonObject executionSnapshot = context.value(QStringLiteral("executionSnapshot")).toObject();
        QCOMPARE(executionSnapshot.value(QStringLiteral("sessionId")).toString(), sessionId);
        QCOMPARE(executionSnapshot.value(QStringLiteral("state")).toInt(), static_cast<int>(RunState::Paused));
        QVERIFY(!executionSnapshot.value(QStringLiteral("pauseEpoch")).toString().isEmpty());
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(calls.load(std::memory_order_acquire), 0);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        const QMetaObject::Connection stopOnFirstResume = connect(
            &engine, &RunEngine::moduleFinished, &engine, [&engine](const QString&, bool, int) { engine.stop(); });
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QVERIFY(!controller.confirmPendingTools(confirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(calls.load(std::memory_order_acquire), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Stopped, 2000);
        QCOMPARE(calls.load(std::memory_order_acquire), 1);
        disconnect(stopOnFirstResume);

        // pending 窗口中的 session 被停止后，迟到批准只记 expired，不恢复任何会话。
        QVERIFY(loadProbeProject());
        QString expiredSession;
        QVERIFY(engine.startCycle(&expiredSession, 1000, false, false, false));
        engine.pause();
        const int callsBeforeExpired = calls.load(std::memory_order_acquire);
        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        engine.stop();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(engine.state(), RunState::Stopped);
        QCOMPARE(calls.load(std::memory_order_acquire), callsBeforeExpired);
        QVERIFY(std::any_of(auditSpy.cbegin(), auditSpy.cend(), [](const QList<QVariant>& args) {
            return args.first().value<AgentActionLogEntry>().action == QLatin1String("resume_flow") &&
                   args.first().value<AgentActionLogEntry>().result == QLatin1String("expired");
        }));

        engine.stop();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testResumeConfirmationExpiresAtNextBreakpointInSameRunOnce() {
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);

        ModuleInstance breakpointA;
        breakpointA.id = QStringLiteral("resume_controller_breakpoint_a");
        breakpointA.moduleId = QStringLiteral("AgentResumeProbeA");
        breakpointA.breakpoint = true;
        project->addModule(breakpointA);
        ModuleInstance breakpointB;
        breakpointB.id = QStringLiteral("resume_controller_breakpoint_b");
        breakpointB.moduleId = QStringLiteral("AgentResumeProbeB");
        breakpointB.breakpoint = true;
        project->addModule(breakpointB);

        std::atomic<int> callsA{0};
        std::atomic<int> callsB{0};
        QVERIFY(engine.loadProject(project, [&callsA, &callsB](const ModuleInstance& instance) {
            return new AgentBusyProbeModule(instance.id == QLatin1String("resume_controller_breakpoint_a") ? &callsA
                                                                                                           : &callsB);
        }));
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(callsA.load(std::memory_order_acquire), 0);
        QCOMPARE(callsB.load(std::memory_order_acquire), 0);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString oldConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!oldConfirmationId.isEmpty());

        // Another authorized path resumes breakpoint A and reaches B in the same RunOnce.
        engine.resume();
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(callsA.load(std::memory_order_acquire), 1);
        QCOMPARE(callsB.load(std::memory_order_acquire), 0);

        QVERIFY(controller.confirmPendingTools(oldConfirmationId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(callsA.load(std::memory_order_acquire), 1);
        QCOMPARE(callsB.load(std::memory_order_acquire), 0);
        QVERIFY(!auditSpy.isEmpty());
        const AgentActionLogEntry audit = auditSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(audit.action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.result, QStringLiteral("expired"));

        engine.stop();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testDestroyedLLMClientIsCleared() {
        AgentController controller;
        {
            MockLLMClient client;
            controller.setLLMClient(&client);
            QCOMPARE(controller.llmClient(), static_cast<ILLMClient*>(&client));
        }
        QVERIFY(controller.llmClient() == nullptr);
        controller.setLLMClient(m_mock);
        QCOMPARE(controller.llmClient(), static_cast<ILLMClient*>(m_mock));
        controller.setLLMClient(nullptr);
    }

    void testSendMessageTransitionsToThinking() {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->lastCtx.messages.size(), 1);
        QCOMPARE(m_mock->lastCtx.messages.first().role, QString("user"));
    }

    void testPlainTextResponseReturnsToIdle() {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        AgentResponse resp;
        resp.success = true;
        resp.content = "Hi there!";
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testErrorReturnsToIdle() {
        AgentController::instance().sendUserMessage("Hello");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        m_mock->emitError("Network timeout");

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testSystemPromptUpdateAffectsNextRequest() {
        AgentController& controller = AgentController::instance();
        const QString originalPrompt = controller.systemPrompt();
        controller.setSystemPrompt(QStringLiteral("Use the current calibration policy."));

        controller.sendUserMessage(QStringLiteral("Check the project."));

        QVERIFY(m_mock->lastCtx.systemPrompt.startsWith(QStringLiteral("Use the current calibration policy.")));
        controller.setSystemPrompt(originalPrompt);
    }

    void testImageAttachmentReachesModelPayload() {
        QPixmap image(2, 2);
        image.fill(Qt::red);

        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Inspect this image."), {image});

        QCOMPARE(m_mock->lastCtx.messages.size(), 1);
        const AgentMessage& message = m_mock->lastCtx.messages.constFirst();
        QCOMPARE(message.role, QStringLiteral("user"));
        QCOMPARE(message.content, QStringLiteral("Inspect this image."));
        QCOMPARE(message.images.size(), 1);
        QVERIFY(!message.images.constFirst().data.isEmpty());
        QCOMPARE(message.images.constFirst().mimeType, QStringLiteral("image/png"));

        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        const QJsonArray content = messages.last().toObject().value(QStringLiteral("content")).toArray();
        QCOMPARE(content.size(), 2);
        QCOMPARE(content.last().toObject().value(QStringLiteral("type")).toString(), QStringLiteral("image_url"));
        QVERIFY(content.last()
                    .toObject()
                    .value(QStringLiteral("image_url"))
                    .toObject()
                    .value(QStringLiteral("url"))
                    .toString()
                    .startsWith(QStringLiteral("data:image/png;base64,")));
    }

    void testInvalidImageAttachmentFailsClosed() {
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);

        AgentController::instance().sendUserMessageWithImages(QString(), {QPixmap()});

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(m_mock->lastCtx.messages.isEmpty());
    }

    void testImageAttachmentCountIsBounded() {
        QList<QPixmap> images;
        for (int i = 0; i < 5; ++i) {
            QPixmap image(1, 1);
            image.fill(Qt::red);
            images.append(image);
        }
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);

        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Too many"), images);

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(m_mock->lastCtx.messages.isEmpty());
    }

    void testImageAttachmentRequiresModelCapability() {
        m_mock->imageInputSupported = false;
        QPixmap image(1, 1);
        image.fill(Qt::red);
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);

        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Inspect"), {image});

        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(m_mock->lastCtx.messages.isEmpty());
    }

    void testOpenAIRequestDefaultsToCompatiblePayload() {
        OpenAILLMClient client;
        AgentConversation context;
        AgentMessage message;
        message.role = QStringLiteral("user");
        message.content = QStringLiteral("Hello");
        context.messages.append(message);

        const QJsonObject defaultPayload = client.buildRequestBody(context, {});
        QVERIFY(!defaultPayload.contains(QStringLiteral("thinking")));
        QVERIFY(!defaultPayload.contains(QStringLiteral("reasoning_effort")));

        client.setThinkingEnabled(true);
        client.setReasoningEffort(QStringLiteral("high"));
        const QJsonObject thinkingPayload = client.buildRequestBody(context, {});
        QCOMPARE(thinkingPayload.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("enabled"));
        QCOMPARE(thinkingPayload.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));
    }

    void testEndpointNormalizationMatchesConnectionTest() {
        QCOMPARE(OpenAILLMClient::normalizeEndpoint(QStringLiteral("api.example.test")),
                 QStringLiteral("https://api.example.test/v1/chat/completions"));
        QCOMPARE(OpenAILLMClient::normalizeEndpoint(QStringLiteral("http://localhost:8080/custom")),
                 QStringLiteral("http://localhost:8080/custom"));
    }

    void testDeepSeekFlashImagePayloadAndBudget() {
        OpenAILLMClient client;
        client.setModel(QStringLiteral("deepseek-flash"));
        client.setThinkingEnabled(true);
        client.setReasoningEffort(QStringLiteral("high"));
        client.setImageDetail(QStringLiteral("low"));

        AgentConversation context;
        AgentMessage message;
        message.role = QStringLiteral("user");
        AgentImageAttachment image;
        image.data = QByteArray(3 * 1024 * 1024, '\0');
        image.mimeType = QStringLiteral("image/jpeg");
        message.images.append(image);
        context.messages.append(message);

        AgentController& controller = AgentController::instance();
        controller.setLLMClient(&client);
        const qint64 contextEstimate = controller.estimateTokensFor(context.messages, QString(), false);
        controller.setLLMClient(m_mock);

        const QJsonObject body = client.buildRequestBody(context, {});
        QVERIFY(!body.contains(QStringLiteral("temperature")));
        QCOMPARE(body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("enabled"));
        QCOMPARE(body.value(QStringLiteral("reasoning_effort")).toString(), QStringLiteral("high"));
        const QJsonObject imageUrl = body.value(QStringLiteral("messages"))
                                         .toArray()
                                         .last()
                                         .toObject()
                                         .value(QStringLiteral("content"))
                                         .toArray()
                                         .last()
                                         .toObject()
                                         .value(QStringLiteral("image_url"))
                                         .toObject();
        QCOMPARE(imageUrl.value(QStringLiteral("detail")).toString(), QStringLiteral("low"));
        QVERIFY(imageUrl.value(QStringLiteral("url")).toString().startsWith(QStringLiteral("data:image/jpeg;base64,")));
        QCOMPARE(client.estimateImageTokens(image), qint64(1024));
        QCOMPARE(client.preferredImageLongEdge(), 512);
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("deepseek-flash")));
        QVERIFY(OpenAILLMClient::isDeepSeekFlashModel(QStringLiteral("deepseek-v4-flash-vision-exp")));

        QCOMPARE(contextEstimate, qint64(1044));

        client.setThinkingEnabled(false);
        const QJsonObject disabledBody = client.buildRequestBody(context, {});
        QCOMPARE(disabledBody.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("disabled"));
        QVERIFY(disabledBody.contains(QStringLiteral("temperature")));
        QVERIFY(!disabledBody.contains(QStringLiteral("reasoning_effort")));

        client.setModel(QStringLiteral("gpt-4o"));
        const QJsonObject genericBody = client.buildRequestBody(context, {});
        const QJsonObject genericImageUrl = genericBody.value(QStringLiteral("messages"))
                                                .toArray()
                                                .last()
                                                .toObject()
                                                .value(QStringLiteral("content"))
                                                .toArray()
                                                .last()
                                                .toObject()
                                                .value(QStringLiteral("image_url"))
                                                .toObject();
        QVERIFY(!genericImageUrl.contains(QStringLiteral("detail")));
        QCOMPARE(client.preferredImageLongEdge(), 1280);
        QCOMPARE(client.estimateImageTokens(image), qint64((image.data.size() + 2) / 3));
    }

    void testDeepSeekToolImageKeepsIndustrialResolution() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString imagePath = tempDir.filePath(QStringLiteral("inspection.png"));
        QImage source(1800, 1200, QImage::Format_RGB32);
        source.fill(QColor(220, 210, 180));
        QVERIFY(source.save(imagePath));

        AgentController& controller = AgentController::instance();
        OpenAILLMClient client;
        client.setModel(QStringLiteral("deepseek-flash"));
        controller.setLLMClient(&client);
        const QJsonArray results{
            QJsonObject{{QStringLiteral("tool"), QStringLiteral("capture_viewport_image")},
                        {QStringLiteral("result"), QJsonObject{{QStringLiteral("imagePath"), imagePath}}}}};
        controller.attachToolImages(results);
        const QList<AgentImageAttachment> attachments = controller.m_conversationHistory.isEmpty()
                                                            ? QList<AgentImageAttachment>()
                                                            : controller.m_conversationHistory.constLast().images;
        controller.clearConversation();
        controller.setLLMClient(m_mock);
        QCOMPARE(attachments.size(), 1);
        const QImage transmitted = QImage::fromData(attachments.constFirst().data);
        QCOMPARE(transmitted.size(), QSize(1800, 1200));
    }

    // ---------- Advisor 模式确认流程 ----------

    void testAdvisorToolCallsTransitionToConfirming() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Add module");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy confirmSpy(&AgentController::instance(), &AgentController::toolsPendingConfirmation);
        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        AgentResponse resp;
        resp.success = true;
        resp.content = "I'll add a module.";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "add_module";
        tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
        resp.toolCalls.append(tc);

        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
        QCOMPARE(confirmSpy.count(), 1);
        QCOMPARE(responseSpy.count(), 1);
        QVERIFY(!AgentController::instance().pendingToolCalls().isEmpty());
    }

    void testSamImageDigestOnlySnapshotDriftExpiresConfirmation() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);

        QObject owner;
        QString imageDigest = QStringLiteral("approved-image-digest");
        int editExecutions = 0;
        controller.registerDataSourceViewHandler(
            &owner, [&imageDigest, &editExecutions](const QString& toolName, const QJsonObject& params) {
                if (toolName == QLatin1String("__sam_mask_edit_preflight"))
                    return QJsonObject();
                if (toolName == QLatin1String("__sam_mask_edit_confirmation_snapshot")) {
                    return QJsonObject{{QStringLiteral("available"), true},
                                       {QStringLiteral("ownerIdentity"), QStringLiteral("owner-identity")},
                                       {QStringLiteral("dialogIdentity"), QStringLiteral("dialog-identity")},
                                       {QStringLiteral("imageWidth"), 48},
                                       {QStringLiteral("imageHeight"), 40},
                                       {QStringLiteral("imageDigest"), imageDigest},
                                       {QStringLiteral("sessionDigest"), QStringLiteral("stable-session-digest")},
                                       {QStringLiteral("params"), params}};
                }
                if (toolName == QLatin1String("edit_active_annotation_masks")) {
                    ++editExecutions;
                    return QJsonObject{{QStringLiteral("status"), QStringLiteral("updated")}};
                }
                return QJsonObject{{QStringLiteral("error"), QStringLiteral("unexpected tool")},
                                   {QStringLiteral("errorCode"), QStringLiteral("UNKNOWN_TOOL")}};
            });

        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const int undoIndex = controller.actor()->undoStack()->index();
        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("edit_active_annotation_masks"),
                                      QJsonObject{{QStringLiteral("operation"), QStringLiteral("dilate")},
                                                  {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("object-1")}},
                                                  {QStringLiteral("radiusPx"), 1}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("confirmationId")).toString();
        imageDigest = QStringLiteral("changed-image-digest"); // no other snapshot field changes
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(editExecutions, 0);
        QCOMPARE(controller.actor()->undoStack()->index(), undoIndex);
        QVERIFY(!audit.isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        controller.unregisterDataSourceViewHandler(&owner);
    }

    void testChatClearRuntimeLogsProducesOnlyPostClearControllerBookkeeping() {
        AgentController& controller = AgentController::instance();
        Logger& logger = Logger::instance();
        const LogLevel previousMinimum = logger.minLevel();
        const bool previousFileLogging = logger.isLogToFileEnabled();
        logger.setMinLevel(LogLevel::Debug);
        logger.setLogToFile(false);
        logger.clearLogs();
        logger.info(QStringLiteral("slice16-chat-old-log"), QStringLiteral("Slice16Test"));

        QObject owner;
        int clearedAtHandler = -1;
        controller.registerDataSourceViewHandler(
            &owner, [&logger, &clearedAtHandler](const QString& toolName, const QJsonObject&) {
                if (toolName != QLatin1String("clear_runtime_logs"))
                    return QJsonObject{{QStringLiteral("error"), QStringLiteral("unexpected tool")},
                                       {QStringLiteral("errorCode"), QStringLiteral("UNKNOWN_TOOL")}};
                clearedAtHandler = logger.logs().size();
                logger.clearLogs();
                return QJsonObject{{QStringLiteral("status"), QStringLiteral("completed")},
                                   {QStringLiteral("clearedLoggerEntries"), clearedAtHandler},
                                   {QStringLiteral("clearedRuntimeRows"), 0}};
            });

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Clear runtime logs with confirmation."));
        AgentResponse response;
        response.success = true;
        response.content = QStringLiteral("Please confirm clearing runtime logs.");
        response.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("slice16-chat-clear-loop")},
                                              {QStringLiteral("type"), QStringLiteral("function")},
                                              {QStringLiteral("name"), QStringLiteral("clear_runtime_logs")},
                                              {QStringLiteral("arguments"), QJsonObject{}}});
        m_mock->emitResponse(response);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(clearedAtHandler, 1);
        const QList<LogEntry> remainingLogs = logger.logs();
        QCOMPARE(remainingLogs.size(), 1);
        QCOMPARE(remainingLogs.first().message, QStringLiteral("[AgentLoop] Executed 1 tool(s)"));
        QCOMPARE(remainingLogs.first().level, LogLevel::Debug);

        controller.unregisterDataSourceViewHandler(&owner);
        controller.clearConversation();
        logger.clearLogs();
        logger.setMinLevel(previousMinimum);
        logger.setLogToFile(previousFileLogging);
    }

    void testConfirmToolsTransitionsToExecutingAndThenThinking() {
        // 设置到 Confirming 状态
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Create project");

        AgentResponse resp;
        resp.success = true;
        resp.content = "Creating...";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "create_project";
        tc["arguments"] = QJsonObject{{"name", "AgentProject"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        // 确认 — QueuedConnection 需要 processEvents 处理入队的 lambda
        // 单次 processEvents 可能不足以处理完所有内部事件，需多次调用
        AgentController::instance().confirmPendingTools(AgentController::instance().pendingConfirmationId());
        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();

        // extendAgentLoop 执行工具后会发送新的 LLM 请求（mock 不响应）
        // 状态应为 Thinking（等待第二轮 LLM 响应）
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    void testRejectToolsReturnsToIdle() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Add module");

        AgentResponse resp;
        resp.success = true;
        resp.content = "Adding...";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "add_module";
        tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        // QueuedConnection 需要 processEvents 处理入队的 lambda
        AgentController::instance().rejectPendingTools(AgentController::instance().pendingConfirmationId());
        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(responseSpy.count(), 1);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    // 回归：用户拒绝确认批次后，assistant 的 tool_calls 必须逐 call 收到 tool 结果，
    // 否则下一次请求会触发 OpenAI 协议错误（insufficient tool messages）。
    void testRejectedToolCallsLeaveNoOrphanedCalls() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        ctrl.sendUserMessage("Add modules");

        AgentResponse resp;
        resp.success = true;
        resp.content = "Adding...";
        for (const QString& id : {QStringLiteral("call_1"), QStringLiteral("call_2")}) {
            QJsonObject tc;
            tc["id"] = id;
            tc["type"] = "function";
            tc["name"] = "add_module";
            tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
            resp.toolCalls.append(tc);
        }
        m_mock->emitResponse(resp);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Confirming);

        QVERIFY(ctrl.rejectPendingTools(ctrl.pendingConfirmationId()));

        // 用户下一条消息触发新的请求；检查发出去的 messages 协议合法性
        ctrl.sendUserMessage("Never mind");
        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        for (int i = 0; i < messages.size(); ++i) {
            const QJsonObject msg = messages.at(i).toObject();
            const QJsonArray calls = msg.value(QStringLiteral("tool_calls")).toArray();
            for (const QJsonValue& v : calls) {
                const QString callId = v.toObject().value(QStringLiteral("id")).toString();
                bool answered = false;
                for (int j = i + 1; j < messages.size(); ++j) {
                    const QJsonObject next = messages.at(j).toObject();
                    if (next.value(QStringLiteral("role")).toString() == QLatin1String("tool") &&
                        next.value(QStringLiteral("tool_call_id")).toString() == callId) {
                        answered = true;
                        break;
                    }
                    if (next.value(QStringLiteral("role")).toString() == QLatin1String("assistant"))
                        break;
                }
                QVERIFY2(answered, qPrintable(QStringLiteral("tool_call %1 has no tool response").arg(callId)));
            }
        }
    }

    // Bridge 发起的确认批次没有 assistant tool_calls 消息，拒绝时不得补 tool 消息，
    // 否则会制造孤立的 tool 响应（同样违反 OpenAI 协议）。
    void testBridgeRejectedToolCallsDoNotAppendToolMessages() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject pending =
            ctrl.handleToolCall("create_project", QJsonObject{{"name", "bridge_reject"}, {"onUnsaved", "discard"}});
        QCOMPARE(pending.value("status").toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(ctrl.rejectPendingTools(ctrl.pendingConfirmationId()));
        QCOMPARE(ctrl.conversationHistorySize(), 0);
    }

    void testAutopilotCreateProjectImplicitSaveRequiresConfirmationAndTracksSymlinkTarget() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString oldFileA = tempDir.filePath(QStringLiteral("old-a.dproj"));
        const QString oldFileB = tempDir.filePath(QStringLiteral("old-b.dproj"));
        const QString oldLink = tempDir.filePath(QStringLiteral("current.dproj"));
        const QString replacementLink = tempDir.filePath(QStringLiteral("current-replacement.dproj"));
        Project* original = projects.newProject();
        QVERIFY(original);
        original->setName(QStringLiteral("original-v3"));
        QVERIFY(projects.saveProject(oldFileA));

        Project targetB;
        targetB.setName(QStringLiteral("sentinel-b"));
        QVERIFY(targetB.save(oldFileB));
        QFile fileA(oldFileA);
        QFile fileB(oldFileB);
        QVERIFY(fileA.open(QIODevice::ReadOnly));
        const QByteArray oldBytesA = fileA.readAll();
        fileA.close();
        QVERIFY(fileB.open(QIODevice::ReadOnly));
        const QByteArray oldBytesB = fileB.readAll();
        fileB.close();
        QCOMPARE(QJsonDocument::fromJson(oldBytesA).object().value(QStringLiteral("version")).toString(),
                 QStringLiteral("3.0"));
        QCOMPARE(QJsonDocument::fromJson(oldBytesB).object().value(QStringLiteral("version")).toString(),
                 QStringLiteral("3.0"));

        if (!QFile::link(oldFileA, oldLink) || !QFileInfo(oldLink).isSymLink())
            QSKIP("POSIX symbolic links are unavailable in this environment");
        original->setFilePath(oldLink);
        original->setName(QStringLiteral("edited-before-switch"));
        QVERIFY(original->isModified());

        qRegisterMetaType<Project*>("Project*");
        QSignalSpy projectSaved(&projects, &ProjectManager::projectSaved);
        QSignalSpy projectCreated(&projects, &ProjectManager::projectCreated);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        QStringList projectEvents;
        QObject eventContext;
        connect(&projects, &ProjectManager::projectSaved, &eventContext,
                [&projectEvents](Project*) { projectEvents.append(QStringLiteral("saved")); });
        connect(&projects, &ProjectManager::projectCreated, &eventContext,
                [&projectEvents](Project*) { projectEvents.append(QStringLiteral("created")); });

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject createParams{{QStringLiteral("name"), QStringLiteral("new-project-after-save")},
                                       {QStringLiteral("onUnsaved"), QStringLiteral("save")}};
        QJsonObject pending = controller.handleToolCall(QStringLiteral("create_project"), createParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonArray effects = controller.pendingConfirmationPayload()
                                 .value(QStringLiteral("context"))
                                 .toObject()
                                 .value(QStringLiteral("sideEffects"))
                                 .toArray();
        QVERIFY(effects.contains(QStringLiteral("project_switch:new")));
        const QString effectPrefix =
            QStringLiteral("file_write:%1 (implicit current project save before switch)").arg(oldLink);
        QVERIFY(std::any_of(effects.begin(), effects.end(), [&effectPrefix, &oldFileA](const QJsonValue& value) {
            const QString effect = value.toString();
            return effect.startsWith(effectPrefix) &&
                   effect.contains(QStringLiteral("resolved_target=%1").arg(oldFileA));
        }));
        int initialSaveEffectIndex = -1;
        int initialSwitchEffectIndex = -1;
        for (int i = 0; i < effects.size(); ++i) {
            const QString effect = effects.at(i).toString();
            if (effect.startsWith(effectPrefix))
                initialSaveEffectIndex = i;
            if (effect == QStringLiteral("project_switch:new"))
                initialSwitchEffectIndex = i;
        }
        QVERIFY(initialSaveEffectIndex >= 0);
        QVERIFY(initialSwitchEffectIndex > initialSaveEffectIndex);
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectCreated.count(), 0);
        QCOMPARE(controller.m_conversationHistory.size(), 0);
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectCreated.count(), 0);
        QCOMPARE(projects.currentProject(), original);
        QVERIFY(original->isModified());
        QVERIFY(projectEvents.isEmpty());
        QVERIFY(fileA.open(QIODevice::ReadOnly));
        QCOMPARE(fileA.readAll(), oldBytesA);
        fileA.close();
        QVERIFY(fileB.open(QIODevice::ReadOnly));
        QCOMPARE(fileB.readAll(), oldBytesB);
        fileB.close();
        QVERIFY(controller.m_conversationHistory.isEmpty()); // Bridge rejection adds no synthetic tool result.

        pending = controller.handleToolCall(QStringLiteral("create_project"), createParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(QFile::link(oldFileB, replacementLink));
        const QByteArray replacementName = QFile::encodeName(replacementLink);
        const QByteArray currentName = QFile::encodeName(oldLink);
        QCOMPARE(std::rename(replacementName.constData(), currentName.constData()), 0);
        QVERIFY(QFileInfo(oldLink).isSymLink());
        QVERIFY(controller.confirmPendingTools(staleId));
        QCOMPARE(auditSpy.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectCreated.count(), 0);
        QCOMPARE(projects.currentProject(), original);
        QVERIFY(original->isModified());
        QVERIFY(projectEvents.isEmpty());
        QVERIFY(fileA.open(QIODevice::ReadOnly));
        QCOMPARE(fileA.readAll(), oldBytesA);
        fileA.close();
        QVERIFY(fileB.open(QIODevice::ReadOnly));
        QCOMPARE(fileB.readAll(), oldBytesB);
        fileB.close();
        QVERIFY(controller.m_conversationHistory.isEmpty());

        pending = controller.handleToolCall(QStringLiteral("create_project"), createParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        effects = controller.pendingConfirmationPayload()
                      .value(QStringLiteral("context"))
                      .toObject()
                      .value(QStringLiteral("sideEffects"))
                      .toArray();
        QVERIFY(std::any_of(effects.begin(), effects.end(), [&effectPrefix, &oldFileB](const QJsonValue& value) {
            const QString effect = value.toString();
            return effect.startsWith(effectPrefix) &&
                   effect.contains(QStringLiteral("resolved_target=%1").arg(oldFileB));
        }));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(projectSaved.count(), 1);
        QCOMPARE(projectCreated.count(), 1);
        QCOMPARE(projectEvents, (QStringList{QStringLiteral("saved"), QStringLiteral("created")}));
        QVERIFY(projects.currentProject());
        QCOMPARE(projects.currentProject()->name(), QStringLiteral("new-project-after-save"));
        QVERIFY(fileA.open(QIODevice::ReadOnly));
        QCOMPARE(fileA.readAll(), oldBytesA);
        fileA.close();
        QVERIFY(fileB.open(QIODevice::ReadOnly));
        const QJsonObject savedB = QJsonDocument::fromJson(fileB.readAll()).object();
        fileB.close();
        QCOMPARE(savedB.value(QStringLiteral("version")).toString(), QStringLiteral("3.0"));
        QCOMPARE(savedB.value(QStringLiteral("name")).toString(), QStringLiteral("edited-before-switch"));
        QVERIFY(!QFileInfo::exists(oldFileB + QStringLiteral(".v2.bak")));

        // Clean + save and explicit cancel/discard policies retain their old semantics.
        const QString cleanPath = tempDir.filePath(QStringLiteral("clean-before-switch.dproj"));
        QVERIFY(projects.saveProject(cleanPath));
        projectSaved.clear();
        projectCreated.clear();
        const QJsonObject cleanSave = controller.handleToolCall(
            QStringLiteral("create_project"), {{QStringLiteral("name"), QStringLiteral("clean-save-path")},
                                               {QStringLiteral("onUnsaved"), QStringLiteral("save")}});
        QVERIFY(cleanSave.value(QStringLiteral("status")).toString() != QStringLiteral("pending_confirmation"));
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectCreated.count(), 1);

        Project* dirtyForCancel = projects.currentProject();
        QVERIFY(dirtyForCancel);
        dirtyForCancel->setName(QStringLiteral("dirty-cancel"));
        const QJsonObject cancel = controller.handleToolCall(QStringLiteral("create_project"),
                                                             {{QStringLiteral("name"), QStringLiteral("cancel-target")},
                                                              {QStringLiteral("onUnsaved"), QStringLiteral("cancel")}});
        QCOMPARE(cancel.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PROJECT_UNSAVED"));
        QCOMPARE(projects.currentProject(), dirtyForCancel);
        QCOMPARE(projectCreated.count(), 1);

        const QJsonObject discard = controller.handleToolCall(
            QStringLiteral("create_project"), {{QStringLiteral("name"), QStringLiteral("discard-target")},
                                               {QStringLiteral("onUnsaved"), QStringLiteral("discard")}});
        QVERIFY(discard.value(QStringLiteral("status")).toString() != QStringLiteral("pending_confirmation"));
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectCreated.count(), 2);

        Project* noPath = projects.currentProject();
        QVERIFY(noPath);
        noPath->setName(QStringLiteral("dirty-without-path"));
        const QJsonObject missingSaveTarget = controller.handleToolCall(
            QStringLiteral("create_project"), {{QStringLiteral("name"), QStringLiteral("must-not-switch")},
                                               {QStringLiteral("onUnsaved"), QStringLiteral("save")}});
        QCOMPARE(missingSaveTarget.value(QStringLiteral("errorCode")).toString(), QStringLiteral("SAVE_FAILED"));
        QCOMPARE(projects.currentProject(), noPath);
        QVERIFY(noPath->isModified());
        QCOMPARE(projectCreated.count(), 2);
#ifdef Q_OS_UNIX
        const QString loopPath = tempDir.filePath(QStringLiteral("loop.dproj"));
        const QByteArray loopPathBytes = QFile::encodeName(loopPath);
        if (::symlink("loop.dproj", loopPathBytes.constData()) == 0) {
            noPath->setFilePath(loopPath);
            const QJsonObject unresolvableSaveTarget = controller.handleToolCall(
                QStringLiteral("create_project"), {{QStringLiteral("name"), QStringLiteral("must-not-switch-either")},
                                                   { QStringLiteral("onUnsaved"),
                                                     QStringLiteral("save") }});
            QCOMPARE(unresolvableSaveTarget.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("SAVE_FAILED"));
            QCOMPARE(projects.currentProject(), noPath);
            QVERIFY(noPath->isModified());
            QCOMPARE(projectCreated.count(), 2);
        }
#endif
        projects.closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testAutopilotOpenProjectBridgeCandidateContentDriftExpiresWithoutSyntheticToolMessages() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString oldPath = tempDir.filePath(QStringLiteral("bridge-old-project.dproj"));
        const QString candidatePath = tempDir.filePath(QStringLiteral("bridge-candidate.dproj"));
        const QString replacementPath = tempDir.filePath(QStringLiteral("bridge-replacement.dproj"));
        Project* oldProject = projects.newProject();
        QVERIFY(oldProject);
        oldProject->setName(QStringLiteral("bridge-old-clean-v3"));
        QVERIFY(projects.saveProject(oldPath));
        Project candidate;
        candidate.setName(QStringLiteral("bridge-candidate-v3"));
        QVERIFY(candidate.save(candidatePath));
        Project replacement;
        replacement.setName(QStringLiteral("bridge-latest-v3"));
        QVERIFY(replacement.save(replacementPath));

        oldProject->setName(QStringLiteral("bridge-old-dirty-v3"));
        QVERIFY(oldProject->isModified());
        QFile oldFile(oldPath);
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        const QByteArray oldBytes = oldFile.readAll();
        oldFile.close();
        QFile replacementFile(replacementPath);
        QVERIFY(replacementFile.open(QIODevice::ReadOnly));
        const QByteArray replacementBytes = replacementFile.readAll();
        replacementFile.close();

        qRegisterMetaType<Project*>("Project*");
        QSignalSpy projectSaved(&projects, &ProjectManager::projectSaved);
        QSignalSpy projectOpened(&projects, &ProjectManager::projectOpened);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("open_project"),
            {{QStringLiteral("path"), candidatePath}, {QStringLiteral("onUnsaved"), QStringLiteral("save")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        const QJsonArray effects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        const QByteArray candidateBytes = [&candidatePath]() {
            QFile file(candidatePath);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        }();
        const QString candidateHash =
            QString::fromLatin1(QCryptographicHash::hash(candidateBytes, QCryptographicHash::Sha256).toHex().left(12));
        QVERIFY(std::any_of(effects.cbegin(), effects.cend(), [&candidatePath, &candidateHash](const QJsonValue& v) {
            const QString effect = v.toString();
            return effect.startsWith(QStringLiteral("project_input:%1").arg(candidatePath)) &&
                   effect.contains(QStringLiteral("sha256=%1").arg(candidateHash));
        }));

        QSaveFile changedCandidate(candidatePath);
        QVERIFY(changedCandidate.open(QIODevice::WriteOnly));
        QCOMPARE(changedCandidate.write(replacementBytes), replacementBytes.size());
        QVERIFY(changedCandidate.commit());
        QVERIFY(controller.confirmPendingTools(staleId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(projects.currentProject(), oldProject);
        QVERIFY(oldProject->isModified());
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectOpened.count(), 0);
        QCOMPARE(projects.currentProject()->name(), QStringLiteral("bridge-old-dirty-v3"));
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        QCOMPARE(oldFile.readAll(), oldBytes);
        oldFile.close();
        QVERIFY(controller.m_conversationHistory.isEmpty());

        const QJsonObject fresh = controller.handleToolCall(
            QStringLiteral("open_project"),
            {{QStringLiteral("path"), candidatePath}, {QStringLiteral("onUnsaved"), QStringLiteral("save")}});
        QCOMPARE(fresh.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(projectSaved.count(), 1);
        QCOMPARE(projectOpened.count(), 1);
        QCOMPARE(projects.currentProject()->name(), QStringLiteral("bridge-latest-v3"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(controller.m_conversationHistory.isEmpty());
        projects.closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testAutopilotOpenProjectMissingCandidateKeepsSaveThenOpenFailedOrder() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString oldPath = tempDir.filePath(QStringLiteral("missing-open-old.dproj"));
        const QString missingPath = tempDir.filePath(QStringLiteral("stable-missing-candidate.dproj"));
        QVERIFY(!QFileInfo::exists(missingPath));
        Project* oldProject = projects.newProject();
        QVERIFY(oldProject);
        oldProject->setName(QStringLiteral("missing-old-clean-v3"));
        QVERIFY(projects.saveProject(oldPath));
        oldProject->setName(QStringLiteral("missing-old-dirty-v3"));
        QVERIFY(oldProject->isModified());

        qRegisterMetaType<Project*>("Project*");
        QSignalSpy projectSaved(&projects, &ProjectManager::projectSaved);
        QSignalSpy projectOpened(&projects, &ProjectManager::projectOpened);
        QStringList events;
        QObject eventContext;
        connect(&projects, &ProjectManager::projectSaved, &eventContext,
                [&events](Project*) { events.append(QStringLiteral("saved")); });
        connect(&projects, &ProjectManager::projectOpened, &eventContext,
                [&events](Project*) { events.append(QStringLiteral("opened")); });
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("open_project"),
            {{QStringLiteral("path"), missingPath}, {QStringLiteral("onUnsaved"), QStringLiteral("save")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray effects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        QVERIFY(std::any_of(effects.cbegin(), effects.cend(), [&missingPath](const QJsonValue& v) {
            return v.toString().startsWith(QStringLiteral("project_input:%1").arg(missingPath)) &&
                   v.toString().endsWith(QStringLiteral("snapshot=unavailable"));
        }));
        QVERIFY(!QFileInfo::exists(missingPath));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(toolExecuted.last().at(0).toString(), QStringLiteral("open_project"));
        QCOMPARE(qvariant_cast<QJsonObject>(toolExecuted.last().at(1)).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("OPEN_FAILED"));
        QCOMPARE(projectSaved.count(), 1);
        QCOMPARE(projectOpened.count(), 0);
        QCOMPARE(events, QStringList{QStringLiteral("saved")});
        QCOMPARE(projects.currentProject(), oldProject);
        QVERIFY(!oldProject->isModified());
        QCOMPARE(oldProject->name(), QStringLiteral("missing-old-dirty-v3"));
        QFile savedOldProject(oldPath);
        QVERIFY(savedOldProject.open(QIODevice::ReadOnly));
        const QJsonObject savedOldJson = QJsonDocument::fromJson(savedOldProject.readAll()).object();
        QCOMPARE(savedOldJson.value(QStringLiteral("version")).toString(), QStringLiteral("3.0"));
        QCOMPARE(savedOldJson.value(QStringLiteral("name")).toString(), QStringLiteral("missing-old-dirty-v3"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        QVERIFY(!QFileInfo::exists(missingPath));
        projects.closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testAutopilotOpenProjectImplicitSaveFailureKeepsCurrentProject() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString oldPath = tempDir.filePath(QStringLiteral("implicit-save-old.dproj"));
        const QString candidatePath = tempDir.filePath(QStringLiteral("implicit-save-candidate.dproj"));
        Project* oldProject = projects.newProject();
        QVERIFY(oldProject);
        oldProject->setName(QStringLiteral("old-clean-before-save-failure"));
        QVERIFY(projects.saveProject(oldPath));
        Project candidate;
        candidate.setName(QStringLiteral("candidate-must-not-open"));
        QVERIFY(candidate.save(candidatePath));
        QFile oldFile(oldPath);
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = oldFile.readAll();
        oldFile.close();

        oldProject->setName(QStringLiteral("old-dirty-save-failure"));
        QVERIFY(oldProject->isModified());
        qRegisterMetaType<Project*>("Project*");
        QSignalSpy projectSaved(&projects, &ProjectManager::projectSaved);
        QSignalSpy projectOpened(&projects, &ProjectManager::projectOpened);
        QSignalSpy projectCreated(&projects, &ProjectManager::projectCreated);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject pending = controller.handleToolCall(
            QStringLiteral("open_project"),
            {{QStringLiteral("path"), candidatePath}, {QStringLiteral("onUnsaved"), QStringLiteral("save")}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(!projectSaved.count());
        QVERIFY(!projectOpened.count());
        Project::failNextSaveAfterBytesForTesting(24);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));

        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(toolExecuted.last().at(0).toString(), QStringLiteral("open_project"));
        QCOMPARE(qvariant_cast<QJsonObject>(toolExecuted.last().at(1)).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SAVE_FAILED"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectOpened.count(), 0);
        QCOMPARE(projectCreated.count(), 0);
        QCOMPARE(projects.currentProject(), oldProject);
        QVERIFY(oldProject->isModified());
        QCOMPARE(oldProject->filePath(), oldPath);
        QCOMPARE(oldProject->name(), QStringLiteral("old-dirty-save-failure"));
        QFile oldFileAfterFailure(oldPath);
        QVERIFY(oldFileAfterFailure.open(QIODevice::ReadOnly));
        QCOMPARE(oldFileAfterFailure.readAll(), originalBytes);
        projects.closeProjectChecked(ProjectManager::UnsavedPolicy::Discard);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testAutopilotOpenProjectImplicitSaveConfirmationBindsMixedLlmBatch() {
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString oldPath = tempDir.filePath(QStringLiteral("old-project.dproj"));
        const QString candidatePath = tempDir.filePath(QStringLiteral("candidate-project.dproj"));
        Project* oldProject = projects.newProject();
        QVERIFY(oldProject);
        oldProject->setName(QStringLiteral("old-clean-v3"));
        QVERIFY(projects.saveProject(oldPath));
        Project candidate;
        candidate.setName(QStringLiteral("opened-candidate-v3"));
        QVERIFY(candidate.save(candidatePath));
        QFile oldFile(oldPath);
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        const QByteArray cleanOldBytes = oldFile.readAll();
        oldFile.close();
        QCOMPARE(QJsonDocument::fromJson(cleanOldBytes).object().value(QStringLiteral("version")).toString(),
                 QStringLiteral("3.0"));

        oldProject->setName(QStringLiteral("old-edited-before-open"));
        QVERIFY(oldProject->isModified());
        qRegisterMetaType<Project*>("Project*");
        QSignalSpy projectSaved(&projects, &ProjectManager::projectSaved);
        QSignalSpy projectOpened(&projects, &ProjectManager::projectOpened);
        QStringList projectEvents;
        QObject eventContext;
        connect(&projects, &ProjectManager::projectSaved, &eventContext,
                [&projectEvents](Project*) { projectEvents.append(QStringLiteral("saved")); });
        connect(&projects, &ProjectManager::projectOpened, &eventContext,
                [&projectEvents](Project*) { projectEvents.append(QStringLiteral("opened")); });

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        controller.sendUserMessage(QStringLiteral("Open the candidate after saving my current project"));
        AgentResponse response;
        response.success = true;
        response.content = QStringLiteral("I will save the current project and open the candidate.");
        response.toolCalls =
            QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("call_open_dirty_project")},
                                   {QStringLiteral("type"), QStringLiteral("function")},
                                   {QStringLiteral("name"), QStringLiteral("open_project")},
                                   {QStringLiteral("arguments"),
                                    QJsonObject{{QStringLiteral("path"), candidatePath},
                                                {QStringLiteral("onUnsaved"), QStringLiteral("save")}}}},
                       QJsonObject{{QStringLiteral("id"), QStringLiteral("call_inspect_opened_project")},
                                   {QStringLiteral("type"), QStringLiteral("function")},
                                   {QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                   {QStringLiteral("arguments"), QJsonObject{}}}};
        m_mock->emitResponse(response);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectOpened.count(), 0);
        QCOMPARE(projects.currentProject(), oldProject);
        QVERIFY(oldProject->isModified());
        QCOMPARE(controller.m_conversationHistory.size(), 2); // user plus assistant tool-call batch
        const QJsonObject context = controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        const QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(effects.contains(QStringLiteral("project_switch:%1").arg(candidatePath)));
        const QString saveEffect =
            QStringLiteral("file_write:%1 (implicit current project save before switch)").arg(oldPath);
        QVERIFY(std::any_of(effects.begin(), effects.end(), [&saveEffect](const QJsonValue& value) {
            return value.toString().startsWith(saveEffect);
        }));
        int saveEffectIndex = -1;
        int switchEffectIndex = -1;
        for (int i = 0; i < effects.size(); ++i) {
            const QString effect = effects.at(i).toString();
            if (effect.startsWith(saveEffect))
                saveEffectIndex = i;
            if (effect == QStringLiteral("project_switch:%1").arg(candidatePath))
                switchEffectIndex = i;
        }
        QVERIFY(saveEffectIndex >= 0);
        QVERIFY(switchEffectIndex > saveEffectIndex);
        QCOMPARE(context.value(QStringLiteral("projectPath")).toString(), oldPath);

        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QVERIFY(controller.confirmPendingTools(confirmationId));
        for (int i = 0; i < 30 && controller.state() == AgentController::AgentState::Executing; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(projectSaved.count(), 1);
        QCOMPARE(projectOpened.count(), 1);
        QCOMPARE(projectEvents, (QStringList{QStringLiteral("saved"), QStringLiteral("opened")}));
        QCOMPARE(projects.currentProject()->filePath(), candidatePath);
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        const QJsonObject savedOld = QJsonDocument::fromJson(oldFile.readAll()).object();
        oldFile.close();
        QCOMPARE(savedOld.value(QStringLiteral("version")).toString(), QStringLiteral("3.0"));
        QCOMPARE(savedOld.value(QStringLiteral("name")).toString(), QStringLiteral("old-edited-before-open"));

        int openCallResults = 0;
        int stateCallResults = 0;
        for (const AgentMessage& message : m_mock->lastCtx.messages) {
            if (message.role != QStringLiteral("tool"))
                continue;
            if (message.toolCallId == QLatin1String("call_open_dirty_project"))
                ++openCallResults;
            if (message.toolCallId == QLatin1String("call_inspect_opened_project"))
                ++stateCallResults;
        }
        QCOMPARE(openCallResults, 1);
        QCOMPARE(stateCallResults, 1);

        AgentResponse finalResponse;
        finalResponse.success = true;
        finalResponse.content = QStringLiteral("The project has been opened.");
        m_mock->emitResponse(finalResponse);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        projects.closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testAutopilotOpenProjectLlmBatchBindsEveryCandidateSnapshot() {
#ifndef Q_OS_UNIX
        QSKIP("The candidate-project target drift fixture requires POSIX symlinks");
#else
        AgentController& controller = AgentController::instance();
        ProjectManager& projects = ProjectManager::instance();
        projects.closeProject();
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());

        const QString oldPath = tempDir.filePath(QStringLiteral("batch-old-project.dproj"));
        const QString candidateAPath = tempDir.filePath(QStringLiteral("candidate-a.dproj"));
        const QString candidateBPath = tempDir.filePath(QStringLiteral("candidate-b.dproj"));
        const QString candidateCPath = tempDir.filePath(QStringLiteral("candidate-c.dproj"));
        const QString candidateLinkPath = tempDir.filePath(QStringLiteral("candidate-link.dproj"));
        const QString latestPath = tempDir.filePath(QStringLiteral("candidate-latest.dproj"));

        Project* oldProject = projects.newProject();
        QVERIFY(oldProject);
        oldProject->setName(QStringLiteral("batch-old-clean-v3"));
        QVERIFY(projects.saveProject(oldPath));
        oldProject->setName(QStringLiteral("batch-old-dirty-v3"));
        QVERIFY(oldProject->isModified());
        QFile oldFile(oldPath);
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        const QByteArray oldBytes = oldFile.readAll();
        oldFile.close();

        Project candidateA;
        candidateA.setName(QStringLiteral("candidate-a-v3"));
        QVERIFY(candidateA.save(candidateAPath));
        QVERIFY(QFile::copy(candidateAPath, candidateBPath));
        QVERIFY(QFile::copy(candidateAPath, candidateCPath));
        if (!QFile::link(candidateBPath, candidateLinkPath))
            QSKIP("Unable to create POSIX candidate symlink");

        qRegisterMetaType<Project*>("Project*");
        QSignalSpy projectSaved(&projects, &ProjectManager::projectSaved);
        QSignalSpy projectOpened(&projects, &ProjectManager::projectOpened);
        QStringList projectEvents;
        QObject eventContext;
        connect(&projects, &ProjectManager::projectSaved, &eventContext,
                [&projectEvents](Project*) { projectEvents.append(QStringLiteral("saved")); });
        connect(&projects, &ProjectManager::projectOpened, &eventContext, [&projectEvents](Project* project) {
            projectEvents.append(QStringLiteral("opened:%1").arg(project->name()));
        });
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        const auto makeBatch = [&candidateAPath, &candidateLinkPath](const QString& prefix) {
            return QJsonArray{QJsonObject{{QStringLiteral("id"), prefix + QStringLiteral("-open-a")},
                                          {QStringLiteral("type"), QStringLiteral("function")},
                                          {QStringLiteral("name"), QStringLiteral("open_project")},
                                          {QStringLiteral("arguments"),
                                           QJsonObject{{QStringLiteral("path"), candidateAPath},
                                                       {QStringLiteral("onUnsaved"), QStringLiteral("save")}}}},
                              QJsonObject{{QStringLiteral("id"), prefix + QStringLiteral("-open-link")},
                                          {QStringLiteral("type"), QStringLiteral("function")},
                                          {QStringLiteral("name"), QStringLiteral("open_project")},
                                          {QStringLiteral("arguments"),
                                           QJsonObject{{QStringLiteral("path"), candidateLinkPath},
                                                       {QStringLiteral("onUnsaved"), QStringLiteral("save")}}}},
                              QJsonObject{{QStringLiteral("id"), prefix + QStringLiteral("-state")},
                                          {QStringLiteral("type"), QStringLiteral("function")},
                                          {QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                          {QStringLiteral("arguments"), QJsonObject{}}}};
        };
        const auto submitBatch = [this, &controller, &makeBatch](const QString& prefix) {
            controller.sendUserMessage(QStringLiteral("Open both reviewed projects."));
            AgentResponse response;
            response.success = true;
            response.content = QStringLiteral("I will open both project files.");
            response.toolCalls = makeBatch(prefix);
            m_mock->emitResponse(response);
        };
        const auto assertToolResults = [this](const QString& prefix, bool expired) {
            const QStringList ids{prefix + QStringLiteral("-open-a"), prefix + QStringLiteral("-open-link"),
                                  prefix + QStringLiteral("-state")};
            for (int i = 0; i < ids.size(); ++i) {
                int count = 0;
                QJsonObject result;
                for (const AgentMessage& message : AgentController::instance().m_conversationHistory) {
                    if (message.role == QLatin1String("tool") && message.toolCallId == ids.at(i)) {
                        ++count;
                        result = QJsonDocument::fromJson(message.content.toUtf8()).object();
                    }
                }
                QCOMPARE(count, 1);
                if (expired) {
                    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(),
                             QStringLiteral("CONFIRMATION_EXPIRED"));
                } else if (i < 2) {
                    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
                } else {
                    QVERIFY(!result.isEmpty());
                    QVERIFY(result.value(QStringLiteral("errorCode")).toString().isEmpty());
                }
            }
        };
        const auto readFile = [](const QString& path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray{};
            return file.readAll();
        };
        const auto candidateInputEffects = [](const QJsonArray& effects) {
            QStringList found;
            for (const QJsonValue& value : effects)
                if (value.toString().startsWith(QStringLiteral("project_input:")))
                    found.append(value.toString());
            return found;
        };

        submitBatch(QStringLiteral("batch1"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Confirming, 5000);
        const QString staleSymlinkId = controller.pendingConfirmationId();
        const QJsonArray firstEffects = controller.pendingConfirmationPayload()
                                            .value(QStringLiteral("context"))
                                            .toObject()
                                            .value(QStringLiteral("sideEffects"))
                                            .toArray();
        const QStringList firstInputs = candidateInputEffects(firstEffects);
        QCOMPARE(firstInputs.size(), 2);
        const QString hashA = QString::fromLatin1(
            QCryptographicHash::hash(readFile(candidateAPath), QCryptographicHash::Sha256).toHex().left(12));
        const QString hashB = QString::fromLatin1(
            QCryptographicHash::hash(readFile(candidateBPath), QCryptographicHash::Sha256).toHex().left(12));
        QVERIFY(std::any_of(firstInputs.cbegin(), firstInputs.cend(), [&candidateAPath, &hashA](const QString& effect) {
            return effect.startsWith(QStringLiteral("project_input:%1").arg(candidateAPath)) &&
                   effect.contains(QStringLiteral("sha256=%1").arg(hashA));
        }));
        QVERIFY(std::any_of(firstInputs.cbegin(), firstInputs.cend(),
                            [&candidateLinkPath, &candidateBPath, &hashB](const QString& effect) {
                                return effect.startsWith(QStringLiteral("project_input:%1").arg(candidateLinkPath)) &&
                                       effect.contains(QStringLiteral("resolved_target=%1").arg(candidateBPath)) &&
                                       effect.contains(QStringLiteral("sha256=%1").arg(hashB));
                            }));
        int implicitSaveEffects = 0;
        for (const QJsonValue& value : firstEffects)
            if (value.toString().contains(QStringLiteral("(implicit current project save before switch)")))
                ++implicitSaveEffects;
        QCOMPARE(implicitSaveEffects, 1);

        QVERIFY(QFile::remove(candidateLinkPath));
        QVERIFY(QFile::link(candidateCPath, candidateLinkPath));
        QVERIFY(controller.confirmPendingTools(staleSymlinkId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectOpened.count(), 0);
        QCOMPARE(projects.currentProject(), oldProject);
        QVERIFY(oldProject->isModified());
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        QCOMPARE(oldFile.readAll(), oldBytes);
        oldFile.close();
        assertToolResults(QStringLiteral("batch1"), true);

        submitBatch(QStringLiteral("batch2"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Confirming, 5000);
        const QString staleContentId = controller.pendingConfirmationId();
        Project latest;
        latest.setName(QStringLiteral("candidate-c-latest-v3"));
        QVERIFY(latest.save(latestPath));
        const QByteArray latestBytes = readFile(latestPath);
        QVERIFY(!latestBytes.isEmpty());
        QSaveFile replaceCandidateC(candidateCPath);
        QVERIFY(replaceCandidateC.open(QIODevice::WriteOnly));
        QCOMPARE(replaceCandidateC.write(latestBytes), latestBytes.size());
        QVERIFY(replaceCandidateC.commit());
        QVERIFY(controller.confirmPendingTools(staleContentId));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(projectSaved.count(), 0);
        QCOMPARE(projectOpened.count(), 0);
        QCOMPARE(projects.currentProject(), oldProject);
        QVERIFY(oldProject->isModified());
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        QCOMPARE(oldFile.readAll(), oldBytes);
        oldFile.close();
        assertToolResults(QStringLiteral("batch2"), true);

        submitBatch(QStringLiteral("batch3"));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Confirming, 5000);
        const QJsonArray freshEffects = controller.pendingConfirmationPayload()
                                            .value(QStringLiteral("context"))
                                            .toObject()
                                            .value(QStringLiteral("sideEffects"))
                                            .toArray();
        const QString freshHash =
            QString::fromLatin1(QCryptographicHash::hash(latestBytes, QCryptographicHash::Sha256).toHex().left(12));
        const QStringList freshInputs = candidateInputEffects(freshEffects);
        QVERIFY(std::any_of(freshInputs.cbegin(), freshInputs.cend(),
                            [&candidateLinkPath, &candidateCPath, &freshHash](const QString& effect) {
                                return effect.startsWith(QStringLiteral("project_input:%1").arg(candidateLinkPath)) &&
                                       effect.contains(QStringLiteral("resolved_target=%1").arg(candidateCPath)) &&
                                       effect.contains(QStringLiteral("sha256=%1").arg(freshHash));
                            }));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Thinking, 5000);
        QCOMPARE(projectSaved.count(), 1);
        QCOMPARE(projectOpened.count(), 2);
        QCOMPARE(projectEvents, (QStringList{QStringLiteral("saved"), QStringLiteral("opened:candidate-a-v3"),
                                             QStringLiteral("opened:candidate-c-latest-v3")}));
        QCOMPARE(projects.currentProject()->name(), QStringLiteral("candidate-c-latest-v3"));
        QCOMPARE(projects.currentProject()->filePath(), candidateLinkPath);
        QVERIFY(oldFile.open(QIODevice::ReadOnly));
        const QJsonObject savedDirtyOld = QJsonDocument::fromJson(oldFile.readAll()).object();
        oldFile.close();
        QCOMPARE(savedDirtyOld.value(QStringLiteral("version")).toString(), QStringLiteral("3.0"));
        QCOMPARE(savedDirtyOld.value(QStringLiteral("name")).toString(), QStringLiteral("batch-old-dirty-v3"));
        assertToolResults(QStringLiteral("batch3"), false);
        AgentResponse finalResponse;
        finalResponse.success = true;
        finalResponse.content = QStringLiteral("Both reviewed projects were opened.");
        m_mock->emitResponse(finalResponse);
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        projects.closeProject();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
#endif
    }

    void testMultipleToolResultsRemainPairedInNextRequest() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        controller.sendUserMessage(QStringLiteral("Inspect workflow"));
        AgentResponse response;
        response.success = true;
        response.toolCalls =
            QJsonArray{QJsonObject{{"id", "call-a"}, {"type", "function"}, {"name", "get_available_plugins"}},
                       QJsonObject{{"id", "call-b"}, {"type", "function"}, {"name", "get_available_plugins"}}};
        m_mock->emitResponse(response);
        QTRY_COMPARE(m_mock->requestCount, 2);
        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        int assistantIndex = -1;
        for (int i = 0; i < messages.size(); ++i) {
            if (messages.at(i).toObject().value("tool_calls").toArray().size() == 2)
                assistantIndex = i;
        }
        QVERIFY(assistantIndex >= 0);
        QVERIFY(assistantIndex + 2 < messages.size());
        QCOMPARE(messages.at(assistantIndex + 1).toObject().value("tool_call_id").toString(), QStringLiteral("call-a"));
        QCOMPARE(messages.at(assistantIndex + 2).toObject().value("tool_call_id").toString(), QStringLiteral("call-b"));
        QCOMPARE(messages.at(assistantIndex + 2).toObject().value("role").toString(), QStringLiteral("tool"));
    }

    void testRejectedChatToolsKeepResultsInNextRequest() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Modify workflow"));
        AgentResponse response;
        response.success = true;
        response.toolCalls =
            QJsonArray{QJsonObject{{"id", "reject-a"}, {"type", "function"}, {"name", "add_module"}},
                       QJsonObject{{"id", "reject-b"}, {"type", "function"}, {"name", "remove_module"}}};
        m_mock->emitResponse(response);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        controller.rejectPendingTools();
        controller.sendUserMessage(QStringLiteral("Continue"));
        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        int assistantIndex = -1;
        for (int i = 0; i < messages.size(); ++i)
            if (messages.at(i).toObject().value("tool_calls").toArray().size() == 2)
                assistantIndex = i;
        QVERIFY(assistantIndex >= 0);
        QCOMPARE(messages.at(assistantIndex + 1).toObject().value("tool_call_id").toString(),
                 QStringLiteral("reject-a"));
        QCOMPARE(messages.at(assistantIndex + 2).toObject().value("tool_call_id").toString(),
                 QStringLiteral("reject-b"));
        QVERIFY(messages.at(assistantIndex + 1).toObject().value("content").toString().contains("USER_REJECTED"));
    }

    void testBridgeRejectionDoesNotCreateToolHistory() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const int before = controller.conversationHistorySize();
        const QJsonObject result = controller.handleToolCall(QStringLiteral("add_module"), {{"plugin", "GrabImage"}});
        QCOMPARE(result.value("status").toString(), QStringLiteral("pending_confirmation"));
        controller.rejectPendingTools();
        QCOMPARE(controller.conversationHistorySize(), before);
    }

    void testAbortQueuedConfirmationKeepsToolResult() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Modify workflow"));
        AgentResponse response;
        response.success = true;
        response.toolCalls = QJsonArray{QJsonObject{{"id", "cancel-a"}, {"type", "function"}, {"name", "add_module"}}};
        m_mock->emitResponse(response);
        controller.confirmPendingTools();
        controller.abortCurrentTurn();
        QCoreApplication::processEvents();
        // B1：中止后不得续跑，模型请求计数保持为首次请求
        QCOMPARE(m_mock->requestCount, 1);
        controller.sendUserMessage(QStringLiteral("Continue"));
        bool found = false;
        for (const QJsonValue& value : m_mock->lastCtx.toOpenAIMessages()) {
            const QJsonObject message = value.toObject();
            if (message.value("tool_call_id").toString() == QLatin1String("cancel-a"))
                found = message.value("content").toString().contains("USER_CANCELLED");
        }
        QVERIFY(found);
    }

    void testPermissionDowngradeBeforeConfirmationDeniesExecution() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("Create project"));
        AgentResponse response;
        response.success = true;
        response.toolCalls = QJsonArray{QJsonObject{{"id", "denied-a"},
                                                    {"type", "function"},
                                                    {"name", "create_project"},
                                                    {"arguments", QJsonObject{{"name", "MustNotCreate"}}}}};
        m_mock->emitResponse(response);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
        QSignalSpy actions(&controller, &AgentController::agentActionCompleted);
        controller.confirmPendingTools();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCoreApplication::processEvents();
        QCOMPARE(actions.count(), 0);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        controller.sendUserMessage(QStringLiteral("Continue"));
        const QJsonArray messages = m_mock->lastCtx.toOpenAIMessages();
        bool found = false;
        for (const QJsonValue& value : messages) {
            const QJsonObject message = value.toObject();
            if (message.value("tool_call_id").toString() == QLatin1String("denied-a"))
                found = message.value("content").toString().contains("PERMISSION_DENIED");
        }
        QVERIFY(found);
    }

    // ---------- 并发防护 ----------

    void testBusyStateRejectsNewMessage() {
        AgentController::instance().sendUserMessage("First");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);
        AgentController::instance().sendUserMessage("Second");

        QCOMPARE(responseSpy.count(), 1);
        QString reply = responseSpy.takeFirst()[0].toString();
        QVERIFY(reply.contains("busy"));
    }

    void testUndoRejectedWhileFlowBusy() {
        AgentController& controller = AgentController::instance();
        controller.m_actor->undoStack()->clear();
        int undoCount = 0;
        controller.m_actor->undoStack()->push(new UndoCounterCommand(&undoCount));

        RunEngine& engine = RunEngine::instance();
        auto* module = new AgentBusyProbeModule();
        module->initialize();
        engine.addModule(module);
        engine.start();

        QVERIFY(engine.isBusy());
        QVERIFY(!controller.undoLastAgentAction());
        QCOMPARE(undoCount, 0);

        engine.stop();
        engine.clearModules();
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(undoCount, 1);
        delete module;
    }

    // ---------- 错误恢复 ----------

    void testErrorInConfirmingCleansPendingTools() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage("Add module");

        AgentResponse resp;
        resp.success = true;
        resp.content = "Adding...";
        QJsonObject tc;
        tc["id"] = "call_1";
        tc["type"] = "function";
        tc["name"] = "add_module";
        tc["arguments"] = QJsonObject{{"plugin", "GrabImage"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);
        QVERIFY(!AgentController::instance().pendingToolCalls().isEmpty());

        // 模拟用户在确认前发生网络错误（通过重新发送消息触发错误）
        // 实际上我们需要测试 onLLMError 在 Confirming 状态下的行为
        // 由于 onLLMError 只在 LLM 响应时触发，我们直接调用它
        m_mock->emitError("Connection lost");

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QVERIFY(AgentController::instance().pendingToolCalls().isEmpty());
    }

    // ---------- History 截断 ----------

    void testTrimHistoryPreservesMessagePairs() {
        AgentController::instance().clearConversation();

        // 构造足够大的历史以超过 token-based 裁剪阈值。
        // 格式: user, assistant, tool, user, assistant, tool, ...
        for (int round = 0; round < 7; ++round) {
            AgentMessage userMsg;
            userMsg.role = "user";
            userMsg.content = QString("msg_%1 ").arg(round) + QString(100000, QLatin1Char('u'));
            AgentController::instance().m_conversationHistory.append(userMsg);

            AgentMessage assistantMsg;
            assistantMsg.role = "assistant";
            assistantMsg.content = QString("resp_%1 ").arg(round) + QString(100000, QLatin1Char('a'));
            if (round % 2 == 0) {
                QJsonArray tcs;
                QJsonObject tc;
                tc["id"] = QString("call_%1").arg(round);
                tc["type"] = "function";
                tc["name"] = "get_flow_state";
                tcs.append(tc);
                assistantMsg.toolCalls = tcs;
            }
            AgentController::instance().m_conversationHistory.append(assistantMsg);

            if (round % 2 == 0) {
                AgentMessage toolMsg;
                toolMsg.role = "tool";
                toolMsg.toolCallId = QString("call_%1").arg(round);
                toolMsg.content = "{\"status\":\"ok\"}";
                AgentController::instance().m_conversationHistory.append(toolMsg);
            }
        }

        int sizeBefore = AgentController::instance().m_conversationHistory.size();
        QVERIFY(sizeBefore > 15);

        AgentController::instance().trimHistoryIfNeeded();

        int sizeAfter = AgentController::instance().conversationHistorySize();
        QVERIFY(sizeAfter < sizeBefore);

        // 验证第一条消息是 user（完整轮次的起点）
        QCOMPARE(AgentController::instance().m_conversationHistory.first().role, QString("user"));

        // 验证没有孤立的 tool message（每个 tool 前面都有带 toolCalls 的 assistant）
        for (int i = 0; i < AgentController::instance().m_conversationHistory.size(); ++i) {
            if (AgentController::instance().m_conversationHistory[i].role == "tool") {
                bool foundMatchingAssistant = false;
                for (int j = i - 1; j >= 0; --j) {
                    if (AgentController::instance().m_conversationHistory[j].role == "assistant") {
                        QJsonArray tcs = AgentController::instance().m_conversationHistory[j].toolCalls;
                        for (const QJsonValue& v : tcs) {
                            if (v.toObject()["id"].toString() ==
                                AgentController::instance().m_conversationHistory[i].toolCallId) {
                                foundMatchingAssistant = true;
                                break;
                            }
                        }
                        break; // 只检查最近的 assistant
                    }
                }
                QVERIFY2(foundMatchingAssistant,
                         qPrintable(QString("Tool message at index %1 has no matching assistant tool_call").arg(i)));
            }
        }
    }

    void testTrimHistoryEnforcesHardMessageLimit() {
        AgentController::instance().clearConversation();
        for (int i = 0; i < 40; ++i) {
            AgentMessage user;
            user.role = QStringLiteral("user");
            user.content = QStringLiteral("user-%1").arg(i);
            AgentController::instance().m_conversationHistory.append(user);
            AgentMessage assistant;
            assistant.role = QStringLiteral("assistant");
            assistant.content = QStringLiteral("assistant-%1").arg(i);
            AgentController::instance().m_conversationHistory.append(assistant);
        }

        AgentController::instance().trimHistoryIfNeeded();

        QVERIFY(AgentController::instance().m_conversationHistory.size() <= 64);
        QCOMPARE(AgentController::instance().m_conversationHistory.first().role, QStringLiteral("user"));
        QCOMPARE(AgentController::instance().m_conversationHistory.last().content, QStringLiteral("assistant-39"));
    }

    void testTrimHistoryEnforcesHardLimitWithinSingleUserTurn() {
        AgentController::instance().clearConversation();
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("single request");
        AgentController::instance().m_conversationHistory.append(user);
        for (int i = 0; i < 35; ++i) {
            AgentMessage assistant;
            assistant.role = QStringLiteral("assistant");
            assistant.toolCalls = QJsonArray{QJsonObject{{"id", QStringLiteral("call-%1").arg(i)}}};
            AgentController::instance().m_conversationHistory.append(assistant);
            AgentMessage tool;
            tool.role = QStringLiteral("tool");
            tool.toolCallId = QStringLiteral("call-%1").arg(i);
            AgentController::instance().m_conversationHistory.append(tool);
        }

        AgentController::instance().trimHistoryIfNeeded();

        QVERIFY(AgentController::instance().m_conversationHistory.size() <= 64);
        QCOMPARE(AgentController::instance().m_conversationHistory.first().role, QStringLiteral("user"));
        QVERIFY(AgentController::instance().m_conversationHistory.size() > 1);
        QCOMPARE(AgentController::instance().m_conversationHistory[1].role, QStringLiteral("assistant"));
    }

    void testTrimHistoryAccountsForImageBytes() {
        AgentController::instance().clearConversation();
        AgentMessage imageTurn;
        imageTurn.role = QStringLiteral("user");
        imageTurn.content = QStringLiteral("old image");
        imageTurn.images.append({QByteArray(2 * 1024 * 1024, 'x'), QStringLiteral("image/png"), QString()});
        AgentController::instance().m_conversationHistory.append(imageTurn);
        AgentMessage response;
        response.role = QStringLiteral("assistant");
        response.content = QStringLiteral("old response");
        AgentController::instance().m_conversationHistory.append(response);
        AgentMessage latest;
        latest.role = QStringLiteral("user");
        latest.content = QStringLiteral("latest");
        AgentController::instance().m_conversationHistory.append(latest);

        AgentController::instance().trimHistoryIfNeeded();

        QCOMPARE(AgentController::instance().m_conversationHistory.size(), 1);
        QCOMPARE(AgentController::instance().m_conversationHistory.first().content, QStringLiteral("latest"));
    }

    // ---------- B3：上下文保护 / 预算失败关闭 ----------

    void testContextBudgetExceededFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);

        controller.sendUserMessage(QString(200000, QLatin1Char('x')));

        QCOMPARE(m_mock->requestCount, 0);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(controller.m_conversationHistory.size(), 1); // 保留可诊断的用户请求
        QVERIFY(!errorSpy.isEmpty());
        QVERIFY2(errorSpy.last().at(0).toString().contains(QStringLiteral("context_budget_exceeded")),
                 qPrintable(errorSpy.last().at(0).toString()));
    }

    void testSystemPromptOverBudgetFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        const QString original = controller.systemPrompt();
        controller.setSystemPrompt(QString(200000, QLatin1Char('s')));

        controller.sendUserMessage(QStringLiteral("hello"));

        const int requests = m_mock->requestCount;
        const AgentController::AgentState finalState = controller.state();
        controller.setSystemPrompt(original); // 先恢复，避免失败提前 return 污染后续用例
        QCOMPARE(requests, 0);
        QCOMPARE(finalState, AgentController::AgentState::Idle);
    }

    void testImageAttachmentOverBudgetFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);

        QImage noise(700, 700, QImage::Format_RGB32);
        quint32 seed = 0x12345678u;
        for (int y = 0; y < noise.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(noise.scanLine(y));
            for (int x = 0; x < noise.width(); ++x) {
                seed = seed * 1664525u + 1013904223u;
                const quint32 value = seed;
                line[x] = qRgb(value & 0xff, (value >> 8) & 0xff, (value >> 16) & 0xff);
            }
        }
        controller.sendUserMessageWithImages(QStringLiteral("inspect"), {QPixmap::fromImage(noise)});

        QCOMPARE(m_mock->requestCount, 0);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    void testInFlightToolCallIsProtected() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.m_conversationHistory.clear();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("current request");
        controller.m_conversationHistory.append(user);

        AgentMessage inFlight;
        inFlight.role = QStringLiteral("assistant");
        inFlight.toolCalls = QJsonArray{QJsonObject{{"id", "pending-call"}}};
        inFlight.content = QString(200000, QLatin1Char('a'));
        controller.m_conversationHistory.append(inFlight);

        controller.trimHistoryIfNeeded();

        QVERIFY(!controller.m_conversationHistory.isEmpty());
        QCOMPARE(controller.m_conversationHistory.constFirst().content, QStringLiteral("current request"));
        const QJsonArray expectedGroup{QJsonObject{{"id", "pending-call"}}};
        QVERIFY(controller.m_conversationHistory.constLast().toolCalls == expectedGroup);
    }

    void testAutoInjectedImageDoesNotCreateTaskBoundary() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.m_conversationHistory.clear();

        AgentMessage original;
        original.role = QStringLiteral("user");
        original.content = QStringLiteral("original task");
        controller.m_conversationHistory.append(original);

        AgentMessage oldGroup;
        oldGroup.role = QStringLiteral("assistant");
        oldGroup.toolCalls = QJsonArray{QJsonObject{{"id", "old-call"}}};
        controller.m_conversationHistory.append(oldGroup);
        AgentMessage oldResult;
        oldResult.role = QStringLiteral("tool");
        oldResult.toolCallId = QStringLiteral("old-call");
        oldResult.content = QString(200000, QLatin1Char('o'));
        controller.m_conversationHistory.append(oldResult);

        AgentMessage recentGroup;
        recentGroup.role = QStringLiteral("assistant");
        recentGroup.toolCalls = QJsonArray{QJsonObject{{"id", "recent-call"}}};
        controller.m_conversationHistory.append(recentGroup);
        AgentMessage recentResult;
        recentResult.role = QStringLiteral("tool");
        recentResult.toolCallId = QStringLiteral("recent-call");
        recentResult.content = QStringLiteral("recent result");
        controller.m_conversationHistory.append(recentResult);

        AgentMessage injected;
        injected.role = QStringLiteral("user");
        injected.content = QStringLiteral("Images produced by the tool calls above");
        injected.internalAutoInjected = true;
        controller.m_conversationHistory.append(injected);

        controller.trimHistoryIfNeeded();

        QCOMPARE(controller.m_conversationHistory.constFirst().role, QStringLiteral("user"));
        QCOMPARE(controller.m_conversationHistory.constFirst().content, QStringLiteral("original task"));
        bool oldGone = true;
        bool recentKept = false;
        bool injectedKept = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("old-call"))
                oldGone = false;
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("recent-call"))
                recentKept = true;
            if (message.internalAutoInjected)
                injectedKept = true;
        }
        QVERIFY2(oldGone, "the older complete tool group must be trimmed first");
        QVERIFY2(recentKept, "the latest complete tool group is protected");
        QVERIFY2(injectedKept, "the auto-injected image message belongs to the protected recent group");
    }

    // B3：保护集合自身超过消息数硬上限时确定性停止（不放宽到 65+ 条）
    void testProtectedMessageLimitFailsClosed() {
        AgentController& controller = AgentController::instance();
        controller.m_conversationHistory.clear();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("one request with many tool results");
        controller.m_conversationHistory.append(user);

        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        QJsonArray calls;
        for (int i = 0; i < 64; ++i)
            calls.append(QJsonObject{{"id", QStringLiteral("bulk-%1").arg(i)}});
        assistant.toolCalls = calls;
        controller.m_conversationHistory.append(assistant);
        for (int i = 0; i < 64; ++i) {
            AgentMessage tool;
            tool.role = QStringLiteral("tool");
            tool.toolCallId = QStringLiteral("bulk-%1").arg(i);
            tool.content = QStringLiteral("{}");
            controller.m_conversationHistory.append(tool);
        }
        QVERIFY(controller.m_conversationHistory.size() > 64);

        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QVERIFY(!controller.sendPreparedContext());
        QCOMPARE(m_mock->requestCount, 0);
        QVERIFY(!errorSpy.isEmpty());
        QVERIFY(errorSpy.last().at(0).toString().contains(QStringLiteral("context_budget_exceeded")));
    }

    void testInvalidToolPairingBlocksSend() {
        AgentController& controller = AgentController::instance();
        controller.m_conversationHistory.clear();
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("request");
        controller.m_conversationHistory.append(user);
        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.toolCalls = QJsonArray{QJsonObject{{"id", "missing-result"}}};
        controller.m_conversationHistory.append(assistant);

        QString error;
        QVERIFY(!controller.historyPairsValid(&error));
        QVERIFY2(error.contains(QStringLiteral("missing")), qPrintable(error));

        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QVERIFY(!controller.sendPreparedContext());
        QCOMPARE(m_mock->requestCount, 0);
        QVERIFY(!errorSpy.isEmpty());
        QVERIFY(errorSpy.last().at(0).toString().contains(QStringLiteral("context_invalid_history")));
    }

    // ---------- 最大轮数限制 ----------

    void testAutopilotStopsAtTurnLimit() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        AgentController::instance().sendUserMessage("Run");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);
        for (int turn = 0; turn < 21; ++turn) {
            AgentResponse resp;
            resp.success = true;
            resp.content = QString("Turn %1").arg(turn);
            QJsonObject tc;
            tc["id"] = QString("call_%1").arg(turn);
            tc["type"] = "function";
            tc["name"] = "get_available_plugins";
            resp.toolCalls.append(tc);

            m_mock->emitResponse(resp);

            if (turn < 20) {
                QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
            }
        }
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.takeFirst()[0].toString().contains("轮数上限"));

        // 阶段1 审核整改 R3：触发轮数上限的那次 tool call 也恰有一个结构化结果——
        // 对话不留孤立 tool_calls（20 条执行结果 + 1 条 TURN_LIMIT_EXCEEDED 补位）
        int toolMsgs = 0;
        bool sawTurnLimitResult = false;
        for (const AgentMessage& m : AgentController::instance().m_conversationHistory) {
            if (m.role != QLatin1String("tool"))
                continue;
            ++toolMsgs;
            if (m.toolCallId == QLatin1String("call_20") && m.content.contains(QLatin1String("TURN_LIMIT_EXCEEDED")))
                sawTurnLimitResult = true;
        }
        QCOMPARE(toolMsgs, 21);
        QVERIFY2(sawTurnLimitResult, "turn-limit batch must still answer every call id");
    }

    void testAutopilotRejectsTooManyToolCallsInOneTurn() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        AgentController::instance().sendUserMessage("Run many tools");
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);
        QSignalSpy logSpy(&AgentController::instance(), &AgentController::actionLogEntryAdded);
        AgentResponse resp;
        resp.success = true;
        resp.content = "Too many tools";
        for (int i = 0; i < 11; ++i) {
            QJsonObject tc;
            tc["id"] = QString("call_%1").arg(i);
            tc["type"] = "function";
            tc["name"] = "get_flow_state";
            resp.toolCalls.append(tc);
        }

        m_mock->emitResponse(resp);

        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.takeFirst()[0].toString().contains("工具数量上限"));

        // 阶段1 审核整改 R3：超限批次逐 call 补 BATCH_LIMIT_EXCEEDED 结果——
        // 11 个原始 callId 各恰有一条 tool 消息与一条 skipped 审计,零执行
        QSet<QString> answeredIds;
        int batchLimitMsgs = 0;
        for (const AgentMessage& m : AgentController::instance().m_conversationHistory) {
            if (m.role != QLatin1String("tool"))
                continue;
            answeredIds.insert(m.toolCallId);
            if (m.content.contains(QLatin1String("BATCH_LIMIT_EXCEEDED")))
                ++batchLimitMsgs;
        }
        QCOMPARE(batchLimitMsgs, 11);
        QCOMPARE(answeredIds.size(), 11);
        int skippedAudits = 0;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry e = args.at(0).value<AgentActionLogEntry>();
            if (e.result == QLatin1String("skipped"))
                ++skippedAudits;
        }
        QCOMPARE(skippedAudits, 11);
    }

    // 阶段1 审核整改 R2：确认绑定 confirmationId——旧批次迟到事件被拒绝（零副作用,
    // 当前批次保持待确认）;正确 id 批准后才执行;批次消费后重复 id 同样拒绝
    void testConfirmationIdMismatchRejectedZeroSideEffects() {
        AgentController& ctrl = AgentController::instance();
        ProjectManager::instance().closeProject(); // 隔离
        QVERIFY(!ctrl.actor()->executeTool("create_project", QJsonObject{{"name", "cid_base"}}).contains("error"));

        // 批次1：进入待确认并取回绑定 id（Bridge 响应内嵌完整载荷）
        const QJsonObject p1 =
            ctrl.handleToolCall("create_project", QJsonObject{{"name", "cid_one"}, {"onUnsaved", "discard"}});
        QCOMPARE(p1.value("status").toString(), QStringLiteral("pending_confirmation"));
        const QString id1 = ctrl.pendingConfirmationId();
        QVERIFY(!id1.isEmpty());
        QCOMPARE(p1.value("confirmation").toObject().value("confirmationId").toString(), id1);
        QVERIFY(ctrl.rejectPendingTools(id1));

        // 批次2：新 id 与批次1 不同
        const QJsonObject p2 =
            ctrl.handleToolCall("create_project", QJsonObject{{"name", "cid_two"}, {"onUnsaved", "discard"}});
        QCOMPARE(p2.value("status").toString(), QStringLiteral("pending_confirmation"));
        const QString id2 = ctrl.pendingConfirmationId();
        QVERIFY(id2 != id1);

        // 批次1 的迟到确认事件：错配 → 拒绝,零副作用,批次2 保持待确认
        QVERIFY(!ctrl.confirmPendingTools(id1));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Confirming);
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("cid_base"));
        QVERIFY(ctrl.hasPendingTools());

        // 正确 id 批准批次2 → 执行
        QVERIFY(ctrl.confirmPendingTools(id2));
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("cid_two"));

        // 批次已消费后,原正确 id 再次迟到 → 拒绝,零副作用
        QVERIFY(!ctrl.confirmPendingTools(id2));
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("cid_two"));
    }

    // 阶段1 复审2 R1：空 confirmationId 一律拒绝——批准必须绑定到所展示批次,
    // 无参旁路已从接口移除（默认参数删除,空 id 显式失败）
    void testEmptyConfirmationIdRejected() {
        AgentController& ctrl = AgentController::instance();
        ProjectManager::instance().closeProject(); // 隔离
        QVERIFY(!ctrl.actor()->executeTool("create_project", QJsonObject{{"name", "eid_base"}}).contains("error"));

        const QJsonObject p =
            ctrl.handleToolCall("create_project", QJsonObject{{"name", "eid_target"}, {"onUnsaved", "discard"}});
        QCOMPARE(p.value("status").toString(), QStringLiteral("pending_confirmation"));

        // 空 id 批准被拒：零副作用,批次保持待确认
        QVERIFY(!ctrl.confirmPendingTools(QString()));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Confirming);
        QVERIFY(ctrl.hasPendingTools());
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("eid_base"));
        // 空 id 拒绝请求同样被忽略（批次仍在,等待有效决定）
        QVERIFY(!ctrl.rejectPendingTools(QString()));
        QVERIFY(ctrl.hasPendingTools());
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("eid_base"));

        // 绑定 id 才能完成有效决定
        QVERIFY(ctrl.rejectPendingTools(ctrl.pendingConfirmationId()));
        QVERIFY(!ctrl.hasPendingTools());
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("eid_base"));
    }

    void testMalformedToolArgumentsRejectWholeBatch() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        QSignalSpy runSpy(&RunEngine::instance(), &RunEngine::runStarted);
        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);
        ctrl.sendUserMessage(QStringLiteral("检查流程后运行"));

        AgentResponse response;
        response.success = true;
        response.toolCalls.append(
            QJsonObject{{"id", "call_read"}, {"name", "get_flow_state"}, {"arguments", QJsonObject()}});
        response.toolCalls.append(QJsonObject{{"id", "call_bad"},
                                              {"type", "function"},
                                              {"function", QJsonObject{{"name", "run_flow"}, {"arguments", "{bad"}}}});
        m_mock->emitResponse(response);

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        const QString summary = errorSpy.takeFirst().at(0).toString();
        QVERIFY(summary.contains(QStringLiteral("call_read")));
        QVERIFY(summary.contains(QStringLiteral("call_bad")));
        QVERIFY(summary.contains(QStringLiteral("未执行")));
        QVERIFY(!ctrl.hasPendingTools());
        QCOMPARE(runSpy.count(), 0);
        QCOMPARE(logSpy.count(), 2);
        QMap<QString, QString> results;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role == QLatin1String("tool"))
                results.insert(message.toolCallId, message.content);
        }
        QCOMPARE(results.size(), 2);
        QVERIFY(results.value(QStringLiteral("call_read")).contains(QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR")));
        QVERIFY(results.value(QStringLiteral("call_bad")).contains(QStringLiteral("INVALID_PARAM")));
        ProjectManager::instance().closeProject();
    }

    void testParameterCorrectionRetriesUntilCorrectedToolSucceeds() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("correction_module");
        module.moduleId = QStringLiteral("GrabImage");
        project->addModule(module);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);

        ctrl.sendUserMessage(QStringLiteral("Inspect the flow"));
        QCOMPARE(m_mock->sendCount, 1);

        AgentResponse invalid;
        invalid.success = true;
        invalid.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_invalid_params")},
                        {QStringLiteral("name"), QStringLiteral("set_module_position")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                  {QStringLiteral("x"), 1.5},
                                                                  {QStringLiteral("y"), 2}}}});
        m_mock->emitResponse(invalid);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 2);
        QVERIFY(errorSpy.isEmpty());

        AgentResponse corrected;
        corrected.success = true;
        corrected.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_corrected_params")},
                        {QStringLiteral("name"), QStringLiteral("set_module_position")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                  {QStringLiteral("x"), 10},
                                                                  {QStringLiteral("y"), 20}}}});
        m_mock->emitResponse(corrected);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 3);

        AgentResponse final;
        final.success = true;
        final.content = QStringLiteral("The flow is empty.");
        m_mock->emitResponse(final);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(errorSpy.isEmpty());

        QMap<QString, int> toolResultCounts;
        QString invalidResult;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role != QLatin1String("tool"))
                continue;
            ++toolResultCounts[message.toolCallId];
            if (message.toolCallId == QLatin1String("call_invalid_params"))
                invalidResult = message.content;
        }
        QCOMPARE(toolResultCounts.value(QStringLiteral("call_invalid_params")), 1);
        QCOMPARE(toolResultCounts.value(QStringLiteral("call_corrected_params")), 1);
        QVERIFY(invalidResult.contains(QStringLiteral("INVALID_PARAM")));
        QCOMPARE(project->moduleById(module.id)->posX, 10.0);
        QCOMPARE(project->moduleById(module.id)->posY, 20.0);
        ProjectManager::instance().closeProject();
    }

    void testMissingParameterAndUnknownToolCanBeCorrected() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("missing_param_correction_module");
        module.moduleId = QStringLiteral("GrabImage");
        project->addModule(module);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);

        ctrl.sendUserMessage(QStringLiteral("Set the module position"));
        AgentResponse missing;
        missing.success = true;
        missing.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_missing_param")},
                                             {QStringLiteral("name"), QStringLiteral("set_module_position")},
                                             {QStringLiteral("arguments"), QJsonObject()}});
        m_mock->emitResponse(missing);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 2);

        AgentResponse corrected;
        corrected.success = true;
        corrected.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_missing_corrected")},
                        {QStringLiteral("name"), QStringLiteral("set_module_position")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                  {QStringLiteral("x"), 11},
                                                                  {QStringLiteral("y"), 12}}}});
        m_mock->emitResponse(corrected);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 3);

        AgentResponse final;
        final.success = true;
        final.content = QStringLiteral("Position updated.");
        m_mock->emitResponse(final);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(project->moduleById(module.id)->posX, 11.0);
        QCOMPARE(project->moduleById(module.id)->posY, 12.0);

        ctrl.sendUserMessage(QStringLiteral("Inspect the current flow"));
        AgentResponse unknown;
        unknown.success = true;
        unknown.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_unknown_tool")},
                                             {QStringLiteral("name"), QStringLiteral("unknown_tool_for_test")},
                                             {QStringLiteral("arguments"), QJsonObject()}});
        m_mock->emitResponse(unknown);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 5);

        AgentResponse known;
        known.success = true;
        known.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_unknown_corrected")},
                                           {QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                           {QStringLiteral("arguments"), QJsonObject()}});
        m_mock->emitResponse(known);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 6);
        m_mock->emitResponse(final);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(errorSpy.isEmpty());

        QMap<QString, QJsonObject> results;
        QMap<QString, int> resultCounts;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role == QLatin1String("tool")) {
                ++resultCounts[message.toolCallId];
                results.insert(message.toolCallId, QJsonDocument::fromJson(message.content.toUtf8()).object());
            }
        }
        QCOMPARE(results.value(QStringLiteral("call_missing_param")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("MISSING_PARAM"));
        QCOMPARE(results.value(QStringLiteral("call_unknown_tool")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("UNKNOWN_TOOL"));
        QCOMPARE(resultCounts.value(QStringLiteral("call_missing_param")), 1);
        QCOMPARE(resultCounts.value(QStringLiteral("call_missing_corrected")), 1);
        QCOMPARE(resultCounts.value(QStringLiteral("call_unknown_tool")), 1);
        QCOMPARE(resultCounts.value(QStringLiteral("call_unknown_corrected")), 1);
        ProjectManager::instance().closeProject();
    }

    void testRepeatedUnchangedParameterFailureStopsAndNewRequestResetsBudget() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("repeat_correction_module");
        module.moduleId = QStringLiteral("GrabImage");
        project->addModule(module);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        const auto emitInvalidCall = [this, &module](const QString& callId) {
            AgentResponse response;
            response.success = true;
            response.toolCalls.append(
                QJsonObject{{QStringLiteral("id"), callId},
                            {QStringLiteral("name"), QStringLiteral("set_module_position")},
                            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                      {QStringLiteral("x"), 1.5},
                                                                      {QStringLiteral("y"), 2}}}});
            m_mock->emitResponse(response);
        };

        ctrl.sendUserMessage(QStringLiteral("Inspect the flow"));
        emitInvalidCall(QStringLiteral("call_same_error_1"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 2);
        emitInvalidCall(QStringLiteral("call_same_error_2"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(m_mock->sendCount, 2);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.takeFirst().at(0).toString().contains(QStringLiteral("连续两次相同参数错误")));

        ctrl.sendUserMessage(QStringLiteral("A new request gets a fresh correction budget"));
        QCOMPARE(m_mock->sendCount, 3);
        emitInvalidCall(QStringLiteral("call_new_request_error"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->sendCount, 4);
        AgentResponse final;
        final.success = true;
        final.content = QStringLiteral("I cannot inspect this yet.");
        m_mock->emitResponse(final);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 0);
        ProjectManager::instance().closeProject();
    }

    void testNonConsecutiveRepeatedParameterErrorDoesNotStopCorrection() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("alternating_correction_module");
        module.moduleId = QStringLiteral("GrabImage");
        project->addModule(module);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        const auto emitInvalidCall = [this, &module](const QString& callId, double x) {
            AgentResponse response;
            response.success = true;
            response.toolCalls.append(
                QJsonObject{{QStringLiteral("id"), callId},
                            {QStringLiteral("name"), QStringLiteral("set_module_position")},
                            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                      {QStringLiteral("x"), x},
                                                                      {QStringLiteral("y"), 2}}}});
            m_mock->emitResponse(response);
        };

        ctrl.sendUserMessage(QStringLiteral("Correct alternating parameter mistakes"));
        emitInvalidCall(QStringLiteral("call_pattern_a1"), 1.5);
        QCOMPARE(m_mock->sendCount, 2);
        emitInvalidCall(QStringLiteral("call_pattern_b"), 2.5);
        QCOMPARE(m_mock->sendCount, 3);
        emitInvalidCall(QStringLiteral("call_pattern_a2"), 1.5);
        QCOMPARE(m_mock->sendCount, 4);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QVERIFY(errorSpy.isEmpty());

        AgentResponse final;
        final.success = true;
        final.content = QStringLiteral("The corrections did not repeat consecutively.");
        m_mock->emitResponse(final);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(errorSpy.isEmpty());
        ProjectManager::instance().closeProject();
    }

    void testDeniedCallAndRuntimeFailureDoNotTriggerCorrection() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Observer);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("denied_correction_module");
        module.moduleId = QStringLiteral("GrabImage");
        module.name = QStringLiteral("Original");
        project->addModule(module);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        ctrl.sendUserMessage(QStringLiteral("Inspect this mixed batch"));
        const int requestsBeforeBatch = m_mock->sendCount;

        AgentResponse response;
        response.success = true;
        response.toolCalls.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("call_denied_edit")},
            {QStringLiteral("name"), QStringLiteral("rename_module")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                      {QStringLiteral("name"), QStringLiteral("MustNotChange")}}}});
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_invalid_read")},
                        {QStringLiteral("name"), QStringLiteral("get_module_output")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id}}}});
        m_mock->emitResponse(response);

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(m_mock->sendCount, requestsBeforeBatch);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(!errorSpy.takeFirst().at(0).toString().isEmpty());
        QCOMPARE(project->moduleById(module.id)->name, QStringLiteral("Original"));

        QMap<QString, QJsonObject> results;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role == QLatin1String("tool"))
                results.insert(message.toolCallId, QJsonDocument::fromJson(message.content.toUtf8()).object());
        }
        QCOMPARE(results.value(QStringLiteral("call_denied_edit")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(results.value(QStringLiteral("call_invalid_read")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("MISSING_PARAM"));
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
    }

    void testLlmErrorClearsParameterCorrectionState() {
        AgentController& ctrl = AgentController::instance();
        ctrl.sendUserMessage(QStringLiteral("Start a request that will fail"));
        ctrl.m_parameterCorrectionRetries = 2;
        ctrl.m_parameterFailureSignatures.insert(QStringLiteral("set_module_position|INVALID_PARAM|{}"));

        m_mock->emitError(QStringLiteral("provider disconnected"));

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(ctrl.m_parameterCorrectionRetries, 0);
        QVERIFY(ctrl.m_parameterFailureSignatures.isEmpty());
    }

    void testSystemPromptExplainsWhenProviderToolsAreDisabled() {
        AgentController& ctrl = AgentController::instance();
        m_mock->toolCallingSupported = false;
        QVERIFY(!m_mock->supportsToolCalling());

        const QString context = ctrl.buildContext();

        QVERIFY(context.contains(QStringLiteral("Tool calling is disabled for this request")));
        QVERIFY(context.contains(QStringLiteral("Answer from the supplied conversation and context only")));
        QVERIFY(context.contains(QStringLiteral("state clearly when the request requires enabling tools")));
        QVERIFY(context.contains(QStringLiteral("5. When tools are available, use read_documentation")));
        QVERIFY(context.contains(QStringLiteral("6. When tools are available, for quick measurement")));
        QVERIFY(context.contains(
            QStringLiteral("7. When tools are available and a tool returns INVALID_PARAM or MISSING_PARAM")));
        QVERIFY(!context.contains(QStringLiteral("5. Use read_documentation when unsure")));
        QVERIFY(!context.contains(QStringLiteral("6. For quick measurement, list viewports")));
    }

    void testDisabledProviderToolCallIsRejectedWithCallIdResults() {
        AgentController& ctrl = AgentController::instance();
        m_mock->toolCallingSupported = false;
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("disabled_provider_module");
        module.moduleId = QStringLiteral("GrabImage");
        module.name = QStringLiteral("Original");
        project->addModule(module);

        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        QSignalSpy auditSpy(&ctrl, &AgentController::actionLogEntryAdded);
        ctrl.sendUserMessage(QStringLiteral("Rename the module"));
        QCOMPARE(m_mock->sendCount, 1);
        QVERIFY(m_mock->lastCtx.systemPrompt.contains(QStringLiteral("Tool calling is disabled")));

        AgentResponse response;
        response.success = true;
        response.toolCalls.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("call_tools_disabled")},
            {QStringLiteral("name"), QStringLiteral("rename_module")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                      {QStringLiteral("name"), QStringLiteral("MustNotChange")}}}});
        m_mock->emitResponse(response);

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(m_mock->sendCount, 1);
        QCOMPARE(project->moduleById(module.id)->name, QStringLiteral("Original"));
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.takeFirst().at(0).toString().contains(QStringLiteral("no tools were executed")));
        QCOMPARE(auditSpy.count(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.first().first()).result, QStringLiteral("denied"));

        int matchingResults = 0;
        QJsonObject callResult;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role != QLatin1String("tool") || message.toolCallId != QLatin1String("call_tools_disabled"))
                continue;
            ++matchingResults;
            callResult = QJsonDocument::fromJson(message.content.toUtf8()).object();
        }
        QCOMPARE(matchingResults, 1);
        QCOMPARE(callResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("TOOLS_DISABLED"));
        QCOMPARE(callResult.value(QStringLiteral("retryable")).toBool(), false);
        ProjectManager::instance().closeProject();
    }

    void testParameterCorrectionRetryLimitIsThree() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("retry_limit_module");
        module.moduleId = QStringLiteral("GrabImage");
        project->addModule(module);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        ctrl.sendUserMessage(QStringLiteral("Inspect the flow"));
        for (int attempt = 1; attempt <= 4; ++attempt) {
            AgentResponse invalid;
            invalid.success = true;
            invalid.toolCalls.append(
                QJsonObject{{QStringLiteral("id"), QStringLiteral("call_retry_%1").arg(attempt)},
                            {QStringLiteral("name"), QStringLiteral("set_module_position")},
                            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                      {QStringLiteral("x"), 0.5 + attempt},
                                                                      {QStringLiteral("y"), 2}}}});
            m_mock->emitResponse(invalid);
            if (attempt <= 3) {
                QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
                QCOMPARE(m_mock->sendCount, attempt + 1);
            }
        }
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(m_mock->sendCount, 4);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.takeFirst().at(0).toString().contains(QStringLiteral("参数纠错次数达到上限（3）")));
        int toolResults = 0;
        for (const AgentMessage& message : ctrl.m_conversationHistory)
            toolResults += message.role == QLatin1String("tool") ? 1 : 0;
        QCOMPARE(toolResults, 4);
        ProjectManager::instance().closeProject();
    }

    void testPartialFailureSummaryPreservesCallOrderAndHidesRawValues() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("slice20_canary_instance");
        module.moduleId = QStringLiteral("GrabImage");
        module.name = QStringLiteral("Original");
        project->addModule(module);

        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);
        ctrl.sendUserMessage(QStringLiteral("Run a mixed batch"));
        const int requestsBeforeBatch = m_mock->sendCount;
        AgentResponse response;
        response.success = true;
        auto appendCall = [&response](const QString& id, const QString& name, const QJsonObject& args) {
            response.toolCalls.append(QJsonObject{{QStringLiteral("id"), id},
                                                  {QStringLiteral("type"), QStringLiteral("function")},
                                                  {QStringLiteral("name"), name},
                                                  {QStringLiteral("arguments"), args}});
        };
        appendCall(QStringLiteral("call_edit_1"), QStringLiteral("rename_module"),
                   {{QStringLiteral("instanceId"), module.id}, {QStringLiteral("name"), QStringLiteral("Edited")}});
        appendCall(QStringLiteral("call_edit_2"), QStringLiteral("set_module_enabled"),
                   {{QStringLiteral("instanceId"), module.id}, {QStringLiteral("enabled"), false}});
        appendCall(QStringLiteral("call_noop"), QStringLiteral("rename_module"),
                   {{QStringLiteral("instanceId"), module.id}, {QStringLiteral("name"), QStringLiteral("Edited")}});
        const QString canary = QStringLiteral("slice20_private_canary_value");
        appendCall(QStringLiteral("call_fail"), QStringLiteral("rename_module"),
                   {{QStringLiteral("instanceId"), canary}, {QStringLiteral("name"), QStringLiteral("Leak me")}});
        appendCall(QStringLiteral("call_skipped"), QStringLiteral("rename_module"),
                   {{QStringLiteral("instanceId"), module.id}, {QStringLiteral("name"), QStringLiteral("MustNotRun")}});
        m_mock->emitResponse(response);

        QCOMPARE(m_mock->sendCount, requestsBeforeBatch);
        QCOMPARE(errorSpy.count(), 1);
        const QString summary = errorSpy.takeFirst().at(0).toString();
        QVERIFY(summary.contains(QStringLiteral("批次部分失败")));
        QVERIFY(summary.contains(QStringLiteral("已发生的副作用未自动回滚")));
        QVERIFY(summary.contains(QStringLiteral("call_edit_1")));
        QVERIFY(summary.contains(QStringLiteral("call_edit_2")));
        QVERIFY(summary.contains(QStringLiteral("call_noop")));
        QVERIFY(summary.contains(QStringLiteral("call_fail")));
        QVERIFY(summary.contains(QStringLiteral("call_skipped")));
        QVERIFY(summary.contains(QStringLiteral("已完成")));
        QVERIFY(summary.contains(QStringLiteral("失败")));
        QVERIFY(summary.contains(QStringLiteral("未执行")));
        QVERIFY(summary.contains(QStringLiteral("无操作")));
        QVERIFY(summary.indexOf(QStringLiteral("call_edit_1")) < summary.indexOf(QStringLiteral("call_edit_2")));
        QVERIFY(summary.indexOf(QStringLiteral("call_edit_2")) < summary.indexOf(QStringLiteral("call_noop")));
        QVERIFY(summary.indexOf(QStringLiteral("call_noop")) < summary.indexOf(QStringLiteral("call_fail")));
        QVERIFY(summary.indexOf(QStringLiteral("call_fail")) < summary.indexOf(QStringLiteral("call_skipped")));
        QVERIFY(!summary.contains(canary));
        QCOMPARE(project->moduleById(module.id)->name, QStringLiteral("Edited"));
        QVERIFY(!project->moduleById(module.id)->enabled);
        QCOMPARE(logSpy.count(), 5);

        QMap<QString, QString> toolResults;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role == QLatin1String("tool"))
                toolResults.insert(message.toolCallId, message.content);
        }
        QCOMPARE(toolResults.size(), 5);
        QVERIFY(toolResults.value(QStringLiteral("call_edit_1")).contains(QStringLiteral("renamed")));
        QVERIFY2(toolResults.value(QStringLiteral("call_fail")).contains(QStringLiteral("INSTANCE_NOT_FOUND")),
                 qPrintable(toolResults.value(QStringLiteral("call_fail"))));
        QVERIFY(toolResults.value(QStringLiteral("call_skipped"))
                    .contains(QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR")));
        ProjectManager::instance().closeProject();
    }

    void testG4PartialFailedControllerBatchSerializesUndoSnapshots() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        project->setName(QStringLiteral("Slice21 serialization"));

        ModuleInstance first;
        first.id = QStringLiteral("slice21_first");
        first.moduleId = QStringLiteral("test.compute");
        first.name = QStringLiteral("Before");
        first.posX = 17;
        first.posY = -23;
        first.params = QJsonObject{
            {QStringLiteral("nested"), QJsonObject{{QStringLiteral("nullable"), QJsonValue(QJsonValue::Null)}}},
            {QStringLiteral("typed"), QJsonArray{1, QStringLiteral("two"), true}}};
        first.note = QStringLiteral("keep first note");
        first.enabled = false;
        first.breakpoint = true;
        project->addModule(first);

        ModuleInstance second;
        second.id = QStringLiteral("slice21_second");
        second.moduleId = QStringLiteral("test.compute");
        second.name = QStringLiteral("Second");
        second.posX = -91;
        second.posY = 44;
        second.params = QJsonObject{{QStringLiteral("keep"), 9}};
        second.note = QStringLiteral("keep second note");
        project->addModule(second);
        project->moveModule(second.id, 0);

        ModuleConnection dataEdge;
        dataEdge.fromModuleId = first.id;
        dataEdge.toModuleId = second.id;
        dataEdge.fromOutput = 2;
        dataEdge.toInput = 1;
        dataEdge.fromPort = QStringLiteral("points");
        dataEdge.toPort = QStringLiteral("input");
        dataEdge.edgeType = QStringLiteral("data");
        project->addConnection(dataEdge);
        ModuleConnection controlEdge = dataEdge;
        controlEdge.fromPort = QStringLiteral("next");
        controlEdge.toPort = QStringLiteral("control");
        controlEdge.edgeType = QStringLiteral("control");
        project->addConnection(controlEdge);
        project->setModified(false);

        const QJsonObject baseline = project->toJson();
        const int baseUndoCount = controller.actor()->undoStack()->count();
        const int baseUndoIndex = controller.actor()->undoStack()->index();
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.sendUserMessage(QStringLiteral("Apply a partial edit batch"));

        AgentResponse response;
        response.success = true;
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("slice21_rename")},
                        {QStringLiteral("name"), QStringLiteral("rename_module")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), first.id},
                                                                  {QStringLiteral("name"), QStringLiteral("After")}}}});
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("slice21_position")},
                        {QStringLiteral("name"), QStringLiteral("set_module_position")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), first.id},
                                                                  {QStringLiteral("x"), 301},
                                                                  {QStringLiteral("y"), -402}}}});
        response.toolCalls.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("slice21_runtime_failure")},
            {QStringLiteral("name"), QStringLiteral("rename_module")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("slice21_missing")},
                                                      {QStringLiteral("name"), QStringLiteral("never")}}}});
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("slice21_skipped")},
                        {QStringLiteral("name"), QStringLiteral("set_module_note")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), second.id},
                                                                  {QStringLiteral("note"), QStringLiteral("never")}}}});
        m_mock->emitResponse(response);

        if (controller.state() == AgentController::AgentState::Confirming)
            QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("批次部分失败")));
        QCOMPARE(controller.actor()->undoStack()->count(), baseUndoCount + 2);
        QCOMPARE(controller.actor()->undoStack()->index(), baseUndoIndex + 2);
        QVERIFY(project->isModified());
        QVERIFY(auditSpy.count() >= 4);

        const QStringList callIds{QStringLiteral("slice21_rename"), QStringLiteral("slice21_position"),
                                  QStringLiteral("slice21_runtime_failure"), QStringLiteral("slice21_skipped")};
        QMap<QString, int> toolMessageCounts;
        QMap<QString, QJsonObject> toolResults;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role != QLatin1String("tool"))
                continue;
            ++toolMessageCounts[message.toolCallId];
            toolResults[message.toolCallId] = QJsonDocument::fromJson(message.content.toUtf8()).object();
        }
        for (const QString& callId : callIds)
            QCOMPARE(toolMessageCounts.value(callId), 1);
        QCOMPARE(toolResults.value(callIds.at(0)).value(QStringLiteral("status")).toString(),
                 QStringLiteral("renamed"));
        QCOMPARE(toolResults.value(callIds.at(1)).value(QStringLiteral("status")).toString(),
                 QStringLiteral("updated"));
        QCOMPARE(toolResults.value(callIds.at(2)).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("INSTANCE_NOT_FOUND"));
        QCOMPARE(toolResults.value(callIds.at(3)).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));

        const auto expectedWithEdits = [&baseline, &first](bool includePosition) {
            QJsonObject expected = baseline;
            QJsonArray modules = expected.value(QStringLiteral("modules")).toArray();
            for (int index = 0; index < modules.size(); ++index) {
                QJsonObject module = modules.at(index).toObject();
                if (module.value(QStringLiteral("id")).toString() != first.id)
                    continue;
                module.insert(QStringLiteral("name"), QStringLiteral("After"));
                if (includePosition) {
                    module.insert(QStringLiteral("posX"), 301);
                    module.insert(QStringLiteral("posY"), -402);
                }
                modules.replace(index, module);
                break;
            }
            expected.insert(QStringLiteral("modules"), modules);
            return expected;
        };
        const auto verifyExpected = [](const QJsonObject& expected, const QJsonObject& actual) {
            const QStringList differences =
                ProjectJsonTestHelpers::differences(ProjectJsonTestHelpers::withoutModifiedTimestamp(expected),
                                                    ProjectJsonTestHelpers::withoutModifiedTimestamp(actual));
            QVERIFY2(differences.isEmpty(), qPrintable(differences.join(QLatin1Char('\n'))));
        };
        const QJsonObject actualAfterBatch = project->toJson();
        verifyExpected(expectedWithEdits(true), actualAfterBatch);
        const auto saveAndVerifyReopen = [&project, baseUndoCount, baseUndoIndex](const QString& path,
                                                                                  int expectedUndoIndex) {
            const QJsonObject beforeSave = project->toJson();
            const int stackCount = AgentController::instance().actor()->undoStack()->count();
            const int stackIndex = AgentController::instance().actor()->undoStack()->index();
            QCOMPARE(stackCount, baseUndoCount + 2);
            QCOMPARE(stackIndex, expectedUndoIndex);
            QVERIFY(project->isModified());
            QVERIFY(project->save(path));
            QVERIFY(!project->isModified());
            QCOMPARE(AgentController::instance().actor()->undoStack()->count(), stackCount);
            QCOMPARE(AgentController::instance().actor()->undoStack()->index(), stackIndex);
            QCOMPARE(project->toJson(), beforeSave);
            Project reopened;
            QVERIFY(reopened.load(path));
            const QStringList reopenDifferences = ProjectJsonTestHelpers::differences(beforeSave, reopened.toJson());
            QVERIFY2(reopenDifferences.isEmpty(), qPrintable(reopenDifferences.join(QLatin1Char('\n'))));
            QVERIFY(!reopened.isModified());
        };

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("slice21-batch.dproj"));
        saveAndVerifyReopen(path, baseUndoIndex + 2);

        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(auditSpy.last().first().value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QCOMPARE(project->moduleById(first.id)->name, QStringLiteral("After"));
        QCOMPARE(project->moduleById(first.id)->posX, first.posX);
        QCOMPARE(project->moduleById(first.id)->posY, first.posY);
        verifyExpected(expectedWithEdits(false), project->toJson());
        saveAndVerifyReopen(path, baseUndoIndex + 1);

        QVERIFY(controller.undoLastAgentAction());
        verifyExpected(baseline, project->toJson());
        saveAndVerifyReopen(path, baseUndoIndex);

        controller.actor()->undoStack()->redo();
        verifyExpected(expectedWithEdits(false), project->toJson());
        saveAndVerifyReopen(path, baseUndoIndex + 1);
        controller.actor()->undoStack()->redo();
        verifyExpected(expectedWithEdits(true), project->toJson());
        saveAndVerifyReopen(path, baseUndoIndex + 2);

        const int beforeNoOpCount = controller.actor()->undoStack()->count();
        const int beforeNoOpIndex = controller.actor()->undoStack()->index();
        controller.sendUserMessage(QStringLiteral("Repeat the existing name"));
        AgentResponse noOpResponse;
        noOpResponse.success = true;
        noOpResponse.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("slice21_noop")},
                        {QStringLiteral("name"), QStringLiteral("rename_module")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), first.id},
                                                                  {QStringLiteral("name"), QStringLiteral("After")}}}});
        m_mock->emitResponse(noOpResponse);
        QCOMPARE(controller.actor()->undoStack()->count(), beforeNoOpCount);
        QCOMPARE(controller.actor()->undoStack()->index(), beforeNoOpIndex);
        AgentResponse noOpFinalResponse;
        noOpFinalResponse.success = true;
        noOpFinalResponse.content = QStringLiteral("No change needed");
        m_mock->emitResponse(noOpFinalResponse);
        QTRY_COMPARE(controller.state(), AgentController::AgentState::Idle);
        verifyExpected(expectedWithEdits(true), project->toJson());

        const QJsonObject beforeFirstFailure = project->toJson();
        const bool beforeFirstFailureModified = project->isModified();
        const int beforeFirstFailureCount = controller.actor()->undoStack()->count();
        const int beforeFirstFailureIndex = controller.actor()->undoStack()->index();
        controller.sendUserMessage(QStringLiteral("Fail before editing"));
        AgentResponse firstFailureResponse;
        firstFailureResponse.success = true;
        firstFailureResponse.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("slice21_first_failure")},
                        {QStringLiteral("name"), QStringLiteral("rename_module")},
                        {QStringLiteral("arguments"),
                         QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("slice21_missing_first")},
                                     {QStringLiteral("name"), QStringLiteral("never")}}}});
        firstFailureResponse.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("slice21_first_failure_skipped")},
                        {QStringLiteral("name"), QStringLiteral("set_module_note")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), second.id},
                                                                  {QStringLiteral("note"), QStringLiteral("never")}}}});
        m_mock->emitResponse(firstFailureResponse);
        QTRY_COMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(controller.actor()->undoStack()->count(), beforeFirstFailureCount);
        QCOMPARE(controller.actor()->undoStack()->index(), beforeFirstFailureIndex);
        QCOMPARE(project->toJson(), beforeFirstFailure);
        QCOMPARE(project->isModified(), beforeFirstFailureModified);
        QMap<QString, int> firstFailureMessageCounts;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool"))
                ++firstFailureMessageCounts[message.toolCallId];
        }
        QCOMPARE(firstFailureMessageCounts.value(QStringLiteral("slice21_first_failure")), 1);
        QCOMPARE(firstFailureMessageCounts.value(QStringLiteral("slice21_first_failure_skipped")), 1);

        const QJsonObject beforeGroupedEdits = project->toJson();
        const int beforeGroupedCount = controller.actor()->undoStack()->count();
        const int beforeGroupedIndex = controller.actor()->undoStack()->index();
        controller.sendUserMessage(QStringLiteral("Apply two edits around a no-op"));
        AgentResponse groupedResponse;
        groupedResponse.success = true;
        groupedResponse.toolCalls.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("slice21_group_rename")},
            {QStringLiteral("name"), QStringLiteral("rename_module")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), first.id},
                                                      {QStringLiteral("name"), QStringLiteral("Grouped")}}}});
        groupedResponse.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("slice21_group_noop")},
                        {QStringLiteral("name"), QStringLiteral("rename_module")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), second.id},
                                                                  {QStringLiteral("name"), second.name}}}});
        groupedResponse.toolCalls.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("slice21_group_note")},
            {QStringLiteral("name"), QStringLiteral("set_module_note")},
            {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), second.id},
                                                      {QStringLiteral("note"), QStringLiteral("Grouped note")}}}});
        m_mock->emitResponse(groupedResponse);
        QTRY_COMPARE(controller.state(), AgentController::AgentState::Thinking);
        AgentResponse groupedFinalResponse;
        groupedFinalResponse.success = true;
        groupedFinalResponse.content = QStringLiteral("Both edits are done");
        m_mock->emitResponse(groupedFinalResponse);
        QTRY_COMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(controller.actor()->undoStack()->count(), beforeGroupedCount + 2);
        QCOMPARE(controller.actor()->undoStack()->index(), beforeGroupedIndex + 2);
        QVERIFY(controller.undoLastAgentAction());
        QCOMPARE(project->moduleById(first.id)->name, QStringLiteral("Grouped"));
        QCOMPARE(project->moduleById(second.id)->note, QStringLiteral("keep second note"));
        QCOMPARE(controller.actor()->undoStack()->index(), beforeGroupedIndex + 1);
        QVERIFY(controller.undoLastAgentAction());
        verifyExpected(beforeGroupedEdits, project->toJson());
        QCOMPARE(controller.actor()->undoStack()->index(), beforeGroupedIndex);
        controller.actor()->undoStack()->redo();
        QCOMPARE(project->moduleById(first.id)->name, QStringLiteral("Grouped"));
        QCOMPARE(project->moduleById(second.id)->note, QStringLiteral("keep second note"));
        QCOMPARE(controller.actor()->undoStack()->index(), beforeGroupedIndex + 1);
        controller.actor()->undoStack()->redo();
        QCOMPARE(project->moduleById(second.id)->note, QStringLiteral("Grouped note"));
        QCOMPARE(controller.actor()->undoStack()->index(), beforeGroupedIndex + 2);

        ProjectManager::instance().closeProject();
    }

    void testActorSchemaPreflightSummaryReportsWholeBatchNotExecuted() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance module;
        module.id = QStringLiteral("slice20_preflight_module");
        module.moduleId = QStringLiteral("GrabImage");
        module.name = QStringLiteral("Before");
        project->addModule(module);
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        const int undoCount = ctrl.actor()->undoStack()->count();
        ctrl.sendUserMessage(QStringLiteral("Validate the batch"));

        AgentResponse response;
        response.success = true;
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_valid_but_not_run")},
                        {QStringLiteral("name"), QStringLiteral("rename_module")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                  {QStringLiteral("name"), QStringLiteral("After")}}}});
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_schema_invalid")},
                        {QStringLiteral("name"), QStringLiteral("set_module_position")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), module.id},
                                                                  {QStringLiteral("x"), 1.5},
                                                                  {QStringLiteral("y"), 2}}}});
        response.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_after_preflight")},
                                              {QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                              {QStringLiteral("arguments"), QJsonObject()}});
        m_mock->emitResponse(response);
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        QVERIFY(errorSpy.isEmpty());

        QJsonArray retryCalls;
        for (const QJsonValue& value : response.toolCalls) {
            QJsonObject call = value.toObject();
            call.insert(QStringLiteral("id"), call.value(QStringLiteral("id")).toString() + QStringLiteral("_retry"));
            retryCalls.append(call);
        }
        response.toolCalls = retryCalls;
        m_mock->emitResponse(response);

        QCOMPARE(errorSpy.count(), 1);
        const QString summary = errorSpy.takeFirst().at(0).toString();
        QVERIFY(summary.contains(QStringLiteral("连续两次相同参数错误")));
        QVERIFY(summary.contains(QStringLiteral("call_valid_but_not_run_retry")));
        QVERIFY(summary.contains(QStringLiteral("call_schema_invalid_retry")));
        QVERIFY(summary.contains(QStringLiteral("call_after_preflight_retry")));
        QVERIFY(summary.contains(QStringLiteral("INVALID_PARAM")));
        QCOMPARE(project->moduleById(module.id)->name, QStringLiteral("Before"));
        QCOMPARE(ctrl.actor()->undoStack()->count(), undoCount);
        ProjectManager::instance().closeProject();
    }

    void testMixedObserverDenialKeepsOriginalCallOrderInFailureSummary() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Observer);
        ProjectManager::instance().closeProject();
        QVERIFY(ProjectManager::instance().newProject());
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        ctrl.sendUserMessage(QStringLiteral("Inspect the current flow"));

        AgentResponse response;
        response.success = true;
        response.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_read_first")},
                                              {QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                              {QStringLiteral("arguments"), QJsonObject()}});
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_write_denied")},
                        {QStringLiteral("name"), QStringLiteral("rename_module")},
                        {QStringLiteral("arguments"),
                         QJsonObject{{QStringLiteral("instanceId"), "x"}, {QStringLiteral("name"), "blocked"}}}});
        const QString canary = QStringLiteral("slice20_unknown_run_canary");
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("call_read_failure")},
                        {QStringLiteral("name"), QStringLiteral("get_run_results")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("runId"), canary}}}});
        response.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_read_skipped")},
                                              {QStringLiteral("name"), QStringLiteral("get_flow_state")},
                                              {QStringLiteral("arguments"), QJsonObject()}});
        m_mock->emitResponse(response);

        QCOMPARE(errorSpy.count(), 1);
        const QString summary = errorSpy.takeFirst().at(0).toString();
        QVERIFY(summary.contains(QStringLiteral("call_read_first")));
        QVERIFY(summary.contains(QStringLiteral("call_read_first · get_flow_state · 调用已返回")));
        QVERIFY(!summary.contains(QStringLiteral("call_read_first · get_flow_state · 已完成")));
        QVERIFY(summary.contains(QStringLiteral("call_write_denied")));
        QVERIFY(summary.contains(QStringLiteral("PERMISSION_DENIED")));
        QVERIFY(summary.contains(QStringLiteral("call_read_failure")));
        QVERIFY(summary.contains(QStringLiteral("RUN_NOT_FOUND")));
        QVERIFY(summary.contains(QStringLiteral("call_read_skipped")));
        QVERIFY(!summary.contains(canary));
        QVERIFY(summary.indexOf(QStringLiteral("call_read_first")) <
                summary.indexOf(QStringLiteral("call_write_denied")));
        QVERIFY(summary.indexOf(QStringLiteral("call_write_denied")) <
                summary.indexOf(QStringLiteral("call_read_failure")));
        QVERIFY(summary.indexOf(QStringLiteral("call_read_failure")) <
                summary.indexOf(QStringLiteral("call_read_skipped")));
        ProjectManager::instance().closeProject();
    }

    // ---------- Observer 模式 ----------

    // 阶段1 §2（F08 修复）：聊天路径与 Bridge 路径共用 evaluateToolGate——
    // Observer 的只读工具【执行并继续问答】（旧行为是整轮丢弃），写工具逐项
    // PERMISSION_DENIED（结构化结果进入对话与审计，零副作用），不进入 Confirming
    void testObserverModeExecutesReadOnlyAndDeniesWriteTools() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QCOMPARE(UserManager::instance().currentUser().type, UserType::Guest);
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("GlobalVar")));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("Camera")));
        GlobalVarManager::instance().clearAllVariables();
        GlobalVarManager::instance().addVariable(new VarModel(QStringLiteral("batch_private_name"), VarDataType::String,
                                                              QStringLiteral("batch_private_value")));
        const QJsonObject variablesBefore =
            GlobalVarManager::instance().userVariableSnapshotJson(QStringLiteral("batch_private_name"));
        ctrl.sendUserMessage("检查流程并试着加模块");
        ProjectManager::instance().closeProject();
        Project* emptyProject = ProjectManager::instance().newProject();
        QVERIFY(emptyProject);
        QVERIFY(RunEngine::instance().loadProject(emptyProject));

        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);

        AgentResponse resp;
        resp.success = true;
        resp.content = "Let me check.";
        QJsonObject tcRead;
        tcRead["id"] = "call_ro";
        tcRead["type"] = "function";
        tcRead["name"] = "read_documentation";
        tcRead["arguments"] = QJsonObject{{"topic", "workflow"}};
        resp.toolCalls.append(tcRead);
        QJsonObject tcValidate;
        tcValidate["id"] = "call_validate";
        tcValidate["type"] = "function";
        tcValidate["name"] = "validate_flow";
        tcValidate["arguments"] = QJsonObject();
        resp.toolCalls.append(tcValidate);
        QJsonObject tcWrite;
        tcWrite["id"] = "call_w";
        tcWrite["type"] = "function";
        tcWrite["name"] = "add_module";
        tcWrite["arguments"] = QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "obs_denied"}};
        resp.toolCalls.append(tcWrite);
        QJsonObject tcFlowWrite;
        tcFlowWrite["id"] = "call_flow_write_denied";
        tcFlowWrite["type"] = "function";
        tcFlowWrite["name"] = "run_flow";
        tcFlowWrite["arguments"] = QJsonObject{{"flowId", "secondary"}};
        resp.toolCalls.append(tcFlowWrite);
        QJsonObject tcMalformedWrite;
        tcMalformedWrite["id"] = "call_malformed_write_denied";
        tcMalformedWrite["type"] = "function";
        tcMalformedWrite["name"] = "connect_camera";
        tcMalformedWrite["arguments"] = QStringLiteral("{malformed");
        resp.toolCalls.append(tcMalformedWrite);
        resp.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_global_denied")},
                                          {QStringLiteral("type"), QStringLiteral("function")},
                                          {QStringLiteral("name"), QStringLiteral("list_global_variables")},
                                          {QStringLiteral("arguments"), QJsonObject{}}});
        resp.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_plugin_list_allowed")},
                                          {QStringLiteral("type"), QStringLiteral("function")},
                                          {QStringLiteral("name"), QStringLiteral("get_available_plugins")},
                                          {QStringLiteral("arguments"), QJsonObject{}}});
        resp.toolCalls.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("call_camera_denied")},
                                          {QStringLiteral("type"), QStringLiteral("function")},
                                          {QStringLiteral("name"), QStringLiteral("list_cameras")},
                                          {QStringLiteral("arguments"), QJsonObject{}}});

        m_mock->emitResponse(resp);

        // 只读工具已执行且闭环继续：结果回传后向 LLM 发起下一次请求（Thinking），
        // 而不是旧行为的直接回 Idle
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);

        // 每个原始 call 恰有一条审计：只读 success、写操作 denied
        QCOMPARE(logSpy.count(), 8);
        QString readResult, validateResult, writeResult, flowWriteResult, malformedWriteResult;
        QString globalResult, pluginsResult, cameraResult;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry e = args.at(0).value<AgentActionLogEntry>();
            if (e.action == QLatin1String("read_documentation"))
                readResult = e.result;
            else if (e.action == QLatin1String("validate_flow"))
                validateResult = e.result;
            else if (e.action == QLatin1String("add_module"))
                writeResult = e.result;
            else if (e.action == QLatin1String("run_flow"))
                flowWriteResult = e.result;
            else if (e.action == QLatin1String("connect_camera"))
                malformedWriteResult = e.result;
            else if (e.action == QLatin1String("list_global_variables"))
                globalResult = e.result;
            else if (e.action == QLatin1String("get_available_plugins"))
                pluginsResult = e.result;
            else if (e.action == QLatin1String("list_cameras"))
                cameraResult = e.result;
        }
        QCOMPARE(readResult, QStringLiteral("denied"));
        QVERIFY(validateResult != QStringLiteral("denied"));
        QCOMPARE(writeResult, QStringLiteral("denied"));
        QCOMPARE(flowWriteResult, QStringLiteral("denied"));
        QCOMPARE(malformedWriteResult, QStringLiteral("denied"));
        QCOMPARE(globalResult, QStringLiteral("denied"));
        QCOMPARE(pluginsResult, QStringLiteral("success"));
        QCOMPARE(cameraResult, QStringLiteral("denied"));

        // 对话合法性：下一次请求上下文中两条 tool 消息各就其位，
        // 被拒消息携带 PERMISSION_DENIED 结构化错误
        const auto& messages = m_mock->lastCtx.messages;
        int toolMessages = 0;
        QMap<QString, QString> toolResults;
        for (const AgentMessage& m : messages) {
            if (m.role != QLatin1String("tool"))
                continue;
            ++toolMessages;
            toolResults.insert(m.toolCallId, m.content);
        }
        QCOMPARE(toolMessages, 8);
        QCOMPARE(toolResults.size(), 8);
        for (const QString& callId :
             {QStringLiteral("call_ro"), QStringLiteral("call_validate"), QStringLiteral("call_w"),
              QStringLiteral("call_flow_write_denied"), QStringLiteral("call_malformed_write_denied"),
              QStringLiteral("call_global_denied"), QStringLiteral("call_plugin_list_allowed"),
              QStringLiteral("call_camera_denied")})
            QVERIFY2(toolResults.contains(callId),
                     qPrintable(QStringLiteral("missing tool result for %1").arg(callId)));
        QVERIFY(toolResults.value(QStringLiteral("call_global_denied")).contains("PERMISSION_DENIED"));
        QVERIFY(!toolResults.value(QStringLiteral("call_global_denied")).contains("batch_private_name"));
        QVERIFY(!toolResults.value(QStringLiteral("call_global_denied")).contains("batch_private_value"));
        QVERIFY(toolResults.value(QStringLiteral("call_ro")).contains("PERMISSION_DENIED"));
        QVERIFY(toolResults.value(QStringLiteral("call_ro")).contains("OpenFile"));
        QVERIFY(!toolResults.value(QStringLiteral("call_plugin_list_allowed")).contains("PERMISSION_DENIED"));
        QVERIFY(toolResults.value(QStringLiteral("call_camera_denied")).contains("PERMISSION_DENIED"));
        QVERIFY(toolResults.value(QStringLiteral("call_camera_denied")).contains("Camera"));
        QCOMPARE(GlobalVarManager::instance().userVariableSnapshotJson(QStringLiteral("batch_private_name")),
                 variablesBefore);
        QCOMPARE(ctrl.actor()->executeTool("get_flow_state", {}).value(QStringLiteral("executionScope")).toString(),
                 QStringLiteral("project_modules"));
        GlobalVarManager::instance().clearAllVariables();
        UserManager::instance().login(QStringLiteral("developer"), QString());
        ProjectManager::instance().closeProject();
    }

    void testFlowSelectorBridgePreflightsBeforeConfirmationAndObserverPermission() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject unavailable = ctrl.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}});
        QCOMPARE(unavailable.value(QStringLiteral("errorCode")).toString(), QStringLiteral("FLOW_UNAVAILABLE"));
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(ctrl.pendingConfirmationPayload().isEmpty());

        ctrl.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject denied = ctrl.handleToolCall(
            QStringLiteral("run_flow"), QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(ctrl.pendingConfirmationPayload().isEmpty());
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testFlowSelectorChatBatchRejectsBeforeConfirmationOrExecution() {
        AgentController& ctrl = AgentController::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ctrl.sendUserMessage(QStringLiteral("Inspect and run a selected flow"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        bool selectorSchemaSent = false;
        for (const ToolDefinition& tool : m_mock->lastTools) {
            if (tool.name != QLatin1String("run_flow"))
                continue;
            for (const ToolParam& param : tool.params)
                selectorSchemaSent |=
                    param.name == QLatin1String("flowId") && param.type == QLatin1String("string") && !param.required;
        }
        QVERIFY(selectorSchemaSent);
        const int runsBefore = RunEngine::instance().totalRuns();
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);

        AgentResponse response;
        response.success = true;
        QJsonObject selector;
        selector["id"] = QStringLiteral("call_selector");
        selector["type"] = QStringLiteral("function");
        selector["name"] = QStringLiteral("run_flow");
        selector["arguments"] = QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}};
        response.toolCalls.append(selector);
        QJsonObject read;
        read["id"] = QStringLiteral("call_read");
        read["type"] = QStringLiteral("function");
        read["name"] = QStringLiteral("get_flow_state");
        read["arguments"] = QJsonObject();
        response.toolCalls.append(read);

        m_mock->emitResponse(response);

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(ctrl.pendingConfirmationPayload().isEmpty());
        QCOMPARE(RunEngine::instance().totalRuns(), runsBefore);
        QCOMPARE(errorSpy.count(), 1);
        const QString summary = errorSpy.takeFirst().at(0).toString();
        QVERIFY(summary.contains(QStringLiteral("FLOW_UNAVAILABLE")));
        QVERIFY(summary.contains(QStringLiteral("当前不支持按 flowId 选择流程")));
        QVERIFY(summary.contains(QStringLiteral("call_selector")));
        QVERIFY(summary.contains(QStringLiteral("call_read")));
        QCOMPARE(logSpy.count(), 2);

        QMap<QString, QJsonObject> resultsByCall;
        QMap<QString, int> toolMessagesByCall;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role == QLatin1String("tool")) {
                ++toolMessagesByCall[message.toolCallId];
                resultsByCall.insert(message.toolCallId, QJsonDocument::fromJson(message.content.toUtf8()).object());
            }
        }
        QCOMPARE(toolMessagesByCall.size(), 2);
        QCOMPARE(toolMessagesByCall.value(QStringLiteral("call_selector")), 1);
        QCOMPARE(toolMessagesByCall.value(QStringLiteral("call_read")), 1);
        QCOMPARE(resultsByCall.value(QStringLiteral("call_selector")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("FLOW_UNAVAILABLE"));
        QCOMPARE(resultsByCall.value(QStringLiteral("call_read")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(resultsByCall.value(QStringLiteral("call_read"))
                     .value(QStringLiteral("details"))
                     .toObject()
                     .value(QStringLiteral("failedCallId"))
                     .toString(),
                 QStringLiteral("call_selector"));
        QString selectorAudit, skippedAudit;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry entry = args.at(0).value<AgentActionLogEntry>();
            if (entry.action == QLatin1String("run_flow"))
                selectorAudit = entry.result;
            else if (entry.action == QLatin1String("get_flow_state"))
                skippedAudit = entry.result;
        }
        QCOMPARE(selectorAudit, QStringLiteral("error"));
        QCOMPARE(skippedAudit, QStringLiteral("skipped"));
        ProjectManager::instance().closeProject();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testObserverFlowSelectorBatchAuditsUnavailableSkippedAndDenied() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Observer);
        ctrl.sendUserMessage(QStringLiteral("Inspect selected flow"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Thinking);
        const int runsBefore = RunEngine::instance().totalRuns();
        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);

        AgentResponse response;
        response.success = true;
        QJsonObject selector;
        selector["id"] = QStringLiteral("obs_selector");
        selector["type"] = QStringLiteral("function");
        selector["name"] = QStringLiteral("validate_flow");
        selector["arguments"] = QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}};
        response.toolCalls.append(selector);
        QJsonObject skipped;
        skipped["id"] = QStringLiteral("obs_skipped");
        skipped["type"] = QStringLiteral("function");
        skipped["name"] = QStringLiteral("get_flow_state");
        skipped["arguments"] = QJsonObject();
        response.toolCalls.append(skipped);
        QJsonObject denied;
        denied["id"] = QStringLiteral("obs_denied");
        denied["type"] = QStringLiteral("function");
        denied["name"] = QStringLiteral("run_flow");
        denied["arguments"] = QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}};
        response.toolCalls.append(denied);

        m_mock->emitResponse(response);

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(ctrl.pendingConfirmationPayload().isEmpty());
        QCOMPARE(RunEngine::instance().totalRuns(), runsBefore);
        QCOMPARE(logSpy.count(), 3);
        QMap<QString, QJsonObject> resultsByCall;
        QMap<QString, int> toolMessagesByCall;
        for (const AgentMessage& message : ctrl.m_conversationHistory) {
            if (message.role == QLatin1String("tool")) {
                ++toolMessagesByCall[message.toolCallId];
                resultsByCall.insert(message.toolCallId, QJsonDocument::fromJson(message.content.toUtf8()).object());
            }
        }
        QCOMPARE(toolMessagesByCall.size(), 3);
        for (const QString& callId :
             {QStringLiteral("obs_selector"), QStringLiteral("obs_skipped"), QStringLiteral("obs_denied")})
            QCOMPARE(toolMessagesByCall.value(callId), 1);
        QCOMPARE(resultsByCall.value(QStringLiteral("obs_selector")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("FLOW_UNAVAILABLE"));
        QCOMPARE(resultsByCall.value(QStringLiteral("obs_skipped")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
        QCOMPARE(resultsByCall.value(QStringLiteral("obs_denied")).value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("PERMISSION_DENIED"));
        QMap<QString, QString> auditByAction;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry entry = args.at(0).value<AgentActionLogEntry>();
            auditByAction.insert(entry.action, entry.result);
        }
        QCOMPARE(auditByAction.value(QStringLiteral("validate_flow")), QStringLiteral("error"));
        QCOMPARE(auditByAction.value(QStringLiteral("get_flow_state")), QStringLiteral("skipped"));
        QCOMPARE(auditByAction.value(QStringLiteral("run_flow")), QStringLiteral("denied"));
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testFlowSelectorChatRejectsInvalidTypeBeforeSemanticSelector() {
        AgentController& ctrl = AgentController::instance();
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        ctrl.sendUserMessage(QStringLiteral("Select flows"));
        QSignalSpy errorSpy(&ctrl, &AgentController::llmErrorOccurred);
        AgentResponse response;
        response.success = true;
        QJsonObject valid;
        valid["id"] = QStringLiteral("call_valid_selector");
        valid["type"] = QStringLiteral("function");
        valid["name"] = QStringLiteral("run_flow");
        valid["arguments"] = QJsonObject{{QStringLiteral("flowId"), QStringLiteral("secondary")}};
        response.toolCalls.append(valid);
        QJsonObject invalid;
        invalid["id"] = QStringLiteral("call_invalid_selector");
        invalid["type"] = QStringLiteral("function");
        invalid["name"] = QStringLiteral("get_flow_state");
        invalid["arguments"] = QJsonObject{{QStringLiteral("flowId"), 7}};
        response.toolCalls.append(invalid);

        m_mock->emitResponse(response);

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QVERIFY(ctrl.pendingConfirmationPayload().isEmpty());
        QCOMPARE(errorSpy.count(), 1);
        const QString summary = errorSpy.takeFirst().at(0).toString();
        QVERIFY(summary.contains(QStringLiteral("INVALID_PARAM")));
        const QJsonObject invalidResult =
            QJsonDocument::fromJson(ctrl.m_conversationHistory.last().content.toUtf8()).object();
        QCOMPARE(ctrl.m_conversationHistory.last().toolCallId, QStringLiteral("call_invalid_selector"));
        QCOMPARE(invalidResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        const QJsonObject validResult =
            QJsonDocument::fromJson(
                ctrl.m_conversationHistory.at(ctrl.m_conversationHistory.size() - 2).content.toUtf8())
                .object();
        QCOMPARE(ctrl.m_conversationHistory.at(ctrl.m_conversationHistory.size() - 2).toolCallId,
                 QStringLiteral("call_valid_selector"));
        QCOMPARE(validResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
    }

    // ---------- 阶段 1（G1）：确认绑定、降级与拒绝 ----------

    // G1-5：确认绑定工程与配置版本——窗口期内工程被编辑,确认后整批失效,零副作用
    void testConfirmationStaleWhenProjectEditedInWindow() {
        AgentController& ctrl = AgentController::instance();
        ProjectManager::instance().closeProject(); // 隔离:清掉前面用例可能遗留的已修改工程
        QVERIFY(!ctrl.actor()->executeTool("create_project", QJsonObject{{"name", "base"}}).contains("error"));

        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);
        QSignalSpy responseSpy(&ctrl, &AgentController::llmResponseReceived);

        const QJsonObject pending = ctrl.handleToolCall("create_project", QJsonObject{{"name", "stale_target"}});
        QCOMPARE(pending.value("status").toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(ctrl.state(), AgentController::AgentState::Confirming);

        // 窗口期修改：另一工程被创建（身份/指纹/撤销栈全部变化）。
        // 注：新建工程经 setName 即处于已修改态,切换需显式 discard——守卫语义一致
        QVERIFY(!ctrl.actor()
                     ->executeTool("create_project", QJsonObject{{"name", "window_edit"}, {"onUnsaved", "discard"}})
                     .contains("error"));

        ctrl.confirmPendingTools(ctrl.pendingConfirmationId()); // 陈旧确认 → 整批拒绝,不执行

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        // 待确认的 create_project 未执行：当前工程仍是窗口期编辑的那个
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("window_edit"));
        bool sawExpired = false;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry e = args.at(0).value<AgentActionLogEntry>();
            if (e.action == QLatin1String("create_project") && e.result == QLatin1String("expired"))
                sawExpired = true;
        }
        QVERIFY2(sawExpired, "stale confirmation must leave an expired audit entry");
        bool sawStaleMessage = false;
        for (const auto& args : responseSpy)
            if (args.at(0).toString().contains(QStringLiteral("确认已失效")))
                sawStaleMessage = true;
        QVERIFY(sawStaleMessage);
    }

    // G1-5：确认窗口内权限降级 → 整批拒绝,不产生未批准操作
    void testPermissionDowngradeDuringConfirmationDenies() {
        AgentController& ctrl = AgentController::instance();
        ProjectManager::instance().closeProject(); // 隔离
        QVERIFY(!ctrl.actor()->executeTool("create_project", QJsonObject{{"name", "base2"}}).contains("error"));

        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);
        const QJsonObject pending = ctrl.handleToolCall("create_project", QJsonObject{{"name", "downgrade_target"}});
        QCOMPARE(pending.value("status").toString(), QStringLiteral("pending_confirmation"));

        ctrl.setPermissionLevel(AgentController::PermissionLevel::Observer); // 窗口期降级
        ctrl.confirmPendingTools(ctrl.pendingConfirmationId());

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(ProjectManager::instance().currentProject()->name(),
                 QStringLiteral("base2")); // 未执行
        bool sawDenied = false;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry e = args.at(0).value<AgentActionLogEntry>();
            if (e.action == QLatin1String("create_project") && e.result == QLatin1String("denied"))
                sawDenied = true;
        }
        QVERIFY2(sawDenied, "downgraded confirmation must leave a denied audit entry");
        ctrl.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    // G1-5：拒绝确认零副作用且逐 call 留 rejected 审计
    void testRejectPendingZeroSideEffects() {
        AgentController& ctrl = AgentController::instance();
        ProjectManager::instance().closeProject(); // 隔离
        QVERIFY(!ctrl.actor()->executeTool("create_project", QJsonObject{{"name", "base3"}}).contains("error"));

        QSignalSpy logSpy(&ctrl, &AgentController::actionLogEntryAdded);
        const QJsonObject pending = ctrl.handleToolCall("create_project", QJsonObject{{"name", "reject_target"}});
        QCOMPARE(pending.value("status").toString(), QStringLiteral("pending_confirmation"));

        ctrl.rejectPendingTools(ctrl.pendingConfirmationId());

        QCOMPARE(ctrl.state(), AgentController::AgentState::Idle);
        QCOMPARE(ProjectManager::instance().currentProject()->name(), QStringLiteral("base3"));
        bool sawRejected = false;
        for (const auto& args : logSpy) {
            const AgentActionLogEntry e = args.at(0).value<AgentActionLogEntry>();
            if (e.action == QLatin1String("create_project") && e.result == QLatin1String("rejected"))
                sawRejected = true;
        }
        QVERIFY2(sawRejected, "rejection must leave a rejected audit entry");
        QVERIFY(!ctrl.hasPendingTools());
    }

    // B2：Observer 纯只读批次正常执行并续跑（查询能力不被降权破坏）
    void testObserverReadOnlyBatchExecutesAndContinues() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        controller.sendUserMessage(QStringLiteral("list plugins"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("checking");
        resp.toolCalls.append(QJsonObject{
            {"id", "read-1"}, {"type", "function"}, {"name", "get_available_plugins"}, {"arguments", QJsonObject()}});
        m_mock->emitResponse(resp);

        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->requestCount, 2);
        bool readOk = false;
        for (const AgentMessage& message : m_mock->lastCtx.messages) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("read-1"))
                readOk = !message.content.contains(QStringLiteral("\"error\""));
        }
        QVERIFY(readOk);

        AgentResponse done;
        done.success = true;
        done.content = QStringLiteral("done");
        m_mock->emitResponse(done);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // B2：Observer 混合批次逐项判定——只读执行、写入拒绝，且不因含读工具放行写工具
    void testObserverMixedBatchExecutesReadsAndDeniesWrites() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().newProject();
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        controller.sendUserMessage(QStringLiteral("read and write"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("working");
        resp.toolCalls.append(QJsonObject{
            {"id", "read-1"}, {"type", "function"}, {"name", "get_available_plugins"}, {"arguments", QJsonObject()}});
        resp.toolCalls.append(QJsonObject{{"id", "write-1"},
                                          {"type", "function"},
                                          {"name", "add_module"},
                                          {"arguments", QJsonObject{{"plugin", "GrabImage"}}}});
        m_mock->emitResponse(resp);

        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(m_mock->requestCount, 1); // 拒绝项结束本轮，不续跑
        QCOMPARE(project->modules().size(), 0);

        bool readSeen = false;
        bool writeDenied = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role != QLatin1String("tool"))
                continue;
            if (message.toolCallId == QLatin1String("read-1"))
                readSeen = !message.content.contains(QStringLiteral("PERMISSION_DENIED"));
            if (message.toolCallId == QLatin1String("write-1"))
                writeDenied = message.content.contains(QStringLiteral("PERMISSION_DENIED"));
        }
        QVERIFY(readSeen);
        QVERIFY(writeDenied);
    }

    // B2：确认前切换工程（含同 ID 模块）——旧确认失效，新工程零修改
    void testConfirmationExpiresOnProjectSwitch() {
        AgentController& controller = AgentController::instance();
        Project* first = ProjectManager::instance().newProject();
        QVERIFY(first != nullptr);
        ModuleInstance sameId;
        sameId.id = QStringLiteral("same_id");
        sameId.moduleId = QStringLiteral("GrabImage");
        first->addModule(sameId);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("remove it"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(QJsonObject{{"id", "remove-old"},
                                          {"type", "function"},
                                          {"name", "remove_module"},
                                          {"arguments", QJsonObject{{"instanceId", "same_id"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        Project* second = ProjectManager::instance().newProject();
        QVERIFY(second != nullptr);
        ModuleInstance sameIdInSecond;
        sameIdInSecond.id = QStringLiteral("same_id");
        sameIdInSecond.moduleId = QStringLiteral("GrabImage");
        second->addModule(sameIdInSecond);

        // 工程切换使旧确认立即失效，且不会执行旧批次
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.pendingToolCalls().isEmpty());
        controller.confirmPendingTools();
        QCoreApplication::processEvents();
        QCOMPARE(second->modules().size(), 1);

        bool expired = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("remove-old"))
                expired = message.content.contains(QStringLiteral("confirmation_expired"));
        }
        QVERIFY(expired);
    }

    // B2：确认入队后切工程——排队 lambda 复核并拒绝
    void testQueuedConfirmationRejectedAfterProjectSwitch() {
        AgentController& controller = AgentController::instance();
        QVERIFY(ProjectManager::instance().newProject() != nullptr);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("add"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(
            QJsonObject{{"id", "add-old"},
                        {"type", "function"},
                        {"name", "add_module"},
                        {"arguments", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "must_not_add"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        controller.confirmPendingTools(); // 入队
        Project* second = ProjectManager::instance().newProject();
        QVERIFY(second != nullptr);
        QCoreApplication::processEvents();

        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(second->modules().size(), 0);
    }

    // B2：外部参数修改使确认失效，确认不得覆盖外部值
    void testExternalParamChangeExpiresConfirmation() {
        AgentController& controller = AgentController::instance();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance inst;
        inst.id = QStringLiteral("mc1");
        inst.moduleId = QStringLiteral("MeasureCircle");
        project->addModule(inst);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("set radius"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(
            QJsonObject{{"id", "param-old"},
                        {"type", "function"},
                        {"name", "set_param"},
                        {"arguments", QJsonObject{{"instanceId", "mc1"}, {"key", "initialRadius"}, {"value", 222}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        // 用户在界面上改了同一参数
        project->setModuleParam(QStringLiteral("mc1"), QStringLiteral("initialRadius"), 111);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);

        controller.confirmPendingTools();
        QCoreApplication::processEvents();
        QCOMPARE(project->moduleById(QStringLiteral("mc1"))->params.value("initialRadius"), QJsonValue(111));
    }

    // B2：目标删除同样使旧确认失效（不得在删除后的工程上执行旧写操作）
    void testTargetRemovalExpiresConfirmation() {
        AgentController& controller = AgentController::instance();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance inst;
        inst.id = QStringLiteral("target-1");
        inst.moduleId = QStringLiteral("GrabImage");
        project->addModule(inst);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("add another module"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(
            QJsonObject{{"id", "add-after-removal"},
                        {"type", "function"},
                        {"name", "add_module"},
                        {"arguments", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "must_not_add"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        project->removeModule(QStringLiteral("target-1"));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        controller.confirmPendingTools();
        QCoreApplication::processEvents();
        QCOMPARE(project->modules().size(), 0);
    }

    // B2：Autopilot 下 create_project 成功后保守终止旧批次，让模型读取新工程再规划
    void testProjectSwitchStopsRemainingBatch() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        controller.sendUserMessage(QStringLiteral("create and add"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("working");
        resp.toolCalls.append(QJsonObject{{"id", "create-1"},
                                          {"type", "function"},
                                          {"name", "create_project"},
                                          {"arguments", QJsonObject{{"name", "BatchSwitch"}}}});
        resp.toolCalls.append(
            QJsonObject{{"id", "add-after"},
                        {"type", "function"},
                        {"name", "add_module"},
                        {"arguments", QJsonObject{{"plugin", "GrabImage"}, {"instanceName", "stale"}}}});
        m_mock->emitResponse(resp);

        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QCOMPARE(m_mock->requestCount, 2); // project_switched 允许重新规划
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project != nullptr);
        QCOMPARE(project->modules().size(), 0);

        bool addSkipped = false;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("add-after"))
                addSkipped = message.content.contains(QStringLiteral("skipped_after_failure"));
        }
        QVERIFY(addSkipped);
    }

    // B2：合法确认只执行一次（双击不重复执行）
    void testConfirmationExecutesOnlyOnce() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        controller.sendUserMessage(QStringLiteral("add one"));
        AgentResponse resp;
        resp.success = true;
        resp.toolCalls.append(QJsonObject{{"id", "create-once"},
                                          {"type", "function"},
                                          {"name", "create_project"},
                                          {"arguments", QJsonObject{{"name", "OnlyOnce"}}}});
        m_mock->emitResponse(resp);
        QCOMPARE(controller.state(), AgentController::AgentState::Confirming);

        controller.confirmPendingTools();
        controller.confirmPendingTools(); // 连点
        QCoreApplication::processEvents();

        int results = 0;
        for (const AgentMessage& message : controller.m_conversationHistory) {
            if (message.role == QLatin1String("tool") && message.toolCallId == QLatin1String("create-once"))
                ++results;
        }
        QCOMPARE(results, 1);
        QCOMPARE(m_mock->requestCount, 2); // 执行后正常续跑一次
    }

    // ---------- 第一迭代：排队执行 / 中止 / 视觉降级 ----------

    void testConfirmDispatchesQueuedExactlyOnce() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage(QStringLiteral("Create project"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("Creating...");
        QJsonObject tc;
        tc["id"] = "call_queued";
        tc["type"] = "function";
        tc["name"] = "create_project";
        tc["arguments"] = QJsonObject{{"name", "QueuedProject"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        QSignalSpy logSpy(&AgentController::instance(), &AgentController::actionLogEntryAdded);
        AgentController::instance().confirmPendingTools();
        // 连点保护：第二次确认不得重复排队或重复执行
        AgentController::instance().confirmPendingTools();
        // 排队 tick 尚未处理：状态不得提前回到 Idle（仍为 Confirming/Executing）
        QVERIFY(AgentController::instance().state() != AgentController::AgentState::Idle);

        QTRY_COMPARE_WITH_TIMEOUT(AgentController::instance().state(), AgentController::AgentState::Thinking, 5000);
        QCOMPARE(logSpy.count(), 1); // create_project 只执行一次
        QVERIFY(ProjectManager::instance().currentProject() != nullptr);
    }

    void testAbortWhileThinkingIsIdempotent() {
        AgentController::instance().sendUserMessage(QStringLiteral("Hello"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        QSignalSpy abortSpy(&AgentController::instance(), &AgentController::agentTurnAborted);
        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        AgentController::instance().abortCurrentTurn();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(responseSpy.count(), 1);
        QCOMPARE(responseSpy.takeFirst().at(0).toString(), QStringLiteral("Stopped by user."));

        const quint64 generation = AgentController::instance().turnGeneration();
        AgentController::instance().abortCurrentTurn(); // Idle → no-op（幂等）
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(AgentController::instance().turnGeneration(), generation);
    }

    void testAbortCancelsQueuedDispatch() {
        ProjectManager::instance().closeProject(); // 隔离：本用例断言"未执行建工程"
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Advisor);
        AgentController::instance().sendUserMessage(QStringLiteral("Create project"));

        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("Creating...");
        QJsonObject tc;
        tc["id"] = "call_cancel";
        tc["type"] = "function";
        tc["name"] = "create_project";
        tc["arguments"] = QJsonObject{{"name", "CancelledProject"}};
        resp.toolCalls.append(tc);
        m_mock->emitResponse(resp);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Confirming);

        QSignalSpy logSpy(&AgentController::instance(), &AgentController::actionLogEntryAdded);
        AgentController::instance().confirmPendingTools(); // 入队
        AgentController::instance().abortCurrentTurn();    // 在 tick 之前中止
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);

        for (int i = 0; i < 10; ++i)
            QCoreApplication::processEvents();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(logSpy.count(), 0); // 排队任务被中止代号作废，未执行
        QVERIFY(ProjectManager::instance().currentProject() == nullptr);
    }

    void testUserMessageRejectedWhileExecuting() {
        AgentController::instance().transitionTo(AgentController::AgentState::Executing);
        QSignalSpy responseSpy(&AgentController::instance(), &AgentController::llmResponseReceived);

        AgentController::instance().sendUserMessage(QStringLiteral("Second"));

        QCOMPARE(responseSpy.count(), 1);
        QVERIFY(responseSpy.takeFirst().at(0).toString().contains(QStringLiteral("busy")));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Executing);
        AgentController::instance().transitionTo(AgentController::AgentState::Idle);
    }

    void testVisionRejectionRetriesWithoutImagesOnce() {
        m_mock->imageInputSupported = true;
        QPixmap image(8, 8);
        image.fill(Qt::blue);
        AgentController::instance().sendUserMessageWithImages(QStringLiteral("Inspect"), {image});
        QCOMPARE(m_mock->requestCount, 1);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);

        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.httpStatus = 400;
        info.retryable = false;
        info.visionDowngradeHint = true;
        info.message = QStringLiteral("The model may not support image input.");
        m_mock->emitClassifiedError(info);

        // 自动去图重发一次：仍处 Thinking，第二次请求里的图片已被剥离
        QCOMPARE(m_mock->requestCount, 2);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        bool foundImageRequest = false;
        for (const AgentMessage& message : m_mock->lastCtx.messages) {
            if (message.role == QStringLiteral("user") && message.content == QStringLiteral("Inspect")) {
                foundImageRequest = true;
                QVERIFY(message.images.isEmpty());
            }
        }
        QVERIFY(foundImageRequest);

        // 第二次仍被拒：不再重发，正常报错回 Idle（只上报一次）
        QSignalSpy errorSpy(&AgentController::instance(), &AgentController::llmErrorOccurred);
        // R6：UI 审计在 llmErrorOccurred 直连槽内能读到本次分类；随后控制器复位
        QString categoryDuringEmit;
        const QMetaObject::Connection categoryConnection =
            connect(&AgentController::instance(), &AgentController::llmErrorOccurred, this, [&categoryDuringEmit]() {
                categoryDuringEmit = AgentController::instance().lastLLMErrorCategory();
            });
        m_mock->emitClassifiedError(info);
        disconnect(categoryConnection);
        QCOMPARE(m_mock->requestCount, 2);
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QCOMPARE(categoryDuringEmit, QStringLiteral("BadRequest"));
        QVERIFY2(AgentController::instance().lastLLMErrorCategory().isEmpty(),
                 "error category must be reset once the failed turn is closed");
    }

    void testAbortWhileExecutingReturnsToIdleAndRecovers() {
        AgentController::instance().transitionTo(AgentController::AgentState::Executing);
        QSignalSpy abortSpy(&AgentController::instance(), &AgentController::agentTurnAborted);

        AgentController::instance().abortCurrentTurn();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
        QCOMPARE(abortSpy.count(), 1);

        // 状态机未被破坏：中止后可立即开始新一轮
        AgentController::instance().sendUserMessage(QStringLiteral("after abort"));
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Thinking);
        AgentController::instance().abortCurrentTurn();
        QCOMPARE(AgentController::instance().state(), AgentController::AgentState::Idle);
    }

    void testHandleToolCallStripsElapsedMsAndAuditsDuration() {
        AgentController::instance().setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QSignalSpy logSpy(&AgentController::instance(), &AgentController::actionLogEntryAdded);

        const QJsonObject result =
            AgentController::instance().handleToolCall(QStringLiteral("get_available_plugins"), {});
        QVERIFY2(!result.contains(QStringLiteral("_elapsedMs")), "_elapsedMs must not leak into IPC responses");
        QVERIFY(!result.contains("error"));

        QCOMPARE(logSpy.count(), 1);
        const AgentActionLogEntry entry = logSpy.takeFirst().at(0).value<AgentActionLogEntry>();
        QVERIFY2(entry.durationMs >= 0, "audit durationMs must be collected from _elapsedMs");
        QCOMPARE(entry.action, QStringLiteral("get_available_plugins"));
    }

    void testDefaultPromptIncludesClosedLoopRules() {
        const QString prompt = AgentController::defaultSystemPrompt();
        QVERIFY2(prompt.contains(QStringLiteral("validate_workflow")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("wait defaults to true")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("get_node_runtime_outputs")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("capture_viewport_image")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("At most 2 tool images are attached per turn")), qPrintable(prompt));
        QVERIFY2(prompt.contains(QStringLiteral("Do not repeat the same failing call unchanged")), qPrintable(prompt));
    }

    // R1：失败的写工具不得在审计里被标成 undoable（实际没有压入撤销栈）
    void testFailedRemoveModuleIsAuditedAsNotUndoable() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QVERIFY(!controller.handleToolCall(QStringLiteral("create_project"), QJsonObject{{"name", "r1-audit"}})
                     .contains("error"));

        QSignalSpy logSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.transitionTo(AgentController::AgentState::Executing);
        QJsonObject call;
        call["id"] = "call_r1";
        call["type"] = "function";
        call["name"] = "remove_module";
        call["arguments"] = QJsonObject{{"instanceId", "missing_node"}};
        controller.extendAgentLoop(QJsonArray{call});

        QCOMPARE(logSpy.count(), 1);
        const AgentActionLogEntry entry = logSpy.takeFirst().at(0).value<AgentActionLogEntry>();
        QCOMPARE(entry.action, QStringLiteral("remove_module"));
        QCOMPARE(entry.result, QStringLiteral("error"));
        QVERIFY2(!entry.undoable, "failed remove_module must not be audited as undoable");
        controller.abortCurrentTurn();
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    }

    // R6：最近错误类别不得串用——未分类错误与成功响应都必须清空
    void testLastErrorCategoryDoesNotLeakBetweenTurns() {
        AgentController& controller = AgentController::instance();
        controller.sendUserMessage(QStringLiteral("first"));
        LLMErrorInfo auth;
        auth.category = LLMErrorCategory::Auth;
        auth.message = QStringLiteral("auth failed");
        m_mock->emitClassifiedError(auth); // 最终失败（非视觉降级）
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY2(controller.lastLLMErrorCategory().isEmpty(), "error category must reset after a failed turn");

        controller.sendUserMessage(QStringLiteral("second"));
        m_mock->emitError(QStringLiteral("unclassified failure"));
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY2(controller.lastLLMErrorCategory().isEmpty(),
                 "unclassified llmErrorOccurred must not inherit a stale category");

        // 分类错误后成功响应同样复位
        controller.sendUserMessage(QStringLiteral("third"));
        m_mock->emitClassifiedError(auth);
        controller.sendUserMessage(QStringLiteral("fourth"));
        AgentResponse resp;
        resp.success = true;
        resp.content = QStringLiteral("ok");
        m_mock->emitResponse(resp);
        QVERIFY(controller.lastLLMErrorCategory().isEmpty());
    }

    // R7：未注册工具的早退分支也不得泄漏内部字段 _elapsedMs
    void testUnknownToolEarlyReturnStripsElapsedMs() {
        const QJsonObject result =
            AgentController::instance().handleToolCall(QStringLiteral("not_a_registered_tool"), {});
        QVERIFY(result.contains("error"));
        QVERIFY2(!result.contains(QStringLiteral("_elapsedMs")),
                 "IPC responses must never expose the internal _elapsedMs field");
    }

    // §9.5 第 11 条：执行/等待期间外部 handleToolCall 必须返回 busy
    void testHandleToolCallBusyDuringExecution() {
        AgentController& controller = AgentController::instance();
        controller.transitionTo(AgentController::AgentState::Executing);
        const QJsonObject result = controller.handleToolCall(QStringLiteral("get_flow_state"), {});
        QVERIFY2(result.value("error").toString().contains(QStringLiteral("busy")),
                 qPrintable(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))));
        controller.transitionTo(AgentController::AgentState::Idle);
    }

    // ---------- 第二迭代：上下文治理（设计 §7/§8） ----------

    // I1/I2/I3：工具组原子裁剪——不存在孤立 tool，assistant(tool_calls) 与响应同生共死
    void testToolGroupTrimIsAtomic() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.clearConversation();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QString(20000, QLatin1Char('u'));
        controller.m_conversationHistory.append(user);

        for (int i = 0; i < 40; ++i) {
            AgentMessage assistant;
            assistant.role = QStringLiteral("assistant");
            assistant.toolCalls = QJsonArray{QJsonObject{{"id", QStringLiteral("call-%1").arg(i)}}};
            controller.m_conversationHistory.append(assistant);
            AgentMessage tool;
            tool.role = QStringLiteral("tool");
            tool.toolCallId = QStringLiteral("call-%1").arg(i);
            tool.content = QString(2000, QLatin1Char('t'));
            controller.m_conversationHistory.append(tool);
        }
        const int sizeBefore = controller.m_conversationHistory.size();
        controller.trimHistoryIfNeeded();
        QVERIFY(controller.m_conversationHistory.size() < sizeBefore);
        QVERIFY(controller.m_conversationHistory.size() <= 64);
        QCOMPARE(controller.m_conversationHistory.first().role, QStringLiteral("user"));

        // I1/I2：每个 tool 必须紧邻其 assistant，且 assistant 的所有 id 都有响应
        const QList<AgentMessage>& history = controller.m_conversationHistory;
        for (int i = 0; i < history.size(); ++i) {
            if (history.at(i).role == QStringLiteral("tool")) {
                QVERIFY2(i > 0 && history.at(i - 1).role == QStringLiteral("assistant"),
                         "tool message must directly follow its assistant(tool_calls)");
                bool idMatched = false;
                for (const QJsonValue& call : history.at(i - 1).toolCalls) {
                    if (call.toObject().value(QStringLiteral("id")).toString() == history.at(i).toolCallId) {
                        idMatched = true;
                        break;
                    }
                }
                QVERIFY2(idMatched, "tool message must match an id of the previous assistant");
            }
            if (history.at(i).role != QStringLiteral("assistant") || history.at(i).toolCalls.isEmpty())
                continue;
            QSet<QString> ids;
            for (const QJsonValue& call : history.at(i).toolCalls)
                ids.insert(call.toObject().value(QStringLiteral("id")).toString());
            int j = i + 1;
            QSet<QString> found;
            while (j < history.size() && history.at(j).role == QStringLiteral("tool")) {
                found.insert(history.at(j).toolCallId);
                ++j;
            }
            QVERIFY2((ids - found).isEmpty(), "assistant(tool_calls) lost part of its tool responses");
        }
    }

    // I1/I2 防御性修复：孤立 tool 与不完整组在裁剪入口被整组移除
    void testTrimRepairsOrphanAndIncompleteToolGroups() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(16000, 6000, 24, 24);
        controller.clearConversation();

        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("u");
        controller.m_conversationHistory.append(user);

        AgentMessage orphanTool;
        orphanTool.role = QStringLiteral("tool");
        orphanTool.toolCallId = QStringLiteral("orphan");
        controller.m_conversationHistory.append(orphanTool);

        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.toolCalls = QJsonArray{QJsonObject{{"id", "a"}}, QJsonObject{{"id", "b"}}};
        controller.m_conversationHistory.append(assistant);
        AgentMessage partialTool;
        partialTool.role = QStringLiteral("tool");
        partialTool.toolCallId = QStringLiteral("a");
        controller.m_conversationHistory.append(partialTool);

        controller.trimHistoryIfNeeded();

        QCOMPARE(controller.m_conversationHistory.size(), 1);
        QCOMPARE(controller.m_conversationHistory.first().role, QStringLiteral("user"));
        QVERIFY(controller.m_conversationHistory.first().content == QStringLiteral("u"));
    }

    // §7.4：tool 结果 6k 保首尾截断，中间标注省略字符数（clamp 生效）
    void testToolResultTruncationKeepsHeadAndTail() {
        const QString big = QString(10000, QLatin1Char('x'));
        const QString truncated = AgentController::truncateToolResult(big, 6000);
        QCOMPARE(truncated.size(), 6000 + QStringLiteral("\n…[truncated 4000 chars]…\n").size());
        QVERIFY2(truncated.startsWith(QString(4000, QLatin1Char('x'))), "head must be preserved");
        QVERIFY2(truncated.endsWith(QString(2000, QLatin1Char('x'))), "tail must be preserved");
        QVERIFY2(truncated.contains(QStringLiteral("…[truncated 4000 chars]…")), qPrintable(truncated.mid(3900, 200)));

        // 短结果不截断
        const QString small = QStringLiteral("{\"status\":\"ok\"}");
        QCOMPARE(AgentController::truncateToolResult(small, 6000), small);

        // clamp：maxChars 下限 1000
        const QString clamped = AgentController::truncateToolResult(big, 500);
        QVERIFY2(clamped.contains(QStringLiteral("…[truncated 9000 chars]…")), qPrintable(clamped.mid(600, 120)));
    }

    // §7.1：预算/阈值可配且 clamp；预算内不裁剪
    void testContextBudgetConfigAndNoTrimWithinBudget() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(1, 1, 0, 0);
        QCOMPARE(controller.contextBudgetTokens(), 16000); // clamp 下限
        QCOMPARE(controller.toolResultMaxChars(), 1000);
        QCOMPARE(controller.flowSummaryThreshold(), 1);
        QCOMPARE(controller.flowSummaryPreviewCount(), 1);

        controller.setContextLimitsForTest(2000000, 100000, 5000, 5000);
        QCOMPARE(controller.contextBudgetTokens(), 1048576); // 对齐 DeepSeek Flash 上下文上限
        QCOMPARE(controller.toolResultMaxChars(), 64000);
        QCOMPARE(controller.flowSummaryThreshold(), 1000);
        QCOMPARE(controller.flowSummaryPreviewCount(), 200);

        // 预算内不裁剪
        controller.setContextLimitsForTest(1000000, 6000, 24, 24);
        controller.clearConversation();
        AgentMessage user;
        user.role = QStringLiteral("user");
        user.content = QStringLiteral("small");
        controller.m_conversationHistory.append(user);
        AgentMessage assistant;
        assistant.role = QStringLiteral("assistant");
        assistant.content = QStringLiteral("reply");
        controller.m_conversationHistory.append(assistant);
        controller.trimHistoryIfNeeded();
        QCOMPARE(controller.m_conversationHistory.size(), 2);
    }

    // §8：buildContext 摘要化阈值两侧 + 大工程 16k 硬上限
    void testBuildContextSummarizesLargeProject() {
        AgentController& controller = AgentController::instance();
        controller.setContextLimitsForTest(128000, 6000, 24, 24);

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        for (int i = 0; i < 3; ++i) {
            ModuleInstance instance;
            instance.id = QStringLiteral("inst-%1").arg(i);
            instance.moduleId = QStringLiteral("plugin-%1").arg(i);
            instance.name = instance.id;
            project->addModule(instance);
        }
        QString smallContext = controller.buildContext();
        for (int i = 0; i < 3; ++i)
            QVERIFY2(smallContext.contains(QStringLiteral("inst-%1").arg(i)), qPrintable(smallContext));
        QVERIFY(!smallContext.contains(QStringLiteral("… (+"))); // 小工程不加摘要后缀（提示词里的 (+M) 除外）

        for (int i = 3; i < 25; ++i) {
            ModuleInstance instance;
            instance.id = QStringLiteral("inst-%1").arg(i);
            instance.moduleId = QStringLiteral("plugin-%1").arg(i);
            instance.name = instance.id;
            project->addModule(instance);
        }
        const QString largeContext = controller.buildContext();
        QVERIFY2(largeContext.contains(QStringLiteral("(25)")), qPrintable(largeContext));
        for (int i = 0; i < 24; ++i)
            QVERIFY2(largeContext.contains(QStringLiteral("inst-%1").arg(i)), qPrintable(largeContext));
        QVERIFY2(!largeContext.contains(QStringLiteral("inst-24")), "第 25 个模块名应被摘要省略");
        QVERIFY2(largeContext.contains(QStringLiteral("(+1 more; call get_flow_state for the full list)")),
                 qPrintable(largeContext));
        // Recent Events 仍 ≤5 条（用时间戳行计数；无事件时 0 条）
        static const QRegularExpression eventLine(QStringLiteral("\\[\\d{2}:\\d{2}:\\d{2}\\]"));
        QVERIFY(largeContext.count(eventLine) <= 5);

        // 16k 硬上限：阈值放大到 1000 且 200 个超长模块 → 强制摘要 + 截断标记 + 长度受限
        ProjectManager::instance().closeProject();
        Project* huge = ProjectManager::instance().newProject();
        QVERIFY(huge != nullptr);
        for (int i = 0; i < 200; ++i) {
            ModuleInstance instance;
            instance.id = QStringLiteral("huge-instance-with-a-quite-long-name-%1").arg(i);
            instance.moduleId =
                QStringLiteral("com.deeplux.huge.plugin.with.a.very.long.module.identifier.number-%1").arg(i);
            instance.name = instance.id;
            huge->addModule(instance);
        }
        controller.setContextLimitsForTest(-1, -1, 1000, 200);
        const QString hugeContext = controller.buildContext();
        QVERIFY2(hugeContext.size() <= 16384,
                 qPrintable(QStringLiteral("buildContext size=%1").arg(hugeContext.size())));
        QVERIFY2(hugeContext.contains(QStringLiteral("…[context truncated]")), qPrintable(hugeContext.right(200)));
        ProjectManager::instance().closeProject();
    }

    // ---------- L3：视觉能力模型名启发式 ----------

    void testVisionHeuristicCommonModels() {
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("gpt-4o")));
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("claude-3-5-sonnet")));
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("qwen2.5-vl-72b")));
        QVERIFY(!OpenAILLMClient::heuristicSupportsVision(QStringLiteral("deepseek-reasoner")));
        QVERIFY(!OpenAILLMClient::heuristicSupportsVision(QStringLiteral("deepseek-chat")));
        QVERIFY(!OpenAILLMClient::heuristicSupportsVision(QStringLiteral("o1-mini")));
        QVERIFY(OpenAILLMClient::heuristicSupportsVision(QStringLiteral("unknown-model-x")));
    }

    // ---------- L9：单轮全局 deadline ----------

    void testTurnDeadlineAbortsContinuation() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineMsForTest(1); // 绕过 clamp，仅测试触发
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        QSignalSpy abortSpy(&controller, &AgentController::agentTurnAborted);

        controller.sendUserMessage(QStringLiteral("deadline"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QThread::msleep(8);
        controller.transitionTo(AgentController::AgentState::Executing);
        const int requestsBefore = m_mock->requestCount;
        controller.extendAgentLoop(
            QJsonArray{QJsonObject{{"id", "call_deadline"}, {"name", "get_flow_state"}, {"arguments", "{}"}}});

        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY2(errorSpy.at(0).at(0).toString().contains(QStringLiteral("单轮总时长")),
                 qPrintable(errorSpy.at(0).at(0).toString()));
        QCOMPARE(abortSpy.count(), 1);
        QCOMPARE(m_mock->requestCount, requestsBefore); // 超时后不得续跑
    }

    void testTurnDeadlineResetsPerUserMessage() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineMsForTest(1);
        controller.sendUserMessage(QStringLiteral("first"));
        QThread::msleep(8);
        QVERIFY(controller.turnDeadlineExceeded());
        controller.endTurnWithReason(QStringLiteral("deadline"), QString(), false);

        controller.setTurnDeadlineMsForTest(-1); // 恢复配置默认
        controller.sendUserMessage(QStringLiteral("second"));
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        QVERIFY2(!controller.turnDeadlineExceeded(), "新用户消息必须重置计时");
        controller.abortCurrentTurn();
    }

    void testTurnDeadlineClamp() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineConfigForTest(1);
        QCOMPARE(controller.turnDeadlineMs(), 10000);
        controller.setTurnDeadlineConfigForTest(1000000000);
        QCOMPARE(controller.turnDeadlineMs(), 3600000);
        controller.setTurnDeadlineConfigForTest(-1);
        QCOMPARE(controller.turnDeadlineMs(), 600000); // 默认配置
    }

    void testTurnDeadlineDoesNotFireEarly() {
        AgentController& controller = AgentController::instance();
        controller.setTurnDeadlineConfigForTest(600000);
        controller.sendUserMessage(QStringLiteral("quick"));
        QTest::qWait(5);
        QVERIFY(!controller.turnDeadlineExceeded());
        QCOMPARE(controller.state(), AgentController::AgentState::Thinking);
        controller.abortCurrentTurn();
    }
};

QTEST_MAIN(TestAgentController)
#include "test_agentcontroller.moc"
