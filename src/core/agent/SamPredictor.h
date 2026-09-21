#pragma once

#include "core/deeplux/PayloadTypes.h"

#include <QJsonValue>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace DeepLux {

class CancellationToken;

/**
 * @brief 同步 SAM 预测核心（SAM 批3：SamPredictPlugin 与 Agent 工具共享）
 *
 * 单一实现收口"栈上 SamBackendClient + 本地事件循环"的同步预测语义
 * （NAM 线程亲和 = 调用线程）：set_image 一次、embedding 复用、逐项 predict。
 * 失败关闭：超时/后端错误/取消/区域无效或不符合 Region2D 契约（含非有限
 * 数值、score 越界）均不伪造区域。
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
    QList<Region2D> regions; // 与 items 一一对应
    // 批4（C）：每项的候选 score 列表（与 items 对齐；后端无候选时为空列表）
    QList<QList<double>> candidateScores;
};

/**
 * 同步执行 set_image + N×predict。
 * - serverUrl/imagePath 先行校验（空/不存在失败关闭，不发网络请求）
 * - timeoutMs ∈ [100,120000]，为"单请求"客户端超时；外层 deadline=timeoutMs+2000
 *   仅作客户端定时器故障兜底，且每收到一次响应重置（批1 复核二轮 P2-2 口径）
 * - cancel 非空时 100ms 轮询，取消后 error="SAM 预测已取消"
 * - 每个产物在返回前过 Region2D 端口契约门禁（portValueMatchesType），
 *   不合格视为后端错误失败关闭
 */
SamPredictOutcome runSamPredictBatch(const QString& serverUrl, const QString& imagePath,
                                     const QList<SamPromptItem>& items, int timeoutMs,
                                     const CancellationToken* cancel = nullptr);

/// JSON prompt 解析（严格有限数值，失败关闭并给出 error）：
/// points/negativePoints 形态 [[x,y],...]；undefined/null 时按 allowEmpty 处理
bool parseSamPointsJson(const QJsonValue& value, QList<QPointF>& out, bool allowEmpty, QString& error);
/// box 形态 [x,y,w,h]（w/h>0，4 有限数值）；undefined/null = 未提供（out 置无效矩形）
bool parseSamBoxJson(const QJsonValue& value, QRectF& out, QString& error);

} // namespace DeepLux
