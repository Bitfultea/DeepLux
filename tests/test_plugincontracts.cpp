#include "TestPluginRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QWidget>
#include <QtTest/QtTest>
#include <core/deeplux/DataContract.h>
#include <core/interface/IModule.h>
#include <core/manager/PluginManager.h>
#include <limits>

using namespace DeepLux;

class TestPluginContracts : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void testRepresentativePluginsHaveIndependentParamPreservingInstances();
    void testEllipse2DDataContractIsStrict();

private:
    struct PluginFixture {
        QString moduleName;
        QString targetName;
    };

    bool installPlugin(const QString& pluginRoot, const PluginFixture& fixture) const;
    QVariant replacementValueFor(const QString& key, const QJsonValue& value) const;
};

void TestPluginContracts::testEllipse2DDataContractIsStrict() {
    QVERIFY(isSupportedPortType(DataType::Ellipse2D));
    QVERIFY(portValueMatchesType(QVariant::fromValue(Ellipse2D{25.0, 25.0, 12.0, 7.0, 0.0}), DataType::Ellipse2D));
    for (const Ellipse2D invalid :
         {Ellipse2D{0, 0, 0, 0, 0}, Ellipse2D{0, 0, -1, -2, 0}, Ellipse2D{0, 0, 7, 12, 0}, Ellipse2D{0, 0, 12, 7, 180},
          Ellipse2D{std::numeric_limits<double>::quiet_NaN(), 0, 12, 7, 0},
          Ellipse2D{0, std::numeric_limits<double>::infinity(), 12, 7, 0},
          Ellipse2D{0, 0, std::numeric_limits<double>::quiet_NaN(), 7, 0},
          Ellipse2D{0, 0, 12, std::numeric_limits<double>::infinity(), 0},
          Ellipse2D{0, 0, 12, 7, std::numeric_limits<double>::quiet_NaN()},
          Ellipse2D{0, 0, 12, 7, std::numeric_limits<double>::infinity()}}) {
        QVERIFY(!portValueMatchesType(QVariant::fromValue(invalid), DataType::Ellipse2D));
    }
    QVERIFY(!portValueMatchesType(QVariant(QVariantList{25.0, 25.0, 12.0, 7.0, 0.0}), DataType::Ellipse2D));
    QVERIFY(!portValueMatchesType(
        QVariant(QVariantMap{
            {"centerX", 25.0}, {"centerY", 25.0}, {"majorRadius", 12.0}, {"minorRadius", 7.0}, {"angleDegrees", 0.0}}),
        DataType::Ellipse2D));
}

void TestPluginContracts::init() {
    PluginManager::instance().shutdown();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

void TestPluginContracts::cleanup() {
    PluginManager::instance().shutdown();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

bool TestPluginContracts::installPlugin(const QString& pluginRoot, const PluginFixture& fixture) const {
    const DeepLuxTest::PluginEntry entry = DeepLuxTest::pluginEntry(fixture.targetName);
    if (entry.libraryPath.isEmpty() || entry.metadataPath.isEmpty()) {
        return false;
    }

    const QString dirName = DeepLuxTest::pluginInstallDir(entry);
    QDir root(pluginRoot);
    if (!root.mkpath(dirName)) {
        return false;
    }

    QDir pluginDir(root.filePath(dirName));
    const QString libName = QFileInfo(entry.libraryPath).fileName();
    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath(libName));

    return QFile::copy(entry.metadataPath, pluginDir.filePath("metadata.json")) &&
           QFile::copy(entry.libraryPath, pluginDir.filePath(libName));
}

QVariant TestPluginContracts::replacementValueFor(const QString& key, const QJsonValue& value) const {
    if (key == "fitMethod") {
        return QString("LS");
    }
    if (key == "operation") {
        return QString("Subtract");
    }
    if (key == "stringSource") {
        return QString("Input");
    }
    if (key == "maxAngle") {
        return 120.0;
    }
    if (key == "matchThreshold") {
        return 0.65;
    }

    if (value.isString()) {
        return value.toString() + "_contract";
    }
    if (value.isBool()) {
        return !value.toBool();
    }
    if (value.isDouble()) {
        return value.toDouble() + 17.0;
    }
    return QString("contract");
}

void TestPluginContracts::testRepresentativePluginsHaveIndependentParamPreservingInstances() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    qputenv("DEEPLUX_APP_DATA_DIR", dir.filePath("appdata").toLocal8Bit());
    const QString pluginRoot = dir.filePath("plugins");

    const QList<PluginFixture> fixtures = {
        {"FitLine", "FitLinePlugin"},
        {"FitCircle", "FitCirclePlugin"},
        {"MeasureLine", "MeasureLinePlugin"},
        {"DistancePP", "DistancePPPlugin"},
        {"PointSurfaceDistance", "PointSurfaceDistancePlugin"},
        {"MeasurementInput", "MeasurementInputPlugin"},
        {"循环", "LoopPlugin"},
        {"Parallel", "ParallelPlugin"},
        {"SystemTime", "SystemTimePlugin"},
        {"Blob", "BlobPlugin"},
        {"PerProcessing", "PerProcessingPlugin"},
        {"Matching", "MatchingPlugin"},
        {"MeasureRect", "MeasureRectPlugin"},
        {"Folder", "FolderPlugin"},
        {"SaveData", "SaveDataPlugin"},
        {"创建字符串", "CreateStringPlugin"},
        {"分割字符串", "SplitStringPlugin"},
        {"数学运算", "MathPlugin"},
    };

    for (const PluginFixture& fixture : fixtures) {
        QVERIFY2(installPlugin(pluginRoot, fixture), qPrintable(QString("install %1").arg(fixture.targetName)));
    }

    PluginManager::instance().addPluginPath(pluginRoot);
    QVERIFY(PluginManager::instance().initialize());

    for (const PluginFixture& fixture : fixtures) {
        QVERIFY2(PluginManager::instance().loadPlugin(fixture.moduleName),
                 qPrintable(QString("load %1").arg(fixture.moduleName)));

        auto first = PluginManager::instance().createManagedModule(fixture.moduleName);
        auto second = PluginManager::instance().createManagedModule(fixture.moduleName);
        QVERIFY2(first != nullptr, qPrintable(QString("create first %1").arg(fixture.moduleName)));
        QVERIFY2(second != nullptr, qPrintable(QString("create second %1").arg(fixture.moduleName)));
        QVERIFY2(first.get() != second.get(),
                 qPrintable(QString("%1 returned shared module instance").arg(fixture.moduleName)));

        QVERIFY2(!first->moduleId().isEmpty(), qPrintable(fixture.moduleName));
        QVERIFY2(!first->name().isEmpty(), qPrintable(fixture.moduleName));
        QVERIFY2(!first->category().isEmpty(), qPrintable(fixture.moduleName));
        QCOMPARE(first->interfaceVersion(), DEEPLUX_MODULE_INTERFACE_VERSION);
        if (fixture.moduleName == QLatin1String("Parallel")) {
            QCOMPARE(first->flowControlType(), ControlFlowType::Parallel);
            QCOMPARE(first->inputPorts().size(), 2);
            QCOMPARE(first->outputPorts().size(), 2);
        }

        QString error;
        QVERIFY2(first->validateParams(first->currentParams(), error),
                 qPrintable(QString("%1 params invalid: %2").arg(fixture.moduleName, error)));

        if (!first->currentParams().isEmpty()) {
            QWidget* configWidget = first->createConfigWidget();
            QVERIFY2(configWidget != nullptr, qPrintable(QString("%1 missing config widget").arg(fixture.moduleName)));
            delete configWidget;

            const QJsonObject defaults = first->currentParams();
            const QString key = defaults.keys().first();
            const QVariant replacement = replacementValueFor(key, defaults.value(key));
            first->setParam(key, replacement);

            IModule* cloned = first->clone();
            QVERIFY2(cloned != nullptr, qPrintable(QString("%1 clone returned null").arg(fixture.moduleName)));
            QVERIFY2(cloned != first.get(), qPrintable(QString("%1 clone returned self").arg(fixture.moduleName)));
            QCOMPARE(cloned->currentParams().value(key).toVariant(), replacement);
            QCOMPARE(second->currentParams().value(key).toVariant(), defaults.value(key).toVariant());
            delete cloned;
        }
    }
}

QTEST_MAIN(TestPluginContracts)
#include "test_plugincontracts.moc"
