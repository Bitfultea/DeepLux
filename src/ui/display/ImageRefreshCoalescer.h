#pragma once

#include "core/engine/DisplayFrame.h"

#include <QElapsedTimer>
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>

class QTimer;

namespace DeepLux {

/**
 * @brief 大图刷新合并器（后台执行设计 §7.2）
 *
 * 每视口一个“刷新节流器” + 待刷新槽位：
 * - 50ms 是刷新频率上限，不是每次通知都重置的防抖延迟；
 * - 距上次渲染不足 50ms 时只更新槽位，已排定的刷新时点不被重置；
 * - 到点后取最新帧渲染；若渲染期间又有新帧，则安排到 `lastRender + 50ms` 的下一个时点；
 * - 同一窗口内多次通知只渲染最后一帧，不积压历史帧；
 * - `forceFlush()` 只在 job 级结束/停止/暂停等时点由 UI 调用，立即渲染最新帧；
 * - `setExpectedIdentity()` 之后，旧 job（jobId 更小）与旧轮（runId 失配）的迟到帧被丢弃。
 *
 * 本类只负责调度与身份过滤，不做线程同步：所有调用都必须发生在 UI 线程（信号槽）。
 */
class ImageRefreshCoalescer : public QObject {
    Q_OBJECT

public:
    /// 刷新频率上限（设计 §7.2：50ms 为上限而非防抖窗口）。
    static constexpr int ThrottleMs = 50;

    using RenderCallback = std::function<void(const DisplayFrame&)>;

    explicit ImageRefreshCoalescer(QObject* parent = nullptr);

    void setRenderCallback(RenderCallback callback);

    /**
     * @brief 锁定当前 job/run 身份（MainWindow 在 job 启动时调用）。
     * @param jobId 最近 job 代号（单调递增）；0 表示清除高水位、不限制 job。
     *        非 0 时丢弃 jobId 更小的迟到帧（解除锁定后仍保留高水位，见 clearExpectedIdentity）。
     * @param runId 空表示接受该 job 的任意轮；非空时丢弃 runId 失配的帧。
     */
    void setExpectedIdentity(quint64 jobId, const QString& runId);
    /// 解除 run 身份锁定，但保留“最近 job”高水位：更旧 job 的迟到帧仍被丢弃。
    void clearExpectedIdentity();

    /// 提交一帧（latest-wins：只保留最新一帧，按身份过滤迟到帧）。
    void schedule(const DisplayFrame& frame);

    /// 立即渲染所有视口的最新待渲染帧（job 结束/停止/暂停、窗口恢复可见）。
    void forceFlush();

    /// 丢弃待渲染帧（新 job 启动、工程切换），不触发渲染。
    void clearPending();

    bool hasPending(const QString& viewportId = QStringLiteral("main")) const;
    quint64 pendingFrameId(const QString& viewportId = QStringLiteral("main")) const;
    QString pendingRunId(const QString& viewportId = QStringLiteral("main")) const;
    quint64 currentJobId() const {
        return m_jobId;
    }

private:
    struct Slot {
        DisplayFrame pending;
        bool hasPending = false;
        QTimer* timer = nullptr;
        qint64 lastRenderMs = -1; // 自 m_clock 起算；-1 表示尚未渲染
    };

    Slot& slotFor(const QString& viewportId);
    const Slot* findSlot(const QString& viewportId) const;
    void scheduleSlot(Slot& slot);
    void renderSlot(const QString& viewportId);
    bool accepts(const DisplayFrame& frame) const;

    RenderCallback m_renderCallback;
    QMap<QString, Slot> m_slots;
    QElapsedTimer m_clock;
    // 最近 job 高水位（jobId 单调递增）：更旧 job 的迟到帧一律丢弃（设计 §7.3）。
    quint64 m_jobId = 0;
    // 可选的 run 身份锁定：非空时同一 job 下 runId 失配的帧被丢弃。
    QString m_expectedRunId;
};

} // namespace DeepLux
