#include <QAction>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/agent/AgentController.h>
#include <core/agent/ToolSchema.h>
#include <core/display/DisplayData.h>
#include <core/manager/ProjectManager.h>
#include <core/model/DataSource.h>
#include <core/model/Project.h>
#include <memory>
#include <thread>
#include <ui/display/3d/Viewport3DContent.h>
#include <ui/display/DisplayManager.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentActionLogWidget.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

class TestAgentViewportRenderMode : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = std::make_unique<QTemporaryDir>();
        m_fixtures = std::make_unique<QTemporaryDir>();
        QVERIFY(m_appDataDir->isValid());
        QVERIFY(m_fixtures->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        ToolSchema::instance().registerDefaultTools();
        ProjectManager::instance().closeProject();
    }

    void testRequiresGuiWithoutWindow() {
        const QJsonObject result = AgentController::instance().executeDataSourceViewAction(
            QStringLiteral("set_point_cloud_render_mode"), QJsonObject{{QStringLiteral("mode"), "uniform"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
        const QJsonObject closeResult = AgentController::instance().executeDataSourceViewAction(
            QStringLiteral("close_viewport"), QJsonObject{{QStringLiteral("viewportId"), "viewport_1"}});
        QCOMPARE(closeResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }

    void testNoPointCloudAndThreadFailures() {
        MainWindow window;
        const QJsonObject noCloud = AgentController::instance().handleToolCall(
            QStringLiteral("set_point_cloud_render_mode"), QJsonObject{{QStringLiteral("mode"), "uniform"}});
        QCOMPARE(noCloud.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_POINT_CLOUD_VIEW"));

        QJsonObject threadResult;
        std::thread worker([&]() {
            threadResult = AgentController::instance().executeDataSourceViewAction(
                QStringLiteral("set_point_cloud_render_mode"), QJsonObject{{QStringLiteral("mode"), "uniform"}});
        });
        worker.join();
        QCOMPARE(threadResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
    }

    void testCloseViewportKeepsLazyPrimaryAndDestroysAdditional() {
        AgentController& controller = AgentController::instance();
        MainWindow window;
        auto* displayManager = window.findChild<DisplayManager*>();
        QVERIFY(displayManager);
        QCOMPARE(displayManager->viewportCount(), 1);
        ViewportWidget* primary = displayManager->allViewports().constFirst();
        QVERIFY(primary);
        const QString primaryId = primary->viewportId();

        ViewportWidget* firstAdditional =
            displayManager->viewport(displayManager->createViewport(QStringLiteral("first-additional")));
        QVERIFY(firstAdditional);
        const QString firstAdditionalId = firstAdditional->viewportId();
        primary->displayImage(QImage(8, 8, QImage::Format_ARGB32));

        QJsonObject definition;
        for (const QJsonValue& value : ToolSchema::instance().toOpenAIFunctions()) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")) == QLatin1String("close_viewport")) {
                definition = function;
                break;
            }
        }
        QVERIFY(!definition.isEmpty());
        QVERIFY(!definition.value(QStringLiteral("dangerous")).toBool());
        QCOMPARE(definition.value(QStringLiteral("parameters")).toObject().value(QStringLiteral("required")).toArray(),
                 QJsonArray{QStringLiteral("viewportId")});

        for (AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            ViewportWidget* additional = firstAdditional;
            QString additionalId = firstAdditionalId;
            if (!additional) {
                additional =
                    displayManager->viewport(displayManager->createViewport(QStringLiteral("permission-additional")));
                QVERIFY(additional);
                additionalId = additional->viewportId();
            }
            QPointer<ViewportWidget> guardedAdditional(additional);
            const QJsonObject result = controller.handleToolCall(
                QStringLiteral("close_viewport"),
                QJsonObject{{QStringLiteral("viewportId"), additionalId}, {QStringLiteral("ignored"), true}});
            QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("closed"));
            QCOMPARE(result.value(QStringLiteral("disposition")).toString(), QStringLiteral("destroyed"));
            QVERIFY(result.value(QStringLiteral("destroyed")).toBool());
            QVERIFY(guardedAdditional.isNull());
            QCOMPARE(displayManager->viewport(primaryId), primary);
            QCOMPARE(displayManager->viewportCount(), 1);
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);
            QCOMPARE(controller
                         .handleToolCall(QStringLiteral("close_viewport"),
                                         QJsonObject{{QStringLiteral("viewportId"), additionalId}})
                         .value(QStringLiteral("errorCode"))
                         .toString(),
                     QStringLiteral("VIEWPORT_NOT_FOUND"));
            QCOMPARE(displayManager->viewportCount(), 1);
            QCOMPARE(displayManager->viewport(primaryId), primary);
            firstAdditional = nullptr;
        }

        for (const QJsonObject& invalid :
             {QJsonObject(), QJsonObject{{QStringLiteral("viewportId"), QStringLiteral(" ")}},
              QJsonObject{{QStringLiteral("viewportId"), 1}}}) {
            const QJsonObject result = controller.handleToolCall(QStringLiteral("close_viewport"), invalid);
            const QJsonValue viewportId = invalid.value(QStringLiteral("viewportId"));
            const QString expectedError =
                viewportId.isUndefined() || (viewportId.isString() && viewportId.toString().trimmed().isEmpty())
                    ? QStringLiteral("MISSING_PARAM")
                    : QStringLiteral("INVALID_PARAM");
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), expectedError);
            QCOMPARE(displayManager->viewport(primaryId), primary);
        }
        QCOMPARE(controller
                     .handleToolCall(QStringLiteral("close_viewport"),
                                     QJsonObject{{QStringLiteral("viewportId"), "missing"}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("VIEWPORT_NOT_FOUND"));

        primary->displayImage(QImage(8, 8, QImage::Format_ARGB32));
        const QJsonObject clearPrimary = controller.handleToolCall(
            QStringLiteral("close_viewport"), QJsonObject{{QStringLiteral("viewportId"), primaryId}});
        QCOMPARE(clearPrimary.value(QStringLiteral("disposition")).toString(), QStringLiteral("cleared"));
        QVERIFY(!clearPrimary.value(QStringLiteral("destroyed")).toBool());
        QCOMPARE(displayManager->viewport(primaryId), primary);
        QVERIFY(primary->currentImage().isNull());

        ViewportWidget* guiAdditional =
            displayManager->viewport(displayManager->createViewport(QStringLiteral("gui-additional")));
        QVERIFY(guiAdditional);
        QPointer<ViewportWidget> guardedGuiAdditional(guiAdditional);
        auto* closeAction = guiAdditional->findChild<QAction*>(QStringLiteral("ViewportCloseAction"));
        QVERIFY(closeAction);
        closeAction->trigger();
        QVERIFY(guardedGuiAdditional.isNull());
        QCOMPARE(displayManager->viewport(primaryId), primary);

        QJsonObject threadResult;
        std::thread worker([&]() {
            threadResult = controller.executeDataSourceViewAction(
                QStringLiteral("close_viewport"), QJsonObject{{QStringLiteral("viewportId"), primaryId}});
        });
        worker.join();
        QCOMPARE(threadResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QCOMPARE(displayManager->viewport(primaryId), primary);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        QVERIFY(!ProjectManager::instance().currentProject());
    }

    void testAvailableModesUseGuiActionAndPersistIntoLazy3DContent() {
        const QString plyPath = m_fixtures->filePath(QStringLiteral("rich.ply"));
        QFile ply(plyPath);
        QVERIFY(ply.open(QIODevice::WriteOnly | QIODevice::Text));
        QVERIFY(ply.write("ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\n"
                          "property float z\nproperty float nx\nproperty float ny\nproperty float nz\n"
                          "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n"
                          "0 0 0 0 0 1 255 0 0\n1 0 0 0 0 1 0 255 0\n0 1 0 0 0 1 0 0 255\n") > 0);
        ply.close();

        MainWindow window;
        window.show();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        DataSource source;
        source.id = QStringLiteral("render_mode_cloud");
        source.name = QStringLiteral("rich.ply");
        source.filePath = plyPath;
        source.type = QStringLiteral("pointcloud");
        project->addDataSource(source);
        QVERIFY(
            QMetaObject::invokeMethod(&window, "onDisplayDataSource", Qt::DirectConnection, Q_ARG(QString, source.id)));

        auto* displayManager = window.findChild<DisplayManager*>();
        auto* actionLog = window.findChild<AgentActionLogWidget*>();
        QVERIFY(displayManager);
        QVERIFY(actionLog);
        QVERIFY(!displayManager->allViewports().isEmpty());
        ViewportWidget* primary = displayManager->allViewports().constFirst();
        QVERIFY(primary);
        QVERIFY(primary->viewport3D());
        auto renderAction = [&window](int index) {
            return window.findChild<QAction*>(QStringLiteral("RenderModeAction%1").arg(index));
        };
        for (int i = 0; i < 6; ++i)
            QVERIFY(renderAction(i));

        ViewportWidget* lazyDual =
            displayManager->viewport(displayManager->createViewport(QStringLiteral("lazy-dual")));
        QVERIFY(lazyDual);
        PointCloudData cloud;
        cloud.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 1.0}, {0.0, 1.0, 2.0}};
        cloud.colors = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
        cloud.normals = {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}};
        DisplayData cloudData;
        cloudData.variant() = cloud;
        const QImage image(16, 12, QImage::Format_ARGB32);
        lazyDual->setDualData(image, cloudData, ViewportWidget::DisplayMode::Auto2D);
        QVERIFY(!lazyDual->viewport3D());

        QJsonObject definition;
        for (const QJsonValue& value : ToolSchema::instance().toOpenAIFunctions()) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")) == QLatin1String("set_point_cloud_render_mode")) {
                definition = function;
                break;
            }
        }
        QVERIFY(!definition.isEmpty());
        const QJsonObject parameters = definition.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(parameters.value(QStringLiteral("required")).toArray(), QJsonArray{QStringLiteral("mode")});
        QCOMPARE(parameters.value(QStringLiteral("properties"))
                     .toObject()
                     .value(QStringLiteral("mode"))
                     .toObject()
                     .value(QStringLiteral("enum"))
                     .toArray(),
                 QJsonArray({QStringLiteral("uniform"), QStringLiteral("rgb"), QStringLiteral("height"),
                             QStringLiteral("intensity"), QStringLiteral("normal"), QStringLiteral("blinn_phong")}));
        QVERIFY(!definition.value(QStringLiteral("dangerous")).toBool());

        const QList<QPair<QString, int>> modes{{QStringLiteral("uniform"), 0},
                                               {QStringLiteral("rgb"), 1},
                                               {QStringLiteral("height"), 2},
                                               {QStringLiteral("normal"), 4},
                                               {QStringLiteral("blinn_phong"), 5}};
        AgentController& controller = AgentController::instance();
        for (AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            for (const auto& [modeName, modeIndex] : modes) {
                QVERIFY(renderAction(modeIndex)->isVisible());
                QVERIFY(renderAction(modeIndex)->isEnabled());
                const QJsonObject result = controller.handleToolCall(
                    QStringLiteral("set_point_cloud_render_mode"),
                    QJsonObject{{QStringLiteral("mode"), modeName}, {QStringLiteral("ignored"), true}});
                QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
                QCOMPARE(result.value(QStringLiteral("mode")).toString(), modeName);
                QVERIFY(renderAction(modeIndex)->isChecked());
                QCOMPARE(primary->renderMode(), modeIndex);
                QCOMPARE(lazyDual->renderMode(), modeIndex);
                QCOMPARE(controller.state(), AgentController::AgentState::Idle);
            }
        }

        QVERIFY(!renderAction(3)->isVisible());
        const QJsonObject chooseRgb = controller.handleToolCall(QStringLiteral("set_point_cloud_render_mode"),
                                                                QJsonObject{{QStringLiteral("mode"), "rgb"}});
        QCOMPARE(chooseRgb.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(primary->renderMode(), 1);
        QCOMPARE(lazyDual->renderMode(), 1);

        ViewportWidget* lateViewport =
            displayManager->viewport(displayManager->createViewport(QStringLiteral("late-viewport")));
        QVERIFY(lateViewport);
        QCOMPARE(lateViewport->renderMode(), static_cast<int>(ColorMode::BlinnPhong));
        const QJsonObject repeatRgb = controller.handleToolCall(QStringLiteral("set_point_cloud_render_mode"),
                                                                QJsonObject{{QStringLiteral("mode"), "rgb"}});
        QCOMPARE(repeatRgb.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(primary->renderMode(), 1);
        QCOMPARE(lazyDual->renderMode(), 1);
        QCOMPARE(lateViewport->renderMode(), 1);

        const int selectedMode = primary->renderMode();
        const QJsonObject unsupported = controller.handleToolCall(QStringLiteral("set_point_cloud_render_mode"),
                                                                  QJsonObject{{QStringLiteral("mode"), "intensity"}});
        QCOMPARE(unsupported.value(QStringLiteral("errorCode")).toString(), QStringLiteral("RENDER_MODE_UNAVAILABLE"));
        QCOMPARE(primary->renderMode(), selectedMode);
        QCOMPARE(lazyDual->renderMode(), selectedMode);
        QCOMPARE(lateViewport->renderMode(), selectedMode);
        QVERIFY(renderAction(1)->isChecked());
        QVERIFY(!lazyDual->viewport3D());
        QCOMPARE(lazyDual->setDisplayMode(ViewportWidget::DisplayMode::Auto3D), true);
        QVERIFY(lazyDual->viewport3D());
        QCOMPARE(lazyDual->viewport3D()->renderMode(), ColorMode::RGB);

        for (const QJsonObject& invalid : {QJsonObject(), QJsonObject{{QStringLiteral("mode"), 1}},
                                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("auto")}}}) {
            const QJsonObject result =
                controller.handleToolCall(QStringLiteral("set_point_cloud_render_mode"), invalid);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(),
                     invalid.isEmpty() ? QStringLiteral("MISSING_PARAM") : QStringLiteral("INVALID_PARAM"));
        }

        bool sawModeAudit = false;
        for (const AgentActionLogEntry& entry : actionLog->entries()) {
            if (entry.action != QLatin1String("set_point_cloud_render_mode"))
                continue;
            const QJsonObject audit = QJsonDocument::fromJson(entry.params.toUtf8()).object();
            if (entry.result == QLatin1String("success")) {
                sawModeAudit = true;
                QCOMPARE(audit.keys(), QStringList{QStringLiteral("mode")});
            }
        }
        QVERIFY(sawModeAudit);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        ProjectManager::instance().closeProject();
    }

private:
    std::unique_ptr<QTemporaryDir> m_appDataDir;
    std::unique_ptr<QTemporaryDir> m_fixtures;
};

QTEST_MAIN(TestAgentViewportRenderMode)
#include "test_agent_viewport_render_mode.moc"
