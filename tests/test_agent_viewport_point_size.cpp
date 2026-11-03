#include <QDoubleSpinBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <cmath>
#include <core/agent/AgentController.h>
#include <core/agent/ToolSchema.h>
#include <core/display/DisplayData.h>
#include <core/manager/ProjectManager.h>
#include <limits>
#include <memory>
#include <thread>
#include <ui/display/3d/Viewport3DContent.h>
#include <ui/display/DisplayManager.h>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentActionLogWidget.h>
#include <ui/widgets/ViewportWidget.h>

using namespace DeepLux;

class TestAgentViewportPointSize : public QObject {
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
            QStringLiteral("set_primary_viewport_point_size"), QJsonObject{{QStringLiteral("size"), 3.5}});
        QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
    }

    void testPointSizeSchemaPermissionsAndValidation() {
        QJsonObject definition;
        for (const QJsonValue& value : ToolSchema::instance().toOpenAIFunctions()) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")) == QLatin1String("set_primary_viewport_point_size")) {
                definition = function;
                break;
            }
        }
        QVERIFY(!definition.isEmpty());
        const QJsonObject parameters = definition.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(parameters.value(QStringLiteral("required")).toArray(), QJsonArray{QStringLiteral("size")});
        QCOMPARE(parameters.value(QStringLiteral("properties"))
                     .toObject()
                     .value(QStringLiteral("size"))
                     .toObject()
                     .value(QStringLiteral("type"))
                     .toString(),
                 QStringLiteral("number"));
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
        auto* spinBox = primary->findChild<QDoubleSpinBox*>(QStringLiteral("ViewportPointSizeSpinBox"));
        QVERIFY(spinBox);

        AgentController& controller = AgentController::instance();
        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject not3D = controller.handleToolCall(QStringLiteral("set_primary_viewport_point_size"),
                                                            QJsonObject{{QStringLiteral("size"), 3.5}});
        QCOMPARE(not3D.value(QStringLiteral("errorCode")).toString(), QStringLiteral("NO_POINT_CLOUD_VIEW"));
        QCOMPARE(spinBox->value(), 2.0);

        PointCloudData cloud;
        cloud.points = {{0.0, 0.0, 0.0}, {1.0, 0.0, 1.0}};
        DisplayData cloudData;
        cloudData.variant() = cloud;
        primary->displayData(cloudData);
        QCOMPARE(primary->displayMode(), ViewportWidget::DisplayMode::Auto3D);
        QVERIFY(primary->viewport3D());

        ViewportWidget* dedicated =
            displayManager->viewport(displayManager->createViewport(QStringLiteral("dedicated")));
        QVERIFY(dedicated);
        dedicated->displayData(cloudData);
        QVERIFY(dedicated->viewport3D());
        const float dedicatedSize = dedicated->viewport3D()->pointSize();

        for (AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            for (double expected : {1.0, 3.7, 8.0}) {
                const QJsonObject result = controller.handleToolCall(QStringLiteral("set_primary_viewport_point_size"),
                                                                     QJsonObject{{QStringLiteral("size"), expected}});
                QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
                QCOMPARE(result.value(QStringLiteral("size")).toDouble(), expected);
                QCOMPARE(spinBox->value(), expected);
                QVERIFY(std::abs(primary->viewport3D()->pointSize() - static_cast<float>(expected)) < 0.00001f);
                QCOMPARE(dedicated->viewport3D()->pointSize(), dedicatedSize);
                QCOMPARE(controller.state(), AgentController::AgentState::Idle);
            }
        }

        const double previous = spinBox->value();
        const QList<QJsonObject> invalidCalls{
            {},
            {{QStringLiteral("size"), QStringLiteral("3.5")}},
            {{QStringLiteral("size"), true}},
            {{QStringLiteral("size"), QJsonValue(QJsonValue::Null)}},
            {{QStringLiteral("size"), QJsonValue(std::numeric_limits<double>::quiet_NaN())}},
            {{QStringLiteral("size"), QJsonValue(std::numeric_limits<double>::infinity())}},
            {{QStringLiteral("size"), QJsonValue(-std::numeric_limits<double>::infinity())}},
            {{QStringLiteral("size"), 0.9}},
            {{QStringLiteral("size"), 8.1}},
            {{QStringLiteral("size"), 3.14}},
        };
        for (const QJsonObject& params : invalidCalls) {
            const QJsonObject result =
                controller.handleToolCall(QStringLiteral("set_primary_viewport_point_size"), params);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(),
                     params.isEmpty() ? QStringLiteral("MISSING_PARAM") : QStringLiteral("INVALID_PARAM"));
            QCOMPARE(spinBox->value(), previous);
            QVERIFY(std::abs(primary->viewport3D()->pointSize() - static_cast<float>(previous)) < 0.00001f);
            QVERIFY(!actionLog->entries().isEmpty());
            const AgentActionLogEntry& lastEntry = actionLog->entries().constLast();
            QCOMPARE(lastEntry.action, QStringLiteral("set_primary_viewport_point_size"));
            QVERIFY(QJsonDocument::fromJson(lastEntry.params.toUtf8()).object().isEmpty());
        }

        bool sawValidAudit = false;
        QList<double> auditedSizes;
        const QList<double> expectedAuditedSizes{1.0, 3.7, 8.0, 1.0, 3.7, 8.0, 1.0, 3.7, 8.0};
        for (const AgentActionLogEntry& entry : actionLog->entries()) {
            if (entry.action != QLatin1String("set_primary_viewport_point_size"))
                continue;
            const QJsonObject audit = QJsonDocument::fromJson(entry.params.toUtf8()).object();
            if (entry.result == QLatin1String("success")) {
                sawValidAudit = true;
                QCOMPARE(audit.keys(), QStringList{QStringLiteral("size")});
                QVERIFY(audit.value(QStringLiteral("size")).isDouble());
                QCOMPARE(audit.value(QStringLiteral("size")).toDouble(), expectedAuditedSizes.at(auditedSizes.size()));
                auditedSizes.append(audit.value(QStringLiteral("size")).toDouble());
            } else if (entry.result == QLatin1String("error")) {
                if (!audit.isEmpty()) {
                    QCOMPARE(audit.keys(),
                             QStringList{QStringLiteral("size")}); // Valid size rejected by the 2D precondition.
                    QVERIFY(audit.value(QStringLiteral("size")).isDouble());
                }
            }
        }
        QVERIFY(sawValidAudit);
        QCOMPARE(auditedSizes, expectedAuditedSizes);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testThreadAffinityAndMissingPrimaryViewport() {
        MainWindow window;
        auto* displayManager = window.findChild<DisplayManager*>();
        QVERIFY(displayManager);
        QJsonObject threadResult;
        std::thread worker([&]() {
            threadResult = AgentController::instance().executeDataSourceViewAction(
                QStringLiteral("set_primary_viewport_point_size"), QJsonObject{{QStringLiteral("size"), 4.0}});
        });
        worker.join();
        QCOMPARE(threadResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));

        QStringList viewportIds;
        for (ViewportWidget* viewport : displayManager->allViewports())
            viewportIds.append(viewport->viewportId());
        for (const QString& id : viewportIds)
            displayManager->destroyViewport(id);
        const QJsonObject unavailable = AgentController::instance().handleToolCall(
            QStringLiteral("set_primary_viewport_point_size"), QJsonObject{{QStringLiteral("size"), 4.0}});
        QCOMPARE(unavailable.value(QStringLiteral("errorCode")).toString(), QStringLiteral("GUI_UNAVAILABLE"));
    }

private:
    std::unique_ptr<QTemporaryDir> m_appDataDir;
};

QTEST_MAIN(TestAgentViewportPointSize)
#include "test_agent_viewport_point_size.moc"
