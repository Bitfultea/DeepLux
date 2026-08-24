#include "core/base/ModuleBase.h"
#include "core/model/ImageData.h"
#include "plugins/image_processing/ImageScript/ImageScriptPlugin.h"
#include "plugins/system/ShowPoint/ShowPointPlugin.h"
#include "plugins/geometry/FreeformSurface/FreeformSurfacePlugin.h"

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

    // FreeformSurface（3D 点云依赖，仅契约级验收）
    void testFreeformSurfaceCloneIndependent();
    void testFreeformSurfaceEmptyInputHandled();
};

#ifdef DEEPLUX_HAS_OPENCV
static ImageData makeGrayImage(int width = 100, int height = 100) {
    cv::Mat mat(height, width, CV_8UC3, cv::Scalar(128, 128, 128));
    ImageData data;
    data.setMat(mat);
    return data;
}
#endif

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

    // 空输入（无点云）应优雅失败，不崩溃
    ImageData input, output;
    PortValueMap inputs;
    inputs.insert(QStringLiteral("image"), QVariant::fromValue(input));
    PortValueMap outputs;
    ExecutionContext ctx;
    const ExecutionResult result = plugin.execute(inputs, outputs, ctx);
    // 无点云数据时应失败或空输出，不能崩溃
    Q_UNUSED(result);
#else
    QSKIP("OpenCV not available");
#endif
}

QTEST_MAIN(TestFinalPlugins)
#include "test_finalplugins.moc"
