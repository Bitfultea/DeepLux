#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QNetworkReply>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/SamBackendClient.h>
#include <core/agent/ToolSchema.h>
#include <core/common/UserManager.h>
#include <core/manager/ConfigManager.h>
#include <core/model/Annotation.h>
#include <thread>
#include <ui/dialogs/SamAnnotatorDialog.h>
#include <ui/display/DisplayManager.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AnnotationOverlayWidget.h>
#include <ui/widgets/HImageWidget.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

class AgentControllerStateRestore {
public:
    explicit AgentControllerStateRestore(AgentController& controller)
        : m_controller(controller), m_permission(controller.permissionLevel()) {}
    ~AgentControllerStateRestore() {
        m_controller.setPermissionLevel(m_permission);
    }

private:
    AgentController& m_controller;
    AgentController::PermissionLevel m_permission;
};

class TestAgentSamAnnotatorHandoff : public QObject {
    Q_OBJECT

private slots:
    void init() {
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    }

    void initTestCase() {
        m_appDataDir = new QTemporaryDir();
        QVERIFY(m_appDataDir->isValid());
        m_modelEnvWasSet = qEnvironmentVariableIsSet("DEEPLUX_SAM_MODEL");
        m_modelEnv = qgetenv("DEEPLUX_SAM_MODEL");
        m_pythonEnvWasSet = qEnvironmentVariableIsSet("SAM_SERVER_PYTHON");
        m_pythonEnv = qgetenv("SAM_SERVER_PYTHON");
        m_apiKeyEnvWasSet = qEnvironmentVariableIsSet("OPENAI_API_KEY");
        m_apiKeyEnv = qgetenv("OPENAI_API_KEY");
        qunsetenv("DEEPLUX_SAM_MODEL");
        qunsetenv("SAM_SERVER_PYTHON");
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        ToolSchema::instance().registerDefaultTools();
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
        if (m_modelEnvWasSet)
            qputenv("DEEPLUX_SAM_MODEL", m_modelEnv);
        else
            qunsetenv("DEEPLUX_SAM_MODEL");
        if (m_pythonEnvWasSet)
            qputenv("SAM_SERVER_PYTHON", m_pythonEnv);
        else
            qunsetenv("SAM_SERVER_PYTHON");
        if (m_apiKeyEnvWasSet)
            qputenv("OPENAI_API_KEY", m_apiKeyEnv);
        else
            qunsetenv("OPENAI_API_KEY");
        delete m_appDataDir;
        m_appDataDir = nullptr;
    }

    void testRequiresGuiAndRejectsMissingImageAndBadCalls() {
        AgentController& controller = AgentController::instance();
        const QJsonObject noGui = controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {});
        QCOMPARE(noGui.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
        const QJsonObject pickerNoGui = controller.handleToolCall(QStringLiteral("open_sam_image_picker_for_user"), {});
        QCOMPARE(pickerNoGui.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));

        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        const QJsonObject invalid = controller.executeDataSourceViewAction(
            QStringLiteral("open_sam_annotator_dialog"), QJsonObject{{QStringLiteral("path"), "private"}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(window.findChildren<SamAnnotatorDialog*>().isEmpty());
        const QJsonObject invalidPicker = controller.executeDataSourceViewAction(
            QStringLiteral("open_sam_image_picker_for_user"), QJsonObject{{QStringLiteral("path"), "private"}});
        QCOMPARE(invalidPicker.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(window.findChildren<SamAnnotatorDialog*>().isEmpty());

        QJsonObject wrongThread;
        std::thread worker([&]() {
            wrongThread = controller.executeDataSourceViewAction(QStringLiteral("open_sam_annotator_dialog"), {});
        });
        worker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(window.findChildren<SamAnnotatorDialog*>().isEmpty());
        std::thread pickerWorker([&]() {
            wrongThread = controller.executeDataSourceViewAction(QStringLiteral("open_sam_image_picker_for_user"), {});
        });
        pickerWorker.join();
        QCOMPARE(wrongThread.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(window.findChildren<SamAnnotatorDialog*>().isEmpty());

        QTemporaryDir fallbackCaptureDir;
        QVERIFY(fallbackCaptureDir.isValid());
        const QString captureDir = qEnvironmentVariable("DEEPLUX_AGENT_CAPTURE_DIR").isEmpty()
                                       ? fallbackCaptureDir.path()
                                       : qEnvironmentVariable("DEEPLUX_AGENT_CAPTURE_DIR");
        QVERIFY(QDir().mkpath(captureDir));
        const QString beforePath = QDir(captureDir).filePath(QStringLiteral("sam-no-image-before.png"));
        QVERIFY(window.grab().save(beforePath));
        QVERIFY(QFileInfo(beforePath).size() > 0);
        const QJsonObject noImage = controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {});
        QCOMPARE(noImage.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_IMAGE"));
        QVERIFY(window.findChildren<SamAnnotatorDialog*>().isEmpty());
        const QString afterPath = QDir(captureDir).filePath(QStringLiteral("sam-no-image-after.png"));
        QVERIFY(window.grab().save(afterPath));
        QVERIFY(QFileInfo(afterPath).size() > 0);
    }

    void testImagePickerHandoffOpensEmptyWindowAndCancelPreservesState() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();

        const bool configInitializedBefore = ConfigManager::instance().isInitialized();
        const QByteArray modelBefore = qgetenv("DEEPLUX_SAM_MODEL");
        const QByteArray pythonBefore = qgetenv("SAM_SERVER_PYTHON");
        const auto appDataSnapshot = [this]() {
            QMap<QString, QByteArray> snapshot;
            QDirIterator iterator(m_appDataDir->path(),
                                  QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                                  QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                const QString filePath = iterator.next();
                QFile file(filePath);
                if (!file.open(QIODevice::ReadOnly)) {
                    snapshot.insert(QDir(m_appDataDir->path()).relativeFilePath(filePath), QByteArray("<unreadable>"));
                    continue;
                }
                snapshot.insert(QDir(m_appDataDir->path()).relativeFilePath(filePath), file.readAll());
            }
            return snapshot;
        };
        const QMap<QString, QByteArray> appDataBefore = appDataSnapshot();
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject result = controller.handleToolCall(QStringLiteral("open_sam_image_picker_for_user"), {});
        const QMap<QString, QByteArray> appDataAfterOpen = appDataSnapshot();
        auto isOnlyLogAppend = [](const QMap<QString, QByteArray>& before, const QMap<QString, QByteArray>& after) {
            for (auto it = before.cbegin(); it != before.cend(); ++it) {
                if (!after.contains(it.key()))
                    return false;
                if (after.value(it.key()) != it.value() &&
                    (!it.key().startsWith(QStringLiteral("logs/")) || !after.value(it.key()).startsWith(it.value())))
                    return false;
            }
            for (auto it = after.cbegin(); it != after.cend(); ++it) {
                if (!before.contains(it.key()) && !it.key().startsWith(QStringLiteral("logs/")))
                    return false;
            }
            return true;
        };
        QVERIFY(isOnlyLogAppend(appDataBefore, appDataAfterOpen));
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        QCOMPARE(result.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QVERIFY(result.value(QStringLiteral("userActionRequired")).toBool());
        QVERIFY(!result.value(QStringLiteral("imageAvailable")).toBool());
        QCOMPARE(result.size(), 4);

        auto* dialog = window.findChild<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog"));
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        auto* openImageButton = dialog->findChild<QPushButton*>(QStringLiteral("SamOpenImageButton"));
        QVERIFY(openImageButton);
        QVERIFY(openImageButton->isVisible());
        QVERIFY(openImageButton->isEnabled());
        QVERIFY(dialog->imageSnapshot().isNull());
        QVERIFY(dialog->session().annotations.isEmpty());
        QCOMPARE(dialog->backendClient()->state(), SamBackendClient::State::NotStarted);
        QVERIFY(dialog->findChildren<QProcess*>().isEmpty());
        QCOMPARE(ConfigManager::instance().isInitialized(), configInitializedBefore);
        QCOMPARE(qgetenv("DEEPLUX_SAM_MODEL"), modelBefore);
        QCOMPARE(qgetenv("SAM_SERVER_PYTHON"), pythonBefore);

        const QJsonObject sessionBefore = dialog->session().toJson();
        const QJsonObject promptBefore = dialog->agentPromptStateSnapshot();
        AnnotationSession detachedSession = dialog->session();
        AnnotationObject detachedObject;
        detachedObject.id = QStringLiteral("slice52-detached-object");
        detachedObject.label = QStringLiteral("must-not-enter-detached-session");
        detachedSession.annotations.append(detachedObject);
        QString detachedError;
        QVERIFY(
            !dialog->openAgentAnnotationSessionSnapshot(detachedSession, QImage(), false, false, {}, &detachedError));
        QVERIFY(!detachedError.isEmpty());
        QCOMPARE(dialog->session().toJson(), sessionBefore);
        const QJsonObject reused = controller.handleToolCall(QStringLiteral("open_sam_image_picker_for_user"), {});
        QCOMPARE(reused.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QCOMPARE(dialog->session().toJson(), sessionBefore);
        QCOMPARE(dialog->agentPromptStateSnapshot(), promptBefore);
        const QMap<QString, QByteArray> appDataBeforeCancel = appDataSnapshot();
        QVERIFY(isOnlyLogAppend(appDataAfterOpen, appDataBeforeCancel));

        QTimer::singleShot(0, []() {
            QTimer::singleShot(100, []() {
                if (QWidget* modal = QApplication::activeModalWidget())
                    modal->close();
            });
        });
        openImageButton->click();
        QCOMPARE(dialog->session().toJson(), sessionBefore);
        QCOMPARE(dialog->agentPromptStateSnapshot(), promptBefore);
        QVERIFY(dialog->imageSnapshot().isNull());
        QCOMPARE(dialog->backendClient()->state(), SamBackendClient::State::NotStarted);
        QCOMPARE(ConfigManager::instance().isInitialized(), configInitializedBefore);
        QCOMPARE(qgetenv("DEEPLUX_SAM_MODEL"), modelBefore);
        QCOMPARE(qgetenv("SAM_SERVER_PYTHON"), pythonBefore);
        QCOMPARE(appDataSnapshot(), appDataBeforeCancel);

        QVERIFY(!auditSpy.isEmpty());
        const AgentActionLogEntry audit = qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0));
        QVERIFY(!audit.params.contains(QStringLiteral("/tmp/")));
        QVERIFY(!audit.result.contains(QStringLiteral("/tmp/")));
        QVERIFY(!audit.params.contains(QStringLiteral("imagePath")));
    }

    void testImagePickerHandoffReusesBoundImageAndSession() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto* display = window.findChild<DisplayManager*>();
        QVERIFY(display);
        const QVector<ViewportWidget*> viewports = display->allViewports();
        QVERIFY(!viewports.isEmpty());
        ViewportWidget* viewport = viewports.first();
        QImage image(24, 18, QImage::Format_RGB32);
        image.fill(QColor(0x29, 0x71, 0xA3));
        viewport->displayImage(image);
        QCOMPARE(controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("opened"));

        auto* dialog = window.findChild<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog"));
        QVERIFY(dialog);
        AnnotationSession session = dialog->session();
        session.imagePath = QStringLiteral("/private/slice52-source.png");
        session.imageDigest = AnnotationSession::computeImageDigest(image);
        session.sourceCoordinateProvenance.sourceImageId = QStringLiteral("slice59-parent-image");
        session.sourceCoordinateProvenance.sourceSize = QSize(640, 480);
        QVERIFY(session.sourceCoordinateProvenance.composeLocalToSource(
            ImageCoordinateProvenance::Matrix3{{2.0, 0.0, 10.0, 0.0, 3.0, 5.0, 0.0, 0.0, 1.0}}));
        AnnotationObject object;
        object.id = QStringLiteral("slice52-object");
        object.label = QStringLiteral("private-label");
        object.bbox = QRectF(2, 3, 8, 7);
        object.polygon = {QPointF(2, 3), QPointF(10, 3), QPointF(10, 10), QPointF(2, 10)};
        session.annotations.append(object);
        QString error;
        QVERIFY2(dialog->openAgentAnnotationSessionSnapshot(session, image, true, false, session.imagePath, &error),
                 qPrintable(error));
        const QJsonObject restoredObject =
            dialog->listAgentAnnotationObjects(0, 1).value(QStringLiteral("objects")).toArray().first().toObject();
        QCOMPARE(restoredObject.value(QStringLiteral("sourceImageId")).toString(),
                 QStringLiteral("slice59-parent-image"));
        const QJsonObject restoredSourceBbox = restoredObject.value(QStringLiteral("sourceBbox")).toObject();
        QCOMPARE(restoredSourceBbox.value(QStringLiteral("x")).toDouble(), 14.0);
        QCOMPARE(restoredSourceBbox.value(QStringLiteral("y")).toDouble(), 14.0);
        QCOMPARE(restoredSourceBbox.value(QStringLiteral("width")).toDouble(), 16.0);
        QCOMPARE(restoredSourceBbox.value(QStringLiteral("height")).toDouble(), 21.0);
        const QJsonObject sessionBefore = dialog->session().toJson();
        const QJsonObject promptBefore = dialog->agentPromptStateSnapshot();
        const int undoIndexBefore = dialog->undoStackIndex();
        const int undoCountBefore = dialog->undoStackCount();
        const QImage imageBefore = dialog->imageSnapshot();
        const SamBackendClient::State backendBefore = dialog->backendClient()->state();

        const QJsonObject result = controller.handleToolCall(QStringLiteral("open_sam_image_picker_for_user"), {});
        QCOMPARE(result.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QVERIFY(result.value(QStringLiteral("imageAvailable")).toBool());
        QCOMPARE(window.findChildren<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog")).size(), 1);
        auto* openImageButton = dialog->findChild<QPushButton*>(QStringLiteral("SamOpenImageButton"));
        QVERIFY(openImageButton);
        QVERIFY(openImageButton->isVisible());
        QCOMPARE(dialog->session().toJson(), sessionBefore);
        QCOMPARE(dialog->agentPromptStateSnapshot(), promptBefore);
        QCOMPARE(dialog->undoStackIndex(), undoIndexBefore);
        QCOMPARE(dialog->undoStackCount(), undoCountBefore);
        QCOMPARE(dialog->imageSnapshot(), imageBefore);
        QCOMPARE(dialog->backendClient()->state(), backendBefore);
        QCOMPARE(viewport->imageWidget()->currentImage(), image);
    }

    void testOpenAnnotationSessionRechecksFilePermissionAtConfirmation() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto* display = window.findChild<DisplayManager*>();
        QVERIFY(display);
        const QVector<ViewportWidget*> viewports = display->allViewports();
        QVERIFY(!viewports.isEmpty());
        QImage image(24, 18, QImage::Format_RGB32);
        image.fill(QColor(0x29, 0x71, 0xA3));
        const QString imagePath = m_appDataDir->filePath(QStringLiteral("permission-source.png"));
        QVERIFY(image.save(imagePath, "PNG"));
        viewports.first()->displayImage(image);
        QCOMPARE(controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("opened"));
        auto* dialog = window.findChild<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog"));
        QVERIFY(dialog);

        AnnotationSession active = dialog->session();
        active.imagePath = imagePath;
        active.imageWidth = image.width();
        active.imageHeight = image.height();
        active.imageDigest = AnnotationSession::computeImageDigest(image);
        AnnotationObject activeObject;
        activeObject.id = QStringLiteral("active-permission-object");
        activeObject.label = QStringLiteral("keep-active");
        activeObject.bbox = QRectF(1, 2, 4, 5);
        active.annotations = {activeObject};
        QString error;
        QVERIFY2(dialog->openAgentAnnotationSessionSnapshot(active, image, true, false, imagePath, &error),
                 qPrintable(error));

        AnnotationSession candidate = active;
        candidate.annotations[0].id = QStringLiteral("candidate-permission-object");
        candidate.annotations[0].label = QStringLiteral("must-not-open");
        const QString sessionPath = m_appDataDir->filePath(QStringLiteral("candidate.deeplux-anno.json"));
        QVERIFY(candidate.save(sessionPath, &error));
        QFile candidateFile(sessionPath);
        QVERIFY(candidateFile.open(QIODevice::ReadOnly));
        const QByteArray candidateBytes = candidateFile.readAll();
        candidateFile.close();

        const QJsonObject activeBefore = dialog->session().toJson();
        const QJsonObject promptBefore = dialog->agentPromptStateSnapshot();
        const int undoIndexBefore = dialog->undoStackIndex();
        const int undoCountBefore = dialog->undoStackCount();
        const QImage imageBefore = dialog->imageSnapshot();
        QSignalSpy responseSpy(&controller, &AgentController::llmResponseReceived);
        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        QSignalSpy toolSpy(controller.actor(), &AgentActor::toolExecuted);

        const QJsonObject pending = controller.handleToolCall(QStringLiteral("open_active_annotation_session"),
                                                              {{QStringLiteral("sessionPath"), sessionPath}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString confirmationId = controller.pendingConfirmationId();
        QVERIFY(!confirmationId.isEmpty());
        QCOMPARE(dialog->session().toJson(), activeBefore);
        QCOMPARE(dialog->agentPromptStateSnapshot(), promptBefore);
        QCOMPARE(dialog->undoStackIndex(), undoIndexBefore);
        QCOMPARE(dialog->undoStackCount(), undoCountBefore);

        QVERIFY(UserManager::instance().login(QStringLiteral("guest-session"), QString()));
        QVERIFY(!UserManager::instance().hasPermission(QStringLiteral("OpenFile")));
        QVERIFY(controller.confirmPendingTools(confirmationId));
        QCOMPARE(dialog->session().toJson(), activeBefore);
        QCOMPARE(dialog->agentPromptStateSnapshot(), promptBefore);
        QCOMPARE(dialog->undoStackIndex(), undoIndexBefore);
        QCOMPARE(dialog->undoStackCount(), undoCountBefore);
        QCOMPARE(dialog->imageSnapshot(), imageBefore);
        QCOMPARE(responseSpy.size(), 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.size(), 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));
        QCOMPARE(toolSpy.count(), 0);
        QVERIFY(candidateFile.open(QIODevice::ReadOnly));
        QCOMPARE(candidateFile.readAll(), candidateBytes);
        candidateFile.close();

        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        const QJsonObject operatorPending = controller.handleToolCall(QStringLiteral("open_active_annotation_session"),
                                                                      {{QStringLiteral("sessionPath"), sessionPath}});
        QCOMPARE(operatorPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        const QJsonArray openedAnnotations = dialog->session().toJson().value(QStringLiteral("annotations")).toArray();
        QCOMPARE(openedAnnotations.size(), 1);
        QCOMPARE(openedAnnotations.first().toObject().value(QStringLiteral("label")).toString(),
                 QStringLiteral("must-not-open"));

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject editSessionBefore = dialog->session().toJson();
        const QStringList categoriesBefore = dialog->categories();
        const int editUndoIndexBefore = dialog->undoStackIndex();
        const int editUndoCountBefore = dialog->undoStackCount();
        const int responsesBeforeEdit = responseSpy.count();
        const int auditsBeforeEdit = auditSpy.count();
        const QJsonObject editPending =
            controller.handleToolCall(QStringLiteral("edit_active_annotation_categories"),
                                      {{QStringLiteral("operation"), QStringLiteral("add_category")},
                                       {QStringLiteral("category"), QStringLiteral("unauthorized-category")}});
        QCOMPARE(editPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString editConfirmationId = controller.pendingConfirmationId();
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(controller.confirmPendingTools(editConfirmationId));
        QCOMPARE(dialog->session().toJson(), editSessionBefore);
        QCOMPARE(dialog->categories(), categoriesBefore);
        QCOMPARE(dialog->undoStackIndex(), editUndoIndexBefore);
        QCOMPARE(dialog->undoStackCount(), editUndoCountBefore);
        QCOMPARE(responseSpy.count(), responsesBeforeEdit + 1);
        QVERIFY(responseSpy.last().at(0).toString().contains(QStringLiteral("未执行任何操作")));
        QCOMPARE(auditSpy.count(), auditsBeforeEdit + 1);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("denied"));

        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
        const QJsonObject undoCommand =
            dialog->applyAgentCategoryEdit({{QStringLiteral("operation"), QStringLiteral("add_category")},
                                            {QStringLiteral("category"), QStringLiteral("undo-redo-sentinel")}});
        QCOMPARE(undoCommand.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
        dialog->undoButton()->click();
        QVERIFY(dialog->redoButton()->isEnabled());
        dialog->positivePointButton()->click();
        QTest::mouseClick(dialog->overlayWidget(), Qt::LeftButton, Qt::NoModifier,
                          dialog->overlayWidget()->rect().center());
        QVERIFY(dialog->agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
        const QJsonObject sessionBeforeCancel = dialog->session().toJson();
        const QJsonObject promptBeforeCancel = dialog->agentPromptStateSnapshot();
        const int cancelUndoIndex = dialog->undoStackIndex();
        const int cancelUndoCount = dialog->undoStackCount();
        const bool redoEnabledBeforeCancel = dialog->redoButton()->isEnabled();
        UserManager::instance().logout();
        const int responsesBeforeCancel = responseSpy.count();
        const QJsonObject cancelPending =
            controller.handleToolCall(QStringLiteral("cancel_active_annotation_prompt"), {});
        QCOMPARE(cancelPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString cancelConfirmationId = controller.pendingConfirmationId();
        QVERIFY(controller.confirmPendingTools(cancelConfirmationId));
        QVERIFY(!dialog->agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
        QCOMPARE(dialog->session().toJson(), sessionBeforeCancel);
        QCOMPARE(dialog->undoStackIndex(), cancelUndoIndex);
        QCOMPARE(dialog->undoStackCount(), cancelUndoCount);
        QCOMPARE(dialog->redoButton()->isEnabled(), redoEnabledBeforeCancel);
        QVERIFY(promptBeforeCancel != dialog->agentPromptStateSnapshot());
        QVERIFY(responseSpy.count() >= responsesBeforeCancel);
        QCOMPARE(qvariant_cast<AgentActionLogEntry>(auditSpy.last().at(0)).result, QStringLiteral("cancelled"));
        QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));

        const QString savePath = m_appDataDir->filePath(QStringLiteral("saved-by-agent.deeplux-anno.json"));
        const QJsonObject savePending = controller.handleToolCall(QStringLiteral("save_active_annotation_session"),
                                                                  {{QStringLiteral("outputPath"), savePath}});
        QCOMPARE(savePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString saveConfirmationId = controller.pendingConfirmationId();
        const QJsonObject openSessionBeforeDeniedSave = dialog->session().toJson();
        QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
        QVERIFY(controller.confirmPendingTools(saveConfirmationId));
        QVERIFY(!QFileInfo::exists(savePath));
        QCOMPARE(dialog->session().toJson(), openSessionBeforeDeniedSave);
    }

    void testSafeOpenReusePreservesBindingAndDoesNotPrepareBackend() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto* display = window.findChild<DisplayManager*>();
        QVERIFY(display);
        const QVector<ViewportWidget*> viewports = display->allViewports();
        QVERIFY(!viewports.isEmpty());
        ViewportWidget* firstViewport = viewports.first();
        QImage firstImage(64, 48, QImage::Format_RGB32);
        firstImage.fill(QColor(0x45, 0x12, 0xAB));
        firstViewport->displayImage(firstImage);
        QVERIFY(firstViewport->imageWidget()->hasImage());

        const QByteArray modelEnvBefore = qgetenv("DEEPLUX_SAM_MODEL");
        const QByteArray pythonEnvBefore = qgetenv("SAM_SERVER_PYTHON");
        const QByteArray apiKeyMarker = QByteArrayLiteral("SLICE45_PRIVATE_API_KEY_SENTINEL");
        qputenv("OPENAI_API_KEY", apiKeyMarker);
        ConfigManager& config = ConfigManager::instance();
        const bool configInitializedBefore = config.isInitialized();
        const QString configPath = config.configFilePath();
        const bool configExistedBefore = QFileInfo::exists(configPath);
        QVERIFY(!configInitializedBefore);
        QVERIFY(!configExistedBefore);
        QByteArray configBytesBefore;
        if (configExistedBefore) {
            QFile file(configPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            configBytesBefore = file.readAll();
        }

        QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
        for (const AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            QCOMPARE(controller.evaluateToolGate(QStringLiteral("open_sam_annotator_dialog")),
                     AgentController::ToolGate::Allow);
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);

        const QJsonObject firstResult = controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {});
        QCOMPARE(firstResult.value(QStringLiteral("status")).toString(), QStringLiteral("opened"));
        QCOMPARE(firstResult.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        QVERIFY(firstResult.value(QStringLiteral("userActionRequired")).toBool());
        QVERIFY(firstResult.value(QStringLiteral("imageAvailable")).toBool());
        QCOMPARE(firstResult.size(), 4);
        auto* dialog = window.findChild<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog"));
        QVERIFY(dialog);
        QVERIFY(dialog->isVisible());
        QCOMPARE(dialog->imageSnapshot(), firstImage);
        QCOMPARE(dialog->backendImagePathForTest(), QString());
        SamBackendClient* client = dialog->backendClient();
        QVERIFY(client);
        QCOMPARE(client->state(), SamBackendClient::State::NotStarted);
        QVERIFY(dialog->findChildren<QProcess*>().isEmpty());

        const QString annotationMarker = QStringLiteral("SLICE45_PRIVATE_ANNOTATION_MARKER");
        AnnotationSession syntheticSession = dialog->session();
        syntheticSession.imagePath = QStringLiteral("/tmp/SLICE45_PRIVATE_IMAGE_PATH.png");
        syntheticSession.modelName = QStringLiteral("SLICE45_PRIVATE_SESSION_MODEL");
        AnnotationObject syntheticObject;
        syntheticObject.id = QStringLiteral("SLICE45_PRIVATE_OBJECT_ID");
        syntheticObject.label = annotationMarker;
        syntheticObject.bbox = QRectF(1, 2, 10, 8);
        syntheticObject.polygon = {QPointF(1, 2), QPointF(11, 2), QPointF(11, 10), QPointF(1, 10)};
        syntheticObject.maskRle = QStringLiteral("SLICE45_PRIVATE_MASK_MARKER");
        syntheticSession.annotations.append(syntheticObject);
        QString sessionError;
        QVERIFY2(dialog->openAgentAnnotationSessionSnapshot(syntheticSession, firstImage, true, false,
                                                            syntheticSession.imagePath, &sessionError),
                 qPrintable(sessionError));
        const QString modelPathMarker = QStringLiteral("/tmp/SLICE45_PRIVATE_MODEL_PATH.pth");
        client->setModelPath(modelPathMarker);
        const QString backendPathBefore = dialog->backendImagePathForTest();
        QVERIFY(!backendPathBefore.isEmpty());
        const QJsonObject sessionBefore = dialog->session().toJson();
        const QJsonObject promptBefore = dialog->agentPromptStateSnapshot();

        const QString secondId = display->createViewport(QStringLiteral("Different image"));
        ViewportWidget* secondViewport = display->viewport(secondId);
        QVERIFY(secondViewport);
        QImage secondImage(80, 60, QImage::Format_RGB32);
        secondImage.fill(Qt::green);
        secondViewport->displayImage(secondImage);
        secondViewport->imageWidget()->setFocus(Qt::OtherFocusReason);
        QCoreApplication::processEvents();

        const QJsonObject reused = controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {});
        QCOMPARE(reused.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QCOMPARE(reused.keys(), firstResult.keys());
        QCOMPARE(reused.value(QStringLiteral("status")), firstResult.value(QStringLiteral("status")));
        QCOMPARE(reused.value(QStringLiteral("userActionRequired")),
                 firstResult.value(QStringLiteral("userActionRequired")));
        QCOMPARE(reused.value(QStringLiteral("imageAvailable")), firstResult.value(QStringLiteral("imageAvailable")));
        QCOMPARE(window.findChildren<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog")).size(), 1);
        QCOMPARE(dialog->imageSnapshot(), firstImage);
        QCOMPARE(dialog->session().toJson(), sessionBefore);
        QCOMPARE(dialog->agentPromptStateSnapshot(), promptBefore);
        QCOMPARE(client->state(), SamBackendClient::State::NotStarted);
        QCOMPARE(dialog->backendImagePathForTest(), backendPathBefore);
        QVERIFY(dialog->findChildren<QProcess*>().isEmpty());
        QCOMPARE(config.isInitialized(), configInitializedBefore);
        QCOMPARE(QFileInfo::exists(configPath), configExistedBefore);
        if (configExistedBefore) {
            QFile file(configPath);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), configBytesBefore);
        }
        QCOMPARE(qgetenv("DEEPLUX_SAM_MODEL"), modelEnvBefore);
        QCOMPARE(qgetenv("SAM_SERVER_PYTHON"), pythonEnvBefore);
        QCOMPARE(qgetenv("OPENAI_API_KEY"), apiKeyMarker);
        QCOMPARE(config.isInitialized(), false);
        QVERIFY(!QFileInfo::exists(configPath));

        QVERIFY(auditSpy.count() >= 2);
        const QStringList secretMarkers = {QStringLiteral("private"),
                                           QStringLiteral("4512AB"),
                                           annotationMarker,
                                           QStringLiteral("SLICE45_PRIVATE_OBJECT_ID"),
                                           QStringLiteral("SLICE45_PRIVATE_MASK_MARKER"),
                                           QStringLiteral("SLICE45_PRIVATE_SESSION_MODEL"),
                                           QStringLiteral("SLICE45_PRIVATE_MODEL_PATH"),
                                           QStringLiteral("SLICE45_PRIVATE_API_KEY_SENTINEL")};
        for (const QList<QVariant>& row : auditSpy) {
            const AgentActionLogEntry entry = qvariant_cast<AgentActionLogEntry>(row.first());
            const QByteArray audit = (entry.actor + entry.action + entry.params + entry.result).toUtf8();
            for (const QString& marker : secretMarkers)
                QVERIFY2(!audit.contains(marker.toUtf8()), qPrintable(marker));
        }
        for (const QJsonObject& result : {firstResult, reused}) {
            const QByteArray response = QJsonDocument(result).toJson(QJsonDocument::Compact);
            for (const QString& marker : secretMarkers)
                QVERIFY2(!response.contains(marker.toUtf8()), qPrintable(marker));
        }
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void testExistingDialogLocalImageWinsWhenViewportsAreEmpty() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto* display = window.findChild<DisplayManager*>();
        QVERIFY(display);

        const QJsonObject setup = controller.handleToolCall(QStringLiteral("open_sam_environment_setup"), {});
        QCOMPARE(setup.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        auto* dialog = window.findChild<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog"));
        QVERIFY(dialog);
        QImage localImage(37, 29, QImage::Format_RGB32);
        localImage.fill(QColor(0xB2, 0x6C, 0xD4));
        dialog->setImageSnapshot(localImage, QStringLiteral("/tmp/SLICE45_PRIVATE_IMAGE_PATH.png"));
        const QJsonObject sessionBefore = dialog->session().toJson();
        const QString backendPathBefore = dialog->backendImagePathForTest();
        QVERIFY(!backendPathBefore.isEmpty());
        dialog->backendClient()->setServerUrl(QStringLiteral("http://127.0.0.1:1"));
        dialog->backendClient()->healthCheck();
        const SamBackendClient::State stateBefore = dialog->backendClient()->state();
        QCOMPARE(stateBefore, SamBackendClient::State::LoadingModel);
        const int pendingRepliesBefore = dialog->backendClient()->findChildren<QNetworkReply*>().size();
        QCOMPARE(pendingRepliesBefore, 1);
        for (ViewportWidget* viewport : display->allViewports()) {
            if (viewport && viewport->imageWidget())
                viewport->imageWidget()->clearImage();
        }

        const QJsonObject result = controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {});
        QCOMPARE(result.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("reused"));
        QVERIFY(result.value(QStringLiteral("imageAvailable")).toBool());
        QCOMPARE(dialog->imageSnapshot(), localImage);
        QCOMPARE(dialog->session().toJson(), sessionBefore);
        QCOMPARE(dialog->backendImagePathForTest(), backendPathBefore);
        QCOMPARE(dialog->backendClient()->state(), stateBefore);
        QCOMPARE(dialog->backendClient()->findChildren<QNetworkReply*>().size(), pendingRepliesBefore);
        QVERIFY(!QJsonDocument(result).toJson(QJsonDocument::Compact).contains("SLICE45_PRIVATE_IMAGE_PATH"));
        dialog->close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    void testCloseReopenAndOwnerDestruction() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        auto* window = new MainWindow();
        window->show();
        QCoreApplication::processEvents();
        ViewportWidget* viewport = window->findChild<DisplayManager*>()->allViewports().first();
        QImage image(32, 24, QImage::Format_RGB32);
        image.fill(Qt::blue);
        viewport->displayImage(image);

        QCOMPARE(controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {})
                     .value(QStringLiteral("dialogAction"))
                     .toString(),
                 QStringLiteral("opened"));
        SamAnnotatorDialog* first = window->findChild<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog"));
        QVERIFY(first);
        QPointer<SamAnnotatorDialog> firstGuard(first);
        first->close();
        const QJsonObject reopened = controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {});
        QCOMPARE(reopened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        SamAnnotatorDialog* second = nullptr;
        for (SamAnnotatorDialog* candidate : window->findChildren<SamAnnotatorDialog*>()) {
            if (candidate->isVisible())
                second = candidate;
        }
        QVERIFY(second);
        QVERIFY(second != first);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(firstGuard.isNull());
        QCOMPARE(window->findChildren<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog")).size(), 1);

        QPointer<SamAnnotatorDialog> secondGuard(second);
        delete window;
        QVERIFY(secondGuard.isNull());
    }

    void testToolbarCreatedDialogCanBeReopenedBeforeDeferredDelete() {
        AgentController& controller = AgentController::instance();
        AgentControllerStateRestore restore(controller);
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        ViewportWidget* viewport = window.findChild<DisplayManager*>()->allViewports().first();
        QImage image(42, 31, QImage::Format_RGB32);
        image.fill(Qt::yellow);
        viewport->displayImage(image);

        QVERIFY(QMetaObject::invokeMethod(&window, "onQuickAnnotate", Qt::DirectConnection));
        SamAnnotatorDialog* toolbarDialog = window.findChild<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog"));
        QVERIFY(toolbarDialog);
        QCOMPARE(toolbarDialog->imageSnapshot(), image);
        QPointer<SamAnnotatorDialog> oldGuard(toolbarDialog);
        toolbarDialog->close();

        const QJsonObject reopened = controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {});
        QCOMPARE(reopened.value(QStringLiteral("dialogAction")).toString(), QStringLiteral("opened"));
        SamAnnotatorDialog* agentDialog = nullptr;
        for (SamAnnotatorDialog* candidate : window.findChildren<SamAnnotatorDialog*>()) {
            if (candidate->isVisible())
                agentDialog = candidate;
        }
        QVERIFY(agentDialog);
        QVERIFY(agentDialog != toolbarDialog);
        QCOMPARE(agentDialog->imageSnapshot(), image);
        QCOMPARE(agentDialog->backendImagePathForTest(), QString());

        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QVERIFY(oldGuard.isNull());
        QVERIFY(agentDialog->isVisible());
        QCOMPARE(window.findChildren<SamAnnotatorDialog*>(QStringLiteral("SamAnnotatorDialog")).size(), 1);
        QCOMPARE(controller.handleToolCall(QStringLiteral("open_sam_annotator_dialog"), {})
                     .value(QStringLiteral("dialogAction"))
                     .toString(),
                 QStringLiteral("reused"));
    }

private:
    QTemporaryDir* m_appDataDir = nullptr;
    QByteArray m_modelEnv;
    QByteArray m_pythonEnv;
    QByteArray m_apiKeyEnv;
    bool m_modelEnvWasSet = false;
    bool m_pythonEnvWasSet = false;
    bool m_apiKeyEnvWasSet = false;
};

QTEST_MAIN(TestAgentSamAnnotatorHandoff)

#include "test_agent_sam_annotator_handoff.moc"
