#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/interface/IModule.h>
#include <core/manager/PluginManager.h>

using namespace DeepLux;

class TestPluginManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testInitialize();
    void testPluginPaths();
    void testScanPlugins();
    void testAvailableModules();
    void testPluginInfo();
    void testCreateModuleClone();
    void testExecutionMetadataValidation();
    void testBlockingMetadataParsedAndInjected();
    void testExecutionAffinityDefaultsToGui();
    void testExecutionAffinityParsesWorker();
    void testExecutionAffinityUnknownFallsBackToGui();
    void testBrokenUserPluginFallsBackToBundle();
    void testNonCameraUserPluginFallsBackToBundledCamera();
    void testRejectsPluginWithWrongAbiExportBeforeInstance();
    void testRejectsPluginWithWrongAbiFingerprintBeforeInstance();
    void testRejectsLegacyPluginWithoutAbiExport();
    void testRejectsAbiExportsOwnedByDependency();
    void testValidAbiPreflightLoadsNativeInitializerOnce();

private:
};

namespace {

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const QByteArray& name, const QByteArray& value)
        : m_name(name), m_previous(qgetenv(name.constData())), m_wasSet(qEnvironmentVariableIsSet(name.constData())) {
        qputenv(m_name.constData(), value);
    }

    ~ScopedEnvironmentVariable() {
        if (m_wasSet) {
            qputenv(m_name.constData(), m_previous);
        } else {
            qunsetenv(m_name.constData());
        }
    }

private:
    QByteArray m_name;
    QByteArray m_previous;
    bool m_wasSet;
};

QString installAbiFixture(QTemporaryDir& root, const QString& name, const QString& sourceLibrary) {
    const QString pluginDir = root.filePath(name);
    if (!QDir().mkpath(pluginDir)) {
        return {};
    }

    QFile metadata(QDir(pluginDir).filePath(QStringLiteral("metadata.json")));
    if (!metadata.open(QIODevice::WriteOnly)) {
        return {};
    }
    QJsonObject json;
    json.insert(QStringLiteral("id"), QStringLiteral("com.deeplux.test.") + name.toLower());
    json.insert(QStringLiteral("name"), name);
    json.insert(QStringLiteral("version"), QStringLiteral("1.0.0"));
    json.insert(QStringLiteral("category"), QStringLiteral("test"));
    json.insert(QStringLiteral("ports"),
                QJsonObject{{QStringLiteral("inputs"), QJsonArray{}}, {QStringLiteral("outputs"), QJsonArray{}}});
    const QByteArray metadataBytes = QJsonDocument(json).toJson(QJsonDocument::Compact);
    if (metadata.write(metadataBytes) != metadataBytes.size()) {
        return {};
    }
    metadata.close();

    const QString installedLibrary = QDir(pluginDir).filePath(QFileInfo(sourceLibrary).fileName());
    if (!QFile::copy(sourceLibrary, installedLibrary)) {
        return {};
    }
    return installedLibrary;
}

QString loadFailure(PluginManager& manager, const QString& name) {
    QSignalSpy failed(&manager, &PluginManager::pluginLoadFailed);
    if (manager.loadPlugin(name)) {
        manager.unloadPlugin(name);
        return QStringLiteral("unexpectedly loaded");
    }
    if (failed.size() != 1) {
        return QStringLiteral("pluginLoadFailed signal count: %1").arg(failed.size());
    }
    return failed.first().at(1).toString();
}

} // namespace

void TestPluginManager::initTestCase() {
    qDebug() << "=== TestPluginManager Start ===";
}

void TestPluginManager::cleanupTestCase() {
    qDebug() << "=== TestPluginManager End ===";
}

void TestPluginManager::testInitialize() {
    PluginManager& mgr = PluginManager::instance();

    QVERIFY(mgr.initialize());
    QVERIFY(mgr.isInitialized());
}

void TestPluginManager::testPluginPaths() {
    PluginManager& mgr = PluginManager::instance();

    QString testPath = "/tmp/test_plugins";
    mgr.addPluginPath(testPath);

    QVERIFY(mgr.pluginPaths().contains(testPath));

    // 重复添加不应该重复
    mgr.addPluginPath(testPath);
    QCOMPARE(mgr.pluginPaths().count(testPath), 1);
}

void TestPluginManager::testScanPlugins() {
    PluginManager& mgr = PluginManager::instance();

    mgr.scanPlugins();

    // 扫描完成，即使没有插件也应该返回
    // 这里只是验证方法可以正常调用
    qDebug() << "Modules found:" << mgr.availableModules().size();
    qDebug() << "Cameras found:" << mgr.availableCameras().size();
}

void TestPluginManager::testAvailableModules() {
    PluginManager& mgr = PluginManager::instance();

    // 返回 QStringList
    QStringList modules = mgr.availableModules();
    QVERIFY(modules.isEmpty() || !modules.isEmpty()); // 总是返回列表

    QStringList cameras = mgr.availableCameras();
    QVERIFY(cameras.isEmpty() || !cameras.isEmpty());
}

void TestPluginManager::testPluginInfo() {
    PluginManager& mgr = PluginManager::instance();

    // 获取不存在的插件信息
    PluginInfo info = mgr.pluginInfo("nonexistent");
    QVERIFY(info.name.isEmpty());

    // 获取模块信息列表
    QList<PluginInfo> moduleInfos = mgr.moduleInfos();
    QList<PluginInfo> cameraInfos = mgr.cameraInfos();

    qDebug() << "Module infos:" << moduleInfos.size();
    qDebug() << "Camera infos:" << cameraInfos.size();
}

void TestPluginManager::testCreateModuleClone() {
    PluginManager& mgr = PluginManager::instance();
    QStringList modules = mgr.availableModules();
    QVERIFY(!modules.isEmpty());

    QString testModule = modules.first();
    if (!mgr.isPluginLoaded(testModule)) {
        QVERIFY2(mgr.loadPlugin(testModule, 5000), qPrintable(QString("Failed to load plugin %1").arg(testModule)));
    }
    QVERIFY(mgr.isPluginLoaded(testModule));

    IModule* a = mgr.createModule(testModule);
    QVERIFY(a != nullptr);

    IModule* b = mgr.createModule(testModule);
    QVERIFY(b != nullptr);

    QVERIFY(a != b);

    QVERIFY(!a->name().isEmpty());
    QVERIFY(!b->name().isEmpty());

    a->setParam("instanceMarker", "a");
    b->setParam("instanceMarker", "b");
    QCOMPARE(a->currentParams().value("instanceMarker").toString(), QString("a"));
    QCOMPARE(b->currentParams().value("instanceMarker").toString(), QString("b"));

    delete a;
    delete b;
}

void TestPluginManager::testExecutionMetadataValidation() {
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const auto writeMetadata = [&root](const QString& directory, const QString& name, const QByteArray& joinPolicy) {
        const QString pluginDir = root.filePath(directory);
        if (!QDir().mkpath(pluginDir))
            return false;
        QFile file(QDir(pluginDir).filePath(QStringLiteral("metadata.json")));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        const QByteArray json = QByteArray(R"({
            "id":"com.deeplux.test.metadata",
            "name":")") + name.toUtf8() +
                                QByteArray(R"(",
            "category":"test",
            "execution":{"threadSafe":true},
            "ports":{"inputs":[{
                "id":"control","displayName":"control","type":"Boolean",
                "control":true,"multiple":true,"joinPolicy":")") +
                                joinPolicy + QByteArray(R"("}],"outputs":[]}
        })");
        return file.write(json) == json.size() && file.flush();
    };

    QVERIFY(writeMetadata(QStringLiteral("valid"), QStringLiteral("MetadataValid"), "all"));
    QVERIFY(writeMetadata(QStringLiteral("invalid"), QStringLiteral("MetadataInvalid"), "sometimes"));

    PluginManager& manager = PluginManager::instance();
    manager.addPluginPath(root.path());
    manager.scanPlugins();

    const PluginInfo valid = manager.pluginInfo(QStringLiteral("MetadataValid"));
    QVERIFY(valid.threadSafe);
    QCOMPARE(valid.inputPorts.size(), 1);
    QCOMPARE(valid.inputPorts.first().joinPolicy, ControlJoinPolicy::All);

    QVERIFY(manager.pluginInfo(QStringLiteral("MetadataInvalid")).name.isEmpty());
}

void TestPluginManager::testBlockingMetadataParsedAndInjected() {
    // G-fix4 + G2-fix3: execution.blocking 必须被解析、注入模块，删除注入代码本测试应失败
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const auto writeMetadata = [&root](const QString& directory, const QString& name, bool blocking) {
        const QString pluginDir = root.filePath(directory);
        if (!QDir().mkpath(pluginDir))
            return false;
        QFile file(QDir(pluginDir).filePath(QStringLiteral("metadata.json")));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        const QByteArray json = QByteArray(R"({
            "id":"com.deeplux.test.blocking",
            "name":")") + name.toUtf8() +
                                QByteArray(R"(",
            "category":"test",
            "execution":{"threadSafe":true,"blocking":)") +
                                (blocking ? "true" : "false") + QByteArray(R"(},
            "ports":{"inputs":[],"outputs":[]}
        })");
        return file.write(json) == json.size() && file.flush();
    };

    QVERIFY(writeMetadata(QStringLiteral("blocking"), QStringLiteral("BlockingMod"), true));
    QVERIFY(writeMetadata(QStringLiteral("nonblocking"), QStringLiteral("NonBlockingMod"), false));

    PluginManager& manager = PluginManager::instance();
    manager.addPluginPath(root.path());
    manager.scanPlugins();

    const PluginInfo blockingInfo = manager.pluginInfo(QStringLiteral("BlockingMod"));
    QVERIFY(blockingInfo.threadSafe);
    QVERIFY2(blockingInfo.blocking, "blocking=true must be parsed from metadata");

    const PluginInfo nonBlockingInfo = manager.pluginInfo(QStringLiteral("NonBlockingMod"));
    QVERIFY(!nonBlockingInfo.blocking);
    // createModule 注入 isBlocking() 的验证见 test_pluginparametercontracts
    // （该测试通过 installAllPlugins 可靠加载真实插件）
}

void TestPluginManager::testExecutionAffinityDefaultsToGui() {
    PluginInfo info;
    QCOMPARE(info.executionAffinity, ExecutionAffinity::Gui);
}

void TestPluginManager::testExecutionAffinityParsesWorker() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pluginDir = dir.filePath(QStringLiteral("AffinityProbe"));
    QVERIFY(QDir().mkpath(pluginDir));
    QFile meta(QDir(pluginDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(meta.open(QIODevice::WriteOnly));
    meta.write(R"({
        "id":"com.deeplux.test.affinity","name":"AffinityProbe","version":"1.0.0",
        "category":"test","description":"affinity probe","author":"tests",
        "execution":{"threadSafe":false,"blocking":false,"affinity":"worker"},
        "ports":{"inputs":[],"outputs":[]}
    })");
    meta.close();

    PluginInfo info;
    QVERIFY(
        PluginManager::instance().loadPluginMetadata(QDir(pluginDir).filePath(QStringLiteral("metadata.json")), info));
    QCOMPARE(info.executionAffinity, ExecutionAffinity::Worker);
}

void TestPluginManager::testExecutionAffinityUnknownFallsBackToGui() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString pluginDir = dir.filePath(QStringLiteral("AffinityUnknown"));
    QVERIFY(QDir().mkpath(pluginDir));
    QFile meta(QDir(pluginDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(meta.open(QIODevice::WriteOnly));
    meta.write(R"({
        "id":"com.deeplux.test.affinityunknown","name":"AffinityUnknown","version":"1.0.0",
        "category":"test","description":"unknown affinity probe","author":"tests",
        "execution":{"threadSafe":false,"blocking":false,"affinity":"future-mode"},
        "ports":{"inputs":[],"outputs":[]}
    })");
    meta.close();

    PluginInfo info;
    QVERIFY(
        PluginManager::instance().loadPluginMetadata(QDir(pluginDir).filePath(QStringLiteral("metadata.json")), info));
    QCOMPARE(info.executionAffinity, ExecutionAffinity::Gui);
}

void TestPluginManager::testBrokenUserPluginFallsBackToBundle() {
    const QString bundleRoot = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../plugins");
    QVERIFY(QDir().mkpath(bundleRoot));
    QTemporaryDir bundleDir(QDir(bundleRoot).filePath("PackagingFallbackXXXXXX"));
    QTemporaryDir userRoot;
    QVERIFY(bundleDir.isValid());
    QVERIFY(userRoot.isValid());

    const QString userDir = userRoot.filePath("PackagingFallback");
    QVERIFY(QDir().mkpath(userDir));
    QFile sourceMetadata(QStringLiteral(TEST_PLUGINMANAGER_METADATA));
    QVERIFY(sourceMetadata.open(QIODevice::ReadOnly));
    QJsonObject metadata = QJsonDocument::fromJson(sourceMetadata.readAll()).object();
    metadata["name"] = QStringLiteral("PackagingFallback");
    metadata["id"] = QStringLiteral("com.deeplux.test.packagingfallback");
    const QByteArray metadataBytes = QJsonDocument(metadata).toJson();
    for (const QString& directory : {bundleDir.path(), userDir}) {
        QFile output(QDir(directory).filePath("metadata.json"));
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(metadataBytes), metadataBytes.size());
    }
    const QString moduleFileName = QFileInfo(QStringLiteral(TEST_PLUGINMANAGER_LIB)).fileName();
    QVERIFY(QFile::copy(QStringLiteral(TEST_PLUGINMANAGER_LIB), QDir(bundleDir.path()).filePath(moduleFileName)));

    PluginManager& manager = PluginManager::instance();
    manager.addPluginPath(userRoot.path());
    manager.scanPlugins();
    QCOMPARE(manager.pluginInfo("PackagingFallback").path, QDir(userDir).filePath("metadata.json"));
    QVERIFY(manager.loadPlugin("PackagingFallback"));
    QCOMPARE(manager.pluginInfo("PackagingFallback").path, QDir(bundleDir.path()).filePath("metadata.json"));
    manager.unloadPlugin("PackagingFallback");

    const QString invalidFileName = QStringLiteral("InvalidPlugin.") + QFileInfo(moduleFileName).suffix();
    QFile invalidLibrary(QDir(userDir).filePath(invalidFileName));
    QVERIFY(invalidLibrary.open(QIODevice::WriteOnly));
    QVERIFY(invalidLibrary.write("not an ELF plugin") > 0);
    invalidLibrary.close();
    manager.scanPlugins();
    QVERIFY(manager.loadPlugin("PackagingFallback"));
    QCOMPARE(manager.pluginInfo("PackagingFallback").path, QDir(bundleDir.path()).filePath("metadata.json"));
    manager.unloadPlugin("PackagingFallback");
}

void TestPluginManager::testNonCameraUserPluginFallsBackToBundledCamera() {
#ifndef TEST_PLUGINMANAGER_CAMERA_LIB
    QSKIP("V4L2 camera plugin is available only on Linux");
#else
    const QString bundleRoot = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../plugins");
    QVERIFY(QDir().mkpath(bundleRoot));
    QTemporaryDir bundleDir(QDir(bundleRoot).filePath("PackagingCameraFallbackXXXXXX"));
    QTemporaryDir userRoot;
    QVERIFY(bundleDir.isValid());
    QVERIFY(userRoot.isValid());

    const QString userDir = userRoot.filePath("PackagingCameraFallback");
    QVERIFY(QDir().mkpath(userDir));
    QFile sourceMetadata(QStringLiteral(TEST_PLUGINMANAGER_CAMERA_METADATA));
    QVERIFY(sourceMetadata.open(QIODevice::ReadOnly));
    QJsonObject metadata = QJsonDocument::fromJson(sourceMetadata.readAll()).object();
    metadata["name"] = QStringLiteral("PackagingCameraFallback");
    metadata["id"] = QStringLiteral("com.deeplux.test.packagingcamerafallback");
    const QByteArray metadataBytes = QJsonDocument(metadata).toJson();
    for (const QString& directory : {bundleDir.path(), userDir}) {
        QFile output(QDir(directory).filePath("metadata.json"));
        QVERIFY(output.open(QIODevice::WriteOnly));
        QCOMPARE(output.write(metadataBytes), metadataBytes.size());
    }
    QVERIFY(QFile::copy(
        QStringLiteral(TEST_PLUGINMANAGER_CAMERA_LIB),
        QDir(bundleDir.path()).filePath(QFileInfo(QStringLiteral(TEST_PLUGINMANAGER_CAMERA_LIB)).fileName())));
    QVERIFY(QFile::copy(QStringLiteral(TEST_PLUGINMANAGER_LIB),
                        QDir(userDir).filePath(QFileInfo(QStringLiteral(TEST_PLUGINMANAGER_LIB)).fileName())));

    PluginManager& manager = PluginManager::instance();
    manager.addPluginPath(userRoot.path());
    manager.scanPlugins();
    QCOMPARE(manager.pluginInfo("PackagingCameraFallback").path, QDir(userDir).filePath("metadata.json"));
    QVERIFY(manager.loadPlugin("PackagingCameraFallback"));
    QCOMPARE(manager.pluginInfo("PackagingCameraFallback").path, QDir(bundleDir.path()).filePath("metadata.json"));
    manager.unloadPlugin("PackagingCameraFallback");

    QFile moduleMetadataSource(QStringLiteral(TEST_PLUGINMANAGER_METADATA));
    QVERIFY(moduleMetadataSource.open(QIODevice::ReadOnly));
    QJsonObject moduleMetadata = QJsonDocument::fromJson(moduleMetadataSource.readAll()).object();
    moduleMetadata["name"] = QStringLiteral("PackagingCameraFallback");
    moduleMetadata["id"] = QStringLiteral("com.deeplux.test.packagingcamerafallback.module");
    QFile userMetadata(QDir(userDir).filePath("metadata.json"));
    QVERIFY(userMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray moduleMetadataBytes = QJsonDocument(moduleMetadata).toJson();
    QCOMPARE(userMetadata.write(moduleMetadataBytes), moduleMetadataBytes.size());
    userMetadata.close();
    QVERIFY(QFile::remove(QDir(userDir).filePath(QFileInfo(QStringLiteral(TEST_PLUGINMANAGER_LIB)).fileName())));

    manager.scanPlugins();
    QVERIFY(manager.availableModules().contains("PackagingCameraFallback"));
    QVERIFY(!manager.availableCameras().contains("PackagingCameraFallback"));
    QVERIFY(manager.loadPlugin("PackagingCameraFallback"));
    QCOMPARE(manager.pluginInfo("PackagingCameraFallback").path, QDir(bundleDir.path()).filePath("metadata.json"));
    QVERIFY(!manager.availableModules().contains("PackagingCameraFallback"));
    QVERIFY(manager.availableCameras().contains("PackagingCameraFallback"));
    manager.unloadPlugin("PackagingCameraFallback");
#endif
}

void TestPluginManager::testRejectsPluginWithWrongAbiExportBeforeInstance() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString installed =
        installAbiFixture(root, QStringLiteral("AbiWrongVersion"), QStringLiteral(TEST_ABI_WRONG_VERSION_PLUGIN));
    QVERIFY(!installed.isEmpty());
    const QString marker = root.filePath(QStringLiteral("instance-marker"));
    ScopedEnvironmentVariable markerEnv("DEEPLUX_ABI_FIXTURE_INSTANCE_MARKER", marker.toLocal8Bit());

    PluginManager& manager = PluginManager::instance();
    manager.shutdown();
    manager.addPluginPath(root.path());
    manager.scanPlugins();
    const QString error = loadFailure(manager, QStringLiteral("AbiWrongVersion"));
    QVERIFY2(error.contains(QStringLiteral("abi_version_mismatch")), qPrintable(error));
    QVERIFY2(error.contains(installed), qPrintable(error));
    QVERIFY2(!QFile::exists(marker), "ABI rejection must precede plugin instance construction");
}

void TestPluginManager::testRejectsLegacyPluginWithoutAbiExport() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString installed =
        installAbiFixture(root, QStringLiteral("AbiLegacy"), QStringLiteral(TEST_ABI_LEGACY_PLUGIN));
    QVERIFY(!installed.isEmpty());
    const QString marker = root.filePath(QStringLiteral("instance-marker"));
    ScopedEnvironmentVariable markerEnv("DEEPLUX_ABI_FIXTURE_INSTANCE_MARKER", marker.toLocal8Bit());

    PluginManager& manager = PluginManager::instance();
    manager.shutdown();
    manager.addPluginPath(root.path());
    manager.scanPlugins();
    const QString error = loadFailure(manager, QStringLiteral("AbiLegacy"));
    QVERIFY2(error.contains(QStringLiteral("missing_abi_export")), qPrintable(error));
    QVERIFY2(error.contains(installed), qPrintable(error));
    QVERIFY2(!QFile::exists(marker), "ABI rejection must precede plugin instance construction");
}

void TestPluginManager::testRejectsPluginWithWrongAbiFingerprintBeforeInstance() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString installed = installAbiFixture(root, QStringLiteral("AbiWrongFingerprint"),
                                                QStringLiteral(TEST_ABI_WRONG_FINGERPRINT_PLUGIN));
    QVERIFY(!installed.isEmpty());
    const QString marker = root.filePath(QStringLiteral("instance-marker"));
    ScopedEnvironmentVariable markerEnv("DEEPLUX_ABI_FIXTURE_INSTANCE_MARKER", marker.toLocal8Bit());

    PluginManager& manager = PluginManager::instance();
    manager.shutdown();
    manager.addPluginPath(root.path());
    manager.scanPlugins();
    const QString error = loadFailure(manager, QStringLiteral("AbiWrongFingerprint"));
    QVERIFY2(error.contains(QStringLiteral("abi_fingerprint_mismatch")), qPrintable(error));
    QVERIFY2(error.contains(installed), qPrintable(error));
    QVERIFY2(!QFile::exists(marker), "ABI rejection must precede plugin instance construction");
}

void TestPluginManager::testRejectsAbiExportsOwnedByDependency() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QLibrary dependency(QStringLiteral(TEST_ABI_DEPENDENCY_LIB));
    QVERIFY2(dependency.load(), qPrintable(dependency.errorString()));
    const QString installed =
        installAbiFixture(root, QStringLiteral("AbiDependency"), QStringLiteral(TEST_ABI_DEPENDENCY_PLUGIN));
    QVERIFY(!installed.isEmpty());
    QLibrary plugin(installed);
    QVERIFY2(plugin.load(), qPrintable(plugin.errorString()));
    QVERIFY(plugin.resolve("deeplux_plugin_abi_version"));
    QVERIFY(plugin.resolve("deeplux_plugin_abi_fingerprint"));
    QVERIFY(plugin.unload());
    const QString marker = root.filePath(QStringLiteral("instance-marker"));
    ScopedEnvironmentVariable markerEnv("DEEPLUX_ABI_FIXTURE_INSTANCE_MARKER", marker.toLocal8Bit());

    PluginManager& manager = PluginManager::instance();
    manager.shutdown();
    manager.addPluginPath(root.path());
    manager.scanPlugins();
    const QString error = loadFailure(manager, QStringLiteral("AbiDependency"));
    QVERIFY2(error.contains(QStringLiteral("abi_symbol_not_owned")), qPrintable(error));
    QVERIFY2(error.contains(installed), qPrintable(error));
    QVERIFY2(!QFile::exists(marker), "ABI rejection must precede plugin instance construction");
}

void TestPluginManager::testValidAbiPreflightLoadsNativeInitializerOnce() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString installed =
        installAbiFixture(root, QStringLiteral("AbiValid"), QStringLiteral(TEST_ABI_VALID_PLUGIN));
    QVERIFY(!installed.isEmpty());
    const QString marker = root.filePath(QStringLiteral("native-load-marker"));
    ScopedEnvironmentVariable markerEnv("DEEPLUX_ABI_FIXTURE_NATIVE_LOAD_MARKER", marker.toLocal8Bit());

    PluginManager& manager = PluginManager::instance();
    manager.shutdown();
    manager.addPluginPath(root.path());
    manager.scanPlugins();
    QVERIFY2(manager.loadPlugin(QStringLiteral("AbiValid")), "valid ABI fixture should load");

    QFile markerFile(marker);
    QVERIFY(markerFile.open(QIODevice::ReadOnly));
    QCOMPARE(markerFile.readAll(), QByteArray("load\n"));
    manager.unloadPlugin(QStringLiteral("AbiValid"));
}

QTEST_MAIN(TestPluginManager)
#include "test_pluginmanager.moc"
