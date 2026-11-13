#include <QAction>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/agent/AgentController.h>
#include <core/agent/ToolSchema.h>
#include <core/display/DisplayData.h>
#include <core/manager/ProjectManager.h>
#include <memory>
#include <thread>
#include <ui/display/3d/Viewport3DContent.h>
#include <ui/display/DisplayManager.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentActionLogWidget.h>
#include <ui/widgets/HImageWidget.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

class TestAgentViewportDisplayMode : public QObject {
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
            QStringLiteral("set_primary_viewport_display_mode"), QJsonObject{{QStringLiteral("mode"), "2d"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }

    void testDualViewportModeSwitchIsExplicitAndIdempotent() {
        QJsonObject definition;
        for (const QJsonValue& value : ToolSchema::instance().toOpenAIFunctions()) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")) == QLatin1String("set_primary_viewport_display_mode")) {
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
                 QJsonArray({QStringLiteral("2d"), QStringLiteral("3d")}));
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

        const QImage image(32, 24, QImage::Format_ARGB32_Premultiplied);
        PointCloudData cloud;
        cloud.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 1.0}};
        DisplayData cloudData;
        cloudData.variant() = cloud;
        primary->setDualData(image, cloudData, ViewportWidget::DisplayMode::Auto3D);
        auto* dedicated = displayManager->viewport(displayManager->createViewport(QStringLiteral("dedicated")));
        QVERIFY(dedicated);
        dedicated->setDualData(image, cloudData, ViewportWidget::DisplayMode::Auto2D);
        const auto dedicatedMode = dedicated->displayMode();

        QSignalSpy imageDisplayedSpy(primary, &ViewportWidget::imageDisplayed);
        AgentController& controller = AgentController::instance();
        for (AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            const int signalCountBefore2D = imageDisplayedSpy.size();
            const QJsonObject to2D = controller.handleToolCall(
                QStringLiteral("set_primary_viewport_display_mode"),
                QJsonObject{{QStringLiteral("mode"), "2d"}, {QStringLiteral("ignored"), true}});
            QCOMPARE(to2D.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(to2D.value(QStringLiteral("mode")).toString(), QStringLiteral("2d"));
            QCOMPARE(to2D.value(QStringLiteral("changed")).toBool(), true);
            QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto2D);
            QVERIFY(primary->imageWidget()->hasImage());
            QCOMPARE(primary->imageWidget()->currentImage().size(), image.size());
            QCOMPARE(imageDisplayedSpy.size(), signalCountBefore2D + 1);

            const int signalCount = imageDisplayedSpy.size();
            const QJsonObject repeated2D = controller.handleToolCall(
                QStringLiteral("set_primary_viewport_display_mode"), QJsonObject{{QStringLiteral("mode"), "2d"}});
            QCOMPARE(repeated2D.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(repeated2D.value(QStringLiteral("changed")).toBool(), false);
            QCOMPARE(imageDisplayedSpy.size(), signalCount);

            const QJsonObject to3D = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                                               QJsonObject{{QStringLiteral("mode"), "3d"}});
            QCOMPARE(to3D.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(to3D.value(QStringLiteral("changed")).toBool(), true);
            QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto3D);
            QVERIFY(primary->viewport3D());
            QCOMPARE(primary->viewport3D()->isVisible(), true);
            const QJsonObject repeated3D = controller.handleToolCall(
                QStringLiteral("set_primary_viewport_display_mode"), QJsonObject{{QStringLiteral("mode"), "3d"}});
            QCOMPARE(repeated3D.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(repeated3D.value(QStringLiteral("changed")).toBool(), false);
            QCOMPARE(imageDisplayedSpy.size(), signalCount);
            QCOMPARE(dedicated->displayMode(), dedicatedMode);
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);
        }

        QAction* toggleAction = primary->findChild<QAction*>(QStringLiteral("ViewportToggleViewAction"));
        QVERIFY(toggleAction);
        QVERIFY(toggleAction->isVisible());
        int signalCountBeforeGuiToggle = imageDisplayedSpy.size();
        toggleAction->trigger();
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto2D);
        QCOMPARE(imageDisplayedSpy.size(), signalCountBeforeGuiToggle + 1);
        signalCountBeforeGuiToggle = imageDisplayedSpy.size();
        toggleAction->trigger();
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto3D);
        QCOMPARE(imageDisplayedSpy.size(), signalCountBeforeGuiToggle);

        const QList<QString> expectedModes{QStringLiteral("2d"), QStringLiteral("2d"), QStringLiteral("3d"),
                                           QStringLiteral("3d"), QStringLiteral("2d"), QStringLiteral("2d"),
                                           QStringLiteral("3d"), QStringLiteral("3d"), QStringLiteral("2d"),
                                           QStringLiteral("2d"), QStringLiteral("3d"), QStringLiteral("3d")};
        QList<QString> auditedModes;
        for (const AgentActionLogEntry& entry : actionLog->entries()) {
            if (entry.action != QLatin1String("set_primary_viewport_display_mode"))
                continue;
            const QJsonObject audit = QJsonDocument::fromJson(entry.params.toUtf8()).object();
            QCOMPARE(audit.keys(), QStringList{QStringLiteral("mode")});
            if (entry.result == QLatin1String("success"))
                auditedModes.append(audit.value(QStringLiteral("mode")).toString());
        }
        QCOMPARE(auditedModes, expectedModes);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testSingleAndInvalidCachesFailClosed() {
        MainWindow window;
        auto* displayManager = window.findChild<DisplayManager*>();
        QVERIFY(displayManager);
        ViewportWidget* primary = displayManager->allViewports().constFirst();
        QVERIFY(primary);
        AgentController& controller = AgentController::instance();
        QSignalSpy imageDisplayedSpy(primary, &ViewportWidget::imageDisplayed);
        const QImage image(16, 12, QImage::Format_ARGB32);
        PointCloudData cloud;
        cloud.points = {{0.0, 0.0, 0.0}};
        DisplayData cloudData;
        cloudData.variant() = cloud;

        primary->displayImage(image);
        int signalCount = imageDisplayedSpy.size();
        QJsonObject result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                                       QJsonObject{{QStringLiteral("mode"), "2d"}});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(result.value(QStringLiteral("changed")).toBool(), false);
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "3d"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_POINT_CLOUD"));
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto2D);
        QCOMPARE(imageDisplayedSpy.size(), signalCount);

        primary->displayData(cloudData);
        signalCount = imageDisplayedSpy.size();
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "3d"}});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(result.value(QStringLiteral("changed")).toBool(), false);
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "2d"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_IMAGE"));
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto3D);
        QCOMPARE(imageDisplayedSpy.size(), signalCount);

        primary->setDualData(QImage(), cloudData, ViewportWidget::DisplayMode::Auto3D);
        signalCount = imageDisplayedSpy.size();
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "2d"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_IMAGE"));
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto3D);
        QCOMPARE(imageDisplayedSpy.size(), signalCount);

        PointCloudData emptyCloud;
        DisplayData emptyCloudData;
        emptyCloudData.variant() = emptyCloud;
        primary->setDualData(image, emptyCloudData, ViewportWidget::DisplayMode::Auto2D);
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "2d"}});
        QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QCOMPARE(result.value(QStringLiteral("changed")).toBool(), false);
        signalCount = imageDisplayedSpy.size();
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "3d"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_POINT_CLOUD"));
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto2D);
        QCOMPARE(imageDisplayedSpy.size(), signalCount);

        primary->setDualData(QImage(), emptyCloudData, ViewportWidget::DisplayMode::Auto2D);
        signalCount = imageDisplayedSpy.size();
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "2d"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_IMAGE"));
        result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"),
                                           QJsonObject{{QStringLiteral("mode"), "3d"}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_IMAGE"));
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto2D);
        QCOMPARE(imageDisplayedSpy.size(), signalCount);

        for (const QJsonObject& invalid : {QJsonObject(), QJsonObject{{QStringLiteral("mode"), 2}},
                                           QJsonObject{{QStringLiteral("mode"), QStringLiteral("toggle")}}}) {
            result = controller.handleToolCall(QStringLiteral("set_primary_viewport_display_mode"), invalid);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(),
                     invalid.isEmpty() ? QStringLiteral("MISSING_PARAM") : QStringLiteral("INVALID_PARAM"));
        }
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testThreadAffinityAndMissingPrimaryViewport() {
        MainWindow window;
        auto* displayManager = window.findChild<DisplayManager*>();
        QVERIFY(displayManager);
        QJsonObject threadResult;
        std::thread worker([&]() {
            threadResult = AgentController::instance().executeDataSourceViewAction(
                QStringLiteral("set_primary_viewport_display_mode"), QJsonObject{{QStringLiteral("mode"), "2d"}});
        });
        worker.join();
        QCOMPARE(threadResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));

        QStringList viewportIds;
        for (ViewportWidget* viewport : displayManager->allViewports())
            viewportIds.append(viewport->viewportId());
        for (const QString& id : viewportIds)
            displayManager->destroyViewport(id);
        const QJsonObject unavailable = AgentController::instance().handleToolCall(
            QStringLiteral("set_primary_viewport_display_mode"), QJsonObject{{QStringLiteral("mode"), "2d"}});
        QCOMPARE(unavailable.value(QStringLiteral("errorCode")).toString(), QStringLiteral("GUI_UNAVAILABLE"));
    }

private:
    std::unique_ptr<QTemporaryDir> m_appDataDir;
};

QTEST_MAIN(TestAgentViewportDisplayMode)
#include "test_agent_viewport_display_mode.moc"
