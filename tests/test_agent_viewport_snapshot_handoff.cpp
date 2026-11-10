#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>
#include <functional>
#include <memory>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/ILLMClient.h>
#include <core/agent/ToolSchema.h>
#include <core/display/DisplayData.h>
#include <core/model/ImageData.h>
#include <ui/display/3d/Viewport3DContent.h>
#include <ui/display/DisplayManager.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/HImageWidget.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

namespace {

void respondWhenFileDialogOpens(QObject* context, const std::function<void(QFileDialog*)>& respond) {
    auto* poller = new QTimer(context);
    poller->setInterval(10);
    auto attempts = std::make_shared<int>(0);
    QObject::connect(poller, &QTimer::timeout, context, [poller, attempts, respond]() {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            auto* dialog = qobject_cast<QFileDialog*>(widget);
            if (dialog && dialog->isVisible()) {
                poller->stop();
                poller->deleteLater();
                respond(dialog);
                return;
            }
        }

        if (++(*attempts) < 500)
            return;

        poller->stop();
        poller->deleteLater();
        QTest::qFail("Timed out waiting for the native snapshot file dialog", __FILE__, __LINE__);
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QFileDialog*>(widget))
                dialog->reject();
        }
    });
    poller->start();
}

} // namespace

class SnapshotHandoffLlmClient : public ILLMClient {
    Q_OBJECT

public:
    using ILLMClient::ILLMClient;
    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}
    void sendRequest(const AgentConversation& context, const QList<ToolDefinition>&) override {
        m_requestId = context.requestId;
    }
    void reply(const AgentResponse& response) {
        AgentResponse correlated = response;
        if (correlated.requestId.isEmpty())
            correlated.requestId = m_requestId;
        emit responseReceived(correlated);
        QCoreApplication::processEvents();
    }

private:
    QString m_requestId;
};

class AgentControllerStateRestore {
public:
    explicit AgentControllerStateRestore(AgentController& controller)
        : m_controller(controller), m_client(controller.llmClient()), m_permission(controller.permissionLevel()) {}
    ~AgentControllerStateRestore() {
        m_controller.setLLMClient(m_client);
        m_controller.setPermissionLevel(m_permission);
        m_controller.clearConversation();
    }

private:
    AgentController& m_controller;
    ILLMClient* m_client;
    AgentController::PermissionLevel m_permission;
};

class TestAgentViewportSnapshotHandoff : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        ToolSchema::instance().registerDefaultTools();
    }

    void testSchemaPermissionAndStrictParamsBeforeConfirmation() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        const ToolDefinition tool = ToolSchema::instance().findTool(QStringLiteral("save_viewport_snapshot_for_user"));
        QCOMPARE(tool.name, QStringLiteral("save_viewport_snapshot_for_user"));
        QVERIFY(tool.dangerous);
        QCOMPARE(tool.params.size(), 1);
        QCOMPARE(tool.params.first().name, QStringLiteral("viewportId"));

        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        ViewportWidget* viewport = window.findChild<DisplayManager*>()->allViewports().first();
        QImage image(45, 32, QImage::Format_RGB32);
        image.fill(QColor(0x21, 0x73, 0xA9));
        viewport->displayImage(image);

        QSignalSpy confirmationSpy(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QString secretPath = QStringLiteral("/tmp/SLICE46_SECRET_PATH.png");
        const QJsonObject invalidParams{{QStringLiteral("viewportId"), viewport->viewportId()},
                                        {QStringLiteral("path"), secretPath}};
        for (const AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Advisor, AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            const QJsonObject rejected =
                controller.handleToolCall(QStringLiteral("save_viewport_snapshot_for_user"), invalidParams);
            QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
            QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        }
        QCOMPARE(confirmationSpy.count(), 0);
        QCOMPARE(auditSpy.count(), 0);
        QVERIFY(QApplication::activeModalWidget() == nullptr);

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        QCOMPARE(controller.evaluateToolGate(QStringLiteral("save_viewport_snapshot_for_user")),
                 AgentController::ToolGate::Deny);
        const QJsonObject denied =
            controller.handleToolCall(QStringLiteral("save_viewport_snapshot_for_user"),
                                      QJsonObject{{QStringLiteral("viewportId"), viewport->viewportId()}});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QCOMPARE(auditSpy.count(), 0);
        QVERIFY(QApplication::activeModalWidget() == nullptr);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QCOMPARE(controller.evaluateToolGate(QStringLiteral("save_viewport_snapshot_for_user")),
                 AgentController::ToolGate::RequireConfirmation);
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        QCOMPARE(controller.evaluateToolGate(QStringLiteral("save_viewport_snapshot_for_user")),
                 AgentController::ToolGate::RequireConfirmation);
    }

    void testConfirmationExpiresWhenMainWindowOwnerIsReplaced() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow originalWindow;
        originalWindow.show();
        QCoreApplication::processEvents();
        ViewportWidget* originalViewport = originalWindow.findChild<DisplayManager*>()->allViewports().first();
        QImage image(36, 24, QImage::Format_RGB32);
        image.fill(Qt::yellow);
        originalViewport->displayImage(image);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("save_viewport_snapshot_for_user"),
                                      QJsonObject{{QStringLiteral("viewportId"), originalViewport->viewportId()}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();

        MainWindow replacementWindow;
        replacementWindow.show();
        QCoreApplication::processEvents();
        ViewportWidget* replacementViewport = replacementWindow.findChild<DisplayManager*>()->allViewports().first();
        QCOMPARE(replacementViewport->viewportId(), originalViewport->viewportId());

        QVERIFY(controller.confirmPendingTools(confirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QVERIFY(toolSpy.isEmpty());
        QVERIFY(QApplication::activeModalWidget() == nullptr);
    }

    void testChatRejectsUnknownPathBeforeCreatingConfirmation() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        ViewportWidget* viewport = window.findChild<DisplayManager*>()->allViewports().first();
        QImage image(40, 30, QImage::Format_RGB32);
        image.fill(Qt::cyan);
        viewport->displayImage(image);

        SnapshotHandoffLlmClient client;
        controller.setLLMClient(&client);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy confirmationSpy(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy errorSpy(&controller, &AgentController::llmErrorOccurred);
        controller.sendUserMessage(QStringLiteral("Save a viewport snapshot"));

        AgentResponse response;
        response.success = true;
        response.toolCalls.append(
            QJsonObject{{QStringLiteral("id"), QStringLiteral("snapshot-extra-path")},
                        {QStringLiteral("name"), QStringLiteral("save_viewport_snapshot_for_user")},
                        {QStringLiteral("arguments"),
                         QJsonObject{{QStringLiteral("viewportId"), viewport->viewportId()},
                                     {QStringLiteral("path"), QStringLiteral("/tmp/SLICE46_CHAT_SECRET.png")}}}});
        client.reply(response);

        QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        QCOMPARE(confirmationSpy.count(), 0);
        QVERIFY(controller.pendingConfirmationPayload().isEmpty());
        QVERIFY(!controller.hasPendingTools());
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(auditSpy.count() >= 1);
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            const QByteArray audit = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            QVERIFY(!audit.contains("SLICE46_CHAT_SECRET.png"));
        }
        QVERIFY(QApplication::activeModalWidget() == nullptr);
    }

    void testAdvisorConfirmsThenUserSaves2dImageWithoutPathDisclosure() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        ViewportWidget* viewport = window.findChild<DisplayManager*>()->allViewports().first();
        QVERIFY(viewport);
        QImage image(52, 34, QImage::Format_RGB32);
        image.fill(QColor(0x36, 0x8B, 0xC1));
        viewport->displayImage(image);

        QTemporaryDir outputDir;
        QVERIFY(outputDir.isValid());
        const QString targetPath = outputDir.filePath(QStringLiteral("SLICE46_PRIVATE_OUTPUT.png"));
        QVERIFY(!QFileInfo::exists(targetPath));
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QSignalSpy confirmationSpy(&controller, &AgentController::toolsPendingConfirmation);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);

        const QJsonObject pending =
            controller.handleToolCall(QStringLiteral("save_viewport_snapshot_for_user"),
                                      QJsonObject{{QStringLiteral("viewportId"), viewport->viewportId()}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(confirmationSpy.count(), 1);
        const QByteArray confirmation = QJsonDocument(controller.pendingConfirmationPayload()).toJson();
        QVERIFY(!confirmation.contains("SLICE46_PRIVATE_OUTPUT.png"));
        QVERIFY(!QFileInfo::exists(targetPath));

        respondWhenFileDialogOpens(this, [targetPath](QFileDialog* dialog) {
            dialog->selectFile(targetPath);
            static_cast<QDialog*>(dialog)->done(QDialog::Accepted);
        });
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(targetPath), 5000);
        QVERIFY(!toolSpy.isEmpty());
        const QJsonObject result = toolSpy.takeLast().at(1).toJsonObject();
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("saved"));
        QVERIFY(!QJsonDocument(result).toJson().contains("SLICE46_PRIVATE_OUTPUT.png"));
        QImage saved(targetPath);
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.size(), viewport->imageWidget()->size());

        bool foundAudit = false;
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            if (entry.action != QLatin1String("save_viewport_snapshot_for_user"))
                continue;
            foundAudit = true;
            const QByteArray audit = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            QVERIFY(!audit.contains("SLICE46_PRIVATE_OUTPUT.png"));
            QCOMPARE(QJsonDocument::fromJson(entry.params.toUtf8()).object().size(), 1);
            QVERIFY(!entry.undoable);
        }
        QVERIFY(foundAudit);
    }

    void testCancelAndDeclinedOverwriteLeaveFilesUnchanged() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        ViewportWidget* viewport = window.findChild<DisplayManager*>()->allViewports().first();
        QImage image(30, 22, QImage::Format_RGB32);
        image.fill(Qt::magenta);
        viewport->displayImage(image);
        QTemporaryDir outputDir;
        QVERIFY(outputDir.isValid());
        const QString cancelPath = outputDir.filePath(QStringLiteral("cancelled.png"));
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);

        const QJsonObject cancelPending =
            controller.handleToolCall(QStringLiteral("save_viewport_snapshot_for_user"),
                                      QJsonObject{{QStringLiteral("viewportId"), viewport->viewportId()}});
        QCOMPARE(cancelPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        respondWhenFileDialogOpens(this, [](QFileDialog* dialog) {
            dialog->reject();
        });
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolSpy.count(), 1, 5000);
        const QList<QVariant> cancelledToolCall = toolSpy.takeLast();
        QCOMPARE(cancelledToolCall.size(), 2);
        QCOMPARE(cancelledToolCall.at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("cancelled"));
        QVERIFY(!QFileInfo::exists(cancelPath));
        bool cancellationAuditedAccurately = false;
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            if (entry.action == QLatin1String("save_viewport_snapshot_for_user") &&
                entry.result == QLatin1String("cancelled"))
                cancellationAuditedAccurately = true;
        }
        QVERIFY(cancellationAuditedAccurately);

        const QString existingPath = outputDir.filePath(QStringLiteral("existing.png"));
        const QByteArray originalBytes = QByteArrayLiteral("SLICE46_EXISTING_BYTES_MUST_SURVIVE");
        QFile existing(existingPath);
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write(originalBytes), qint64(originalBytes.size()));
        existing.close();
        const QJsonObject overwritePending =
            controller.handleToolCall(QStringLiteral("save_viewport_snapshot_for_user"),
                                      QJsonObject{{QStringLiteral("viewportId"), viewport->viewportId()}});
        QCOMPARE(overwritePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        bool finalTargetWasChecked = false;
        const QString selectedPathWithoutSuffix =
            existingPath.left(existingPath.size() - QStringLiteral(".png").size());
        const QJsonObject refusedOverwrite = viewport->saveSnapshotForUser(
            {}, false, [selectedPathWithoutSuffix]() { return selectedPathWithoutSuffix; },
            [&finalTargetWasChecked, existingPath](const QString& finalTarget) {
                finalTargetWasChecked = true;
                return finalTarget != existingPath;
            });
        QCOMPARE(refusedOverwrite.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
        QVERIFY(finalTargetWasChecked);
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), originalBytes);
        existing.close();
    }

    void test3dFramebufferCaptureSeamAndNoFrameFailure() {
        ViewportWidget viewport(QStringLiteral("snapshot-test"));
        PointCloudData cloud;
        cloud.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.5}, {0.0, 1.0, 1.0}};
        DisplayData cloudData;
        cloudData.variant() = cloud;
        viewport.setDualData(QImage(20, 16, QImage::Format_RGB32), cloudData, ViewportWidget::DisplayMode::Auto3D);
        QCOMPARE(viewport.displayMode(), ViewportWidget::DisplayMode::Auto3D);

        QTemporaryDir outputDir;
        QVERIFY(outputDir.isValid());
        const QString targetPath = outputDir.filePath(QStringLiteral("synthetic-frame.png"));
        QImage frame(27, 19, QImage::Format_ARGB32);
        frame.fill(QColor(0x80, 0x20, 0x40, 0xFF));
        const QJsonObject savedResult = viewport.saveSnapshotForUser(
            [frame]() { return frame; }, false, [targetPath]() { return targetPath; });
        QCOMPARE(savedResult.value(QStringLiteral("status")).toString(), QStringLiteral("saved"));
        QImage savedFrame(targetPath);
        QVERIFY(!savedFrame.isNull());
        QCOMPARE(savedFrame.size(), frame.size());
        QCOMPARE(savedFrame.pixelColor(0, 0), frame.pixelColor(0, 0));

        viewport.setDualData(QImage(20, 16, QImage::Format_RGB32), cloudData, ViewportWidget::DisplayMode::Auto3D);
        const QStringList dialogsBefore = [&]() {
            QStringList names;
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (qobject_cast<QFileDialog*>(widget) && widget->isVisible())
                    names.append(widget->objectName());
            }
            return names;
        }();
        const QJsonObject noFrame = viewport.saveSnapshotForUser();
        QCOMPARE(noFrame.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_SNAPSHOT"));
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (qobject_cast<QFileDialog*>(widget) && widget->isVisible())
                QVERIFY(dialogsBefore.contains(widget->objectName()));
        }

        const QString failedPath = outputDir.filePath(QStringLiteral("missing-parent/fail.png"));
        const QJsonObject failedSave =
            viewport.saveSnapshotForUser([frame]() { return frame; }, false, [failedPath]() { return failedPath; });
        QCOMPARE(failedSave.value(QStringLiteral("errorCode")).toString(), QStringLiteral("SAVE_FAILED"));
        QVERIFY(!QFileInfo::exists(failedPath));
    }

    void testToolbarKeepsLegacy2dFallbackWhen3dFramebufferIsUnavailable() {
        ViewportWidget viewport(QStringLiteral("toolbar-snapshot-test"));
        viewport.resize(360, 240);
        viewport.show();
        QImage previousImage(24, 18, QImage::Format_RGB32);
        previousImage.fill(Qt::green);
        viewport.displayImage(previousImage);

        PointCloudData cloud;
        cloud.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 0.5}, {0.0, 1.0, 1.0}};
        DisplayData cloudData;
        cloudData.variant() = cloud;
        viewport.setDualData(QImage(20, 16, QImage::Format_RGB32), cloudData, ViewportWidget::DisplayMode::Auto3D);
        QCOMPARE(viewport.displayMode(), ViewportWidget::DisplayMode::Auto3D);
        QVERIFY(viewport.imageWidget()->hasImage());

        QTemporaryDir outputDir;
        QVERIFY(outputDir.isValid());
        const QString targetPath = outputDir.filePath(QStringLiteral("toolbar-legacy.png"));
        QAction* snapshotAction = viewport.findChild<QAction*>(QStringLiteral("ViewportSnapshotAction"));
        QVERIFY(snapshotAction);
        QVERIFY(snapshotAction->isEnabled());
        const QImage expectedLegacyCapture = viewport.imageWidget()->grab().toImage();
        const QJsonObject fallbackResult = viewport.saveSnapshotForUser(
            []() { return QImage(); }, false, [targetPath]() { return targetPath; }, {}, true);
        QCOMPARE(fallbackResult.value(QStringLiteral("status")).toString(), QStringLiteral("saved"));

        QImage saved(targetPath);
        QVERIFY(!saved.isNull());
        QCOMPARE(saved.size(), viewport.imageWidget()->size());
        QCOMPARE(saved.convertToFormat(expectedLegacyCapture.format()), expectedLegacyCapture);
    }

private:
};

QTEST_MAIN(TestAgentViewportSnapshotHandoff)

#include "test_agent_viewport_snapshot_handoff.moc"
