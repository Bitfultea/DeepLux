#pragma once

#include "CameraParameterWrite.h"
#include "core/interface/ICamera.h"

#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QSharedPointer>
#include <QThread>
#include <QTimer>

namespace DeepLux {

struct CameraCaptureFrameState {
    QMutex mutex;
    quint64 requestId = 0;
    quint64 generation = 0;
    bool accepting = false;
    bool received = false;
    bool cancelRequested = false;
    QString cancelErrorCode;
    QImage image;
};

class CameraOperationSession final : public QObject {
    Q_OBJECT

public:
    CameraOperationSession(QString deviceId, QString pluginId, quint64 generation, ICamera* camera,
                           QSharedPointer<CameraParameterBinding> parameterBinding, QObject* parent);
    ~CameraOperationSession() override;

    bool startConnect(quint64 requestId, int timeoutMs);
    bool startDisconnect(quint64 requestId, int timeoutMs);
    bool requestDetails(quint64 requestId);
    bool requestSetParameter(quint64 requestId, quint64 expectedBindingVersion, const QString& serialNumber,
                             const QString& parameter, const QJsonValue& value);
    bool requestCapture(quint64 requestId, double exposureTime, double gain);
    void cancelCapture(quint64 requestId, const QString& errorCode);
    void cancelConnectForShutdown();
    bool isBusy() const;
    bool isConnected() const;
    bool hasCamera() const;
    QString deviceId() const;
    QString pluginId() const;
    quint64 generation() const;

signals:
    void operationFinished(const QString& deviceId, const QString& pluginId, quint64 generation, quint64 requestId,
                           const QString& operation, const QString& status, const QString& errorCode, bool retained);
    void detailsQueryFinished(const QString& deviceId, const QString& pluginId, quint64 generation, quint64 requestId,
                              const QString& status, const QString& errorCode, const QJsonObject& details);
    void parameterWriteFinished(const QString& deviceId, const QString& pluginId, quint64 generation, quint64 requestId,
                                const QJsonObject& result);
    void captureFinished(const QString& deviceId, const QString& pluginId, quint64 generation, quint64 requestId,
                         const QImage& image, const QString& errorCode, const QJsonObject& diagnostics);
    void captureSessionRetired(const QString& deviceId, const QString& pluginId, quint64 generation);
    void staleDisconnectWorkerIdle(const QString& deviceId, const QString& pluginId, quint64 generation);

private:
    enum class State {
        Created,
        Connecting,
        Connected,
        ReadingDetails,
        WritingParameter,
        Capturing,
        Error,
        Disconnecting,
        Retiring,
        Retired
    };

    void onConnectFinished(quint64 requestId, bool connectSucceeded, bool connected, bool stateKnown,
                           const QString& errorCode);
    void onConnectTimeout();
    void onDisconnectFinished(quint64 requestId, bool disconnected, const QString& errorCode, bool cancelledConnect);
    void onDisconnectTimeout();
    void onDetailsFinished(quint64 requestId, const QString& status, const QString& errorCode,
                           const QJsonObject& details);
    void onParameterWriteFinished(quint64 requestId, const QJsonObject& result);
    void onCaptureTriggerFinished(quint64 requestId, const QString& errorCode, bool startedByCapture,
                                  const QJsonObject& diagnostics);
    void onCaptureImageAvailable(quint64 requestId, quint64 generation, const QImage& image);
    void pollCaptureFrame();
    void onCaptureCleanupFinished(quint64 requestId, const QString& errorCode, bool retireSession, const QImage& image,
                                  const QString& captureError, const QJsonObject& diagnostics);
    void maybeCompleteCapture(quint64 requestId);
    void queueCaptureCleanup(quint64 requestId, bool retireSession, const QString& captureError,
                             const QJsonObject& diagnostics);
    void retire(quint64 requestId, const QString& operation, const QString& status, const QString& errorCode);
    void maybeFinishRetirement();
    void queueDisconnect(quint64 requestId, bool cancelledConnect, int timeoutMs);

    QString m_deviceId;
    QString m_pluginId;
    quint64 m_generation = 0;
    QPointer<ICamera> m_camera;
    QSharedPointer<CameraParameterBinding> m_parameterBinding;
    QThread* m_thread = nullptr;
    QTimer* m_connectTimeoutTimer = nullptr;
    QTimer* m_disconnectTimeoutTimer = nullptr;
    QTimer* m_capturePollTimer = nullptr;
    State m_state = State::Created;
    quint64 m_activeRequestId = 0;
    QString m_activeOperation;
    bool m_cancelConnectOnCompletion = false;
    bool m_disconnectWorkerActive = false;
    quint64 m_captureRequestId = 0;
    bool m_captureTriggerFinished = false;
    bool m_captureImageReceived = false;
    bool m_captureWorkerCallActive = false;
    bool m_captureStartedAcquisition = false;
    bool m_captureExpired = false;
    bool m_captureCleanupQueued = false;
    QMetaObject::Connection m_captureImageConnection;
    QSharedPointer<CameraCaptureFrameState> m_captureFrameState;
    QString m_captureError;
    QImage m_captureImage;
    QJsonObject m_captureDiagnostics;
    QTimer* m_captureTimer = nullptr;
    bool m_cameraDestroyed = false;
    bool m_threadFinished = false;
    QString m_retireStatus;
    QString m_retireErrorCode;
    QString m_retireOperation;
    quint64 m_retireRequestId = 0;
};

} // namespace DeepLux
