#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <cmath>
#include <core/agent/AgentController.h>
#include <core/agent/ToolSchema.h>
#include <core/display/DisplayData.h>
#include <core/manager/ProjectManager.h>
#include <memory>
#include <thread>
#include <ui/display/DisplayManager.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentActionLogWidget.h>
#include <ui/widgets/HImageWidget.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

class TestAgentViewportZoom : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        ToolSchema::instance().registerDefaultTools();
    }

    void testRequiresGuiWithoutWindow() {
        const QJsonObject result = AgentController::instance().executeDataSourceViewAction(
            QStringLiteral("set_primary_viewport_zoom"),
            QJsonObject{{QStringLiteral("action"), QStringLiteral("fit")}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }

    void testSchemaActionsPermissionsAndZoomBehavior() {
        QJsonObject definition;
        for (const QJsonValue& value : ToolSchema::instance().toOpenAIFunctions()) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")) == QLatin1String("set_primary_viewport_zoom")) {
                definition = function;
                break;
            }
        }
        QVERIFY(!definition.isEmpty());
        const QJsonObject parameters = definition.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(parameters.value(QStringLiteral("required")).toArray(), QJsonArray{QStringLiteral("action")});
        const QJsonObject actionSchema =
            parameters.value(QStringLiteral("properties")).toObject().value(QStringLiteral("action")).toObject();
        QCOMPARE(actionSchema.value(QStringLiteral("enum")).toArray(),
                 QJsonArray({QStringLiteral("zoom_in"), QStringLiteral("zoom_out"), QStringLiteral("fit"),
                             QStringLiteral("actual")}));
        QVERIFY(!definition.value(QStringLiteral("dangerous")).toBool());

        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto* displayManager = window.findChild<DisplayManager*>();
        auto* actionLog = window.findChild<AgentActionLogWidget*>();
        QVERIFY(displayManager);
        QVERIFY(actionLog);
        QVERIFY(!ProjectManager::instance().currentProject());
        QVERIFY(!displayManager->allViewports().isEmpty());
        ViewportWidget* primary = displayManager->allViewports().constFirst();
        QVERIFY(primary);
        const QImage image(40, 30, QImage::Format_ARGB32_Premultiplied);
        primary->displayImage(image);
        QVERIFY(primary->imageWidget()->hasImage());

        AgentController& controller = AgentController::instance();
        for (AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            const double startZoom = primary->imageWidget()->zoom();
            const QJsonObject zoomIn = controller.handleToolCall(QStringLiteral("set_primary_viewport_zoom"),
                                                                 QJsonObject{{QStringLiteral("action"), "zoom_in"}});
            QCOMPARE(zoomIn.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QVERIFY(qAbs(primary->imageWidget()->zoom() - startZoom * 1.25) < 0.000001);
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);
            const QJsonObject zoomOut = controller.handleToolCall(QStringLiteral("set_primary_viewport_zoom"),
                                                                  QJsonObject{{QStringLiteral("action"), "zoom_out"}});
            QCOMPARE(zoomOut.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QVERIFY(qAbs(primary->imageWidget()->zoom() - startZoom) < 0.000001);
            const QJsonObject fit = controller.handleToolCall(QStringLiteral("set_primary_viewport_zoom"),
                                                              QJsonObject{{QStringLiteral("action"), "fit"}});
            QCOMPARE(fit.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QVERIFY(std::isfinite(fit.value(QStringLiteral("zoomFactor")).toDouble()));
            QVERIFY(fit.value(QStringLiteral("zoomFactor")).toDouble() > 0.0);
            const QJsonObject actual = controller.handleToolCall(QStringLiteral("set_primary_viewport_zoom"),
                                                                 QJsonObject{{QStringLiteral("action"), "actual"}});
            QCOMPARE(actual.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(actual.value(QStringLiteral("zoomFactor")).toDouble(), 1.0);
        }

        ViewportWidget* dedicated =
            displayManager->viewport(displayManager->createViewport(QStringLiteral("dedicated")));
        QVERIFY(dedicated);
        dedicated->displayImage(image);
        const double dedicatedZoom = dedicated->imageWidget()->zoom();
        const QJsonObject zoomDedicatedIsolation = controller.handleToolCall(
            QStringLiteral("set_primary_viewport_zoom"), QJsonObject{{QStringLiteral("action"), "zoom_in"}});
        QCOMPARE(zoomDedicatedIsolation.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(dedicated->imageWidget()->zoom(), dedicatedZoom);

        PointCloudData cloud;
        cloud.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 1.0}};
        DisplayData cloudData;
        cloudData.variant() = cloud;
        primary->setDualData(image, cloudData, ViewportWidget::DisplayMode::Auto3D);
        QVERIFY(primary->imageWidget()->hasImage());
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto3D);
        const double auto3DZoom = primary->imageWidget()->zoom();
        for (const QString& action :
             {QStringLiteral("zoom_in"), QStringLiteral("zoom_out"), QStringLiteral("fit"), QStringLiteral("actual")}) {
            const QJsonObject rejected = controller.handleToolCall(QStringLiteral("set_primary_viewport_zoom"),
                                                                   QJsonObject{{QStringLiteral("action"), action}});
            QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_IMAGE"));
            QCOMPARE(primary->imageWidget()->zoom(), auto3DZoom);
        }

        primary->displayImage(QImage());
        const double emptyZoom = primary->imageWidget()->zoom();
        const QJsonObject noImage = controller.handleToolCall(QStringLiteral("set_primary_viewport_zoom"),
                                                              QJsonObject{{QStringLiteral("action"), "actual"}});
        QCOMPARE(noImage.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_IMAGE"));
        QCOMPARE(primary->imageWidget()->zoom(), emptyZoom);

        for (const QJsonObject& invalid : {QJsonObject(), QJsonObject{{QStringLiteral("action"), 3}},
                                           QJsonObject{{QStringLiteral("action"), QStringLiteral("reset")}}}) {
            const QJsonObject result = controller.handleToolCall(QStringLiteral("set_primary_viewport_zoom"), invalid);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(),
                     invalid.isEmpty() ? QStringLiteral("MISSING_PARAM") : QStringLiteral("INVALID_PARAM"));
            QCOMPARE(primary->imageWidget()->zoom(), emptyZoom);
        }

        bool sawAuditedAction = false;
        for (const AgentActionLogEntry& entry : actionLog->entries()) {
            if (entry.action != QLatin1String("set_primary_viewport_zoom"))
                continue;
            sawAuditedAction = true;
            const QJsonObject audited = QJsonDocument::fromJson(entry.params.toUtf8()).object();
            QVERIFY(audited.isEmpty() || audited.keys() == QStringList{QStringLiteral("action")});
        }
        QVERIFY(sawAuditedAction);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testThreadAffinityAndMissingPrimaryViewport() {
        MainWindow window;
        auto* displayManager = window.findChild<DisplayManager*>();
        QVERIFY(displayManager);
        ViewportWidget* primary = displayManager->allViewports().constFirst();
        primary->displayImage(QImage(20, 20, QImage::Format_ARGB32));

        QJsonObject threadResult;
        std::thread worker([&]() {
            threadResult = AgentController::instance().executeDataSourceViewAction(
                QStringLiteral("set_primary_viewport_zoom"), QJsonObject{{QStringLiteral("action"), "actual"}});
        });
        worker.join();
        QCOMPARE(threadResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));

        const QStringList viewportIds = [&]() {
            QStringList ids;
            for (ViewportWidget* viewport : displayManager->allViewports())
                ids.append(viewport->viewportId());
            return ids;
        }();
        for (const QString& id : viewportIds)
            displayManager->destroyViewport(id);
        const QJsonObject missingViewport = AgentController::instance().handleToolCall(
            QStringLiteral("set_primary_viewport_zoom"), QJsonObject{{QStringLiteral("action"), "actual"}});
        QCOMPARE(missingViewport.value(QStringLiteral("errorCode")).toString(), QStringLiteral("GUI_UNAVAILABLE"));
    }

private:
    std::unique_ptr<QTemporaryDir> m_appDataDir;
};

QTEST_MAIN(TestAgentViewportZoom)
#include "test_agent_viewport_zoom.moc"
