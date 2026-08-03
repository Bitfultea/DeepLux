#include "core/manager/GlobalVarManager.h"
#include "core/model/ImageData.h"
#include "plugins/system/SaveData/SaveDataPlugin.h"
#include "plugins/system/SystemTime/SystemTimePlugin.h"
#include "plugins/variable/CreateString/CreateStringPlugin.h"
#include "plugins/variable/MathPlugin/MathPlugin.h"
#include "plugins/variable/SplitString/SplitStringPlugin.h"
#include "plugins/variable/StrFormatPlugin/StrFormatPlugin.h"

#include <QJsonArray>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestVariableSystemPlugins : public QObject {
    Q_OBJECT

private slots:
    void testCreateStringFixedOutput();
    void testCreateStringRejectsInvalidParams();
    void testSplitStringOutputAndValidation();
    void testMathOutputAndValidation();
    void testMathRejectsUnsafeRuntimeInputs();
    void testSaveDataReturnsFalseWhenWriteFails();
    void testSystemTimeOutputAndValidation();
    void testStrFormatUsesInputVariables();
};

void TestVariableSystemPlugins::testCreateStringFixedOutput() {
    CreateStringPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(
        QJsonObject{{"stringSource", "Fixed"}, {"fixedString", "batch-001"}, {"outputVarName", "created"}});

    ImageData input;
    ImageData output;
    QVERIFY(plugin.execute(input, output));

    QCOMPARE(output.data("created").toString(), QString("batch-001"));
    QCOMPARE(output.data("string_length").toInt(), 9);
}

void TestVariableSystemPlugins::testCreateStringRejectsInvalidParams() {
    CreateStringPlugin plugin;
    QString error;

    QJsonObject params = plugin.defaultParams();
    params["stringSource"] = "Unknown";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject unsupported string source");
    QVERIFY(!error.isEmpty());

    error.clear();
    params = plugin.defaultParams();
    params["outputVarName"] = "";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject empty output variable name");
    QVERIFY(!error.isEmpty());
}

void TestVariableSystemPlugins::testSplitStringOutputAndValidation() {
    SplitStringPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"inputString", "A,B,C"}, {"separator", ","}, {"useRegex", false}, {"outputPrefix", "item"}, {"maxSplits", 0}});

    ImageData input;
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("split_count").toInt(), 3);
    QCOMPARE(output.data("split_result").toString(), QString("A|B|C"));
    QCOMPARE(output.data("item_0").toString(), QString("A"));
    QCOMPARE(output.data("item_1").toString(), QString("B"));
    QCOMPARE(output.data("item_2").toString(), QString("C"));
    QCOMPARE(output.data("item_total").toInt(), 3);

    QString error;
    QJsonObject params = plugin.defaultParams();
    params["separator"] = "";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject empty separator");
    QVERIFY(!error.isEmpty());

    error.clear();
    params = plugin.defaultParams();
    params["outputPrefix"] = "";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject empty output prefix");
    QVERIFY(!error.isEmpty());

    error.clear();
    params = plugin.defaultParams();
    params["useRegex"] = true;
    params["separator"] = "[";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject invalid regular expression");
    QVERIFY(!error.isEmpty());
}

void TestVariableSystemPlugins::testMathOutputAndValidation() {
    MathPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(
        QJsonObject{{"operation", "Multiply"}, {"operandA", "left"}, {"operandB", "4"}, {"outputVar", "product"}});

    ImageData input;
    input.setData("left", 3.5);
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("product").toDouble(), 14.0);
    QCOMPARE(output.data("product_string").toString(), QString("14"));

    QString error;
    QJsonObject params = plugin.defaultParams();
    params["operation"] = "Unsupported";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject unsupported math operation");
    QVERIFY(!error.isEmpty());

    error.clear();
    params = plugin.defaultParams();
    params["outputVar"] = "";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject empty output variable name");
    QVERIFY(!error.isEmpty());
}

void TestVariableSystemPlugins::testMathRejectsUnsafeRuntimeInputs() {
    MathPlugin divide;
    QVERIFY(divide.initialize());
    divide.setParams(
        QJsonObject{{"operation", "Divide"}, {"operandA", "a"}, {"operandB", "b"}, {"outputVar", "quotient"}});

    ImageData input;
    input.setData("a", 10.0);
    input.setData("b", 0.0);
    ImageData output;
    QVERIFY2(!divide.execute(input, output), "Should reject division by zero");
    QVERIFY(!output.data("quotient").isValid());

    MathPlugin add;
    QVERIFY(add.initialize());
    add.setParams(
        QJsonObject{{"operation", "Add"}, {"operandA", "missing_or_literal"}, {"operandB", "2"}, {"outputVar", "sum"}});

    output = ImageData();
    QVERIFY2(!add.execute(ImageData(), output), "Should reject unresolved non-numeric operand");
    QVERIFY(!output.data("sum").isValid());
}

void TestVariableSystemPlugins::testSaveDataReturnsFalseWhenWriteFails() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SaveDataPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"filePath", dir.filePath("missing/output.json")},
        {"fileFormat", "json"},
        {"appendMode", false},
    });

    ImageData input;
    input.setData("value", 42);
    ImageData output;
    QVERIFY2(!plugin.execute(input, output), "SaveData should fail the module when the file cannot be written");
    QCOMPARE(output.data("save_result").toBool(), false);
}

void TestVariableSystemPlugins::testSystemTimeOutputAndValidation() {
    SystemTimePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"timeFormat", "yyyy"}});

    ImageData input;
    ImageData output;
    QVERIFY(plugin.execute(input, output));

    const QString timeString = output.data("time_string").toString();
    QCOMPARE(timeString.size(), 4);
    QCOMPARE(timeString.toInt(), output.data("time_year").toInt());
    QVERIFY(output.data("time_stamp").toLongLong() > 0);
    QVERIFY(output.data("time_month").toInt() >= 1);
    QVERIFY(output.data("time_month").toInt() <= 12);
    QVERIFY(output.data("time_day").toInt() >= 1);
    QVERIFY(output.data("time_day").toInt() <= 31);

    QString error;
    QJsonObject params = plugin.defaultParams();
    params["timeFormat"] = "";
    QVERIFY2(!plugin.validateParams(params, error), "Should reject empty time format");
    QVERIFY(!error.isEmpty());
}

void TestVariableSystemPlugins::testStrFormatUsesInputVariables() {
    StrFormatPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"format", "%s_%s"},
                                 {"inputVariables", QJsonArray{QStringLiteral("batch"), QStringLiteral("001")}},
                                 {"outputVariable", "formatted"}});

    ImageData output;
    QVERIFY(plugin.execute(ImageData(), output));
    QCOMPARE(GlobalVarManager::instance().getVariableValue("formatted").toString(), QString("batch_001"));
    QCOMPARE(output.data("formatted").toString(), QString("batch_001"));

    QString error;
    QJsonObject invalid = plugin.currentParams();
    invalid["inputVariables"] = "batch";
    QVERIFY(!plugin.validateParams(invalid, error));
}

QTEST_MAIN(TestVariableSystemPlugins)
#include "test_variable_system_plugins.moc"
