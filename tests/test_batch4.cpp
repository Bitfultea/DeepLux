#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/deeplux/DataContract.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "plugins/calibration/CalculateOffset/CalculateOffsetPlugin.h"
#include "plugins/calibration/NPointCalibration/NPointCalibrationPlugin.h"
#include "plugins/geometry/MeasurementInput/MeasurementInputPlugin.h"
#include "plugins/image_processing/CropImage/CropImagePlugin.h"
#include "plugins/image_processing/JigsawPuzzle/JigsawPuzzlePlugin.h"

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLineF>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWidget>
#include <QtTest>
#include <cmath>
#include <limits>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

using namespace DeepLux;

namespace {
// 确定性纹理图：value = (7x + 13y) % 256，裁剪内容可逐像素校验
cv::Mat makePatternImage(int w, int h) {
    cv::Mat m(h, w, CV_8UC1);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            m.at<uchar>(y, x) = static_cast<uchar>((7 * x + 13 * y) % 256);
        }
    }
    return m;
}

QJsonObject cropParams(const QJsonArray& rects, bool outputFirst = true) {
    return QJsonObject{{"rectangles", rects}, {"outputFirstAsImage", outputFirst}};
}

QJsonArray rect(double cx, double cy, double l1, double l2, double deg) {
    return QJsonArray{cx, cy, l1, l2, deg};
}

QJsonObject offsetParams(double cx, double cy, double ca, double tx, double ty, double ta) {
    return QJsonObject{{"currentX", cx}, {"currentY", cy}, {"currentAngle", ca},
                       {"targetX", tx},  {"targetY", ty},  {"targetAngle", ta}};
}
} // namespace

class TestBatch4 : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_appDir;

    // 阶7 批4：metadata/库/数据路径全部 CMake 注入（沿用批2复核二轮模式）
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
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("GrabImage"), QStringLiteral(TEST_BATCH4_META_GrabImage),
                               QStringLiteral(TEST_BATCH4_LIB_GrabImage)),
                 "install GrabImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("CropImage"), QStringLiteral(TEST_BATCH4_META_CropImage),
                               QStringLiteral(TEST_BATCH4_LIB_CropImage)),
                 "install CropImage");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("MeasureCircle"),
                               QStringLiteral(TEST_BATCH4_META_MeasureCircle),
                               QStringLiteral(TEST_BATCH4_LIB_MeasureCircle)),
                 "install MeasureCircle");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("MeasurementInput"),
                               QStringLiteral(TEST_BATCH4_META_MeasurementInput),
                               QStringLiteral(TEST_BATCH4_LIB_MeasurementInput)),
                 "install MeasurementInput");
        QVERIFY2(installPlugin(pluginRoot, QStringLiteral("CalculateOffset"),
                               QStringLiteral(TEST_BATCH4_META_CalculateOffset),
                               QStringLiteral(TEST_BATCH4_LIB_CalculateOffset)),
                 "install CalculateOffset");
        PluginManager::instance().addPluginPath(pluginRoot);
        QVERIFY(PluginManager::instance().initialize());
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("CropImage")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasureCircle")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("MeasurementInput")));
        QVERIFY(PluginManager::instance().loadPlugin(QStringLiteral("CalculateOffset")));
    }

    void cleanupTestCase() {
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void cleanup() {
        RunEngine::instance().stop();
        RunEngine::instance().clearModules();
        RunEngine::instance().clearOutputs();
    }

    // ---------- CropImage ----------

    void testCalibrationCompositionOverflowInvalidatesOutputProvenance() {
        ImageData input(makePatternImage(128, 128));
        ImageCoordinateProvenance source;
        source.sourceImageId = QStringLiteral("slice197-overflow-source");
        source.sourceSize = QSize(256, 256);
        source.localToSource = {1e308, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
        source.hasLocalToSourceTransform = true;
        QVERIFY(source.isValid());
        QVERIFY(std::isfinite(source.localToSource[0]));
        QCOMPARE(source.localToSource[0], 1e308);
        QCOMPARE(source.sourceSize, QSize(256, 256));
        input.setCoordinateProvenance(source);
        QVERIFY(input.hasCoordinateProvenance());
        const cv::Mat originalPixels = input.toMat().clone();
        const ImageCoordinateProvenance originalSource = input.coordinateProvenance();

        const QJsonArray calibrationPoints{QJsonObject{{QStringLiteral("image"), QJsonArray{0.0, 0.0}},
                                                       {QStringLiteral("world"), QJsonArray{0.0, 0.0}}},
                                           QJsonObject{{QStringLiteral("image"), QJsonArray{100.0, 0.0}},
                                                       {QStringLiteral("world"), QJsonArray{50.0, 0.0}}},
                                           QJsonObject{{QStringLiteral("image"), QJsonArray{0.0, 100.0}},
                                                       {QStringLiteral("world"), QJsonArray{0.0, 100.0}}},
                                           QJsonObject{{QStringLiteral("image"), QJsonArray{100.0, 100.0}},
                                                       {QStringLiteral("world"), QJsonArray{50.0, 100.0}}}};
        const QJsonObject calibrationParams{{QStringLiteral("calibrationType"), QStringLiteral("Affine")},
                                            {QStringLiteral("outputWidth"), 128},
                                            {QStringLiteral("outputHeight"), 128},
                                            {QStringLiteral("inverseTransform"), true},
                                            {QStringLiteral("calibrationPoints"), calibrationPoints}};

        ImageData controlInput(makePatternImage(128, 128));
        ImageCoordinateProvenance controlSource;
        controlSource.sourceImageId = QStringLiteral("slice197-control-source");
        controlSource.sourceSize = QSize(256, 256);
        controlSource.localToSource = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
        controlSource.hasLocalToSourceTransform = true;
        QVERIFY(controlSource.isValid());
        controlInput.setCoordinateProvenance(controlSource);
        NPointCalibrationPlugin controlCalibration;
        QVERIFY(controlCalibration.initialize());
        controlCalibration.setParams(calibrationParams);
        ImageData controlOutput;
        QVERIFY(controlCalibration.execute(controlInput, controlOutput));
        QVERIFY(controlOutput.data(QStringLiteral("calibration_valid")).toBool());
        QVERIFY(controlOutput.hasCoordinateProvenance());
        const auto controlMapped = controlOutput.coordinateProvenance().mapLocalToSource(QPointF(20.0, 40.0));
        QVERIFY(controlMapped.has_value());
        QVERIFY(QLineF(*controlMapped, QPointF(40.0, 40.0)).length() < 1e-3);

        NPointCalibrationPlugin calibration;
        QVERIFY(calibration.initialize());
        calibration.setParams(calibrationParams);

        // The four fixed pairs define x_world = x_image / 2, y_world = y_image.
        // Therefore output-to-input is finite, invertible diag(2, 1, 1); only
        // composing that matrix with the valid source scale 1e308 overflows.
        const ImageCoordinateProvenance::Matrix3 outputToInput{{2.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0}};
        for (double coefficient : outputToInput)
            QVERIFY(std::isfinite(coefficient));
        QCOMPARE(outputToInput[0], 2.0);
        QCOMPARE(outputToInput[4], 1.0);
        QCOMPARE(outputToInput[8], 1.0);
        QCOMPARE(outputToInput[0] * outputToInput[4] * outputToInput[8], 2.0);
        QVERIFY(!std::isfinite(source.localToSource[0] * outputToInput[0]));

        ImageData calibrated;
        QVERIFY(calibration.execute(input, calibrated));
        QVERIFY(calibrated.data(QStringLiteral("calibration_valid")).toBool());
        QVERIFY(calibrated.isValid());
        QVERIFY(!calibrated.isEmpty());
        QCOMPARE(calibrated.width(), 128);
        QCOMPARE(calibrated.height(), 128);
        QCOMPARE(calibrated.toMat().at<uchar>(40, 20), originalPixels.at<uchar>(40, 40));
        QVERIFY(!calibrated.hasCoordinateProvenance());

        QCOMPARE(cv::countNonZero(input.toMat() != originalPixels), 0);
        const ImageCoordinateProvenance unchanged = input.coordinateProvenance();
        QCOMPARE(unchanged.sourceImageId, originalSource.sourceImageId);
        QCOMPARE(unchanged.sourceSize, originalSource.sourceSize);
        QVERIFY(unchanged.hasLocalToSourceTransform);
        for (size_t index = 0; index < originalSource.localToSource.size(); ++index)
            QCOMPARE(unchanged.localToSource[index], originalSource.localToSource[index]);
    }

    void testCropAxisAligned() {
        CropImagePlugin plugin;
        QVERIFY(plugin.initialize()); // 默认参数：中心 100×100 矩形
        // 阶7 批4复核三轮（P1-3）：l1/l2 为半长（旧版 HALCON Length1/2 口径）——
        // 半长 100/50 → 裁剪 200×100
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(320, 240, 100, 50, 0))}));
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "axis-aligned crop must succeed");
        QCOMPARE(out.data("crop_count").toDouble(), 1.0);
        // outputFirstAsImage 默认 true：image 端口即首个裁剪图
        QCOMPARE(out.toMat().cols, 200);
        QCOMPARE(out.toMat().rows, 100);
        // 裁剪内容逐像素一致：crop(0,0) == src(190,220)
        const cv::Mat src = makePatternImage(640, 480);
        QCOMPARE(static_cast<int>(out.toMat().at<uchar>(0, 0)), static_cast<int>(src.at<uchar>(190, 220)));
        QCOMPARE(static_cast<int>(out.toMat().at<uchar>(99, 199)), static_cast<int>(src.at<uchar>(289, 419)));
        QVERIFY(portValueMatchesType(out.data("crop_images"), DataType::Table));
        const QVariantList rows = out.data("crop_images").toList();
        QCOMPARE(rows.size(), 1);
        const QVariantMap row = rows.first().toMap();
        QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
        QCOMPARE(row.value(QStringLiteral("crop_x")).toInt(), 220);
        QCOMPARE(row.value(QStringLiteral("crop_y")).toInt(), 190);
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 200);
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 100);
        QVERIFY(row.value(QStringLiteral("image")).canConvert<ImageData>());
        QCOMPARE(row.value(QStringLiteral("image")).value<ImageData>().toMat().cols, 200);
    }

    void testCropCoordinateProvenanceComposesAndSerializesOnlyPixels() {
        CropImagePlugin crop;
        QVERIFY(crop.initialize());
        ImageData source(makePatternImage(80, 60));
        QVERIFY(source.hasCoordinateProvenance());
        const ImageCoordinateProvenance sourceProvenance = source.coordinateProvenance();

        crop.setParams(cropParams(QJsonArray{rect(20, 30, 10, 10, 0)}));
        ImageData firstOutput;
        QVERIFY(crop.execute(source, firstOutput));
        QCOMPARE(firstOutput.coordinateProvenance().sourceImageId, sourceProvenance.sourceImageId);
        QCOMPARE(firstOutput.coordinateProvenance().sourceSize, QSize(80, 60));
        QCOMPARE(firstOutput.coordinateProvenance().localOriginInSource, QPointF(10, 20));
        const QVariantMap firstRow = firstOutput.data("crop_images").toList().first().toMap();
        const ImageData firstCrop = firstRow.value("image").value<ImageData>();
        QCOMPARE(firstCrop.coordinateProvenance().sourceImageId, sourceProvenance.sourceImageId);
        QCOMPARE(firstCrop.coordinateProvenance().localOriginInSource, QPointF(10, 20));
        const auto firstMapped = firstCrop.coordinateProvenance().mapLocalToSource(QPointF(19, 19));
        QVERIFY(firstMapped);
        QCOMPARE(*firstMapped, QPointF(29, 39));

        crop.setParams(cropParams(QJsonArray{rect(5, 6, 2, 3, 0)}));
        ImageData secondOutput;
        QVERIFY(crop.execute(firstCrop, secondOutput));
        QCOMPARE(secondOutput.coordinateProvenance().localOriginInSource, QPointF(13, 23));
        const ImageData secondCrop =
            secondOutput.data("crop_images").toList().first().toMap().value("image").value<ImageData>();
        QCOMPARE(secondCrop.coordinateProvenance().localOriginInSource, QPointF(13, 23));
        const auto secondMapped = secondCrop.coordinateProvenance().mapLocalToSource(QPointF(0, 0));
        QVERIFY(secondMapped);
        QCOMPARE(*secondMapped, QPointF(13, 23));

        const ImageData roundTrip = ImageData::fromByteArray(secondCrop.toByteArray());
        QVERIFY(!roundTrip.hasCoordinateProvenance());
        QCOMPARE(source.toMat().at<uchar>(20, 10), static_cast<uchar>((7 * 10 + 13 * 20) % 256));
    }

    void testMeasurementInputReportsLocalAndSourceCoordinates() {
        MeasurementInputPlugin plugin;
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(80, 60));
        input.offsetCoordinateOrigin(10, 20);
        plugin.setParams(QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_line")},
                                     {QStringLiteral("pick_coordinate_frame"), QStringLiteral("ImageLocalPixel")},
                                     {QStringLiteral("point"), QJsonArray{3.0, 4.0, 0.0}},
                                     {QStringLiteral("line"), QJsonArray{1.0, 2.0, 5.0, 6.0}}});
        ImageData output;
        QVERIFY(plugin.execute(input, output));
        QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
        QCOMPARE(output.data(QStringLiteral("source_coordinate_available")).toBool(), true);
        QCOMPARE(output.data(QStringLiteral("point_source")).toList(), QVariantList({13.0, 24.0, 0.0}));
        QCOMPARE(output.data(QStringLiteral("line_source")).toList(), QVariantList({11.0, 22.0, 15.0, 26.0}));

        input.invalidateCoordinateProvenance();
        QVERIFY(plugin.execute(input, output));
        QCOMPARE(output.data(QStringLiteral("source_coordinate_available")).toBool(), false);
        QVERIFY(!output.hasData(QStringLiteral("point_source")));

        input.establishCoordinateSource();
        input.offsetCoordinateOrigin(10, 20);
        plugin.setParams(
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_plane")},
                        {QStringLiteral("point"), QJsonArray{1.0, 2.0, 3.0}},
                        {QStringLiteral("plane"), QJsonArray{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0}}});
        QVERIFY(plugin.execute(input, output));
        QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("World3D"));
        QCOMPARE(output.data(QStringLiteral("source_coordinate_available")).toBool(), false);
        QCOMPARE(output.data(QStringLiteral("point")).toList(), QVariantList({1.0, 2.0, 3.0}));
        QVERIFY(!output.hasData(QStringLiteral("point_source")));
    }

    void testCropRotated45() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(320, 240, 70, 10, 45))})); // 半长 70/10
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "rotated crop must succeed");
        const QVariantMap row = out.data("crop_images").toList().first().toMap();
        // 包围盒半宽 = 70·cos45 + 10·sin45 ≈ 56.57 → 全宽 ≈113.1 → floor/ceil 取整 114
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 114);
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 114);
        QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
    }

    void testCropMultipleRectangles() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(
            QJsonArray{rect(100, 100, 80, 60, 0), rect(300, 200, 100, 100, 30), rect(500, 400, 120, 40, -45)}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "multi-rect crop must succeed");
        QCOMPARE(out.data("crop_count").toDouble(), 3.0);
        const QVariantList rows = out.data("crop_images").toList();
        QCOMPARE(rows.size(), 3);
        for (int i = 0; i < rows.size(); ++i) {
            const QVariantMap row = rows[i].toMap();
            QCOMPARE(row.value(QStringLiteral("index")).toInt(), i);
            QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
            const ImageData crop = row.value(QStringLiteral("image")).value<ImageData>();
            QCOMPARE(crop.toMat().cols, row.value(QStringLiteral("crop_width")).toInt());
            QCOMPARE(crop.toMat().rows, row.value(QStringLiteral("crop_height")).toInt());
            QVERIFY(crop.hasCoordinateProvenance());
            QCOMPARE(crop.coordinateProvenance().localOriginInSource,
                     QPointF(row.value(QStringLiteral("crop_x")).toInt(), row.value(QStringLiteral("crop_y")).toInt()));
        }
    }

    void testJigsawPuzzlePreservesKnownPixelLineage() {
        ImageData input(makePatternImage(19, 17));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("jigsaw-parent");
        provenance.sourceSize = QSize(100, 90);
        QVERIFY(provenance.composeLocalToSource(
            ImageCoordinateProvenance::Matrix3{{2.0, 0.0, 10.0, 0.0, 3.0, 5.0, 0.0, 0.0, 1.0}}));
        input.setCoordinateProvenance(provenance);

        JigsawPuzzlePlugin puzzle;
        puzzle.setParams(QJsonObject{{QStringLiteral("rows"), 3}, {QStringLiteral("cols"), 4}});
        QVERIFY(puzzle.initialize());
        ImageData output;
        QVERIFY(puzzle.execute(input, output));
        QCOMPARE(output.width(), 16);
        QCOMPARE(output.height(), 15);
        QVERIFY(output.hasCoordinateProvenance());
        QCOMPARE(output.coordinateProvenance().sourceImageId, QStringLiteral("jigsaw-parent"));
        QCOMPARE(output.coordinateProvenance().sourceSize, QSize(100, 90));
        for (int y = 0; y < output.height(); ++y) {
            for (int x = 0; x < output.width(); ++x)
                QCOMPARE(output.toMat().at<uchar>(y, x), input.toMat().at<uchar>(y, x));
        }
        QCOMPARE(output.coordinateProvenance().mapLocalToSource(QPointF(3, 4)).value(), QPointF(16, 17));
        QCOMPARE(output.coordinateProvenance().mapLocalToSource(QPointF(14, 10)).value(), QPointF(38, 35));
    }

    void testJigsawPuzzleDoesNotInventLineageForUntrackedOrEmptyOutput() {
        ImageData untracked(makePatternImage(19, 17));
        untracked.invalidateCoordinateProvenance();
        JigsawPuzzlePlugin puzzle;
        QVERIFY(puzzle.initialize());
        ImageData output;
        QVERIFY(puzzle.execute(untracked, output));
        QVERIFY(!output.hasCoordinateProvenance());

        ImageData smallImage(makePatternImage(19, 17));
        smallImage.establishCoordinateSource(QStringLiteral("small-image"));
        smallImage.setData(QStringLiteral("sentinel"), QStringLiteral("unchanged"));
        const ImageData inputBefore = smallImage;
        ImageData previousOutput(makePatternImage(4, 4));
        previousOutput.setData(QStringLiteral("previous"), true);
        output = previousOutput;
        QSignalSpy errorSpy(&puzzle, &ModuleBase::errorOccurred);

        puzzle.setParams(QJsonObject{{QStringLiteral("rows"), 18}, {QStringLiteral("cols"), 3}});
        QVERIFY(!puzzle.execute(smallImage, output));
        QVERIFY(output.isEmpty());
        QVERIFY(!output.hasCoordinateProvenance());
        QVERIFY(output.allData().isEmpty());
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(!errorSpy.takeFirst().first().toString().isEmpty());
        QCOMPARE(smallImage.data(QStringLiteral("sentinel")).toString(), QStringLiteral("unchanged"));
        QCOMPARE(smallImage.coordinateProvenance().localToSourceMatrix(),
                 inputBefore.coordinateProvenance().localToSourceMatrix());
        QCOMPARE(smallImage.toMat().at<uchar>(4, 7), inputBefore.toMat().at<uchar>(4, 7));

        previousOutput = ImageData(makePatternImage(4, 4));
        previousOutput.setData(QStringLiteral("previous"), true);
        output = previousOutput;
        errorSpy.clear();
        puzzle.setParams(QJsonObject{{QStringLiteral("rows"), 3}, {QStringLiteral("cols"), 20}});
        QVERIFY(!puzzle.execute(smallImage, output));
        QVERIFY(output.isEmpty());
        QVERIFY(!output.hasCoordinateProvenance());
        QVERIFY(output.allData().isEmpty());
        QCOMPARE(errorSpy.count(), 1);
        QVERIFY(!errorSpy.takeFirst().first().toString().isEmpty());
        QCOMPARE(smallImage.data(QStringLiteral("sentinel")).toString(), QStringLiteral("unchanged"));
        QCOMPARE(smallImage.coordinateProvenance().localToSourceMatrix(),
                 inputBefore.coordinateProvenance().localToSourceMatrix());
        QCOMPARE(smallImage.toMat().at<uchar>(4, 7), inputBefore.toMat().at<uchar>(4, 7));
    }

    void testCalibrationProvidesImageCoordinateProvenance() {
        ImageData input(makePatternImage(16, 16));

        NPointCalibrationPlugin calibration;
        QVERIFY(calibration.initialize());
        const QJsonArray calibrationPoints{QJsonObject{{QStringLiteral("image"), QJsonArray{0.0, 0.0}},
                                                       {QStringLiteral("world"), QJsonArray{0.0, 0.0}}},
                                           QJsonObject{{QStringLiteral("image"), QJsonArray{15.0, 0.0}},
                                                       {QStringLiteral("world"), QJsonArray{30.0, 0.0}}},
                                           QJsonObject{{QStringLiteral("image"), QJsonArray{15.0, 15.0}},
                                                       {QStringLiteral("world"), QJsonArray{30.0, 30.0}}},
                                           QJsonObject{{QStringLiteral("image"), QJsonArray{0.0, 15.0}},
                                                       {QStringLiteral("world"), QJsonArray{0.0, 30.0}}}};
        calibration.setParams(QJsonObject{{QStringLiteral("calibrationType"), QStringLiteral("Perspective")},
                                          {QStringLiteral("outputWidth"), 16},
                                          {QStringLiteral("outputHeight"), 16},
                                          {QStringLiteral("inverseTransform"), true},
                                          {QStringLiteral("clearPointsOnRun"), true},
                                          {QStringLiteral("calibrationPoints"), calibrationPoints}});
        ImageData calibrated;
        QVERIFY(calibration.execute(input, calibrated));
        QVERIFY(calibrated.data(QStringLiteral("calibration_valid")).toBool());
        QVERIFY(calibrated.hasCoordinateProvenance());
        const auto sourcePoint = calibrated.coordinateProvenance().mapLocalToSource(QPointF(10.0, 10.0));
        QVERIFY(sourcePoint);
        QVERIFY(std::abs(sourcePoint->x() - 5.0) < 1e-3);
        QVERIFY(std::abs(sourcePoint->y() - 5.0) < 1e-3);
    }

    void testCalibrationCoordinateProvenanceComposesAffineAndPerspectiveMappings() {
        const auto makeCalibrationPoints = [](const std::vector<cv::Point2f>& imagePoints,
                                              const std::vector<cv::Point2f>& worldPoints) {
            QJsonArray points;
            for (size_t i = 0; i < imagePoints.size(); ++i) {
                points.append(QJsonObject{{QStringLiteral("image"), QJsonArray{imagePoints[i].x, imagePoints[i].y}},
                                          {QStringLiteral("world"), QJsonArray{worldPoints[i].x, worldPoints[i].y}}});
            }
            return points;
        };
        const auto verifyMapped = [](const ImageData& image, const QPointF& outputPoint,
                                     const QPointF& expectedSource) {
            const auto mapped = image.coordinateProvenance().mapLocalToSource(outputPoint);
            QVERIFY(mapped);
            QVERIFY2(QLineF(*mapped, expectedSource).length() < 1e-3, "calibrated point must map to source pixels");
        };

        CropImagePlugin crop;
        QVERIFY(crop.initialize());
        crop.setParams(cropParams(QJsonArray{rect(74, 84, 64, 64, 0)}));
        ImageData cropOutput;
        QVERIFY(crop.execute(ImageData(makePatternImage(256, 256)), cropOutput));
        const ImageData input = cropOutput.data(QStringLiteral("crop_images"))
                                    .toList()
                                    .first()
                                    .toMap()
                                    .value(QStringLiteral("image"))
                                    .value<ImageData>();
        const std::vector<cv::Point2f> affineImage{{0, 0}, {100, 0}, {100, 100}, {0, 100}};
        const std::vector<cv::Point2f> affineWorld{{10, 20}, {210, 20}, {210, 320}, {10, 320}};
        NPointCalibrationPlugin affine;
        QVERIFY(affine.initialize());
        affine.setParams(
            QJsonObject{{QStringLiteral("calibrationType"), QStringLiteral("Affine")},
                        {QStringLiteral("outputWidth"), 128},
                        {QStringLiteral("outputHeight"), 128},
                        {QStringLiteral("inverseTransform"), true},
                        {QStringLiteral("calibrationPoints"), makeCalibrationPoints(affineImage, affineWorld)}});
        ImageData affineOutput;
        QVERIFY(affine.execute(input, affineOutput));
        verifyMapped(affineOutput, QPointF(50.0, 80.0), QPointF(30.0, 40.0));
        verifyMapped(affineOutput, QPointF(90.0, 110.0), QPointF(50.0, 50.0));

        crop.setParams(cropParams(QJsonArray{rect(64, 64, 20, 10, 0)}));
        ImageData postCalibrationCropOutput;
        QVERIFY(crop.execute(affineOutput, postCalibrationCropOutput));
        const ImageData postCalibrationCrop = postCalibrationCropOutput.data(QStringLiteral("crop_images"))
                                                  .toList()
                                                  .first()
                                                  .toMap()
                                                  .value(QStringLiteral("image"))
                                                  .value<ImageData>();
        verifyMapped(postCalibrationCrop, QPointF(5.0, 5.0), QPointF(29.5, 33.0));

        MeasurementInputPlugin measurement;
        QVERIFY(measurement.initialize());
        measurement.setParams(QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_pair")},
                                          {QStringLiteral("point1"), QJsonArray{50.0, 80.0}},
                                          {QStringLiteral("point2"), QJsonArray{90.0, 110.0}}});
        ImageData measured;
        QVERIFY(measurement.execute(affineOutput, measured));
        QCOMPARE(measured.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
        QCOMPARE(measured.data(QStringLiteral("source_coordinate_available")).toBool(), true);
        QCOMPARE(measured.data(QStringLiteral("point1")).toList(), QVariantList({50.0, 80.0}));
        QCOMPARE(measured.data(QStringLiteral("point1_source")).toList(), QVariantList({30.0, 40.0}));
        QCOMPARE(measured.data(QStringLiteral("point2_source")).toList(), QVariantList({50.0, 50.0}));
        QCOMPARE(measured.data(QStringLiteral("source_image_id")).toString(),
                 input.coordinateProvenance().sourceImageId);
        QCOMPARE(measured.data(QStringLiteral("source_image_size")).toList(), QVariantList({256, 256}));

        const ImageData noSource = ImageData::fromByteArray(input.toByteArray());
        QVERIFY(!noSource.hasCoordinateProvenance());
        ImageData untrackedCalibration;
        QVERIFY(affine.execute(noSource, untrackedCalibration));
        QVERIFY(!untrackedCalibration.hasCoordinateProvenance());
        QVERIFY(measurement.execute(untrackedCalibration, measured));
        QCOMPARE(measured.data(QStringLiteral("source_coordinate_available")).toBool(), false);
        QVERIFY(!measured.hasData(QStringLiteral("point1_source")));

        const std::vector<cv::Point2f> perspectiveImage{{0, 0}, {100, 0}, {100, 100}, {0, 100}};
        const std::vector<cv::Point2f> perspectiveWorld{{10, 20}, {110, 25}, {95, 120}, {5, 90}};
        NPointCalibrationPlugin perspective;
        QVERIFY(perspective.initialize());
        perspective.setParams(QJsonObject{
            {QStringLiteral("calibrationType"), QStringLiteral("Perspective")},
            {QStringLiteral("outputWidth"), 128},
            {QStringLiteral("outputHeight"), 128},
            {QStringLiteral("inverseTransform"), true},
            {QStringLiteral("calibrationPoints"), makeCalibrationPoints(perspectiveImage, perspectiveWorld)}});
        ImageData perspectiveOutput;
        QVERIFY(perspective.execute(input, perspectiveOutput));
        cv::Mat forward = cv::getPerspectiveTransform(perspectiveImage, perspectiveWorld);
        const std::vector<cv::Point2f> sourceInputs{{20, 30}, {70, 40}, {45, 80}};
        std::vector<cv::Point2f> outputPoints;
        cv::perspectiveTransform(sourceInputs, outputPoints, forward);
        for (size_t i = 0; i < sourceInputs.size(); ++i) {
            verifyMapped(perspectiveOutput, QPointF(outputPoints[i].x, outputPoints[i].y),
                         QPointF(sourceInputs[i].x + 10.0, sourceInputs[i].y + 20.0));
        }
        QVERIFY(!ImageData::fromByteArray(perspectiveOutput.toByteArray()).hasCoordinateProvenance());
    }

    void testProjectiveCoordinateMappingRejectsHorizonAndKeepsPointSetAlignment() {
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("projective-source");
        provenance.sourceSize = QSize(100, 100);
        QVERIFY(provenance.composeLocalToSource({{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -10.0}}));
        QVERIFY(provenance.mapLocalToSource(QPointF(2.0, 3.0)));
        QVERIFY(!provenance.mapLocalToSource(QPointF(10.0, 3.0)));
        QVERIFY(!provenance.mapRectToSourceBounds(QRectF(0.0, 0.0, 20.0, 10.0)));
        QVERIFY(!provenance.composeLocalToSource({{1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0}}));

        MeasurementInputPlugin measurement;
        QVERIFY(measurement.initialize());
        ImageData input(makePatternImage(32, 32));
        provenance.sourceImageId = input.coordinateProvenance().sourceImageId;
        provenance.sourceSize = input.coordinateProvenance().sourceSize;
        input.setCoordinateProvenance(provenance);
        measurement.setParams(QJsonObject{
            {QStringLiteral("mode"), QStringLiteral("point_set")},
            {QStringLiteral("points"), QJsonArray{QJsonArray{2.0, 3.0}, QJsonArray{10.0, 3.0}, QJsonArray{4.0, 5.0}}}});
        ImageData output;
        QVERIFY(measurement.execute(input, output));
        const QVariantList mappedPoints = output.data(QStringLiteral("fit_points_source")).toList();
        QCOMPARE(mappedPoints.size(), 3);
        QCOMPARE(mappedPoints[0].toList().size(), 2);
        QVERIFY(!mappedPoints[1].isValid() || mappedPoints[1].isNull());
        QCOMPARE(mappedPoints[2].toList().size(), 2);
    }

    void testCropOutsideImageInvalidRow() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(320, 240, 100, 100, 0), rect(5000, 5000, 50, 50, 0)}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "mixed valid/invalid must succeed");
        QCOMPARE(out.data("crop_count").toDouble(), 1.0); // 无效矩形不计数
        const QVariantList rows = out.data("crop_images").toList();
        QCOMPARE(rows.size(), 2); // 但保留 valid=false 行（不静默丢弃）
        QCOMPARE(rows[1].toMap().value(QStringLiteral("valid")).toBool(), false);
        QCOMPARE(rows[1].toMap().value(QStringLiteral("crop_width")).toInt(), 0);
        QVERIFY(!rows[1].toMap().value(QStringLiteral("image")).canConvert<ImageData>());
    }

    void testCropAllOutsideFails() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{rect(5000, 5000, 50, 50, 0), rect(-9000, 10, 50, 50, 0)}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData out;
        QVERIFY2(!plugin.execute(input, out), "all-outside must fail closed");
        QVERIFY2(errSpy.count() >= 1 && errSpy.first().at(0).toString().contains(QStringLiteral("无交集")),
                 "error must report no intersection");
    }

    void testCropClampedToImage() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(10, 10, 50, 50, 0))})); // 半长 50，越出左上边界
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "clamped crop must succeed");
        const QVariantMap row = out.data("crop_images").toList().first().toMap();
        QCOMPARE(row.value(QStringLiteral("crop_x")).toInt(), 0);
        QCOMPARE(row.value(QStringLiteral("crop_y")).toInt(), 0);
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 60);
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 60);
    }

    // 阶7 批4复核（P1-3）：分数中心——包含式包围盒 floor/ceil 不得向内丢像素
    // （旧 qRound：220.5→221、420.5→421 得宽 200 且丢失 x=420 列）
    void testCropFractionalCenter() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(320.5, 240.5, 100, 50, 0))})); // 半长 100/50
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "fractional center crop must succeed");
        const QVariantMap row = out.data("crop_images").toList().first().toMap();
        QCOMPARE(row.value(QStringLiteral("crop_x")).toInt(), 220);      // floor(220.5)
        QCOMPARE(row.value(QStringLiteral("crop_y")).toInt(), 190);      // floor(190.5)
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 201);  // ceil(420.5)-220
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 101); // ceil(290.5)-190
    }

    // 阶7 批4复核（P1-3）：小尺寸矩形不得因取整产生零宽/零高被误判无效
    void testCropTinyRectStillValid() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(320, 240, 0.5, 0.5, 0))}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "tiny rect must stay valid (旧 qRound 得零宽判无效)");
        const QVariantMap row = out.data("crop_images").toList().first().toMap();
        QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
        QVERIFY(row.value(QStringLiteral("crop_width")).toInt() >= 1);
        QVERIFY(row.value(QStringLiteral("crop_height")).toInt() >= 1);
    }

    // 阶7 批4复核（P1-3）：贴边 ROI——原点处 1×1 矩形钳制后仍有效
    void testCropEdgeTouching() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(0, 0, 1, 1, 0))}));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "edge-touching ROI must stay valid");
        const QVariantMap row = out.data("crop_images").toList().first().toMap();
        QCOMPARE(row.value(QStringLiteral("valid")).toBool(), true);
        QCOMPARE(row.value(QStringLiteral("crop_x")).toInt(), 0);
        QCOMPARE(row.value(QStringLiteral("crop_width")).toInt(), 1);
        QCOMPARE(row.value(QStringLiteral("crop_height")).toInt(), 1);
    }

    // 阶7 批4复核（P1-2/三轮）：GUI 可配置性——createConfigWidget 提供矩形数组
    // 编辑；合法 JSON 经 validateParams+setParams 写入；非法编辑毒化草稿参数，
    // 由外层"确定"统一校验阻止关闭（错误输入不被静默丢弃）
    void testCropConfigWidget() {
        CropImagePlugin plugin;
        QVERIFY(plugin.initialize());
        QWidget* w = plugin.createConfigWidget();
        QVERIFY(w != nullptr);
        auto* rectsEdit = w->findChild<QLineEdit*>("CropImageRectanglesEdit");
        QVERIFY(rectsEdit != nullptr);
        auto* firstCheck = w->findChild<QCheckBox*>("CropImageOutputFirstCheck");
        QVERIFY(firstCheck != nullptr);
        auto* status = w->findChild<QLabel*>("CropImageConfigStatus");
        QVERIFY(status != nullptr);
        QString verr;

        rectsEdit->setText(QStringLiteral("[[10,10,20,20,0],[50,50,30,30,45]]"));
        const QJsonArray rects = plugin.currentParams()["rectangles"].toArray();
        QCOMPARE(rects.size(), 2);
        QCOMPARE(rects[0].toArray()[2].toDouble(), 20.0);
        QVERIFY(plugin.validateParams(plugin.currentParams(), verr));

        firstCheck->setChecked(false);
        QCOMPARE(plugin.currentParams()["outputFirstAsImage"].toBool(), false);

        // 阶7 批4复核三轮（P1-2）：非法编辑必须把非法状态写入草稿（setParam 直写），
        // 使外层"确定"的统一校验（validateParams(草稿参数)）阻止关闭弹窗——
        // 仅"保留旧参数"会让外层校验通过、错误输入被静默丢弃
        rectsEdit->setText(QStringLiteral("not json"));
        QVERIFY(!status->text().isEmpty());
        QVERIFY2(!plugin.validateParams(plugin.currentParams(), verr),
                 "unparsable edit must poison draft params so outer OK validation blocks closing");

        // 语义非法（l1=0）：同样毒化草稿
        rectsEdit->setText(QStringLiteral("[[10,10,0,20,0]]"));
        QVERIFY2(!plugin.validateParams(plugin.currentParams(), verr),
                 "semantic-invalid edit must poison draft params too");

        // 恢复合法输入：草稿重新可通过外层校验
        rectsEdit->setText(QStringLiteral("[[10,10,20,20,0]]"));
        QVERIFY2(plugin.validateParams(plugin.currentParams(), verr),
                 qPrintable(QString("valid edit must restore draft, err: %1").arg(verr)));
        QCOMPARE(plugin.currentParams()["rectangles"].toArray().size(), 1);
        delete w;
    }

    void testCropOutputFirstAsImageFalse() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(320, 240, 200, 100, 0))}, false));
        QVERIFY(plugin.initialize());
        ImageData input(makePatternImage(640, 480));
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "passthrough mode must succeed");
        QCOMPARE(out.toMat().cols, 640); // image 端口原样透传
        QCOMPARE(out.toMat().rows, 480);
        QCOMPARE(out.data("crop_count").toDouble(), 1.0); // 裁剪图组仍在 Table
        QCOMPARE(out.data("crop_images").toList().size(), 1);
    }

    void testCropValidation() {
        CropImagePlugin plugin;
        QString error;
        QVERIFY(plugin.validateParams(cropParams(QJsonArray{QJsonValue(rect(0, 0, 10, 10, 0))}), error));
        QVERIFY(!plugin.validateParams(QJsonObject{{"rectangles", 42}, {"outputFirstAsImage", true}}, error)); // 非数组
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{}), error));                                      // 空数组
        QJsonArray tooMany;
        for (int i = 0; i < 65; ++i) {
            tooMany.append(rect(0, 0, 10, 10, 0));
        }
        QVERIFY(!plugin.validateParams(cropParams(tooMany), error)); // 超上限 64
        QVERIFY(
            !plugin.validateParams(cropParams(QJsonArray{QJsonValue(QJsonArray{1.0, 2.0, 3.0, 4.0})}), error)); // 4 元
        QVERIFY(!plugin.validateParams(
            cropParams(QJsonArray{QJsonValue(QJsonArray{1.0, 2.0, 3.0, 4.0, QStringLiteral("0")})}),
            error));                                                                                       // 字符串元素
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{QJsonValue(rect(0, 0, 0, 10, 0))}), error));  // l1=0
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{QJsonValue(rect(0, 0, -5, 10, 0))}), error)); // l1<0
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{QJsonValue(rect(0, 0, 10, 10, 400))}), error)); // deg 超界
        QVERIFY(!plugin.validateParams(cropParams(QJsonArray{QJsonValue(rect(1000000.5, 0, 10, 10, 0))}),
                                       error)); // cx 超界
        QVERIFY(!plugin.validateParams(QJsonObject{{"rectangles", QJsonArray{QJsonValue(rect(0, 0, 10, 10, 0))}},
                                                   {"outputFirstAsImage", QStringLiteral("yes")}},
                                       error)); // 布尔严格
        QVERIFY(
            plugin.validateParams(cropParams(QJsonArray{QJsonValue(rect(0, 0, 1e6, 1e6, 360))}), error)); // 边界合法
    }

    void testCropClone() {
        CropImagePlugin plugin;
        plugin.setParams(cropParams(QJsonArray{QJsonValue(rect(50, 50, 40, 30, 15))}, false));
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* cloneCrop = qobject_cast<CropImagePlugin*>(clone);
        QVERIFY(cloneCrop != nullptr);
        QCOMPARE(cloneCrop->currentParams()["outputFirstAsImage"].toBool(), false);
        cloneCrop->setParams(cropParams(QJsonArray{QJsonValue(rect(0, 0, 10, 10, 0))}));
        QCOMPARE(plugin.currentParams()["outputFirstAsImage"].toBool(), false); // 原体不受影响
        delete clone;
    }

    // ---------- CalculateOffset ----------

    // 阶7 批4复核（P1-1）：方向与旧版一致——offset = 实测(current) − 参考(target)
    void testOffsetBasic() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(10, 20, 30, 35, 55, 40));
        QVERIFY(plugin.initialize());
        ImageData input;
        ImageData out;
        QVERIFY2(plugin.execute(input, out), "basic offset must succeed");
        QCOMPARE(out.data("offset_x").toDouble(), -25.0); // 10-35（实测−参考）
        QCOMPARE(out.data("offset_y").toDouble(), -35.0); // 20-55
        QCOMPARE(out.data("offset_a").toDouble(), -10.0); // 30-40
    }

    void testOffsetAngleNormalization() {
        CalculateOffsetPlugin plugin;
        QVERIFY(plugin.initialize());
        const auto run = [&plugin](double curA, double tgtA) {
            plugin.setParams(offsetParams(0, 0, curA, 0, 0, tgtA));
            ImageData in;
            ImageData out;
            if (!plugin.execute(in, out)) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            return out.data("offset_a").toDouble();
        };
        // offset_a = normalize180(current − target)（实测−参考）
        QCOMPARE(run(170, -170), -20.0); // +340 → -20
        QCOMPARE(run(0, 181), 179.0);    // -181 → +179
        QCOMPARE(run(0, 180), 180.0);    // -180 归一化为 +180（(-180,180] 口径）
        QCOMPARE(run(0, -180), 180.0);   // +180 上界保留
        QCOMPARE(run(-350, 0), 10.0);    // -350 → +10
        QCOMPARE(run(0, 0), 0.0);
    }

    // 阶7 批4复核（P1-4）：current_angle Number 端口逐帧覆盖当前角度（旧版
    // DegLink 运行时链接对应物）；类型错误/非有限失败关闭；无端口回退参数
    void testOffsetAnglePortOverride() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(0, 0, 0, 0, 0, 40)); // targetAngle=40
        QVERIFY(plugin.initialize());

        ImageData in;
        in.setData("current_angle", 100.0);
        ImageData out;
        QVERIFY2(plugin.execute(in, out), "current_angle port must override");
        QCOMPARE(out.data("offset_a").toDouble(), 60.0); // 100-40

        ImageData badIn;
        badIn.setData("current_angle", QStringLiteral("90"));
        QSignalSpy spy(&plugin, &DeepLux::IModule::errorOccurred);
        ImageData badOut;
        QVERIFY2(!plugin.execute(badIn, badOut), "wrong-typed angle port must fail closed");
        QVERIFY2(spy.count() >= 1 && spy.first().at(0).toString().contains(QStringLiteral("类型非法")),
                 "error must report type violation");

        ImageData nanIn;
        nanIn.setData("current_angle", std::numeric_limits<double>::quiet_NaN());
        ImageData nanOut;
        QVERIFY2(!plugin.execute(nanIn, nanOut), "NaN angle port must fail closed");

        ImageData noPortIn;
        ImageData noPortOut;
        QVERIFY2(plugin.execute(noPortIn, noPortOut), "no port must fall back to params");
        QCOMPARE(noPortOut.data("offset_a").toDouble(), -40.0); // 0-40
    }

    // Point3D 端口覆盖当前坐标：插件判定与核心契约一致（批1模式），
    // 非 3 数值列表/字符串/QPointF 一律拒绝，非有限值失败关闭
    void testOffsetPointPortContract() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(0, 0, 0, 15, 25, 0));
        QVERIFY(plugin.initialize());

        // 合法：3 数值列表 → 覆盖 currentX/Y
        ImageData okIn;
        okIn.setData("current_point", QVariantList{5.0, 7.0, 9.0});
        QVERIFY(portValueMatchesType(okIn.data("current_point"), DataType::Point3D));
        ImageData okOut;
        QVERIFY2(plugin.execute(okIn, okOut), "Point3D list must override current coords");
        QCOMPARE(okOut.data("offset_x").toDouble(), -10.0); // 5-15（实测−参考）
        QCOMPARE(okOut.data("offset_y").toDouble(), -18.0); // 7-25

        // 非法载荷：判定必须与核心契约一致（全部拒绝）
        const QVariantList badPayloads{
            QVariant(QVariantList{5.0, 7.0}),                      // 2 数值（Point2D 不是 Point3D）
            QVariant(QVariantList{QStringLiteral("5"), 7.0, 9.0}), // 字符串元素
            QVariant(QPointF(5, 7)),                               // QPointF 非数值列表
            QVariant(QVariantList{std::numeric_limits<double>::quiet_NaN(), 7.0, 9.0}), // NaN（类型合法值非法）
        };
        for (const QVariant& bad : badPayloads) {
            ImageData in;
            in.setData("current_point", bad);
            QSignalSpy errSpy(&plugin, &DeepLux::IModule::errorOccurred);
            ImageData out;
            QVERIFY2(!plugin.execute(in, out), "illegal current_point must fail closed");
            QVERIFY2(errSpy.count() >= 1, "must emit error");
            const QString msg = errSpy.first().at(0).toString();
            QVERIFY2(msg.contains(QStringLiteral("格式非法")) || msg.contains(QStringLiteral("非有限")),
                     qPrintable(QString("error must be format/non-finite, got: %1").arg(msg)));
        }
        // 无端口时回退参数
        ImageData noPortIn;
        ImageData noPortOut;
        QVERIFY2(plugin.execute(noPortIn, noPortOut), "no port must fall back to params");
        QCOMPARE(noPortOut.data("offset_x").toDouble(), -15.0); // 0-15
    }

    // 阶7 批4复核三轮（P0-1）：超大有限角度必须常数时间归一——旧循环实现对
    // 1e300 因 d-360==d 浮点吸收永不终止（流程永久卡死）；配合 CTest TIMEOUT
    // 防挂死回归
    void testOffsetHugeAngleTerminates() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(0, 0, 0, 0, 0, 40));
        QVERIFY(plugin.initialize());
        const double huge[] = {1e300, -1e300, 1e18, -1e18};
        for (const double a : huge) {
            ImageData in;
            in.setData("current_angle", a);
            ImageData out;
            QVERIFY2(plugin.execute(in, out), qPrintable(QString("huge angle %1 must terminate").arg(a)));
            const double oa = out.data("offset_a").toDouble();
            QVERIFY2(std::isfinite(oa), "offset_a must be finite");
            QVERIFY2(oa > -180.0 && oa <= 180.0,
                     qPrintable(QString("offset_a %1 must normalize into (-180,180]").arg(oa)));
        }
    }

    void testOffsetValidation() {
        CalculateOffsetPlugin plugin;
        QString error;
        QVERIFY(plugin.validateParams(offsetParams(0, 0, 0, 0, 0, 0), error));
        QVERIFY(!plugin.validateParams(offsetParams(0, 0, 400, 0, 0, 0), error));       // 角度超界
        QVERIFY(!plugin.validateParams(offsetParams(1000000.5, 0, 0, 0, 0, 0), error)); // X 超界
        QVERIFY(!plugin.validateParams(QJsonObject{{"currentX", QStringLiteral("1")},
                                                   {"currentY", 0.0},
                                                   {"currentAngle", 0.0},
                                                   {"targetX", 0.0},
                                                   {"targetY", 0.0},
                                                   {"targetAngle", 0.0}},
                                       error));                                               // 字符串数值
        QVERIFY(plugin.validateParams(offsetParams(-1e6, -1e6, -360, 1e6, 1e6, 360), error)); // 边界合法
    }

    void testOffsetClone() {
        CalculateOffsetPlugin plugin;
        plugin.setParams(offsetParams(1, 2, 3, 4, 5, 6));
        IModule* clone = plugin.clone();
        QVERIFY(clone != nullptr);
        auto* cloneOffset = qobject_cast<CalculateOffsetPlugin*>(clone);
        QVERIFY(cloneOffset != nullptr);
        QCOMPARE(cloneOffset->currentParams()["targetX"].toDouble(), 4.0);
        cloneOffset->setParams(offsetParams(0, 0, 0, 0, 0, 0));
        QCOMPARE(plugin.currentParams()["targetX"].toDouble(), 4.0); // 原体不受影响
        delete clone;
    }

    // ---------- 流程验收 ----------

    // 流程验收：GrabImage → CropImage(300×300 中心裁剪) → MeasureCircle——
    // 裁剪图直接驱动下游圆测量（圆心换算到裁剪坐标系 150,150），证明
    // outputFirstAsImage 的 image 端口替换在真实调度中生效
    void testFlowGrabToCropToMeasureCircle() {
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params["grabSource"] = QStringLiteral("Path");
        grab.params["filePath"] = QStringLiteral(TEST_BATCH4_DATA_Circle);
        project->addModule(grab);

        ModuleInstance crop;
        crop.id = QStringLiteral("crop");
        crop.moduleId = QStringLiteral("CropImage");
        QJsonArray rects;
        rects.append(rect(320, 240, 150, 150, 0)); // 半长 150 → 裁剪 300×300
        crop.params["rectangles"] = rects;
        crop.params["outputFirstAsImage"] = true;
        project->addModule(crop);

        ModuleInstance mc;
        mc.id = QStringLiteral("mc");
        mc.moduleId = QStringLiteral("MeasureCircle");
        mc.params["initialCenterX"] = 150.0; // 裁剪坐标系：320-170
        mc.params["initialCenterY"] = 150.0; // 240-90
        mc.params["initialRadius"] = 100.0;
        mc.params["threshold"] = 20.0;
        mc.params["measureCount"] = 36;
        mc.params["searchLength"] = 30.0;
        project->addModule(mc);

        ModuleConnection c1;
        c1.fromModuleId = QStringLiteral("grab");
        c1.toModuleId = QStringLiteral("crop");
        c1.fromPort = QStringLiteral("image");
        c1.toPort = QStringLiteral("image");
        c1.edgeType = QStringLiteral("data");
        project->addConnection(c1);
        ModuleConnection c2;
        c2.fromModuleId = QStringLiteral("crop");
        c2.toModuleId = QStringLiteral("mc");
        c2.fromPort = QStringLiteral("image");
        c2.toPort = QStringLiteral("image");
        c2.edgeType = QStringLiteral("data");
        project->addConnection(c2);

        QVERIFY(engine.loadProject(project));
        engine.runOnce();

        const ImageData cropOut = engine.moduleOutput(QStringLiteral("crop"));
        QCOMPARE(cropOut.data("crop_count").toDouble(), 1.0);
        QCOMPARE(cropOut.toMat().cols, 300);
        QCOMPARE(cropOut.toMat().rows, 300);

        const ImageData mcOut = engine.moduleOutput(QStringLiteral("mc"));
        QVERIFY2(mcOut.hasData("circle_radius"), "cropped image must drive circle measurement");
        QVERIFY2(std::abs(mcOut.data("circle_radius").toDouble() - 100) < 5.0, "radius ~100 in crop frame");
        QVERIFY2(std::abs(mcOut.data("circle_center_x").toDouble() - 150) < 5.0, "center x ~150 in crop frame");
        QVERIFY2(std::abs(mcOut.data("circle_center_y").toDouble() - 150) < 5.0, "center y ~150 in crop frame");
    }

    // 流程验收：MeasurementInput(point_line) → CalculateOffset(current_point)——
    // 跨名端口送达 Point3D 载荷并覆盖参数坐标（offset = target − 端口点）
    void testFlowMeasurementInputToCalculateOffset() {
        ToolSchema::instance().registerDefaultTools();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project != nullptr);

        ModuleInstance mi;
        mi.id = QStringLiteral("mi");
        mi.moduleId = QStringLiteral("MeasurementInput");
        mi.params["mode"] = QStringLiteral("point_line");
        mi.params["point"] = QJsonArray{10.0, 20.0, 0.0};
        mi.params["line"] = QJsonArray{0.0, 0.0, 100.0, 0.0};
        project->addModule(mi);

        ModuleInstance co;
        co.id = QStringLiteral("co");
        co.moduleId = QStringLiteral("CalculateOffset");
        co.params["currentX"] = 999.0; // 故意错误：必须被端口覆盖
        co.params["currentY"] = 999.0;
        co.params["currentAngle"] = 30.0;
        co.params["targetX"] = 35.0;
        co.params["targetY"] = 55.0;
        co.params["targetAngle"] = 40.0;
        project->addModule(co);

        ModuleConnection conn;
        conn.fromModuleId = QStringLiteral("mi");
        conn.toModuleId = QStringLiteral("co");
        conn.fromPort = QStringLiteral("point");
        conn.toPort = QStringLiteral("current_point");
        conn.edgeType = QStringLiteral("data");
        project->addConnection(conn);

        QVERIFY(engine.loadProject(project));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QVERIFY(!runId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY2(engine.runObservation(runId).result.success,
                 qPrintable(engine.runObservation(runId).result.errorMessage));

        const ImageData out = engine.moduleOutput(QStringLiteral("co"));
        QVERIFY2(out.hasData("offset_x"), "flow must produce offset_x");
        QCOMPARE(out.data("offset_x").toDouble(), -25.0); // 10-35（端口值生效，实测−参考）
        QCOMPARE(out.data("offset_y").toDouble(), -35.0); // 20-55
        QCOMPARE(out.data("offset_a").toDouble(), -10.0); // 30-40

        AgentActor actor;
        const QMap<QString, double> expected{{QStringLiteral("offset_x"), out.data("offset_x").toDouble()},
                                             {QStringLiteral("offset_y"), out.data("offset_y").toDouble()},
                                             {QStringLiteral("offset_a"), out.data("offset_a").toDouble()}};
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            for (bool byPort : {true, false}) {
                QJsonObject args{{QStringLiteral("runId"), runId},
                                 {QStringLiteral("instanceId"), QStringLiteral("co")}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), it.key());
                const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QCOMPARE(response.value(QStringLiteral("value")).toDouble(), it.value());
                if (it.key() == QLatin1String("offset_a")) {
                    QCOMPARE(response.value(QStringLiteral("unit")).toString(), QStringLiteral("deg"));
                } else {
                    QVERIFY(!response.contains(QStringLiteral("unit")));
                    QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
                    QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
                }
            }
        }
    }
};

QTEST_MAIN(TestBatch4)
#include "test_batch4.moc"
