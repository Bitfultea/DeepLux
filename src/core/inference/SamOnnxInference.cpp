#include "SamOnnxInference.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <cmath>
#include <cstring>
#include <onnxruntime_c_api.h>
#include <opencv2/imgproc.hpp>

namespace DeepLux {

// ===== 常量（安装版 segment_anything 口径，阶段1 已对照验证） =====
static constexpr int TARGET_SIZE = 1024;
static const double PIXEL_MEAN[3] = {123.675, 116.28, 103.53};
static const double PIXEL_STD[3] = {58.395, 57.12, 57.375};

// ===== ORT C API RAII =====
namespace {
const OrtApi* g_ort = nullptr;

struct OrtEnvHolder {
    OrtEnv* env = nullptr;
    OrtEnvHolder() {
        if (!g_ort)
            g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
        g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "DeepLux_SamNative", &env);
    }
    ~OrtEnvHolder() {
        if (env)
            g_ort->ReleaseEnv(env);
    }
};

// 全局单例（线程安全的静态初始化）
OrtEnvHolder& ortEnv() {
    static OrtEnvHolder h;
    return h;
}

#define ORT_CK(expr)                                                                                                   \
    do {                                                                                                               \
        OrtStatus* _st = (expr);                                                                                       \
        if (_st) {                                                                                                     \
            const char* _msg = g_ort->GetErrorMessage(_st);                                                            \
            g_ort->ReleaseStatus(_st);                                                                                 \
            throw std::runtime_error(_msg);                                                                            \
        }                                                                                                              \
    } while (0)

struct OrtSessionRAII {
    OrtSession* sess = nullptr;
    OrtAllocator* allocator = nullptr; // 默认分配器（单例，不可 Release）
    ~OrtSessionRAII() {
        // allocator 是默认单例，不释放（仅 Release Session）
        if (sess)
            g_ort->ReleaseSession(sess);
    }
};

// 创建 OrtValue 张量（零拷贝：用外部 buffer）
struct OrtValueRAII {
    OrtValue* val = nullptr;
    ~OrtValueRAII() {
        if (val)
            g_ort->ReleaseValue(val);
    }
};

// 预处理形状（安装版 ResizeLongestSide.get_preprocess_shape：int(old*scale+0.5)，无 round）
QSize getPreprocessShape(int h, int w, int target = TARGET_SIZE) {
    double scale = target * 1.0 / std::max(h, w);
    return {int(w * scale + 0.5), int(h * scale + 0.5)}; // (width, height)
}

// 图像哈希（下采样 128² → sha256，快速检测换图）
QString computeImageHash(const cv::Mat& image) {
    cv::Mat small;
    cv::resize(image, small, cv::Size(128, 128), 0, 0, cv::INTER_AREA);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(reinterpret_cast<const char*>(small.data), small.total() * small.elemSize());
    return QString::fromLatin1(hash.result().toHex());
}

// 由二值掩膜导出 bbox（sam_server._mask_to_bbox 口径：[x1,y1,w,h]，w=x2-x1+1）
QRectF maskToBbox(const cv::Mat& mask) {
    cv::Mat idx;
    cv::findNonZero(mask > 0, idx);
    if (idx.empty())
        return QRectF();
    cv::Rect bb = cv::boundingRect(idx);
    return QRectF(bb.x, bb.y, bb.width - 1 + 1.0, bb.height - 1 + 1.0);
    // 注：sam_server 用 xs.max()-xs.min()+1，等价于 boundingRect.width
}

// 坐标变换（安装版 apply_coords 口径：纯缩放）
QPointF scalePoint(const QPointF& pt, int origH, int origW, const QSize& newSize) {
    double sx = newSize.width() / double(origW);
    double sy = newSize.height() / double(origH);
    return {pt.x() * sx, pt.y() * sy};
}

// 框变换（安装版 apply_boxes 口径：reshape(-1,2,2) 全四分量缩放）
QRectF scaleBox(const QRectF& box, int origH, int origW, const QSize& newSize) {
    double sx = newSize.width() / double(origW);
    double sy = newSize.height() / double(origH);
    // xyxy → xyxy
    double x1 = box.x() * sx, y1 = box.y() * sy;
    double x2 = (box.x() + box.width()) * sx, y2 = (box.y() + box.height()) * sy;
    return {x1, y1, x2 - x1, y2 - y1}; // 返回 xywh（调用方需要 xyxy 时自行转换）
}

} // anonymous namespace

// ===== Impl =====
struct SamOnnxInference::Impl {
    // manifest
    QJsonObject manifest;
    QString packageDir;
    QString lastError;
    bool loaded = false;

    // ORT sessions
    OrtSessionRAII encoder;
    OrtSessionRAII decoder;

    // embedding cache
    cv::Mat cachedEmbedding; // [1,256,64,64] float32
    QString cachedImageHash;
    QSize cachedInputSize; // (new_w, new_h) 即 resize 后、pad 前的尺寸

    // cancel
    std::atomic<bool> cancelled{false};

    // 预处理：RGB float32 → resize → pad → normalize → NCHW blob
    cv::Mat preprocess(const cv::Mat& image, QSize& outInputSize) {
        cv::Mat rgb;
        if (image.channels() == 1)
            cv::cvtColor(image, rgb, cv::COLOR_GRAY2RGB);
        else if (image.channels() == 4)
            cv::cvtColor(image, rgb, cv::COLOR_BGRA2RGB);
        else
            rgb = image;

        int h = rgb.rows, w = rgb.cols;
        QSize newSize = getPreprocessShape(h, w);
        outInputSize = newSize;

        cv::Mat resized;
        cv::resize(rgb, resized, cv::Size(newSize.width(), newSize.height()), 0, 0, cv::INTER_LINEAR);

        // pad to 1024²
        cv::Mat padded = cv::Mat::zeros(TARGET_SIZE, TARGET_SIZE, CV_32FC3);
        cv::Mat resizedf;
        resized.convertTo(resizedf, CV_32FC3);
        resizedf.copyTo(padded(cv::Rect(0, 0, newSize.width(), newSize.height())));

        // normalize: (x - mean) / std（per-channel，先归一化后填充已在上一步完成）
        cv::Mat chs[3];
        cv::split(padded, chs);
        for (int c = 0; c < 3; ++c)
            chs[c] = (chs[c] - PIXEL_MEAN[c]) / PIXEL_STD[c];

        // HWC → NCHW：三通道扁平化拼接为 [3*1024*1024] float32
        cv::Mat blob(1, 3 * TARGET_SIZE * TARGET_SIZE, CV_32F);
        float* dst = (float*) blob.data;
        for (int c = 0; c < 3; ++c) {
            cv::Mat flat = chs[c].reshape(1, 1); // [1, 1024*1024]
            std::memcpy(dst + c * TARGET_SIZE * TARGET_SIZE, flat.data, TARGET_SIZE * TARGET_SIZE * sizeof(float));
        }
        return blob; // [1, 3*1024*1024] 连续 float32，ORT 用 shape [1,3,1024,1024] 解释
    }

    // 编码器前向
    cv::Mat runEncoder(const cv::Mat& inputBlob) {
        const char* inNames[1];
        const char* outNames[1];
        char* inName;
        ORT_CK(g_ort->SessionGetInputName(encoder.sess, 0, encoder.allocator, &inName));
        inNames[0] = inName;
        char* outName;
        ORT_CK(g_ort->SessionGetOutputName(encoder.sess, 0, encoder.allocator, &outName));
        outNames[0] = outName;

        OrtMemoryInfo* memInfo = nullptr;
        ORT_CK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo));

        int64_t shape[] = {1, 3, TARGET_SIZE, TARGET_SIZE};
        OrtValueRAII inputTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo, (void*) inputBlob.data,
                                                     inputBlob.total() * inputBlob.elemSize(), shape, 4,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor.val));
        g_ort->ReleaseMemoryInfo(memInfo);

        OrtValue* outputs[1] = {nullptr};
        ORT_CK(g_ort->Run(encoder.sess, nullptr, inNames, &inputTensor.val, 1, outNames, 1, outputs));

        // 提取输出
        void* outData;
        ORT_CK(g_ort->GetTensorMutableData(outputs[0], &outData));
        cv::Mat embedding(1, 256 * 64 * 64, CV_32F, outData);
        cv::Mat copy = embedding.clone();
        g_ort->ReleaseValue(outputs[0]);
        g_ort->AllocatorFree(encoder.allocator, inName);
        g_ort->AllocatorFree(encoder.allocator, outName);
        return copy.reshape(1, {1, 256, 64, 64});
    }

    // 解码器前向
    // 返回 (low_res_masks [1,3,256,256] float32, iou [1,3] float32)
    std::pair<cv::Mat, cv::Mat> runDecoder(const cv::Mat& embedding, const QList<QPointF>& scaledPos,
                                           const QList<QPointF>& scaledNeg, const QRectF& scaledBoxXYXY, int origH,
                                           int origW) {
        // 构造输入
        int nPts = scaledPos.size() + scaledNeg.size();
        int nBox = scaledBoxXYXY.isValid() ? 1 : 0;

        // point_coords [N,2] float32
        std::vector<float> coordsData(std::max(nPts, 1) * 2, 0);
        int idx = 0;
        for (const auto& p : scaledPos) {
            coordsData[idx * 2] = p.x();
            coordsData[idx * 2 + 1] = p.y();
            ++idx;
        }
        for (const auto& p : scaledNeg) {
            coordsData[idx * 2] = p.x();
            coordsData[idx * 2 + 1] = p.y();
            ++idx;
        }

        // point_labels [N] int64
        std::vector<int64_t> labelsData(std::max(nPts, 1), 0);
        idx = 0;
        for (int i = 0; i < scaledPos.size(); ++i)
            labelsData[idx++] = 1;
        for (int i = 0; i < scaledNeg.size(); ++i)
            labelsData[idx++] = 0;

        // boxes [M,4] float32
        std::vector<float> boxesData(std::max(nBox, 1) * 4, 0);
        if (nBox) {
            boxesData[0] = scaledBoxXYXY.x();
            boxesData[1] = scaledBoxXYXY.y();
            boxesData[2] = scaledBoxXYXY.x() + scaledBoxXYXY.width();
            boxesData[3] = scaledBoxXYXY.y() + scaledBoxXYXY.height();
        }

        // 如果没有点也没有框 → 返回错误
        if (nPts == 0 && nBox == 0)
            throw std::runtime_error("至少需要一个 prompt（点或框）");

        // 获取 decoder 的输入/输出名称
        char* names[4];
        const char* inNames[4];
        for (size_t i = 0; i < 4; ++i) {
            ORT_CK(g_ort->SessionGetInputName(decoder.sess, i, decoder.allocator, &names[i]));
            inNames[i] = names[i];
        }
        size_t nOut = 2;
        char* outNamesC[2];
        const char* outNames[2];
        for (size_t i = 0; i < nOut; ++i) {
            ORT_CK(g_ort->SessionGetOutputName(decoder.sess, i, decoder.allocator, &outNamesC[i]));
            outNames[i] = outNamesC[i];
        }

        OrtMemoryInfo* memInfo = nullptr;
        ORT_CK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo));

        // image_embeddings: [1,256,64,64] float32（零拷贝）
        int64_t embShape[] = {1, 256, 64, 64};
        OrtValueRAII embTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo, (void*) embedding.data,
                                                     embedding.total() * embedding.elemSize(), embShape, 4,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &embTensor.val));

        // point_coords: [N,2] float32
        int64_t coordsShape[] = {std::max(nPts, 1), 2};
        OrtValueRAII coordsTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo, coordsData.data(), coordsData.size() * sizeof(float),
                                                     coordsShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                     &coordsTensor.val));

        // point_labels: [N] int64
        int64_t labelsShape[] = {std::max(nPts, 1)};
        OrtValueRAII labelsTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo, labelsData.data(), labelsData.size() * sizeof(int64_t),
                                                     labelsShape, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
                                                     &labelsTensor.val));

        // boxes: [M,4] float32
        int64_t boxesShape[] = {std::max(nBox, 1), 4};
        OrtValueRAII boxesTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo, boxesData.data(), boxesData.size() * sizeof(float),
                                                     boxesShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                     &boxesTensor.val));

        g_ort->ReleaseMemoryInfo(memInfo);

        const OrtValue* inputs[4] = {embTensor.val, coordsTensor.val, labelsTensor.val, boxesTensor.val};
        OrtValue* outputs[2] = {nullptr, nullptr};
        ORT_CK(g_ort->Run(decoder.sess, nullptr, inNames, inputs, 4, outNames, 2, outputs));

        // 提取 low_res_masks [1,3,256,256] 和 iou [1,3]
        void* maskData;
        ORT_CK(g_ort->GetTensorMutableData(outputs[0], &maskData));
        cv::Mat lowRes(1, 3 * 256 * 256, CV_32F, maskData);
        cv::Mat lowResCopy = lowRes.clone();

        void* iouData;
        ORT_CK(g_ort->GetTensorMutableData(outputs[1], &iouData));
        cv::Mat iou(1, 3, CV_32F, iouData);
        cv::Mat iouCopy = iou.clone();

        g_ort->ReleaseValue(outputs[0]);
        g_ort->ReleaseValue(outputs[1]);
        for (size_t i = 0; i < 4; ++i)
            g_ort->AllocatorFree(decoder.allocator, names[i]);
        for (size_t i = 0; i < 2; ++i)
            g_ort->AllocatorFree(decoder.allocator, outNamesC[i]);

        return {lowResCopy.reshape(1, {1, 3, 256, 256}), iouCopy.reshape(1, {1, 3})};
    }

    // 后处理：安装版 Sam.postprocess_masks 同口径
    // 256→1024² → 裁剪去填充 → 原图 → 阈值 >0
    cv::Mat postprocess(const cv::Mat& lowResCandidate, const QSize& inputSize, int origH, int origW) {
        // lowResCandidate: [256,256] float32 (single candidate)
        // 1) 256→1024² bilinear
        cv::Mat up1024;
        cv::resize(lowResCandidate, up1024, cv::Size(TARGET_SIZE, TARGET_SIZE), 0, 0, cv::INTER_LINEAR);
        // 2) 裁剪去填充
        cv::Mat cropped = up1024(cv::Rect(0, 0, inputSize.width(), inputSize.height()));
        // 3) 裁剪后 → 原图 bilinear
        cv::Mat fullSize;
        cv::resize(cropped, fullSize, cv::Size(origW, origH), 0, 0, cv::INTER_LINEAR);
        // 4) 阈值 >0
        cv::Mat binary;
        cv::threshold(fullSize, binary, 0, 255, cv::THRESH_BINARY);
        binary.convertTo(binary, CV_8U);
        return binary;
    }
};

// ===== 公共接口 =====
SamOnnxInference::SamOnnxInference(QObject* parent) : QObject(parent), m_impl(std::make_unique<Impl>()) {}
SamOnnxInference::~SamOnnxInference() = default;

bool SamOnnxInference::loadPackage(const QString& packageDir) {
    m_impl->loaded = false;
    m_impl->packageDir = packageDir;
    m_impl->lastError.clear();

    // 1. 加载 manifest
    QString manPath = packageDir + "/manifest.json";
    QFile f(manPath);
    if (!f.open(QIODevice::ReadOnly)) {
        m_impl->lastError = QStringLiteral("无法读取 manifest.json：%1").arg(manPath);
        return false;
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError) {
        m_impl->lastError = QStringLiteral("manifest.json 解析失败：%1").arg(pe.errorString());
        return false;
    }
    m_impl->manifest = doc.object();

    // 2. 校验 manifest 关键字段
    if (m_impl->manifest.value("formatVersion").toInt() != 1) {
        m_impl->lastError = QStringLiteral("不支持的模型包格式版本");
        return false;
    }
    if (m_impl->manifest.value("family").toString() != "sam1") {
        m_impl->lastError = QStringLiteral("不支持的模型家族（仅支持 sam1）");
        return false;
    }
    if (m_impl->manifest.value("variant").toString() != "vit_b") {
        m_impl->lastError = QStringLiteral("不支持的模型变体（仅支持 vit_b）");
        return false;
    }
    // 必需语义字段
    for (const char* field : {"preprocessing", "coordinateTransform", "semantics"}) {
        if (!m_impl->manifest.value(field).isObject() || m_impl->manifest.value(field).toObject().isEmpty()) {
            m_impl->lastError = QStringLiteral("manifest 缺少必需语义字段 %1").arg(field);
            return false;
        }
    }
    // files
    QJsonObject files = m_impl->manifest.value("files").toObject();
    QJsonObject encMeta = files.value("encoder").toObject();
    QJsonObject decMeta = files.value("decoder").toObject();
    QString encFile = encMeta.value("file").toString();
    QString decFile = decMeta.value("file").toString();
    if (encFile != QFileInfo(encFile).fileName() || decFile != QFileInfo(decFile).fileName()) {
        m_impl->lastError = QStringLiteral("files.*.file 必须为包内裸文件名（拒绝绝对路径/穿越）");
        return false;
    }

    // 3. SHA-256 校验
    auto checkSha256 = [](const QString& path, const QString& expected) -> bool {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&f);
        return QString::fromLatin1(hash.result().toHex()) == expected;
    };

    QString encPath = packageDir + "/" + encFile;
    QString decPath = packageDir + "/" + decFile;
    if (!checkSha256(encPath, encMeta.value("sha256").toString())) {
        m_impl->lastError = QStringLiteral("encoder SHA-256 不符");
        return false;
    }
    if (!checkSha256(decPath, decMeta.value("sha256").toString())) {
        m_impl->lastError = QStringLiteral("decoder SHA-256 不符");
        return false;
    }

    // 4. 加载 ORT sessions
    try {
        OrtEnvHolder& env = ortEnv();
        OrtSessionOptions* opts;
        ORT_CK(g_ort->CreateSessionOptions(&opts));
        g_ort->AddSessionConfigEntry(opts, "session.intra_op_num_threads", "4");
        ORT_CK(g_ort->CreateSession(env.env, encPath.toUtf8().constData(), opts, &m_impl->encoder.sess));
        ORT_CK(g_ort->CreateSession(env.env, decPath.toUtf8().constData(), opts, &m_impl->decoder.sess));
        g_ort->ReleaseSessionOptions(opts);
        ORT_CK(g_ort->GetAllocatorWithDefaultOptions(&m_impl->encoder.allocator));
        ORT_CK(g_ort->GetAllocatorWithDefaultOptions(&m_impl->decoder.allocator));
    } catch (const std::exception& e) {
        m_impl->lastError = QStringLiteral("ORT 加载失败：%1").arg(e.what());
        return false;
    }

    m_impl->loaded = true;
    m_impl->cachedImageHash.clear(); // 换模型清空缓存
    return true;
}

bool SamOnnxInference::isLoaded() const {
    return m_impl->loaded;
}
QString SamOnnxInference::lastError() const {
    return m_impl->lastError;
}
QString SamOnnxInference::packageDir() const {
    return m_impl->packageDir;
}
void SamOnnxInference::cancel() {
    m_impl->cancelled = true;
}
bool SamOnnxInference::isCancelled() const {
    return m_impl->cancelled;
}
bool SamOnnxInference::hasCachedEmbedding() const {
    return !m_impl->cachedEmbedding.empty();
}
QString SamOnnxInference::cachedImageHash() const {
    return m_impl->cachedImageHash;
}

SamNativeResult SamOnnxInference::predict(const cv::Mat& image, const QList<QPointF>& positivePoints,
                                          const QList<QPointF>& negativePoints, const QRectF& box) {
    SamNativeResult result;
    m_impl->cancelled = false;

    if (!m_impl->loaded) {
        result.error = QStringLiteral("模型包未加载");
        return result;
    }
    if (positivePoints.isEmpty() && negativePoints.isEmpty() && !box.isValid()) {
        result.error = QStringLiteral("至少需要一个 prompt（点或框）");
        return result;
    }

    int origH = image.rows, origW = image.cols;

    try {
        // 1. embedding 缓存
        QString imgHash = computeImageHash(image);
        cv::Mat embedding;
        QSize inputSize;

        if (imgHash == m_impl->cachedImageHash && !m_impl->cachedEmbedding.empty()) {
            embedding = m_impl->cachedEmbedding;
            inputSize = m_impl->cachedInputSize;
            result.cacheHit = true;
        } else {
            if (m_impl->cancelled) {
                result.cancelled = true;
                return result;
            }
            cv::Mat inputBlob = m_impl->preprocess(image, inputSize);
            if (m_impl->cancelled) {
                result.cancelled = true;
                return result;
            }
            embedding = m_impl->runEncoder(inputBlob);
            m_impl->cachedEmbedding = embedding;
            m_impl->cachedImageHash = imgHash;
            m_impl->cachedInputSize = inputSize;
        }

        if (m_impl->cancelled) {
            result.cancelled = true;
            return result;
        }

        // 2. 坐标变换
        QList<QPointF> scaledPos, scaledNeg;
        for (const auto& p : positivePoints)
            scaledPos.append(scalePoint(p, origH, origW, inputSize));
        for (const auto& p : negativePoints)
            scaledNeg.append(scalePoint(p, origH, origW, inputSize));
        QRectF scaledBox;
        if (box.isValid()) {
            QRectF xywh = scaleBox(box, origH, origW, inputSize);
            scaledBox = QRectF(xywh.x(), xywh.y(), xywh.width(), xywh.height()); // xywh
            // decoder 需要.xyxy: 在 runDecoder 中构造
        }

        // 3. 解码器
        auto [lowRes, iou] =
            m_impl->runDecoder(embedding, scaledPos, scaledNeg,
                               box.isValid() ? QRectF(scaledBox.x(), scaledBox.y(), scaledBox.x() + scaledBox.width(),
                                                      scaledBox.y() + scaledBox.height())
                                             : QRectF(),
                               origH, origW);

        if (m_impl->cancelled) {
            result.cancelled = true;
            return result;
        }

        // 4. 后处理 + 候选
        // lowRes: [1,3,256,256]，iou: [1,3]
        const float* iouPtr = (const float*) iou.data;
        QList<QPair<double, int>> scored; // (score, channelIndex)
        for (int c = 0; c < 3; ++c)
            scored.append({iouPtr[c], c});
        // 降序排序
        std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

        for (const auto& [score, ch] : scored) {
            // 提取单个候选: [256,256]（从 [1,3,256,256] 中取第 ch 个通道）
            cv::Mat chMat(256, 256, CV_32F, (float*) lowRes.data + ch * 256 * 256);
            cv::Mat mask = m_impl->postprocess(chMat, inputSize, origH, origW);
            SamNativeCandidate cand;
            cand.mask = mask;
            cand.score = score; // 原始值（未 clamp，可 >1）
            cand.bbox = maskToBbox(mask);
            result.candidates.append(cand);
        }

        result.modelName = QStringLiteral("sam-vit_b-native");
        result.ok = true;
        return result;

    } catch (const std::exception& e) {
        result.error = QStringLiteral("推理失败：%1").arg(e.what());
        return result;
    }
}

} // namespace DeepLux
