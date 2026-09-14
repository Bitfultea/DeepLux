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
#include <cmath>

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
    // 阶段 2：消除假配置/假成功
    void testImageScriptInvalidTypeRejected();
    void testImageScriptFailureNotMarkedExecuted();
    void testImageScriptNonIntegerSetParamFailsAtRuntime();

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
    QJsonObject params{{"scriptType", 0}}; // 0 = 反转

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
    // 阶段 2 复核：用彩色图像做像素级断言——操作必须真实改变像素，
    // 仅比较 script_type 元数据无法发现"复制输入"的假成功。
    ImageScriptPlugin pGray, pInvert;
    QVERIFY(pGray.initialize());
    QVERIFY(pInvert.initialize());

    // 彩色图（BGR 30/90/210）：BGR2GRAY 灰度值 ≈ 0.114*30 + 0.587*90 + 0.299*210 ≈ 119
    cv::Mat color(60, 80, CV_8UC3, cv::Scalar(30, 90, 210));
    ImageData input;
    input.setMat(color);

    // 1 = 灰度：三通道应相等、等于灰度值，且与原彩色像素不同
    ImageData outGray;
    QVERIFY(runModule(pGray, QJsonObject{{"scriptType", 1}}, input, outGray).success);
    QCOMPARE(outGray.data("script_type").toInt(), 1);
    cv::Mat grayOut = outGray.toMat();
    QVERIFY(!grayOut.empty());
    const cv::Vec3b pg = grayOut.at<cv::Vec3b>(30, 40);
    QCOMPARE(int(pg[0]), int(pg[1]));
    QCOMPARE(int(pg[1]), int(pg[2]));
    QVERIFY2(std::abs(pg[0] - 119) <= 2, "gray value must match BGR2GRAY result");
    cv::Mat diffGray;
    cv::absdiff(grayOut, color, diffGray);
    QVERIFY2(cv::countNonZero(diffGray.reshape(1)) > 0, "grayscale must change color pixels");

    // 0 = 反转：逐通道 255-x
    ImageData outInvert;
    QVERIFY(runModule(pInvert, QJsonObject{{"scriptType", 0}}, input, outInvert).success);
    QCOMPARE(outInvert.data("script_type").toInt(), 0);
    const cv::Vec3b pi = outInvert.toMat().at<cv::Vec3b>(30, 40);
    QCOMPARE(int(pi[0]), 255 - 30);
    QCOMPARE(int(pi[1]), 255 - 90);
    QCOMPARE(int(pi[2]), 255 - 210);

    // 四通道 BGRA 输入：灰度同样必须生效，且保留 alpha 通道
    ImageScriptPlugin pGray4;
    QVERIFY(pGray4.initialize());
    cv::Mat bgra(60, 80, CV_8UC4, cv::Scalar(30, 90, 210, 128));
    ImageData input4;
    input4.setMat(bgra);
    ImageData outGray4;
    QVERIFY(runModule(pGray4, QJsonObject{{"scriptType", 1}}, input4, outGray4).success);
    cv::Mat gray4 = outGray4.toMat();
    QCOMPARE(gray4.channels(), 4);
    const cv::Vec4b p4 = gray4.at<cv::Vec4b>(30, 40);
    QCOMPARE(int(p4[0]), int(p4[1]));
    QCOMPARE(int(p4[1]), int(p4[2]));
    QVERIFY2(std::abs(p4[0] - 119) <= 2, "BGRA grayscale must change color pixels");
    QCOMPARE(int(p4[3]), 128); // alpha 保留
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testImageScriptEmptyImageFails() {
#ifdef DEEPLUX_HAS_OPENCV
    ImageScriptPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空
    QJsonObject params{{"scriptType", 0}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty image must fail");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testImageScriptInvalidTypeRejected() {
    // 阶段 2：scriptType 严格校验 0–3，越界拒绝（失败关闭）
    ImageScriptPlugin plugin;
    QString error;

    QJsonObject tooBig{{"scriptType", 4}};
    QVERIFY2(!plugin.validateParams(tooBig, error), "scriptType=4 must be rejected");

    QJsonObject negative{{"scriptType", -1}};
    QVERIFY2(!plugin.validateParams(negative, error), "scriptType=-1 must be rejected");

    QJsonObject nonInteger{{"scriptType", 1.5}};
    QVERIFY2(!plugin.validateParams(nonInteger, error), "non-integer scriptType must be rejected");

    // 错误 JSON 类型同样拒绝：toDouble() 会把字符串/布尔默转为 0，不得被当作合法反转操作
    QJsonObject stringType{{"scriptType", QStringLiteral("abc")}};
    QVERIFY2(!plugin.validateParams(stringType, error), "string scriptType must be rejected");

    QJsonObject boolType{{"scriptType", true}};
    QVERIFY2(!plugin.validateParams(boolType, error), "bool scriptType must be rejected");

    QJsonObject nullType;
    nullType.insert("scriptType", QJsonValue());
    QVERIFY2(!plugin.validateParams(nullType, error), "null scriptType must be rejected");

    // setParams 失败关闭：非法值（含错误类型）不得覆盖已设置的合法值
    plugin.setParams(QJsonObject{{"scriptType", 2}});
    plugin.setParams(QJsonObject{{"scriptType", 9}});
    plugin.setParams(QJsonObject{{"scriptType", QStringLiteral("abc")}});
    QCOMPARE(plugin.currentParams().value("scriptType").toInt(), 2);
}

void TestFinalPlugins::testImageScriptFailureNotMarkedExecuted() {
#ifdef DEEPLUX_HAS_OPENCV
    // 阶段 2：失败时不得设置 script_executed=true（消除假成功）
    ImageScriptPlugin plugin;
    QVERIFY(plugin.initialize());

    ImageData input; // 空图 → process 失败
    QJsonObject params{{"scriptType", 0}};

    ImageData output;
    const ExecutionResult result = runModule(plugin, params, input, output);
    QVERIFY2(!result.success, "empty image must fail");
    QVERIFY2(!output.data("script_executed").toBool(), "failed run must not set script_executed=true");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testImageScriptNonIntegerSetParamFailsAtRuntime() {
#ifdef DEEPLUX_HAS_OPENCV
    // 阶段 2 复核二轮：setParam 绕过 setParams 的校验，process() 必须复用完整
    // validateParams——非整数 1.5 不得被 toInt() 后静默执行类型 1 操作。
    ImageScriptPlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParam("scriptType", 1.5);

    ImageData input = makeGrayImage();
    ImageData output;
    QVERIFY2(!plugin.execute(input, output), "non-integer scriptType via setParam must fail at runtime");
    QVERIFY2(!output.data("script_executed").toBool(), "must not mark executed on invalid param");
#else
    QSKIP("OpenCV not available");
#endif
}

void TestFinalPlugins::testImageScriptCloneIndependent() {
    ImageScriptPlugin plugin;
    QVERIFY(plugin.initialize());
    QJsonObject params{{"scriptType", 1}};
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
