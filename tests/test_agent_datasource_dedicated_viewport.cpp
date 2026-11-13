#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/ToolSchema.h>
#include <core/common/UserManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/DataSource.h>
#include <core/model/Project.h>
#include <memory>
#include <thread>
#include <ui/views/MainWindow.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

class TestAgentDataSourceDedicatedViewport : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        m_appDataDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        ToolSchema::instance().registerDefaultTools();
        ProjectManager::instance().closeProject();
    }

    void testDedicatedViewportLoadsWithoutChangingPrimaryAndHonorsConfirmationSnapshots() {
        AgentController& controller = AgentController::instance();
        const QJsonObject requiresGui = controller.executeDataSourceViewAction(
            QStringLiteral("display_data_source_in_dedicated_viewport"),
            QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("source")}});
        QCOMPARE(requiresGui.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));

        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QString primaryPath = files.filePath(QStringLiteral("primary.png"));
        QImage primaryImage(20, 16, QImage::Format_RGB32);
        primaryImage.fill(QColor("#2563EB"));
        QVERIFY(primaryImage.save(primaryPath));

        const QString imagePath = files.filePath(QStringLiteral("cold.png"));
        QImage sourceImage(18, 12, QImage::Format_RGB32);
        sourceImage.fill(QColor("#16A34A"));
        QVERIFY(sourceImage.save(imagePath));

        const QString cloudPath = files.filePath(QStringLiteral("cold.ply"));
        QFile cloudFile(cloudPath);
        QVERIFY(cloudFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(cloudFile.write("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
                                "property float z\nend_header\n0 0 0\n1 0 0\n0 1 0\n") > 0);
        cloudFile.close();

        const QString brokenPath = files.filePath(QStringLiteral("broken.png"));
        QFile brokenFile(brokenPath);
        QVERIFY(brokenFile.open(QIODevice::WriteOnly));
        QVERIFY(brokenFile.write("not an image") > 0);
        brokenFile.close();

        const QString generationPath = files.filePath(QStringLiteral("generation.png"));
        QImage generationImage(8, 8, QImage::Format_RGB32);
        generationImage.fill(Qt::black);
        QVERIFY(generationImage.save(generationPath));
        const QString hiddenPath = files.filePath(QStringLiteral("hidden.png"));
        QVERIFY(generationImage.save(hiddenPath));

        MainWindow window;
        window.resize(1100, 720);
        window.show();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        QTRY_VERIFY_WITH_TIMEOUT(project->layoutInitialized(), 1000);
        auto addSource = [project](const QString& id, const QString& name, const QString& path, const QString& type) {
            DataSource source;
            source.id = id;
            source.name = name;
            source.filePath = path;
            source.type = type;
            project->addDataSource(source);
            return source;
        };
        const DataSource primarySource =
            addSource(QStringLiteral("primary"), QStringLiteral("Primary"), primaryPath, QStringLiteral("image"));
        const DataSource imageSource =
            addSource(QStringLiteral("cold_image"), QStringLiteral("Cold image"), imagePath, QStringLiteral("image"));
        const DataSource cloudSource = addSource(QStringLiteral("cold_cloud"), QStringLiteral("Cold cloud"), cloudPath,
                                                 QStringLiteral("pointcloud"));
        const DataSource brokenSource =
            addSource(QStringLiteral("broken"), QStringLiteral("Broken"), brokenPath, QStringLiteral("image"));

        AgentActor actor;
        const ToolDefinition definition =
            ToolSchema::instance().findTool(QStringLiteral("display_data_source_in_dedicated_viewport"));
        QVERIFY(!definition.name.isEmpty());
        QVERIFY(definition.dangerous);
        QCOMPARE(definition.params.size(), 1);
        QCOMPARE(definition.params.first().name, QStringLiteral("dataSourceId"));

        const QJsonObject primaryResult = actor.executeTool(
            QStringLiteral("display_data_source"), QJsonObject{{QStringLiteral("dataSourceId"), primarySource.id}});
        QCOMPARE(primaryResult.value(QStringLiteral("status")).toString(), QStringLiteral("displayed"));
        const QString primaryViewportId = primaryResult.value(QStringLiteral("viewportId")).toString();
        QVERIFY(!primaryViewportId.isEmpty());
        auto findViewport = [&window](const QString& id) -> ViewportWidget* {
            for (ViewportWidget* viewport : window.findChildren<ViewportWidget*>())
                if (viewport && viewport->viewportId() == id)
                    return viewport;
            return nullptr;
        };
        ViewportWidget* primaryViewport = findViewport(primaryViewportId);
        QVERIFY(primaryViewport);
        QCOMPARE(primaryViewport->currentImage().pixelColor(0, 0), QColor("#2563EB"));
        const QImage primaryBefore = primaryViewport->currentImage();
        const auto modeBefore = primaryViewport->displayMode();
        QJsonObject projectBefore = project->toJson();

        const int initialViewportCount = window.findChildren<ViewportWidget*>().size();
        const AgentController::PermissionLevel previousPermission = controller.permissionLevel();
        struct RestorePermission {
            AgentController& controller;
            AgentController::PermissionLevel level;
            ~RestorePermission() {
                controller.setPermissionLevel(level);
            }
        } restorePermission{controller, previousPermission};

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject denied =
            controller.handleToolCall(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), initialViewportCount);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject rejected =
            controller.handleToolCall(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject context = controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
        QCOMPARE(context.value(QStringLiteral("sideEffects")).toArray().first().toString(),
                 QStringLiteral("gui_display_data_source_in_dedicated_viewport:cold_image (read-only source)"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), initialViewportCount);

        const QJsonObject pendingStale =
            controller.handleToolCall(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(pendingStale.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        addSource(QStringLiteral("generation_bump"), QStringLiteral("Generation bump"), generationPath,
                  QStringLiteral("image"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), initialViewportCount);
        projectBefore = project->toJson();
        const QJsonObject afterStale = actor.executeTool(QStringLiteral("get_data_source_view_state"), {});
        for (const QJsonValue& value : afterStale.value(QStringLiteral("dataSources")).toArray()) {
            const QJsonObject state = value.toObject();
            if (state.value(QStringLiteral("id")).toString() == imageSource.id)
                QVERIFY(!state.value(QStringLiteral("displayed")).toBool());
        }

        const QJsonObject displayedImage =
            actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                              QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(displayedImage.value(QStringLiteral("status")).toString(), QStringLiteral("displayed"));
        QVERIFY(displayedImage.value(QStringLiteral("created")).toBool());
        const QString imageViewportId = displayedImage.value(QStringLiteral("viewportId")).toString();
        QVERIFY(!imageViewportId.isEmpty());
        ViewportWidget* imageViewport = findViewport(imageViewportId);
        QVERIFY(imageViewport);
        QCOMPARE(imageViewport->currentImage().pixelColor(0, 0), QColor("#16A34A"));
        QCOMPARE(primaryViewport->currentImage(), primaryBefore);
        QCOMPARE(primaryViewport->displayMode(), modeBefore);

        QSignalSpy actionSpy(&controller, &AgentController::actionLogEntryAdded);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject approvedPending =
            controller.handleToolCall(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(approvedPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(actionSpy.count(), 1, 5000);
        const AgentActionLogEntry approvedEntry = actionSpy.last().at(0).value<AgentActionLogEntry>();
        QCOMPARE(approvedEntry.action, QStringLiteral("display_data_source_in_dedicated_viewport"));
        QCOMPARE(approvedEntry.result, QStringLiteral("success"));
        QCOMPARE(QJsonDocument::fromJson(approvedEntry.params.toUtf8()).object().keys(),
                 QStringList{QStringLiteral("dataSourceId")});
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), initialViewportCount + 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        const QJsonObject autopilotPending =
            controller.handleToolCall(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), cloudSource.id}});
        QCOMPARE(autopilotPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), initialViewportCount + 1);

        const QJsonObject repeatedImage =
            actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                              QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(repeatedImage.value(QStringLiteral("viewportId")).toString(), imageViewportId);
        QVERIFY(!repeatedImage.value(QStringLiteral("created")).toBool());
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), initialViewportCount + 1);
        QCOMPARE(primaryViewport->currentImage(), primaryBefore);
        QCOMPARE(primaryViewport->displayMode(), modeBefore);

        const QJsonObject displayedCloud =
            actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                              QJsonObject{{QStringLiteral("dataSourceId"), cloudSource.id}});
        QCOMPARE(displayedCloud.value(QStringLiteral("status")).toString(), QStringLiteral("displayed"));
        QVERIFY(displayedCloud.value(QStringLiteral("created")).toBool());
        ViewportWidget* cloudViewport = findViewport(displayedCloud.value(QStringLiteral("viewportId")).toString());
        QVERIFY(cloudViewport);
        QVERIFY(cloudViewport->hasCachedPointCloud());
        QCOMPARE(primaryViewport->currentImage(), primaryBefore);
        QCOMPARE(primaryViewport->displayMode(), modeBefore);
        QCOMPARE(project->toJson(), projectBefore);

        const QString duplicatePath = files.filePath(QStringLiteral("duplicate-id.png"));
        QImage duplicateImage(8, 8, QImage::Format_RGB32);
        duplicateImage.fill(Qt::red);
        QVERIFY(duplicateImage.save(duplicatePath));
        addSource(imageSource.id, QStringLiteral("Duplicate ID"), duplicatePath, QStringLiteral("image"));
        const int viewportCountBeforeAmbiguous = window.findChildren<ViewportWidget*>().size();
        const QJsonObject ambiguous = actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                                        QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(ambiguous.value(QStringLiteral("errorCode")).toString(), QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), viewportCountBeforeAmbiguous);

        const QJsonObject missing =
            actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                              QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("missing")}});
        QCOMPARE(missing.value(QStringLiteral("errorCode")).toString(), QStringLiteral("DATA_SOURCE_NOT_FOUND"));
        const QJsonObject broken = actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                                     QJsonObject{{QStringLiteral("dataSourceId"), brokenSource.id}});
        QCOMPARE(broken.value(QStringLiteral("errorCode")).toString(), QStringLiteral("DATA_SOURCE_LOAD_FAILED"));
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), initialViewportCount + 2);
        QCOMPARE(primaryViewport->currentImage(), primaryBefore);
        QCOMPARE(primaryViewport->displayMode(), modeBefore);

        const DataSource hiddenSource =
            addSource(QStringLiteral("hidden"), QStringLiteral("Hidden"), hiddenPath, QStringLiteral("image"));
        QCOMPARE(actor
                     .executeTool(QStringLiteral("set_data_source_visibility"),
                                  QJsonObject{{QStringLiteral("dataSourceId"), hiddenSource.id},
                                              {QStringLiteral("visible"), false}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("hidden"));
        const int viewportCountBeforeHidden = window.findChildren<ViewportWidget*>().size();
        const QJsonObject hidden = actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                                     QJsonObject{{QStringLiteral("dataSourceId"), hiddenSource.id}});
        QCOMPARE(hidden.value(QStringLiteral("errorCode")).toString(), QStringLiteral("DATA_SOURCE_HIDDEN"));
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), viewportCountBeforeHidden);

        const QJsonObject invalid = actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                                      QJsonObject{{QStringLiteral("dataSourceId"), QString()}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));

        QJsonObject threadResult;
        std::thread worker([&]() {
            threadResult =
                controller.executeDataSourceViewAction(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                                       QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        });
        worker.join();
        QCOMPARE(threadResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));

        const QJsonObject projectBeforeCloseViewport = project->toJson();
        const QString cloudViewportId = cloudViewport->viewportId();
        const QJsonObject closeDedicated = controller.handleToolCall(
            QStringLiteral("close_viewport"), QJsonObject{{QStringLiteral("viewportId"), cloudViewportId}});
        QCOMPARE(closeDedicated.value(QStringLiteral("status")).toString(), QStringLiteral("closed"));
        QCOMPARE(closeDedicated.value(QStringLiteral("disposition")).toString(), QStringLiteral("destroyed"));
        QVERIFY(!findViewport(cloudViewportId));
        QCOMPARE(project->toJson(), projectBeforeCloseViewport);
        const QJsonObject redisplayedCloud =
            actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                              QJsonObject{{QStringLiteral("dataSourceId"), cloudSource.id}});
        QCOMPARE(redisplayedCloud.value(QStringLiteral("status")).toString(), QStringLiteral("displayed"));
        QVERIFY(redisplayedCloud.value(QStringLiteral("created")).toBool());
        QVERIFY(redisplayedCloud.value(QStringLiteral("viewportId")).toString() != cloudViewportId);

        QClipboard* clipboard = QGuiApplication::clipboard();
        QVERIFY(clipboard);
        const QString clipboardSentinel = QStringLiteral("preserve-me");
        clipboard->setText(clipboardSentinel);
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("copy_data_source_path_to_clipboard"), {})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("MISSING_PARAM"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("copy_data_source_path_to_clipboard"),
                                   QJsonObject{{QStringLiteral("dataSourceId"), 7}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("INVALID_PARAM"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("copy_data_source_path_to_clipboard"),
                                   QJsonObject{{QStringLiteral("dataSourceId"), QStringLiteral("unknown-source")}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("DATA_SOURCE_NOT_FOUND"));
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("copy_data_source_path_to_clipboard"),
                                   QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"));
        QCOMPARE(clipboard->text(), clipboardSentinel);
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject observerCopy =
            controller.handleToolCall(QStringLiteral("copy_data_source_path_to_clipboard"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), cloudSource.id}});
        QCOMPARE(observerCopy.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
        QCOMPARE(clipboard->text(), clipboardSentinel);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject rejectedCopy =
            controller.handleToolCall(QStringLiteral("copy_data_source_path_to_clipboard"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), cloudSource.id}});
        QCOMPARE(rejectedCopy.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject confirmation = rejectedCopy.value(QStringLiteral("confirmation")).toObject();
        const QJsonObject confirmationContext = confirmation.value(QStringLiteral("context")).toObject();
        QVERIFY(confirmationContext.value(QStringLiteral("summary")).toString().contains(cloudSource.filePath));
        bool disclosesClipboardReplacement = false;
        for (const QJsonValue& effect : confirmationContext.value(QStringLiteral("sideEffects")).toArray())
            disclosesClipboardReplacement |=
                effect.toString().contains(QStringLiteral("replaces the current system clipboard"));
        QVERIFY(disclosesClipboardReplacement);
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(clipboard->text(), clipboardSentinel);

        const QJsonObject staleCopy =
            controller.handleToolCall(QStringLiteral("copy_data_source_path_to_clipboard"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), cloudSource.id}});
        QCOMPARE(staleCopy.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString clipboardDriftPath = files.filePath(QStringLiteral("clipboard-drift.png"));
        QVERIFY(generationImage.save(clipboardDriftPath));
        const quint64 generationBeforeClipboardDrift = project->dataSourceCollectionGeneration();
        addSource(QStringLiteral("clipboard_confirmation_drift"), QStringLiteral("Clipboard drift"), clipboardDriftPath,
                  QStringLiteral("image"));
        QVERIFY(project->dataSourceCollectionGeneration() > generationBeforeClipboardDrift);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(clipboard->text(), clipboardSentinel);

        const QJsonObject approvedCopy =
            controller.handleToolCall(QStringLiteral("copy_data_source_path_to_clipboard"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), cloudSource.id}});
        QCOMPARE(approvedCopy.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonObject projectBeforeCopy = project->toJson();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(clipboard->text(), cloudSource.filePath);
        QCOMPARE(project->toJson(), projectBeforeCopy);

        DataSource emptyPathSource;
        emptyPathSource.id = QStringLiteral("empty_path_source");
        emptyPathSource.name = QStringLiteral("No path");
        emptyPathSource.type = QStringLiteral("image");
        project->addDataSource(emptyPathSource);
        clipboard->setText(clipboardSentinel);
        const QJsonObject emptyPathCopy =
            controller.actor()->executeTool(QStringLiteral("copy_data_source_path_to_clipboard"),
                                            QJsonObject{{QStringLiteral("dataSourceId"), emptyPathSource.id}});
        QCOMPARE(emptyPathCopy.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("DATA_SOURCE_PATH_UNAVAILABLE"));
        QCOMPARE(clipboard->text(), clipboardSentinel);

        ProjectManager::instance().closeProject();
        const QJsonObject noProject = actor.executeTool(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                                        QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(noProject.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_PROJECT"));
        const int viewportCountBeforeOwnerExpiry = window.findChildren<ViewportWidget*>().size();

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject ownerPending =
            controller.handleToolCall(QStringLiteral("display_data_source_in_dedicated_viewport"),
                                      QJsonObject{{QStringLiteral("dataSourceId"), imageSource.id}});
        QCOMPARE(ownerPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        controller.unregisterDataSourceViewHandler(&window);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QCOMPARE(window.findChildren<ViewportWidget*>().size(), viewportCountBeforeOwnerExpiry);
    }

private:
    std::unique_ptr<QTemporaryDir> m_appDataDir;
};

QTEST_MAIN(TestAgentDataSourceDedicatedViewport)
#include "test_agent_datasource_dedicated_viewport.moc"
