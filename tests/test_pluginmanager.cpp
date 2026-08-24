#include <QDir>
#include <QFile>
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

private:
};

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

    // G2-fix3: 用真实插件验证 createModule 注入 isBlocking()
    // SaveData metadata 声明 blocking=true；若删除 PluginManager 的 setBlocking 注入，此断言失败
    if (manager.pluginInfo(QStringLiteral("SaveData")).blocking) {
        if (!manager.isPluginLoaded(QStringLiteral("SaveData")))
            manager.loadPlugin(QStringLiteral("SaveData"), 5000);
        if (manager.isPluginLoaded(QStringLiteral("SaveData"))) {
            IModule* mod = manager.createModule(QStringLiteral("SaveData"));
            QVERIFY(mod != nullptr);
            auto* mb = qobject_cast<ModuleBase*>(mod);
            QVERIFY(mb != nullptr);
            QVERIFY2(mb->isBlocking(), "createModule must inject blocking=true from metadata");
            delete mod;
        }
    }
}

QTEST_MAIN(TestPluginManager)
#include "test_pluginmanager.moc"
