#pragma once

#include "platform/Platform.h"

#include <QImage>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QVariantMap>

namespace DeepLux {

/**
 * @brief 执行线程发布的显示帧快照（后台执行设计 §7.1）
 *
 * - `jobId`：FlowExecutionService 的 job 代号（单调递增，跨 Cycle 轮稳定）；无 Service job 时为 0。
 * - `runId`：RunEngine 每轮唯一的 runId（Cycle 每轮不同）。
 * - `frameId`：该 run 内的帧序号（不复用全局 frameId 作为唯一键）。
 * - `moduleId`：产生该图像的模块运行名。
 * - `image`：执行线程内完成深拷贝的 QImage，脱离工作线程的共享缓冲。
 * - `moduleDataSnapshot`：该轮各模块 allData() 的不可变副本（不含大图）；仅 run 结束时填充。
 * - `viewportId`：目标视口（当前实现仅主视口 "main"）。
 */
struct DEEPLUX_API DisplayFrame {
    quint64 jobId = 0;
    QString runId;
    quint64 frameId = 0;
    QString moduleId;
    QString viewportId;
    QImage image;
    QMap<QString, QVariantMap> moduleDataSnapshot;

    bool hasImage() const {
        return !image.isNull();
    }
    // 中间帧只带图像；带快照的帧（run 结束帧）才允许绘制测量叠加（设计 §7.3）。
    bool hasSnapshot() const {
        return !moduleDataSnapshot.isEmpty();
    }
    // 帧身份是否匹配给定 (jobId, runId)：旧 job/旧轮的迟到帧必须丢弃（设计 §7.3）。
    bool matchesIdentity(quint64 expectedJobId, const QString& expectedRunId) const {
        return jobId == expectedJobId && runId == expectedRunId;
    }
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::DisplayFrame)
