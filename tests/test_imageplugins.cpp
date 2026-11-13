#include "core/base/ModuleBase.h"
#include "core/deeplux/PayloadTypes.h"
#include "core/model/ImageData.h"
#include "plugins/detection/Matching/MatchingPlugin.h"
#include "plugins/image_processing/Blob/BlobPlugin.h"
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
    void testPerProcessingBuiltinsPreserveCoordinateProvenance();

    // Matching
    void testMatchingFindsTemplateDeterministic();
    void testMatchingEmptyImageFails();
    void testMatchingCloneIndependent();

    // Blob output freshness
    void testBlobFailureClearsInheritedLargestBlobCenter();
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

void TestImagePlugins::testPerProcessingBuiltinsPreserveCoordinateProvenance() {
#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat pixels(47, 61, CV_8UC3);
    for (int y = 0; y < pixels.rows; ++y) {
        for (int x = 0; x < pixels.cols; ++x)
            pixels.at<cv::Vec3b>(y, x) =
                cv::Vec3b(uchar((3 * x + y) % 256), uchar((x + 5 * y) % 256), uchar((7 * x + 2 * y) % 256));
    }
    ImageData input;
    input.setMat(pixels);
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("per-processing-parent");
    provenance.sourceSize = QSize(200, 150);
    const ImageCoordinateProvenance::Matrix3 matrix{{1.5, 0.0, 7.0, 0.0, 2.0, 9.0, 0.0, 0.0, 1.0}};
    QVERIFY(provenance.composeLocalToSource(matrix));
    input.setCoordinateProvenance(provenance);

    PerProcessingPlugin plugin;
    QVERIFY(plugin.initialize());
    const QStringList operations{
        QStringLiteral("GaussianBlur"), QStringLiteral("MedianBlur"), QStringLiteral("BilateralFilter"),
        QStringLiteral("Sobel"),        QStringLiteral("Laplacian"),  QStringLiteral("Canny"),
        QStringLiteral("Dilate"),       QStringLiteral("Erode"),      QStringLiteral("Open"),
        QStringLiteral("Close"),        QStringLiteral("Gradient")};
    for (const QString& operation : operations) {
        ImageData output;
        const QJsonObject params{{QStringLiteral("processType"), operation},
                                 {QStringLiteral("kernelSize"), 5},
                                 {QStringLiteral("sigmaX"), 1.5},
                                 {QStringLiteral("iterations"), 1}};
        QVERIFY2(runModule(plugin, params, input, output).success, qPrintable(operation));
        QCOMPARE(output.width(), input.width());
        QCOMPARE(output.height(), input.height());
        const ImageCoordinateProvenance actual = output.coordinateProvenance();
        QVERIFY(actual.isValid());
        QCOMPARE(actual.sourceImageId, provenance.sourceImageId);
        QCOMPARE(actual.sourceSize, provenance.sourceSize);
        const auto actualMatrix = actual.localToSourceMatrix();
        for (size_t i = 0; i < matrix.size(); ++i)
            QCOMPARE(actualMatrix[i], matrix[i]);
        for (const QPointF point : {QPointF(3, 5), QPointF(57, 43)})
            QCOMPARE(actual.mapLocalToSource(point).value(), provenance.mapLocalToSource(point).value());
    }
#else
    QSKIP("OpenCV not available");
#endif
}

// ===== Matching =====

void TestImagePlugins::testMatchingFindsTemplateDeterministic() {
#ifdef DEEPLUX_HAS_OPENCV
    MatchingPlugin plugin;
    QVERIFY(plugin.initialize());

    // 合成图像含中心白色矩形，默认模板取中心区域 → 应能匹配到自身
    ImageData input = makeSyntheticImage();
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
    provenance.sourceSize = QSize(640, 480);
    const ImageCoordinateProvenance::Matrix3 matrix{{1.5, 0.0, 13.0, 0.0, 2.0, 17.0, 0.0, 0.0, 1.0}};
    QVERIFY(provenance.composeLocalToSource(matrix));
    input.setCoordinateProvenance(provenance);
    QJsonObject params{{"matchThreshold", 0.5}, {"maxMatches", 5}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(output.width(), input.width());
    QCOMPARE(output.height(), input.height());
    const ImageCoordinateProvenance outputProvenance = output.coordinateProvenance();
    QVERIFY(outputProvenance.isValid());
    QCOMPARE(outputProvenance.sourceImageId, provenance.sourceImageId);
    QCOMPARE(outputProvenance.sourceSize, provenance.sourceSize);
    QCOMPARE(outputProvenance.localToSourceMatrix(), provenance.localToSourceMatrix());
    for (const QPointF point : {QPointF(0.0, 0.0), QPointF(input.width() - 1.0, input.height() - 1.0)}) {
        const auto expectedPoint = provenance.mapLocalToSource(point);
        const auto actualPoint = outputProvenance.mapLocalToSource(point);
        QVERIFY(expectedPoint);
        QVERIFY(actualPoint);
        QCOMPARE(actualPoint.value(), expectedPoint.value());
    }
    QVERIFY(output.data("match_count").toInt() >= 1);
    QVERIFY(output.hasData("match_x"));
    QVERIFY(output.hasData("match_y"));
    QVERIFY(output.hasData("match_score"));
    const double score = output.data("match_score").toDouble();
    QVERIFY(score > params["matchThreshold"].toDouble());
    QVERIFY(score <= 1.0);
    QVERIFY(output.data("detections").canConvert<DetectionList>());
    const DetectionList detections = output.data("detections").value<DetectionList>();
    QVERIFY(!detections.isEmpty());
    const Detection& first = detections.items.first();
    QCOMPARE(first.x, output.data("match_x").toDouble());
    QCOMPARE(first.y, output.data("match_y").toDouble());
    QCOMPARE(first.width, output.data("match_width").toDouble());
    QCOMPARE(first.height, output.data("match_height").toDouble());
    QCOMPARE(first.score, score);
    QCOMPARE(first.label, QStringLiteral("Match 1"));
    const auto sourceBounds =
        outputProvenance.mapRectToSourceBounds(QRectF(first.x, first.y, first.width, first.height));
    QVERIFY(sourceBounds);
    const auto expectedSourceBounds =
        provenance.mapRectToSourceBounds(QRectF(first.x, first.y, first.width, first.height));
    QVERIFY(expectedSourceBounds);
    QCOMPARE(sourceBounds.value(), expectedSourceBounds.value());
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

void TestImagePlugins::testBlobFailureClearsInheritedLargestBlobCenter() {
#ifdef DEEPLUX_HAS_OPENCV
    BlobPlugin plugin;
    QVERIFY(plugin.initialize());
    ImageData input(cv::Mat::zeros(100, 120, CV_8UC1));
    input.setData(QStringLiteral("largest_blob_center"), QPointF(999.0, -777.0));
    ImageData output;
    QVERIFY(!plugin.execute(input, output));
    QVERIFY(!output.hasData(QStringLiteral("largest_blob_center")));
#else
    QSKIP("OpenCV not available");
#endif
}

QTEST_MAIN(TestImagePlugins)
#include "test_imageplugins.moc"
