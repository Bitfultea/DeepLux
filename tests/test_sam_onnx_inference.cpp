// 阶段2 测试：原生 SAM ONNX 推理核心——无 Python 真实模型流程、缓存失效、失败关闭
#include "core/inference/SamOnnxInference.h"

#include <QImage>
#include <QTest>
#include <opencv2/imgproc.hpp>

using namespace DeepLux;

class TestSamOnnxInference : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        m_packageDir = QStringLiteral(TEST_SAM_PACKAGE_DIR);
        if (!QFile::exists(m_packageDir + "/manifest.json"))
            QSKIP("模型包不存在，跳过（需 ~/Data/Models/sam1_vit_b_onnx/）");
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

private:
    QString m_packageDir;
};

QTEST_MAIN(TestSamOnnxInference)
#include "test_sam_onnx_inference.moc"
