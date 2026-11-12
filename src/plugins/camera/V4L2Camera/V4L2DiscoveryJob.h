#pragma once

#include "V4L2DiscoverySystem.h"
#include "core/device/CameraDiscoveryJob.h"

#include <QThread>
#include <atomic>
#include <memory>

namespace DeepLux {

class V4L2DiscoveryWorker;

class V4L2DiscoveryJob final : public CameraDiscoveryJob {
    Q_OBJECT
public:
    V4L2DiscoveryJob(quint64 requestId, std::shared_ptr<IV4L2DiscoverySystem> system,
                     std::shared_ptr<std::atomic_bool> cancelRequested, QObject* parent);
    ~V4L2DiscoveryJob() override;

private slots:
    void startWorker();
    void onWorkerResult(const QByteArray& payload);
    void onWorkerThreadFinished();

private:
    void publishWhenDrained();

    quint64 m_requestId;
    std::shared_ptr<IV4L2DiscoverySystem> m_system;
    std::shared_ptr<std::atomic_bool> m_cancelRequested;
    QThread* m_thread = nullptr;
    V4L2DiscoveryWorker* m_worker = nullptr;
    QByteArray m_payload;
    bool m_hasPayload = false;
    bool m_threadFinished = false;
};

} // namespace DeepLux
