#include "core/deeplux/DataContract.h"
#include "core/engine/RunEngine.h"
#include "core/geometry/MeasurementData.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "plugins/geometry/FitPlane/FitPlanePlugin.h"
#include "plugins/geometry/GapMeasure3D/GapMeasure3DPlugin.h"
#include "plugins/image_processing/PreProcessing3D/PreProcessing3DPlugin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <limits>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

namespace {
// 倾斜平面高度图（CV_32F）：z = ax·x + ay·y + z0（像素坐标）
cv::Mat makeTiltedPlane(int w, int h, double ax, double ay, double z0) {
    cv::Mat m(h, w, CV_32F);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            m.at<float>(y, x) = static_cast<float>(ax * x + ay * y + z0);
        }
    }
    return m;
}

// V 槽高度图（CV_32F）：基准高度 base，行 [y0,y1] 内列 [x0,x1] 下沉 depth（1px 竖直壁）。
// 默认槽宽 40px：落沿斜率极值在 299.5，升沿在 339.5（抛物线细化后宽度恰为 40）。
cv::Mat makeGrooveHeight(int w, int h, int y0, int y1, int x0, int x1, double base, double depth) {
    cv::Mat m(h, w, CV_32F, cv::Scalar(base));
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            m.at<float>(y, x) = static_cast<float>(base - depth);
        }
    }
    return m;
}

QJsonObject preParams(bool filterOn, double hMin, double hMax, double fill, int cx, int cy, int rw, int rh) {
    return QJsonObject{{"heightFilterEnabled", filterOn},
                       {"heightFilterMin", hMin},
                       {"heightFilterMax", hMax},
                       {"fillValue", fill},
                       {"autoNoData", true},
                       {"roiCenterX", cx},
                       {"roiCenterY", cy},
                       {"roiWidth", rw},
                       {"roiHeight", rh}};
}

QJsonObject planeParams(double cx, double cy, double l1, double l2, double ang, double px, double py, double zs,
                        double invalid) {
    return QJsonObject{{"roiCenterX", cx},        {"roiCenterY", cy},  {"roiLength1", l1}, {"roiLength2", l2},
                       {"roiAngle", ang},         {"pixelSizeX", px},  {"pixelSizeY", py}, {"zScale", zs},
                       {"invalidValue", invalid}, {"autoNoData", true}};
}

QJsonObject gapParams(double cx, double cy, int len, int rows) {
    return QJsonObject{{"roiCenterX", cx},        {"roiCenterY", cy},     {"roiLength", len},
                       {"roiHeight", rows},       {"pixelSizeX", 1.0},    {"zScale", 1.0},
                       {"smoothSigma", 1.0},      {"medianSize", 0},      {"derivativeThreshold", 0.05},
                       {"edgeTrim", 2},           {"minPeakDistance", 5}, {"invalidValue", 0.0},
                       {"autoNoData", true},      {"offsetMm", 0.0},      {"specUpperLimit", 50.0},
                       {"measureFailValue", -1.0}};
}
} // namespace

class Test3DBatch3 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDir;

    // 阶7 批3：metadata 与库路径均由 CMake 注入（跨平台/多配置，沿用批2复核二轮模式）
    bool installPlugin(const QString& pluginRoot, const QString& name, const QString& metaSrc,
                       const QString& libSrc) const {
        QDir root(pluginRoot);
        if (!root.mkpath(name))
            return false;
        QDir dir(root.filePath(name));
        if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
            return false;
        const QString destLib = dir.filePath(QFileInfo(libSrc).fileName());
        QFile::remove(dir.filePath("metadata.json"));
        QFile::remove(destLib);
        return QFile::copy(metaSrc, dir.filePath("metadata.json")) && QFile::copy(libSrc, destLib);
    }

private slots:
    void initTestCase() {
        QVERIFY(m_appDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDir.path().toLocal8Bit());
        PluginManager::instance().shutdown();
        const QString pluginRoot = QDir(m_appDir.path()).filePath("plugins");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_BATCH3_META_GrabImage),
                               QStringLiteral(TEST_BATCH3_LIB_GrabImage)),
                 "install GrabImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("3DPreProcessing"),
                               QStringLiteral(TEST_BATCH3_META_PreProcessing3D),
                               QStringLiteral(TEST_BATCH3_LIB_PreProcessing3D)),
                 "install 3DPreProcessing");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("FitPlane"), QStringLiteral(TEST_BATCH3_META_FitPlane),
                               QStringLiteral(TEST_BATCH3_LIB_FitPlane)),
                 "install FitPlane");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GapMeasure3D"),
                               QStringLiteral(TEST_BATCH3_META_GapMeasure3D),
                               QStringLiteral(TEST_BATCH3_LIB_GapMeasure3D)),
                 "install GapMeasure3D");
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("3DPreProcessing")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("FitPlane")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GapMeasure3D")));
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void cleanup() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        RunEngine::instance().clearOutputs();
    }

    // ---------- 3DPreProcessing ----------

    void testPre3DHeightFilter() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(true, 10.0, 50.0, -1.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        ImageData input(makeTiltedPlane(640, 480, 0.1, 0.0, 0.0)); // z = 0.1x ∈ [0, 63.9]
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "height filter must succeed");
        QCOMPARE(out.toMat().depth(), CV_32F);
        QVERIFY(std::abs(out.toMat().at<float>(240, 300) - 30.0f) < 1e-3f); // 区间内保留
        QCOMPARE(out.toMat().at<float>(240, 50), -1.0f);                    // 低于下限 → 填充
        QCOMPARE(out.toMat().at<float>(240, 550), -1.0f);                   // 高于上限 → 填充
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 401.0 * 480.0);  // x ∈ [100,500]
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 640.0 * 480.0 - 401.0 * 480.0);
        QVERIFY(std::abs(out.data("min_height").toDouble() - 10.0) < 1e-3);
        QVERIFY(std::abs(out.data("max_height").toDouble() - 50.0) < 1e-3);
    }

    void testPre3DFilterDisabledPassthrough() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, -1.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        ImageData input(makeTiltedPlane(640, 480, 0.1, 0.0, 0.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "passthrough must succeed");
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 640.0 * 480.0);
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 0.0);
        QVERIFY(std::abs(out.toMat().at<float>(240, 300) - 30.0f) < 1e-3f);
    }

    void testPre3DTwoChannelDepthExtraction() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, -1.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        // 2 通道：ch0=强度垃圾值，ch1=深度（旧版 Decompose2 语义取第 2 通道）
        std::vector<cv::Mat> chs{cv::Mat(480, 640, CV_32F, cv::Scalar(7.0f)), makeTiltedPlane(640, 480, 0.1, 0.0, 0.0)};
        cv::Mat two;
        cv::merge(chs, two);
        ImageData input(two);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "2-channel depth extraction must succeed");
        QVERIFY(std::abs(out.toMat().at<float>(240, 300) - 30.0f) < 1e-3f);
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 640.0 * 480.0);
    }

    void testPre3DThreeChannelRejected() {
        PreProcessing3DPlugin plugin;
        QVERIFY(plugin.initialize());
        cv::Mat c3(480, 640, CV_8UC3, cv::Scalar(10, 20, 30));
        ImageData input(c3);
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "3-channel must fail");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("通道")),
                 "error must mention channels");
    }

    void testPre3DRoiFill() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, -7.0, 320, 240, 100, 100));
        QVERIFY(plugin.initialize());
        ImageData input(makeTiltedPlane(640, 480, 0.1, 0.0, 0.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "ROI must succeed");
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 100.0 * 100.0);
        QVERIFY(std::abs(out.toMat().at<float>(240, 300) - 30.0f) < 1e-3f); // ROI 内保留
        QCOMPARE(out.toMat().at<float>(240, 100), -7.0f);                   // ROI 外填充
        QCOMPARE(out.toMat().at<float>(100, 300), -7.0f);
    }

    void testPre3DNaNFilled() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, -3.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        cv::Mat m = makeTiltedPlane(640, 480, 0.1, 0.0, 0.0);
        m(cv::Rect(100, 100, 10, 10)).setTo(std::numeric_limits<float>::quiet_NaN());
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "NaN fill must succeed");
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 100.0);
        QCOMPARE(out.toMat().at<float>(105, 105), -3.0f);
    }

    void testPre3DAllFilteredFails() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(true, 100.0, 200.0, 0.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        ImageData input(makeTiltedPlane(640, 480, 0.1, 0.0, 0.0)); // 值域 [0,63.9] 全部越界
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "all-filtered must fail closed");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("无有效像素")),
                 "error must report no valid pixels");
    }

    // 阶7 批3复核（P1-1）：自动 NoData 检测（TiffLoader 重复极值判据）——
    // 69% 像素为 -21474836 哨兵的高度图，默认参数（筛选关闭）也必须剔除并填充
    void testPre3DNoDataAutoDetect() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        cv::Mat m(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                m.at<float>(y, x) = (x >= 62) ? -21474836.0f : static_cast<float>(50.0 + 0.1 * x);
            }
        }
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "auto NoData detect must succeed");
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 138.0 * 100.0); // 69% NoData
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 62.0 * 100.0);
        QCOMPARE(out.toMat().at<float>(50, 100), 0.0f);                   // NoData → fillValue
        QVERIFY(std::abs(out.toMat().at<float>(50, 30) - 53.0f) < 1e-3f); // 真实高度保留
        QCOMPARE(out.data("height_invalid_value").toDouble(), 0.0);       // 契约键写出
    }

    // 阶7 批3复核三轮（P1-1）：合法大面积平台不得误删——69% 像素为合法恒定
    // 平台 -1000 + 31% 细节 0..0.62，旧判据（无量级门禁）会把平台判为 NoData
    void testPre3DFlatValidPlateauNotRemoved() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        cv::Mat m(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                m.at<float>(y, x) = (x < 138) ? -1000.0f : static_cast<float>(0.01 * (x - 138) + 0.005 * (y % 3));
            }
        }
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "legit plateau must survive");
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 0.0);
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 200.0 * 100.0);
        QVERIFY2(!out.hasData("height_invalid_value"), "no fill must not write contract key");
        QCOMPARE(out.toMat().at<float>(50, 50), -1000.0f);
    }

    // 阶7 批3复核五轮（P1-1）：两值图（哨兵 + 恒定有效值，双侧极值同时满足
    // 重复率+间隙判据）——哨兵语义无法从像素分布推导（{0=NoData, 2e6=有效} 与
    // {0=有效, 2e6=NoData} 直方图完全相同），按绝对值/占比猜测可能反向删除全部
    // 合法数据，必须上报歧义失败关闭（取代三轮的"两值图检出"口径）
    void testPre3DTwoValueAmbiguityFailsClosed() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        cv::Mat m(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                m.at<float>(y, x) = (x >= 62) ? -21474836.0f : 42.0f;
            }
        }
        ImageData input(m);
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "two-value image must be ambiguous, fail closed");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("歧义")),
                 qPrintable(QString("error must report ambiguity, got: %1")
                                .arg(errSpy.count() ? errSpy.first().at(0).toString() : QString())));
    }

    // 阶7 批3复核五轮（P1-1）：审核指定三构型——双侧同满足判据时全部歧义失败
    // 关闭，旧绝对值决胜会反向删除合法平台
    void testNoDataAmbiguityReviewCases() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        const auto runExpectAmbiguous = [&plugin](const cv::Mat& img, const char* ctx) {
            ImageData input(img);
            QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
            ImageData out;
            QVERIFY2(!plugin.execute(input, out), ctx);
            QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("歧义")), ctx);
        };
        // (a) 0=NoData 编码 + 大正值合法定深度平台：旧决胜按绝对值选 2e6 → 反向删除
        cv::Mat a(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                a.at<float>(y, x) = (x < 138) ? 0.0f : 2000000.0f;
            }
        }
        runExpectAmbiguous(a, "0=NoData + 2e6 legit plateau must be ambiguous");
        // (b) 较小负值=合法平台 + 较大正值=NoData
        cv::Mat b(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                b.at<float>(y, x) = (x < 62) ? -1000.0f : 21474836.0f;
            }
        }
        runExpectAmbiguous(b, "-1000 plateau + 21M NoData must be ambiguous");
        // (c) 两个合法平台且间距超过门限（图中根本没有哨兵）
        cv::Mat c(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                c.at<float>(y, x) = (x < 100) ? 0.0f : 3000000.0f;
            }
        }
        runExpectAmbiguous(c, "two legit plateaus must be ambiguous");
    }

    // 阶7 批3复核五轮（P1-1）：哨兵 + 恒定有效值但有效侧占比低于重复阈值
    // （4% < 5%）——单侧满足判据，仍可检出（三轮"平坦有效面加 NoData"意图
    // 在无歧义子空间的保留）
    void testPre3DConstantValidSmallShareDetected() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        cv::Mat m(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                m.at<float>(y, x) = (x >= 8) ? -21474836.0f : 42.0f; // 4% 恒定有效值
            }
        }
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "single-side sentinel must be detected");
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 192.0 * 100.0);
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 8.0 * 100.0);
        QCOMPARE(out.data("min_height").toDouble(), 42.0);
        QCOMPARE(out.data("max_height").toDouble(), 42.0);
        QCOMPARE(out.data("height_invalid_value").toDouble(), 0.0);
    }

    // 阶7 批3复核四轮（P1-1）：真实图有效高度为大负值（-4455.5..-2740，
    // NoData=-21474836 占 69%）——三轮量级门禁用绝对高度作数据尺度
    // （1e4×4455.5=44.6M > 21.5M）漏检，哨兵全部进入计算；绝对间隙下限
    // （>1e6）与高度偏置无关，必须检出
    void testPre3DLargeNegativeValidRangeNoData() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0));
        QVERIFY(plugin.initialize());
        cv::Mat m(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                if (x >= 62) {
                    m.at<float>(y, x) = -21474836.0f; // 69% NoData
                } else {
                    const double t = ((x % 62) + y) / 160.0; // 0..1
                    m.at<float>(y, x) = static_cast<float>(-4455.5 + 1715.5 * t);
                }
            }
        }
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "large-negative valid range NoData must be detected");
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 138.0 * 100.0);
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 62.0 * 100.0);
        QVERIFY(std::abs(out.data("min_height").toDouble() - (-4455.5)) < 0.5);
        QVERIFY(std::abs(out.data("max_height").toDouble() - (-2740.0)) < 0.5);
        QCOMPARE(out.toMat().at<float>(50, 100), 0.0f); // NoData → fillValue
        QCOMPARE(out.data("height_invalid_value").toDouble(), 0.0);
    }

    // 阶7 批3复核四轮（P1-2）：非有限携带契约值（NaN/±Inf/1e300 经 float
    // 量化溢出为 Inf）三插件都必须失败关闭——DataType::Number 只验证 QVariant
    // 类型不验证有限性，无效契约会关闭自动检测且 v==NaN 恒假，令有限哨兵全部放行
    void testCarriedContractNonFiniteRejected() {
        cv::Mat m = makeTiltedPlane(200, 100, 0.1, -0.2, 50.0); // CV_32F
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double inf = std::numeric_limits<double>::infinity();
        const QVector<double> badValues{nan, inf, -inf, 1e300};

        for (const double bad : badValues) {
            PreProcessing3DPlugin pre;
            pre.setParams(preParams(false, 0.0, 65535.0, -7.0, 0, 0, 0, 0));
            QVERIFY(pre.initialize());
            ImageData inPre(m);
            inPre.setData("height_invalid_value", bad);
            QSignalSpy spyPre(&pre, &DeepLux::IModule::errorOccurred);
            ImageData outPre;
            QVERIFY2(!pre.execute(inPre, outPre), qPrintable(QString("3DPre must reject carried %1").arg(bad)));
            QVERIFY2(spyPre.count() >= 1 && spyPre.first().at(0).toString().contains(QStringLiteral("契约值非法")),
                     "error must report illegal contract value");

            FitPlanePlugin fp;
            fp.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 0.0));
            QVERIFY(fp.initialize());
            ImageData inFp(m);
            inFp.setData("height_invalid_value", bad);
            QSignalSpy spyFp(&fp, &DeepLux::IModule::errorOccurred);
            ImageData outFp;
            QVERIFY2(!fp.execute(inFp, outFp), qPrintable(QString("FitPlane must reject carried %1").arg(bad)));
            QVERIFY2(spyFp.count() >= 1 && spyFp.first().at(0).toString().contains(QStringLiteral("契约值非法")),
                     "error must report illegal contract value");

            GapMeasure3DPlugin gap;
            gap.setParams(gapParams(100, 50, 100, 5));
            QVERIFY(gap.initialize());
            ImageData inGap(m);
            inGap.setData("height_invalid_value", bad);
            QSignalSpy spyGap(&gap, &DeepLux::IModule::errorOccurred);
            ImageData outGap;
            QVERIFY2(!gap.execute(inGap, outGap), qPrintable(QString("GapMeasure3D must reject carried %1").arg(bad)));
            QVERIFY2(spyGap.count() >= 1 && spyGap.first().at(0).toString().contains(QStringLiteral("契约值非法")),
                     "error must report illegal contract value");
        }
    }

    // 阶7 批3复核五轮（P2-3）：携带契约键存在但类型错误（字符串/布尔/列表）——
    // 三插件必须失败关闭，不得静默当作"无契约"并让错误值随 output=input 继续传播
    void testCarriedContractWrongTypeRejected() {
        cv::Mat m = makeTiltedPlane(200, 100, 0.1, -0.2, 50.0);
        const QVariantList badValues{QVariant(QStringLiteral("-7")), QVariant(true), QVariant(QVariantList{1, 2})};

        for (const QVariant& bad : badValues) {
            PreProcessing3DPlugin pre;
            pre.setParams(preParams(false, 0.0, 65535.0, -7.0, 0, 0, 0, 0));
            QVERIFY(pre.initialize());
            ImageData inPre(m);
            inPre.setData("height_invalid_value", bad);
            QSignalSpy spyPre(&pre, &DeepLux::IModule::errorOccurred);
            ImageData outPre;
            QVERIFY2(!pre.execute(inPre, outPre), "3DPre must reject wrong-typed contract");
            QVERIFY2(spyPre.count() >= 1 && spyPre.first().at(0).toString().contains(QStringLiteral("契约类型非法")),
                     "error must report contract type violation");

            FitPlanePlugin fp;
            fp.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 0.0));
            QVERIFY(fp.initialize());
            ImageData inFp(m);
            inFp.setData("height_invalid_value", bad);
            QSignalSpy spyFp(&fp, &DeepLux::IModule::errorOccurred);
            ImageData outFp;
            QVERIFY2(!fp.execute(inFp, outFp), "FitPlane must reject wrong-typed contract");
            QVERIFY2(spyFp.count() >= 1 && spyFp.first().at(0).toString().contains(QStringLiteral("契约类型非法")),
                     "error must report contract type violation");

            GapMeasure3DPlugin gap;
            gap.setParams(gapParams(100, 50, 100, 5));
            QVERIFY(gap.initialize());
            ImageData inGap(m);
            inGap.setData("height_invalid_value", bad);
            QSignalSpy spyGap(&gap, &DeepLux::IModule::errorOccurred);
            ImageData outGap;
            QVERIFY2(!gap.execute(inGap, outGap), "GapMeasure3D must reject wrong-typed contract");
            QVERIFY2(spyGap.count() >= 1 && spyGap.first().at(0).toString().contains(QStringLiteral("契约类型非法")),
                     "error must report contract type violation");
        }
    }

    // 阶7 批3复核三轮（P1-1）：autoNoData=false 必须完全关闭检测——哨兵按用户
    // 语义保留为有效高度，不填充、不写契约键
    void testPre3DAutoNoDataDisabled() {
        QJsonObject p = preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0);
        p["autoNoData"] = false;
        PreProcessing3DPlugin plugin;
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        cv::Mat m(100, 200, CV_32F);
        for (int y = 0; y < 100; ++y) {
            for (int x = 0; x < 200; ++x) {
                m.at<float>(y, x) = (x >= 62) ? -21474836.0f : 42.0f;
            }
        }
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "disabled detection must keep all finite pixels");
        QCOMPARE(out.data("filtered_pixel_count").toDouble(), 0.0);
        QCOMPARE(out.toMat().at<float>(50, 100), -21474836.0f);
        QVERIFY2(!out.hasData("height_invalid_value"), "no fill must not write contract key");
    }

    // 阶7 批3复核三轮（P1-2）：有限填充值与存活合法高度相同（合法零平面 +
    // 1 个 NaN，fillValue=0）时契约无法区分填充与合法像素——必须失败关闭，
    // 不得写出歧义键让下游删除全部合法零高度
    void testPre3DFillCollisionFails() {
        cv::Mat m(480, 640, CV_32F, cv::Scalar(0.0f)); // 合法零平面
        m.at<float>(10, 10) = std::numeric_limits<float>::quiet_NaN();
        ImageData input(m);

        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0)); // fill=0 与合法高度碰撞
        QVERIFY(plugin.initialize());
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "fill/valid collision must fail closed");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("碰撞")),
                 qPrintable(QString("error must report collision, got: %1")
                                .arg(errSpy.count() ? errSpy.first().at(0).toString() : QString())));

        // 不碰撞的填充值：成功且契约键/填充像素正确
        PreProcessing3DPlugin ok;
        ok.setParams(preParams(false, 0.0, 65535.0, -7.0, 0, 0, 0, 0));
        QVERIFY(ok.initialize());
        ImageData out2;
        QVERIFY2(ok.execute(input, out2), "non-colliding fill must succeed");
        QCOMPARE(out2.data("height_invalid_value").toDouble(), -7.0);
        QCOMPARE(out2.toMat().at<float>(10, 10), -7.0f);
        QCOMPARE(out2.data("filtered_pixel_count").toDouble(), 1.0);
    }

    // 阶7 批3复核三轮（P1-3）：两级预处理串联——第二级必须消费第一级契约并把
    // 旧填充值统一转为本次 fillValue；否则第二级写出的新契约键掩盖旧填充值，
    // -7 像素泄漏进 FitPlane 摧毁平面度
    void testPre3DConsumesUpstreamContract() {
        // 平面值域 [-45.8,113.9]，填充值取 -100/-200（值域外，规避碰撞门禁——
        // 门禁本身由 testPre3DFillCollisionFails 覆盖）
        PreProcessing3DPlugin pre1;
        pre1.setParams(preParams(false, 0.0, 65535.0, -100.0, 0, 0, 0, 0));
        QVERIFY(pre1.initialize());
        cv::Mat m = makeTiltedPlane(640, 480, 0.1, -0.2, 50.0);
        m(cv::Rect(200, 200, 10, 10)).setTo(std::numeric_limits<float>::quiet_NaN());
        ImageData in1(m);
        ImageData mid;
        QVERIFY2(pre1.execute(in1, mid), "pre1 must succeed");
        QCOMPARE(mid.data("height_invalid_value").toDouble(), -100.0);

        // pre2：筛选关闭但 ROI 裁边 → filtered>0 → 契约键将被覆盖为 -200
        PreProcessing3DPlugin pre2;
        pre2.setParams(preParams(false, 0.0, 65535.0, -200.0, 320, 240, 400, 300));
        QVERIFY(pre2.initialize());
        ImageData mid2;
        QVERIFY2(pre2.execute(mid, mid2), "pre2 must succeed");
        QCOMPARE(mid2.data("height_invalid_value").toDouble(), -200.0);
        QCOMPARE(mid2.toMat().at<float>(205, 205), -200.0f); // 上游 -100 已转为 -200

        FitPlanePlugin fp;
        fp.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 0.0)); // 仅凭携带契约
        QVERIFY(fp.initialize());
        ImageData fpOut;
        QVERIFY2(fp.execute(mid2, fpOut), "fit must succeed");
        QVERIFY2(fpOut.data("flatness").toDouble() < 0.01, "leaked -100 pixels would destroy flatness");
        QCOMPARE(fpOut.data("valid_pixel_count").toDouble(), mid2.data("valid_pixel_count").toDouble());
    }

    // 阶7 批3复核（P1-1）：非零 fillValue 串联——FitPlane 自身 invalidValue 参数
    // 故意设为不匹配哨兵，必须凭输入携带的 height_invalid_value 排除填充像素
    // （契约不贯通时 -7 平面会彻底摧毁拟合 → 测试变红）
    void testPre3DNonZeroFillCarriedToDownstream() {
        PreProcessing3DPlugin pre;
        pre.setParams(preParams(true, 10.0, 1000.0, -7.0, 0, 0, 0, 0));
        QVERIFY(pre.initialize());
        ImageData planeIn(makeTiltedPlane(640, 480, 0.1, -0.2, 50.0));
        ImageData preOut;
        QVERIFY2(pre.execute(planeIn, preOut), "preprocess must succeed");
        QCOMPARE(preOut.data("height_invalid_value").toDouble(), -7.0);
        QVERIFY2(preOut.data("filtered_pixel_count").toDouble() > 0.0, "some pixels must be filtered");

        FitPlanePlugin fp;
        fp.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 12345.0)); // 故意不匹配的参数
        QVERIFY(fp.initialize());
        ImageData fpOut;
        QVERIFY2(fp.execute(preOut, fpOut), "fit on preprocessed output must succeed");
        const double norm = std::sqrt(1.05);
        QVERIFY2(std::abs(fpOut.data("plane_nx").toDouble() - (-0.1 / norm)) < 1e-3,
                 "carried invalid value must exclude filled pixels");
        QVERIFY2(fpOut.data("flatness").toDouble() < 0.01, "flatness must ignore filled pixels");
    }

    void testPre3DValidation() {
        PreProcessing3DPlugin plugin;
        QString error;
        // 阶7 批3复核（P2-5）：ROI 单维度配置必须拒绝（不得静默退回全图）
        QVERIFY(!plugin.validateParams(preParams(false, 0.0, 100.0, 0.0, 10, 10, 100, 0), error));
        QVERIFY(!plugin.validateParams(preParams(false, 0.0, 100.0, 0.0, 10, 10, 0, 100), error));
        QVERIFY(plugin.validateParams(preParams(false, 0.0, 100.0, 0.0, 10, 10, 100, 100), error));
        QVERIFY(!plugin.validateParams(preParams(true, 50.0, 10.0, 0.0, 0, 0, 0, 0), error)); // min>max
        QVERIFY(!plugin.validateParams(QJsonObject{{"heightFilterEnabled", QStringLiteral("yes")},
                                                   {"heightFilterMin", 0.0},
                                                   {"heightFilterMax", 100.0},
                                                   {"fillValue", 0.0},
                                                   {"roiCenterX", 0},
                                                   {"roiCenterY", 0},
                                                   {"roiWidth", 0},
                                                   {"roiHeight", 0}},
                                       error)); // 布尔类型严格
        QVERIFY(!plugin.validateParams(QJsonObject{{"heightFilterEnabled", false},
                                                   {"heightFilterMin", 0.0},
                                                   {"heightFilterMax", 100.0},
                                                   {"fillValue", 0.0},
                                                   {"roiCenterX", 0},
                                                   {"roiCenterY", 0},
                                                   {"roiWidth", 10.5},
                                                   {"roiHeight", 0}},
                                       error)); // ROI 宽度必须整数
        QVERIFY(plugin.validateParams(preParams(true, -5.0, 100.0, 0.0, 10, 10, 20, 20), error));
        // 阶7 批3复核三轮（P1-1）：autoNoData 布尔严格
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = preParams(false, 0.0, 100.0, 0.0, 0, 0, 0, 0);
                p["autoNoData"] = QStringLiteral("yes");
                return p;
            }(),
            error));
    }

    void testPre3DClone() {
        PreProcessing3DPlugin plugin;
        plugin.setParams(preParams(true, 5.0, 40.0, -2.0, 100, 100, 200, 200));
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* clonePre = qobject_cast<PreProcessing3DPlugin*>(clone);
        QVERIFY(clonePre != nullptr);
        QCOMPARE(clonePre->currentParams()["heightFilterMin"].toDouble(), 5.0);
        clonePre->setParams(preParams(false, 0.0, 65535.0, 0.0, 0, 0, 0, 0));
        QCOMPARE(plugin.currentParams()["heightFilterMin"].toDouble(), 5.0); // 原体不受影响
        delete clone;
    }

    // ---------- FitPlane ----------

    void testFitPlaneRecoversTilt() {
        FitPlanePlugin plugin;
        plugin.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, -99999.0));
        QVERIFY(plugin.initialize());
        // z = 0.1x - 0.2y + 50 → n = (-0.1, 0.2, 1)/√1.05, D = -50/√1.05
        ImageData input(makeTiltedPlane(640, 480, 0.1, -0.2, 50.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "tilted plane fit must succeed");
        const double norm = std::sqrt(1.05);
        QVERIFY2(std::abs(out.data("plane_nx").toDouble() - (-0.1 / norm)) < 1e-3, "nx");
        QVERIFY2(std::abs(out.data("plane_ny").toDouble() - (0.2 / norm)) < 1e-3, "ny");
        QVERIFY2(std::abs(out.data("plane_nz").toDouble() - (1.0 / norm)) < 1e-3, "nz");
        QVERIFY2(std::abs(out.data("plane_d").toDouble() - (-50.0 / norm)) < 1e-2, "D");
        QVERIFY2(out.data("flatness").toDouble() < 0.01, "exact plane flatness ~0");
        QVERIFY2(out.data("rms").toDouble() < 0.01, "exact plane rms ~0");
        // 平面度恒等于偏差极值差（口径一致性）
        QVERIFY2(std::abs(out.data("flatness").toDouble() -
                          (out.data("max_deviation").toDouble() - out.data("min_deviation").toDouble())) < 1e-12,
                 "flatness must equal max-min deviation");
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 640.0 * 480.0);
    }

    void testFitPlaneInvalidExclusion() {
        FitPlanePlugin plugin;
        plugin.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, -99999.0));
        QVERIFY(plugin.initialize());
        cv::Mat m = makeTiltedPlane(640, 480, 0.1, -0.2, 50.0);
        m(cv::Rect(0, 0, 100, 480)).setTo(-99999.0f); // 左 100 列标记无效
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "fit with invalid block must succeed");
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 540.0 * 480.0);
        const double norm = std::sqrt(1.05);
        QVERIFY2(std::abs(out.data("plane_nx").toDouble() - (-0.1 / norm)) < 1e-3, "nx unaffected by invalid block");
        QVERIFY2(out.data("flatness").toDouble() < 0.01, "flatness on valid pixels only");
    }

    // 阶7 批3复核（P1-2）：CV_32F 非整数哨兵——-21474.8359 存储后实为
    // -21474.8359375，double 参数精确比较必须按源精度量化后命中；哨兵块取
    // 40×40（<5% 有限像素）确保不触发 NoData 自动检测，隔离量化路径
    void testFitPlaneFloat32Sentinel() {
        FitPlanePlugin plugin;
        plugin.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, -21474.8359));
        QVERIFY(plugin.initialize());
        cv::Mat m = makeTiltedPlane(640, 480, 0.1, -0.2, 50.0);
        m(cv::Rect(0, 0, 40, 40)).setTo(static_cast<float>(-21474.8359));
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "sentinel block must be excluded");
        QCOMPARE(out.data("valid_pixel_count").toDouble(), 640.0 * 480.0 - 40.0 * 40.0);
        QVERIFY2(out.data("flatness").toDouble() < 0.01, "sentinel pixels must not enter the fit");
        const double norm = std::sqrt(1.05);
        QVERIFY2(std::abs(out.data("plane_nx").toDouble() - (-0.1 / norm)) < 1e-3, "nx unaffected by sentinel");
    }

    // 阶7 批3复核三轮（P1-1/P2-6）：自动检测的让位与开关——哨兵块 31% 像素：
    // (a) 输入携带契约键 → 检测让位，哨兵不被剔除（用户显式接管语义）；
    // (b) autoNoData=false → 同样不检测；(c) 无携带且开启 → 哨兵被检出剔除
    void testFitPlaneCarriedSkipsAutoDetect() {
        cv::Mat m = makeTiltedPlane(640, 480, 0.1, -0.2, 50.0);
        m(cv::Rect(0, 0, 200, 480)).setTo(-21474836.0f); // 96000 像素 = 31% >= 5%

        FitPlanePlugin carriedFp;
        carriedFp.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, -99999.0));
        QVERIFY(carriedFp.initialize());
        ImageData in1(m);
        in1.setData("height_invalid_value", -12345.0); // 携带契约（值不在图中）
        ImageData out1;
        QVERIFY2(carriedFp.execute(in1, out1), "carried input must succeed");
        QCOMPARE(out1.data("valid_pixel_count").toDouble(), 640.0 * 480.0); // 检测让位

        QJsonObject off = planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, -99999.0);
        off["autoNoData"] = false;
        FitPlanePlugin offFp;
        offFp.setParams(off);
        QVERIFY(offFp.initialize());
        ImageData in2(m);
        ImageData out2;
        QVERIFY2(offFp.execute(in2, out2), "autoNoData=false must succeed");
        QCOMPARE(out2.data("valid_pixel_count").toDouble(), 640.0 * 480.0);

        FitPlanePlugin autoFp;
        autoFp.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, -99999.0));
        QVERIFY(autoFp.initialize());
        ImageData in3(m);
        ImageData out3;
        QVERIFY2(autoFp.execute(in3, out3), "auto detection must succeed");
        QCOMPARE(out3.data("valid_pixel_count").toDouble(), 440.0 * 480.0); // 哨兵被检出剔除
    }

    void testFitPlaneRotatedRoiIgnoresOutside() {
        FitPlanePlugin plugin;
        plugin.setParams(planeParams(320, 240, 200, 100, 30.0, 1.0, 1.0, 1.0, -99999.0));
        QVERIFY(plugin.initialize());
        cv::Mat m = makeTiltedPlane(640, 480, 0.1, -0.2, 50.0);
        // 破坏 ROI 之外的区域（旋转 ROI 的包围盒 ⊂ [208,432]×[146,334]）
        m(cv::Rect(0, 0, 100, 480)).setTo(1000.0f);
        m(cv::Rect(541, 0, 99, 480)).setTo(1000.0f);
        m(cv::Rect(0, 0, 640, 80)).setTo(1000.0f);
        m(cv::Rect(0, 401, 640, 79)).setTo(1000.0f);
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "rotated ROI fit must succeed");
        const double count = out.data("valid_pixel_count").toDouble();
        QVERIFY2(count > 15000 && count < 25000, qPrintable(QString("ROI pixel count ~20000, got %1").arg(count)));
        QVERIFY2(out.data("flatness").toDouble() < 0.01, "garbage outside ROI must not affect fit");
        const double norm = std::sqrt(1.05);
        QVERIFY2(std::abs(out.data("plane_nx").toDouble() - (-0.1 / norm)) < 1e-3, "nx");
    }

    void testFitPlaneCollinearFails() {
        FitPlanePlugin plugin;
        // 短边=1 → 单行像素 → 设计矩阵秩亏（共线），必须失败关闭
        plugin.setParams(planeParams(320, 240, 200, 1, 0.0, 1.0, 1.0, 1.0, -99999.0));
        QVERIFY(plugin.initialize());
        ImageData input(makeTiltedPlane(640, 480, 0.1, -0.2, 50.0));
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "single-row ROI must fail closed");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("共线")),
                 qPrintable(QString("error must report collinear, got: %1")
                                .arg(errSpy.count() ? errSpy.first().at(0).toString() : QString())));
    }

    void testFitPlaneTooFewValidFails() {
        FitPlanePlugin plugin;
        plugin.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 5.0));
        QVERIFY(plugin.initialize());
        cv::Mat m(480, 640, CV_32F, cv::Scalar(5.0f)); // 全部等于 invalidValue
        ImageData input(m);
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "all-invalid must fail closed");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("有效像素不足")),
                 "error must report too few valid pixels");
    }

    void testFitPlaneScaling() {
        FitPlanePlugin plugin;
        // z = 0.1x-0.2y+50，pixelSize=2、zScale=3 → 物理平面 Z = 0.15X - 0.3Y + 150
        plugin.setParams(planeParams(0, 0, 0, 0, 0.0, 2.0, 2.0, 3.0, -99999.0));
        QVERIFY(plugin.initialize());
        ImageData input(makeTiltedPlane(640, 480, 0.1, -0.2, 50.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "scaled fit must succeed");
        const double norm = std::sqrt(0.15 * 0.15 + 0.3 * 0.3 + 1.0);
        QVERIFY2(std::abs(out.data("plane_nx").toDouble() - (-0.15 / norm)) < 1e-3, "scaled nx");
        QVERIFY2(std::abs(out.data("plane_ny").toDouble() - (0.3 / norm)) < 1e-3, "scaled ny");
        QVERIFY2(std::abs(out.data("plane_d").toDouble() - (-150.0 / norm)) < 1e-2, "scaled D");
        QVERIFY2(out.data("flatness").toDouble() < 0.01, "consistent scaling keeps flatness ~0");
    }

    void testFitPlaneOutputPlane3DContract() {
        FitPlanePlugin plugin;
        plugin.setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, -99999.0));
        QVERIFY(plugin.initialize());
        ImageData input(makeTiltedPlane(640, 480, 0.1, -0.2, 50.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "fit must succeed");
        const QVariant planeVar = out.data("plane");
        QVERIFY2(portValueMatchesType(planeVar, DataType::Plane3D), "plane output must satisfy Plane3D contract");
        QString parseError;
        const auto parsed = MeasurementData::parsePlane3D(planeVar, &parseError);
        QVERIFY2(parsed.has_value(), qPrintable("core parsePlane3D must accept plane output: " + parseError));
        // 解析出的 3 点法向与输出法向平行（|dot| ≈ 1）
        const MeasurementPoint3D& p1 = parsed->p1;
        const MeasurementPoint3D& p2 = parsed->p2;
        const MeasurementPoint3D& p3 = parsed->p3;
        const double v1x = p2.x - p1.x, v1y = p2.y - p1.y, v1z = p2.z - p1.z;
        const double v2x = p3.x - p1.x, v2y = p3.y - p1.y, v2z = p3.z - p1.z;
        const double crx = v1y * v2z - v1z * v2y;
        const double cry = v1z * v2x - v1x * v2z;
        const double crz = v1x * v2y - v1y * v2x;
        const double crLen = std::sqrt(crx * crx + cry * cry + crz * crz);
        QVERIFY2(crLen > 1e-9, "parsed points must be non-collinear");
        const double dot = (crx * out.data("plane_nx").toDouble() + cry * out.data("plane_ny").toDouble() +
                            crz * out.data("plane_nz").toDouble()) /
                           crLen;
        QVERIFY2(std::abs(std::abs(dot) - 1.0) < 1e-3,
                 qPrintable(QString("normals must be parallel, dot=%1").arg(dot)));
    }

    void testFitPlaneValidation() {
        FitPlanePlugin plugin;
        QString error;
        QVERIFY(!plugin.validateParams(planeParams(0, 0, 0, 0, 0.0, 0.0, 1.0, 1.0, 0.0), error));    // pixelSizeX=0
        QVERIFY(!plugin.validateParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, -1.0, 0.0), error));   // zScale<0
        QVERIFY(!plugin.validateParams(planeParams(0, 0, 0, 0, 400.0, 1.0, 1.0, 1.0, 0.0), error));  // roiAngle>360
        QVERIFY(!plugin.validateParams(planeParams(0, 0, 10.5, 0, 0.0, 1.0, 1.0, 1.0, 0.0), error)); // 非整数长度
        QVERIFY(!plugin.validateParams(planeParams(-1, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 0.0), error));   // 中心X<0
        // 阶7 批3复核（P2-5）：ROI 单维度配置必须拒绝
        QVERIFY(!plugin.validateParams(planeParams(10, 10, 100, 0, 0.0, 1.0, 1.0, 1.0, 0.0), error));
        QVERIFY(!plugin.validateParams(planeParams(10, 10, 0, 100, 0.0, 1.0, 1.0, 1.0, 0.0), error));
        QVERIFY(plugin.validateParams(planeParams(10, 10, 100, 50, 0.0, 1.0, 1.0, 1.0, 0.0), error));
        QVERIFY(plugin.validateParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 0.0), error));
        // 阶7 批3复核三轮（P1-1）：autoNoData 布尔严格
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 0.0);
                p["autoNoData"] = QStringLiteral("yes");
                return p;
            }(),
            error));
    }

    void testFitPlaneClone() {
        FitPlanePlugin plugin;
        plugin.setParams(planeParams(100, 100, 50, 40, 10.0, 2.0, 2.0, 3.0, -1.0));
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* clonePlane = qobject_cast<FitPlanePlugin*>(clone);
        QVERIFY(clonePlane != nullptr);
        QCOMPARE(clonePlane->currentParams()["pixelSizeX"].toDouble(), 2.0);
        clonePlane->setParams(planeParams(0, 0, 0, 0, 0.0, 1.0, 1.0, 1.0, 0.0));
        QCOMPARE(plugin.currentParams()["pixelSizeX"].toDouble(), 2.0);
        delete clone;
    }

    // ---------- GapMeasure3D ----------

    void testGapMeasuresGrooveWidth() {
        GapMeasure3DPlugin plugin;
        plugin.setParams(gapParams(320, 240, 200, 5));
        QVERIFY(plugin.initialize());
        // V 槽 x∈[300,339]（宽 40px）、深 10、行 200..280
        ImageData input(makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "groove measurement must succeed");
        QVERIFY2(out.data("gap_found").toBool(), "groove must be found");
        const double width = out.data("gap_width").toDouble();
        QVERIFY2(std::abs(width - 40.0) < 1.5, qPrintable(QString("width ~40, got %1").arg(width)));
        QVERIFY2(out.data("is_pass").toBool(), "width 40 <= spec 50 must pass");
        QVERIFY2(out.data("corner_dz_mm").toDouble() < 2.0, "symmetric groove corners at same height");
        const double dx = out.data("corner_dx_mm").toDouble();
        const double dz = out.data("corner_dz_mm").toDouble();
        QVERIFY2(std::abs(out.data("corner_dist_mm").toDouble() - std::hypot(dx, dz)) < 1e-9, "euclid consistency");
        QCOMPARE(out.data("gap_offset_width").toDouble(), width); // offsetMm=0
        QCOMPARE(out.data("gap_algorithm").toString(), QStringLiteral("derivative-peak"));
    }

    void testGapSpecFail() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(320, 240, 200, 5);
        p["specUpperLimit"] = 30.0;
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        ImageData input(makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "measurement must succeed");
        QVERIFY2(out.data("gap_found").toBool(), "groove must be found");
        QVERIFY2(!out.data("is_pass").toBool(), "width 40 > spec 30 must fail spec");
    }

    void testGapFlatSurfaceNotFound() {
        GapMeasure3DPlugin plugin;
        plugin.setParams(gapParams(320, 240, 200, 5));
        QVERIFY(plugin.initialize());
        cv::Mat flat(480, 640, CV_32F, cv::Scalar(100.0f));
        ImageData input(flat);
        ImageData out;
        // 未检出不是插件失败：按契约输出失败值 + gap_found=false（非伪成功）
        QVERIFY2(plugin.execute(input, out), "flat surface must succeed with not-found contract");
        QVERIFY2(!out.data("gap_found").toBool(), "flat surface has no gap");
        QCOMPARE(out.data("gap_width").toDouble(), -1.0); // measureFailValue
        QCOMPARE(out.data("gap_offset_width").toDouble(), -1.0);
        QVERIFY2(!out.data("is_pass").toBool(), "not-found must not pass");
    }

    void testGapOffsetWidth() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(320, 240, 200, 5);
        p["offsetMm"] = 2.5;
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        ImageData input(makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "offset measurement must succeed");
        QVERIFY(out.data("gap_found").toBool());
        const double width = out.data("gap_width").toDouble();
        QVERIFY2(std::abs(out.data("gap_offset_width").toDouble() - (width + 5.0)) < 1e-9,
                 "offset width = width + 2·offsetMm");
    }

    void testGapMinPeakDistanceTooLarge() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(320, 240, 200, 5);
        p["minPeakDistance"] = 50; // 槽宽仅 40px → 升降沿距离不足
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        ImageData input(makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "must succeed with not-found contract");
        QVERIFY2(!out.data("gap_found").toBool(), "corners closer than minPeakDistance must not pair");
        QCOMPARE(out.data("gap_width").toDouble(), -1.0);
    }

    void testGapMedianRemovesSpikes() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(320, 240, 200, 5);
        p["medianSize"] = 3;
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        cv::Mat m = makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0);
        // 中心行上的孤立尖刺（行均值后成为截面伪峰，中值窗口必须滤除）
        for (const int sx : {250, 270, 290, 320, 350, 370}) {
            m.at<float>(240, sx) = 500.0f;
        }
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "spiked groove must succeed");
        QVERIFY2(out.data("gap_found").toBool(), "groove must survive spikes");
        const double width = out.data("gap_width").toDouble();
        QVERIFY2(std::abs(width - 40.0) < 2.0,
                 qPrintable(QString("median must remove spikes, width ~40, got %1").arg(width)));
    }

    // 阶7 批3复核（P1-2）：CV_32F 非整数哨兵条带必须按源精度量化后排除——
    // 否则条带边界会伪造 ±10787 的斜率极值，宽度测成条带而非真实槽（19≠40）
    void testGapFloat32Sentinel() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(320, 240, 200, 5);
        p["invalidValue"] = -21474.8359;
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        cv::Mat m = makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0);
        m(cv::Rect(250, 238, 20, 5)).setTo(static_cast<float>(-21474.8359)); // 截面窗口内、槽外
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "sentinel strip must be excluded");
        QVERIFY2(out.data("gap_found").toBool(), "real groove must still be found");
        const double width = out.data("gap_width").toDouble();
        QVERIFY2(std::abs(width - 40.0) < 1.5,
                 qPrintable(QString("sentinel strip must not fabricate corners, width ~40, got %1").arg(width)));
    }

    // 阶7 批3复核（P1-3）：两条无效条带夹真实平台——σ=7 高斯半径 21 > 条带
    // 半宽 10，旧补洞行为会把 100→85→100 混出伪造降/升沿（gap_found=true，
    // 宽度 ≈40 的假间隙）；NaN 保留后条带两侧不相邻，必须不检出
    void testGapInvalidBandsNoFabricatedGap() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(320, 240, 200, 5);
        p["smoothSigma"] = 7.0;
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        cv::Mat m(480, 640, CV_32F, cv::Scalar(100.0f));
        m(cv::Rect(310, 200, 20, 81)).setTo(85.0f); // 真实平台
        m(cv::Rect(290, 200, 20, 81)).setTo(0.0f);  // 无效条带 1（invalidValue=0）
        m(cv::Rect(330, 200, 20, 81)).setTo(0.0f);  // 无效条带 2
        ImageData input(m);
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "must succeed with not-found contract");
        QVERIFY2(!out.data("gap_found").toBool(), "invalid bands must not fabricate a gap");
        QCOMPARE(out.data("gap_width").toDouble(), -1.0);
        QVERIFY2(!out.data("is_pass").toBool(), "not-found must not pass");
    }

    // 阶7 批3复核三轮（P1-1）：GapMeasure3D autoNoData 开关——整列哨兵带
    // x∈[240,289]（24000 像素，检出条件满足）：开启时带 → NaN，真实槽
    // （300..339）照常测得 40；关闭时哨兵进入截面，伪造 ~50 宽假间隙
    void testGapAutoNoDataToggle() {
        cv::Mat m = makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0);
        m(cv::Rect(240, 0, 50, 480)).setTo(-21474836.0f);

        GapMeasure3DPlugin onP;
        onP.setParams(gapParams(320, 240, 200, 5));
        QVERIFY(onP.initialize());
        ImageData in1(m);
        ImageData out1;
        QVERIFY2(onP.execute(in1, out1), "auto NoData on must succeed");
        QVERIFY2(out1.data("gap_found").toBool(), "real groove must be found with sentinel excluded");
        QVERIFY2(std::abs(out1.data("gap_width").toDouble() - 40.0) < 1.5, "width ~40");

        QJsonObject off = gapParams(320, 240, 200, 5);
        off["autoNoData"] = false;
        GapMeasure3DPlugin offP;
        offP.setParams(off);
        QVERIFY(offP.initialize());
        ImageData in2(m);
        ImageData out2;
        QVERIFY2(offP.execute(in2, out2), "auto NoData off must succeed");
        QVERIFY2(out2.data("gap_found").toBool(), "sentinel band fabricates edges when not excluded");
        QVERIFY2(
            std::abs(out2.data("gap_width").toDouble() - 50.0) < 3.0,
            qPrintable(QString("fabricated width ~50 (sentinel band), got %1").arg(out2.data("gap_width").toDouble())));
    }

    void testGapPixelScaling() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(320, 240, 200, 5);
        p["pixelSizeX"] = 2.0;
        p["zScale"] = 2.0;
        p["specUpperLimit"] = 100.0;
        plugin.setParams(p);
        QVERIFY(plugin.initialize());
        ImageData input(makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "scaled measurement must succeed");
        QVERIFY(out.data("gap_found").toBool());
        const double width = out.data("gap_width").toDouble();
        QVERIFY2(std::abs(width - 80.0) < 3.0, qPrintable(QString("width 40px·2mm/px = 80, got %1").arg(width)));
        QVERIFY2(out.data("is_pass").toBool(), "80 <= spec 100");
    }

    void testGapRoiOutsideImageFails() {
        GapMeasure3DPlugin plugin;
        plugin.setParams(gapParams(5000, 5000, 100, 5));
        QVERIFY(plugin.initialize());
        ImageData input(makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0));
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "ROI outside image must fail");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("无交集")),
                 "error must report no intersection");
    }

    void testGapValidation() {
        GapMeasure3DPlugin plugin;
        QString error;
        QVERIFY(plugin.validateParams(gapParams(320, 240, 100, 5), error));
        QJsonObject evenMedian = gapParams(320, 240, 100, 5);
        evenMedian["medianSize"] = 4;
        QVERIFY(!plugin.validateParams(evenMedian, error)); // 偶数中值窗口
        QJsonObject smallMedian = gapParams(320, 240, 100, 5);
        smallMedian["medianSize"] = 2;
        QVERIFY(!plugin.validateParams(smallMedian, error)); // <3 非零窗口
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["roiLength"] = 2;
                return p;
            }(),
            error)); // 截面长度 <3
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["pixelSizeX"] = 0.0;
                return p;
            }(),
            error));
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["specUpperLimit"] = 0.0;
                return p;
            }(),
            error));
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["smoothSigma"] = -0.5;
                return p;
            }(),
            error));
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["minPeakDistance"] = 0;
                return p;
            }(),
            error));
        QJsonObject oddMedian = gapParams(320, 240, 100, 5);
        oddMedian["medianSize"] = 3;
        QVERIFY(plugin.validateParams(oddMedian, error)); // 奇数窗口合法
        // 阶7 批3复核（P2-4）：roiLength 公开下限与执行需求统一为 5
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["roiLength"] = 4;
                return p;
            }(),
            error));
        QVERIFY(plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["roiLength"] = 5;
                return p;
            }(),
            error));
        // 阶7 批3复核三轮（P1-1）：autoNoData 布尔严格
        QVERIFY(!plugin.validateParams(
            [&] {
                auto p = gapParams(320, 240, 100, 5);
                p["autoNoData"] = QStringLiteral("yes");
                return p;
            }(),
            error));
    }

    void testGapClone() {
        GapMeasure3DPlugin plugin;
        QJsonObject p = gapParams(100, 100, 50, 3);
        p["pixelSizeX"] = 0.5;
        plugin.setParams(p);
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* cloneGap = qobject_cast<GapMeasure3DPlugin*>(clone);
        QVERIFY(cloneGap != nullptr);
        QCOMPARE(cloneGap->currentParams()["pixelSizeX"].toDouble(), 0.5);
        cloneGap->setParams(gapParams(320, 240, 100, 5));
        QCOMPARE(plugin.currentParams()["pixelSizeX"].toDouble(), 0.5);
        delete clone;
    }

    // ---------- 流程验收 ----------

    // 流程验收：GrabImage(32F TIFF 高度图) → 3DPreProcessing(高度筛选剔除离群块)
    // → FitPlane(全图拟合)，经 PluginManager 真实加载 + RunEngine 调度
    void testFlowPreprocessToPlaneFit() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();

        QTemporaryDir dataDir;
        QVERIFY(dataDir.isValid());
        const QString tiffPath = dataDir.filePath("tilted_height.tiff");
        cv::Mat plane = makeTiltedPlane(640, 480, 0.05, -0.1, 50.0);
        plane(cv::Rect(0, 0, 40, 40)).setTo(5000.0f); // 离群块 → 高度筛选剔除
        QVERIFY(cv::imwrite(tiffPath.toStdString(), plane));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = tiffPath;
        project->addModule(grab);

        ModuleInstance pre;
        pre.id = QStringLiteral("pre");
        pre.moduleId = QStringLiteral("3DPreProcessing");
        pre.params["heightFilterEnabled"] = true;
        pre.params["heightFilterMin"] = -1000.0;
        pre.params["heightFilterMax"] = 1000.0;
        // 阶7 批3复核（P1-1）：非零 fillValue——下游 FitPlane 不再手工对齐参数，
        // 必须凭输入携带的 height_invalid_value 排除填充像素
        pre.params["fillValue"] = -7.0;
        project->addModule(pre);

        ModuleInstance fp;
        fp.id = QStringLiteral("fp");
        fp.moduleId = QStringLiteral("FitPlane"); // invalidValue 保持默认 0（≠ -7）
        project->addModule(fp);

        ModuleConnection c1;
        c1.fromModuleId = QStringLiteral("grab");
        c1.toModuleId = QStringLiteral("pre");
        c1.fromPort = QStringLiteral("image");
        c1.toPort = QStringLiteral("image");
        c1.edgeType = QStringLiteral("data");
        project->addConnection(c1);
        ModuleConnection c2;
        c2.fromModuleId = QStringLiteral("pre");
        c2.toModuleId = QStringLiteral("fp");
        c2.fromPort = QStringLiteral("image");
        c2.toPort = QStringLiteral("image");
        c2.edgeType = QStringLiteral("data");
        project->addConnection(c2);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();

        const ImageData preOut = engine.moduleOutput(QStringLiteral("pre"));
        QVERIFY2(preOut.hasData("valid_pixel_count"), "flow must produce preprocessing stats");
        QCOMPARE(preOut.data("filtered_pixel_count").toDouble(), 40.0 * 40.0);
        QCOMPARE(preOut.data("height_invalid_value").toDouble(), -7.0);
        QCOMPARE(preOut.toMat().depth(), CV_32F);

        const ImageData fpOut = engine.moduleOutput(QStringLiteral("fp"));
        QVERIFY2(fpOut.hasData("plane_nz"), "flow must produce plane normal");
        QCOMPARE(fpOut.data("valid_pixel_count").toDouble(), 640.0 * 480.0 - 40.0 * 40.0);
        const double norm = std::sqrt(0.05 * 0.05 + 0.1 * 0.1 + 1.0);
        QVERIFY2(std::abs(fpOut.data("plane_nx").toDouble() - (-0.05 / norm)) < 0.01, "flow nx");
        QVERIFY2(std::abs(fpOut.data("plane_ny").toDouble() - (0.1 / norm)) < 0.01, "flow ny");
        QVERIFY2(fpOut.data("flatness").toDouble() < 0.05, "outlier block removed → flatness small");
        QVERIFY(portValueMatchesType(fpOut.data("plane"), DataType::Plane3D));
    }

    // 流程验收：GrabImage(32F TIFF V 槽高度图) → GapMeasure3D，间隙宽度 ~40
    void testFlowGrabToGapMeasure3D() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();

        QTemporaryDir dataDir;
        QVERIFY(dataDir.isValid());
        const QString tiffPath = dataDir.filePath("groove_height.tiff");
        cv::Mat groove = makeGrooveHeight(640, 480, 200, 280, 300, 339, 100.0, 10.0);
        QVERIFY(cv::imwrite(tiffPath.toStdString(), groove));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = tiffPath;
        project->addModule(grab);

        ModuleInstance gap;
        gap.id = QStringLiteral("gap");
        gap.moduleId = QStringLiteral("GapMeasure3D");
        gap.params["roiCenterX"] = 320;
        gap.params["roiCenterY"] = 240;
        gap.params["roiLength"] = 200;
        gap.params["roiHeight"] = 5;
        gap.params["specUpperLimit"] = 50.0;
        project->addModule(gap);

        ModuleConnection conn;
        conn.fromModuleId = QStringLiteral("grab");
        conn.toModuleId = QStringLiteral("gap");
        conn.fromPort = QStringLiteral("image");
        conn.toPort = QStringLiteral("image");
        conn.edgeType = QStringLiteral("data");
        project->addConnection(conn);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();
        const ImageData out = engine.moduleOutput(QStringLiteral("gap"));
        QVERIFY2(out.hasData("gap_width"), "flow must produce gap_width");
        QVERIFY2(out.data("gap_found").toBool(), "flow must find the groove");
        const double width = out.data("gap_width").toDouble();
        QVERIFY2(std::abs(width - 40.0) < 1.5, qPrintable(QString("flow gap width ~40, got %1").arg(width)));
        QVERIFY2(out.data("is_pass").toBool(), "flow gap must pass spec 50");
    }

    // 流程验收（阶7 批3复核 P1-1 真实回归）：400×300 CV_32F TIFF，69% 像素为
    // NoData 哨兵 -21474836——默认流程（3DPreProcessing 关闭高度筛选 + FitPlane
    // 默认参数）必须自动检测哨兵并以非零 fillValue 契约键贯通剔除；
    // 无自动检测时 69% 哨兵像素进入拟合，平面彻底摧毁 → 测试变红
    void testFlowNoDataTiffEndToEnd() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();

        QTemporaryDir dataDir;
        QVERIFY(dataDir.isValid());
        const QString tiffPath = dataDir.filePath("nodata_height.tiff");
        cv::Mat m(300, 400, CV_32F);
        for (int y = 0; y < 300; ++y) {
            for (int x = 0; x < 400; ++x) {
                m.at<float>(y, x) = (x >= 124) ? -21474836.0f : static_cast<float>(0.05 * x - 0.1 * y + 50.0);
            }
        }
        QVERIFY(cv::imwrite(tiffPath.toStdString(), m)); // 124/400 = 31% 真实，69% NoData

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = tiffPath;
        project->addModule(grab);

        ModuleInstance pre;
        pre.id = QStringLiteral("pre");
        pre.moduleId = QStringLiteral("3DPreProcessing");
        pre.params["fillValue"] = -7.0; // 高度筛选保持默认关闭
        project->addModule(pre);

        ModuleInstance fp;
        fp.id = QStringLiteral("fp");
        fp.moduleId = QStringLiteral("FitPlane"); // 全默认参数（invalidValue=0 ≠ -7）
        project->addModule(fp);

        ModuleConnection c1;
        c1.fromModuleId = QStringLiteral("grab");
        c1.toModuleId = QStringLiteral("pre");
        c1.fromPort = QStringLiteral("image");
        c1.toPort = QStringLiteral("image");
        c1.edgeType = QStringLiteral("data");
        project->addConnection(c1);
        ModuleConnection c2;
        c2.fromModuleId = QStringLiteral("pre");
        c2.toModuleId = QStringLiteral("fp");
        c2.fromPort = QStringLiteral("image");
        c2.toPort = QStringLiteral("image");
        c2.edgeType = QStringLiteral("data");
        project->addConnection(c2);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();

        const ImageData preOut = engine.moduleOutput(QStringLiteral("pre"));
        QCOMPARE(preOut.data("filtered_pixel_count").toDouble(), 276.0 * 300.0); // 69% NoData
        QCOMPARE(preOut.data("valid_pixel_count").toDouble(), 124.0 * 300.0);
        QCOMPARE(preOut.data("height_invalid_value").toDouble(), -7.0);

        const ImageData fpOut = engine.moduleOutput(QStringLiteral("fp"));
        QVERIFY2(fpOut.hasData("plane_nz"), "flow must produce plane normal");
        QCOMPARE(fpOut.data("valid_pixel_count").toDouble(), 124.0 * 300.0);
        const double norm = std::sqrt(0.05 * 0.05 + 0.1 * 0.1 + 1.0);
        QVERIFY2(std::abs(fpOut.data("plane_nx").toDouble() - (-0.05 / norm)) < 0.01, "NoData-free nx");
        QVERIFY2(std::abs(fpOut.data("plane_ny").toDouble() - (0.1 / norm)) < 0.01, "NoData-free ny");
        QVERIFY2(fpOut.data("flatness").toDouble() < 0.05, "sentinel pixels must not enter the fit");
    }
};

QTEST_MAIN(Test3DBatch3)
#include "test_3d_batch3.moc"
