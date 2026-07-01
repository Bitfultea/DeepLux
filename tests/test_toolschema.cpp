#include <QtTest/QtTest>
#include "core/agent/ToolSchema.h"

using namespace DeepLux;

class TestToolSchema : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        ToolSchema::instance().registerDefaultTools();
    }

    void testHasTool()
    {
        QVERIFY(ToolSchema::instance().hasTool("create_project"));
        QVERIFY(ToolSchema::instance().hasTool("add_module"));
        QVERIFY(ToolSchema::instance().hasTool("run_flow"));
        QVERIFY(!ToolSchema::instance().hasTool("nonexistent"));
    }

    void testFindTool()
    {
        ToolDefinition t = ToolSchema::instance().findTool("create_project");
        QCOMPARE(t.name, QString("create_project"));
        QCOMPARE(t.params.size(), 1);
        QCOMPARE(t.params[0].name, QString("name"));
    }

    void testOpenAIFunctionFormat()
    {
        ToolDefinition t = ToolSchema::instance().findTool("run_flow");
        QJsonObject func = t.toOpenAIFunction();
        QVERIFY(func.contains("function"));
        QJsonObject f = func["function"].toObject();
        QCOMPARE(f["name"].toString(), QString("run_flow"));
        QVERIFY(f.contains("parameters"));
    }

    void testDangerousFlag()
    {
        ToolDefinition remove = ToolSchema::instance().findTool("remove_module");
        QVERIFY(remove.dangerous);

        ToolDefinition add = ToolSchema::instance().findTool("add_module");
        QVERIFY(!add.dangerous);
    }

    void testAllTools()
    {
        auto tools = ToolSchema::instance().allTools();
        QVERIFY(tools.size() >= 10);
    }
};

QTEST_MAIN(TestToolSchema)
#include "test_toolschema.moc"
