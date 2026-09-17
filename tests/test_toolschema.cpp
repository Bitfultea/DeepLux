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

    // SAM 批3：sam_predict / sam_annotate schema 形状与危险级口径
    void testSamToolsSchema()
    {
        QVERIFY(ToolSchema::instance().hasTool("sam_predict"));
        QVERIFY(ToolSchema::instance().hasTool("sam_annotate"));

        const ToolDefinition predict = ToolSchema::instance().findTool("sam_predict");
        QVERIFY(!predict.dangerous); // 不写文件；本地推理非危险级
        QVERIFY(!predict.description.isEmpty());
        bool imagePathRequired = false;
        bool pointsOptional = false;
        bool boxOptional = false;
        for (const ToolParam& p : predict.params) {
            if (p.name == QLatin1String("imagePath"))
                imagePathRequired = p.required;
            if (p.name == QLatin1String("points"))
                pointsOptional = !p.required;
            if (p.name == QLatin1String("box"))
                boxOptional = !p.required;
        }
        QVERIFY(imagePathRequired);
        QVERIFY(pointsOptional);
        QVERIFY(boxOptional);
        // OpenAI function 形状可序列化且 required 列表正确
        const QJsonObject f = predict.toOpenAIFunction()["function"].toObject();
        QCOMPARE(f["name"].toString(), QString("sam_predict"));
        QVERIFY(f["parameters"].toObject()["properties"].toObject().contains("imagePath"));
        QVERIFY(f["parameters"].toObject()["required"].toArray().contains(QJsonValue("imagePath")));

        const ToolDefinition annotate = ToolSchema::instance().findTool("sam_annotate");
        QVERIFY(annotate.dangerous); // 写标注会话文件 → 与 save_project 同口径需确认
        bool sessionPathRequired = false;
        bool itemsRequired = false;
        for (const ToolParam& p : annotate.params) {
            if (p.name == QLatin1String("sessionPath"))
                sessionPathRequired = p.required;
            if (p.name == QLatin1String("items"))
                itemsRequired = p.required;
        }
        QVERIFY(sessionPathRequired);
        QVERIFY(itemsRequired);
    }
};

QTEST_MAIN(TestToolSchema)
#include "test_toolschema.moc"
