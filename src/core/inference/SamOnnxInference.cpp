#include "SamOnnxInference.h"

#include "SamPillowResize.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <onnxruntime_c_api.h>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <string>
#include <vector>

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
        if (!g_ort)
            throw std::runtime_error("onnxruntime API version mismatch");
        OrtStatus* status = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "DeepLux_SamNative", &env);
        if (status) {
            std::string message(g_ort->GetErrorMessage(status));
            g_ort->ReleaseStatus(status);
            throw std::runtime_error(message);
        }
    }
};

// CUDA Provider 在进程退出时可能晚于静态对象析构；环境保持到进程终止，会话仍按需释放。
OrtEnvHolder* ortEnv() {
    static auto* const h = new OrtEnvHolder();
    return h;
}

#define ORT_CK(expr)                                                                                                   \
    do {                                                                                                               \
        OrtStatus* _st = (expr);                                                                                       \
        if (_st) {                                                                                                     \
            std::string _msgCopy(g_ort->GetErrorMessage(_st));                                                         \
            g_ort->ReleaseStatus(_st);                                                                                 \
            throw std::runtime_error(_msgCopy);                                                                        \
        }                                                                                                              \
    } while (0)

struct OrtSessionRAII {
    OrtSession* sess = nullptr;
    OrtAllocator* allocator = nullptr; // 默认分配器（单例，不可 Release）
    void reset() {
        // allocator 是默认单例，不释放（仅 Release Session）
        if (sess)
            g_ort->ReleaseSession(sess);
        sess = nullptr;
        allocator = nullptr;
    }
    ~OrtSessionRAII() {
        reset();
    }
};

struct OrtSessionOptionsRAII {
    OrtSessionOptions* opts = nullptr;
    ~OrtSessionOptionsRAII() {
        if (opts)
            g_ort->ReleaseSessionOptions(opts);
    }
};

struct OrtCUDAOptionsRAII {
    OrtCUDAProviderOptionsV2* opts = nullptr;
    ~OrtCUDAOptionsRAII() {
        if (opts)
            g_ort->ReleaseCUDAProviderOptions(opts);
    }
};

struct OrtShapeRAII {
    OrtTensorTypeAndShapeInfo* info = nullptr;
    ~OrtShapeRAII() {
        if (info)
            g_ort->ReleaseTensorTypeAndShapeInfo(info);
    }
};

struct OrtMemoryInfoRAII {
    OrtMemoryInfo* info = nullptr;
    ~OrtMemoryInfoRAII() {
        if (info)
            g_ort->ReleaseMemoryInfo(info);
    }
};

template <size_t N> struct OrtNamesRAII {
    explicit OrtNamesRAII(OrtAllocator* allocator) : allocator(allocator) {}
    ~OrtNamesRAII() {
        for (char* name : names)
            if (name) {
                if (OrtStatus* status = g_ort->AllocatorFree(allocator, name))
                    g_ort->ReleaseStatus(status);
            }
    }
    OrtAllocator* allocator;
    std::array<char*, N> names{};
};

template <size_t N> struct OrtOutputsRAII {
    ~OrtOutputsRAII() {
        for (OrtValue* value : values)
            if (value)
                g_ort->ReleaseValue(value);
    }
    std::array<OrtValue*, N> values{};
};

void validateOutputShape(OrtValue* value, std::initializer_list<int64_t> expected, const char* name) {
    OrtShapeRAII shape;
    ORT_CK(g_ort->GetTensorTypeAndShape(value, &shape.info));
    ONNXTensorElementDataType type;
    ORT_CK(g_ort->GetTensorElementType(shape.info, &type));
    if (type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        throw std::runtime_error(std::string(name) + " 输出类型必须为 float32");
    size_t rank = 0;
    ORT_CK(g_ort->GetDimensionsCount(shape.info, &rank));
    if (rank != expected.size())
        throw std::runtime_error(std::string(name) + " 输出维数不符");
    std::vector<int64_t> dimensions(rank);
    ORT_CK(g_ort->GetDimensions(shape.info, dimensions.data(), rank));
    if (!std::equal(dimensions.begin(), dimensions.end(), expected.begin()))
        throw std::runtime_error(std::string(name) + " 输出形状不符");
}
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

// 图像哈希（全量像素数据 + 尺寸 → sha256；不漏 1 像素差异，P1-4 修正）
QString computeImageHash(const cv::Mat& image) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // 尺寸 + 通道数先入哈希（换尺寸同内容也不命中）
    const int32_t meta[] = {image.rows, image.cols, image.type()};
    hash.addData(reinterpret_cast<const char*>(meta), sizeof(meta));
    // 全量像素数据（连续行扫描；非连续 Mat 逐行入哈希）
    for (int y = 0; y < image.rows; ++y)
        hash.addData(reinterpret_cast<const char*>(image.ptr(y)), image.cols * image.elemSize());
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
    QString device = QStringLiteral("cpu");
    QString requestedDevice = QStringLiteral("cpu");
    QString lastError;
    bool loaded = false;
    bool profilingEnabled = false;

    // ORT sessions
    OrtSessionRAII encoder;
    OrtSessionRAII decoder;

    // embedding cache
    cv::Mat cachedEmbedding; // [1,256,64,64] float32
    QString cachedImageHash;
    QSize cachedInputSize; // (new_w, new_h) 即 resize 后、pad 前的尺寸

    // cancel
    std::atomic<bool> cancelled{false};

    // 预处理：RGB → resize → **先归一化后零填充**（安装版 Sam.preprocess 口径，
    // P0-1 修正：填充区值为归一化空间的 0 而非 (0-mean)/std≈[-2.118,...]）
    cv::Mat preprocess(const cv::Mat& image, QSize& outInputSize) {
        int h = image.rows, w = image.cols;
        QSize newSize = getPreprocessShape(h, w);
        outInputSize = newSize;

        cv::Mat resized = SamNativeDetail::resizePillowBilinear(image, cv::Size(newSize.width(), newSize.height()));

        // 先归一化（per-channel (x-mean)/std）
        cv::Mat resizedf;
        resized.convertTo(resizedf, CV_32FC3);
        cv::Mat chs[3];
        cv::split(resizedf, chs);
        for (int c = 0; c < 3; ++c)
            chs[c] = (chs[c] - PIXEL_MEAN[c]) / PIXEL_STD[c];
        cv::Mat normalized;
        cv::merge(chs, 3, normalized);

        // 后零填充（归一化空间 0 = 无信息；1024² 底色）
        cv::Mat padded = cv::Mat::zeros(TARGET_SIZE, TARGET_SIZE, CV_32FC3);
        normalized.copyTo(padded(cv::Rect(0, 0, newSize.width(), newSize.height())));

        // HWC → NCHW
        cv::split(padded, chs);
        cv::Mat blob(1, 3 * TARGET_SIZE * TARGET_SIZE, CV_32F);
        float* dst = (float*) blob.data;
        for (int c = 0; c < 3; ++c) {
            cv::Mat flat = chs[c].reshape(1, 1);
            std::memcpy(dst + c * TARGET_SIZE * TARGET_SIZE, flat.data, TARGET_SIZE * TARGET_SIZE * sizeof(float));
        }
        return blob;
    }

    // 编码器前向
    cv::Mat runEncoder(const cv::Mat& inputBlob) {
        OrtNamesRAII<1> inNames(encoder.allocator), outNames(encoder.allocator);
        ORT_CK(g_ort->SessionGetInputName(encoder.sess, 0, encoder.allocator, &inNames.names[0]));
        ORT_CK(g_ort->SessionGetOutputName(encoder.sess, 0, encoder.allocator, &outNames.names[0]));
        const char* inputNames[] = {inNames.names[0]};
        const char* outputNames[] = {outNames.names[0]};

        OrtMemoryInfoRAII memInfo;
        ORT_CK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo.info));

        int64_t shape[] = {1, 3, TARGET_SIZE, TARGET_SIZE};
        OrtValueRAII inputTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo.info, (void*) inputBlob.data,
                                                     inputBlob.total() * inputBlob.elemSize(), shape, 4,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputTensor.val));
        OrtOutputsRAII<1> outputs;
        ORT_CK(
            g_ort->Run(encoder.sess, nullptr, inputNames, &inputTensor.val, 1, outputNames, 1, outputs.values.data()));
        validateOutputShape(outputs.values[0], {1, 256, 64, 64}, "encoder");
        void* outData;
        ORT_CK(g_ort->GetTensorMutableData(outputs.values[0], &outData));
        cv::Mat embedding(1, 256 * 64 * 64, CV_32F, outData);
        cv::Mat copy = embedding.clone();
        return copy.reshape(1, {1, 256, 64, 64});
    }

    // 解码器前向
    // 返回 (low_res_masks [1,3,256,256] float32, iou [1,3] float32)
    std::pair<cv::Mat, cv::Mat> runDecoder(const cv::Mat& embedding, const QList<QPointF>& scaledPos,
                                           const QList<QPointF>& scaledNeg, const QRectF& scaledBox) {
        // 构造输入
        int nPts = scaledPos.size() + scaledNeg.size();
        int nBox = scaledBox.isValid() ? 1 : 0;

        // point_coords [N,2] float32（N=0 时传真正的零维张量 [0,2]——P0-2 修正：
        // 不伪造空提示，模型 wrapper 的 index_select 自身处理零尺寸）
        std::vector<float> coordsData(nPts * 2, 0);
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
        std::vector<int64_t> labelsData(nPts, 0);
        idx = 0;
        for (int i = 0; i < scaledPos.size(); ++i)
            labelsData[idx++] = 1;
        for (int i = 0; i < scaledNeg.size(); ++i)
            labelsData[idx++] = 0;

        // boxes [M,4] float32（M=0 时传真正的零维张量 [0,4]）
        std::vector<float> boxesData(nBox * 4, 0);
        if (nBox) {
            // scaledBox 是 xywh（scaleBox 返回口径）：x1, y1, x2-x1, y2-y1
            boxesData[0] = scaledBox.x();
            boxesData[1] = scaledBox.y();
            boxesData[2] = scaledBox.x() + scaledBox.width();
            boxesData[3] = scaledBox.y() + scaledBox.height();
        }

        // 如果没有点也没有框 → 返回错误
        if (nPts == 0 && nBox == 0)
            throw std::runtime_error("至少需要一个 prompt（点或框）");

        // 获取 decoder 的输入/输出名称
        OrtNamesRAII<4> names(decoder.allocator);
        OrtNamesRAII<2> outNamesC(decoder.allocator);
        const char* inNames[4];
        for (size_t i = 0; i < 4; ++i) {
            ORT_CK(g_ort->SessionGetInputName(decoder.sess, i, decoder.allocator, &names.names[i]));
            inNames[i] = names.names[i];
        }
        const char* outNames[2];
        for (size_t i = 0; i < 2; ++i) {
            ORT_CK(g_ort->SessionGetOutputName(decoder.sess, i, decoder.allocator, &outNamesC.names[i]));
            outNames[i] = outNamesC.names[i];
        }

        OrtMemoryInfoRAII memInfo;
        ORT_CK(g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memInfo.info));

        // image_embeddings: [1,256,64,64] float32（零拷贝）
        int64_t embShape[] = {1, 256, 64, 64};
        OrtValueRAII embTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo.info, (void*) embedding.data,
                                                     embedding.total() * embedding.elemSize(), embShape, 4,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &embTensor.val));

        // point_coords: [N,2] float32（N=0 时 shape={0,2} 真正零维——P0-2）
        int64_t coordsShape[] = {nPts, 2};
        OrtValueRAII coordsTensor;
        // nPts=0 时 data 长度=0，ORT 接受 nullptr + 0 长度的零维张量
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo.info, coordsData.data(), coordsData.size() * sizeof(float),
                                                     coordsShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                     &coordsTensor.val));

        // point_labels: [N] int64（N=0 时 shape={0}）
        int64_t labelsShape[] = {nPts};
        OrtValueRAII labelsTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo.info, labelsData.data(),
                                                     labelsData.size() * sizeof(int64_t), labelsShape, 1,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &labelsTensor.val));

        // boxes: [M,4] float32（M=0 时 shape={0,4}）
        int64_t boxesShape[] = {nBox, 4};
        OrtValueRAII boxesTensor;
        ORT_CK(g_ort->CreateTensorWithDataAsOrtValue(memInfo.info, boxesData.data(), boxesData.size() * sizeof(float),
                                                     boxesShape, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                                                     &boxesTensor.val));

        const OrtValue* inputs[4] = {embTensor.val, coordsTensor.val, labelsTensor.val, boxesTensor.val};
        OrtOutputsRAII<2> outputs;
        ORT_CK(g_ort->Run(decoder.sess, nullptr, inNames, inputs, 4, outNames, 2, outputs.values.data()));
        validateOutputShape(outputs.values[0], {1, 3, 256, 256}, "decoder.masks");
        validateOutputShape(outputs.values[1], {1, 3}, "decoder.iou");

        // 提取 low_res_masks [1,3,256,256] 和 iou [1,3]
        void* maskData;
        ORT_CK(g_ort->GetTensorMutableData(outputs.values[0], &maskData));
        cv::Mat lowRes(1, 3 * 256 * 256, CV_32F, maskData);
        cv::Mat lowResCopy = lowRes.clone();

        void* iouData;
        ORT_CK(g_ort->GetTensorMutableData(outputs.values[1], &iouData));
        cv::Mat iou(1, 3, CV_32F, iouData);
        cv::Mat iouCopy = iou.clone();

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

bool SamOnnxInference::loadPackage(const QString& packageDir, const QString& device, const QString& profilePrefix) {
    m_impl->loaded = false;
    m_impl->profilingEnabled = false;
    m_impl->encoder.reset();
    m_impl->decoder.reset();
    m_impl->cachedImageHash.clear();
    m_impl->cachedEmbedding.release();
    m_impl->cachedInputSize = QSize();
    m_impl->packageDir = packageDir;
    m_impl->lastError.clear();
    if (device != QLatin1String("auto") && device != QLatin1String("cpu") && device != QLatin1String("cuda")) {
        m_impl->lastError = QStringLiteral("原生 SAM device 仅支持 auto、cpu 或 cuda：%1").arg(device);
        return false;
    }

    // 1. 加载 manifest
    QString manPath = packageDir + "/manifest.json";
    if (QFileInfo(manPath).isSymLink()) {
        m_impl->lastError = QStringLiteral("manifest.json 不得为符号链接");
        return false;
    }
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
    // 必需语义字段 + 具体值校验（P1-5：初版只查非空对象，接受 {"junk":1}）
    auto requireField = [&](const QString& path, const QJsonValue& val, const QString& type) -> bool {
        if (type == "object" && (!val.isObject() || val.toObject().isEmpty())) {
            m_impl->lastError = QStringLiteral("manifest.%1 必须为非空对象").arg(path);
            return false;
        }
        if (type == "string" && (!val.isString() || val.toString().isEmpty())) {
            m_impl->lastError = QStringLiteral("manifest.%1 必须为非空字符串").arg(path);
            return false;
        }
        if (type == "array3num") {
            QJsonArray arr = val.toArray();
            if (arr.size() != 3 || !arr.at(0).isDouble() || !arr.at(1).isDouble() || !arr.at(2).isDouble()) {
                m_impl->lastError = QStringLiteral("manifest.%1 必须为 3 个数值数组").arg(path);
                return false;
            }
        }
        return true;
    };

    // preprocessing
    QJsonObject pp = m_impl->manifest.value("preprocessing").toObject();
    if (!requireField("preprocessing", m_impl->manifest.value("preprocessing"), "object"))
        return false;
    if (pp.value("colorOrder").toString() != "RGB") {
        m_impl->lastError = QStringLiteral("preprocessing.colorOrder 必须为 RGB");
        return false;
    }
    QJsonObject rz = pp.value("resize").toObject();
    if (rz.value("mode").toString() != "longestSide" || !rz.value("targetSize").isDouble()) {
        m_impl->lastError = QStringLiteral("preprocessing.resize.mode 必须为 longestSide 且 targetSize 为整数");
        return false;
    }
    if (rz.value("targetSize").toDouble() != TARGET_SIZE) {
        m_impl->lastError = QStringLiteral("preprocessing.resize.targetSize 必须为 %1").arg(TARGET_SIZE);
        return false;
    }
    if (rz.value("interpolation").toString() !=
        QStringLiteral("ResizeLongestSide.apply_image 口径：torchvision resize(to_pil_image)（PIL 双线性；"
                       "阶段2 C++ 重采样器须与门禁对照验证）")) {
        m_impl->lastError = QStringLiteral("preprocessing.resize.interpolation 不支持");
        return false;
    }
    const QJsonArray padTo = pp.value("pad").toObject().value("to").toArray();
    const QJsonArray padSides = pp.value("pad").toObject().value("sides").toArray();
    if (pp.value("order").toString() !=
            QStringLiteral("先归一化后零填充（Sam.preprocess 顺序；填充值为归一化空间的 0）") ||
        padTo != QJsonArray{TARGET_SIZE, TARGET_SIZE} || padSides != QJsonArray{"bottom", "right"} ||
        pp.value("pad").toObject().value("value").toDouble(-1) != 0) {
        m_impl->lastError = QStringLiteral("preprocessing 归一化/填充契约不符");
        return false;
    }
    QJsonObject nm = pp.value("normalize").toObject();
    if (nm.value("scale").toString() != QStringLiteral("0-255 float32（不除 255）")) {
        m_impl->lastError = QStringLiteral("preprocessing.normalize.scale 不符");
        return false;
    }
    if (!requireField("preprocessing.normalize.mean", nm.value("mean"), "array3num") ||
        !requireField("preprocessing.normalize.std", nm.value("std"), "array3num"))
        return false;
    // P1-4：均值/标准差须与实际使用的常量匹配（初版只验证三项是数字）
    {
        QJsonArray meanArr = nm.value("mean").toArray();
        QJsonArray stdArr = nm.value("std").toArray();
        for (int c = 0; c < 3; ++c) {
            if (std::abs(meanArr.at(c).toDouble() - PIXEL_MEAN[c]) > 1e-3) {
                m_impl->lastError =
                    QStringLiteral("preprocessing.normalize.mean[%1] 不匹配实际常量 %2").arg(c).arg(PIXEL_MEAN[c]);
                return false;
            }
            if (std::abs(stdArr.at(c).toDouble() - PIXEL_STD[c]) > 1e-3) {
                m_impl->lastError =
                    QStringLiteral("preprocessing.normalize.std[%1] 不匹配实际常量 %2").arg(c).arg(PIXEL_STD[c]);
                return false;
            }
        }
    }

    // coordinateTransform
    QJsonObject ct = m_impl->manifest.value("coordinateTransform").toObject();
    if (!requireField("coordinateTransform", m_impl->manifest.value("coordinateTransform"), "object"))
        return false;
    if (!requireField("coordinateTransform.mode", ct.value("mode"), "string"))
        return false;
    if (ct.value("mode").toString() != QStringLiteral("scale-only（ResizeLongestSide.apply_coords/apply_boxes）")) {
        m_impl->lastError = QStringLiteral("coordinateTransform.mode 不支持");
        return false;
    }

    // semantics
    QJsonObject sem = m_impl->manifest.value("semantics").toObject();
    if (!requireField("semantics", m_impl->manifest.value("semantics"), "object"))
        return false;
    if (!requireField("semantics.decoderOutputs", sem.value("decoderOutputs"), "string"))
        return false;
    if (!requireField("semantics.candidateSelection", sem.value("candidateSelection"), "string"))
        return false;
    QJsonObject post = sem.value("postprocess").toObject();
    if (post.value("mode").toString() != "caller") {
        m_impl->lastError = QStringLiteral("semantics.postprocess.mode 必须为 caller");
        return false;
    }

    // files：包内裸文件名（拒绝绝对路径/穿越/符号链接——P1-5）
    QJsonObject files = m_impl->manifest.value("files").toObject();
    QJsonObject encMeta = files.value("encoder").toObject();
    QJsonObject decMeta = files.value("decoder").toObject();
    QString encFile = encMeta.value("file").toString();
    QString decFile = decMeta.value("file").toString();
    for (const auto& [meta, name] :
         {std::make_pair(encMeta, QStringLiteral("encoder")), std::make_pair(decMeta, QStringLiteral("decoder"))}) {
        QString fname = meta.value("file").toString();
        if (fname != QFileInfo(fname).fileName() || fname.isEmpty()) {
            m_impl->lastError = QStringLiteral("files.%1.file 必须为包内裸文件名").arg(name);
            return false;
        }
        // 符号链接拒绝（包自包含）
        QString fullPath = packageDir + "/" + fname;
        QFileInfo fi(fullPath);
        if (fi.isSymLink()) {
            m_impl->lastError = QStringLiteral("files.%1.file 不得为符号链接").arg(name);
            return false;
        }
    }

    // 3. SHA-256 + sizeBytes 校验
    auto checkFile = [&](const QString& path, const QJsonObject& meta, const QString& name) -> bool {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            m_impl->lastError = QStringLiteral("%1 文件不可读").arg(name);
            return false;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&f);
        if (QString::fromLatin1(hash.result().toHex()) != meta.value("sha256").toString()) {
            m_impl->lastError = QStringLiteral("%1 SHA-256 不符").arg(name);
            return false;
        }
        // P1-2 修正：sizeBytes 必须存在且匹配（初版缺失时跳过）
        const QJsonValue sizeValue = meta.value("sizeBytes");
        if (!sizeValue.isDouble() || !std::isfinite(sizeValue.toDouble()) || sizeValue.toDouble() < 0 ||
            sizeValue.toDouble() >= std::ldexp(1.0, 63) || std::floor(sizeValue.toDouble()) != sizeValue.toDouble()) {
            m_impl->lastError = QStringLiteral("%1 sizeBytes 必须为有效非负整数").arg(name);
            return false;
        }
        qint64 actualSize = f.size();
        qint64 expectedSize = static_cast<qint64>(meta.value("sizeBytes").toDouble(-1));
        if (actualSize != expectedSize) {
            m_impl->lastError =
                QStringLiteral("%1 大小不符：manifest=%2 实际=%3").arg(name).arg(expectedSize).arg(actualSize);
            return false;
        }
        return true;
    };

    QString encPath = packageDir + "/" + encFile;
    QString decPath = packageDir + "/" + decFile;
    if (!checkFile(encPath, encMeta, "encoder"))
        return false;
    if (!checkFile(decPath, decMeta, "decoder"))
        return false;

    // 两段都成功后才发布新会话。
    try {
        const auto env = ortEnv();
        auto createSessions = [&](const QString& selectedDevice, OrtSessionRAII& newEncoder,
                                  OrtSessionRAII& newDecoder) {
            auto configure = [&](OrtSessionOptionsRAII& opts, const QString& name) {
                ORT_CK(g_ort->CreateSessionOptions(&opts.opts));
                ORT_CK(g_ort->SetIntraOpNumThreads(opts.opts, 4));
                if (selectedDevice == QLatin1String("cuda")) {
                    OrtCUDAOptionsRAII cuda;
                    ORT_CK(g_ort->CreateCUDAProviderOptions(&cuda.opts));
                    ORT_CK(g_ort->SessionOptionsAppendExecutionProvider_CUDA_V2(opts.opts, cuda.opts));
                }
                if (!profilePrefix.isEmpty()) {
#ifdef _WIN32
                    const std::wstring prefix = (profilePrefix + name).toStdWString();
                    ORT_CK(g_ort->EnableProfiling(opts.opts, prefix.c_str()));
#else
                    const QByteArray prefix = (profilePrefix + name).toUtf8();
                    ORT_CK(g_ort->EnableProfiling(opts.opts, prefix.constData()));
#endif
                }
            };
            OrtSessionOptionsRAII encoderOpts, decoderOpts;
            configure(encoderOpts, QStringLiteral("_encoder"));
            configure(decoderOpts, QStringLiteral("_decoder"));
            // ORTCHAR_T is wchar_t on Windows and char elsewhere; CreateSession
            // does not accept UTF-8 on Windows.
#ifdef _WIN32
            const std::wstring encoderPath = encPath.toStdWString();
            const std::wstring decoderPath = decPath.toStdWString();
            ORT_CK(g_ort->CreateSession(env->env, encoderPath.c_str(), encoderOpts.opts, &newEncoder.sess));
            ORT_CK(g_ort->CreateSession(env->env, decoderPath.c_str(), decoderOpts.opts, &newDecoder.sess));
#else
            const QByteArray encoderPath = encPath.toUtf8();
            const QByteArray decoderPath = decPath.toUtf8();
            ORT_CK(g_ort->CreateSession(env->env, encoderPath.constData(), encoderOpts.opts, &newEncoder.sess));
            ORT_CK(g_ort->CreateSession(env->env, decoderPath.constData(), decoderOpts.opts, &newDecoder.sess));
#endif
        };
        OrtSessionRAII newEncoder, newDecoder;
        QString activeDevice = device;
        if (device == QLatin1String("auto")) {
            try {
                createSessions(QStringLiteral("cuda"), newEncoder, newDecoder);
                activeDevice = QStringLiteral("cuda");
            } catch (const std::exception&) {
                newEncoder.reset();
                newDecoder.reset();
                createSessions(QStringLiteral("cpu"), newEncoder, newDecoder);
                activeDevice = QStringLiteral("cpu");
            }
        } else {
            createSessions(device, newEncoder, newDecoder);
        }
        ORT_CK(g_ort->GetAllocatorWithDefaultOptions(&newEncoder.allocator));
        newDecoder.allocator = newEncoder.allocator;
        std::swap(m_impl->encoder.sess, newEncoder.sess);
        std::swap(m_impl->decoder.sess, newDecoder.sess);
        m_impl->encoder.allocator = newEncoder.allocator;
        m_impl->decoder.allocator = newDecoder.allocator;
        m_impl->device = activeDevice;
        m_impl->requestedDevice = device;
        m_impl->profilingEnabled = !profilePrefix.isEmpty();
    } catch (const std::exception& e) {
        m_impl->lastError = QStringLiteral("ORT 加载失败：%1").arg(e.what());
        return false;
    }

    m_impl->loaded = true;
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
QString SamOnnxInference::device() const {
    return m_impl->device;
}
QString SamOnnxInference::requestedDevice() const {
    return m_impl->requestedDevice;
}
QStringList SamOnnxInference::endProfiling() {
    if (!m_impl->loaded || !m_impl->profilingEnabled)
        return {};
    QStringList paths;
    try {
        for (OrtSessionRAII* session : {&m_impl->encoder, &m_impl->decoder}) {
            char* path = nullptr;
            ORT_CK(g_ort->SessionEndProfiling(session->sess, session->allocator, &path));
            if (!path)
                throw std::runtime_error("ORT profiling 未返回文件路径");
            paths.append(QString::fromUtf8(path));
            ORT_CK(g_ort->AllocatorFree(session->allocator, path));
        }
    } catch (const std::exception& e) {
        m_impl->lastError = QStringLiteral("ORT profiling 结束失败：%1").arg(e.what());
        return {};
    }
    m_impl->profilingEnabled = false;
    return paths;
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
    if (image.empty() || image.type() != CV_8UC3) {
        result.error = QStringLiteral("图像必须为非空 RGB 8 位 3 通道");
        return result;
    }
    for (const QPointF& point : positivePoints + negativePoints) {
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            result.error = QStringLiteral("prompt 点坐标必须为有限数值");
            return result;
        }
    }
    if (box != QRectF() && (!box.isValid() || !std::isfinite(box.x()) || !std::isfinite(box.y()) ||
                            !std::isfinite(box.width()) || !std::isfinite(box.height()))) {
        result.error = QStringLiteral("prompt 框必须为有限且有效的矩形");
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
            // scaleBox 返回 xywh (x1, y1, x2-x1, y2-y1)——runDecoder 内部
            // x()+width()=x2, y()+height()=y2 转为 xyxy，直接传 xywh 即可
            // P0-3 修正：初版把 xywh 的 w/h 当绝对坐标再包一层 QRectF 导致
            // x()+width()=x1+x2 双重相加（框传成 [160,160,640,640] 而非 [160,160,480,480]）
            scaledBox = scaleBox(box, origH, origW, inputSize);
        }

        // 3. 解码器
        auto [lowRes, iou] = m_impl->runDecoder(embedding, scaledPos, scaledNeg, box.isValid() ? scaledBox : QRectF());

        if (m_impl->cancelled) {
            result.cancelled = true;
            return result;
        }

        // 4. 后处理 + 候选
        // lowRes: [1,3,256,256]，iou: [1,3]
        const float* iouPtr = (const float*) iou.data;
        QList<QPair<double, int>> scored; // (score, channelIndex)
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(iouPtr[c]))
                throw std::runtime_error("decoder 输出非有限分数");
            scored.append({iouPtr[c], c});
        }
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
            if (m_impl->cancelled) {
                result.candidates.clear();
                result.cancelled = true;
                return result;
            }
        }

        if (m_impl->cancelled) {
            result.candidates.clear();
            result.cancelled = true;
            return result;
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
