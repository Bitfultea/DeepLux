#pragma once

#include "SamBackendClient.h"
#include "SamNativeSettings.h"
#include "core/deeplux/PayloadTypes.h"

#include <QJsonValue>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>

#ifdef DEEPLUX_HAS_ONNXRUNTIME
class QThreadPool;
#endif

namespace DeepLux {

class CancellationToken;
#ifdef DEEPLUX_HAS_ONNXRUNTIME
struct SamNativeResult;
QThreadPool* samNativeThreadPool(); // 应用持有，关闭标注窗口不等待在途 ONNX 调用
#endif

/**
 * @brief 同步 SAM 预测核心（SAM 批3：SamPredictPlugin 与 Agent 工具共享）
 *
 * HTTP 模式用栈上 SamBackendClient + 本地事件循环；显式 serverUrl="native"
 * 时使用 SamNativeSettings 提供的模型包配置，环境变量可作为部署覆盖。
 * 失败关闭：超时/后端错误/取消/区域无效或不符合 Region2D 契约（含非有限
 * 数值）均不伪造区域。原生模型输出未经裁剪的候选分数，写入 Region2D 时
 * 才归一到其 [0,1] 契约；HTTP 路径继续拒绝协议越界分数。
 */

/// 一次 prompt（批量预测的一项）
struct SamPromptItem {
    QList<QPointF> positivePoints;
    QList<QPointF> negativePoints;
    QRectF box;    // 无效 = 未提供
    QString label; // 写入产物 Region2D.label
    // 批4（C）：多掩膜候选选择——后端返回 candidates（score 降序）时选取
    // 第 candidateIndex 个；0 = 最佳（与顶层 best 字段一致）。后端未返回
    // 候选而 index>0，或 index 越界，均失败关闭
    int candidateIndex = 0;
};

/// 同步预测结果（ok=false 时 error 非空；regions 仅成功时完整）
struct SamPredictOutcome {
    bool ok = false;
    QString error;
    int failedIndex = -1;    // 失败发生在第几个 prompt（-1 = set_image/参数阶段）
    QString modelName;       // 后端模型名（成功时；空则调用方自行回退）
    QString nativeDevice;    // 原生模式实际使用的设备（auto 解析后）
    QList<Region2D> regions; // 与 items 一一对应
    // 与 items 对齐；native 保留原始模型分数（可能 >1），HTTP 为协议分数
    QList<QList<double>> candidateScores;
    // 仅显式请求时填充，供 GUI 循环预览；普通 Agent/插件路径不物化全部掩膜。
    QList<QList<SamCandidate>> previewCandidates;
};

#ifdef DEEPLUX_HAS_ONNXRUNTIME
namespace SamPredictorDetail {
SamPredictOutcome convertNativeCandidates(const SamNativeResult& prediction, const SamPromptItem& item,
                                          const QSize& imageSize, bool includeCandidates);
}
#endif

/**
 * 同步执行 set_image + N×predict。
 * - serverUrl/imagePath 先行校验（空/不存在失败关闭，不发网络请求）；
 *   native 仅在 DEEPLUX_NATIVE_SAM=ON 构建中可用，不会静默回退 Python
 * - timeoutMs ∈ [100,120000]，为"单请求"客户端超时；外层 deadline=timeoutMs+2000
 *   仅作客户端定时器故障兜底，且每收到一次响应重置（批1 复核二轮 P2-2 口径）
 * - HTTP 模式 cancel 每 100ms 轮询；原生模式在模型操作前后检查取消，
 *   timeoutMs 为操作完成后的失败关闭上限，不能中断正在执行的 ORT 调用
 * - 每个产物在返回前过 Region2D 端口契约门禁（portValueMatchesType），
 *   不合格视为后端错误失败关闭
 */
SamPredictOutcome runSamPredictBatch(const QString& serverUrl, const QString& imagePath,
                                     const QList<SamPromptItem>& items, int timeoutMs,
                                     const CancellationToken* cancel = nullptr, bool includeCandidates = false,
                                     const SamNativeConfig& nativeConfig = {});

/// JSON prompt 解析（严格有限数值，失败关闭并给出 error）：
/// points/negativePoints 形态 [[x,y],...]；undefined/null 时按 allowEmpty 处理
bool parseSamPointsJson(const QJsonValue& value, QList<QPointF>& out, bool allowEmpty, QString& error);
/// box 形态 [x,y,w,h]（w/h>0，4 有限数值）；undefined/null = 未提供（out 置无效矩形）
bool parseSamBoxJson(const QJsonValue& value, QRectF& out, QString& error);

} // namespace DeepLux
