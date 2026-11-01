#include "ImageRefreshCoalescer.h"

#include <QTimer>
#include <algorithm>

namespace DeepLux {

ImageRefreshCoalescer::ImageRefreshCoalescer(QObject* parent) : QObject(parent) {
    m_clock.start();
}

void ImageRefreshCoalescer::setRenderCallback(RenderCallback callback) {
    m_renderCallback = std::move(callback);
}

void ImageRefreshCoalescer::setExpectedIdentity(quint64 jobId, const QString& runId) {
    const bool jobChanged = (jobId != 0 && jobId != m_jobId);
    if (jobChanged) {
        // 新 job：丢弃旧 job 的待渲染帧，避免旧帧被渲染到新 job 画面上（设计 §7.3）。
        clearPending();
    }
    m_jobId = jobId;
    m_expectedRunId = runId;
}

void ImageRefreshCoalescer::clearExpectedIdentity() {
    // 保留 m_jobId 作为“最近 job”高水位（设计 §7.3 加固）：即使解除身份锁定，
    // 更旧 job 的迟到帧仍会被 accepts() 丢弃；新 job（jobId 更大）照常接管。
    m_expectedRunId.clear();
}

bool ImageRefreshCoalescer::accepts(const DisplayFrame& frame) const {
    if (frame.jobId == 0) {
        // 无 Service job 的同步运行（CLI/测试兼容路径）：不受 job 身份限制。
        return true;
    }
    if (m_jobId != 0 && frame.jobId < m_jobId) {
        return false; // 更旧 job 的迟到帧：无活动 job 时同样丢弃，避免污染当前画面
    }
    if (!m_expectedRunId.isEmpty() && frame.jobId == m_jobId && frame.runId != m_expectedRunId) {
        return false; // 同一 job 下身份不匹配的帧
    }
    return true;
}

void ImageRefreshCoalescer::schedule(const DisplayFrame& frame) {
    if (!accepts(frame)) {
        return;
    }
    if (frame.jobId != 0 && m_jobId != 0 && frame.jobId > m_jobId) {
        // 未显式锁定时也要保证“新 job 到来清空旧 pending”（设计 §7.2/§7.3）。
        clearPending();
        m_jobId = frame.jobId;
        if (!m_expectedRunId.isEmpty()) {
            m_expectedRunId.clear();
        }
    } else if (frame.jobId != 0 && m_jobId == 0) {
        m_jobId = frame.jobId;
    }

    Slot& slot = slotFor(frame.viewportId);
    slot.pending = frame;
    slot.hasPending = true;
    scheduleSlot(slot);
}

void ImageRefreshCoalescer::scheduleSlot(Slot& slot) {
    if (slot.timer && slot.timer->isActive()) {
        return; // 已排定的刷新时点不得被后续通知重置（频率上限而非防抖）
    }
    const qint64 now = m_clock.elapsed();
    qint64 delay = 0;
    if (slot.lastRenderMs >= 0) {
        delay = slot.lastRenderMs + ThrottleMs - now;
    }
    delay = std::max<qint64>(0, delay);
    if (!slot.timer) {
        return;
    }
    slot.timer->start(static_cast<int>(delay));
}

void ImageRefreshCoalescer::renderSlot(const QString& viewportId) {
    auto it = m_slots.find(viewportId);
    if (it == m_slots.end()) {
        return;
    }
    Slot& slot = it.value();
    if (!slot.hasPending) {
        return;
    }
    DisplayFrame frame = slot.pending;
    slot.pending = DisplayFrame();
    slot.hasPending = false;
    slot.lastRenderMs = m_clock.elapsed();
    if (m_renderCallback) {
        m_renderCallback(frame);
    }
}

void ImageRefreshCoalescer::forceFlush() {
    const QStringList viewportIds = m_slots.keys();
    for (const QString& viewportId : viewportIds) {
        auto it = m_slots.find(viewportId);
        if (it == m_slots.end()) {
            continue;
        }
        if (it.value().timer) {
            it.value().timer->stop();
        }
        renderSlot(viewportId);
    }
}

void ImageRefreshCoalescer::clearPending() {
    for (auto it = m_slots.begin(); it != m_slots.end(); ++it) {
        if (it.value().timer) {
            it.value().timer->stop();
        }
        it.value().pending = DisplayFrame();
        it.value().hasPending = false;
    }
}

bool ImageRefreshCoalescer::hasPending(const QString& viewportId) const {
    const Slot* slot = findSlot(viewportId);
    return slot && slot->hasPending;
}

quint64 ImageRefreshCoalescer::pendingFrameId(const QString& viewportId) const {
    const Slot* slot = findSlot(viewportId);
    return slot ? slot->pending.frameId : 0;
}

QString ImageRefreshCoalescer::pendingRunId(const QString& viewportId) const {
    const Slot* slot = findSlot(viewportId);
    return slot ? slot->pending.runId : QString();
}

ImageRefreshCoalescer::Slot& ImageRefreshCoalescer::slotFor(const QString& viewportId) {
    auto it = m_slots.find(viewportId);
    if (it != m_slots.end()) {
        return it.value();
    }
    Slot slot;
    slot.timer = new QTimer(this);
    slot.timer->setSingleShot(true);
    const QString id = viewportId;
    connect(slot.timer, &QTimer::timeout, this, [this, id]() { renderSlot(id); });
    return m_slots.insert(viewportId, slot).value();
}

const ImageRefreshCoalescer::Slot* ImageRefreshCoalescer::findSlot(const QString& viewportId) const {
    const auto it = m_slots.constFind(viewportId);
    return it == m_slots.constEnd() ? nullptr : &it.value();
}

} // namespace DeepLux
