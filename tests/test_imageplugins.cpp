#include "core/base/ModuleBase.h"
#include "core/model/ImageData.h"
#include "plugins/detection/Matching/MatchingPlugin.h"
#include "plugins/image_processing/PerProcessing/PerProcessingPlugin.h"

#include <QJsonObject>
#include <QVariant>
#include <QtTest/QtTest>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

// 阶段 G 行为级验收：PerProcessing / Matching（OpenCV 依赖）
// 覆盖：参数影响结果 / 结构化错误 / clone 独立 / 确定性正常+失败样例

class TestImagePlugins : public QObject {
    Q_OBJECT

private slots:
    // PerProcessing
    void testPerProcessingGaussianBlurDeterministic();
    void testPerProcessingKernelSizeAffectsResult();
    void testPerProcessingEmptyImageFails();
    void testPerProcessingValidateRejectsBadKernel();
    void testPerProcessingCloneIndependent();

    // Matching
    void testMatchingFindsTemplateDeterministic();
    void testMatchingEmptyImageFails();
    void testMatchingCloneIndependent();
};

#ifdef DEEPLUX_HAS_OPENCV
// 构造确定性合成图像：渐变 + 中心亮斑
static ImageData makeSyntheticImage(int width = 200, int height = 200) {
    cv::Mat mat(height, width, CV_8UC3, cv::Scalar(30, 30, 30));
    // 中心画一个白色矩形作为特征
    cv::rectangle(mat, cv::Point(width / 2 - 20, height / 2 - 20), cv::Point(width / 2 + 20, height / 2 + 20),
                  cv::Scalar(255, 255, 255), -1);
    ImageData data;
    data.setMat(mat);
    return data;
}
#endif

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

// ===== PerProcessing =====

void TestImagePlugins::testPerProcessingGaussianBlurDeterministic() {
#ifdef DEEPLUX_HAS_OPENCV
    PerProcessingPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input = makeSyntheticImage();
    QJsonObject params{{"processType", "GaussianBlur"}, {"kernelSize", 5}, {"sigmaX", 1.5}, {"iterations", 1}};

    ImageData out1, out2;
    QVERIFY(runModule(plugin, params, input, out1).success);
    QVERIFY(runModule(plugin, params, input, out2).success);

    // 确定性：相同输入+参数 → 相同输出
    cv::Mat m1 = out1.toMat();
    cv::Mat m2 = out2.toMat();
    QVERIFY(!m1.empty());
    QCOMPARE(m1.cols, m2.cols);
    QCOMPARE(m1.rows, m2.rows);
    // 逐像素比较（确定性）
    cv::Mat diff;
    cv::absdiff(m1, m2, diff);
    QCOMPARE(cv::countNonZero(diff.reshape(1)), 0);
#else
    QSKIP("OpenCV not available");
#endif
}

void TestImagePlugins::testPerProcessingKernelSizeAffectsResult() {
#ifdef DEEPLUX_HAS_OPENCV
    ImageData input = makeSyntheticImage();

    // 用两个独立实例排除状态串扰
    PerProcessingPlugin pluginSmall, pluginLarge;
    QVERIFY(pluginSmall.initialize());
    QVERIFY(pluginLarge.initialize());

    QJsonObject small{{"processType", "GaussianBlur"}, {"kernelSize", 3}, {"sigmaX", 1.0}, {"iterations", 1}};
    QJsonObject large{{"processType", "GaussianBlur"}, {"kernelSize", 31}, {"sigmaX", 8.0}, {"iterations", 1}};

    ImageData outSmall, outLarge;
    QVERIFY(runModule(pluginSmall, small, input, outSmall).success);
    QVERIFY(runModule(pluginLarge, large, input, outLarge).success);

    // 确认参数确实被应用
    QCOMPARE(pluginSmall.currentParams().value("kernelSize").toInt(), 3);
    QCOMPARE(pluginLarge.currentParams().value("kernelSize").toInt(), 31);

    // 不同 kernelSize → 不同模糊结果（证明参数影响结果）
    cv::Mat mSmall = outSmall.toMat();
    cv::Mat mLarge = outLarge.toMat();
    QVERIFY(!mSmall.empty());
    QVERIFY(!mLarge.empty());
    cv::Mat diff;
    cv::absdiff(mSmall, mLarge, diff);
    QVERIFY2(cv::countNonZero(diff.reshape(1)) > 0, "different kernelSize must produce different results");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestImagePlugins::testPerProcessingEmptyImageFails() {
#ifdef DEEPLUX_HAS_OPENCV
    PerProcessingPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空图像（无 mat）
    QJsonObject params{{"processType", "GaussianBlur"}, {"kernelSize", 5}, {"sigmaX", 1.5}, {"iterations", 1}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty image must fail");
    QVERIFY(!result.userMessage.isEmpty());
#else
    QSKIP("OpenCV not available");
#endif
}

void TestImagePlugins::testPerProcessingValidateRejectsBadKernel() {
    PerProcessingPlugin plugin;
    QString error;
    QJsonObject bad{{"processType", "GaussianBlur"}, {"kernelSize", 0}, {"sigmaX", 1.0}, {"iterations", 1}};
    QVERIFY2(!plugin.validateParams(bad, error), "kernelSize < 1 must be rejected");
    QVERIFY(!error.isEmpty());
}

void TestImagePlugins::testPerProcessingCloneIndependent() {
    PerProcessingPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"processType", "Canny"}, {"kernelSize", 7}, {"sigmaX", 2.0}, {"iterations", 2}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("processType").toString(), QString("Canny"));
    QCOMPARE(cloneBase->currentParams().value("kernelSize").toInt(), 7);

    cloneBase->setParam("kernelSize", 99);
    QCOMPARE(plugin.currentParams().value("kernelSize").toInt(), 7);
    delete clone;
}

// ===== Matching =====

void TestImagePlugins::testMatchingFindsTemplateDeterministic() {
#ifdef DEEPLUX_HAS_OPENCV
    MatchingPlugin plugin;
    QVERIFY(plugin.initialize());

    // 合成图像含中心白色矩形，默认模板取中心区域 → 应能匹配到自身
    ImageData input = makeSyntheticImage();
    QJsonObject params{{"matchThreshold", 0.5}, {"maxMatches", 5}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QVERIFY(output.data("match_count").toInt() >= 1);
    QVERIFY(output.hasData("match_x"));
    QVERIFY(output.hasData("match_y"));
#else
    QSKIP("OpenCV not available");
#endif
}

void TestImagePlugins::testMatchingEmptyImageFails() {
#ifdef DEEPLUX_HAS_OPENCV
    MatchingPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空
    QJsonObject params{{"matchThreshold", 0.5}, {"maxMatches", 5}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty image must fail");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestImagePlugins::testMatchingCloneIndependent() {
    MatchingPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"matchThreshold", 0.7}, {"maxMatches", 3}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("maxMatches").toInt(), 3);

    cloneBase->setParam("maxMatches", 99);
    QCOMPARE(plugin.currentParams().value("maxMatches").toInt(), 3);
    delete clone;
}

QTEST_MAIN(TestImagePlugins)
#include "test_imageplugins.moc"
