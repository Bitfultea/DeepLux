#include "core/base/ModuleBase.h"
#include "core/display/DisplayData.h"
#include "core/geometry/MeasurementData.h"
#include "core/model/ImageData.h"
#include "plugins/geometry/FreeformSurface/FreeformSurfacePlugin.h"
#include "plugins/image_processing/ImageScript/ImageScriptPlugin.h"
#include "plugins/system/ShowPoint/ShowPointPlugin.h"

#include <QJsonObject>
#include <QVariant>
#include <QtTest/QtTest>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

// 阶段 G 行为级验收：ImageScript / ShowPoint / FreeformSurface

class TestFinalPlugins : public QObject {
    Q_OBJECT

private slots:
    // ImageScript
    void testImageScriptInvertDeterministic();
    void testImageScriptTypeParamAffectsResult();
    void testImageScriptEmptyImageFails();
    void testImageScriptCloneIndependent();

    // ShowPoint
    void testShowPointDrawsMarker();
    void testShowPointMissingPointFails();
    void testShowPointCloneIndependent();

    // FreeformSurface（3D 点云依赖）
    void testFreeformSurfaceCloneIndependent();
    void testFreeformSurfaceEmptyInputHandled();
    void testFreeformSurfaceCoplanarPointsLowRoughness();
};

#ifdef DEEPLUX_HAS_OPENCV
static ImageData makeGrayImage(int width = 100, int height = 100) {
    cv::Mat mat(height, width, CV_8UC3, cv::Scalar(128, 128, 128));
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

// ===== ImageScript =====

void TestFinalPlugins::testImageScriptInvertDeterministic() {
#ifdef DEEPLUX_HAS_OPENCV
    ImageScriptPlugin p1, p2;
    QVERIFY(p1.initialize());
    QVERIFY(p2.initialize());

    ImageData input = makeGrayImage();
    QJsonObject params{{"scriptType", 0}, {"script", ""}}; // 0 = 反转

    ImageData out1, out2;
    QVERIFY(runModule(p1, params, input, out1).success);
    QVERIFY(runModule(p2, params, input, out2).success);

    // 确定性
    cv::Mat m1 = out1.toMat();
    cv::Mat m2 = out2.toMat();
    cv::Mat diff;
    cv::absdiff(m1, m2, diff);
    QCOMPARE(cv::countNonZero(diff.reshape(1)), 0);

    // 反转后灰度 128 → 127 (255-128)
    QVERIFY(m1.at<cv::Vec3b>(50, 50)[0] != 128);
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testImageScriptTypeParamAffectsResult() {
#ifdef DEEPLUX_HAS_OPENCV
    ImageScriptPlugin pInvert, pGray;
    QVERIFY(pInvert.initialize());
    QVERIFY(pGray.initialize());

    ImageData input = makeGrayImage();
    QJsonObject invert{{"scriptType", 0}, {"script", ""}};
    QJsonObject gray{{"scriptType", 1}, {"script", ""}}; // 1 = 灰度

    ImageData outInvert, outGray;
    QVERIFY(runModule(pInvert, invert, input, outInvert).success);
    QVERIFY(runModule(pGray, gray, input, outGray).success);

    // 不同 scriptType → 不同结果
    QCOMPARE(outInvert.data("script_type").toInt(), 0);
    QCOMPARE(outGray.data("script_type").toInt(), 1);
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testImageScriptEmptyImageFails() {
#ifdef DEEPLUX_HAS_OPENCV
    ImageScriptPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空
    QJsonObject params{{"scriptType", 0}, {"script", ""}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty image must fail");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testImageScriptCloneIndependent() {
    ImageScriptPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"scriptType", 1}, {"script", "test"}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("scriptType").toInt(), 1);

    cloneBase->setParam("scriptType", 99);
    QCOMPARE(plugin.currentParams().value("scriptType").toInt(), 1);
    delete clone;
}

// ===== ShowPoint =====

void TestFinalPlugins::testShowPointDrawsMarker() {
#ifdef DEEPLUX_HAS_OPENCV
    ShowPointPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input = makeGrayImage();
    input.setData("point", QVariantList{50.0, 50.0});
    QJsonObject params{{"markerSize", 5}, {"colorR", 255}, {"colorG", 0}, {"colorB", 0}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    cv::Mat outMat = output.toMat();
    QVERIFY(!outMat.empty());

    // G2-fix2: 验证 (50,50) 处确实绘制了红色标记（原图灰色 128）
    cv::Vec3b center = outMat.at<cv::Vec3b>(50, 50);
    QVERIFY2(center[2] > 200, "marker center should be red (R channel high)"); // BGR: [2]=R
    QVERIFY2(center[0] < 100, "marker center should have low blue");           // [0]=B
    // 远离标记的点应保持原灰色
    cv::Vec3b far = outMat.at<cv::Vec3b>(5, 5);
    QCOMPARE(static_cast<int>(far[0]), 128);
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testShowPointMissingPointFails() {
#ifdef DEEPLUX_HAS_OPENCV
    ShowPointPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input = makeGrayImage(); // 无 point
    QJsonObject params{{"markerSize", 5}, {"colorR", 255}, {"colorG", 0}, {"colorB", 0}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "missing point must fail");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testShowPointCloneIndependent() {
    ShowPointPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"markerSize", 8}, {"colorR", 10}, {"colorG", 20}, {"colorB", 30}};
    plugin.setParams(params);

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    QCOMPARE(cloneBase->currentParams().value("markerSize").toInt(), 8);

    cloneBase->setParam("markerSize", 99);
    QCOMPARE(plugin.currentParams().value("markerSize").toInt(), 8);
    delete clone;
}

// ===== FreeformSurface（3D 点云，契约级验收）=====

void TestFinalPlugins::testFreeformSurfaceCloneIndependent() {
    FreeformSurfacePlugin plugin;
    QVERIFY(plugin.initialize());

    IModule* clone = plugin.clone();
    QVERIFY(clone != nullptr);
    QVERIFY(clone != &plugin);
    auto* cloneBase = qobject_cast<ModuleBase*>(clone);
    QVERIFY(cloneBase != nullptr);
    // clone 保留默认参数
    QCOMPARE(cloneBase->currentParams(), plugin.currentParams());
    delete clone;
}

void TestFinalPlugins::testFreeformSurfaceEmptyInputHandled() {
#ifdef DEEPLUX_HAS_OPENCV
    FreeformSurfacePlugin plugin;
    QVERIFY(plugin.initialize());

    // 空输入（无点云）必须明确失败，不能静默成功
    ImageData input, output;
    PortValueMap inputs;
    inputs.insert(QStringLiteral("image"), QVariant::fromValue(input));
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = plugin.execute(inputs, outputs, ctx);
    QVERIFY2(!result.success, "empty point cloud must fail, not silently succeed");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testFreeformSurfaceCoplanarPointsLowRoughness() {
#ifdef DEEPLUX_HAS_OPENCV
    // G2-fix2: 提供共面点云（PointCloudData），验证输出语义（点数/粗糙度≈0）
    FreeformSurfacePlugin plugin;
    QVERIFY(plugin.initialize());

    // 构造 z=0 平面上的网格点（完全共面 → 粗糙度应接近 0）
    PointCloudData cloud;
    for (int x = 0; x < 10; ++x) {
        for (int y = 0; y < 10; ++y) {
            cloud.points.emplace_back(static_cast<double>(x), static_cast<double>(y), 0.0);
        }
    }
    ImageData input;
    MeasurementData::setPointCloud(input, cloud);

    ImageData output;
    QJsonObject params{{"samplingInterval", 1.0}};
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(result.success, qPrintable(result.userMessage));
    QCOMPARE(output.data("point_count").toInt(), 100);
    // 共面点粗糙度应极小
    QVERIFY2(output.data("surface_roughness").toDouble() < 0.01, "coplanar points must yield near-zero roughness");
#else
    QSKIP("OpenCV not available");
#endif
}

QTEST_MAIN(TestFinalPlugins)
#include "test_finalplugins.moc"
