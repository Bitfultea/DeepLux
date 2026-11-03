#pragma once

#include "../interface/ICameraPlugin.h"
#include "../platform/Platform.h"
#include "CameraParameterWrite.h"

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QSharedPointer>
#include <QString>
#include <QStringList>
#include <QWaitCondition>
#include <functional>
#include <limits>

namespace DeepLux {

class ICamera;
class ICameraPlugin;
class ICameraDiscoveryAsync;
class CameraDiscoveryJob;
class CameraOperationSession;
class CameraManager;

class DEEPLUX_API CameraLease {
public:
    CameraLease() = default;
    CameraLease(CameraLease&& other) noexcept;
    CameraLease& operator=(CameraLease&& other) noexcept;
    ~CameraLease();

    CameraLease(const CameraLease&) = delete;
    CameraLease& operator=(const CameraLease&) = delete;

    ICamera* get() const {
        return m_camera;
    }
    ICamera* operator->() const {
        return m_camera;
    }
    explicit operator bool() const {
        return m_camera != nullptr;
    }

private:
    friend class CameraManager;
    CameraLease(CameraManager* manager, QString deviceId, ICamera* camera);
    void reset();

    CameraManager* m_manager = nullptr;
    QString m_deviceId;
    ICamera* m_camera = nullptr;
};

/**
 * @brief 相机状态
 */
enum class CameraState { Disconnected, Connecting, Connected, Grabbing, Error };

/**
 * @brief 相机状态信息
 */
struct CameraStatus {
    QString deviceId;
    QString name;
    CameraState state = CameraState::Disconnected;
    QDateTime lastGrabTime;
    QString lastError;
};

struct CameraOperationSnapshot {
    quint64 requestId = 0;
    QString operation;
};

struct CameraCaptureResult {
    QImage image;
    QString errorCode;
    QJsonObject diagnostics;
    bool success() const {
        return errorCode.isEmpty() && !image.isNull();
    }
};

struct CameraCaptureWaitState {
    QMutex mutex;
    QWaitCondition condition;
    bool completed = false;
    CameraCaptureResult result;
};

/**
 * @brief 相机管理器 - 单例模式
 *
 * 管理所有相机插件和相机实例
 */
class DEEPLUX_API CameraManager : public QObject {
    Q_OBJECT

public:
    static CameraManager& instance();

    // ========== 插件管理 ==========

    void registerCameraPlugin(ICameraPlugin* plugin);
    void unregisterCameraPlugin(const QString& pluginId);
    bool tryUnregisterCameraPlugin(const QString& pluginId, QString* error = nullptr);
    QList<ICameraPlugin*> plugins() const;
    ICameraPlugin* findPlugin(const QString& pluginId) const;

    // ========== 相机发现 ==========

    QList<CameraStatus> discoverCameras();
    QList<CameraStatus> knownCameras();
    void refreshCameras();

    // Optional async discovery prototype. All three QObjects must share this manager's thread.
    bool startDiscoveryAsync(const QString& pluginId, quint64 requestId, QString* error = nullptr);
    QJsonObject cameraDiscoveryTargets() const;
    bool requestCameraDiscoveryBatchAsync(quint64 expectedRegistryGeneration, const QStringList& pluginIds,
                                          const QStringList& unsupportedPluginIds, quint64* requestId,
                                          QString* error = nullptr);
    bool getCameraDiscoveryOperationResult(quint64 requestId, QJsonObject* result) const;
    bool requestDiscoveryCancel(quint64 requestId, QString* error = nullptr);
    bool hasPendingDiscovery(quint64 requestId) const;
    int pendingDiscoveryCount() const;
    bool connectCameraAsync(const QString& deviceId, quint64 requestId, QString* error = nullptr);
    bool disconnectCameraAsync(const QString& deviceId, quint64 requestId, QString* error = nullptr);
    bool requestConnectCameraAsync(const QString& deviceId, quint64* requestId, QString* error = nullptr);
    bool requestConnectCameraAsync(const QString& deviceId, const QJsonObject& expectedTarget, quint64* requestId,
                                   QString* error = nullptr);
    bool requestDisconnectCameraAsync(const QString& deviceId, quint64* requestId, QString* error = nullptr);
    bool requestDisconnectCameraAsync(const QString& deviceId, const QJsonObject& expectedTarget, quint64* requestId,
                                      QString* error = nullptr);
    QJsonObject cameraLifecycleSnapshot(const QString& deviceId) const;
    bool requestCameraDetailsAsync(const QString& deviceId, quint64* requestId, QString* error = nullptr);
    bool requestCameraParameterWriteAsync(const QString& deviceId, quint64 expectedBindingVersion,
                                          const QString& serialNumber, const QString& parameter,
                                          const QJsonValue& value, quint64* requestId, QString* error = nullptr);
    bool getCameraOperationResult(quint64 requestId, QJsonObject* result) const;
    bool hasAsyncCameraSession(const QString& deviceId) const;
#ifdef DEEPLUX_ENABLE_TEST_HOOKS
    void setAsyncConnectTimeoutForTesting(int timeoutMs);
    void setAsyncDisconnectTimeoutForTesting(int timeoutMs);
#endif
    CameraCaptureResult captureAsyncSessionImage(const QString& deviceId, double exposureTime, double gain,
                                                 int timeoutMs, const std::function<bool()>& isCancelled = {});
    bool supportsAsyncOperations(const QString& deviceId) const;
    bool hasPendingCameraOperation(const QString& deviceId) const;
    bool pendingCameraOperation(const QString& deviceId, CameraOperationSnapshot* snapshot) const;
    int cameraSessionCount() const;
    void shutdownAsync();

    // ========== 连接管理 ==========

    bool connectCamera(const QString& deviceId);
    void disconnectCamera(const QString& deviceId);
    void disconnectAll();

    // ========== 相机访问 ==========

    // Borrowed pointers: callers must not retain them across event processing or concurrent manager mutation.
    ICamera* getCamera(const QString& deviceId) const;
    QList<ICamera*> allCameras() const;
    CameraLease acquireCamera(const QString& deviceId);

    // ========== 状态查询 ==========

    bool isConnected(const QString& deviceId) const;
    CameraState cameraState(const QString& deviceId) const;
    QString lastError(const QString& deviceId) const;
    quint64 cameraParameterBindingVersion(const QString& deviceId) const;
    quint64 invalidateCameraParameterBinding(const QString& deviceId);
    void noteCameraParameterChanged(const QString& deviceId);
    CameraParameterWriteResult writeCameraParameterIfCurrent(const QString& deviceId, quint64 expectedVersion,
                                                             ICamera* camera, const QString& serialNumber,
                                                             const QString& parameter, const QJsonValue& value);

signals:
    void pluginRegistered(const QString& pluginId);
    void pluginUnregistered(const QString& pluginId);
    void cameraDiscovered(const QString& deviceId, const QString& name);
    void cameraConnected(const QString& deviceId);
    void cameraDisconnected(const QString& deviceId);
    void cameraStateChanged(const QString& deviceId, CameraState state);
    void errorOccurred(const QString& deviceId, const QString& error);
    void asyncDiscoveryFinished(quint64 requestId, const QJsonObject& result);
    void cameraDiscoverySettled(const QString& pluginId, quint64 requestId, const QString& status,
                                const QString& errorCode);
    void cameraDiscoveryBatchSettled(quint64 requestId, const QString& status, const QString& errorCode);
    void cameraDiscoveryError(const QString& pluginId, const QString& errorCode);
    void cameraOperationSettled(const QString& deviceId, quint64 requestId, const QString& operation,
                                const QString& status, const QString& errorCode);
    void cameraOperationStarted(const QString& deviceId, quint64 requestId, const QString& operation);
    void cameraListChanged();
    void shutdownCompleted();

private:
    friend class CameraLease;
    struct CameraManagerEvent {
        enum class Type { Error, Connected, Disconnected, StateChanged };
        Type type = Type::Error;
        QString deviceId;
        QString message;
        CameraState state = CameraState::Disconnected;
    };
    CameraManager();
    ~CameraManager();
    void releaseCameraLease(const QString& deviceId);
    void dispatchQueuedEvents();
    void handleDiscoveryCompleted(quint64 expectedRequestId, quint64 signaledRequestId, const QByteArray& payload);
    void finishDiscoveryAfterJobDestroyed(quint64 requestId);
    void finishCameraOperation(quint64 requestId, const QString& status, const QString& errorCode,
                               CameraOperationSession* session = nullptr, bool retained = false);
    void finishCameraDetailsQuery(const QString& deviceId, const QString& pluginId, quint64 generation,
                                  quint64 requestId, const QString& status, const QString& errorCode,
                                  const QJsonObject& details);
    void finishCameraParameterWrite(const QString& deviceId, const QString& pluginId, quint64 generation,
                                    quint64 requestId, const QJsonObject& writeResult);
    bool beginCameraCapture(const QString& deviceId, double exposureTime, double gain,
                            const QSharedPointer<CameraCaptureWaitState>& waitState, quint64* requestId,
                            QString* errorCode);
    void finishCameraCapture(const QString& deviceId, const QString& pluginId, quint64 generation, quint64 requestId,
                             const QImage& image, const QString& errorCode, const QJsonObject& diagnostics);
    void finishCameraCaptureSessionRetired(const QString& deviceId, const QString& pluginId, quint64 generation);
    void handleStaleDisconnectWorkerIdle(const QString& deviceId, const QString& pluginId, quint64 generation);
    void cancelCameraCapture(quint64 requestId, const QString& errorCode);
    void handleCameraOperationFinished(const QString& deviceId, const QString& pluginId, quint64 generation,
                                       quint64 requestId, const QString& operation, const QString& status,
                                       const QString& errorCode, bool retained);
    bool beginSessionDisconnect(const QString& deviceId, quint64 requestId, QString* error);
    void startIdleSessionDisconnects();
    bool hasBusyCameraSession() const;
    quint64 nextManagedRequestId();
    bool requestIdInUse(quint64 requestId) const;
    void maybeFinishShutdown();
    bool startDiscoveryAsyncImpl(const QString& pluginId, quint64 requestId, bool* startFailureSettled, QString* error,
                                 quint64 reservationOwner = 0);
    void releaseDiscoveryReservation(const QString& pluginId, quint64 reservationOwner);
    void startManagedDiscovery(const QString& pluginId);
    bool pluginHasPendingDiscovery(const QString& pluginId) const;
    bool applyAsyncDiscoveryResult(const QString& pluginId, const QJsonArray& cameras);
    void handleAgentDiscoveryChildResult(quint64 childRequestId, const QJsonObject& result);
    void handleAgentDiscoveryChildSettled(const QString& pluginId, quint64 childRequestId, const QString& status,
                                          const QString& errorCode, bool finishBatch = true);
    void finishAgentDiscoveryBatchIfReady(quint64 batchRequestId);
    QJsonObject buildAgentDiscoveryBatchResult(quint64 batchRequestId, const QString& status,
                                               const QString& errorCode) const;

    struct PendingDiscovery {
        QString pluginId;
        ICameraPlugin* plugin = nullptr;
        ICameraDiscoveryAsync* extension = nullptr;
        CameraDiscoveryJob* job = nullptr;
        QString terminalStatus;
        QString errorCode;
        bool cancelRequested = false;
    };
    struct PendingCameraOperation {
        QString deviceId;
        QString pluginId;
        quint64 generation = 0;
        QString operation;
        QString parameter;
        QJsonValue requested = QJsonValue(QJsonValue::Undefined);
    };
    struct PendingCameraCapture {
        QString deviceId;
        QString pluginId;
        quint64 generation = 0;
        CameraOperationSession* session = nullptr;
        QSharedPointer<CameraCaptureWaitState> waitState;
    };
    struct PendingAgentDiscoveryBatch {
        quint64 requestId = 0;
        quint64 registryGeneration = 0;
        QStringList pluginIds;
        QStringList unsupportedPluginIds;
        QMap<QString, quint64> childRequestIds;
        QSet<quint64> childResultsReceived;
        QMap<QString, QJsonObject> pluginResults;
        bool starting = true;
        bool terminalPublished = false;
    };

    QSharedPointer<CameraParameterBinding> cameraParameterBindingFor(const QString& deviceId) const;

    Q_DISABLE_COPY(CameraManager)

    QMap<QString, ICameraPlugin*> m_plugins; // pluginId -> plugin
    QMap<QString, ICamera*> m_cameras;       // deviceId -> camera instance
    QMap<QString, CameraInfo> m_discoveredCameraInfo;
    QMap<QString, QMap<QString, CameraInfo>> m_asyncDiscoveryCandidates; // deviceId -> pluginId -> info
    QMap<QString, QSet<QString>> m_asyncPluginDevices;                   // pluginId -> reported device IDs
    QSet<QString> m_asyncDiscoveryPlugins;
    QMap<QString, QString> m_deviceOwners;        // deviceId -> pluginId
    QMap<QString, CameraStatus> m_cameraStatuses; // deviceId -> status
    QMap<QString, int> m_cameraLeaseCounts;
    QSet<QString> m_pendingDisconnects;
    QList<CameraManagerEvent> m_queuedEvents;
    bool m_lifecycleMutationInProgress = false;
    bool m_disconnectAllInProgress = false;
    bool m_disconnectAllSweepComplete = false;
    bool m_dispatchingEvents = false;
    bool m_shuttingDown = false;
    bool m_shutdownSignalSent = false;
    int m_asyncConnectTimeoutMs = 30000;
    int m_asyncDisconnectTimeoutMs = 30000;
    QMap<quint64, PendingDiscovery> m_pendingDiscoveries;
    QSet<QString> m_pluginsWithPendingDiscovery;
    QMap<QString, quint64> m_discoveryReservationOwners;
    QSet<quint64> m_terminalDiscoveries;
    QMap<quint64, PendingAgentDiscoveryBatch> m_pendingAgentDiscoveryBatches;
    QMap<quint64, quint64> m_agentDiscoveryChildToBatch;
    QSet<quint64> m_reservedCameraDiscoveryChildIds;
    QMap<quint64, QJsonObject> m_agentDiscoveryResults;
    QList<quint64> m_agentDiscoveryResultOrder;
    QMap<QString, CameraOperationSession*> m_cameraSessions;
    QMap<quint64, PendingCameraOperation> m_pendingCameraOperations;
    QMap<quint64, PendingCameraCapture> m_pendingCameraCaptures;
    QMap<quint64, QJsonObject> m_cameraOperationResults;
    QList<quint64> m_cameraOperationResultOrder;
    QMap<QString, quint64> m_deviceOperations;
    QMap<QString, quint64> m_deviceGenerations;
    mutable QMutex m_cameraParameterBindingsMutex;
    mutable QMap<QString, QSharedPointer<CameraParameterBinding>> m_cameraParameterBindings;
    quint64 m_nextManagedRequestId = std::numeric_limits<quint64>::max();
    quint64 m_cameraPluginRegistryGeneration = 0;
    mutable QMutex m_mutex;
};

} // namespace DeepLux
