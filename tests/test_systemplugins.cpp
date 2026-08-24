#include "core/base/ModuleBase.h"
#include "core/model/ImageData.h"
#include "plugins/system/DataCheck/DataCheckPlugin.h"
#include "plugins/system/SaveData/SaveDataPlugin.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QVariant>
#include <QtTest/QtTest>

using namespace DeepLux;

// 阶段 G 行为级验收：DataCheck / SaveData
// 覆盖：参数影响结果 / 结构化错误 / clone 独立 / 确定性正常+失败样例

class TestSystemPlugins : public QObject {
    Q_OBJECT

private slots:
    // DataCheck
    void testDataCheckRangePass();
    void testDataCheckRangeFailOutputsCheckPassedFalse();
    void testDataCheckRangeListAllChecked();
    void testDataCheckLengthPass();
    void testDataCheckLengthFail();
    void testDataCheckNullPass();
    void testDataCheckMissingDataFails();
    void testDataCheckParamsAffectResult();
    void testDataCheckValidateRejectsInvertedRange();
    void testDataCheckCloneIndependent();

    // SaveData
    void testSaveDataWritesJsonFile();
    void testSaveDataOutputsCsvFile();
    void testSaveDataEmptyPathFails();
    void testSaveDataAppendModeMergesJson();
    void testSaveDataValidateRejectsBadFormat();
    void testSaveDataCloneIndependent();
};

static ExecutionResult runModule(ModuleBase& plugin, const QJsonObject& params,
                                 const ImageData& input, ImageData& output) {
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

// ===== DataCheck =====

void TestSystemPlugins::testDataCheckRangePass() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("check_data", 50.0);
    QJsonObject params{{"checkType", "Range"}, {"minValue", 0.0}, {"maxValue", 100.0}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(output.data("check_passed").toBool(), true);
    QCOMPARE(output.data("check_error").toString(), QString());
}

void TestSystemPlugins::testDataCheckRangeFailOutputsCheckPassedFalse() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("check_data", 150.0); // 超出 [0,100]
    QJsonObject params{{"checkType", "Range"}, {"minValue", 0.0}, {"maxValue", 100.0}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    // DataCheck 设计上：校验失败通过 check_passed 输出传达，模块本身仍成功执行
    QVERIFY(result.success);
    QCOMPARE(output.data("check_passed").toBool(), false);
    QVERIFY(!output.data("check_error").toString().isEmpty());
}

void TestSystemPlugins::testDataCheckRangeListAllChecked() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("check_data", QVariantList{10.0, 20.0, 200.0}); // 200 超范围
    QJsonObject params{{"checkType", "Range"}, {"minValue", 0.0}, {"maxValue", 100.0}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY(result.success);
    QCOMPARE(output.data("check_passed").toBool(), false);
}

void TestSystemPlugins::testDataCheckLengthPass() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("check_data", QStringLiteral("hello")); // 长度 5
    QJsonObject params{{"checkType", "Length"}, {"minLength", 1}, {"maxLength", 10}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY(result.success);
    QCOMPARE(output.data("check_passed").toBool(), true);
}

void TestSystemPlugins::testDataCheckLengthFail() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("check_data", QStringLiteral("hi")); // 长度 2 < minLength 5
    QJsonObject params{{"checkType", "Length"}, {"minLength", 5}, {"maxLength", 10}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY(result.success);
    QCOMPARE(output.data("check_passed").toBool(), false);
}

void TestSystemPlugins::testDataCheckNullPass() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("check_data", QStringLiteral("not_null"));
    QJsonObject params{{"checkType", "Null"}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY(result.success);
    QCOMPARE(output.data("check_passed").toBool(), true);
}

void TestSystemPlugins::testDataCheckMissingDataFails() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output; // 无 check_data
    QJsonObject params{{"checkType", "Range"}, {"minValue", 0.0}, {"maxValue", 100.0}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "missing check_data must fail");
    QVERIFY(!result.userMessage.isEmpty());
}

void TestSystemPlugins::testDataCheckParamsAffectResult() {
    // 同一输入，不同参数范围 → 不同结果（证明参数影响结果）
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input;
    input.setData("check_data", 50.0);

    ImageData out1, out2;
    QJsonObject narrow{{"checkType", "Range"}, {"minValue", 0.0}, {"maxValue", 10.0}};
    QJsonObject wide{{"checkType", "Range"}, {"minValue", 0.0}, {"maxValue", 100.0}};

    QVERIFY(runModule(plugin, narrow, input, out1).success);
    QCOMPARE(out1.data("check_passed").toBool(), false); // 50 不在 [0,10]

    QVERIFY(runModule(plugin, wide, input, out2).success);
    QCOMPARE(out2.data("check_passed").toBool(), true); // 50 在 [0,100]
}

void TestSystemPlugins::testDataCheckValidateRejectsInvertedRange() {
    DataCheckPlugin plugin;
    QString error;
    QJsonObject bad{{"checkType", "Range"}, {"minValue", 100.0}, {"maxValue", 0.0}};
    QVERIFY2(!plugin.validateParams(bad, error), "inverted range must be rejected");
    QVERIFY(!error.isEmpty());
}

void TestSystemPlugins::testDataCheckCloneIndependent() {
    DataCheckPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"checkType", "Range"}, {"minValue", 5.0}, {"maxValue", 50.0}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("minValue").toDouble(), 5.0);

    cloneBase->setParam("minValue", 99.0);
    QCOMPARE(plugin.currentParams().value("minValue").toDouble(), 5.0);
    delete clone;
}

// ===== SaveData =====

void TestSystemPlugins::testSaveDataWritesJsonFile() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath("out.json");

    SaveDataPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("result_value", 42.0);
    input.setData("result_name", QStringLiteral("test"));
    QJsonObject params{{"filePath", path}, {"fileFormat", "json"}, {"appendMode", false}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(output.data("save_result").toBool(), true);

    // 验证文件内容
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object().value("result_value").toDouble(), 42.0);
    QCOMPARE(doc.object().value("result_name").toString(), QString("test"));
}

void TestSystemPlugins::testSaveDataOutputsCsvFile() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath("out.csv");

    SaveDataPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output;
    input.setData("col_a", QStringLiteral("1"));
    input.setData("col_b", QStringLiteral("2"));
    QJsonObject params{{"filePath", path}, {"fileFormat", "csv"}, {"appendMode", false}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(file.readAll());
    file.close();
    // CSV 应含表头和值
    QVERIFY(content.contains("col_a"));
    QVERIFY(content.contains("col_b"));
}

void TestSystemPlugins::testSaveDataEmptyPathFails() {
    SaveDataPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input, output; // 无 file_path，参数 filePath 也为空
    QJsonObject params{{"filePath", ""}, {"fileFormat", "json"}, {"appendMode", false}};

    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty file path must fail");
    QVERIFY(!result.userMessage.isEmpty());
}

void TestSystemPlugins::testSaveDataAppendModeMergesJson() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString path = tmp.filePath("append.json");

    SaveDataPlugin plugin;
    QVERIFY(plugin.initialize());

    // 第一次写入
    ImageData input1, output1;
    input1.setData("first_key", QStringLiteral("first_value"));
    QJsonObject params1{{"filePath", path}, {"fileFormat", "json"}, {"appendMode", false}};
    QVERIFY(runModule(plugin, params1, input1, output1).success);

    // 追加模式写入第二个键
    ImageData input2, output2;
    input2.setData("second_key", QStringLiteral("second_value"));
    QJsonObject params2{{"filePath", path}, {"fileFormat", "json"}, {"appendMode", true}};
    QVERIFY(runModule(plugin, params2, input2, output2).success);

    // 验证两个键都存在（追加合并）
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object().value("first_key").toString(), QString("first_value"));
    QCOMPARE(doc.object().value("second_key").toString(), QString("second_value"));
}

void TestSystemPlugins::testSaveDataValidateRejectsBadFormat() {
    SaveDataPlugin plugin;
    QString error;
    QJsonObject bad{{"filePath", "x.txt"}, {"fileFormat", "xml"}, {"appendMode", false}};
    QVERIFY2(!plugin.validateParams(bad, error), "invalid format must be rejected");
    QVERIFY(!error.isEmpty());
}

void TestSystemPlugins::testSaveDataCloneIndependent() {
    SaveDataPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"filePath", "/tmp/a.json"}, {"fileFormat", "csv"}, {"appendMode", true}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("fileFormat").toString(), QString("csv"));

    cloneBase->setParam("fileFormat", "json");
    QCOMPARE(plugin.currentParams().value("fileFormat").toString(), QString("csv"));
    delete clone;
}

QTEST_MAIN(TestSystemPlugins)
#include "test_systemplugins.moc"
