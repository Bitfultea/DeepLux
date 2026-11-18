#include <QAction>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QToolButton>
#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/geometry/MeasurementData.h>
#include <core/manager/PluginManager.h>
#include <core/model/ImageData.h>
#include <ui/ThemeManager.h>
#include <ui/widgets/ModuleInspectorPanel.h>

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
    void testGrabImageResultsShowImageAndPointCloudInfo();
    void testFailureWithoutImageShowsReadableError();
    void testSetDirtyShowsRerunHint();
    void testSetModuleClearsDirtyState();
    void testCollapsedEmptyStateStaysHidden();
    void testSetPinnedPreventsSelectionSwitch();
    void testThemeSwitchDoesNotCrash();
    void testCloseSignalEmitted();
    void testPrimaryAndOverflowActions();
    void testRerunButtonFitsScaledText();
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
    QVERIFY2(emptyState->isVisibleTo(&panel), "Empty state should be visible when no module is selected");
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

    QLabel* redundantTitle = panel.findChild<QLabel*>("PropertyPanelTitle");
    QVERIFY2(redundantTitle == nullptr, "Inspector header should be the single module title");
    QToolButton* infoToggle = panel.findChild<QToolButton*>("ModuleInfoToggle");
    QWidget* infoContent = panel.findChild<QWidget*>("ModuleInfoContent");
    QVERIFY(infoToggle != nullptr);
    QVERIFY(infoContent != nullptr);
    QVERIFY2(infoContent->isHidden(), "Technical module metadata should be collapsed by default");
    infoToggle->setChecked(true);
    QVERIFY2(!infoContent->isHidden(), "Technical module metadata should remain available on demand");
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
    QVERIFY2(resultsTable->rowCount() >= 2, "Results table should contain at least 2 rows of output data");

    // Check status label shows success
    QLabel* statusLabel = panel.findChild<QLabel*>("InspectorStatus");
    QVERIFY(statusLabel != nullptr);
    QVERIFY(statusLabel->text().contains(QStringLiteral("成功")));
}

void TestModuleInspectorPanel::testGrabImageResultsShowImageAndPointCloudInfo() {
    ModuleInspectorPanel panel;
    InspectorTestModule module(QStringLiteral("com.deeplux.plugin.grabimage"));
    PluginInfo info;
    info.id = module.moduleId();
    panel.setModule(&module, QStringLiteral("grab_1"), info);

    ImageData output(QImage(640, 480, QImage::Format_RGB888));
    panel.setOutput(output, true, 15);

    auto* table = panel.findChild<QTableWidget*>(QStringLiteral("InspectorResultsTable"));
    QVERIFY(table != nullptr);
    const auto valueFor = [table](const QString& label) {
        for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 0)->text() == label)
                return table->item(row, 1)->text();
        }
        return QString();
    };
    QCOMPARE(valueFor(QStringLiteral("图像尺寸")), QStringLiteral("640 × 480"));
    QCOMPARE(valueFor(QStringLiteral("通道数")), QStringLiteral("3"));

    PointCloudData cloud;
    cloud.points = {Eigen::Vector3d(-1.0, 2.0, 0.0), Eigen::Vector3d(3.0, -4.0, 5.0), Eigen::Vector3d(0.0, 1.0, -2.0)};
    MeasurementData::setPointCloud(output, cloud);
    panel.setOutput(output, true, 15);
    QCOMPARE(valueFor(QStringLiteral("3D 点数")), QStringLiteral("3"));
    QCOMPARE(valueFor(QStringLiteral("3D AABB 最小值")), QStringLiteral("(-1, -4, -2)"));
    QCOMPARE(valueFor(QStringLiteral("3D AABB 最大值")), QStringLiteral("(3, 2, 5)"));
}

void TestModuleInspectorPanel::testFailureWithoutImageShowsReadableError() {
    ModuleInspectorPanel panel;
    InspectorTestModule module;
    panel.setModule(&module, QStringLiteral("inst_1"), PluginInfo{});

    panel.setOutput(ImageData(), false, 7);

    auto* statusLabel = panel.findChild<QLabel*>("InspectorStatus");
    auto* resultsTable = panel.findChild<QTableWidget*>("InspectorResultsTable");
    QVERIFY(statusLabel != nullptr);
    QVERIFY(resultsTable != nullptr);
    QVERIFY(statusLabel->text().contains(QStringLiteral("失败")));
    QCOMPARE(resultsTable->rowCount(), 1);
    QVERIFY(resultsTable->wordWrap());
    QVERIFY(!resultsTable->item(0, 1)->text().isEmpty());
    QCOMPARE(resultsTable->item(0, 1)->toolTip(), resultsTable->item(0, 1)->text());
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
    QVERIFY2(dirtyDot->isVisibleTo(&panel), "Dirty state dot should be visible when module is dirty");

    // Results table should NOT contain a hint row (no duplication)
    QTableWidget* resultsTable = panel.findChild<QTableWidget*>("InspectorResultsTable");
    QVERIFY(resultsTable != nullptr);
    QCOMPARE(resultsTable->rowCount(), 0);

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
    QVERIFY2(!dirtyDot->isVisibleTo(&panel), "Dirty state dot should be hidden when module is not dirty");
}

void TestModuleInspectorPanel::testSetModuleClearsDirtyState() {
    ModuleInspectorPanel panel;
    InspectorTestModule first(QStringLiteral("first"));
    InspectorTestModule second(QStringLiteral("second"));
    auto* dirtyDot = panel.findChild<QLabel*>("InspectorDirtyDot");
    QVERIFY(dirtyDot != nullptr);

    panel.setModule(&first, QStringLiteral("first_1"), PluginInfo{});
    panel.setDirty(true);
    QVERIFY(!dirtyDot->isHidden());

    panel.setModule(&second, QStringLiteral("second_1"), PluginInfo{});
    QVERIFY(dirtyDot->isHidden());
}

void TestModuleInspectorPanel::testCollapsedEmptyStateStaysHidden() {
    ModuleInspectorPanel panel;
    auto* emptyState = panel.findChild<QWidget*>("InspectorEmptyState");
    QVERIFY(emptyState != nullptr);

    panel.clear();
    panel.setLayoutMode(ModuleInspectorPanel::LayoutMode::Collapsed);
    QVERIFY(emptyState->isHidden());

    panel.setLayoutMode(ModuleInspectorPanel::LayoutMode::Docked);
    QVERIFY(!emptyState->isHidden());
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

void TestModuleInspectorPanel::testPrimaryAndOverflowActions() {
    ModuleInspectorPanel panel;
    InspectorTestModule module;
    panel.setModule(&module, QStringLiteral("inst_1"), PluginInfo{});

    auto* rerunButton = panel.findChild<QPushButton*>("InspectorRerunBtn");
    auto* moreButton = panel.findChild<QToolButton*>("InspectorMoreBtn");
    QAction* advancedAction = panel.findChild<QAction*>("InspectorAdvancedAction");
    QAction* resetAction = panel.findChild<QAction*>("InspectorResetAction");
    QVERIFY2(rerunButton != nullptr, "Rerun should remain the inspector's visible primary action");
    QVERIFY2(moreButton != nullptr && moreButton->menu() != nullptr,
             "Secondary inspector actions should live in one overflow menu");
    QVERIFY(advancedAction != nullptr);
    QVERIFY(resetAction != nullptr);

    QSignalSpy advancedSpy(&panel, &ModuleInspectorPanel::advancedConfigRequested);
    QSignalSpy resetSpy(&panel, &ModuleInspectorPanel::resetDefaultsRequested);
    advancedAction->trigger();
    resetAction->trigger();
    QCOMPARE(advancedSpy.count(), 1);
    QCOMPARE(resetSpy.count(), 1);
}

void TestModuleInspectorPanel::testRerunButtonFitsScaledText() {
    ModuleInspectorPanel panel;
    panel.setStyleSheet(ThemeManager::styleSheet(false));
    panel.setLayoutMode(ModuleInspectorPanel::LayoutMode::Floating);
    panel.resize(285, 500);
    InspectorTestModule module;
    panel.setModule(&module, QStringLiteral("scaled-text"), PluginInfo{});
    panel.show();

    auto* rerunButton = panel.findChild<QPushButton*>("InspectorRerunBtn");
    QVERIFY(rerunButton != nullptr);
    QFont scaledFont = rerunButton->font();
    scaledFont.setPixelSize(24);
    rerunButton->setFont(scaledFont);
    QCoreApplication::processEvents();

    QVERIFY(rerunButton->width() >= rerunButton->fontMetrics().horizontalAdvance(rerunButton->text()) + 24);
    const QWidget* bottomBar = panel.findChild<QWidget*>("InspectorBottomBar");
    QVERIFY(bottomBar != nullptr);
    for (QWidget* child : bottomBar->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (child->isVisible())
            QVERIFY(child->geometry().right() <= bottomBar->contentsRect().right());
    }

    panel.resize(500, 500);
    QCoreApplication::processEvents();
    auto* noteEdit = panel.findChild<QLineEdit*>("InspectorNoteEdit");
    QVERIFY(noteEdit != nullptr);
    QVERIFY(rerunButton->geometry().top() < noteEdit->geometry().bottom());
    QVERIFY(noteEdit->geometry().top() < rerunButton->geometry().bottom());

    panel.resize(285, 500);
    QCoreApplication::processEvents();
    QVERIFY(noteEdit->geometry().top() >= rerunButton->geometry().bottom());
}

QTEST_MAIN(TestModuleInspectorPanel)
#include "test_moduleinspectorpanel.moc"
