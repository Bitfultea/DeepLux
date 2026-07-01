#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <core/platform/PathUtils.h>

using namespace DeepLux;

class TestPathUtils : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testAppDataPath();
    void testAppDataPathEnvOverride();
    void testPluginPath();
    void testConfigPath();
    void testLogPath();
    void testProjectPath();
    void testNormalize();
    void testJoin();
    void testEnsureDirExists();
    void testApplicationDirPath();

private:
    QString m_testDir;
};

void TestPathUtils::initTestCase() {
    qDebug() << "=== TestPathUtils Start ===";
    m_testDir = "/tmp/deeplux_test_" + QString::number(QDateTime::currentMSecsSinceEpoch());
}

void TestPathUtils::cleanupTestCase() {
    // 清理测试目录
    QDir dir(m_testDir);
    if (dir.exists()) {
        dir.removeRecursively();
    }
    qDebug() << "=== TestPathUtils End ===";
}

void TestPathUtils::testAppDataPath() {
    QString path = PathUtils::appDataPath();

    qDebug() << "appDataPath:" << path;

    // 检查路径非空
    QVERIFY(!path.isEmpty());

    // 检查目录存在
    QVERIFY(QDir(path).exists());

#ifdef DEEPLUX_PLATFORM_LINUX
    // Linux 下应该是 ~/.deeplux
    QVERIFY(path.endsWith("/.deeplux"));
#endif
}

void TestPathUtils::testAppDataPathEnvOverride() {
    const bool hadPrevious = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
    const QByteArray previous = qgetenv("DEEPLUX_APP_DATA_DIR");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(dir.path()));

    QCOMPARE(PathUtils::appDataPath(), dir.path());
    QVERIFY(QDir(dir.path()).exists());

    if (hadPrevious) {
        qputenv("DEEPLUX_APP_DATA_DIR", previous);
    } else {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }
}

void TestPathUtils::testPluginPath() {
    QString path = PathUtils::pluginPath();

    qDebug() << "pluginPath:" << path;

    QVERIFY(!path.isEmpty());
    QVERIFY(path.contains("plugins"));
    QVERIFY(QDir(path).exists());
}

void TestPathUtils::testConfigPath() {
    QString path = PathUtils::configPath();

    qDebug() << "configPath:" << path;

    QVERIFY(!path.isEmpty());
    QVERIFY(path.contains("config"));
    QVERIFY(QDir(path).exists());
}

void TestPathUtils::testLogPath() {
    QString path = PathUtils::logPath();

    qDebug() << "logPath:" << path;

    QVERIFY(!path.isEmpty());
    QVERIFY(path.contains("logs"));
    QVERIFY(QDir(path).exists());
}

void TestPathUtils::testProjectPath() {
    QString path = PathUtils::projectPath();

    qDebug() << "projectPath:" << path;

    QVERIFY(!path.isEmpty());
    QVERIFY(path.contains("projects"));
    QVERIFY(QDir(path).exists());
}

void TestPathUtils::testNormalize() {
    QCOMPARE(PathUtils::normalize("/home/user/../user/./docs"), QString("/home/user/docs"));
    QCOMPARE(PathUtils::normalize("/home/user/"), QString("/home/user"));
    QCOMPARE(PathUtils::normalize("./test"), QString("test"));
}

void TestPathUtils::testJoin() {
    QString result = PathUtils::join("/home/user", "docs");
    qDebug() << "join result:" << result;

    QVERIFY(result.contains("user"));
    QVERIFY(result.contains("docs"));
}

void TestPathUtils::testEnsureDirExists() {
    QString testPath = m_testDir + "/test_subdir";

    // 目录不应该存在
    QVERIFY(!QDir(testPath).exists());

    // 创建目录
    bool result = PathUtils::ensureDirExists(testPath);
    QVERIFY(result);
    QVERIFY(QDir(testPath).exists());

    // 再次调用应该返回 true（目录已存在）
    result = PathUtils::ensureDirExists(testPath);
    QVERIFY(result);
}

void TestPathUtils::testApplicationDirPath() {
    QString path = PathUtils::applicationDirPath();

    qDebug() << "applicationDirPath:" << path;

    QVERIFY(!path.isEmpty());
    QVERIFY(QDir(path).exists());
}

QTEST_MAIN(TestPathUtils)
#include "test_pathtutils.moc"
