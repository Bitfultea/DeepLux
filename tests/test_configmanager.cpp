#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/manager/ConfigManager.h>

using namespace DeepLux;

class TestConfigManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testInitialize();
    void testSetValue();
    void testGroupValue();
    void testTypedGetters();
    void testDefaults();
    void testPersistence();
    void testRemove();

private:
    QTemporaryDir* m_appDataDir = nullptr;
    QByteArray m_previousAppDataDir;
    bool m_hadPreviousAppDataDir = false;
};

void TestConfigManager::initTestCase() {
    qDebug() << "=== TestConfigManager Start ===";

    m_hadPreviousAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
    if (m_hadPreviousAppDataDir) {
        m_previousAppDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
    }

    m_appDataDir = new QTemporaryDir();
    QVERIFY(m_appDataDir->isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_appDataDir->path()));
}

void TestConfigManager::cleanupTestCase() {
    ConfigManager::instance().save();

    if (m_hadPreviousAppDataDir) {
        qputenv("DEEPLUX_APP_DATA_DIR", m_previousAppDataDir);
    } else {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }
    delete m_appDataDir;
    m_appDataDir = nullptr;

    qDebug() << "=== TestConfigManager End ===";
}

void TestConfigManager::testInitialize() {
    ConfigManager& mgr = ConfigManager::instance();
    QVERIFY(mgr.initialize());
    QVERIFY(mgr.isInitialized());
}

void TestConfigManager::testSetValue() {
    ConfigManager& mgr = ConfigManager::instance();

    mgr.setValue("testKey", "testValue");
    QCOMPARE(mgr.value("testKey").toString(), QString("testValue"));

    mgr.setValue("intKey", 42);
    QCOMPARE(mgr.intVal("intKey"), 42);

    mgr.setValue("boolKey", true);
    QCOMPARE(mgr.boolVal("boolKey"), true);
}

void TestConfigManager::testGroupValue() {
    ConfigManager& mgr = ConfigManager::instance();

    mgr.setGroupValue("testGroup", "key1", "value1");
    mgr.setGroupValue("testGroup", "key2", 123);

    QCOMPARE(mgr.groupString("testGroup", "key1"), QString("value1"));
    QCOMPARE(mgr.groupInt("testGroup", "key2"), 123);

    QJsonObject grp = mgr.getGroup("testGroup");
    QVERIFY(grp.contains("key1"));
}

void TestConfigManager::testTypedGetters() {
    ConfigManager& mgr = ConfigManager::instance();

    mgr.setValue("strKey", "hello");
    mgr.setValue("intKey", 100);
    mgr.setValue("doubleKey", 3.14);
    mgr.setValue("boolKey", false);

    QCOMPARE(mgr.string("strKey"), QString("hello"));
    QCOMPARE(mgr.intVal("intKey"), 100);
    QCOMPARE(mgr.doubleVal("doubleKey"), 3.14);
    QCOMPARE(mgr.boolVal("boolKey"), false);

    // 默认值测试
    QCOMPARE(mgr.string("nonexistent", "default"), QString("default"));
    QCOMPARE(mgr.intVal("nonexistent", 999), 999);
}

void TestConfigManager::testDefaults() {
    ConfigManager& mgr = ConfigManager::instance();

    QVERIFY(mgr.contains("application"));
    QVERIFY(mgr.contains("ui"));

    QCOMPARE(mgr.groupBool("application", "autoSave"), true);
    QCOMPARE(mgr.groupInt("ui", "windowWidth"), 1600);
}

void TestConfigManager::testPersistence() {
    ConfigManager& mgr = ConfigManager::instance();

    mgr.setValue("persistTest", "persisted");
    QVERIFY(mgr.save());
    QCOMPARE(mgr.string("persistTest"), QString("persisted"));
}

void TestConfigManager::testRemove() {
    ConfigManager& mgr = ConfigManager::instance();

    mgr.setValue("removeTest", "value");
    QVERIFY(mgr.contains("removeTest"));

    mgr.remove("removeTest");
    QVERIFY(!mgr.contains("removeTest"));

    mgr.setGroupValue("removeGroup", "key", "value");
    QVERIFY(mgr.containsGroupKey("removeGroup", "key"));

    mgr.removeGroupKey("removeGroup", "key");
    QVERIFY(!mgr.containsGroupKey("removeGroup", "key"));
}

QTEST_MAIN(TestConfigManager)
#include "test_configmanager.moc"
