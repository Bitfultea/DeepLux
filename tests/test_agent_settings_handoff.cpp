#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/OpenAILLMClient.h>
#include <core/agent/ToolSchema.h>
#include <core/manager/ConfigManager.h>
#include <thread>
#include <ui/dialogs/AgentSettingsDialog.h>
#include <ui/views/MainWindow.h>

using namespace DeepLux;

class TestAgentSettingsHandoff : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = new QTemporaryDir();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        QVERIFY(m_server.listen(QHostAddress::LocalHost, 0));
        m_fixtureEndpoint = QStringLiteral("http://127.0.0.1:%1/v1/chat/completions").arg(m_server.serverPort());

        ConfigManager& config = ConfigManager::instance();
        QVERIFY(config.initialize());
        config.setGroup(QStringLiteral("agent"), initialSettings());
        QVERIFY(config.save());
        ToolSchema::instance().registerDefaultTools();
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
        delete m_appDataDir;
        m_appDataDir = nullptr;
    }

    void testRequiresGui() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject result = controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }

    void testConnectionButtonSendsLocalRequestAndReportsResponse() {
        AgentSettingsDialog dialog;
        auto* endpointEdit = dialog.findChild<QLineEdit*>(QStringLiteral("AgentSettingsEndpointEdit"));
        auto* apiKeyEdit = dialog.findChild<QLineEdit*>(QStringLiteral("AgentSettingsApiKeyEdit"));
        auto* modelEdit = dialog.findChild<QLineEdit*>(QStringLiteral("AgentSettingsModelEdit"));
        auto* thinkingCheck = dialog.findChild<QCheckBox*>(QStringLiteral("AgentSettingsThinkingEnabledCheck"));
        auto* testButton = dialog.findChild<QPushButton*>(QStringLiteral("AgentSettingsTestConnectionButton"));
        QVERIFY(endpointEdit);
        QVERIFY(apiKeyEdit);
        QVERIFY(modelEdit);
        QVERIFY(thinkingCheck);
        QVERIFY(testButton);

        endpointEdit->setText(m_fixtureEndpoint);
        apiKeyEdit->setText(QStringLiteral("synthetic-connection-test-key"));
        modelEdit->setText(QStringLiteral("synthetic-connection-test-model"));
        thinkingCheck->setChecked(true);
        dialog.show();
        QCoreApplication::processEvents();
        QVERIFY(dialog.isVisible());
        QVERIFY(testButton->isVisible());
        QCOMPARE(endpointEdit->text(), m_fixtureEndpoint);
        QCOMPARE(apiKeyEdit->text(), QStringLiteral("synthetic-connection-test-key"));
        QCOMPARE(modelEdit->text(), QStringLiteral("synthetic-connection-test-model"));

        QByteArray request;
        bool requestComplete = false;
        bool buttonWasDisabledDuringRequest = false;
        connect(&m_server, &QTcpServer::newConnection, &dialog,
                [this, &dialog, testButton, &request, &requestComplete, &buttonWasDisabledDuringRequest]() {
                    while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                        connect(socket, &QTcpSocket::readyRead, socket,
                                [socket, &request, &requestComplete, &buttonWasDisabledDuringRequest, testButton]() {
                                    request.append(socket->readAll());
                                    const int headerEnd = request.indexOf("\r\n\r\n");
                                    if (headerEnd < 0)
                                        return;
                                    int contentLength = 0;
                                    for (const QByteArray& header : request.left(headerEnd).split('\n')) {
                                        if (header.toLower().startsWith("content-length:"))
                                            contentLength = header.mid(header.indexOf(':') + 1).trimmed().toInt();
                                    }
                                    if (request.size() < headerEnd + 4 + contentLength)
                                        return;
                                    requestComplete = true;
                                    buttonWasDisabledDuringRequest = !testButton->isEnabled();
                                    const QByteArray body = R"({"model":"synthetic-fixture-model"})";
                                    socket->write(
                                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                        QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                                    socket->disconnectFromHost();
                                });
                    }
                });

        QString feedbackTitle;
        QString feedbackText;
        QTimer feedbackPoll;
        feedbackPoll.setInterval(10);
        connect(&feedbackPoll, &QTimer::timeout, &dialog, [&]() {
            auto* messageBox = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!messageBox)
                return;
            feedbackTitle = messageBox->windowTitle();
            feedbackText = messageBox->text();
            messageBox->accept();
        });
        feedbackPoll.start();
        QSignalSpy clickSpy(testButton, &QPushButton::clicked);
        QTest::mouseClick(testButton, Qt::LeftButton);
        QCOMPARE(clickSpy.count(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(requestComplete, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!feedbackText.isEmpty(), 5000);
        feedbackPoll.stop();
        QVERIFY(buttonWasDisabledDuringRequest);
        QVERIFY(testButton->isEnabled());
        QCOMPARE(feedbackTitle, QStringLiteral("测试连接"));
        QCOMPARE(feedbackText, QStringLiteral("连接成功！\n模型: synthetic-fixture-model"));
        const int headerEnd = request.indexOf("\r\n\r\n");
        QVERIFY(headerEnd >= 0);
        const QByteArray headers = request.left(headerEnd);
        QVERIFY(headers.startsWith("POST /v1/chat/completions "));
        QVERIFY(headers.toLower().contains("authorization: bearer synthetic-connection-test-key"));
        const QJsonObject body = requestBody(request);
        QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("synthetic-connection-test-model"));
        QCOMPARE(body.value(QStringLiteral("max_tokens")).toInt(), 5);
        const QJsonArray messages = body.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages.first().toObject().value(QStringLiteral("role")).toString(), QStringLiteral("user"));
        QCOMPARE(messages.first().toObject().value(QStringLiteral("content")).toString(), QStringLiteral("Hi"));
        QCOMPARE(body.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
                 QStringLiteral("enabled"));
    }

    void testOpenCancelManualSaveAndModalReuse() {
        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        ConfigManager& config = ConfigManager::instance();
        const QJsonObject configBefore = config.getGroup(QStringLiteral("agent"));
        const QString configPath = config.configFilePath();
        QFile configFile(configPath);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        const QByteArray fileBytesBefore = configFile.readAll();
        configFile.close();
        const QString promptBefore = controller.systemPrompt();
        const AgentController::PermissionLevel permissionBefore = controller.permissionLevel();
        auto* clientBefore = qobject_cast<OpenAILLMClient*>(controller.llmClient());
        QVERIFY(clientBefore);

        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject invalidParams = controller.executeDataSourceViewAction(
            QStringLiteral("open_agent_settings_dialog"), QJsonObject{{QStringLiteral("unexpected"), true}});
        QCOMPARE(invalidParams.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(window.findChildren<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog")).isEmpty());

        QJsonObject wrongThread;
        std::thread worker([&]() {
            wrongThread = controller.executeDataSourceViewAction(QStringLiteral("open_agent_settings_dialog"), {});
        });
        worker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(window.findChildren<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog")).isEmpty());

        const QJsonObject opened = controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
        QCOMPARE(opened.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        QCOMPARE(opened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QVERIFY(opened.value(QStringLiteral("userActionRequired")).toBool());
        QCOMPARE(opened.size(), 3);
        QVERIFY(!QJsonDocument(opened).toJson(QJsonDocument::Compact).contains("synthetic"));
        QCOMPARE(config.getGroup(QStringLiteral("agent")), configBefore);
        QCOMPARE(controller.systemPrompt(), promptBefore);
        QCOMPARE(controller.permissionLevel(), permissionBefore);
        QCOMPARE(controller.llmClient(), clientBefore);

        AgentSettingsDialog* firstDialog =
            window.findChild<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog"));
        QVERIFY(firstDialog);
        QVERIFY(firstDialog->isVisible());
        QVERIFY(!firstDialog->isModal());
        auto* initialApiKeyEdit = firstDialog->findChild<QLineEdit*>(QStringLiteral("AgentSettingsApiKeyEdit"));
        QVERIFY(initialApiKeyEdit);
        QCOMPARE(initialApiKeyEdit->echoMode(), QLineEdit::Password);
        initialApiKeyEdit->clearFocus();
        const QImage loadedKeyView = initialApiKeyEdit->grab().toImage();
        initialApiKeyEdit->setText(QStringLiteral("synthetic-seed-xyz")); // same length as stored key
        QCoreApplication::processEvents();
        QVERIFY(loadedKeyView == initialApiKeyEdit->grab().toImage());
        QPointer<AgentSettingsDialog> firstGuard(firstDialog);

        for (const AgentController::PermissionLevel level :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(level);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_agent_settings_dialog")),
                     AgentController::ToolGate::Allow);
        }
        controller.setPermissionLevel(permissionBefore);
        const QJsonObject reused = controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
        QCOMPARE(reused.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QCOMPARE(window.findChildren<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog")).size(), 1);

        firstDialog->close();
        const QJsonObject reopened = controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
        QCOMPARE(reopened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        const QList<AgentSettingsDialog*> pendingDialogs =
            window.findChildren<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog"));
        QCOMPARE(pendingDialogs.size(), 2);
        AgentSettingsDialog* activeDialog = nullptr;
        for (AgentSettingsDialog* candidate : pendingDialogs) {
            if (candidate->isVisible())
                activeDialog = candidate;
        }
        QVERIFY(activeDialog);
        QVERIFY(activeDialog != firstDialog);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(firstGuard.isNull());
        QCOMPARE(window.findChildren<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog")).size(), 1);
        activeDialog->hide();
        const QJsonObject reusedAfterOldDelete =
            controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
        QCOMPARE(reusedAfterOldDelete.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QCOMPARE(window.findChildren<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog")).size(), 1);
        QVERIFY(activeDialog->isVisible());

        int permissionChangeCount = 0;
        connect(&controller, &AgentController::permissionLevelChanged, &window,
                [&permissionChangeCount](AgentController::PermissionLevel) { ++permissionChangeCount; });
        auto* cancelButton = activeDialog->findChild<QPushButton*>(QStringLiteral("AgentSettingsCancelButton"));
        QVERIFY(cancelButton);
        QPointer<AgentSettingsDialog> cancelGuard(activeDialog);
        cancelButton->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(cancelGuard.isNull());
        QCOMPARE(permissionChangeCount, 0);
        QCOMPARE(config.getGroup(QStringLiteral("agent")), configBefore);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        QCOMPARE(configFile.readAll(), fileBytesBefore);
        configFile.close();
        QCOMPARE(controller.systemPrompt(), promptBefore);
        QCOMPARE(controller.permissionLevel(), permissionBefore);
        QCOMPARE(controller.llmClient(), clientBefore);
        QVERIFY(clientBefore->supportsImageInput());
        QByteArray initialRequestBytes;
        QVERIFY2(captureClientRequest(clientBefore,
                                      {ToolSchema::instance().findTool(QStringLiteral("get_run_results"))},
                                      &initialRequestBytes),
                 "OpenAI client failed to send the local fixture request after Cancel");
        QVERIFY(initialRequestBytes.toLower().contains("authorization: bearer synthetic-seed-key"));
        const QJsonObject initialRequestBody = requestBody(initialRequestBytes);
        QCOMPARE(initialRequestBody.value(QStringLiteral("model")).toString(), QStringLiteral("synthetic-seed-model"));
        QCOMPARE(initialRequestBody.value(QStringLiteral("temperature")).toDouble(), 0.25);
        QCOMPARE(initialRequestBody.value(QStringLiteral("max_tokens")).toInt(), 768);
        QVERIFY(initialRequestBody.value(QStringLiteral("tools")).isArray());
        QVERIFY(!initialRequestBody.contains(QStringLiteral("thinking")));

        const QJsonObject saveDialogResult =
            controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
        QCOMPARE(saveDialogResult.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        AgentSettingsDialog* saveDialog = window.findChild<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog"));
        QVERIFY(saveDialog);
        saveDialog->findChild<QLineEdit*>(QStringLiteral("AgentSettingsEndpointEdit"))->setText(m_fixtureEndpoint);
        auto* apiKeyEdit = saveDialog->findChild<QLineEdit*>(QStringLiteral("AgentSettingsApiKeyEdit"));
        QVERIFY(apiKeyEdit);
        QCOMPARE(apiKeyEdit->echoMode(), QLineEdit::Password);
        apiKeyEdit->setText(QStringLiteral("synthetic-handoff-api-key"));
        apiKeyEdit->clearFocus();
        const QImage savedKeyView = apiKeyEdit->grab().toImage();
        apiKeyEdit->setText(QStringLiteral("synthetic-handoff-api-xyz")); // same length as submitted key
        QCoreApplication::processEvents();
        QVERIFY(savedKeyView == apiKeyEdit->grab().toImage());
        apiKeyEdit->setText(QStringLiteral("synthetic-handoff-api-key"));
        saveDialog->findChild<QLineEdit*>(QStringLiteral("AgentSettingsModelEdit"))
            ->setText(QStringLiteral("synthetic-handoff-model"));
        saveDialog->findChild<QDoubleSpinBox*>(QStringLiteral("AgentSettingsTemperatureSpin"))->setValue(0.7);
        saveDialog->findChild<QSpinBox*>(QStringLiteral("AgentSettingsMaxTokensSpin"))->setValue(1536);
        auto* permissionCombo = saveDialog->findChild<QComboBox*>(QStringLiteral("AgentSettingsPermissionCombo"));
        QVERIFY(permissionCombo);
        permissionCombo->setCurrentIndex(
            permissionCombo->findData(static_cast<int>(AgentController::PermissionLevel::Autopilot)));
        saveDialog->findChild<QTextEdit*>(QStringLiteral("AgentSettingsSystemPromptEdit"))
            ->setPlainText(QStringLiteral("synthetic handoff prompt"));
        saveDialog->findChild<QCheckBox*>(QStringLiteral("AgentSettingsToolsEnabledCheck"))->setChecked(false);
        saveDialog->findChild<QCheckBox*>(QStringLiteral("AgentSettingsVisionEnabledCheck"))->setChecked(false);
        saveDialog->findChild<QCheckBox*>(QStringLiteral("AgentSettingsThinkingEnabledCheck"))->setChecked(true);
        auto* okButton = saveDialog->findChild<QPushButton*>(QStringLiteral("AgentSettingsOkButton"));
        QVERIFY(okButton);
        QPointer<AgentSettingsDialog> savedGuard(saveDialog);
        okButton->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(savedGuard.isNull());

        QCOMPARE(permissionChangeCount, 1);
        QCOMPARE(controller.permissionLevel(), AgentController::PermissionLevel::Autopilot);
        QCOMPARE(controller.systemPrompt(), QStringLiteral("synthetic handoff prompt"));
        QCOMPARE(controller.llmClient(), clientBefore);
        auto* updatedClient = qobject_cast<OpenAILLMClient*>(controller.llmClient());
        QVERIFY(updatedClient);
        QVERIFY(!updatedClient->supportsImageInput());
        const QJsonObject persisted = config.getGroup(QStringLiteral("agent"));
        QCOMPARE(persisted.value(QStringLiteral("endpoint")).toString(), m_fixtureEndpoint);
        QCOMPARE(persisted.value(QStringLiteral("apiKey")).toString(), QStringLiteral("synthetic-handoff-api-key"));
        QCOMPARE(persisted.value(QStringLiteral("model")).toString(), QStringLiteral("synthetic-handoff-model"));
        QCOMPARE(persisted.value(QStringLiteral("temperature")).toDouble(), 0.7);
        QCOMPARE(persisted.value(QStringLiteral("maxTokens")).toInt(), 1536);
        QCOMPARE(persisted.value(QStringLiteral("permissionLevel")).toInt(),
                 static_cast<int>(AgentController::PermissionLevel::Autopilot));
        QCOMPARE(persisted.value(QStringLiteral("systemPrompt")).toString(),
                 QStringLiteral("synthetic handoff prompt"));
        QCOMPARE(persisted.value(QStringLiteral("toolsEnabled")).toBool(), false);
        QCOMPARE(persisted.value(QStringLiteral("visionEnabled")).toBool(), false);
        QCOMPARE(persisted.value(QStringLiteral("thinkingEnabled")).toBool(), true);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        const QJsonObject persistedOnDisk =
            QJsonDocument::fromJson(configFile.readAll()).object().value(QStringLiteral("agent")).toObject();
        configFile.close();
        QCOMPARE(persistedOnDisk, persisted);

        QByteArray updatedRequestBytes;
        QVERIFY2(captureClientRequest(updatedClient,
                                      {ToolSchema::instance().findTool(QStringLiteral("get_run_results"))},
                                      &updatedRequestBytes),
                 "Updated OpenAI client failed to send to the local HTTP fixture");
        QVERIFY(updatedRequestBytes.toLower().contains("authorization: bearer synthetic-handoff-api-key"));
        const QJsonObject updatedRequestBody = requestBody(updatedRequestBytes);
        QCOMPARE(updatedRequestBody.value(QStringLiteral("model")).toString(),
                 QStringLiteral("synthetic-handoff-model"));
        QCOMPARE(updatedRequestBody.value(QStringLiteral("temperature")).toDouble(), 0.7);
        QCOMPARE(updatedRequestBody.value(QStringLiteral("max_tokens")).toInt(), 1536);
        QVERIFY(!updatedRequestBody.contains(QStringLiteral("tools")));
        QCOMPARE(
            updatedRequestBody.value(QStringLiteral("thinking")).toObject().value(QStringLiteral("type")).toString(),
            QStringLiteral("enabled"));

        QVERIFY(auditSpy.count() >= 5);
        const AgentActionLogEntry firstAudit = qvariant_cast<AgentActionLogEntry>(auditSpy.first().at(0));
        QCOMPARE(firstAudit.action, QStringLiteral("open_agent_settings_dialog"));
        QVERIFY(QJsonDocument::fromJson(firstAudit.params.toUtf8()).object().isEmpty());
        for (const QString& sensitive :
             {QStringLiteral("synthetic-seed-key"), QStringLiteral("synthetic-handoff-api-key"),
              QStringLiteral("synthetic-handoff-model")}) {
            QVERIFY(!QJsonDocument(opened).toJson(QJsonDocument::Compact).contains(sensitive.toUtf8()));
            for (const QList<QVariant>& row : auditSpy) {
                const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
                const QByteArray auditBytes = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
                QVERIFY(!auditBytes.contains(sensitive.toUtf8()));
            }
        }

        QAction* agentSettingsAction = window.findChild<QAction*>(QStringLiteral("AgentSettingsMenuAction"));
        QVERIFY(agentSettingsAction);
        const QJsonObject modelessResult = controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
        QCOMPARE(modelessResult.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        AgentSettingsDialog* modeless = window.findChild<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog"));
        QVERIFY(modeless);
        QVERIFY(modeless->isVisible());
        QVERIFY(!modeless->isModal());
        QTimer::singleShot(0, &window, [&]() {
            QTimer::singleShot(0, &window, [&]() {
                const QList<AgentSettingsDialog*> dialogs =
                    window.findChildren<AgentSettingsDialog*>(QStringLiteral("AgentSettingsDialog"));
                bool reusedModal = false;
                QDialog* visibleModal = nullptr;
                for (AgentSettingsDialog* candidate : dialogs) {
                    if (candidate->isVisible() && candidate->isModal())
                        visibleModal = candidate;
                }
                if (dialogs.size() == 2 && visibleModal && modeless->isVisible() && !modeless->isModal()) {
                    const QJsonObject result =
                        controller.handleToolCall(QStringLiteral("open_agent_settings_dialog"), {});
                    reusedModal = result.value(QStringLiteral("dialogAction")).toString() == QStringLiteral("reused") &&
                                  visibleModal->isModal();
                }
                if (!reusedModal)
                    QTest::qFail("Agent tool did not reuse the visible menu modal settings dialog", __FILE__, __LINE__);
                visibleModal->reject();
            });
            agentSettingsAction->trigger();
        });
        QCoreApplication::processEvents();
        QVERIFY(modeless->isVisible());
        modeless->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

private:
    bool captureClientRequest(OpenAILLMClient* client, const QList<ToolDefinition>& tools, QByteArray* request) {
        if (!client || !request)
            return false;

        const QSignalBlocker signalBlocker(client);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(client, &OpenAILLMClient::responseReceived, &loop, &QEventLoop::quit);
        connect(client, &OpenAILLMClient::errorOccurred, &loop, &QEventLoop::quit);
        connect(&m_server, &QTcpServer::newConnection, &loop, [this, request, &loop]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [socket, request, &loop]() {
                    request->append(socket->readAll());
                    const int headerEnd = request->indexOf("\r\n\r\n");
                    if (headerEnd < 0)
                        return;
                    int contentLength = 0;
                    for (const QByteArray& header : request->left(headerEnd).split('\n')) {
                        if (header.toLower().startsWith("content-length:"))
                            contentLength = header.mid(header.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (request->size() < headerEnd + 4 + contentLength)
                        return;
                    const QByteArray body = R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                  QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                    QTimer::singleShot(100, &loop, &QEventLoop::quit);
                });
            }
        });
        AgentConversation conversation;
        client->sendRequest(conversation, tools);
        timeout.start(5000);
        loop.exec();
        return !request->isEmpty();
    }

    static QJsonObject requestBody(const QByteArray& request) {
        const int bodyStart = request.indexOf("\r\n\r\n");
        if (bodyStart < 0)
            return {};
        return QJsonDocument::fromJson(request.mid(bodyStart + 4)).object();
    }

    QJsonObject initialSettings() const {
        return QJsonObject{
            {QStringLiteral("endpoint"), m_fixtureEndpoint},
            {QStringLiteral("apiKey"), QStringLiteral("synthetic-seed-key")},
            {QStringLiteral("model"), QStringLiteral("synthetic-seed-model")},
            {QStringLiteral("temperature"), 0.25},
            {QStringLiteral("maxTokens"), 768},
            {QStringLiteral("permissionLevel"), static_cast<int>(AgentController::PermissionLevel::Observer)},
            {QStringLiteral("systemPrompt"), QStringLiteral("synthetic seed prompt")},
            {QStringLiteral("toolsEnabled"), true},
            {QStringLiteral("visionEnabled"), true},
            {QStringLiteral("thinkingEnabled"), false}};
    }

    QTemporaryDir* m_appDataDir = nullptr;
    QTcpServer m_server;
    QString m_fixtureEndpoint;
};

QTEST_MAIN(TestAgentSettingsHandoff)
#include "test_agent_settings_handoff.moc"
