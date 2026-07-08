#include <QtTest/QtTest>
#include <QJsonObject>
#include <QVariant>

#include "core/model/ImageData.h"
#include "core/engine/RunEngine.h"
#include "plugins/logic/If/IfPlugin.h"
#include "plugins/logic/Loop/LoopPlugin.h"
#include "plugins/logic/While/WhilePlugin.h"

using namespace DeepLux;

class TestLogicPlugins : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    // IfPlugin - Expression mode
    void testIfExpressionTrue();
    void testIfExpressionFalse();
    void testIfExpressionTrueWithInversion();
    void testIfExpressionFalseWithInversion();
    void testIfExpressionEmptyReturnsFalse();

    // IfPlugin - BoolLink mode
    void testIfBoolLinkTrue();
    void testIfBoolLinkFalse();

    // IfPlugin - validation
    void testIfValidationEmptyBoolLink();
    void testIfValidationExpressionOk();

    // IfPlugin - metadata
    void testIfFlowControlType();
    void testIfEndProcess();

    // LoopPlugin
    void testLoopSetsCount();
    void testLoopDefaultParams();
    void testLoopValidationZeroCount();
    void testLoopFlowControlType();

    // WhilePlugin
    void testWhileNotEmptyTrue();
    void testWhileNotEmptyFalse();
    void testWhileEqual();
    void testWhileNotEqual();
    void testWhileGreaterThan();
    void testWhileLessThan();
    void testWhileValidationEmptyVar();
    void testWhileValidationMaxIterations();
    void testWhileFlowControlType();
    void testWhileEndProcess();
};

void TestLogicPlugins::initTestCase()
{
    qDebug() << "=== TestLogicPlugins Start ===";
}

void TestLogicPlugins::cleanupTestCase()
{
    qDebug() << "=== TestLogicPlugins End ===";
}

void TestLogicPlugins::cleanup()
{
    // Clear RunEngine outputs between tests to avoid cross-test leakage
    RunEngine::instance().clearOutputs();
}

// =========================================================================
// IfPlugin - Expression mode
// =========================================================================

void TestLogicPlugins::testIfExpressionTrue()
{
    IfPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionType", "Expression"},
        {"expressionString", "true"},
        {"boolLinkText", ""},
        {"boolInversion", false}
    });

    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("if_result").toBool(), true);
    QCOMPARE(output.data("if_passed").toBool(), true);
}

void TestLogicPlugins::testIfExpressionFalse()
{
    IfPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionType", "Expression"},
        {"expressionString", "false"},
        {"boolInversion", false}
    });

    ImageData input, output;
    QVERIFY2(!plugin.execute(input, output), "process should return false when condition is false");
    QCOMPARE(output.data("if_result").toBool(), false);
    QCOMPARE(output.data("if_passed").toBool(), false);
}

void TestLogicPlugins::testIfExpressionTrueWithInversion()
{
    IfPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionType", "Expression"},
        {"expressionString", "true"},
        {"boolInversion", true}
    });

    ImageData input, output;
    QVERIFY2(!plugin.execute(input, output), "inverted true should return false");
    QCOMPARE(output.data("if_result").toBool(), false);
}

void TestLogicPlugins::testIfExpressionFalseWithInversion()
{
    IfPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionType", "Expression"},
        {"expressionString", "false"},
        {"boolInversion", true}
    });

    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("if_result").toBool(), true);
}

void TestLogicPlugins::testIfExpressionEmptyReturnsFalse()
{
    IfPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionType", "Expression"},
        {"expressionString", ""},
        {"boolInversion", false}
    });

    ImageData input, output;
    QVERIFY2(!plugin.execute(input, output), "empty expression should evaluate to false");
    QCOMPARE(output.data("if_result").toBool(), false);
}

// =========================================================================
// IfPlugin - BoolLink mode
// =========================================================================

void TestLogicPlugins::testIfBoolLinkTrue()
{
    // Set up RunEngine output so the BoolLink can resolve
    RunEngine::instance().setOutput("SourceModule", "flag", QVariant(true));

    IfPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionType", "BoolLink"},
        {"boolLinkText", "$SourceModule.flag"},
        {"boolInversion", false}
    });

    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("if_result").toBool(), true);
}

void TestLogicPlugins::testIfBoolLinkFalse()
{
    RunEngine::instance().setOutput("SourceModule", "flag", QVariant(false));

    IfPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionType", "BoolLink"},
        {"boolLinkText", "$SourceModule.flag"},
        {"boolInversion", false}
    });

    ImageData input, output;
    QVERIFY2(!plugin.execute(input, output), "BoolLink resolving to false should return false");
    QCOMPARE(output.data("if_result").toBool(), false);
}

// =========================================================================
// IfPlugin - validation
// =========================================================================

void TestLogicPlugins::testIfValidationEmptyBoolLink()
{
    IfPlugin plugin;
    QString error;
    QJsonObject params = plugin.defaultParams();
    params["conditionType"] = "BoolLink";
    params["boolLinkText"] = "";
    QVERIFY2(!plugin.validateParams(params, error), "Empty BoolLink should fail validation");
    QVERIFY(!error.isEmpty());
}

void TestLogicPlugins::testIfValidationExpressionOk()
{
    IfPlugin plugin;
    QString error;
    QJsonObject params = plugin.defaultParams();
    params["conditionType"] = "Expression";
    params["boolLinkText"] = "";  // Should be OK in Expression mode
    QVERIFY2(plugin.validateParams(params, error), "Expression mode should not require boolLinkText");
    QVERIFY(error.isEmpty());
}

// =========================================================================
// IfPlugin - metadata
// =========================================================================

void TestLogicPlugins::testIfFlowControlType()
{
    IfPlugin plugin;
    QCOMPARE(plugin.flowControlType(), ControlFlowType::Conditional);
    QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.if"));
    QCOMPARE(plugin.category(), QString("logic"));
}

void TestLogicPlugins::testIfEndProcess()
{
    IfEndPlugin plugin;
    QVERIFY(plugin.initialize());
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(plugin.flowControlType(), ControlFlowType::ConditionalEnd);
}

// =========================================================================
// LoopPlugin
// =========================================================================

void TestLogicPlugins::testLoopSetsCount()
{
    LoopPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{"loopCount", 3}});

    ImageData input, output;
    QVERIFY(plugin.execute(input, output));

    QCOMPARE(output.data("loop_count").toInt(), 3);
    QCOMPARE(output.data("loop_current").toInt(), 0);
    QCOMPARE(output.data("loop_active").toBool(), true);
}

void TestLogicPlugins::testLoopDefaultParams()
{
    LoopPlugin plugin;
    QJsonObject defaults = plugin.defaultParams();
    QCOMPARE(defaults["loopCount"].toInt(), 1);
}

void TestLogicPlugins::testLoopValidationZeroCount()
{
    LoopPlugin plugin;
    QString error;
    QJsonObject params = plugin.defaultParams();
    params["loopCount"] = 0;
    QVERIFY2(!plugin.validateParams(params, error), "loopCount=0 should fail validation");
    QVERIFY(!error.isEmpty());

    // Also verify execute fails with loopCount <= 0
    plugin.setParams(QJsonObject{{"loopCount", 0}});
    ImageData input, output;
    QVERIFY2(!plugin.execute(input, output), "execute should return false for loopCount=0");
}

void TestLogicPlugins::testLoopFlowControlType()
{
    LoopPlugin plugin;
    QCOMPARE(plugin.flowControlType(), ControlFlowType::Loop);
    QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.loop"));
    QCOMPARE(plugin.category(), QString("logic"));
}

// =========================================================================
// WhilePlugin
// =========================================================================

void TestLogicPlugins::testWhileNotEmptyTrue()
{
    WhilePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionVariable", "val"},
        {"comparison", "NotEmpty"},
        {"compareValue", ""},
        {"maxIterations", 100}
    });

    ImageData input;
    input.setData("val", QString("hello"));
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), true);
}

void TestLogicPlugins::testWhileNotEmptyFalse()
{
    WhilePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionVariable", "val"},
        {"comparison", "NotEmpty"},
        {"compareValue", ""},
        {"maxIterations", 100}
    });

    ImageData input;
    input.setData("val", QString(""));  // empty value
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), false);
}

void TestLogicPlugins::testWhileEqual()
{
    WhilePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionVariable", "val"},
        {"comparison", "Equal"},
        {"compareValue", "42"},
        {"maxIterations", 100}
    });

    ImageData input;
    input.setData("val", QString("42"));
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), true);

    // Non-equal value
    input.setData("val", QString("99"));
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), false);
}

void TestLogicPlugins::testWhileNotEqual()
{
    WhilePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionVariable", "val"},
        {"comparison", "NotEqual"},
        {"compareValue", "42"},
        {"maxIterations", 100}
    });

    ImageData input;
    input.setData("val", QString("99"));
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), true);

    input.setData("val", QString("42"));
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), false);
}

void TestLogicPlugins::testWhileGreaterThan()
{
    WhilePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionVariable", "val"},
        {"comparison", "GreaterThan"},
        {"compareValue", "5"},
        {"maxIterations", 100}
    });

    ImageData input;
    input.setData("val", QString("10"));
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), true);

    input.setData("val", QString("3"));
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), false);
}

void TestLogicPlugins::testWhileLessThan()
{
    WhilePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{
        {"conditionVariable", "val"},
        {"comparison", "LessThan"},
        {"compareValue", "5"},
        {"maxIterations", 100}
    });

    ImageData input;
    input.setData("val", QString("3"));
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), true);

    input.setData("val", QString("10"));
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("while_result").toBool(), false);
}

void TestLogicPlugins::testWhileValidationEmptyVar()
{
    WhilePlugin plugin;
    QString error;
    QJsonObject params = plugin.defaultParams();
    params["conditionVariable"] = "";
    QVERIFY2(!plugin.validateParams(params, error), "Empty conditionVariable should fail validation");
    QVERIFY(!error.isEmpty());
}

void TestLogicPlugins::testWhileValidationMaxIterations()
{
    WhilePlugin plugin;
    QString error;
    QJsonObject params = plugin.defaultParams();
    params["conditionVariable"] = "val";
    params["maxIterations"] = 0;
    QVERIFY2(!plugin.validateParams(params, error), "maxIterations=0 should fail validation");
    QVERIFY(!error.isEmpty());
}

void TestLogicPlugins::testWhileFlowControlType()
{
    WhilePlugin plugin;
    QCOMPARE(plugin.flowControlType(), ControlFlowType::While);
    QCOMPARE(plugin.moduleId(), QString("com.deeplux.plugin.while"));
    QCOMPARE(plugin.category(), QString("logic"));
}

void TestLogicPlugins::testWhileEndProcess()
{
    WhileEndPlugin plugin;
    QVERIFY(plugin.initialize());
    ImageData input, output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(plugin.flowControlType(), ControlFlowType::WhileEnd);
}

QTEST_MAIN(TestLogicPlugins)
#include "test_logicplugins.moc"
