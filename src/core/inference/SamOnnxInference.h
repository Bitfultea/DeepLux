#pragma once

// SAM1 原生 ONNX 推理核心（阶段2）：
// 使用 onnxruntime C API（C++ wrapper 的 GetShape 在 -1 动态维下崩溃，
// C API 正确处理 int64_t -1）+ OpenCV 预处理/后处理。
// 模型专属逻辑不进入 UI；不创建空的 SAM2/3 实现类。

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <opencv2/core.hpp>

namespace DeepLux {

struct SamNativeCandidate {
    cv::Mat mask;     // 二值掩膜（CV_8U, 0/255），原图尺寸
    QRectF bbox;      // 由掩膜导出（sam_server _mask_to_bbox 口径）
    double score = 0; // 原始 IoU（未 clamp，可 >1；F1 口径）
};

struct SamNativeResult {
    QList<SamNativeCandidate> candidates; // 按 score 降序
    QString modelName;
    bool ok = false;
    QString error;
    bool cancelled = false;
    bool cacheHit = false; // 本次预测是否复用了缓存的 embedding
};

class SamOnnxInference : public QObject {
    Q_OBJECT
public:
    explicit SamOnnxInference(QObject* parent = nullptr);
    ~SamOnnxInference();

    // 加载模型包（manifest.json + encoder.onnx + decoder.onnx）
    bool loadPackage(const QString& packageDir);
    bool isLoaded() const;
    QString lastError() const;
    QString packageDir() const;

    // 预测：给图像 + prompt，返回候选列表
    // 同步执行（调用方应在工作线程调用）
    SamNativeResult predict(const cv::Mat& image, const QList<QPointF>& positivePoints,
                            const QList<QPointF>& negativePoints, const QRectF& box = QRectF());

    // 取消（线程安全标志，在操作间检查）
    void cancel();
    bool isCancelled() const;

    // embedding 缓存信息
    bool hasCachedEmbedding() const;
    QString cachedImageHash() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace DeepLux
