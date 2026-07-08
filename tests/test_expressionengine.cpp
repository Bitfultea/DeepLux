#include <QtTest/QtTest>
#include <QMap>
#include <QVariant>

#include "core/common/ExpressionEngine.h"
#include "core/common/VarModel.h"

using namespace DeepLux;

class TestExpressionEngine : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    // Instance-based compile + evaluate
    void testCompileAndEvaluateBooleanTrue();
    void testCompileAndEvaluateBooleanFalse();
    void testCompileAndEvaluateNumeric();
    void testCompileAndEvaluateStringLiteralDouble();
    void testCompileAndEvaluateStringLiteralSingle();
    void testCompileAndEvaluatePlainStringFallback();

    // Compile failures
    void testCompileEmptyFails();
    void testCompileInvalidBracesFails();

    // Static evaluateExpression
    void testStaticEvaluateBoolean();
    void testStaticEvaluateNumeric();
    void testStaticEvaluateVariable();
    void testStaticEvaluateFallbackString();

    // Static validateExpression
    void testValidateEmptyFails();
    void testValidateMismatchedBracesFails();
    void testValidateValidExpression();

    // VarModel integration
    void testVarModelEvaluateExpression();
    void testVarModelEmptyExpressionReturnsValue();
    void testVarModelNumericExpression();
    void testVarModelBoolExpression();
    void testVarModelConvertToExpectedType();
    void testVarModelDataTypeToString();
    void testVarModelStringToDataType();
};

void TestExpressionEngine::initTestCase()
{
    qDebug() << "=== TestExpressionEngine Start ===";
}

void TestExpressionEngine::cleanupTestCase()
{
    qDebug() << "=== TestExpressionEngine End ===";
}

// =========================================================================
// Instance-based compile + evaluate
// =========================================================================

void TestExpressionEngine::testCompileAndEvaluateBooleanTrue()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY2(engine.compile("true"), "compile('true') should succeed");
    QVERIFY2(engine.evaluate(), "evaluate should succeed for 'true'");
    QCOMPARE(engine.result().toBool(), true);
    QVERIFY(engine.errorMessage().isEmpty());
}

void TestExpressionEngine::testCompileAndEvaluateBooleanFalse()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY(engine.compile("false"));
    QVERIFY(engine.evaluate());
    QCOMPARE(engine.result().toBool(), false);
}

void TestExpressionEngine::testCompileAndEvaluateNumeric()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY(engine.compile("42"));
    QVERIFY(engine.evaluate());
    QCOMPARE(engine.result().toDouble(), 42.0);
}

void TestExpressionEngine::testCompileAndEvaluateStringLiteralDouble()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY(engine.compile("\"hello\""));
    QVERIFY(engine.evaluate());
    QCOMPARE(engine.result().toString(), QString("hello"));
}

void TestExpressionEngine::testCompileAndEvaluateStringLiteralSingle()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY(engine.compile("'world'"));
    QVERIFY(engine.evaluate());
    QCOMPARE(engine.result().toString(), QString("world"));
}

void TestExpressionEngine::testCompileAndEvaluatePlainStringFallback()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY(engine.compile("some_text"));
    QVERIFY(engine.evaluate());
    QCOMPARE(engine.result().toString(), QString("some_text"));
}

// =========================================================================
// Compile failures
// =========================================================================

void TestExpressionEngine::testCompileEmptyFails()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY2(!engine.compile(""), "compile('') should fail");
    QVERIFY(!engine.errorMessage().isEmpty());
}

void TestExpressionEngine::testCompileInvalidBracesFails()
{
    ExpressionEngine& engine = ExpressionEngine::instance();
    QVERIFY2(!engine.compile("{{invalid}}"), "compile with {{}} should fail");
    QVERIFY(!engine.errorMessage().isEmpty());
}

// =========================================================================
// Static evaluateExpression
// =========================================================================

void TestExpressionEngine::testStaticEvaluateBoolean()
{
    QCOMPARE(ExpressionEngine::evaluateExpression("true").toBool(), true);
    QCOMPARE(ExpressionEngine::evaluateExpression("false").toBool(), false);
}

void TestExpressionEngine::testStaticEvaluateNumeric()
{
    QCOMPARE(ExpressionEngine::evaluateExpression("42").toDouble(), 42.0);
    QCOMPARE(ExpressionEngine::evaluateExpression("3.14").toDouble(), 3.14);
}

void TestExpressionEngine::testStaticEvaluateVariable()
{
    QMap<QString, QVariant> vars;
    vars["x"] = 42;
    vars["name"] = QString("test");
    QCOMPARE(ExpressionEngine::evaluateExpression("x", vars).toInt(), 42);
    QCOMPARE(ExpressionEngine::evaluateExpression("name", vars).toString(), QString("test"));
}

void TestExpressionEngine::testStaticEvaluateFallbackString()
{
    QMap<QString, QVariant> emptyVars;
    QCOMPARE(ExpressionEngine::evaluateExpression("unknown", emptyVars).toString(), QString("unknown"));
}

// =========================================================================
// Static validateExpression
// =========================================================================

void TestExpressionEngine::testValidateEmptyFails()
{
    QString error;
    QVERIFY2(!ExpressionEngine::validateExpression("", error), "Empty expression should fail validation");
    QVERIFY(!error.isEmpty());
}

void TestExpressionEngine::testValidateMismatchedBracesFails()
{
    QString error;
    QVERIFY2(!ExpressionEngine::validateExpression("{{", error), "Mismatched braces should fail");
    QVERIFY(!error.isEmpty());

    error.clear();
    QVERIFY2(!ExpressionEngine::validateExpression("}}", error), "Mismatched braces should fail");
    QVERIFY(!error.isEmpty());
}

void TestExpressionEngine::testValidateValidExpression()
{
    QString error;
    QVERIFY2(ExpressionEngine::validateExpression("true", error), "Valid expression should pass");
    QVERIFY2(error.isEmpty(), "No error for valid expression");

    QVERIFY2(ExpressionEngine::validateExpression("1 + 2", error), "Valid expression should pass");
}

// =========================================================================
// VarModel integration
// =========================================================================

void TestExpressionEngine::testVarModelEvaluateExpression()
{
    VarModel model;
    model.setName("testVar");
    model.setDataType(VarDataType::String);
    model.setExpression("hello");
    QVERIFY(model.compileExpression("proj1", "mod1"));
    QVariant result = model.evaluateExpression();
    // "hello" is not numeric, not bool literal, so returns m_value (which is empty)
    QCOMPARE(result.toString(), QString(""));
}

void TestExpressionEngine::testVarModelEmptyExpressionReturnsValue()
{
    VarModel model("myVar", VarDataType::Int, 42);
    model.setExpression("");
    QVERIFY(model.compileExpression("proj", "mod"));
    QCOMPARE(model.evaluateExpression().toInt(), 42);

    model.setExpression("NULL");
    QVERIFY(model.compileExpression("proj", "mod"));
    QCOMPARE(model.evaluateExpression().toInt(), 42);
}

void TestExpressionEngine::testVarModelNumericExpression()
{
    VarModel model("numVar", VarDataType::Double, 0.0);
    model.setExpression("3.14");
    QVERIFY(model.compileExpression("proj", "mod"));
    QCOMPARE(model.evaluateExpression().toDouble(), 3.14);
}

void TestExpressionEngine::testVarModelBoolExpression()
{
    VarModel model("boolVar", VarDataType::Bool, false);
    model.setExpression("true");
    QVERIFY(model.compileExpression("proj", "mod"));
    QCOMPARE(model.evaluateExpression().toBool(), true);

    model.setExpression("false");
    QVERIFY(model.compileExpression("proj", "mod"));
    QCOMPARE(model.evaluateExpression().toBool(), false);
}

void TestExpressionEngine::testVarModelConvertToExpectedType()
{
    // Double conversion
    QCOMPARE(VarModel::convertToExpectedType(42, VarDataType::Double).toDouble(), 42.0);

    // Int conversion (QVariant rounds to nearest int)
    QCOMPARE(VarModel::convertToExpectedType(3.7, VarDataType::Int).toInt(), 4);

    // String conversion
    QCOMPARE(VarModel::convertToExpectedType(42, VarDataType::String).toString(), QString("42"));

    // Bool conversion
    QCOMPARE(VarModel::convertToExpectedType(true, VarDataType::Bool).toBool(), true);
    QCOMPARE(VarModel::convertToExpectedType(false, VarDataType::Bool).toBool(), false);

    // Invalid value returns empty QVariant
    QVERIFY(!VarModel::convertToExpectedType(QVariant(), VarDataType::Int).isValid());
}

void TestExpressionEngine::testVarModelDataTypeToString()
{
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Int), QString("int"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Double), QString("double"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::String), QString("string"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Bool), QString("bool"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::IntArray), QString("int[]"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::DoubleArray), QString("double[]"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::StringArray), QString("string[]"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::BoolArray), QString("bool[]"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Image), QString("Image"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Region), QString("Region"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Point), QString("Point"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Line), QString("Line"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Circle), QString("Circle"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::Rect), QString("Rect"));
    QCOMPARE(VarModel::dataTypeToString(VarDataType::None), QString("None"));
}

void TestExpressionEngine::testVarModelStringToDataType()
{
    QCOMPARE(VarModel::stringToDataType("int"), VarDataType::Int);
    QCOMPARE(VarModel::stringToDataType("double"), VarDataType::Double);
    QCOMPARE(VarModel::stringToDataType("string"), VarDataType::String);
    QCOMPARE(VarModel::stringToDataType("bool"), VarDataType::Bool);
    QCOMPARE(VarModel::stringToDataType("int[]"), VarDataType::IntArray);
    QCOMPARE(VarModel::stringToDataType("double[]"), VarDataType::DoubleArray);
    QCOMPARE(VarModel::stringToDataType("string[]"), VarDataType::StringArray);
    QCOMPARE(VarModel::stringToDataType("bool[]"), VarDataType::BoolArray);
    QCOMPARE(VarModel::stringToDataType("Image"), VarDataType::Image);
    QCOMPARE(VarModel::stringToDataType("Region"), VarDataType::Region);
    QCOMPARE(VarModel::stringToDataType("Point"), VarDataType::Point);
    QCOMPARE(VarModel::stringToDataType("Line"), VarDataType::Line);
    QCOMPARE(VarModel::stringToDataType("Circle"), VarDataType::Circle);
    QCOMPARE(VarModel::stringToDataType("Rect"), VarDataType::Rect);
    QCOMPARE(VarModel::stringToDataType("unknown"), VarDataType::None);
}

QTEST_MAIN(TestExpressionEngine)
#include "test_expressionengine.moc"
