#include "CameraOperationSession.h"

#include "CameraParameterWrite.h"
#include "core/interface/ICameraParameterControl.h"

#include <QJsonArray>
#include <QMetaObject>
#include <QMutexLocker>
#include <cmath>

namespace DeepLux {

namespace {

QString exceptionCode() {
    return QStringLiteral("CAMERA_OPERATION_EXCEPTION");
}

QString cleanupCamera(ICamera* camera) {
    try {
        if (camera->isAcquiring())
            camera->stopAcquisition();
        if (camera->isAcquiring())
            return QStringLiteral("CAMERA_STOP_NOT_CONFIRMED");
        if (camera->isConnected()) {
            try {
                camera->disconnect();
            } catch (...) {
                return QStringLiteral("CAMERA_DISCONNECT_UNCONFIRMED");
            }
        }
        if (camera->isConnected())
            return QStringLiteral("CAMERA_DISCONNECT_NOT_CONFIRMED");
    } catch (...) {
        return exceptionCode();
    }
    return {};
}

} // namespace

CameraOperationSession::CameraOperationSession(QString deviceId, QString pluginId, quint64 generation, ICamera* camera,
                                               QSharedPointer<CameraParameterBinding> parameterBinding, QObject* parent)
    : QObject(parent), m_deviceId(std::move(deviceId)), m_pluginId(std::move(pluginId)), m_generation(generation),
      m_camera(camera), m_parameterBinding(std::move(parameterBinding)), m_thread(new QThread(this)),
      m_connectTimeoutTimer(new QTimer(this)), m_disconnectTimeoutTimer(new QTimer(this)),
      m_capturePollTimer(new QTimer(this)) {
    m_connectTimeoutTimer->setSingleShot(true);
    connect(m_connectTimeoutTimer, &QTimer::timeout, this, &CameraOperationSession::onConnectTimeout);
    m_disconnectTimeoutTimer->setSingleShot(true);
    connect(m_disconnectTimeoutTimer, &QTimer::timeout, this, &CameraOperationSession::onDisconnectTimeout);
    m_capturePollTimer->setInterval(10);
    connect(m_capturePollTimer, &QTimer::timeout, this, &CameraOperationSession::pollCaptureFrame);
    connect(m_thread, &QThread::finished, this, [this]() {
        m_threadFinished = true;
        maybeFinishRetirement();
    });
    connect(m_thread, &QThread::finished, camera, &QObject::deleteLater);
    connect(camera, &QObject::destroyed, this, [this]() {
        m_camera.clear();
        m_cameraDestroyed = true;
        maybeFinishRetirement();
    });
}

CameraOperationSession::~CameraOperationSession() {
    Q_ASSERT(m_threadFinished || !m_thread->isRunning());
}

bool CameraOperationSession::startConnect(quint64 requestId, int timeoutMs) {
    if (m_state != State::Created || !m_camera || QThread::currentThread() != thread() ||
        m_camera->thread() != thread() || m_camera->parent())
        return false;

    m_activeRequestId = requestId;
    m_activeOperation = QStringLiteral("connect");
    m_state = State::Connecting;
    m_camera->moveToThread(m_thread);
    if (m_camera->thread() != m_thread) {
        m_state = State::Created;
        return false;
    }

    ICamera* camera = m_camera;
    m_thread->start();
    m_connectTimeoutTimer->start(qMax(1, timeoutMs));
    QMetaObject::invokeMethod(
        camera,
        [this, camera, requestId]() {
            bool connectSucceeded = false;
            bool connectionKnown = false;
            bool connected = false;
            QString errorCode;
            try {
                connectSucceeded = camera->connect();
                connected = camera->isConnected();
                connectionKnown = true;
                if (!connectSucceeded || !connected)
                    errorCode = QStringLiteral("CAMERA_CONNECT_NOT_CONFIRMED");
            } catch (...) {
                errorCode = exceptionCode();
                try {
                    connected = camera->isConnected();
                    connectionKnown = true;
                } catch (...) {
                    connectionKnown = false;
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, requestId, connectSucceeded, connected, connectionKnown, errorCode]() {
                    onConnectFinished(requestId, connectSucceeded, connected, connectionKnown, errorCode);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    return true;
}

bool CameraOperationSession::startDisconnect(quint64 requestId, int timeoutMs) {
    if ((m_state != State::Connected && m_state != State::Error) || !m_camera || isBusy() ||
        QThread::currentThread() != thread())
        return false;
    queueDisconnect(requestId, false, timeoutMs);
    return true;
}

bool CameraOperationSession::requestDetails(quint64 requestId) {
    if (m_state != State::Connected || !m_camera || requestId == 0 || QThread::currentThread() != thread())
        return false;

    m_activeRequestId = requestId;
    m_activeOperation = QStringLiteral("get_details");
    m_state = State::ReadingDetails;
    ICamera* camera = m_camera;
    const bool queued = QMetaObject::invokeMethod(
        camera,
        [this, camera, requestId]() {
            QString status = QStringLiteral("success");
            QString errorCode;
            QJsonObject details;
            try {
                if (!camera->isConnected()) {
                    status = QStringLiteral("error");
                    errorCode = QStringLiteral("CAMERA_NOT_CONNECTED");
                } else {
                    const CameraCapabilities capabilities = camera->capabilities();
                    const double exposureTime = camera->exposureTime();
                    const double gain = camera->gain();
                    const double frameRate = camera->frameRate();
                    if (!std::isfinite(exposureTime) || !std::isfinite(gain) || !std::isfinite(frameRate)) {
                        status = QStringLiteral("error");
                        errorCode = QStringLiteral("CAMERA_DETAILS_INVALID_VALUE");
                    } else {
                        const QRect roi = camera->roi();
                        const TriggerMode triggerMode = camera->triggerMode();
                        QString triggerModeName;
                        switch (triggerMode) {
                        case TriggerMode::Software:
                            triggerModeName = QStringLiteral("software");
                            break;
                        case TriggerMode::Hardware:
                            triggerModeName = QStringLiteral("hardware");
                            break;
                        case TriggerMode::Continuous:
                            triggerModeName = QStringLiteral("continuous");
                            break;
                        default:
                            triggerModeName = QStringLiteral("unknown");
                            break;
                        }
                        details = {
                            {QStringLiteral("deviceId"), camera->deviceId()},
                            {QStringLiteral("name"), camera->name()},
                            {QStringLiteral("serialNumber"), camera->serialNumber()},
                            {QStringLiteral("manufacturer"), camera->manufacturer()},
                            {QStringLiteral("capabilities"),
                             QJsonObject{{QStringLiteral("softwareTrigger"), capabilities.softwareTrigger},
                                         {QStringLiteral("hardwareTrigger"), capabilities.hardwareTrigger},
                                         {QStringLiteral("continuousMode"), capabilities.continuousMode},
                                         {QStringLiteral("exposureControl"), capabilities.exposureControl},
                                         {QStringLiteral("gainControl"), capabilities.gainControl},
                                         {QStringLiteral("frameRateControl"), capabilities.frameRateControl},
                                         {QStringLiteral("roiControl"), capabilities.roiControl},
                                         {QStringLiteral("pixelFormatControl"), capabilities.pixelFormatControl},
                                         {QStringLiteral("whiteBalance"), capabilities.whiteBalance},
                                         {QStringLiteral("depthData"), capabilities.depthData},
                                         {QStringLiteral("supportedOnWindows"), capabilities.supportedOnWindows},
                                         {QStringLiteral("supportedOnLinux"), capabilities.supportedOnLinux}}},
                            {QStringLiteral("parameters"),
                             QJsonObject{{QStringLiteral("triggerMode"), triggerModeName},
                                         {QStringLiteral("exposureTime"), exposureTime},
                                         {QStringLiteral("gain"), gain},
                                         {QStringLiteral("frameRate"), frameRate},
                                         {QStringLiteral("roi"), QJsonObject{{QStringLiteral("x"), roi.x()},
                                                                             {QStringLiteral("y"), roi.y()},
                                                                             {QStringLiteral("width"), roi.width()},
                                                                             {QStringLiteral("height"), roi.height()}}},
                                         {QStringLiteral("imageWidth"), camera->imageWidth()},
                                         {QStringLiteral("imageHeight"), camera->imageHeight()}}}};
                    }
                }
            } catch (...) {
                status = QStringLiteral("error");
                errorCode = QStringLiteral("CAMERA_DETAILS_READ_FAILED");
                details = {};
            }
            QMetaObject::invokeMethod(
                this,
                [this, requestId, status, errorCode, details]() {
                    onDetailsFinished(requestId, status, errorCode, details);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    if (!queued) {
        m_state = State::Connected;
        m_activeRequestId = 0;
        m_activeOperation.clear();
        return false;
    }
    return true;
}

bool CameraOperationSession::requestSetParameter(quint64 requestId, quint64 expectedBindingVersion,
                                                 const QString& serialNumber, const QString& parameter,
                                                 const QJsonValue& value) {
    if (m_state != State::Connected || !m_camera || requestId == 0 || QThread::currentThread() != thread())
        return false;
    m_activeRequestId = requestId;
    m_activeOperation = QStringLiteral("set_parameter");
    m_state = State::WritingParameter;
    ICamera* camera = m_camera;
    const bool queued = QMetaObject::invokeMethod(
        camera,
        [this, camera, requestId, expectedBindingVersion, serialNumber, parameter, value]() {
            QJsonObject result{{QStringLiteral("operation"), QStringLiteral("set_parameter")},
                               {QStringLiteral("parameter"), parameter},
                               {QStringLiteral("requested"), value},
                               {QStringLiteral("retryable"), false},
                               {QStringLiteral("sideEffectStatus"), QStringLiteral("none")},
                               {QStringLiteral("setterCalled"), false}};
            CameraParameterWriteResult write;
            if (m_parameterBinding)
                write = m_parameterBinding->write(camera, expectedBindingVersion, m_deviceId, serialNumber, parameter,
                                                  value);
            else {
                write.parameter = parameter;
                write.requested = value;
                write.errorCode = QStringLiteral("CAMERA_PARAMETER_BINDING_UNAVAILABLE");
            }
            result.insert(QStringLiteral("status"), write.status);
            result.insert(QStringLiteral("errorCode"), write.errorCode);
            result.insert(QStringLiteral("setterCalled"), write.setterCalled);
            result.insert(QStringLiteral("sideEffectStatus"), write.sideEffectStatus);
            if (!write.before.isUndefined())
                result.insert(QStringLiteral("before"), write.before);
            if (!write.actual.isUndefined())
                result.insert(QStringLiteral("actual"), write.actual);
            QMetaObject::invokeMethod(
                this, [this, requestId, result]() { onParameterWriteFinished(requestId, result); },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
    if (!queued) {
        m_state = State::Connected;
        m_activeRequestId = 0;
        m_activeOperation.clear();
        return false;
    }
    return true;
}

bool CameraOperationSession::requestCapture(quint64 requestId, double exposureTime, double gain) {
    if (m_state != State::Connected || !m_camera || requestId == 0 || QThread::currentThread() != thread() ||
        !std::isfinite(exposureTime) || !std::isfinite(gain))
        return false;

    m_state = State::Capturing;
    m_activeRequestId = requestId;
    m_activeOperation = QStringLiteral("capture");
    m_captureRequestId = requestId;
    m_captureTriggerFinished = false;
    m_captureImageReceived = false;
    m_captureWorkerCallActive = true;
    m_captureStartedAcquisition = false;
    m_captureExpired = false;
    m_captureCleanupQueued = false;
    m_captureError.clear();
    m_captureImage = {};
    m_captureDiagnostics = {};

    ICamera* camera = m_camera;
    m_captureFrameState = QSharedPointer<CameraCaptureFrameState>::create();
    m_captureFrameState->requestId = requestId;
    m_captureFrameState->generation = m_generation;
    const QWeakPointer<CameraCaptureFrameState> weakFrameState(m_captureFrameState);
    m_captureImageConnection = connect(
        camera, &ICamera::imageReceived, this,
        [weakFrameState, requestId, generation = m_generation](const QImage& image, const QVariantMap&) {
            if (image.isNull())
                return;
            const QImage frame = image.copy();
            const QSharedPointer<CameraCaptureFrameState> frameState = weakFrameState.toStrongRef();
            if (!frameState)
                return;
            QMutexLocker locker(&frameState->mutex);
            if (frameState->accepting && frameState->requestId == requestId && frameState->generation == generation &&
                !frameState->received) {
                frameState->image = frame;
                frameState->received = true;
            }
        },
        Qt::DirectConnection);
    m_capturePollTimer->start();
    const QJsonValue exposureValue(exposureTime);
    const QJsonValue gainValue(gain);
    const QSharedPointer<CameraCaptureFrameState> frameState = m_captureFrameState;
    const bool queued = QMetaObject::invokeMethod(
        camera,
        [this, camera, requestId, exposureValue, gainValue, frameState]() {
            QString errorCode;
            QJsonObject diagnostics;
            bool startedAcquisition = false;
            QJsonArray completedParameters;
            auto cancellationCode = [frameState]() {
                QMutexLocker locker(&frameState->mutex);
                return frameState->cancelRequested ? frameState->cancelErrorCode : QString();
            };

            try {
                if (!cancellationCode().isEmpty()) {
                    errorCode = cancellationCode();
                } else if (!camera->isConnected()) {
                    errorCode = QStringLiteral("CAMERA_NOT_CONNECTED");
                } else if (!camera->capabilities().softwareTrigger) {
                    errorCode = QStringLiteral("CAMERA_PARAMETER_UNSUPPORTED");
                } else {
                    auto* control = qobject_cast<ICameraParameterControl*>(camera);
                    if (!camera->capabilities().exposureControl || !camera->capabilities().gainControl || !control) {
                        errorCode = QStringLiteral("CAMERA_PARAMETER_UNSUPPORTED");
                    } else {
                        auto validateBeforeWrite = [&](const QString& parameter, const QJsonValue& requested) {
                            QString validationError;
                            bool valid = false;
                            try {
                                valid = control->validateParameter(parameter, requested, &validationError);
                            } catch (...) {
                                errorCode = QStringLiteral("CAMERA_PARAMETER_VALIDATE_FAILED");
                            }
                            if (errorCode.isEmpty() && !valid) {
                                errorCode = validationError == QLatin1String("CAMERA_PARAMETER_UNSUPPORTED")
                                                ? validationError
                                                : QStringLiteral("CAMERA_PARAMETER_OUT_OF_RANGE");
                            }
                            if (!errorCode.isEmpty()) {
                                diagnostics.insert(QStringLiteral("failedParameter"), parameter);
                                diagnostics.insert(QStringLiteral("requested"), requested);
                                diagnostics.insert(QStringLiteral("sideEffectStatus"), QStringLiteral("none"));
                            }
                        };
                        validateBeforeWrite(QStringLiteral("exposureTime"), exposureValue);
                        if (errorCode.isEmpty())
                            validateBeforeWrite(QStringLiteral("gain"), gainValue);
                    }
                }

                if (!errorCode.isEmpty() && diagnostics.contains(QStringLiteral("failedParameter"))) {
                    const QString failedParameter = diagnostics.value(QStringLiteral("failedParameter")).toString();
                    try {
                        const double before =
                            failedParameter == QLatin1String("exposureTime") ? camera->exposureTime() : camera->gain();
                        diagnostics.insert(QStringLiteral("before"), before);
                        diagnostics.insert(QStringLiteral("actual"), before);
                    } catch (...) {
                        // The validation error remains authoritative; readback is best effort.
                    }
                }

                if (errorCode.isEmpty()) {
                    for (const auto& parameter : {qMakePair(QStringLiteral("exposureTime"), exposureValue),
                                                  qMakePair(QStringLiteral("gain"), gainValue)}) {
                        errorCode = cancellationCode();
                        if (!errorCode.isEmpty())
                            break;
                        CameraParameterWriteResult write =
                            m_parameterBinding
                                ? m_parameterBinding->write(camera, m_parameterBinding->generation(), m_deviceId,
                                                            camera->serialNumber(), parameter.first, parameter.second)
                                : writeCameraParameter(camera, m_deviceId, camera->serialNumber(), parameter.first,
                                                       parameter.second);
                        if (!write.errorCode.isEmpty()) {
                            errorCode = write.errorCode;
                            diagnostics.insert(QStringLiteral("failedParameter"), parameter.first);
                            diagnostics.insert(QStringLiteral("before"), write.before);
                            diagnostics.insert(QStringLiteral("requested"), write.requested);
                            if (!write.actual.isUndefined())
                                diagnostics.insert(QStringLiteral("actual"), write.actual);
                            diagnostics.insert(QStringLiteral("sideEffectStatus"), write.sideEffectStatus);
                            break;
                        }
                        completedParameters.append(
                            QJsonObject{{QStringLiteral("parameter"), parameter.first},
                                        {QStringLiteral("before"), write.before},
                                        {QStringLiteral("requested"), write.requested},
                                        {QStringLiteral("actual"), write.actual},
                                        {QStringLiteral("sideEffectStatus"), write.sideEffectStatus}});
                    }
                }
                diagnostics.insert(QStringLiteral("completedParameters"), completedParameters);
                if (errorCode.isEmpty()) {
                    errorCode = cancellationCode();
                    if (errorCode.isEmpty()) {
                        camera->setTriggerMode(TriggerMode::Software);
                        if (!camera->isAcquiring()) {
                            if (!camera->startAcquisition())
                                errorCode = QStringLiteral("CAMERA_TRIGGER_FAILED");
                            else
                                startedAcquisition = true;
                        }
                    }
                }
                if (errorCode.isEmpty()) {
                    errorCode = cancellationCode();
                    if (errorCode.isEmpty()) {
                        QMutexLocker locker(&frameState->mutex);
                        if (frameState->cancelRequested)
                            errorCode = frameState->cancelErrorCode;
                        else
                            frameState->accepting = true;
                    }
                    if (errorCode.isEmpty() && !camera->triggerSoftware())
                        errorCode = QStringLiteral("CAMERA_TRIGGER_FAILED");
                }
            } catch (...) {
                errorCode = QStringLiteral("CAMERA_TRIGGER_FAILED");
                diagnostics.insert(QStringLiteral("exception"), true);
            }

            QMetaObject::invokeMethod(
                this,
                [this, requestId, errorCode, startedAcquisition, diagnostics]() {
                    onCaptureTriggerFinished(requestId, errorCode, startedAcquisition, diagnostics);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);

    if (!queued) {
        QObject::disconnect(m_captureImageConnection);
        m_captureImageConnection = {};
        m_capturePollTimer->stop();
        if (m_captureFrameState) {
            QMutexLocker locker(&m_captureFrameState->mutex);
            m_captureFrameState->accepting = false;
        }
        m_captureFrameState.clear();
        m_state = State::Connected;
        m_activeRequestId = 0;
        m_activeOperation.clear();
        m_captureRequestId = 0;
        m_captureWorkerCallActive = false;
        return false;
    }
    return true;
}

void CameraOperationSession::cancelCapture(quint64 requestId, const QString& errorCode) {
    if (QThread::currentThread() != thread() || m_state != State::Capturing || requestId != m_captureRequestId)
        return;
    m_captureExpired = true;
    if (m_captureError.isEmpty())
        m_captureError = errorCode.isEmpty() ? QStringLiteral("CAMERA_GRAB_CANCELLED") : errorCode;
    if (m_captureFrameState) {
        QMutexLocker locker(&m_captureFrameState->mutex);
        m_captureFrameState->cancelRequested = true;
        m_captureFrameState->cancelErrorCode = m_captureError;
        m_captureFrameState->accepting = false;
    }
    if (!m_captureWorkerCallActive)
        queueCaptureCleanup(requestId, true, m_captureError, m_captureDiagnostics);
}

void CameraOperationSession::onCaptureTriggerFinished(quint64 requestId, const QString& errorCode,
                                                      bool startedByCapture, const QJsonObject& diagnostics) {
    if (m_state != State::Capturing || requestId != m_captureRequestId)
        return;
    m_captureWorkerCallActive = false;
    m_captureTriggerFinished = true;
    m_captureStartedAcquisition = startedByCapture;
    m_captureDiagnostics = diagnostics;
    if (!errorCode.isEmpty() && m_captureError.isEmpty())
        m_captureError = errorCode;
    if (!m_captureError.isEmpty() || m_captureExpired) {
        queueCaptureCleanup(requestId, true,
                            m_captureError.isEmpty() ? QStringLiteral("CAMERA_GRAB_CANCELLED") : m_captureError,
                            m_captureDiagnostics);
        return;
    }
    maybeCompleteCapture(requestId);
}

void CameraOperationSession::onCaptureImageAvailable(quint64 requestId, quint64 generation, const QImage& image) {
    if (generation != m_generation || m_state != State::Capturing || requestId != m_captureRequestId ||
        image.isNull() || m_captureExpired || m_captureCleanupQueued || m_captureImageReceived)
        return;
    m_captureImage = image.copy();
    m_captureImageReceived = true;
    maybeCompleteCapture(requestId);
}

void CameraOperationSession::pollCaptureFrame() {
    if (m_state != State::Capturing || !m_captureFrameState || m_captureImageReceived || m_captureExpired ||
        m_captureCleanupQueued)
        return;
    QImage image;
    {
        QMutexLocker locker(&m_captureFrameState->mutex);
        if (!m_captureFrameState->received || m_captureFrameState->requestId != m_captureRequestId ||
            m_captureFrameState->generation != m_generation)
            return;
        image = m_captureFrameState->image;
        m_captureFrameState->received = false;
    }
    onCaptureImageAvailable(m_captureRequestId, m_generation, image);
}

void CameraOperationSession::maybeCompleteCapture(quint64 requestId) {
    if (m_state != State::Capturing || requestId != m_captureRequestId || !m_captureTriggerFinished ||
        !m_captureImageReceived || m_captureWorkerCallActive || m_captureCleanupQueued)
        return;
    // ICamera::imageReceived has no request identifier. Retire this session after one frame so a late
    // duplicate frame cannot be attributed to a subsequent capture request.
    queueCaptureCleanup(requestId, true, {}, m_captureDiagnostics);
}

void CameraOperationSession::queueCaptureCleanup(quint64 requestId, bool retireSession, const QString& captureError,
                                                 const QJsonObject& diagnostics) {
    if (m_state != State::Capturing || requestId != m_captureRequestId || m_captureCleanupQueued ||
        m_captureWorkerCallActive || !m_camera)
        return;
    m_captureCleanupQueued = true;
    QObject::disconnect(m_captureImageConnection);
    m_captureImageConnection = {};
    m_capturePollTimer->stop();
    if (m_captureFrameState) {
        QMutexLocker locker(&m_captureFrameState->mutex);
        m_captureFrameState->accepting = false;
    }
    ICamera* camera = m_camera;
    const bool stopAcquisition =
        retireSession || m_captureStartedAcquisition || m_captureExpired || !captureError.isEmpty();
    const QImage image = m_captureImage;
    QMetaObject::invokeMethod(
        camera,
        [this, camera, requestId, retireSession, captureError, diagnostics, stopAcquisition, image]() {
            QString cleanupError;
            try {
                if (stopAcquisition && camera->isAcquiring())
                    camera->stopAcquisition();
                if (stopAcquisition && camera->isAcquiring())
                    cleanupError = QStringLiteral("CAMERA_STOP_NOT_CONFIRMED");
                if (cleanupError.isEmpty() && retireSession && camera->isConnected())
                    camera->disconnect();
                if (cleanupError.isEmpty() && retireSession && camera->isConnected())
                    cleanupError = QStringLiteral("CAMERA_DISCONNECT_NOT_CONFIRMED");
            } catch (...) {
                cleanupError = QStringLiteral("CAMERA_OPERATION_EXCEPTION");
            }
            QMetaObject::invokeMethod(
                this,
                [this, requestId, retireSession, cleanupError, image, captureError, diagnostics]() {
                    onCaptureCleanupFinished(requestId, cleanupError, retireSession, image, captureError, diagnostics);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void CameraOperationSession::onCaptureCleanupFinished(quint64 requestId, const QString& errorCode, bool retireSession,
                                                      const QImage& image, const QString& captureError,
                                                      const QJsonObject& diagnostics) {
    if (m_state != State::Capturing || requestId != m_captureRequestId)
        return;
    const QString finalError = !errorCode.isEmpty() ? errorCode : captureError;
    emit captureFinished(m_deviceId, m_pluginId, m_generation, requestId, finalError.isEmpty() ? image : QImage(),
                         finalError, diagnostics);
    m_captureRequestId = 0;
    m_captureImage = {};
    m_captureImageReceived = false;
    m_captureTriggerFinished = false;
    m_captureWorkerCallActive = false;
    m_captureCleanupQueued = false;
    m_captureExpired = false;
    m_captureStartedAcquisition = false;
    m_captureError.clear();
    m_captureDiagnostics = {};
    m_captureFrameState.clear();
    if (retireSession && errorCode.isEmpty()) {
        retire(requestId, QStringLiteral("capture"),
               finalError.isEmpty() ? QStringLiteral("success") : QStringLiteral("error"), finalError);
    } else {
        m_state = errorCode.isEmpty() ? State::Connected : State::Error;
        m_activeRequestId = 0;
        m_activeOperation.clear();
    }
}

void CameraOperationSession::cancelConnectForShutdown() {
    if (m_state == State::Connecting)
        m_cancelConnectOnCompletion = true;
}

bool CameraOperationSession::isBusy() const {
    return m_state == State::Connecting || m_state == State::ReadingDetails || m_state == State::WritingParameter ||
           m_state == State::Capturing || m_state == State::Disconnecting || m_state == State::Retiring ||
           m_disconnectWorkerActive;
}

bool CameraOperationSession::isConnected() const {
    return m_state == State::Connected;
}

bool CameraOperationSession::hasCamera() const {
    return !m_camera.isNull();
}

QString CameraOperationSession::deviceId() const {
    return m_deviceId;
}

QString CameraOperationSession::pluginId() const {
    return m_pluginId;
}

quint64 CameraOperationSession::generation() const {
    return m_generation;
}

void CameraOperationSession::onConnectFinished(quint64 requestId, bool connectSucceeded, bool connected,
                                               bool stateKnown, const QString& errorCode) {
    if (m_state != State::Connecting || requestId != m_activeRequestId)
        return;
    m_connectTimeoutTimer->stop();
    if (m_cancelConnectOnCompletion) {
        queueDisconnect(requestId, true, 0);
        return;
    }
    if (connectSucceeded && connected) {
        m_state = State::Connected;
        emit operationFinished(m_deviceId, m_pluginId, m_generation, requestId, QStringLiteral("connect"),
                               QStringLiteral("success"), {}, true);
    } else if (!connectSucceeded && connected) {
        m_state = State::Error;
        const QString finalErrorCode = errorCode == QLatin1String("CAMERA_CONNECT_NOT_CONFIRMED")
                                           ? QStringLiteral("CAMERA_CONNECT_UNCONFIRMED")
                                           : errorCode;
        emit operationFinished(m_deviceId, m_pluginId, m_generation, requestId, QStringLiteral("connect"),
                               QStringLiteral("error"), finalErrorCode, true);
    } else if (stateKnown && !connected) {
        m_state = State::Disconnecting;
        ICamera* camera = m_camera;
        QMetaObject::invokeMethod(
            camera,
            [this, camera, requestId, errorCode]() {
                const QString cleanupError = cleanupCamera(camera);
                QMetaObject::invokeMethod(
                    this,
                    [this, requestId, errorCode, cleanupError]() {
                        if (cleanupError.isEmpty())
                            retire(requestId, QStringLiteral("connect"), QStringLiteral("error"), errorCode);
                        else {
                            m_state = State::Error;
                            emit operationFinished(m_deviceId, m_pluginId, m_generation, requestId,
                                                   QStringLiteral("connect"), QStringLiteral("error"), cleanupError,
                                                   true);
                        }
                    },
                    Qt::QueuedConnection);
            },
            Qt::QueuedConnection);
    } else {
        m_state = State::Error;
        emit operationFinished(m_deviceId, m_pluginId, m_generation, requestId, QStringLiteral("connect"),
                               QStringLiteral("error"), errorCode, true);
    }
}

void CameraOperationSession::onConnectTimeout() {
    if (m_state != State::Connecting || m_activeRequestId == 0)
        return;
    const quint64 requestId = m_activeRequestId;
    m_state = State::Error;
    m_activeRequestId = 0;
    m_activeOperation.clear();
    emit operationFinished(m_deviceId, m_pluginId, m_generation, requestId, QStringLiteral("connect"),
                           QStringLiteral("error"), QStringLiteral("CAMERA_CONNECT_TIMEOUT_UNCONFIRMED"), true);
}

void CameraOperationSession::onDisconnectFinished(quint64 requestId, bool disconnected, const QString& errorCode,
                                                  bool cancelledConnect) {
    m_disconnectWorkerActive = false;
    m_disconnectTimeoutTimer->stop();
    if (m_state != State::Disconnecting || requestId != m_activeRequestId) {
        emit staleDisconnectWorkerIdle(m_deviceId, m_pluginId, m_generation);
        return;
    }
    if (!disconnected) {
        m_state = State::Error;
        const QString operation = cancelledConnect ? QStringLiteral("connect") : QStringLiteral("disconnect");
        const QString status = cancelledConnect ? QStringLiteral("cancelled") : QStringLiteral("error");
        emit operationFinished(m_deviceId, m_pluginId, m_generation, requestId, operation, status, errorCode, true);
        return;
    }
    const QString operation = cancelledConnect ? QStringLiteral("connect") : QStringLiteral("disconnect");
    const QString status = cancelledConnect ? QStringLiteral("cancelled") : QStringLiteral("success");
    const QString finalError = cancelledConnect ? QStringLiteral("CAMERA_OPERATION_CANCELLED") : QString();
    retire(requestId, operation, status, finalError);
}

void CameraOperationSession::onDisconnectTimeout() {
    if (m_state != State::Disconnecting || m_activeOperation != QLatin1String("disconnect") || m_activeRequestId == 0)
        return;
    const quint64 requestId = m_activeRequestId;
    m_state = State::Error;
    m_activeRequestId = 0;
    m_activeOperation.clear();
    emit operationFinished(m_deviceId, m_pluginId, m_generation, requestId, QStringLiteral("disconnect"),
                           QStringLiteral("error"), QStringLiteral("CAMERA_DISCONNECT_TIMEOUT_UNCONFIRMED"), true);
}

void CameraOperationSession::onDetailsFinished(quint64 requestId, const QString& status, const QString& errorCode,
                                               const QJsonObject& details) {
    if (m_state != State::ReadingDetails || requestId != m_activeRequestId)
        return;
    m_state = State::Connected;
    m_activeRequestId = 0;
    m_activeOperation.clear();
    emit detailsQueryFinished(m_deviceId, m_pluginId, m_generation, requestId, status, errorCode, details);
}

void CameraOperationSession::onParameterWriteFinished(quint64 requestId, const QJsonObject& result) {
    if (m_state != State::WritingParameter || requestId != m_activeRequestId)
        return;
    m_state = State::Connected;
    m_activeRequestId = 0;
    m_activeOperation.clear();
    emit parameterWriteFinished(m_deviceId, m_pluginId, m_generation, requestId, result);
}

void CameraOperationSession::retire(quint64 requestId, const QString& operation, const QString& status,
                                    const QString& errorCode) {
    if (!m_camera || !m_thread || m_state == State::Retiring || m_state == State::Retired)
        return;
    m_state = State::Retiring;
    m_retireRequestId = requestId;
    m_retireOperation = operation;
    m_retireStatus = status;
    m_retireErrorCode = errorCode;
    QThread* worker = m_thread;
    ICamera* camera = m_camera;
    QMetaObject::invokeMethod(
        camera, [worker]() { worker->quit(); }, Qt::QueuedConnection);
}

void CameraOperationSession::queueDisconnect(quint64 requestId, bool cancelledConnect, int timeoutMs) {
    if (!m_camera || !m_thread)
        return;
    m_state = State::Disconnecting;
    m_activeRequestId = requestId;
    m_activeOperation = cancelledConnect ? QStringLiteral("connect") : QStringLiteral("disconnect");
    m_disconnectWorkerActive = true;
    if (!cancelledConnect)
        m_disconnectTimeoutTimer->start(qMax(1, timeoutMs));
    ICamera* camera = m_camera;
    QMetaObject::invokeMethod(
        camera,
        [this, camera, requestId, cancelledConnect]() {
            bool disconnected = false;
            QString errorCode;
            try {
                errorCode = cleanupCamera(camera);
                disconnected = errorCode.isEmpty();
            } catch (...) {
                errorCode = exceptionCode();
            }
            QMetaObject::invokeMethod(
                this,
                [this, requestId, cancelledConnect, disconnected, errorCode]() {
                    onDisconnectFinished(requestId, disconnected, errorCode, cancelledConnect);
                },
                Qt::QueuedConnection);
        },
        Qt::QueuedConnection);
}

void CameraOperationSession::maybeFinishRetirement() {
    if (m_state != State::Retiring || !m_cameraDestroyed || !m_threadFinished)
        return;
    m_state = State::Retired;
    emit operationFinished(m_deviceId, m_pluginId, m_generation, m_retireRequestId, m_retireOperation, m_retireStatus,
                           m_retireErrorCode, false);
    if (m_retireOperation == QLatin1String("capture"))
        emit captureSessionRetired(m_deviceId, m_pluginId, m_generation);
}

} // namespace DeepLux
