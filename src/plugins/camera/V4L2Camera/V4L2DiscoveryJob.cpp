#include "V4L2DiscoveryJob.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QTimer>
#include <QVariantMap>
#include <exception>

namespace DeepLux {

namespace {

QJsonObject errorPayload(quint64 requestId, const QString& errorCode) {
    const bool cancelled = errorCode == QLatin1String("CAMERA_DISCOVERY_CANCELLED");
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("requestId"), QString::number(requestId)},
            {QStringLiteral("status"), cancelled ? QStringLiteral("cancelled") : QStringLiteral("error")},
            {QStringLiteral("errorCode"), errorCode}};
}

QJsonObject cameraPayload(const QString& path, const V4L2DeviceInfo& device) {
    const QJsonObject rawInfo{{QStringLiteral("driver"), device.driver}};
    return {{QStringLiteral("pluginId"), QStringLiteral("v4l2")},
            {QStringLiteral("deviceId"), path},
            {QStringLiteral("name"), device.card},
            {QStringLiteral("serialNumber"), device.busInfo},
            {QStringLiteral("ipAddress"), QString()},
            {QStringLiteral("width"), device.width},
            {QStringLiteral("height"), device.height},
            {QStringLiteral("pixelFormat"), device.pixelFormat},
            {QStringLiteral("isConnected"), false},
            {QStringLiteral("rawInfo"), rawInfo}};
}

class ScopedFd {
public:
    ScopedFd(std::shared_ptr<IV4L2DiscoverySystem> system, int fd) : m_system(std::move(system)), m_fd(fd) {}
    ~ScopedFd() {
        if (m_fd >= 0)
            m_system->closeDevice(m_fd);
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

private:
    std::shared_ptr<IV4L2DiscoverySystem> m_system;
    int m_fd;
};

} // namespace

class V4L2DiscoveryWorker final : public QObject {
    Q_OBJECT
public:
    V4L2DiscoveryWorker(quint64 requestId, std::shared_ptr<IV4L2DiscoverySystem> system,
                        std::shared_ptr<std::atomic_bool> cancelRequested)
        : m_requestId(requestId), m_system(std::move(system)), m_cancelRequested(std::move(cancelRequested)) {}

public slots:
    void run() {
        QByteArray payload;
        try {
            QString errorCode;
            const QList<QString> paths = m_system->listVideoDevicePaths(errorCode);
            if (!errorCode.isEmpty()) {
                payload = QJsonDocument(errorPayload(m_requestId, errorCode)).toJson(QJsonDocument::Compact);
            } else {
                QJsonArray cameras;
                bool cancelled = false;
                for (const QString& path : paths) {
                    if (m_cancelRequested->load(std::memory_order_relaxed)) {
                        cancelled = true;
                        break;
                    }
                    bool isCharacterDevice = false;
                    errorCode.clear();
                    if (!m_system->statDevice(path, isCharacterDevice, errorCode) || !isCharacterDevice)
                        continue;
                    if (m_cancelRequested->load(std::memory_order_relaxed)) {
                        cancelled = true;
                        break;
                    }
                    errorCode.clear();
                    const int fd = m_system->openDevice(path, errorCode);
                    if (fd < 0)
                        continue;
                    ScopedFd guard(m_system, fd);
                    if (m_cancelRequested->load(std::memory_order_relaxed)) {
                        cancelled = true;
                        break;
                    }
                    V4L2DeviceInfo device;
                    errorCode.clear();
                    const bool queriedCapabilities = m_system->queryCapabilities(fd, device, errorCode);
                    if (m_cancelRequested->load(std::memory_order_relaxed)) {
                        cancelled = true;
                        break;
                    }
                    if (!queriedCapabilities || !device.isV4L2)
                        continue;
                    errorCode.clear();
                    m_system->queryFormat(fd, device, errorCode);
                    if (m_cancelRequested->load(std::memory_order_relaxed)) {
                        cancelled = true;
                        break;
                    }
                    cameras.append(cameraPayload(path, device));
                }
                if (cancelled || m_cancelRequested->load(std::memory_order_relaxed)) {
                    payload = QJsonDocument(errorPayload(m_requestId, QStringLiteral("CAMERA_DISCOVERY_CANCELLED")))
                                  .toJson(QJsonDocument::Compact);
                } else {
                    payload = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                                        {QStringLiteral("requestId"), QString::number(m_requestId)},
                                                        {QStringLiteral("status"), QStringLiteral("success")},
                                                        {QStringLiteral("cameras"), cameras}})
                                  .toJson(QJsonDocument::Compact);
                }
            }
        } catch (const std::exception&) {
            payload = QJsonDocument(errorPayload(m_requestId, QStringLiteral("CAMERA_DISCOVERY_BACKEND_FAILED")))
                          .toJson(QJsonDocument::Compact);
        } catch (...) {
            payload = QJsonDocument(errorPayload(m_requestId, QStringLiteral("CAMERA_DISCOVERY_BACKEND_FAILED")))
                          .toJson(QJsonDocument::Compact);
        }
        emit resultReady(payload);
        QThread::currentThread()->quit();
    }

signals:
    void resultReady(const QByteArray& payload);

private:
    quint64 m_requestId;
    std::shared_ptr<IV4L2DiscoverySystem> m_system;
    std::shared_ptr<std::atomic_bool> m_cancelRequested;
};

V4L2DiscoveryJob::V4L2DiscoveryJob(quint64 requestId, std::shared_ptr<IV4L2DiscoverySystem> system,
                                   std::shared_ptr<std::atomic_bool> cancelRequested, QObject* parent)
    : CameraDiscoveryJob(parent), m_requestId(requestId), m_system(std::move(system)),
      m_cancelRequested(std::move(cancelRequested)), m_thread(new QThread(this)),
      m_worker(new V4L2DiscoveryWorker(m_requestId, m_system, m_cancelRequested)) {
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &V4L2DiscoveryWorker::run, Qt::QueuedConnection);
    connect(m_worker, &V4L2DiscoveryWorker::resultReady, this, &V4L2DiscoveryJob::onWorkerResult, Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, this, &V4L2DiscoveryJob::onWorkerThreadFinished, Qt::QueuedConnection);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    QTimer::singleShot(0, this, &V4L2DiscoveryJob::startWorker);
}

V4L2DiscoveryJob::~V4L2DiscoveryJob() {
    if (m_thread && m_thread->isRunning()) {
        m_cancelRequested->store(true, std::memory_order_relaxed);
        m_thread->quit();
        m_thread->wait();
    }
}

void V4L2DiscoveryJob::startWorker() {
    if (!m_thread->isRunning())
        m_thread->start();
}

void V4L2DiscoveryJob::onWorkerResult(const QByteArray& payload) {
    m_payload = payload;
    m_hasPayload = true;
    publishWhenDrained();
}

void V4L2DiscoveryJob::onWorkerThreadFinished() {
    m_threadFinished = true;
    publishWhenDrained();
}

void V4L2DiscoveryJob::publishWhenDrained() {
    if (m_hasPayload && m_threadFinished)
        completeOnce(m_requestId, m_payload);
}

} // namespace DeepLux

#include "V4L2DiscoveryJob.moc"
