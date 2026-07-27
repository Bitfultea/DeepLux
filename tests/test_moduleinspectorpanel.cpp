#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/manager/PluginManager.h>
#include <core/model/ImageData.h>
#include <ui/widgets/ModuleInspectorPanel.h>

#include <QLabel>
#include <QSignalSpy>
#include <QTableWidget>
#include <QToolButton>

using namespace DeepLux;

class InspectorTestModule : public ModuleBase {
    Q_OBJECT

public:
    explicit InspectorTestModule(const QString& id = "inspector-test") {
        m_moduleId = id;
        m_name = QStringLiteral("Inspector Test Module");
        m_category = QStringLiteral("test");
        m_params = QJsonObject{
            {"param1", 1.5},
            {"param2", QStringLiteral("hello")},
        };
        m_defaultParams = m_params;
    }

    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        QImage img(8, 8, QImage::Format_RGB32);
        img.fill(Qt::green);
        output = ImageData(img);
        output.setData(QStringLiteral("area"), 42.5);
        output.setData(QStringLiteral("count"), 7);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class TestModuleInspectorPanel : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void testEmptyStateWhenNoSelection();
    void testSetModuleShowsNameAndIcon();
    void testSetOutputShowsResultsData();
    void testSetDirtyShowsRerunHint();
    void testSetPinnedPreventsSelectionSwitch();
    void testThemeSwitchDoesNotCrash();
    void testCloseSignalEmitted();
};

void TestModuleInspectorPanel::init() {}
void TestModuleInspectorPanel::cleanup() {}

void TestModuleInspectorPanel::testEmptyStateWhenNoSelection() {
    ModuleInspectorPanel panel;
    panel.show();
    QVERIFY(panel.currentInstanceId().isEmpty());
    QVERIFY(!panel.isPinned());

    // The empty state widget should exist and be visible
    QWidget* emptyState = panel.findChild<QWidget*>("InspectorEmptyState");
    QVERIFY(emptyState != nullptr);
    QVERIFY2(emptyState->isVisibleTo(&panel),
             "Empty state should be visible when no module is selected");
}

void TestModuleInspectorPanel::testSetModuleShowsNameAndIcon() {
    ModuleInspectorPanel panel;
    InspectorTestModule module;

    PluginInfo info;
    panel.setModule(&module, QStringLiteral("inst_1"), info);

    QCOMPARE(panel.currentInstanceId(), QString("inst_1"));

    // The module name should be displayed in the header
    QLabel* nameLabel = panel.findChild<QLabel*>("InspectorModuleName");
    QVERIFY(nameLabel != nullptr);
    QCOMPARE(nameLabel->text(), module.name());

    // Empty state should be hidden
    QWidget* emptyState = panel.findChild<QWidget*>("InspectorEmptyState");
    QVERIFY(emptyState != nullptr);
    QVERIFY(!emptyState->isVisible());
}

void TestModuleInspectorPanel::testSetOutputShowsResultsData() {
    ModuleInspectorPanel panel;
    InspectorTestModule module;

    PluginInfo info;
    // Build ui.results metadata
    QJsonObject resultsMeta;
    QJsonObject areaMeta;
    areaMeta["label"] = QStringLiteral("面积");
    areaMeta["unit"] = QStringLiteral("px");
    areaMeta["order"] = 1;
    resultsMeta["area"] = areaMeta;
    QJsonObject countMeta;
    countMeta["label"] = QStringLiteral("数量");
    countMeta["order"] = 2;
    resultsMeta["count"] = countMeta;
    QJsonObject uiObj;
    uiObj["results"] = resultsMeta;
    info.ui = uiObj;

    panel.setModule(&module, QStringLiteral("inst_1"), info);

    // Process the module to generate output
    ImageData input, output;
    module.process(input, output);
    QVERIFY(output.isValid());

    panel.setOutput(output, true, 15);

    // The results table should contain data
    QTableWidget* resultsTable = panel.findChild<QTableWidget*>("InspectorResultsTable");
    QVERIFY(resultsTable != nullptr);
    QVERIFY2(resultsTable->rowCount() >= 2,
             "Results table should contain at least 2 rows of output data");

    // Check status label shows success
    QLabel* statusLabel = panel.findChild<QLabel*>("InspectorStatus");
    QVERIFY(statusLabel != nullptr);
    QVERIFY(statusLabel->text().contains(QStringLiteral("成功")));
}

void TestModuleInspectorPanel::testSetDirtyShowsRerunHint() {
    ModuleInspectorPanel panel;
    InspectorTestModule module;

    PluginInfo info;
    panel.setModule(&module, QStringLiteral("inst_1"), info);

    // First set some output
    ImageData input, output;
    module.process(input, output);
    panel.setOutput(output, true, 10);

    // Mark dirty
    panel.setDirty(true);

    // Dirty state 应通过标题栏小色点指示（而非文字）
    QLabel* dirtyDot = panel.findChild<QLabel*>("InspectorDirtyDot");
    QVERIFY2(dirtyDot != nullptr, "Dirty state should expose a small color dot in the header");
    QVERIFY2(dirtyDot->isVisibleTo(&panel),
             "Dirty state dot should be visible when module is dirty");

    // Results table should NOT contain a hint row (no duplication)
    QTableWidget* resultsTable = panel.findChild<QTableWidget*>("InspectorResultsTable");
    QVERIFY(resultsTable != nullptr);

    bool foundHint = false;
    for (int i = 0; i < resultsTable->rowCount(); ++i) {
        QTableWidgetItem* item = resultsTable->item(i, 1);
        if (item && item->text().contains(QStringLiteral("重新运行"))) {
            foundHint = true;
            break;
        }
    }
    QVERIFY2(!foundHint, "Dirty state should not duplicate hint in results table");

    // Clear dirty state should hide the dot
    panel.setDirty(false);
    QVERIFY2(!dirtyDot->isVisibleTo(&panel),
             "Dirty state dot should be hidden when module is not dirty");
}

void TestModuleInspectorPanel::testSetPinnedPreventsSelectionSwitch() {
    ModuleInspectorPanel panel;
    InspectorTestModule module;

    PluginInfo info;
    panel.setModule(&module, QStringLiteral("inst_1"), info);

    // Pin the inspector
    panel.setPinned(true);
    QVERIFY(panel.isPinned());

    // The pin button should reflect the pinned state
    QToolButton* pinBtn = panel.findChild<QToolButton*>("InspectorPinBtn");
    QVERIFY(pinBtn != nullptr);
    QVERIFY(pinBtn->isChecked());
}

void TestModuleInspectorPanel::testThemeSwitchDoesNotCrash() {
    ModuleInspectorPanel panel;
    InspectorTestModule module;

    PluginInfo info;
    panel.setModule(&module, QStringLiteral("inst_1"), info);

    // Switch to dark theme
    panel.applyTheme(true);
    // Switch to light theme
    panel.applyTheme(false);
    // Should not crash
    QVERIFY(true);
}

void TestModuleInspectorPanel::testCloseSignalEmitted() {
    ModuleInspectorPanel panel;
    QSignalSpy spy(&panel, &ModuleInspectorPanel::closeRequested);

    QToolButton* closeBtn = panel.findChild<QToolButton*>("InspectorCloseBtn");
    QVERIFY(closeBtn != nullptr);

    QTest::mouseClick(closeBtn, Qt::LeftButton);

    QCOMPARE(spy.count(), 1);
}

QTEST_MAIN(TestModuleInspectorPanel)
#include "test_moduleinspectorpanel.moc"
