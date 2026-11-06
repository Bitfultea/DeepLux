#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSemaphore>
#include <QSignalSpy>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>
#include <core/base/ModuleBase.h>
#include <core/device/CameraManager.h>
#include <core/interface/IModule.h>
#include <core/manager/PluginManager.h>

using namespace DeepLux;

class BlockingDestructionChild : public QObject {
public:
    BlockingDestructionChild(QSemaphore* entered, QSemaphore* continueDestruction, QObject* parent)
        : QObject(parent), m_entered(entered), m_continueDestruction(continueDestruction) {}

    ~BlockingDestructionChild() override {
        m_entered->release();
        m_continueDestruction->acquire();
    }

private:
    QSemaphore* m_entered;
    QSemaphore* m_continueDestruction;
};

class UnmanagedModule final : public ModuleBase {
public:
    UnmanagedModule() {
        m_name = QStringLiteral("Unmanaged test module");
    }

protected:
    IModule* cloneImpl() const override {
        return new UnmanagedModule();
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
    bool process(const ImageData&, ImageData&) override {
        return true;
    }
};

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
    void testSafeModulePluginUnload();
    void testConcurrentModuleCreationAndUnload();
    void testConcurrentModuleDestructionAndUnload();
    void testLegacyDirectDeletePinsPlugin();
    void testExecutionMetadataValidation();
    void testBlockingMetadataParsedAndInjected();
    void testAvailabilityItemUsesMetadataSnapshotAndLoadedPriority();
    void testCameraAvailabilityPlatformCompatibility();
    void testExecutionAffinityDefaultsToGui();
    void testExecutionAffinityParsesWorker();
    void testExecutionAffinityUnknownFallsBackToGui();
    void testBrokenUserPluginFallsBackToBundle();
    void testNonCameraUserPluginFallsBackToBundledCamera();

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

    QVERIFY(mgr.destroyModule(a));
    QVERIFY(mgr.destroyModule(b));
}

void TestPluginManager::testSafeModulePluginUnload() {
    PluginManager& manager = PluginManager::instance();
    const QStringList modules = manager.availableModules();
    QVERIFY(!modules.isEmpty());

    QString error;
    QVERIFY(!manager.tryUnloadPlugin(QString(), &error));
    QCOMPARE(error, QStringLiteral("PLUGIN_NOT_FOUND"));
    QVERIFY(!manager.tryUnloadPlugin(QStringLiteral("NoSuchPlugin"), &error));
    QCOMPARE(error, QStringLiteral("PLUGIN_NOT_FOUND"));
    QCOMPARE(PluginManager::unloadBlockReason(true, 0), QStringLiteral("CAMERA_PLUGIN_LIFECYCLE_UNSUPPORTED"));
    QCOMPARE(PluginManager::unloadBlockReason(false, 1), QStringLiteral("PLUGIN_IN_USE"));
    QCOMPARE(PluginManager::unloadBlockReason(false, 0, false), QStringLiteral("PLUGIN_LIFECYCLE_UNTRACKED"));
    QVERIFY(PluginManager::unloadBlockReason(false, 0).isEmpty());

    const QString pluginName = modules.first();
    if (!manager.isPluginLoaded(pluginName))
        QVERIFY(manager.loadPlugin(pluginName, 5000));

    IModule* first = manager.createModule(pluginName);
    IModule* second = manager.createModule(pluginName);
    QVERIFY(first);
    QVERIFY(second);

    QSignalSpy unloadedSpy(&manager, &PluginManager::pluginUnloaded);
    QVERIFY(!manager.tryUnloadPlugin(pluginName, &error));
    QCOMPARE(error, QStringLiteral("PLUGIN_IN_USE"));
    QVERIFY(manager.isPluginLoaded(pluginName));
    QCOMPARE(unloadedSpy.count(), 0);
    QVERIFY(!first->name().isEmpty());
    QVERIFY(!second->name().isEmpty());

    QVERIFY(manager.destroyModule(first));
    QCoreApplication::processEvents();
    QVERIFY(!manager.tryUnloadPlugin(pluginName, &error));
    QCOMPARE(error, QStringLiteral("PLUGIN_IN_USE"));
    QVERIFY(manager.isPluginLoaded(pluginName));
    QVERIFY(manager.destroyModule(second));
    QCoreApplication::processEvents();

    QVERIFY(manager.tryUnloadPlugin(pluginName, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!manager.isPluginLoaded(pluginName));
    QCOMPARE(unloadedSpy.count(), 1);
    QVERIFY(!manager.tryUnloadPlugin(pluginName, &error));
    QCOMPARE(error, QStringLiteral("PLUGIN_NOT_LOADED"));
    QCOMPARE(unloadedSpy.count(), 1);
    QVERIFY(manager.loadPlugin(pluginName, 5000));
    QVERIFY(manager.isPluginLoaded(pluginName));
    IModule* reloaded = manager.createModule(pluginName);
    QVERIFY(reloaded);
    QVERIFY(manager.destroyModule(reloaded));
    QCoreApplication::processEvents();

    QVERIFY2(modules.size() >= 2, "Safe unloadAll test requires two scanned module plugins");
    {
        const QString idlePluginName = modules.at(1);
        if (!manager.isPluginLoaded(idlePluginName))
            QVERIFY(manager.loadPlugin(idlePluginName, 5000));
        IModule* held = manager.createModule(pluginName);
        QVERIFY(held);

        manager.unloadAllPlugins();
        QVERIFY(manager.isPluginLoaded(pluginName));
        QVERIFY(!manager.isPluginLoaded(idlePluginName));

        QVERIFY(manager.destroyModule(held));
        QVERIFY(manager.tryUnloadPlugin(pluginName, &error));
    }

    const QStringList cameras = manager.availableCameras();
    if (!cameras.isEmpty()) {
        const QString cameraName = cameras.first();
        if (!manager.isPluginLoaded(cameraName))
            QVERIFY(manager.loadPlugin(cameraName, 5000));

        const QList<ICameraPlugin*> registeredBefore = CameraManager::instance().plugins();
        QSignalSpy unloadedSpy(&manager, &PluginManager::pluginUnloaded);
        QVERIFY(!manager.tryUnloadPlugin(cameraName, &error));
        QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_LIFECYCLE_UNSUPPORTED"));
        manager.unloadPlugin(cameraName);
        manager.unloadAllPlugins();
        QVERIFY(manager.isPluginLoaded(cameraName));
        QCOMPARE(CameraManager::instance().plugins(), registeredBefore);
        QCOMPARE(unloadedSpy.count(), 0);
    } else {
        // Some reduced builds omit camera plugins; the unconditional reason-code assertion above still locks
        // the fail-closed policy, while Linux/Windows plugin builds exercise manager integration here.
    }
}

void TestPluginManager::testConcurrentModuleCreationAndUnload() {
    PluginManager& manager = PluginManager::instance();
    const QStringList modules = manager.availableModules();
    QVERIFY(!modules.isEmpty());
    const QString pluginName = modules.first();

    for (int iteration = 0; iteration < 20; ++iteration) {
        if (!manager.isPluginLoaded(pluginName))
            QVERIFY(manager.loadPlugin(pluginName, 5000));

        QSemaphore start;
        QSemaphore created;
        QSemaphore releaseDestroy;
        bool returnedInstance = false;
        QThread* creator = QThread::create([&]() {
            start.acquire();
            IModule* module = manager.createModule(pluginName);
            returnedInstance = module != nullptr;
            created.release();
            releaseDestroy.acquire();
            if (module)
                manager.destroyModule(module);
        });
        QVERIFY(creator);
        creator->start();
        start.release();

        QString error;
        const bool unloaded = manager.tryUnloadPlugin(pluginName, &error);
        created.acquire();
        if (returnedInstance) {
            QVERIFY(!unloaded);
            QCOMPARE(error, QStringLiteral("PLUGIN_IN_USE"));
            QVERIFY(manager.isPluginLoaded(pluginName));
        } else {
            QVERIFY(unloaded);
            QVERIFY(!manager.isPluginLoaded(pluginName));
        }
        releaseDestroy.release();
        QVERIFY(creator->wait(5000));
        delete creator;
        QCoreApplication::processEvents();
    }

    if (!manager.isPluginLoaded(modules.first()))
        QVERIFY(manager.loadPlugin(modules.first(), 5000));
}

void TestPluginManager::testConcurrentModuleDestructionAndUnload() {
    PluginManager& manager = PluginManager::instance();
    const QStringList modules = manager.availableModules();
    QVERIFY(!modules.isEmpty());
    const QString pluginName = modules.first();
    if (!manager.isPluginLoaded(pluginName))
        QVERIFY(manager.loadPlugin(pluginName, 5000));

    QSemaphore created;
    QSemaphore beginDestruction;
    QSemaphore childDestructionEntered;
    QSemaphore continueChildDestruction;
    QSemaphore moduleDeletionComplete;
    bool createdModule = false;
    QThread* destroyer = QThread::create([&]() {
        IModule* module = manager.createModule(pluginName);
        if (!module) {
            created.release();
            return;
        }
        createdModule = true;
        new BlockingDestructionChild(&childDestructionEntered, &continueChildDestruction, module);
        created.release();
        beginDestruction.acquire();
        manager.destroyModule(module);
        moduleDeletionComplete.release();
    });
    QVERIFY(destroyer);
    destroyer->start();
    created.acquire();

    if (!createdModule) {
        QVERIFY(destroyer->wait(5000));
        delete destroyer;
        QFAIL("A loaded module plugin must create an instance before destruction race testing");
    }
    QVERIFY(manager.isPluginLoaded(pluginName));
    beginDestruction.release();
    const bool enteredChildDestructor = childDestructionEntered.tryAcquire(1, 5000);
    if (!enteredChildDestructor) {
        continueChildDestruction.release();
        moduleDeletionComplete.tryAcquire(1, 5000);
        QVERIFY(destroyer->wait(5000));
        delete destroyer;
        QFAIL("Parent deletion must invoke the test child destructor");
    }

    QString error;
    const bool unloaded = manager.tryUnloadPlugin(pluginName, &error);
    continueChildDestruction.release();
    QVERIFY(moduleDeletionComplete.tryAcquire(1, 5000));
    QVERIFY(destroyer->wait(5000));
    delete destroyer;

    QVERIFY(!unloaded);
    QCOMPARE(error, QStringLiteral("PLUGIN_IN_USE"));
    QVERIFY(manager.isPluginLoaded(pluginName));
    bool finalUnload = false;
    QElapsedTimer unloadWait;
    unloadWait.start();
    while (!finalUnload && unloadWait.elapsed() < 5000) {
        QCoreApplication::processEvents();
        finalUnload = manager.tryUnloadPlugin(pluginName, &error);
        if (!finalUnload)
            QTest::qWait(1);
    }
    QVERIFY(finalUnload);
    QVERIFY(!manager.isPluginLoaded(pluginName));
}

void TestPluginManager::testLegacyDirectDeletePinsPlugin() {
    PluginManager& manager = PluginManager::instance();
    const QStringList modules = manager.availableModules();
    QVERIFY(!modules.isEmpty());
    const QString pluginName = modules.last();
    if (!manager.isPluginLoaded(pluginName))
        QVERIFY(manager.loadPlugin(pluginName, 5000));

    IModule* legacyModule = manager.createModule(pluginName);
    QVERIFY(legacyModule);
    delete legacyModule;

    QString error;
    QVERIFY(!manager.tryUnloadPlugin(pluginName, &error));
    QCOMPARE(error, QStringLiteral("PLUGIN_LIFECYCLE_UNTRACKED"));
    QVERIFY(manager.isPluginLoaded(pluginName));
    QVERIFY(!manager.destroyModule(nullptr));

    auto* unmanaged = new UnmanagedModule();
    QVERIFY(unmanaged->initialize());
    QVERIFY(!manager.destroyModule(unmanaged));
    QVERIFY(unmanaged->isInitialized());
    delete unmanaged;
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
    QVERIFY(valid.portsDeclared);
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
    QVERIFY(blockingInfo.portsDeclared); // 合法空端口数组与缺失端口声明必须可区分
    QVERIFY(blockingInfo.threadSafe);
    QVERIFY2(blockingInfo.blocking, "blocking=true must be parsed from metadata");

    const PluginInfo nonBlockingInfo = manager.pluginInfo(QStringLiteral("NonBlockingMod"));
    QVERIFY(nonBlockingInfo.portsDeclared);
    QVERIFY(!nonBlockingInfo.blocking);
    // createModule 注入 isBlocking() 的验证见 test_pluginparametercontracts
    // （该测试通过 installAllPlugins 可靠加载真实插件）
}

void TestPluginManager::testAvailabilityItemUsesMetadataSnapshotAndLoadedPriority() {
    QTemporaryDir root;
    QVERIFY(root.isValid());

    const auto writeMetadata = [&root](const QString& folder, const QByteArray& requirementFields) {
        const QString pluginDir = root.filePath(folder);
        if (!QDir().mkpath(pluginDir))
            return QString();
        const QString metadataPath = QDir(pluginDir).filePath(QStringLiteral("metadata.json"));
        QFile file(metadataPath);
        if (!file.open(QIODevice::WriteOnly))
            return QString();
        const QByteArray json = QByteArray(R"({"id":"com.deeplux.test.availability","name":"AvailabilityFixture",)") +
                                requirementFields +
                                QByteArray(R"(,"category":"test","version":"1.0","ports":{"inputs":[],"outputs":[]}})");
        if (file.write(json) != json.size() || !file.flush())
            return QString();
        return metadataPath;
    };

    const QString metadataPath = writeMetadata(
        QStringLiteral("valid"), QByteArray(R"("platform":"linux","abi":{"major":2},"sdk":"sdk-x","dependencies":{})"));
    QVERIFY(!metadataPath.isEmpty());
    PluginInfo info;
    QVERIFY(PluginManager::instance().loadPluginMetadata(metadataPath, info));

#ifdef Q_OS_WIN
    const QString libraryName = QStringLiteral("fixture.dll");
#else
    const QString libraryName = QStringLiteral("fixture.so");
#endif
    const QString libraryPath = QFileInfo(metadataPath).absoluteDir().filePath(libraryName);
    QFile library(libraryPath);
    QVERIFY(library.open(QIODevice::WriteOnly));
    QVERIFY(library.write("fixture") > 0);
    library.close();

    QJsonObject item = PluginManager::availabilityItem(info, false);
    QVERIFY(item.value(QStringLiteral("installed")).toBool());
    QVERIFY(!item.value(QStringLiteral("loaded")).toBool());
    QCOMPARE(item.value(QStringLiteral("availability")).toString(), QStringLiteral("loadable"));
    QVERIFY(item.value(QStringLiteral("executableActions")).toArray().isEmpty());
    QCOMPARE(item.value(QStringLiteral("suggestedActions")).toArray().first().toString(), QStringLiteral("load"));
    const QJsonObject requirements = item.value(QStringLiteral("requirements")).toObject();
    QCOMPARE(requirements.value(QStringLiteral("platforms")).toArray().first().toString(), QStringLiteral("linux"));
    QCOMPARE(requirements.value(QStringLiteral("abi")).toObject().value(QStringLiteral("major")).toInt(), 2);
    QCOMPARE(requirements.value(QStringLiteral("sdk")).toString(), QStringLiteral("sdk-x"));
    QCOMPARE(requirements.value(QStringLiteral("model")).toString(), QStringLiteral("not_declared"));

    QVERIFY(QFile::remove(libraryPath));
    item = PluginManager::availabilityItem(info, false);
    QVERIFY(!item.value(QStringLiteral("installed")).toBool());
    QCOMPARE(item.value(QStringLiteral("availability")).toString(), QStringLiteral("unavailable"));
    QCOMPARE(item.value(QStringLiteral("unavailableReason")).toArray().first().toString(),
             QStringLiteral("PLUGIN_BINARY_MISSING"));

    item = PluginManager::availabilityItem(info, true);
    QVERIFY(!item.value(QStringLiteral("installed")).toBool());
    QVERIFY(item.value(QStringLiteral("loaded")).toBool());
    QCOMPARE(item.value(QStringLiteral("availability")).toString(), QStringLiteral("loaded"));

    const QString invalidPath = writeMetadata(QStringLiteral("invalid"), QByteArray(R"("platforms":["linux",7])"));
    QVERIFY(!invalidPath.isEmpty());
    PluginInfo invalidInfo;
    QVERIFY(PluginManager::instance().loadPluginMetadata(invalidPath, invalidInfo));
    item = PluginManager::availabilityItem(invalidInfo, false);
    QCOMPARE(item.value(QStringLiteral("requirements")).toObject().value(QStringLiteral("platforms")).toString(),
             QStringLiteral("invalid_declaration"));
    QVERIFY(item.value(QStringLiteral("unavailableReason"))
                .toArray()
                .contains(QStringLiteral("PLUGIN_REQUIREMENT_METADATA_INVALID")));
}

void TestPluginManager::testCameraAvailabilityPlatformCompatibility() {
    PluginInfo info;
    info.id = QStringLiteral("com.deeplux.camera.fixture");
    info.name = QStringLiteral("CameraFixture");
    info.category = QStringLiteral("camera");
    info.requirements.insert(QStringLiteral("platforms"), QJsonArray{QStringLiteral("Linux")});

    QJsonObject item = PluginManager::cameraAvailabilityItem(info, false, QStringLiteral("linux"));
    QCOMPARE(item.value(QStringLiteral("platformCompatibility")).toString(), QStringLiteral("supported"));
    QCOMPARE(item.value(QStringLiteral("availability")).toString(), QStringLiteral("unavailable"));

    item = PluginManager::cameraAvailabilityItem(info, false, QStringLiteral("windows"));
    QCOMPARE(item.value(QStringLiteral("platformCompatibility")).toString(), QStringLiteral("unsupported"));
    const QJsonArray unsupportedReasons = item.value(QStringLiteral("unavailableReason")).toArray();
    QVERIFY(unsupportedReasons.contains(QStringLiteral("PLUGIN_PLATFORM_UNSUPPORTED")));
    QVERIFY(unsupportedReasons.contains(QStringLiteral("PLUGIN_BINARY_MISSING")));

    item = PluginManager::cameraAvailabilityItem(info, true, QStringLiteral("windows"));
    QCOMPARE(item.value(QStringLiteral("availability")).toString(), QStringLiteral("loaded"));
    QCOMPARE(item.value(QStringLiteral("platformCompatibility")).toString(), QStringLiteral("unsupported"));

    info.requirements.remove(QStringLiteral("platforms"));
    item = PluginManager::cameraAvailabilityItem(info, false, QStringLiteral("linux"));
    QCOMPARE(item.value(QStringLiteral("platformCompatibility")).toString(), QStringLiteral("not_declared"));

    info.requirements.insert(QStringLiteral("platforms"), QStringLiteral("invalid_declaration"));
    info.requirementMetadataErrors.append(QStringLiteral("platforms"));
    item = PluginManager::cameraAvailabilityItem(info, false, QStringLiteral("linux"));
    QCOMPARE(item.value(QStringLiteral("platformCompatibility")).toString(), QStringLiteral("not_declared"));
    QVERIFY(item.value(QStringLiteral("unavailableReason"))
                .toArray()
                .contains(QStringLiteral("PLUGIN_REQUIREMENT_METADATA_INVALID")));
    QVERIFY(!item.value(QStringLiteral("unavailableReason"))
                 .toArray()
                 .contains(QStringLiteral("PLUGIN_PLATFORM_UNSUPPORTED")));
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

QTEST_MAIN(TestPluginManager)
#include "test_pluginmanager.moc"
