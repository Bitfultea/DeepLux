#include "core/base/ModuleBase.h"
#include "core/manager/GlobalVarManager.h"
#include "core/model/ImageData.h"
#include "plugins/logic/QueueIn/QueueInPlugin.h"
#include "plugins/logic/QueueOut/QueueOutPlugin.h"

#include <QJsonObject>
#include <QVariant>
#include <QtTest/QtTest>

using namespace DeepLux;

// 阶段 G 行为级验收：QueueIn / QueueOut
// 覆盖：参数影响结果 / 结构化错误 / clone 独立 / 确定性正常+失败样例

class TestQueuePlugins : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // QueueIn
    void testQueueInEnqueuesAndReportsSize();
    void testQueueInEmptyQueueNameFails();
    void testQueueInEmptyDataFails();
    void testQueueInDataVariableParamAffectsSource();
    void testQueueInCloneIndependent();

    // QueueOut
    void testQueueOutDequeuesItem();
    void testQueueOutEmptyQueueReturnsEmptyNotError();
    void testQueueOutPeekOnlyDoesNotRemove();
    void testQueueOutOutputVariableParamAffectsTarget();
    void testQueueOutCloneIndependent();

    // 端到端
    void testQueueInThenOutRoundTrip();
};

void TestQueuePlugins::init() {
    // 清理测试队列，保证确定性
    GlobalVarManager::instance().clearQueue(QStringLiteral("testq"));
}

void TestQueuePlugins::cleanup() {
    GlobalVarManager::instance().clearQueue(QStringLiteral("testq"));
}

static ExecutionResult runModule(ModuleBase& plugin, const QJsonObject& params, const ImageData& input,
                                 ImageData& output) {
    plugin.setParams(params);
    PortValueMap inputs;
    inputs.insert(QStringLiteral("image"), QVariant::fromValue(input));
    const QMap<QString, QVariant> carrierData = input.allData();
    for (auto it = carrierData.constBegin(); it != carrierData.constEnd(); ++it) {
        if (it.key() != QLatin1String("image"))
            inputs.insert(it.key(), it.value());
    }
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = plugin.execute(inputs, outputs, ctx);
    if (outputs.contains(QStringLiteral("image")))
        output = outputs.value(QStringLiteral("image")).value<ImageData>();
    return result;
}

void TestQueuePlugins::testQueueInEnqueuesAndReportsSize() {
    QueueInPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("item_data", QStringLiteral("value1"));
    QJsonObject params{{"queueName", "testq"}, {"dataVariable", ""}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(GlobalVarManager::instance().queueSize(QStringLiteral("testq")), 1);
    QCOMPARE(output.data("queue_testq_size").toInt(), 1);
}

void TestQueuePlugins::testQueueInEmptyQueueNameFails() {
    QueueInPlugin plugin;
    QVERIFY(plugin.initialize());

    // 契约：doValidateParams 拒绝空队列名，setParams 不应用非法值
    QString error;
    QJsonObject badParams{{"queueName", ""}, {"dataVariable", ""}};
    QVERIFY2(!plugin.validateParams(badParams, error), "validateParams must reject empty queueName");
    QVERIFY(!error.isEmpty());

    // setParams 拒绝后保留默认 queueName，process 不会因空名失败
    plugin.setParams(badParams);
    QCOMPARE(plugin.currentParams().value("queueName").toString(), QString("defaultQueue"));

    // 防御路径：绕过校验直接置空，process 必须失败而非误入队
    plugin.setParam("queueName", QString());
    ImageData input, output;
    input.setData("item_data", QStringLiteral("value1"));
    PortValueMap inputs;
    inputs.insert(QStringLiteral("image"), QVariant::fromValue(input));
    inputs.insert(QStringLiteral("item_data"), QStringLiteral("value1"));
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = plugin.execute(inputs, outputs, ctx);
    QVERIFY2(!result.success, "process with empty queueName must fail");
}

void TestQueuePlugins::testQueueInEmptyDataFails() {
    QueueInPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output; // 无 item_data
    QJsonObject params{{"queueName", "testq"}, {"dataVariable", ""}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty data must fail");
    QVERIFY(!result.userMessage.isEmpty());
}

void TestQueuePlugins::testQueueInDataVariableParamAffectsSource() {
    QueueInPlugin plugin;
    QVERIFY(plugin.initialize());

    // dataVariable 指定从 custom_var 读取
    ImageData input, output;
    input.setData("custom_var", QStringLiteral("from_custom"));
    QJsonObject params{{"queueName", "testq"}, {"dataVariable", "custom_var"}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(GlobalVarManager::instance().peekQueue(QStringLiteral("testq")).toString(), QString("from_custom"));
}

void TestQueuePlugins::testQueueInCloneIndependent() {
    QueueInPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"queueName", "testq"}, {"dataVariable", "x"}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    QVERIFY(clone != &plugin);

    // clone 保留参数
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("queueName").toString(), QString("testq"));

    // 修改 clone 参数不影响原实例
    cloneBase->setParam("queueName", "otherq");
    QCOMPARE(plugin.currentParams().value("queueName").toString(), QString("testq"));
    QCOMPARE(cloneBase->currentParams().value("queueName").toString(), QString("otherq"));

    delete clone;
}

void TestQueuePlugins::testQueueOutDequeuesItem() {
    GlobalVarManager::instance().enqueue(QStringLiteral("testq"), QStringLiteral("item_a"));

    QueueOutPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    QJsonObject params{{"queueName", "testq"}, {"outputVariable", "queue_item"}, {"peekOnly", false}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(output.data("queue_item").toString(), QString("item_a"));
    QCOMPARE(output.data("queue_empty").toBool(), false);
    QCOMPARE(GlobalVarManager::instance().queueSize(QStringLiteral("testq")), 0);
}

void TestQueuePlugins::testQueueOutEmptyQueueReturnsEmptyNotError() {
    QueueOutPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    QJsonObject params{{"queueName", "testq"}, {"outputVariable", "queue_item"}, {"peekOnly", false}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    // 空队列不是错误，返回成功且标记 queue_empty
    QVERIFY2(result.success, "empty queue should not be an error");
    QCOMPARE(output.data("queue_empty").toBool(), true);
}

void TestQueuePlugins::testQueueOutPeekOnlyDoesNotRemove() {
    GlobalVarManager::instance().enqueue(QStringLiteral("testq"), QStringLiteral("peek_me"));

    QueueOutPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    QJsonObject params{{"queueName", "testq"}, {"outputVariable", "queue_item"}, {"peekOnly", true}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(output.data("queue_item").toString(), QString("peek_me"));
    // peek 不移除
    QCOMPARE(GlobalVarManager::instance().queueSize(QStringLiteral("testq")), 1);
}

void TestQueuePlugins::testQueueOutOutputVariableParamAffectsTarget() {
    GlobalVarManager::instance().enqueue(QStringLiteral("testq"), QStringLiteral("val"));

    QueueOutPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    QJsonObject params{{"queueName", "testq"}, {"outputVariable", "my_custom_out"}, {"peekOnly", false}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    // outputVariable 参数决定输出键名
    QCOMPARE(output.data("my_custom_out").toString(), QString("val"));
}

void TestQueuePlugins::testQueueOutCloneIndependent() {
    QueueOutPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"queueName", "testq"}, {"outputVariable", "out"}, {"peekOnly", true}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("outputVariable").toString(), QString("out"));

    cloneBase->setParam("outputVariable", "changed");
    QCOMPARE(plugin.currentParams().value("outputVariable").toString(), QString("out"));

    delete clone;
}

void TestQueuePlugins::testQueueInThenOutRoundTrip() {
    // 端到端：QueueIn 入队 → QueueOut 出队，数据一致
    QueueInPlugin inPlugin;
    QueueOutPlugin outPlugin;
    QVERIFY(inPlugin.initialize());
    QVERIFY(outPlugin.initialize());

    ImageData inInput, inOutput;
    inInput.setData("item_data", QStringLiteral("roundtrip_value"));
    QJsonObject inParams{{"queueName", "testq"}, {"dataVariable", ""}};
    QVERIFY(runModule(inPlugin, inParams, inInput, inOutput).success);

    ImageData outInput, outOutput;
    QJsonObject outParams{{"queueName", "testq"}, {"outputVariable", "queue_item"}, {"peekOnly", false}};
    QVERIFY(runModule(outPlugin, outParams, outInput, outOutput).success);

    QCOMPARE(outOutput.data("queue_item").toString(), QString("roundtrip_value"));
    QCOMPARE(GlobalVarManager::instance().queueSize(QStringLiteral("testq")), 0);
}

QTEST_MAIN(TestQueuePlugins)
#include "test_queueplugins.moc"
