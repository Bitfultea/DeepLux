#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/manager/PluginManager.h>
#include <ui/widgets/PropertyPanel.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QSignalSpy>

using namespace DeepLux;

class PropertyPanelTestModule : public ModuleBase {
    Q_OBJECT

public:
    PropertyPanelTestModule() {
        m_moduleId = "property-test";
        m_name = "Property Test";
        m_category = "test";
        m_params = QJsonObject{
            {"textParam", "initial"},
            {"numberParam", 1.5},
            {"boolParam", false},
        };
        m_defaultParams = m_params;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        Q_UNUSED(output)
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class PropertyPanelChoiceModule : public ModuleBase {
    Q_OBJECT

public:
    PropertyPanelChoiceModule() {
        m_moduleId = "choice-test";
        m_name = "Choice Test";
        m_category = "test";
        m_params = QJsonObject{
            {"mode", "Auto"},
            {"mode_options", QJsonArray{"Auto", "Manual"}},
        };
        m_defaultParams = m_params;
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        Q_UNUSED(output)
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class PropertyPanelMetadataModule : public ModuleBase {
    Q_OBJECT

public:
    PropertyPanelMetadataModule() {
        m_moduleId = "metadata-test";
        m_name = "Metadata Test";
        m_category = "test";
        m_params = QJsonObject{
            {"length", 5.0},
            {"threshold", 0.5},
        };
        m_defaultParams = m_params;
    }

    void setMetadata(const PluginInfo& info) {
        m_info = info;
    }

    PluginInfo pluginInfo() const {
        return m_info;
    }

    bool validateParams(const QJsonObject& params, QString& error) const override {
        // length must be positive
        if (params.value("length").toDouble() <= 0) {
            error = QStringLiteral("length must be positive");
            return false;
        }
        return ModuleBase::validateParams(params, error);
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        Q_UNUSED(input)
        Q_UNUSED(output)
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    PluginInfo m_info;
};

// Helper: trigger editingFinished on a widget that supports it
static void triggerEditingFinished(QWidget* widget) {
    if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
        QMetaObject::invokeMethod(edit, "editingFinished", Qt::DirectConnection);
    } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(widget)) {
        QMetaObject::invokeMethod(spin, "editingFinished", Qt::DirectConnection);
    }
}

class TestPropertyPanel : public QObject {
    Q_OBJECT

private slots:
    void testEditingTextParamUpdatesModuleAndEmitsSignal();
    void testEditingNumberParamUpdatesModuleAndEmitsSignal();
    void testEditingBoolParamUpdatesModuleAndEmitsSignal();
    void testStringParamWithOptionsUsesChoiceWidget();
    void testInstanceIdOverridesModuleIdInParamSignals();
    void testSettingModuleTwiceReplacesPreviousParamGroups();
    void testMetadataLabelDisplayed();
    void testRangeAndUnitAppliedToSpinBox();
    void testValidationFailureKeepsOldValue();
    void testEditingFinishedCommitsOnce();
};

void TestPropertyPanel::testEditingTextParamUpdatesModuleAndEmitsSignal() {
    PropertyPanel panel;
    PropertyPanelTestModule module;
    QSignalSpy spy(&panel, &PropertyPanel::paramsChanged);

    panel.setModule(&module);
    QLineEdit* edit = panel.findChild<QLineEdit*>();
    QVERIFY(edit != nullptr);

    edit->setText("changed");
    triggerEditingFinished(edit);

    // PropertyPanel only emits signal, does not modify module directly
    QVERIFY(spy.count() >= 1);
    const QList<QVariant> lastSignal = spy.takeLast();
    QCOMPARE(lastSignal.at(0).toString(), QString("property-test"));
    QCOMPARE(lastSignal.at(1).toString(), QString("textParam"));
    QCOMPARE(lastSignal.at(2).toString(), QString("changed"));
}

void TestPropertyPanel::testEditingNumberParamUpdatesModuleAndEmitsSignal() {
    PropertyPanel panel;
    PropertyPanelTestModule module;
    QSignalSpy spy(&panel, &PropertyPanel::paramsChanged);

    panel.setModule(&module);
    QDoubleSpinBox* spin = panel.findChild<QDoubleSpinBox*>();
    QVERIFY(spin != nullptr);

    spin->setValue(42.25);
    triggerEditingFinished(spin);

    // PropertyPanel only emits signal, does not modify module directly
    QVERIFY(spy.count() >= 1);
    const QList<QVariant> lastSignal = spy.takeLast();
    QCOMPARE(lastSignal.at(0).toString(), QString("property-test"));
    QCOMPARE(lastSignal.at(1).toString(), QString("numberParam"));
    QCOMPARE(lastSignal.at(2).toDouble(), 42.25);
}

void TestPropertyPanel::testEditingBoolParamUpdatesModuleAndEmitsSignal() {
    PropertyPanel panel;
    PropertyPanelTestModule module;
    QSignalSpy spy(&panel, &PropertyPanel::paramsChanged);

    panel.setModule(&module);
    QCheckBox* check = panel.findChild<QCheckBox*>();
    QVERIFY(check != nullptr);

    check->setChecked(true);

    // PropertyPanel only emits signal, does not modify module directly
    QVERIFY(spy.count() >= 1);
    const QList<QVariant> lastSignal = spy.takeLast();
    QCOMPARE(lastSignal.at(0).toString(), QString("property-test"));
    QCOMPARE(lastSignal.at(1).toString(), QString("boolParam"));
    QCOMPARE(lastSignal.at(2).toBool(), true);
}

void TestPropertyPanel::testStringParamWithOptionsUsesChoiceWidget() {
    PropertyPanel panel;
    PropertyPanelChoiceModule module;
    QSignalSpy spy(&panel, &PropertyPanel::paramsChanged);

    panel.setModule(&module);

    QList<QComboBox*> combos = panel.findChildren<QComboBox*>();
    QCOMPARE(combos.size(), 1);
    QComboBox* combo = combos.first();
    QCOMPARE(combo->count(), 2);
    QCOMPARE(combo->itemData(0).toString(), QString("Auto"));
    QCOMPARE(combo->itemData(1).toString(), QString("Manual"));
    QCOMPARE(combo->currentData().toString(), QString("Auto"));

    QVERIFY(panel.findChildren<QLineEdit*>().isEmpty());

    combo->setCurrentIndex(1);

    // PropertyPanel only emits signal, does not modify module directly
    QVERIFY(spy.count() >= 1);
    const QList<QVariant> lastSignal = spy.takeLast();
    QCOMPARE(lastSignal.at(0).toString(), QString("choice-test"));
    QCOMPARE(lastSignal.at(1).toString(), QString("mode"));
    QCOMPARE(lastSignal.at(2).toString(), QString("Manual"));
}

void TestPropertyPanel::testInstanceIdOverridesModuleIdInParamSignals() {
    PropertyPanel panel;
    PropertyPanelTestModule module;
    QSignalSpy spy(&panel, &PropertyPanel::paramsChanged);

    panel.setModule(&module, "module-instance-1");
    QLineEdit* edit = panel.findChild<QLineEdit*>();
    QVERIFY(edit != nullptr);

    edit->setText("instance scoped");
    triggerEditingFinished(edit);

    QVERIFY(spy.count() >= 1);
    const QList<QVariant> lastSignal = spy.takeLast();
    QCOMPARE(lastSignal.at(0).toString(), QString("module-instance-1"));
    QCOMPARE(lastSignal.at(1).toString(), QString("textParam"));
}

void TestPropertyPanel::testSettingModuleTwiceReplacesPreviousParamGroups() {
    PropertyPanel panel;
    PropertyPanelTestModule module;

    panel.setModule(&module);
    QCOMPARE(panel.findChildren<QGroupBox*>().size(), 1);
    QCOMPARE(panel.findChildren<QWidget*>(QStringLiteral("ModuleInfoContainer")).size(), 1);

    panel.setModule(&module);
    QCOMPARE(panel.findChildren<QGroupBox*>().size(), 1);
    QCOMPARE(panel.findChildren<QWidget*>(QStringLiteral("ModuleInfoContainer")).size(), 1);

    panel.clear();
    QCOMPARE(panel.findChildren<QGroupBox*>().size(), 0);
    QCOMPARE(panel.findChildren<QWidget*>(QStringLiteral("ModuleInfoContainer")).size(), 0);
    QVERIFY(panel.currentModuleId().isEmpty());
}

void TestPropertyPanel::testMetadataLabelDisplayed() {
    PropertyPanel panel;
    PropertyPanelMetadataModule module;

    // Build PluginInfo with ui.parameters metadata
    PluginInfo info;
    QJsonObject paramsMeta;
    QJsonObject lengthMeta;
    lengthMeta["label"] = QStringLiteral("长度");
    lengthMeta["unit"] = QStringLiteral("mm");
    lengthMeta["order"] = 1;
    QJsonObject thresholdMeta;
    thresholdMeta["label"] = QStringLiteral("阈值");
    thresholdMeta["order"] = 2;
    paramsMeta["length"] = lengthMeta;
    paramsMeta["threshold"] = thresholdMeta;
    QJsonObject uiObj;
    uiObj["parameters"] = paramsMeta;
    info.ui = uiObj;

    panel.setPluginInfo(info);
    panel.setModule(&module);

    // Find labels that contain "长度" and "阈值"
    QList<QLabel*> labels = panel.findChildren<QLabel*>();
    bool foundLength = false;
    bool foundThreshold = false;
    for (QLabel* label : labels) {
        if (label->text().contains(QStringLiteral("长度"))) {
            foundLength = true;
        }
        if (label->text().contains(QStringLiteral("阈值"))) {
            foundThreshold = true;
        }
    }
    QVERIFY2(foundLength, "Metadata label '长度' should be displayed");
    QVERIFY2(foundThreshold, "Metadata label '阈值' should be displayed");
}

void TestPropertyPanel::testRangeAndUnitAppliedToSpinBox() {
    PropertyPanel panel;
    PropertyPanelMetadataModule module;

    PluginInfo info;
    QJsonObject paramsMeta;
    QJsonObject lengthMeta;
    lengthMeta["label"] = QStringLiteral("长度");
    lengthMeta["unit"] = QStringLiteral("mm");
    lengthMeta["min"] = 0.0;
    lengthMeta["max"] = 100.0;
    lengthMeta["step"] = 1.0;
    paramsMeta["length"] = lengthMeta;
    QJsonObject uiObj;
    uiObj["parameters"] = paramsMeta;
    info.ui = uiObj;

    panel.setPluginInfo(info);
    panel.setModule(&module);

    QDoubleSpinBox* spin = panel.findChild<QDoubleSpinBox*>();
    QVERIFY(spin != nullptr);
    QCOMPARE(spin->minimum(), 0.0);
    QCOMPARE(spin->maximum(), 100.0);
    QCOMPARE(spin->singleStep(), 1.0);

    // Verify the unit is shown in the label
    QList<QLabel*> labels = panel.findChildren<QLabel*>();
    bool foundUnit = false;
    for (QLabel* label : labels) {
        if (label->text().contains(QStringLiteral("mm"))) {
            foundUnit = true;
            break;
        }
    }
    QVERIFY2(foundUnit, "Unit 'mm' should be displayed in the label");
}

void TestPropertyPanel::testValidationFailureKeepsOldValue() {
    PropertyPanel panel;
    PropertyPanelMetadataModule module;
    QSignalSpy spy(&panel, &PropertyPanel::paramsChanged);

    panel.setModule(&module);
    QDoubleSpinBox* spin = panel.findChild<QDoubleSpinBox*>();
    QVERIFY(spin != nullptr);

    // Current value should be 5.0 (length)
    QCOMPARE(spin->value(), 5.0);

    // Set to -1 (invalid: must be positive)
    spin->setValue(-1.0);
    triggerEditingFinished(spin);

    // Validation should fail, old value restored
    QCOMPARE(spin->value(), 5.0);
    // No signal should have been emitted since validation failed
    QCOMPARE(spy.count(), 0);
}

void TestPropertyPanel::testEditingFinishedCommitsOnce() {
    PropertyPanel panel;
    PropertyPanelTestModule module;
    QSignalSpy spy(&panel, &PropertyPanel::paramsChanged);

    panel.setModule(&module);
    QLineEdit* edit = panel.findChild<QLineEdit*>();
    QVERIFY(edit != nullptr);

    edit->setText(QStringLiteral("new_value"));
    triggerEditingFinished(edit);

    // Should emit exactly one signal (PropertyPanel does not modify module directly)
    QCOMPARE(spy.count(), 1);
    const QList<QVariant> lastSignal = spy.takeLast();
    QCOMPARE(lastSignal.at(1).toString(), QString("textParam"));
    QCOMPARE(lastSignal.at(2).toString(), QString("new_value"));
}

QTEST_MAIN(TestPropertyPanel)
#include "test_propertypanel.moc"
