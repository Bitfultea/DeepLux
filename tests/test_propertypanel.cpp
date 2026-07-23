#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <ui/widgets/PropertyPanel.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QLineEdit>
#include <QMetaObject>

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

    QCOMPARE(module.currentParams()["textParam"].toString(), QString("changed"));
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

    QCOMPARE(module.currentParams()["numberParam"].toDouble(), 42.25);
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

    QCOMPARE(module.currentParams()["boolParam"].toBool(), true);
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

    QCOMPARE(module.currentParams()["mode"].toString(), QString("Manual"));
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
    QCOMPARE(panel.findChildren<QGroupBox*>().size(), 2);

    panel.setModule(&module);
    QCOMPARE(panel.findChildren<QGroupBox*>().size(), 2);

    panel.clear();
    QCOMPARE(panel.findChildren<QGroupBox*>().size(), 0);
    QVERIFY(panel.currentModuleId().isEmpty());
}

QTEST_MAIN(TestPropertyPanel)
#include "test_propertypanel.moc"
