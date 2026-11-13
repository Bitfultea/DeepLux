#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSplitter>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest/QtTest>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/ToolSchema.h>
#include <core/base/ModuleBase.h>
#include <core/common/Logger.h>
#include <core/manager/ProjectManager.h>
#include <core/model/Project.h>
#include <memory>
#include <thread>
#include <ui/views/MainWindow.h>
#include <ui/widgets/AgentActionLogWidget.h>
#include <ui/widgets/FlowCanvas.h>
#include <ui/widgets/ModuleInspectorPanel.h>

using namespace DeepLux;

class InspectorVisibilityTestModule : public ModuleBase {
    Q_OBJECT

public:
    InspectorVisibilityTestModule() {
        m_moduleId = QStringLiteral("MissingTestModule");
        m_name = QStringLiteral("Inspector visibility test module");
    }

    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        Q_UNUSED(output)
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class TestAgentInspectorPin : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        m_appDataDir = std::make_unique<QTemporaryDir>();
        QVERIFY(m_appDataDir->isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
        ToolSchema::instance().registerDefaultTools();
    }

    void testInspectorPinIsExplicitStrictAndReadOnly() {
        const QJsonArray definitions = ToolSchema::instance().toOpenAIFunctions();
        QJsonObject definition;
        for (const QJsonValue& value : definitions) {
            const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
            if (function.value(QStringLiteral("name")) == QLatin1String("set_inspector_pinned")) {
                definition = function;
                break;
            }
        }
        QVERIFY(!definition.isEmpty());
        const QJsonObject parameters = definition.value(QStringLiteral("parameters")).toObject();
        QCOMPARE(parameters.value(QStringLiteral("required")).toArray(), QJsonArray{QStringLiteral("pinned")});
        QCOMPARE(parameters.value(QStringLiteral("properties"))
                     .toObject()
                     .value(QStringLiteral("pinned"))
                     .toObject()
                     .value(QStringLiteral("type"))
                     .toString(),
                 QStringLiteral("boolean"));
        QVERIFY(!definition.value(QStringLiteral("dangerous")).toBool());

        AgentController& controller = AgentController::instance();
        const QJsonObject noGui = controller.executeDataSourceViewAction(QStringLiteral("set_inspector_pinned"),
                                                                         QJsonObject{{QStringLiteral("pinned"), true}});
        QCOMPARE(noGui.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));

        ProjectManager::instance().closeProject();
        QVERIFY(!ProjectManager::instance().currentProject());
        MainWindow window;
        window.show();
        QCoreApplication::processEvents();
        auto* inspector = window.findChild<ModuleInspectorPanel*>();
        auto* pinButton = window.findChild<QToolButton*>(QStringLiteral("InspectorPinBtn"));
        auto* actionLog = window.findChild<AgentActionLogWidget*>();
        QVERIFY(inspector);
        QVERIFY(pinButton);
        QVERIFY(actionLog);
        inspector->setPinned(false);
        QCOMPARE(pinButton->isChecked(), false);
        QVERIFY(!ProjectManager::instance().currentProject());

        controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
        const QJsonObject invalidNumber = controller.handleToolCall(QStringLiteral("set_inspector_pinned"),
                                                                    QJsonObject{{QStringLiteral("pinned"), 0}});
        QCOMPARE(invalidNumber.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!inspector->isPinned());
        QVERIFY(!pinButton->isChecked());
        const QJsonObject invalidString = controller.handleToolCall(
            QStringLiteral("set_inspector_pinned"), QJsonObject{{QStringLiteral("pinned"), QStringLiteral("false")}});
        QCOMPARE(invalidString.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!inspector->isPinned());
        QVERIFY(!pinButton->isChecked());
        const QJsonObject missing = controller.handleToolCall(QStringLiteral("set_inspector_pinned"), {});
        QCOMPARE(missing.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
        QVERIFY(!inspector->isPinned());

        const QJsonObject nullValue =
            controller.handleToolCall(QStringLiteral("set_inspector_pinned"),
                                      QJsonObject{{QStringLiteral("pinned"), QJsonValue(QJsonValue::Null)}});
        QCOMPARE(nullValue.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!inspector->isPinned());
        QVERIFY(!pinButton->isChecked());

        for (AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            const QJsonObject setPinned = controller.handleToolCall(
                QStringLiteral("set_inspector_pinned"),
                QJsonObject{{QStringLiteral("pinned"), true}, {QStringLiteral("unknown"), QStringLiteral("filtered")}});
            QCOMPARE(setPinned.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QCOMPARE(setPinned.value(QStringLiteral("pinned")).toBool(), true);
            QVERIFY(inspector->isPinned());
            QVERIFY(pinButton->isChecked());
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);

            const QJsonObject clearPinned = controller.handleToolCall(QStringLiteral("set_inspector_pinned"),
                                                                      QJsonObject{{QStringLiteral("pinned"), false}});
            QCOMPARE(clearPinned.value(QStringLiteral("pinned")).toBool(), false);
            QVERIFY(!inspector->isPinned());
            QVERIFY(!pinButton->isChecked());
        }
        QCOMPARE(actionLog->entries().size(), 10);
        for (int i = 4; i < actionLog->entries().size(); ++i) {
            const AgentActionLogEntry& entry = actionLog->entries().at(i);
            if (entry.action != QLatin1String("set_inspector_pinned"))
                continue;
            const QJsonObject audited = QJsonDocument::fromJson(entry.params.toUtf8()).object();
            QCOMPARE(audited.keys(), QStringList{QStringLiteral("pinned")});
        }
        const int logCountBeforeDirectActor = Logger::instance().logs().size();
        const QJsonObject explicitSet = controller.actor()->executeTool(QStringLiteral("set_inspector_pinned"),
                                                                        QJsonObject{{QStringLiteral("pinned"), true}});
        QCOMPARE(explicitSet.value(QStringLiteral("pinned")).toBool(), true);
        const QJsonObject repeatedSet = controller.actor()->executeTool(QStringLiteral("set_inspector_pinned"),
                                                                        QJsonObject{{QStringLiteral("pinned"), true}});
        QCOMPARE(repeatedSet.value(QStringLiteral("pinned")).toBool(), true);
        QVERIFY(inspector->isPinned());
        QCOMPARE(Logger::instance().logs().size(), logCountBeforeDirectActor);

        QJsonObject affinityResult;
        std::thread worker([&]() {
            affinityResult = controller.executeDataSourceViewAction(QStringLiteral("set_inspector_pinned"),
                                                                    QJsonObject{{QStringLiteral("pinned"), false}});
        });
        worker.join();
        QCOMPARE(affinityResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(inspector->isPinned());
        QVERIFY(pinButton->isChecked());
        QVERIFY(!ProjectManager::instance().currentProject());
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
    }

    void testNoGuiFailureAfterMainWindowCloses() {
        AgentController& controller = AgentController::instance();
        {
            MainWindow window;
            QVERIFY(window.findChild<ModuleInspectorPanel*>());
        }
        const QJsonObject noGui = controller.executeDataSourceViewAction(QStringLiteral("set_inspector_pinned"),
                                                                         QJsonObject{{QStringLiteral("pinned"), true}});
        QCOMPARE(noGui.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
        for (const auto& request :
             {qMakePair(QStringLiteral("set_inspector_collapsed"), QJsonObject{{QStringLiteral("collapsed"), true}}),
              qMakePair(QStringLiteral("set_inspector_visibility"), QJsonObject{{QStringLiteral("visible"), false}})}) {
            const QJsonObject result = controller.executeDataSourceViewAction(request.first, request.second);
            QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("REQUIRES_GUI"));
        }
    }

    void testInspectorCollapseAndVisibilityPreserveProjectSelection() {
        AgentController& controller = AgentController::instance();
        ProjectManager::instance().closeProject();
        QPointer<InspectorVisibilityTestModule> runtimeModule = new InspectorVisibilityTestModule();
        MainWindow window;
        window.resize(1200, 800);
        window.show();
        QVERIFY(!ProjectManager::instance().currentProject());
        QCOMPARE(controller
                     .handleToolCall(QStringLiteral("set_inspector_collapsed"),
                                     QJsonObject{{QStringLiteral("collapsed"), true}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("ok"));
        QCOMPARE(controller
                     .handleToolCall(QStringLiteral("set_inspector_visibility"),
                                     QJsonObject{{QStringLiteral("visible"), true}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("ok"));
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);

        ModuleInstance module;
        module.id = QStringLiteral("inspector_state_module");
        module.moduleId = QStringLiteral("MissingTestModule");
        module.name = QStringLiteral("Inspector state module");
        project->addModule(module);
        window.registerFlowModule(module.id, runtimeModule);
        QCoreApplication::processEvents();

        auto* inspector = window.findChild<ModuleInspectorPanel*>();
        auto* collapseButton = window.findChild<QToolButton*>(QStringLiteral("InspectorCollapseBtn"));
        auto* closeButton = window.findChild<QToolButton*>(QStringLiteral("InspectorCloseBtn"));
        auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("ProcessTree"));
        auto* canvas = window.findChild<FlowCanvas*>();
        auto* splitter = window.findChild<QSplitter*>(QStringLiteral("RightTopSplitter"));
        QVERIFY(inspector);
        QVERIFY(collapseButton);
        QVERIFY(closeButton);
        QVERIFY(tree);
        QVERIFY(canvas);
        QVERIFY(splitter);

        AgentActor actor;
        const QJsonArray tools = ToolSchema::instance().toOpenAIFunctions();
        for (const QString& name :
             {QStringLiteral("set_inspector_collapsed"), QStringLiteral("set_inspector_visibility")}) {
            bool found = false;
            for (const QJsonValue& value : tools) {
                const QJsonObject function = value.toObject().value(QStringLiteral("function")).toObject();
                if (function.value(QStringLiteral("name")) == name) {
                    found = true;
                    QVERIFY(!function.value(QStringLiteral("dangerous")).toBool());
                }
            }
            QVERIFY(found);
        }

        for (AgentController::PermissionLevel permission :
             {AgentController::PermissionLevel::Observer, AgentController::PermissionLevel::Advisor,
              AgentController::PermissionLevel::Autopilot}) {
            controller.setPermissionLevel(permission);
            const QJsonObject collapse = controller.handleToolCall(QStringLiteral("set_inspector_collapsed"),
                                                                   QJsonObject{{QStringLiteral("collapsed"), true}});
            QCOMPARE(collapse.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QVERIFY(collapse.value(QStringLiteral("collapsed")).toBool());
            QVERIFY(inspector->isCollapsed());
            QCoreApplication::processEvents();
            QVERIFY(inspector->width() <= 32);
            const QJsonObject expand = controller.handleToolCall(QStringLiteral("set_inspector_collapsed"),
                                                                 QJsonObject{{QStringLiteral("collapsed"), false}});
            QCOMPARE(expand.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QVERIFY(!inspector->isCollapsed());
            QCoreApplication::processEvents();
            QVERIFY(inspector->width() > 32);
        }

        collapseButton->click();
        QVERIFY(inspector->isCollapsed());
        const QJsonObject repeatedCollapse = controller.actor()->executeTool(
            QStringLiteral("set_inspector_collapsed"), QJsonObject{{QStringLiteral("collapsed"), true}});
        QVERIFY(!repeatedCollapse.value(QStringLiteral("changed")).toBool());
        controller.actor()->executeTool(QStringLiteral("set_inspector_collapsed"),
                                        QJsonObject{{QStringLiteral("collapsed"), false}});

        const QJsonObject missing = controller.handleToolCall(QStringLiteral("set_inspector_collapsed"), {});
        QCOMPARE(missing.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
        const QJsonObject invalid = controller.handleToolCall(QStringLiteral("set_inspector_collapsed"),
                                                              QJsonObject{{QStringLiteral("collapsed"), 1}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!inspector->isCollapsed());

        const QJsonObject missingVisibility = controller.handleToolCall(QStringLiteral("set_inspector_visibility"), {});
        QCOMPARE(missingVisibility.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
        const QJsonObject invalidVisibility = controller.handleToolCall(QStringLiteral("set_inspector_visibility"),
                                                                        QJsonObject{{QStringLiteral("visible"), 1}});
        QCOMPARE(invalidVisibility.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(inspector->isVisible());

        const QJsonObject selected = actor.executeTool(QStringLiteral("select_flow_module"),
                                                       QJsonObject{{QStringLiteral("instanceId"), module.id}});
        QCOMPARE(selected.value(QStringLiteral("status")).toString(), QStringLiteral("selected"));
        QCOMPARE(inspector->currentInstanceId(), module.id);
        QCoreApplication::processEvents();
        QTreeWidgetItem* selectedItem = tree->currentItem();
        QVERIFY(selectedItem);
        QCOMPARE(selectedItem->data(0, Qt::UserRole + 1).toString(), module.id);
        FlowNodeItem* selectedNode = canvas->nodeItem(module.id);
        QVERIFY(selectedNode);
        QVERIFY(selectedNode->isSelected());

        for (bool pinned : {false, true}) {
            const QJsonObject pin = controller.actor()->executeTool(QStringLiteral("set_inspector_pinned"),
                                                                    QJsonObject{{QStringLiteral("pinned"), pinned}});
            QCOMPARE(pin.value(QStringLiteral("pinned")).toBool(), pinned);
            const QJsonObject close = controller.handleToolCall(QStringLiteral("set_inspector_visibility"),
                                                                QJsonObject{{QStringLiteral("visible"), false}});
            QCOMPARE(close.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QVERIFY(!close.value(QStringLiteral("visible")).toBool());
            QVERIFY(!inspector->isVisible());
            QCOMPARE(inspector->isPinned(), pinned);
            QVERIFY(inspector->currentInstanceId().isEmpty());
            QCOMPARE(tree->currentItem(), selectedItem);
            QCOMPARE(selectedItem->data(0, Qt::UserRole + 1).toString(), module.id);
            QVERIFY(selectedNode->isSelected());

            const QJsonObject reopen = controller.handleToolCall(QStringLiteral("set_inspector_visibility"),
                                                                 QJsonObject{{QStringLiteral("visible"), true}});
            QCOMPARE(reopen.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
            QVERIFY(reopen.value(QStringLiteral("visible")).toBool());
            QVERIFY(inspector->isVisible());
            QCOMPARE(inspector->isPinned(), pinned);
            QCOMPARE(inspector->currentInstanceId(), module.id);
            QCOMPARE(tree->currentItem(), selectedItem);
            QVERIFY(selectedNode->isSelected());
        }

        closeButton->click();
        QVERIFY(!inspector->isVisible());
        QVERIFY(inspector->currentInstanceId().isEmpty());
        QCOMPARE(tree->currentItem(), selectedItem);
        QVERIFY(selectedNode->isSelected());
        const QJsonObject reopenAfterGuiClose = controller.handleToolCall(
            QStringLiteral("set_inspector_visibility"), QJsonObject{{QStringLiteral("visible"), true}});
        QCOMPARE(reopenAfterGuiClose.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
        QVERIFY(inspector->isVisible());
        QCOMPARE(inspector->currentInstanceId(), module.id);
        QCOMPARE(tree->currentItem(), selectedItem);
        QVERIFY(selectedNode->isSelected());

        QJsonObject visibilityAffinityResult;
        std::thread visibilityWorker([&]() {
            visibilityAffinityResult = controller.executeDataSourceViewAction(
                QStringLiteral("set_inspector_visibility"), QJsonObject{{QStringLiteral("visible"), false}});
        });
        visibilityWorker.join();
        QCOMPARE(visibilityAffinityResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("THREAD_AFFINITY"));
        QVERIFY(inspector->isVisible());

        auto* actionLog = window.findChild<AgentActionLogWidget*>();
        QVERIFY(actionLog);
        bool sawCollapseAudit = false;
        bool sawVisibilityAudit = false;
        for (const AgentActionLogEntry& entry : actionLog->entries()) {
            const QJsonObject params = QJsonDocument::fromJson(entry.params.toUtf8()).object();
            if (entry.action == QLatin1String("set_inspector_collapsed")) {
                if (entry.result != QLatin1String("success"))
                    continue;
                sawCollapseAudit = true;
                QCOMPARE(params.keys(), QStringList{QStringLiteral("collapsed")});
            } else if (entry.action == QLatin1String("set_inspector_visibility")) {
                if (entry.result != QLatin1String("success"))
                    continue;
                sawVisibilityAudit = true;
                QCOMPARE(params.keys(), QStringList{QStringLiteral("visible")});
            }
        }
        QVERIFY(sawCollapseAudit);
        QVERIFY(sawVisibilityAudit);
        QVERIFY(ProjectManager::instance().currentProject() == project);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        ProjectManager::instance().closeProject();
        delete runtimeModule.data();
    }

    void testMissingInspectorPanelFailsClosed() {
        AgentController& controller = AgentController::instance();
        MainWindow window;
        auto* inspector = window.findChild<ModuleInspectorPanel*>();
        QVERIFY(inspector);
        delete inspector;

        const QJsonObject unavailable = controller.executeDataSourceViewAction(
            QStringLiteral("set_inspector_pinned"), QJsonObject{{QStringLiteral("pinned"), true}});
        QCOMPARE(unavailable.value(QStringLiteral("errorCode")).toString(), QStringLiteral("GUI_UNAVAILABLE"));
        QCOMPARE(controller
                     .executeDataSourceViewAction(QStringLiteral("set_inspector_collapsed"),
                                                  QJsonObject{{QStringLiteral("collapsed"), true}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("GUI_UNAVAILABLE"));
        QCOMPARE(controller
                     .executeDataSourceViewAction(QStringLiteral("set_inspector_visibility"),
                                                  QJsonObject{{QStringLiteral("visible"), false}})
                     .value(QStringLiteral("errorCode"))
                     .toString(),
                 QStringLiteral("GUI_UNAVAILABLE"));
        QVERIFY(!ProjectManager::instance().currentProject());
    }

private:
    std::unique_ptr<QTemporaryDir> m_appDataDir;
};

QTEST_MAIN(TestAgentInspectorPin)
#include "test_agent_inspector_pin.moc"
