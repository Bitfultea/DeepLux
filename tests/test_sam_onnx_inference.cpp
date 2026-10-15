// 阶段2 测试：原生 SAM ONNX 推理核心——无 Python 真实模型流程、缓存失效、失败关闭
#include "core/agent/AgentActor.h"
#include "core/agent/SamPredictor.h"
#include "core/agent/ToolSchema.h"
#include "core/common/CancellationToken.h"
#include "core/deeplux/DataContract.h"
#include "core/inference/SamOnnxInference.h"
#include "core/inference/SamPillowResize.h"
#include "core/model/AnnotationMaskOps.h"
#include "core/model/ImageData.h"
#include "plugins/detection/SamPredict/SamPredictPlugin.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

using namespace DeepLux;

class TestSamOnnxInference : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        m_packageDir = QStringLiteral(TEST_SAM_PACKAGE_DIR);
        QVERIFY2(QFile::exists(m_packageDir + "/manifest.json"),
                 qPrintable(QString("缺少 SAM 模型包：%1").arg(m_packageDir)));
        qputenv("DEEPLUX_SAM_PACKAGE", QFile::encodeName(m_packageDir));
    }

    void loadAndPredict() {
        SamOnnxInference inf;
        QVERIFY2(inf.loadPackage(m_packageDir), qPrintable(inf.lastError()));
        QVERIFY(inf.isLoaded());

        // 合成图 640×480（非方形，覆盖去填充裁剪路径）
        cv::Mat img(480, 640, CV_8UC3, cv::Scalar(96, 110, 100));
        // 画一个矩形（给点 prompt 一个可分割目标）
        cv::rectangle(img, cv::Rect(200, 150, 80, 60), cv::Scalar(255, 255, 255), -1);

        QList<QPointF> pos = {QPointF(240, 180)};
        SamNativeResult r = inf.predict(img, pos, {}, QRectF());
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.candidates.size(), 3);
        QVERIFY(!r.candidates[0].mask.empty());
        QCOMPARE(r.candidates[0].mask.rows, 480);
        QCOMPARE(r.candidates[0].mask.cols, 640);
        QVERIFY(r.candidates[0].score != 0);
        // 降序
        QVERIFY(r.candidates[0].score >= r.candidates[1].score);
        QVERIFY(r.candidates[1].score >= r.candidates[2].score);
        // 掩膜面积 >0
        QVERIFY(cv::countNonZero(r.candidates[0].mask > 0) > 0);
    }

    void optionalEmptyCandidateDoesNotDiscardValidSelection() {
        cv::Mat valid(32, 32, CV_8UC1, cv::Scalar(0));
        cv::rectangle(valid, cv::Rect(8, 8, 12, 12), cv::Scalar(255), -1);
        cv::Mat alternate(32, 32, CV_8UC1, cv::Scalar(0));
        cv::rectangle(alternate, cv::Rect(12, 12, 10, 10), cv::Scalar(255), -1);
        SamNativeResult prediction;
        prediction.ok = true;
        prediction.candidates = {{valid, {}, 0.9}, {cv::Mat::zeros(32, 32, CV_8UC1), {}, 0.8}, {alternate, {}, 0.7}};
        SamPromptItem item;
        item.label = QStringLiteral("defect");
        SamPredictOutcome converted =
            SamPredictorDetail::convertNativeCandidates(prediction, item, QSize(32, 32), true);
        QVERIFY2(converted.ok, qPrintable(converted.error));
        QCOMPARE(converted.regions.size(), 1);
        QCOMPARE(converted.previewCandidates.first().size(), 2);
        QCOMPARE(converted.candidateScores.first().size(), 3);
        item.candidateIndex = 1;
        converted = SamPredictorDetail::convertNativeCandidates(prediction, item, QSize(32, 32), true);
        QVERIFY(!converted.ok);
        QVERIFY(converted.error.contains(QStringLiteral("空掩膜")));
    }

    void embeddingCache() {
        SamOnnxInference inf;
        QVERIFY(inf.loadPackage(m_packageDir));

        cv::Mat img(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
        cv::rectangle(img, cv::Rect(100, 100, 100, 100), cv::Scalar(255, 255, 255), -1);
        QList<QPointF> pos = {QPointF(150, 150)};

        SamNativeResult r1 = inf.predict(img, pos, {}, QRectF());
        QVERIFY2(r1.ok, qPrintable(r1.error));
        QVERIFY(!r1.cacheHit); // 首次：编码

        SamNativeResult r2 = inf.predict(img, pos, {}, QRectF());
        QVERIFY2(r2.ok, qPrintable(r2.error));
        QVERIFY(r2.cacheHit); // 同图：复用缓存

        // 换图：缓存失效
        cv::Mat img2(480, 640, CV_8UC3, cv::Scalar(200, 200, 200));
        cv::rectangle(img2, cv::Rect(300, 300, 50, 50), cv::Scalar(0, 0, 0), -1);
        SamNativeResult r3 = inf.predict(img2, pos, {}, QRectF());
        QVERIFY2(r3.ok, qPrintable(r3.error));
        QVERIFY(!r3.cacheHit); // 换图：重新编码
    }

    void failClosedBadPackage() {
        SamOnnxInference inf;
        QVERIFY(!inf.loadPackage("/nonexistent/package"));
        QVERIFY(!inf.lastError().isEmpty());
    }

    void failClosedNoPrompt() {
        SamOnnxInference inf;
        QVERIFY(inf.loadPackage(m_packageDir));
        cv::Mat img(100, 100, CV_8UC3, cv::Scalar(0));
        SamNativeResult r = inf.predict(img, {}, {}, QRectF());
        QVERIFY(!r.ok);
        QVERIFY(r.error.contains("prompt"));
    }

    void boxPrompt() {
        SamOnnxInference inf;
        QVERIFY(inf.loadPackage(m_packageDir));
        cv::Mat img(480, 640, CV_8UC3, cv::Scalar(50, 50, 50));
        cv::rectangle(img, cv::Rect(100, 100, 200, 200), cv::Scalar(255, 255, 255), -1);
        SamNativeResult r = inf.predict(img, {}, {}, QRectF(100, 100, 200, 200));
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.candidates.size(), 3);
        QVERIFY(cv::countNonZero(r.candidates[0].mask > 0) > 0);
    }

    // PyTorch SamPredictor, sam_vit_b_01ec64.pth (SHA256 ec2df627...1eb8c912).
    // 480x640 RGB(96,110,100), white rectangle [200,150,80,60]; PNGs sorted by score.
    void equivalenceWithPyTorch() {
        SamOnnxInference inf;
        QVERIFY2(inf.loadPackage(m_packageDir), qPrintable(inf.lastError()));
        cv::Mat img(480, 640, CV_8UC3, cv::Scalar(96, 110, 100));
        cv::rectangle(img, cv::Rect(200, 150, 80, 60), cv::Scalar(255, 255, 255), -1);
        struct Case {
            const char* name;
            QList<QPointF> positive;
            QList<QPointF> negative;
            QRectF box;
            std::array<double, 3> scores;
        };
        const Case cases[] = {
            {"point", {QPointF(240, 180)}, {}, {}, {1.002875328, 0.993395805, 0.657050192}},
            {"posneg", {QPointF(240, 180)}, {QPointF(320, 250)}, {}, {1.008125782, 1.001309633, 0.673343003}},
            {"box", {}, {}, QRectF(100, 100, 200, 200), {0.939850688, 0.929948449, 0.929257810}},
            {"mixed", {QPointF(240, 180)}, {}, QRectF(100, 100, 200, 200), {0.968480348, 0.967603385, 0.966671169}},
        };
        for (const Case& testCase : cases) {
            SamNativeResult r = inf.predict(img, testCase.positive, testCase.negative, testCase.box);
            QVERIFY2(r.ok, qPrintable(QString("%1: %2").arg(testCase.name, r.error)));
            QCOMPARE(r.candidates.size(), 3);
            std::array<cv::Mat, 3> goldenMasks;
            for (int j = 0; j < 3; ++j) {
                const QString path =
                    QStringLiteral(TEST_SAM_GOLDEN_DIR) + "/" + testCase.name + "_" + QString::number(j) + ".png";
                goldenMasks[j] = cv::imread(QFile::encodeName(path).constData(), cv::IMREAD_GRAYSCALE);
                QVERIFY2(!goldenMasks[j].empty(), qPrintable(path));
            }
            for (int i = 0; i < 3; ++i) {
                const auto& candidate = r.candidates[i];
                QVERIFY2(std::abs(candidate.score - testCase.scores[i]) <= 0.01, testCase.name);
                cv::Mat intersection, united;
                cv::bitwise_and(candidate.mask, goldenMasks[i], intersection);
                cv::bitwise_or(candidate.mask, goldenMasks[i], united);
                const int unionPixels = cv::countNonZero(united);
                QVERIFY2(unionPixels && double(cv::countNonZero(intersection)) / unionPixels >= 0.99, testCase.name);
                const cv::Rect bbox = cv::boundingRect(goldenMasks[i]);
                const QRectF& actual = candidate.bbox;
                QVERIFY2(std::abs(actual.x() - bbox.x) <= 1 && std::abs(actual.y() - bbox.y) <= 1 &&
                             std::abs(actual.width() - bbox.width) <= 1 && std::abs(actual.height() - bbox.height) <= 1,
                         testCase.name);
            }
        }
    }

    // Synthetic image matches stage1 make_small_defect_image; references are PyTorch SamPredictor ViT-B outputs.
    void smallDefectPillowResizeParity() {
        SamOnnxInference inf;
        QVERIFY2(inf.loadPackage(m_packageDir), qPrintable(inf.lastError()));
        cv::Mat image(720, 1280, CV_8UC3);
        for (int y = 0; y < image.rows; ++y) {
            auto* row = image.ptr<cv::Vec3b>(y);
            for (int x = 0; x < image.cols; ++x) {
                const uchar base = uchar(((x * 3 + y * 5) / 16) % 48 + 96);
                row[x] = cv::Vec3b(base, base, base);
            }
        }
        for (int k = 0; k < 6; ++k) {
            const int cx = 120 + k * 190;
            const int cy = 110 + (k * 137) % 500;
            const int radius = 3 + (k * 7) % 8;
            const uchar value = k % 2 == 0 ? 30 : 225;
            for (int y = cy - radius; y <= cy + radius; ++y)
                for (int x = cx - radius; x <= cx + radius; ++x)
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius)
                        image.at<cv::Vec3b>(y, x) = cv::Vec3b(value, value, value);
        }
        const SamNativeResult result = inf.predict(image, {QPointF(120, 110)}, {});
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.candidates.size(), 3);
        const std::array<double, 3> pytorchScores{0.9049928, 0.9021003, 0.4983702};
        const std::array<int, 3> pytorchAreas{43, 45, 53916};
        for (int i = 0; i < 3; ++i) {
            QVERIFY(std::abs(result.candidates[i].score - pytorchScores[i]) < 0.001);
            QVERIFY(std::abs(cv::countNonZero(result.candidates[i].mask) - pytorchAreas[i]) <= 2);
        }
        QVERIFY(std::abs(result.candidates[2].bbox.width() - 701) <= 1);
        QVERIFY(std::abs(result.candidates[2].bbox.height() - 295) <= 1);
    }

    void portraitResizeMatchesPillowPixels() {
        cv::Mat image(1200, 480, CV_8UC3);
        for (int y = 0; y < image.rows; ++y) {
            auto* row = image.ptr<cv::Vec3b>(y);
            for (int x = 0; x < image.cols; ++x)
                row[x] = cv::Vec3b((x * 17 + y * 13) % 256, (x * 7 + y * 29 + 53) % 256, (x * 31 + y * 3 + 101) % 256);
        }
        const cv::Mat resized = SamNativeDetail::resizePillowBilinear(image, cv::Size(410, 1024));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        for (int y = 0; y < resized.rows; ++y)
            hash.addData(reinterpret_cast<const char*>(resized.ptr(y)), resized.cols * resized.elemSize());
        QCOMPARE(hash.result().toHex(), QByteArray("3cf48675ec7b43fc4ca7a35e09682782ca9b24335041b1559443dce276e4c44d"));
    }

    void nativeProductFlow() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        cv::Mat rgb(480, 640, CV_8UC3, cv::Scalar(96, 110, 100));
        cv::rectangle(rgb, cv::Rect(200, 150, 80, 60), cv::Scalar(255, 255, 255), -1);
        cv::Mat bgr;
        cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
        const QString imagePath = dir.path() + "/prompt.png";
        QVERIFY(cv::imwrite(QFile::encodeName(imagePath).constData(), bgr));

        SamPromptItem item;
        item.positivePoints = {QPointF(240, 180)};
        item.label = "defect";
        bool guiEventProcessed = false;
        QTimer::singleShot(0, this, [&] { guiEventProcessed = true; });
        const SamPredictOutcome result = runSamPredictBatch("native", imagePath, {item}, 30000);
        QVERIFY(guiEventProcessed); // 主线程等待推理期间仍处理 GUI 事件
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.regions.size(), 1);
        QCOMPARE(result.candidateScores.size(), 1);
        QCOMPARE(result.candidateScores[0].size(), 3);
        QCOMPARE(result.regions[0].label, QString("defect"));
        QVERIFY(result.candidateScores[0][0] > 1.0); // 保留原始模型分数用于对照和候选排序
        QCOMPARE(result.regions[0].score, 1.0);      // 仅 Region2D 按载荷契约归一
        QVERIFY(result.regions[0].score >= 0.0 && result.regions[0].score <= 1.0);
        QVERIFY(result.regions[0].bbox.isValid());
        bool decoded = false;
        const QImage mask = MaskOps::decodeRle(result.regions[0].maskRle, QSize(640, 480), &decoded);
        QVERIFY(decoded);
        QVERIFY(MaskOps::foregroundCount(mask) > 0);

        ToolSchema::instance().registerDefaultTools();
        AgentActor actor;
        const QJsonObject toolResult =
            actor.executeTool("sam_predict", QJsonObject{{"imagePath", imagePath},
                                                         {"serverUrl", "native"},
                                                         {"points", QJsonArray{QJsonArray{240, 180}}},
                                                         {"label", "defect"},
                                                         {"timeoutMs", 30000}});
        QVERIFY2(!toolResult.contains("error"), qPrintable(toolResult.value("error").toString()));
        QCOMPARE(toolResult.value("status").toString(), QString("predicted"));
        QVERIFY(toolResult.value("maskRleLength").toInt() > 0);

        SamPredictPlugin plugin;
        QVERIFY(plugin.initialize());
        QJsonObject params = plugin.defaultParams();
        params.insert("serverUrl", "native");
        params.insert("label", "defect");
        params.insert("points", QJsonArray{QJsonArray{240, 180}});
        params.insert("timeoutMs", 30000);
        QString paramError;
        QVERIFY2(plugin.validateParams(params, paramError), qPrintable(paramError));
        plugin.setParams(params);
        QCOMPARE(plugin.currentParams().value("serverUrl").toString(), QString("native"));
        ImageData pluginOutput;
        QVERIFY(plugin.execute(ImageData(QImage(imagePath)), pluginOutput));
        const QVariant regionValue = pluginOutput.data("region");
        QVERIFY(portValueMatchesType(regionValue, DataType::Region2D));
        QCOMPARE(regionValue.value<Region2D>().label, QString("defect"));
        QVERIFY(!regionValue.value<Region2D>().maskRle.isEmpty());

        item.candidateIndex = 3;
        const SamPredictOutcome invalid = runSamPredictBatch("native", imagePath, {item}, 30000);
        QVERIFY(!invalid.ok);
        QCOMPARE(invalid.failedIndex, 0);
        QVERIFY(invalid.regions.isEmpty());
        QVERIFY(invalid.error.contains("candidateIndex"));

        item.candidateIndex = 1;
        const SamPredictOutcome alternative = runSamPredictBatch("native", imagePath, {item}, 30000);
        QVERIFY2(alternative.ok, qPrintable(alternative.error));
        QVERIFY(alternative.regions[0].maskRle != result.regions[0].maskRle);

        CancellationToken cancelled;
        cancelled.cancel();
        const SamPredictOutcome stopped = runSamPredictBatch("native", imagePath, {item}, 30000, &cancelled);
        QVERIFY(!stopped.ok);
        QVERIFY(stopped.error.contains("取消"));

        CancellationToken cancelDuringRun;
        bool cancelEventProcessed = false;
        QTimer::singleShot(0, this, [&] {
            cancelEventProcessed = true;
            cancelDuringRun.cancel();
        });
        const SamPredictOutcome interrupted = runSamPredictBatch("native", imagePath, {item}, 30000, &cancelDuringRun);
        QVERIFY(cancelEventProcessed);
        QVERIFY(!interrupted.ok);
        QVERIFY(interrupted.error.contains("取消"));

        qunsetenv("DEEPLUX_SAM_PACKAGE");
        const SamPredictOutcome unconfigured = runSamPredictBatch("native", imagePath, {item}, 30000);
        QVERIFY(!unconfigured.ok);
        QVERIFY(unconfigured.error.contains("DEEPLUX_SAM_PACKAGE"));
        qputenv("DEEPLUX_SAM_PACKAGE", QFile::encodeName(m_packageDir));
    }

    void persistentPoolSessionSurvivesShutdown() {
        static QThreadPool pool; // 先于工作线程中的 ORT 环境构造，且晚于它析构
        pool.setMaxThreadCount(1);
        pool.setExpiryTimeout(-1);
        QSemaphore done;
        bool loaded = false;
        QString error;
        pool.start([&] {
            thread_local SamOnnxInference inference;
            loaded = inference.loadPackage(m_packageDir);
            error = inference.lastError();
            done.release();
        });
        const bool completed = done.tryAcquire(1, 30000);
        if (!completed)
            pool.waitForDone(); // 超时也须结束引用栈变量的任务
        QVERIFY2(completed, "native model load timed out");
        QVERIFY2(loaded, qPrintable(error));
    }

    void rejectsManifestMismatch() {
        QFile source(m_packageDir + "/manifest.json");
        QVERIFY(source.open(QIODevice::ReadOnly));
        const QJsonObject original = QJsonDocument::fromJson(source.readAll()).object();
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int variant = 0; variant < 5; ++variant) {
            QJsonObject manifest = original;
            QString expectedError;
            if (variant == 0) {
                QJsonObject transform = manifest.value("coordinateTransform").toObject();
                transform["mode"] = "scale-arbitrary";
                manifest["coordinateTransform"] = transform;
                expectedError = "coordinateTransform.mode";
            } else {
                QJsonObject preprocessing = manifest.value("preprocessing").toObject();
                if (variant == 1) {
                    QJsonObject normalize = preprocessing.value("normalize").toObject();
                    normalize["mean"] = QJsonArray{0.0, 0.0, 0.0};
                    preprocessing["normalize"] = normalize;
                    expectedError = "preprocessing.normalize.mean";
                } else if (variant == 2) {
                    preprocessing["order"] = "pad before normalize";
                    expectedError = "preprocessing";
                } else if (variant == 3) {
                    QJsonObject resize = preprocessing.value("resize").toObject();
                    resize["targetSize"] = 1024.5;
                    preprocessing["resize"] = resize;
                    expectedError = "preprocessing.resize.targetSize";
                } else {
                    QJsonObject resize = preprocessing.value("resize").toObject();
                    resize["interpolation"] = "nearest";
                    preprocessing["resize"] = resize;
                    expectedError = "preprocessing.resize.interpolation";
                }
                manifest["preprocessing"] = preprocessing;
            }
            QFile out(dir.path() + "/manifest.json");
            QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
            out.write(QJsonDocument(manifest).toJson());
            out.close();
            SamOnnxInference inf;
            QVERIFY2(!inf.loadPackage(dir.path()), qPrintable(QString("variant %1").arg(variant)));
            QVERIFY2(inf.lastError().contains(expectedError), qPrintable(inf.lastError()));
        }
    }

    void rejectsWrongImageType() {
        SamOnnxInference inf;
        QVERIFY(inf.loadPackage(m_packageDir));
        const QList<QPointF> point = {QPointF(10, 10)};
        for (const cv::Mat& image : {cv::Mat(), cv::Mat(20, 20, CV_8UC1), cv::Mat(20, 20, CV_32FC3)}) {
            SamNativeResult result = inf.predict(image, point, {});
            QVERIFY(!result.ok);
            QVERIFY(result.error.contains("RGB"));
        }
    }

    void rejectsNonFinitePrompt() {
        SamOnnxInference inf;
        QVERIFY(inf.loadPackage(m_packageDir));
        cv::Mat image(20, 20, CV_8UC3, cv::Scalar(0, 0, 0));
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const double infValue = std::numeric_limits<double>::infinity();
        for (const auto& point : {QPointF(nan, 10), QPointF(10, infValue)}) {
            SamNativeResult result = inf.predict(image, {point}, {});
            QVERIFY(!result.ok);
            QVERIFY(result.error.contains("有限"));
        }
        SamNativeResult result = inf.predict(image, {QPointF(10, 10)}, {}, QRectF(infValue, 0, 10, 10));
        QVERIFY(!result.ok);
        QVERIFY(result.error.contains("有限"));
    }

private:
    QString m_packageDir;
};

QTEST_MAIN(TestSamOnnxInference)
#include "test_sam_onnx_inference.moc"
