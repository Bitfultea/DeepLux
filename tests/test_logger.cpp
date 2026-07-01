#include <QtTest/QtTest>
#include <core/common/Logger.h>

using namespace DeepLux;

class TestLogger : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testSingleton();
    void testLogLevels();
    void testLogWithCategory();
    void testLogFiltering();
    void testLogsQuery();
    void testClearLogs();
    void testLogToFile();
    void testLevelToString();
    void testLevelToColor();

private:
    void clearLoggerState();
};

void TestLogger::clearLoggerState()
{
    // Clear logs between tests
    Logger::instance().clearLogs();
}

void TestLogger::initTestCase()
{
    qDebug() << "=== TestLogger Start ===";
}

void TestLogger::cleanupTestCase()
{
    qDebug() << "=== TestLogger End ===";
}

void TestLogger::testSingleton()
{
    Logger& instance1 = Logger::instance();
    Logger& instance2 = Logger::instance();

    // Verify singleton behavior
    QVERIFY(&instance1 == &instance2);
}

void TestLogger::testLogLevels()
{
    clearLoggerState();
    Logger& logger = Logger::instance();

    // Test all log levels
    logger.debug("Debug message");
    logger.info("Info message");
    logger.warning("Warning message");
    logger.error("Error message");
    logger.success("Success message");

    QList<LogEntry> logs = logger.logs();
    QVERIFY(logs.size() >= 5);
}

void TestLogger::testLogWithCategory()
{
    clearLoggerState();
    Logger& logger = Logger::instance();

    logger.info("Test message", "TestCategory");

    QList<LogEntry> categoryLogs = logger.logs("TestCategory");
    QVERIFY(categoryLogs.size() >= 1);
    QCOMPARE(categoryLogs.first().category, QString("TestCategory"));
}

void TestLogger::testLogFiltering()
{
    clearLoggerState();
    Logger& logger = Logger::instance();

    logger.setMinLevel(LogLevel::Warning);

    logger.debug("Debug message - should be filtered");
    logger.info("Info message - should be filtered");
    logger.warning("Warning message");
    logger.error("Error message");

    QList<LogEntry> logs = logger.logs();
    // Only Warning and Error should be present
    for (const LogEntry& entry : logs) {
        QVERIFY(entry.level >= LogLevel::Warning);
    }

    // Reset level
    logger.setMinLevel(LogLevel::Debug);
}

void TestLogger::testLogsQuery()
{
    clearLoggerState();
    Logger& logger = Logger::instance();

    logger.debug("Debug 1");
    logger.info("Info 1");
    logger.warning("Warning 1");
    logger.error("Error 1");

    QList<LogEntry> debugLogs = logger.logs(LogLevel::Debug);
    QVERIFY(debugLogs.size() >= 1);

    QList<LogEntry> errorLogs = logger.logs(LogLevel::Error);
    QVERIFY(errorLogs.size() >= 1);

    QList<LogEntry> infoLogs = logger.logs(LogLevel::Info);
    QVERIFY(infoLogs.size() >= 1);
}

void TestLogger::testClearLogs()
{
    clearLoggerState();
    Logger& logger = Logger::instance();

    logger.info("Message before clear");

    QList<LogEntry> logsBefore = logger.logs();
    QVERIFY(logsBefore.size() >= 1);

    logger.clearLogs();

    QList<LogEntry> logsAfter = logger.logs();
    QVERIFY(logsAfter.isEmpty());
}

void TestLogger::testLogToFile()
{
    Logger& logger = Logger::instance();

    // Verify file logging is enabled by default
    QVERIFY(logger.isLogToFileEnabled());

    // Verify log file path is set
    QVERIFY(!logger.logFilePath().isEmpty());

    // Test disabling file logging
    logger.setLogToFile(false);
    QVERIFY(!logger.isLogToFileEnabled());

    // Re-enable
    logger.setLogToFile(true);
    QVERIFY(logger.isLogToFileEnabled());
}

void TestLogger::testLevelToString()
{
    QCOMPARE(Logger::levelToString(LogLevel::Debug), QString("DEBUG"));
    QCOMPARE(Logger::levelToString(LogLevel::Info), QString("INFO"));
    QCOMPARE(Logger::levelToString(LogLevel::Warning), QString("WARN"));
    QCOMPARE(Logger::levelToString(LogLevel::Error), QString("ERROR"));
    QCOMPARE(Logger::levelToString(LogLevel::Success), QString("SUCCESS"));
}

void TestLogger::testLevelToColor()
{
    QString debugColor = Logger::levelToColor(LogLevel::Debug);
    QString infoColor = Logger::levelToColor(LogLevel::Info);
    QString warningColor = Logger::levelToColor(LogLevel::Warning);
    QString errorColor = Logger::levelToColor(LogLevel::Error);
    QString successColor = Logger::levelToColor(LogLevel::Success);

    QVERIFY(!debugColor.isEmpty());
    QVERIFY(!infoColor.isEmpty());
    QVERIFY(!warningColor.isEmpty());
    QVERIFY(!errorColor.isEmpty());
    QVERIFY(!successColor.isEmpty());

    // Verify colors are different
    QVERIFY(debugColor != infoColor);
    QVERIFY(infoColor != warningColor);
    QVERIFY(warningColor != errorColor);
    QVERIFY(errorColor != successColor);
}

QTEST_MAIN(TestLogger)
#include "test_logger.moc"
