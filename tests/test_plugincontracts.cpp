#include <QtTest/QtTest>

#include <core/interface/IModule.h>
#include <core/manager/PluginManager.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QWidget>

using namespace DeepLux;

class TestPluginContracts : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void testRepresentativePluginsHaveIndependentParamPreservingInstances();

private:
    struct PluginFixture {
        QString installDir;
        QString moduleName;
        QString metadataRelPath;
        QString libraryName;
    };

    bool installPlugin(const QString& pluginRoot, const PluginFixture& fixture) const;
    QVariant replacementValueFor(const QString& key, const QJsonValue& value) const;
};

void TestPluginContracts::init() {
    PluginManager::instance().shutdown();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

void TestPluginContracts::cleanup() {
    PluginManager::instance().shutdown();
    qunsetenv("DEEPLUX_APP_DATA_DIR");
}

bool TestPluginContracts::installPlugin(const QString& pluginRoot, const PluginFixture& fixture) const {
    QDir root(pluginRoot);
    if (!root.mkpath(fixture.installDir)) {
        return false;
    }

    QDir pluginDir(root.filePath(fixture.installDir));
    const QString metadataSrc =
        QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../" + fixture.metadataRelPath);
    const QString libSrc = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib/" + fixture.libraryName);

    if (!QFileInfo::exists(metadataSrc) || !QFileInfo::exists(libSrc)) {
        return false;
    }

    QFile::remove(pluginDir.filePath("metadata.json"));
    QFile::remove(pluginDir.filePath(fixture.libraryName));

    return QFile::copy(metadataSrc, pluginDir.filePath("metadata.json")) &&
           QFile::copy(libSrc, pluginDir.filePath(fixture.libraryName));
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
        {"FitLine", "FitLine", "src/plugins/geometry/FitLine/metadata.json", "libFitLinePlugin.so"},
        {"FitCircle", "FitCircle", "src/plugins/geometry/FitCircle/metadata.json", "libFitCirclePlugin.so"},
        {"MeasureLine", "MeasureLine", "src/plugins/detection/MeasureLine/metadata.json", "libMeasureLinePlugin.so"},
        {"DistancePP", "DistancePP", "src/plugins/geometry/DistancePP/metadata.json", "libDistancePPPlugin.so"},
        {"SystemTime", "SystemTime", "src/plugins/system/SystemTime/metadata.json", "libSystemTimePlugin.so"},
        {"Blob", "Blob", "src/plugins/image_processing/Blob/metadata.json", "libBlobPlugin.so"},
        {"PerProcessing", "PerProcessing", "src/plugins/image_processing/PerProcessing/metadata.json", "libPerProcessingPlugin.so"},
        {"Matching", "Matching", "src/plugins/detection/Matching/metadata.json", "libMatchingPlugin.so"},
        {"MeasureRect", "MeasureRect", "src/plugins/detection/MeasureRect/metadata.json", "libMeasureRectPlugin.so"},
        {"Folder", "Folder", "src/plugins/system/Folder/metadata.json", "libFolderPlugin.so"},
        {"SaveData", "SaveData", "src/plugins/system/SaveData/metadata.json", "libSaveDataPlugin.so"},
        {"CreateString", "创建字符串", "src/plugins/variable/CreateString/metadata.json", "libCreateStringPlugin.so"},
        {"SplitString", "分割字符串", "src/plugins/variable/SplitString/metadata.json", "libSplitStringPlugin.so"},
        {"MathPlugin", "数学运算", "src/plugins/variable/MathPlugin/metadata.json", "libMathPlugin.so"},
    };

    for (const PluginFixture& fixture : fixtures) {
        QVERIFY2(installPlugin(pluginRoot, fixture), qPrintable(QString("install %1").arg(fixture.installDir)));
    }

    PluginManager::instance().addPluginPath(pluginRoot);
    QVERIFY(PluginManager::instance().initialize());

    for (const PluginFixture& fixture : fixtures) {
        QVERIFY2(PluginManager::instance().loadPlugin(fixture.moduleName),
                 qPrintable(QString("load %1").arg(fixture.moduleName)));

        IModule* first = PluginManager::instance().createModule(fixture.moduleName);
        IModule* second = PluginManager::instance().createModule(fixture.moduleName);
        QVERIFY2(first != nullptr, qPrintable(QString("create first %1").arg(fixture.moduleName)));
        QVERIFY2(second != nullptr, qPrintable(QString("create second %1").arg(fixture.moduleName)));
        QVERIFY2(first != second, qPrintable(QString("%1 returned shared module instance").arg(fixture.moduleName)));

        QVERIFY2(!first->moduleId().isEmpty(), qPrintable(fixture.moduleName));
        QVERIFY2(!first->name().isEmpty(), qPrintable(fixture.moduleName));
        QVERIFY2(!first->category().isEmpty(), qPrintable(fixture.moduleName));
        QCOMPARE(first->interfaceVersion(), DEEPLUX_MODULE_INTERFACE_VERSION);

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
            QVERIFY2(cloned != first, qPrintable(QString("%1 clone returned self").arg(fixture.moduleName)));
            QCOMPARE(cloned->currentParams().value(key).toVariant(), replacement);
            QCOMPARE(second->currentParams().value(key).toVariant(), defaults.value(key).toVariant());
            delete cloned;
        }

        delete first;
        delete second;
    }
}

QTEST_MAIN(TestPluginContracts)
#include "test_plugincontracts.moc"
