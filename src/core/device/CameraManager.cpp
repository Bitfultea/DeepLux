#include "CameraManager.h"

#include "CameraDiscoveryJob.h"
#include "CameraOperationSession.h"
#include "core/common/Logger.h"
#include "core/engine/RunEngine.h"
#include "core/interface/ICamera.h"
#include "core/interface/ICameraDiscoveryAsync.h"
#include "core/interface/ICameraPlugin.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <algorithm>

namespace DeepLux {

namespace {

QJsonObject discoveryError(quint64 requestId, const QString& code) {
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("requestId"), QString::number(requestId)},
            {QStringLiteral("status"), QStringLiteral("error")},
            {QStringLiteral("errorCode"), code}};
}

bool validCameraObject(const QJsonValue& value) {
    if (!value.isObject())
        return false;
    const QJsonObject camera = value.toObject();
    const QStringList strings = {QStringLiteral("pluginId"),  QStringLiteral("deviceId"),
                                 QStringLiteral("name"),      QStringLiteral("serialNumber"),
                                 QStringLiteral("ipAddress"), QStringLiteral("pixelFormat")};
    for (const QString& key : strings)
        if (!camera.value(key).isString())
            return false;
    if (!camera.value(QStringLiteral("width")).isDouble() || !camera.value(QStringLiteral("height")).isDouble() ||
        !camera.value(QStringLiteral("isConnected")).isBool() || !camera.value(QStringLiteral("rawInfo")).isObject())
        return false;
    return camera.value(QStringLiteral("width")).toInt(-1) >= 0 &&
           camera.value(QStringLiteral("height")).toInt(-1) >= 0 &&
           !camera.value(QStringLiteral("deviceId")).toString().isEmpty();
}

bool sameCameraInfo(const CameraInfo& lhs, const CameraInfo& rhs) {
    return lhs.pluginId == rhs.pluginId && lhs.deviceId == rhs.deviceId && lhs.name == rhs.name &&
           lhs.serialNumber == rhs.serialNumber && lhs.ipAddress == rhs.ipAddress && lhs.width == rhs.width &&
           lhs.height == rhs.height && lhs.pixelFormat == rhs.pixelFormat && lhs.rawInfo == rhs.rawInfo &&
           lhs.isConnected == rhs.isConnected;
}

QString cameraStateName(CameraState state) {
    switch (state) {
    case CameraState::Disconnected:
        return QStringLiteral("disconnected");
    case CameraState::Connecting:
        return QStringLiteral("connecting");
    case CameraState::Connected:
        return QStringLiteral("connected");
    case CameraState::Grabbing:
        return QStringLiteral("grabbing");
    case CameraState::Error:
        return QStringLiteral("error");
    }
    return QStringLiteral("unknown");
}

} // namespace

CameraManager& CameraManager::instance() {
    static CameraManager instance;
    return instance;
}

CameraLease::CameraLease(CameraManager* manager, QString deviceId, ICamera* camera)
    : m_manager(manager), m_deviceId(std::move(deviceId)), m_camera(camera) {}

CameraLease::CameraLease(CameraLease&& other) noexcept
    : m_manager(other.m_manager), m_deviceId(std::move(other.m_deviceId)), m_camera(other.m_camera) {
    other.m_manager = nullptr;
    other.m_camera = nullptr;
}

CameraLease& CameraLease::operator=(CameraLease&& other) noexcept {
    if (this == &other)
        return *this;
    reset();
    m_manager = other.m_manager;
    m_deviceId = std::move(other.m_deviceId);
    m_camera = other.m_camera;
    other.m_manager = nullptr;
    other.m_camera = nullptr;
    return *this;
}

CameraLease::~CameraLease() {
    reset();
}

void CameraLease::reset() {
    CameraManager* manager = m_manager;
    const QString deviceId = m_deviceId;
    m_manager = nullptr;
    m_deviceId.clear();
    m_camera = nullptr;
    if (manager)
        manager->releaseCameraLease(deviceId);
}

CameraManager::CameraManager() {
    connect(
        this, &CameraManager::asyncDiscoveryFinished, this,
        [this](quint64 requestId, const QJsonObject& result) { handleAgentDiscoveryChildResult(requestId, result); },
        Qt::DirectConnection);
    connect(
        this, &CameraManager::cameraDiscoverySettled, this,
        [this](const QString& pluginId, quint64 requestId, const QString& status, const QString& errorCode) {
            handleAgentDiscoveryChildSettled(pluginId, requestId, status, errorCode);
        },
        Qt::DirectConnection);
    Logger::instance().info("Camera manager initialized", "Camera");
}

CameraManager::~CameraManager() {
    disconnectAll();
}

void CameraManager::registerCameraPlugin(ICameraPlugin* plugin) {
    if (!plugin)
        return;

    if (QThread::currentThread() != thread() || plugin->thread() != thread()) {
        Logger::instance().error("Camera plugin registration must run on CameraManager's thread", "Camera");
        return;
    }
    if (m_shuttingDown) {
        Logger::instance().warning("Camera plugin registration rejected during shutdown", "Camera");
        return;
    }

    QString pluginId;
    QList<QPair<QString, QString>> discoveredSignals;
    const auto* asyncExtension = qobject_cast<ICameraDiscoveryAsync*>(plugin);
    {
        QMutexLocker locker(&m_mutex);
        if (m_lifecycleMutationInProgress) {
            Logger::instance().warning("Reentrant camera plugin lifecycle mutation rejected", "Camera");
            return;
        }

        pluginId = plugin->pluginId();
        if (pluginId.trimmed().isEmpty()) {
            Logger::instance().warning("Rejected camera plugin with empty plugin ID", "Camera");
            return;
        }
        if (m_plugins.contains(pluginId)) {
            Logger::instance().warning(QString("Camera plugin already registered: %1").arg(pluginId), "Camera");
            return;
        }

        m_lifecycleMutationInProgress = true;
        m_plugins.insert(pluginId, plugin);
        ++m_cameraPluginRegistryGeneration;
        if (asyncExtension)
            m_asyncDiscoveryPlugins.insert(pluginId);
        Logger::instance().info(
            QString("Camera plugin registered: %1 (%2)").arg(plugin->pluginName()).arg(plugin->manufacturer()),
            "Camera");

        if (!asyncExtension && plugin->isAvailable()) {
            const QList<CameraInfo> cameras = plugin->discoverCameras();
            for (CameraInfo info : cameras) {
                if (info.pluginId.isEmpty())
                    info.pluginId = pluginId;
                if (info.pluginId != pluginId || info.deviceId.isEmpty()) {
                    Logger::instance().warning(QString("Rejected invalid camera metadata from %1").arg(pluginId),
                                               "Camera");
                    continue;
                }
                const QString existingOwner = m_deviceOwners.value(info.deviceId);
                if (!existingOwner.isEmpty() && existingOwner != pluginId) {
                    Logger::instance().warning(QString("Rejected duplicate camera device ID %1 from %2 (owned by %3)")
                                                   .arg(info.deviceId, pluginId, existingOwner),
                                               "Camera");
                    continue;
                }

                m_deviceOwners.insert(info.deviceId, pluginId);
                m_discoveredCameraInfo.insert(info.deviceId, info);
                CameraStatus status = m_cameraStatuses.value(info.deviceId);
                status.deviceId = info.deviceId;
                status.name = info.name;
                if (!m_cameraStatuses.contains(info.deviceId))
                    status.state = CameraState::Disconnected;
                m_cameraStatuses.insert(info.deviceId, status);
                discoveredSignals.append(qMakePair(info.deviceId, info.name));
            }
        }
    }

    for (const auto& camera : discoveredSignals)
        emit cameraDiscovered(camera.first, camera.second);
    emit pluginRegistered(pluginId);
    if (!discoveredSignals.isEmpty())
        emit cameraListChanged();

    {
        QMutexLocker locker(&m_mutex);
        m_lifecycleMutationInProgress = false;
    }
    if (asyncExtension)
        startManagedDiscovery(pluginId);
}

void CameraManager::unregisterCameraPlugin(const QString& pluginId) {
    QString error;
    if (!tryUnregisterCameraPlugin(pluginId, &error) && !error.isEmpty())
        Logger::instance().warning(QString("Camera plugin unregister rejected (%1): %2").arg(pluginId, error),
                                   "Camera");
}

bool CameraManager::tryUnregisterCameraPlugin(const QString& pluginId, QString* error) {
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (error)
        error->clear();
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));

    {
        QMutexLocker locker(&m_mutex);
        if (m_lifecycleMutationInProgress)
            return fail(QStringLiteral("CAMERA_MANAGER_LIFECYCLE_REENTRANT"));
        if (!m_plugins.contains(pluginId))
            return fail(QStringLiteral("CAMERA_PLUGIN_NOT_FOUND"));
        if (m_pluginsWithPendingDiscovery.contains(pluginId))
            return fail(QStringLiteral("CAMERA_PLUGIN_IN_USE"));
        for (auto it = m_cameraSessions.cbegin(); it != m_cameraSessions.cend(); ++it)
            if (it.value() && it.value()->pluginId() == pluginId)
                return fail(QStringLiteral("CAMERA_PLUGIN_IN_USE"));
        for (auto it = m_pendingCameraOperations.cbegin(); it != m_pendingCameraOperations.cend(); ++it)
            if (it->pluginId == pluginId)
                return fail(QStringLiteral("CAMERA_PLUGIN_IN_USE"));
        for (auto it = m_cameras.cbegin(); it != m_cameras.cend(); ++it) {
            if (m_deviceOwners.value(it.key()) == pluginId)
                return fail(QStringLiteral("CAMERA_PLUGIN_IN_USE"));
        }

        m_lifecycleMutationInProgress = true;
        m_plugins.remove(pluginId);
        ++m_cameraPluginRegistryGeneration;
        m_asyncDiscoveryPlugins.remove(pluginId);
        const QSet<QString> reported = m_asyncPluginDevices.take(pluginId);
        for (const QString& deviceId : reported) {
            auto candidates = m_asyncDiscoveryCandidates.find(deviceId);
            if (candidates != m_asyncDiscoveryCandidates.end()) {
                candidates->remove(pluginId);
                if (candidates->isEmpty())
                    m_asyncDiscoveryCandidates.erase(candidates);
            }
        }
        for (auto it = m_deviceOwners.begin(); it != m_deviceOwners.end();) {
            if (it.value() == pluginId) {
                const auto candidates = m_asyncDiscoveryCandidates.value(it.key());
                if (candidates.isEmpty()) {
                    m_cameraStatuses.remove(it.key());
                    m_discoveredCameraInfo.remove(it.key());
                    it = m_deviceOwners.erase(it);
                } else {
                    const QString nextOwner = candidates.firstKey();
                    const CameraInfo info = candidates.value(nextOwner);
                    it.value() = nextOwner;
                    m_discoveredCameraInfo.insert(it.key(), info);
                    m_cameraStatuses[it.key()].name = info.name;
                    ++it;
                }
            } else {
                ++it;
            }
        }
    }

    emit pluginUnregistered(pluginId);
    {
        QMutexLocker locker(&m_mutex);
        m_lifecycleMutationInProgress = false;
    }
    Logger::instance().info(QString("Camera plugin unregistered: %1").arg(pluginId), "Camera");
    emit cameraListChanged();
    return true;
}

bool CameraManager::startDiscoveryAsync(const QString& pluginId, quint64 requestId, QString* error) {
    return startDiscoveryAsyncImpl(pluginId, requestId, nullptr, error, requestId);
}

QJsonObject CameraManager::cameraDiscoveryTargets() const {
    if (QThread::currentThread() != thread())
        return {{QStringLiteral("status"), QStringLiteral("error")},
                {QStringLiteral("errorCode"), QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY")}};
    QStringList asyncPluginIds;
    QStringList unsupportedPluginIds;
    for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it) {
        if (qobject_cast<ICameraDiscoveryAsync*>(it.value()))
            asyncPluginIds.append(it.key());
        else
            unsupportedPluginIds.append(it.key());
    }
    return {{QStringLiteral("status"), QStringLiteral("success")},
            {QStringLiteral("registryGeneration"), QString::number(m_cameraPluginRegistryGeneration)},
            {QStringLiteral("asyncPluginIds"), QJsonArray::fromStringList(asyncPluginIds)},
            {QStringLiteral("unsupportedPluginIds"), QJsonArray::fromStringList(unsupportedPluginIds)},
            {QStringLiteral("discoveryPerformed"), false}};
}

bool CameraManager::requestCameraDiscoveryBatchAsync(quint64 expectedRegistryGeneration, const QStringList& pluginIds,
                                                     const QStringList& unsupportedPluginIds, quint64* requestId,
                                                     QString* error) {
    if (requestId)
        *requestId = 0;
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (!requestId)
        return fail(QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_REQUIRED"));
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    if (m_shuttingDown)
        return fail(QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
    if (RunEngine::instance().isBusy())
        return fail(QStringLiteral("ENGINE_BUSY"));
    if (m_lifecycleMutationInProgress)
        return fail(QStringLiteral("CAMERA_MANAGER_LIFECYCLE_REENTRANT"));

    const QJsonObject currentTargets = cameraDiscoveryTargets();
    if (currentTargets.value(QStringLiteral("status")).toString() != QLatin1String("success"))
        return fail(currentTargets.value(QStringLiteral("errorCode")).toString());
    const quint64 currentGeneration =
        currentTargets.value(QStringLiteral("registryGeneration")).toString().toULongLong();
    if (currentGeneration != expectedRegistryGeneration)
        return fail(QStringLiteral("CAMERA_TARGET_CHANGED"));
    QStringList currentAsync;
    for (const QJsonValue& value : currentTargets.value(QStringLiteral("asyncPluginIds")).toArray())
        currentAsync.append(value.toString());
    QStringList currentUnsupported;
    for (const QJsonValue& value : currentTargets.value(QStringLiteral("unsupportedPluginIds")).toArray())
        currentUnsupported.append(value.toString());
    if (currentAsync.isEmpty() || pluginIds.isEmpty())
        return fail(QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
    const bool legacyOnly = std::all_of(pluginIds.cbegin(), pluginIds.cend(), [&currentUnsupported](const QString& id) {
        return currentUnsupported.contains(id);
    });
    if (legacyOnly)
        return fail(QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
    if (pluginIds != currentAsync || unsupportedPluginIds != currentUnsupported)
        return fail(QStringLiteral("CAMERA_TARGET_CHANGED"));

    for (const QString& pluginId : pluginIds) {
        ICameraPlugin* plugin = m_plugins.value(pluginId, nullptr);
        if (!plugin || !qobject_cast<ICameraDiscoveryAsync*>(plugin))
            return fail(QStringLiteral("CAMERA_TARGET_CHANGED"));
        if (plugin->thread() != thread())
            return fail(QStringLiteral("CAMERA_PLUGIN_THREAD_AFFINITY"));
        if (m_pluginsWithPendingDiscovery.contains(pluginId))
            return fail(QStringLiteral("CAMERA_DISCOVERY_BUSY"));
    }

    const quint64 batchId = nextManagedRequestId();
    if (batchId == 0)
        return fail(QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_EXHAUSTED"));
    PendingAgentDiscoveryBatch batch;
    batch.requestId = batchId;
    batch.registryGeneration = currentGeneration;
    batch.pluginIds = pluginIds;
    batch.unsupportedPluginIds = unsupportedPluginIds;
    m_pendingAgentDiscoveryBatches.insert(batchId, batch);

    QMap<QString, quint64> childIds;
    for (const QString& pluginId : pluginIds) {
        const quint64 childId = nextManagedRequestId();
        if (childId == 0) {
            for (quint64 allocatedChildId : childIds) {
                m_reservedCameraDiscoveryChildIds.remove(allocatedChildId);
                m_agentDiscoveryChildToBatch.remove(allocatedChildId);
            }
            m_pendingAgentDiscoveryBatches.remove(batchId);
            return fail(QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_EXHAUSTED"));
        }
        childIds.insert(pluginId, childId);
        m_reservedCameraDiscoveryChildIds.insert(childId);
        m_agentDiscoveryChildToBatch.insert(childId, batchId);
    }
    m_pendingAgentDiscoveryBatches[batchId].childRequestIds = childIds;

    // Reserve every plugin before the first plugin virtual call. Reentrant start/unregister
    // calls observe these reservations and cannot race the batch's admission snapshot.
    {
        QMutexLocker locker(&m_mutex);
        for (const QString& pluginId : pluginIds) {
            m_pluginsWithPendingDiscovery.insert(pluginId);
            m_discoveryReservationOwners.insert(pluginId, batchId);
        }
    }

    for (const QString& pluginId : pluginIds) {
        const quint64 childId = childIds.value(pluginId);
        QString childError;
        bool failureSettled = false;
        if (!startDiscoveryAsyncImpl(pluginId, childId, &failureSettled, &childError, batchId) && !failureSettled) {
            const QString code = childError.isEmpty() ? QStringLiteral("CAMERA_DISCOVERY_START_FAILED") : childError;
            handleAgentDiscoveryChildSettled(pluginId, childId, QStringLiteral("start_failed"), code);
            m_reservedCameraDiscoveryChildIds.remove(childId);
            emit cameraDiscoverySettled(pluginId, childId, QStringLiteral("start_failed"), code);
            releaseDiscoveryReservation(pluginId, batchId);
        }
    }
    m_pendingAgentDiscoveryBatches[batchId].starting = false;
    finishAgentDiscoveryBatchIfReady(batchId);
    *requestId = batchId;
    if (error)
        error->clear();
    return true;
}

bool CameraManager::getCameraDiscoveryOperationResult(quint64 requestId, QJsonObject* result) const {
    if (result)
        *result = QJsonObject();
    if (!result || requestId == 0 || QThread::currentThread() != thread())
        return false;
    const auto completed = m_agentDiscoveryResults.constFind(requestId);
    if (completed != m_agentDiscoveryResults.cend()) {
        *result = completed.value();
        return true;
    }
    if (!m_pendingAgentDiscoveryBatches.contains(requestId))
        return false;
    *result = buildAgentDiscoveryBatchResult(requestId, QStringLiteral("pending"), {});
    return true;
}

bool CameraManager::startDiscoveryAsyncImpl(const QString& pluginId, quint64 requestId, bool* startFailureSettled,
                                            QString* error, quint64 reservationOwner) {
    if (startFailureSettled)
        *startFailureSettled = false;
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (error)
        error->clear();
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    if (m_shuttingDown)
        return fail(QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
    if (requestId == 0)
        return fail(QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_INVALID"));
    if (m_pendingDiscoveries.contains(requestId) || m_pendingCameraOperations.contains(requestId) ||
        m_cameraOperationResults.contains(requestId) || m_pendingAgentDiscoveryBatches.contains(requestId) ||
        m_agentDiscoveryResults.contains(requestId) ||
        (m_reservedCameraDiscoveryChildIds.contains(requestId) &&
         (reservationOwner == 0 || m_agentDiscoveryChildToBatch.value(requestId) != reservationOwner)))
        return fail(QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_IN_USE"));

    ICameraPlugin* plugin = findPlugin(pluginId);
    if (!plugin)
        return fail(QStringLiteral("CAMERA_PLUGIN_NOT_FOUND"));
    auto* extension = qobject_cast<ICameraDiscoveryAsync*>(plugin);
    if (!extension)
        return fail(QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
    if (thread() != plugin->thread() || QThread::currentThread() != plugin->thread())
        return fail(QStringLiteral("CAMERA_PLUGIN_THREAD_AFFINITY"));
    {
        QMutexLocker locker(&m_mutex);
        const bool reserved = m_pluginsWithPendingDiscovery.contains(pluginId);
        const quint64 currentOwner = m_discoveryReservationOwners.value(pluginId, 0);
        if (reserved && (reservationOwner == 0 || currentOwner != reservationOwner))
            return fail(QStringLiteral("CAMERA_DISCOVERY_BUSY"));
        if (!reserved) {
            if (reservationOwner != 0 && reservationOwner != requestId)
                return fail(QStringLiteral("CAMERA_DISCOVERY_RESERVATION_LOST"));
            m_pluginsWithPendingDiscovery.insert(pluginId);
            m_discoveryReservationOwners.insert(pluginId, requestId);
            reservationOwner = requestId;
        }
    }

    // Publish the child before calling plugin code. A reentrant start, cancel, unregister,
    // or shutdown therefore sees the admission and cannot start a second job or unload it.
    PendingDiscovery pending{pluginId, plugin, extension, nullptr, {}, {}};
    m_pendingDiscoveries.insert(requestId, pending);

    QString pluginError;
    CameraDiscoveryJob* job = extension->startDiscovery(requestId, pluginError);
    if (!job) {
        const QString code = pluginError.isEmpty() ? QStringLiteral("CAMERA_DISCOVERY_START_FAILED") : pluginError;
        m_pendingDiscoveries.remove(requestId);
        fail(code);
        if (startFailureSettled)
            *startFailureSettled = true;
        emit cameraDiscoveryError(pluginId, code);
        emit cameraDiscoverySettled(pluginId, requestId, QStringLiteral("start_failed"), code);
        releaseDiscoveryReservation(pluginId, reservationOwner);
        return false;
    }
    if (job->parent() != plugin || job->thread() != plugin->thread() || job->isTerminal()) {
        auto pendingIt = m_pendingDiscoveries.find(requestId);
        if (pendingIt != m_pendingDiscoveries.end()) {
            pendingIt->job = job;
            pendingIt->terminalStatus = QStringLiteral("start_failed");
            pendingIt->errorCode = QStringLiteral("CAMERA_DISCOVERY_JOB_INVALID");
        }
        {
            QMutexLocker locker(&m_mutex);
            if (m_discoveryReservationOwners.value(pluginId) == reservationOwner)
                m_discoveryReservationOwners[pluginId] = requestId;
        }
        connect(
            job, &QObject::destroyed, this, [this, requestId]() { finishDiscoveryAfterJobDestroyed(requestId); },
            Qt::QueuedConnection);
        job->deleteLater();
        const QString code = QStringLiteral("CAMERA_DISCOVERY_JOB_INVALID");
        fail(code);
        emit cameraDiscoveryError(pluginId, code);
        // The request has been admitted. The child and plugin reservation remain live until
        // the invalid QObject is actually destroyed and the plugin lease is safe to release.
        return true;
    }

    m_pendingDiscoveries[requestId].job = job;
    {
        QMutexLocker locker(&m_mutex);
        // The plugin reservation becomes a child job lease. Aggregate-owned reservations
        // use a distinct owner token until this exact child is safely destroyed.
        if (m_discoveryReservationOwners.value(pluginId) != reservationOwner) {
            m_pendingDiscoveries.remove(requestId);
            job->deleteLater();
            return fail(QStringLiteral("CAMERA_DISCOVERY_RESERVATION_LOST"));
        }
        m_discoveryReservationOwners[pluginId] = requestId;
    }
    connect(
        job, &CameraDiscoveryJob::completed, this,
        [this, requestId](quint64 signaledId, const QByteArray& payload) {
            handleDiscoveryCompleted(requestId, signaledId, payload);
        },
        Qt::QueuedConnection);
    connect(
        job, &QObject::destroyed, this, [this, requestId]() { finishDiscoveryAfterJobDestroyed(requestId); },
        Qt::QueuedConnection);
    if (m_shuttingDown)
        requestDiscoveryCancel(requestId);
    return true;
}

void CameraManager::releaseDiscoveryReservation(const QString& pluginId, quint64 reservationOwner) {
    QMutexLocker locker(&m_mutex);
    if (m_discoveryReservationOwners.value(pluginId, 0) != reservationOwner)
        return;
    m_discoveryReservationOwners.remove(pluginId);
    m_pluginsWithPendingDiscovery.remove(pluginId);
}

void CameraManager::handleAgentDiscoveryChildResult(quint64 childRequestId, const QJsonObject& result) {
    const quint64 batchRequestId = m_agentDiscoveryChildToBatch.value(childRequestId, 0);
    auto batch = m_pendingAgentDiscoveryBatches.find(batchRequestId);
    if (batch == m_pendingAgentDiscoveryBatches.end())
        return;
    QString pluginId;
    for (auto it = batch->childRequestIds.cbegin(); it != batch->childRequestIds.cend(); ++it) {
        if (it.value() == childRequestId) {
            pluginId = it.key();
            break;
        }
    }
    if (pluginId.isEmpty())
        return;
    if (batch->childResultsReceived.contains(childRequestId))
        return;
    batch->childResultsReceived.insert(childRequestId);
    QJsonObject pluginResult = batch->pluginResults.value(pluginId);
    pluginResult.insert(QStringLiteral("pluginId"), pluginId);
    const QString status = result.value(QStringLiteral("status")).toString();
    if (!status.isEmpty())
        pluginResult.insert(QStringLiteral("status"), status);
    if (result.value(QStringLiteral("cameras")).isArray())
        pluginResult.insert(QStringLiteral("cameras"), result.value(QStringLiteral("cameras")));
    if (result.value(QStringLiteral("errorCode")).isString())
        pluginResult.insert(QStringLiteral("errorCode"), result.value(QStringLiteral("errorCode")));
    batch->pluginResults.insert(pluginId, pluginResult);
}

void CameraManager::handleAgentDiscoveryChildSettled(const QString& pluginId, quint64 childRequestId,
                                                     const QString& status, const QString& errorCode,
                                                     bool finishBatch) {
    const quint64 batchRequestId = m_agentDiscoveryChildToBatch.value(childRequestId, 0);
    auto batch = m_pendingAgentDiscoveryBatches.find(batchRequestId);
    if (batch == m_pendingAgentDiscoveryBatches.end() || batch->childRequestIds.value(pluginId) != childRequestId)
        return;
    QJsonObject pluginResult = batch->pluginResults.value(pluginId);
    if (pluginResult.value(QStringLiteral("terminal")).toBool())
        return;
    pluginResult.insert(QStringLiteral("pluginId"), pluginId);
    pluginResult.insert(QStringLiteral("status"), status);
    pluginResult.insert(QStringLiteral("terminal"), true);
    if (!pluginResult.value(QStringLiteral("cameras")).isArray())
        pluginResult.insert(QStringLiteral("cameras"), QJsonArray{});
    if (!errorCode.isEmpty())
        pluginResult.insert(QStringLiteral("errorCode"), errorCode);
    else
        pluginResult.remove(QStringLiteral("errorCode"));
    batch->pluginResults.insert(pluginId, pluginResult);
    if (finishBatch)
        finishAgentDiscoveryBatchIfReady(batchRequestId);
}

void CameraManager::finishAgentDiscoveryBatchIfReady(quint64 batchRequestId) {
    auto batch = m_pendingAgentDiscoveryBatches.find(batchRequestId);
    if (batch == m_pendingAgentDiscoveryBatches.end() || batch->starting || batch->terminalPublished)
        return;
    for (const QString& pluginId : batch->pluginIds) {
        if (!batch->pluginResults.value(pluginId).value(QStringLiteral("terminal")).toBool())
            return;
    }

    int succeeded = 0;
    for (const QString& pluginId : batch->pluginIds)
        if (batch->pluginResults.value(pluginId).value(QStringLiteral("status")).toString() == QLatin1String("success"))
            ++succeeded;
    const QString status = succeeded == batch->pluginIds.size()
                               ? QStringLiteral("success")
                               : (succeeded > 0 ? QStringLiteral("partial") : QStringLiteral("error"));
    const QString errorCode =
        status == QLatin1String("partial")
            ? QStringLiteral("CAMERA_DISCOVERY_PARTIAL")
            : (status == QLatin1String("error") ? QStringLiteral("CAMERA_DISCOVERY_FAILED") : QString());
    batch->terminalPublished = true;
    const QJsonObject result = buildAgentDiscoveryBatchResult(batchRequestId, status, errorCode);
    m_agentDiscoveryResults.insert(batchRequestId, result);
    m_agentDiscoveryResultOrder.removeAll(batchRequestId);
    m_agentDiscoveryResultOrder.append(batchRequestId);
    if (m_agentDiscoveryResultOrder.size() > 256) {
        const quint64 expiredRequestId = m_agentDiscoveryResultOrder.takeFirst();
        m_agentDiscoveryResults.remove(expiredRequestId);
    }
    const QString pluginStatus = status;
    m_pendingAgentDiscoveryBatches.erase(batch);
    for (auto it = m_agentDiscoveryChildToBatch.begin(); it != m_agentDiscoveryChildToBatch.end();) {
        if (it.value() == batchRequestId) {
            m_reservedCameraDiscoveryChildIds.remove(it.key());
            it = m_agentDiscoveryChildToBatch.erase(it);
        } else {
            ++it;
        }
    }
    emit cameraDiscoveryBatchSettled(batchRequestId, pluginStatus, errorCode);
}

QJsonObject CameraManager::buildAgentDiscoveryBatchResult(quint64 batchRequestId, const QString& status,
                                                          const QString& errorCode) const {
    const auto batch = m_pendingAgentDiscoveryBatches.constFind(batchRequestId);
    if (batch == m_pendingAgentDiscoveryBatches.cend())
        return {};
    QJsonArray pluginResults;
    QJsonArray cameras;
    for (const QString& pluginId : batch->pluginIds) {
        QJsonObject pluginResult = batch->pluginResults.value(pluginId);
        if (pluginResult.isEmpty()) {
            pluginResult.insert(QStringLiteral("pluginId"), pluginId);
            pluginResult.insert(QStringLiteral("status"), QStringLiteral("pending"));
            pluginResult.insert(QStringLiteral("cameras"), QJsonArray{});
        }
        if (status == QLatin1String("pending") || !pluginResult.value(QStringLiteral("terminal")).toBool()) {
            pluginResult.insert(QStringLiteral("status"), QStringLiteral("pending"));
            pluginResult.insert(QStringLiteral("cameras"), QJsonArray{});
            pluginResult.remove(QStringLiteral("errorCode"));
        }
        pluginResult.remove(QStringLiteral("terminal"));
        pluginResults.append(pluginResult);
        if (pluginResult.value(QStringLiteral("status")).toString() == QLatin1String("success")) {
            for (const QJsonValue& camera : pluginResult.value(QStringLiteral("cameras")).toArray())
                cameras.append(camera);
        }
    }
    QJsonObject result{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("requestId"), QString::number(batchRequestId)},
        {QStringLiteral("operation"), QStringLiteral("discover_cameras")},
        {QStringLiteral("status"), status},
        {QStringLiteral("pluginResults"), pluginResults},
        {QStringLiteral("cameras"), cameras},
        {QStringLiteral("unsupportedPluginIds"), QJsonArray::fromStringList(batch->unsupportedPluginIds)},
        {QStringLiteral("retryable"), false}};
    if (!errorCode.isEmpty())
        result.insert(QStringLiteral("errorCode"), errorCode);
    return result;
}

bool CameraManager::requestDiscoveryCancel(quint64 requestId, QString* error) {
    if (error)
        error->clear();
    if (QThread::currentThread() != thread()) {
        if (error)
            *error = QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY");
        return false;
    }
    auto it = m_pendingDiscoveries.find(requestId);
    if (it == m_pendingDiscoveries.end() || it->terminalStatus == QLatin1String("start_failed")) {
        if (error)
            *error = QStringLiteral("CAMERA_DISCOVERY_NOT_FOUND");
        return false;
    }
    if (it->cancelRequested)
        return true;
    it->cancelRequested = true;
    it->extension->requestCancel(requestId);
    return true;
}

bool CameraManager::hasPendingDiscovery(quint64 requestId) const {
    if (QThread::currentThread() != thread())
        return false;
    return m_pendingDiscoveries.contains(requestId);
}

int CameraManager::pendingDiscoveryCount() const {
    if (QThread::currentThread() != thread())
        return 0;
    return m_pendingDiscoveries.size();
}

bool CameraManager::connectCameraAsync(const QString& deviceId, quint64 requestId, QString* error) {
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (error)
        error->clear();
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    if (m_shuttingDown)
        return fail(QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
    if (requestId == 0)
        return fail(QStringLiteral("CAMERA_OPERATION_REQUEST_ID_INVALID"));
    if (deviceId.isEmpty())
        return fail(QStringLiteral("CAMERA_DEVICE_ID_REQUIRED"));
    if (requestIdInUse(requestId))
        return fail(QStringLiteral("CAMERA_OPERATION_REQUEST_ID_IN_USE"));

    QString ownerId;
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_disconnectAllInProgress || m_deviceOperations.contains(deviceId) || m_cameras.contains(deviceId) ||
            m_cameraLeaseCounts.value(deviceId) > 0 || m_pendingDisconnects.contains(deviceId))
            return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));
        if (!m_deviceOwners.contains(deviceId))
            return fail(QStringLiteral("CAMERA_NOT_FOUND"));
        ownerId = m_deviceOwners.value(deviceId);
        if (m_cameraSessions.contains(deviceId)) {
            const CameraState state = m_cameraStatuses.value(deviceId).state;
            return fail(state == CameraState::Connected ? QStringLiteral("CAMERA_ALREADY_CONNECTED")
                                                        : QStringLiteral("CAMERA_OPERATION_BUSY"));
        }
        generation = ++m_deviceGenerations[deviceId];
        m_pendingCameraOperations.insert(
            requestId,
            PendingCameraOperation{
                deviceId, ownerId, generation, QStringLiteral("connect"), {}, QJsonValue(QJsonValue::Undefined)});
        m_deviceOperations.insert(deviceId, requestId);
        CameraStatus& status = m_cameraStatuses[deviceId];
        status.deviceId = deviceId;
        status.lastError.clear();
        status.state = CameraState::Connecting;
        status.lastError.clear();
    }

    emit cameraOperationStarted(deviceId, requestId, QStringLiteral("connect"));
    emit cameraStateChanged(deviceId, CameraState::Connecting);
    QMetaObject::invokeMethod(
        this,
        [this, deviceId, ownerId, generation, requestId]() {
            const auto pending = m_pendingCameraOperations.constFind(requestId);
            if (pending == m_pendingCameraOperations.cend() || m_shuttingDown ||
                m_deviceGenerations.value(deviceId) != generation) {
                finishCameraOperation(requestId, QStringLiteral("cancelled"),
                                      QStringLiteral("CAMERA_OPERATION_CANCELLED"));
                return;
            }

            ICameraPlugin* plugin = findPlugin(ownerId);
            if (!plugin || plugin->thread() != thread() || QThread::currentThread() != plugin->thread()) {
                finishCameraOperation(requestId, QStringLiteral("error"),
                                      plugin ? QStringLiteral("CAMERA_PLUGIN_THREAD_AFFINITY")
                                             : QStringLiteral("CAMERA_PLUGIN_NOT_FOUND"));
                return;
            }
            const CameraInfo info = m_discoveredCameraInfo.value(deviceId);
            if (info.deviceId != deviceId || info.pluginId != ownerId) {
                finishCameraOperation(requestId, QStringLiteral("error"), QStringLiteral("CAMERA_INFO_NOT_CACHED"));
                return;
            }

            QObject* object = plugin->createCamera(info);
            ICamera* camera = qobject_cast<ICamera*>(object);
            if (!camera || object == plugin || object == this || camera->parent() || camera->thread() != thread()) {
                if (object && object != plugin && object != this) {
                    connect(
                        object, &QObject::destroyed, this,
                        [this, requestId]() {
                            finishCameraOperation(requestId, QStringLiteral("error"),
                                                  QStringLiteral("CAMERA_FACTORY_INVALID"));
                        },
                        Qt::QueuedConnection);
                    object->deleteLater();
                    return;
                }
                finishCameraOperation(requestId, QStringLiteral("error"), QStringLiteral("CAMERA_FACTORY_INVALID"));
                return;
            }

            auto* session = new CameraOperationSession(deviceId, ownerId, generation, camera,
                                                       cameraParameterBindingFor(deviceId), this);
            connect(session, &CameraOperationSession::operationFinished, this,
                    &CameraManager::handleCameraOperationFinished);
            connect(session, &CameraOperationSession::detailsQueryFinished, this,
                    &CameraManager::finishCameraDetailsQuery);
            connect(session, &CameraOperationSession::parameterWriteFinished, this,
                    &CameraManager::finishCameraParameterWrite);
            connect(session, &CameraOperationSession::captureFinished, this, &CameraManager::finishCameraCapture);
            connect(session, &CameraOperationSession::captureSessionRetired, this,
                    &CameraManager::finishCameraCaptureSessionRetired);
            connect(session, &CameraOperationSession::staleDisconnectWorkerIdle, this,
                    &CameraManager::handleStaleDisconnectWorkerIdle);
            {
                QMutexLocker locker(&m_mutex);
                m_cameraSessions.insert(deviceId, session);
            }
            if (!session->startConnect(requestId, m_asyncConnectTimeoutMs)) {
                {
                    QMutexLocker locker(&m_mutex);
                    m_cameraSessions.remove(deviceId);
                }
                delete camera;
                session->deleteLater();
                finishCameraOperation(requestId, QStringLiteral("error"),
                                      QStringLiteral("CAMERA_SESSION_START_FAILED"));
            }
        },
        Qt::QueuedConnection);
    return true;
}

bool CameraManager::disconnectCameraAsync(const QString& deviceId, quint64 requestId, QString* error) {
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (error)
        error->clear();
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    if (m_shuttingDown)
        return fail(QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
    if (deviceId.isEmpty())
        return fail(QStringLiteral("CAMERA_DEVICE_ID_REQUIRED"));
    return beginSessionDisconnect(deviceId, requestId, error);
}

bool CameraManager::requestConnectCameraAsync(const QString& deviceId, quint64* requestId, QString* error) {
    if (requestId)
        *requestId = 0;
    if (!requestId) {
        if (error)
            *error = QStringLiteral("CAMERA_OPERATION_REQUEST_ID_REQUIRED");
        return false;
    }
    if (QThread::currentThread() != thread()) {
        if (error)
            *error = QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY");
        return false;
    }
    if (m_shuttingDown) {
        if (error)
            *error = QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN");
        return false;
    }
    const quint64 allocatedId = nextManagedRequestId();
    if (allocatedId == 0) {
        if (error)
            *error = QStringLiteral("CAMERA_OPERATION_ID_EXHAUSTED");
        return false;
    }
    QString submitError;
    if (!connectCameraAsync(deviceId, allocatedId, &submitError)) {
        if (error)
            *error = submitError;
        return false;
    }
    *requestId = allocatedId;
    if (error)
        error->clear();
    return true;
}

bool CameraManager::requestConnectCameraAsync(const QString& deviceId, const QJsonObject& expectedTarget,
                                              quint64* requestId, QString* error) {
    if (requestId)
        *requestId = 0;
    if (QThread::currentThread() != thread()) {
        if (error)
            *error = QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY");
        return false;
    }
    if (!expectedTarget.isEmpty() && cameraLifecycleSnapshot(deviceId) != expectedTarget) {
        if (error)
            *error = QStringLiteral("CAMERA_TARGET_CHANGED");
        return false;
    }
    return requestConnectCameraAsync(deviceId, requestId, error);
}

bool CameraManager::requestDisconnectCameraAsync(const QString& deviceId, quint64* requestId, QString* error) {
    if (requestId)
        *requestId = 0;
    if (!requestId) {
        if (error)
            *error = QStringLiteral("CAMERA_OPERATION_REQUEST_ID_REQUIRED");
        return false;
    }
    if (QThread::currentThread() != thread()) {
        if (error)
            *error = QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY");
        return false;
    }
    if (m_shuttingDown) {
        if (error)
            *error = QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN");
        return false;
    }
    const quint64 allocatedId = nextManagedRequestId();
    if (allocatedId == 0) {
        if (error)
            *error = QStringLiteral("CAMERA_OPERATION_ID_EXHAUSTED");
        return false;
    }
    QString submitError;
    if (!disconnectCameraAsync(deviceId, allocatedId, &submitError)) {
        if (error)
            *error = submitError;
        return false;
    }
    *requestId = allocatedId;
    if (error)
        error->clear();
    return true;
}

bool CameraManager::requestDisconnectCameraAsync(const QString& deviceId, const QJsonObject& expectedTarget,
                                                 quint64* requestId, QString* error) {
    if (requestId)
        *requestId = 0;
    if (QThread::currentThread() != thread()) {
        if (error)
            *error = QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY");
        return false;
    }
    if (!expectedTarget.isEmpty() && cameraLifecycleSnapshot(deviceId) != expectedTarget) {
        if (error)
            *error = QStringLiteral("CAMERA_TARGET_CHANGED");
        return false;
    }
    return requestDisconnectCameraAsync(deviceId, requestId, error);
}

QJsonObject CameraManager::cameraLifecycleSnapshot(const QString& deviceId) const {
    if (QThread::currentThread() != thread())
        return {{QStringLiteral("status"), QStringLiteral("error")},
                {QStringLiteral("errorCode"), QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY")}};
    const quint64 parameterGeneration = cameraParameterBindingVersion(deviceId);
    QMutexLocker locker(&m_mutex);
    const bool found = m_deviceOwners.contains(deviceId) && m_discoveredCameraInfo.contains(deviceId);
    const CameraInfo info = m_discoveredCameraInfo.value(deviceId);
    const CameraState state = m_cameraStatuses.value(deviceId).state;
    CameraOperationSession* session = m_cameraSessions.value(deviceId, nullptr);
    return {{QStringLiteral("found"), found},
            {QStringLiteral("deviceId"), deviceId},
            {QStringLiteral("ownerPluginId"), m_deviceOwners.value(deviceId)},
            {QStringLiteral("name"), info.name},
            {QStringLiteral("serialNumber"), info.serialNumber},
            {QStringLiteral("ipAddress"), info.ipAddress},
            {QStringLiteral("state"), cameraStateName(state)},
            {QStringLiteral("deviceGeneration"), QString::number(m_deviceGenerations.value(deviceId))},
            {QStringLiteral("sessionGeneration"), session ? QString::number(session->generation()) : QString()},
            {QStringLiteral("hasSession"), session != nullptr},
            {QStringLiteral("hasLegacyCamera"), m_cameras.contains(deviceId)},
            {QStringLiteral("operationPending"), m_deviceOperations.contains(deviceId)},
            {QStringLiteral("parameterBindingGeneration"), QString::number(parameterGeneration)}};
}

bool CameraManager::requestCameraDetailsAsync(const QString& deviceId, quint64* requestId, QString* error) {
    if (requestId)
        *requestId = 0;
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (!requestId)
        return fail(QStringLiteral("CAMERA_OPERATION_REQUEST_ID_REQUIRED"));
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_THREAD_AFFINITY"));
    if (m_shuttingDown)
        return fail(QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
    if (m_disconnectAllInProgress)
        return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));
    if (deviceId.isEmpty())
        return fail(QStringLiteral("CAMERA_DEVICE_ID_REQUIRED"));
    if (!m_deviceOwners.contains(deviceId))
        return fail(QStringLiteral("CAMERA_NOT_FOUND"));
    if (m_deviceOperations.contains(deviceId))
        return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));

    CameraOperationSession* session = m_cameraSessions.value(deviceId, nullptr);
    if (!session || !supportsAsyncOperations(deviceId))
        return fail(QStringLiteral("CAMERA_SESSION_REQUIRED"));
    const CameraState state = m_cameraStatuses.value(deviceId).state;
    if ((state != CameraState::Connected && state != CameraState::Grabbing) || !session->isConnected())
        return fail(QStringLiteral("CAMERA_NOT_CONNECTED"));

    const quint64 allocatedId = nextManagedRequestId();
    if (allocatedId == 0)
        return fail(QStringLiteral("CAMERA_OPERATION_ID_EXHAUSTED"));
    const QString pluginId = session->pluginId();
    const quint64 generation = session->generation();
    m_pendingCameraOperations.insert(
        allocatedId,
        PendingCameraOperation{
            deviceId, pluginId, generation, QStringLiteral("get_details"), {}, QJsonValue(QJsonValue::Undefined)});
    m_deviceOperations.insert(deviceId, allocatedId);
    emit cameraOperationStarted(deviceId, allocatedId, QStringLiteral("get_details"));
    *requestId = allocatedId;
    if (!session->requestDetails(allocatedId)) {
        finishCameraDetailsQuery(deviceId, pluginId, generation, allocatedId, QStringLiteral("error"),
                                 QStringLiteral("CAMERA_DETAILS_QUEUE_FAILED"), {});
    }
    if (error)
        error->clear();
    return true;
}

bool CameraManager::requestCameraParameterWriteAsync(const QString& deviceId, quint64 expectedBindingVersion,
                                                     const QString& serialNumber, const QString& parameter,
                                                     const QJsonValue& value, quint64* requestId, QString* error) {
    if (requestId)
        *requestId = 0;
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (!requestId)
        return fail(QStringLiteral("CAMERA_OPERATION_REQUEST_ID_REQUIRED"));
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_THREAD_AFFINITY"));
    if (m_shuttingDown)
        return fail(QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
    if (m_disconnectAllInProgress || m_deviceOperations.contains(deviceId))
        return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));
    if (!m_deviceOwners.contains(deviceId))
        return fail(QStringLiteral("CAMERA_NOT_FOUND"));
    const auto parameterBinding = cameraParameterBindingFor(deviceId);
    if (!parameterBinding || parameterBinding->generation() != expectedBindingVersion)
        return fail(QStringLiteral("CAMERA_TARGET_CHANGED"));
    CameraOperationSession* session = m_cameraSessions.value(deviceId, nullptr);
    if (!session || !supportsAsyncOperations(deviceId))
        return fail(QStringLiteral("CAMERA_SESSION_REQUIRED"));
    const CameraState state = m_cameraStatuses.value(deviceId).state;
    if ((state != CameraState::Connected && state != CameraState::Grabbing) || !session->isConnected())
        return fail(QStringLiteral("CAMERA_NOT_CONNECTED"));

    const quint64 allocatedId = nextManagedRequestId();
    if (allocatedId == 0)
        return fail(QStringLiteral("CAMERA_OPERATION_ID_EXHAUSTED"));
    const QString pluginId = session->pluginId();
    const quint64 sessionGeneration = session->generation();
    m_pendingCameraOperations.insert(allocatedId,
                                     PendingCameraOperation{deviceId, pluginId, sessionGeneration,
                                                            QStringLiteral("set_parameter"), parameter, value});
    m_deviceOperations.insert(deviceId, allocatedId);
    emit cameraOperationStarted(deviceId, allocatedId, QStringLiteral("set_parameter"));
    *requestId = allocatedId;
    if (!session->requestSetParameter(allocatedId, expectedBindingVersion, serialNumber, parameter, value)) {
        finishCameraParameterWrite(
            deviceId, pluginId, sessionGeneration, allocatedId,
            QJsonObject{{QStringLiteral("errorCode"), QStringLiteral("CAMERA_PARAMETER_QUEUE_FAILED")}});
    }
    if (error)
        error->clear();
    return true;
}

bool CameraManager::getCameraOperationResult(quint64 requestId, QJsonObject* result) const {
    if (result)
        *result = QJsonObject();
    if (!result || requestId == 0 || QThread::currentThread() != thread())
        return false;
    const auto completed = m_cameraOperationResults.constFind(requestId);
    if (completed != m_cameraOperationResults.cend()) {
        *result = completed.value();
        return true;
    }
    const auto pending = m_pendingCameraOperations.constFind(requestId);
    if (pending == m_pendingCameraOperations.cend())
        return false;
    *result = QJsonObject{{QStringLiteral("requestId"), QString::number(requestId)},
                          {QStringLiteral("deviceId"), pending->deviceId},
                          {QStringLiteral("operation"), pending->operation},
                          {QStringLiteral("status"), QStringLiteral("pending")}};
    return true;
}

bool CameraManager::hasAsyncCameraSession(const QString& deviceId) const {
    if (QThread::currentThread() == thread())
        return m_cameraSessions.contains(deviceId) && m_cameraSessions.value(deviceId) &&
               supportsAsyncOperations(deviceId);
    bool result = false;
    const bool invoked = QMetaObject::invokeMethod(
        const_cast<CameraManager*>(this),
        [this, &deviceId, &result]() {
            result = m_cameraSessions.contains(deviceId) && m_cameraSessions.value(deviceId) &&
                     supportsAsyncOperations(deviceId);
        },
        Qt::BlockingQueuedConnection);
    return invoked && result;
}

#ifdef DEEPLUX_ENABLE_TEST_HOOKS
void CameraManager::setAsyncConnectTimeoutForTesting(int timeoutMs) {
    if (QThread::currentThread() == thread() && timeoutMs > 0)
        m_asyncConnectTimeoutMs = timeoutMs;
}

void CameraManager::setAsyncDisconnectTimeoutForTesting(int timeoutMs) {
    if (QThread::currentThread() == thread() && timeoutMs > 0)
        m_asyncDisconnectTimeoutMs = timeoutMs;
}
#endif

CameraCaptureResult CameraManager::captureAsyncSessionImage(const QString& deviceId, double exposureTime, double gain,
                                                            int timeoutMs, const std::function<bool()>& isCancelled) {
    CameraCaptureResult immediate;
    if (deviceId.isEmpty()) {
        immediate.errorCode = QStringLiteral("CAMERA_NOT_FOUND");
        return immediate;
    }
    if (timeoutMs <= 0) {
        immediate.errorCode = QStringLiteral("CAMERA_GRAB_TIMEOUT");
        return immediate;
    }

    auto waitState = QSharedPointer<CameraCaptureWaitState>::create();
    quint64 requestId = 0;
    QString startError;
    auto begin = [this, &deviceId, exposureTime, gain, &waitState, &requestId, &startError]() {
        beginCameraCapture(deviceId, exposureTime, gain, waitState, &requestId, &startError);
    };
    if (QThread::currentThread() == thread()) {
        begin();
    } else if (!QMetaObject::invokeMethod(this, begin, Qt::BlockingQueuedConnection)) {
        immediate.errorCode = QStringLiteral("CAMERA_MANAGER_UNAVAILABLE");
        return immediate;
    }
    if (!startError.isEmpty()) {
        immediate.errorCode = startError;
        return immediate;
    }

    QElapsedTimer deadline;
    deadline.start();
    auto completeWait = [&waitState](const QString& errorCode) {
        QMutexLocker locker(&waitState->mutex);
        if (waitState->completed)
            return false;
        waitState->result.errorCode = errorCode;
        waitState->completed = true;
        waitState->condition.wakeAll();
        return true;
    };
    auto requestCancel = [this, requestId](const QString& errorCode) {
        auto cancel = [this, requestId, errorCode]() { cancelCameraCapture(requestId, errorCode); };
        if (QThread::currentThread() == thread())
            cancel();
        else
            QMetaObject::invokeMethod(this, cancel, Qt::QueuedConnection);
    };

    if (QThread::currentThread() == thread()) {
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(10);
        connect(&poll, &QTimer::timeout, &loop, [&]() {
            {
                QMutexLocker locker(&waitState->mutex);
                if (waitState->completed) {
                    loop.quit();
                    return;
                }
            }
            if (isCancelled && isCancelled()) {
                if (completeWait(QStringLiteral("CAMERA_GRAB_CANCELLED")))
                    requestCancel(QStringLiteral("CAMERA_GRAB_CANCELLED"));
                loop.quit();
            } else if (deadline.elapsed() >= timeoutMs) {
                if (completeWait(QStringLiteral("CAMERA_GRAB_TIMEOUT")))
                    requestCancel(QStringLiteral("CAMERA_GRAB_TIMEOUT"));
                loop.quit();
            }
        });
        poll.start();
        bool alreadyCompleted = false;
        {
            QMutexLocker locker(&waitState->mutex);
            alreadyCompleted = waitState->completed;
        }
        if (!alreadyCompleted)
            loop.exec(QEventLoop::AllEvents);
        poll.stop();
    } else {
        QMutexLocker locker(&waitState->mutex);
        while (!waitState->completed) {
            if (isCancelled && isCancelled()) {
                locker.unlock();
                if (completeWait(QStringLiteral("CAMERA_GRAB_CANCELLED")))
                    requestCancel(QStringLiteral("CAMERA_GRAB_CANCELLED"));
                locker.relock();
                break;
            }
            const qint64 remaining = timeoutMs - deadline.elapsed();
            if (remaining <= 0) {
                locker.unlock();
                if (completeWait(QStringLiteral("CAMERA_GRAB_TIMEOUT")))
                    requestCancel(QStringLiteral("CAMERA_GRAB_TIMEOUT"));
                locker.relock();
                break;
            }
            waitState->condition.wait(&waitState->mutex, static_cast<unsigned long>(std::min<qint64>(remaining, 25)));
        }
    }

    QMutexLocker locker(&waitState->mutex);
    return waitState->result;
}

bool CameraManager::beginCameraCapture(const QString& deviceId, double exposureTime, double gain,
                                       const QSharedPointer<CameraCaptureWaitState>& waitState, quint64* requestId,
                                       QString* errorCode) {
    auto fail = [errorCode](const QString& code) {
        if (errorCode)
            *errorCode = code;
        return false;
    };
    if (requestId)
        *requestId = 0;
    if (errorCode)
        errorCode->clear();
    if (QThread::currentThread() != thread())
        return fail(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    if (m_shuttingDown)
        return fail(QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
    if (!m_deviceOwners.contains(deviceId))
        return fail(QStringLiteral("CAMERA_NOT_FOUND"));
    CameraOperationSession* session = m_cameraSessions.value(deviceId, nullptr);
    if (!supportsAsyncOperations(deviceId))
        return fail(QStringLiteral("CAMERA_SESSION_REQUIRED"));
    if (!session)
        return fail(QStringLiteral("CAMERA_NOT_CONNECTED"));
    if (m_deviceOperations.contains(deviceId) || m_disconnectAllInProgress || session->isBusy())
        return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));
    if (!session->isConnected())
        return fail(QStringLiteral("CAMERA_NOT_CONNECTED"));
    const quint64 id = nextManagedRequestId();
    if (id == 0)
        return fail(QStringLiteral("CAMERA_OPERATION_ID_EXHAUSTED"));
    const QString pluginId = session->pluginId();
    const quint64 generation = session->generation();
    m_pendingCameraCaptures.insert(id, PendingCameraCapture{deviceId, pluginId, generation, session, waitState});
    if (!session->requestCapture(id, exposureTime, gain)) {
        m_pendingCameraCaptures.remove(id);
        return fail(QStringLiteral("CAMERA_CAPTURE_QUEUE_FAILED"));
    }
    if (requestId)
        *requestId = id;
    return true;
}

void CameraManager::finishCameraCapture(const QString& deviceId, const QString& pluginId, quint64 generation,
                                        quint64 requestId, const QImage& image, const QString& errorCode,
                                        const QJsonObject& diagnostics) {
    const auto it = m_pendingCameraCaptures.find(requestId);
    if (it == m_pendingCameraCaptures.end() || it->deviceId != deviceId || it->pluginId != pluginId ||
        it->generation != generation)
        return;
    const auto waitState = it->waitState;
    m_pendingCameraCaptures.erase(it);
    {
        QMutexLocker locker(&waitState->mutex);
        if (!waitState->completed) {
            waitState->result.image = errorCode.isEmpty() ? image : QImage();
            waitState->result.errorCode = errorCode;
            waitState->result.diagnostics = diagnostics;
            waitState->completed = true;
            waitState->condition.wakeAll();
        }
    }
    if (m_shuttingDown)
        QMetaObject::invokeMethod(
            this, [this]() { shutdownAsync(); }, Qt::QueuedConnection);
    else if (m_disconnectAllInProgress)
        QMetaObject::invokeMethod(
            this, [this]() { startIdleSessionDisconnects(); }, Qt::QueuedConnection);
}

void CameraManager::finishCameraCaptureSessionRetired(const QString& deviceId, const QString& pluginId,
                                                      quint64 generation) {
    CameraOperationSession* session = m_cameraSessions.value(deviceId, nullptr);
    if (!session || session->pluginId() != pluginId || session->generation() != generation)
        return;
    {
        QMutexLocker locker(&m_mutex);
        if (m_cameraSessions.value(deviceId) != session)
            return;
        m_cameraSessions.remove(deviceId);
        ++m_deviceGenerations[deviceId];
        m_cameraStatuses[deviceId].state = CameraState::Disconnected;
        m_cameraStatuses[deviceId].lastError.clear();
    }
    invalidateCameraParameterBinding(deviceId);
    emit cameraStateChanged(deviceId, CameraState::Disconnected);
    emit cameraDisconnected(deviceId);
    session->deleteLater();
    if (m_shuttingDown)
        shutdownAsync();
    else if (m_disconnectAllInProgress)
        startIdleSessionDisconnects();
    maybeFinishShutdown();
}

void CameraManager::cancelCameraCapture(quint64 requestId, const QString& errorCode) {
    const auto it = m_pendingCameraCaptures.constFind(requestId);
    if (it == m_pendingCameraCaptures.cend())
        return;
    if (it->waitState) {
        QMutexLocker locker(&it->waitState->mutex);
        if (!it->waitState->completed) {
            it->waitState->result.errorCode = errorCode.isEmpty() ? QStringLiteral("CAMERA_GRAB_CANCELLED") : errorCode;
            it->waitState->completed = true;
            it->waitState->condition.wakeAll();
        }
    }
    if (it->session)
        it->session->cancelCapture(requestId, errorCode);
}

bool CameraManager::supportsAsyncOperations(const QString& deviceId) const {
    if (QThread::currentThread() != thread() || deviceId.isEmpty())
        return false;
    ICameraPlugin* plugin = m_plugins.value(m_deviceOwners.value(deviceId), nullptr);
    return plugin && plugin->thread() == thread() && qobject_cast<ICameraDiscoveryAsync*>(plugin);
}

bool CameraManager::hasPendingCameraOperation(const QString& deviceId) const {
    if (QThread::currentThread() != thread() || deviceId.isEmpty())
        return false;
    return m_deviceOperations.contains(deviceId);
}

bool CameraManager::pendingCameraOperation(const QString& deviceId, CameraOperationSnapshot* snapshot) const {
    if (snapshot)
        *snapshot = CameraOperationSnapshot{};
    if (!snapshot || QThread::currentThread() != thread() || deviceId.isEmpty())
        return false;
    const quint64 requestId = m_deviceOperations.value(deviceId, 0);
    const auto pending = m_pendingCameraOperations.constFind(requestId);
    if (requestId == 0 || pending == m_pendingCameraOperations.cend() || pending->deviceId != deviceId)
        return false;
    snapshot->requestId = requestId;
    snapshot->operation = pending->operation;
    return true;
}

int CameraManager::cameraSessionCount() const {
    QMutexLocker locker(&m_mutex);
    return m_cameraSessions.size();
}

bool CameraManager::beginSessionDisconnect(const QString& deviceId, quint64 requestId, QString* error) {
    auto fail = [error](const QString& code) {
        if (error)
            *error = code;
        return false;
    };
    if (requestId == 0)
        return fail(QStringLiteral("CAMERA_OPERATION_REQUEST_ID_INVALID"));
    if (requestIdInUse(requestId))
        return fail(QStringLiteral("CAMERA_OPERATION_REQUEST_ID_IN_USE"));
    CameraOperationSession* session = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        if (m_deviceOperations.contains(deviceId))
            return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));
        session = m_cameraSessions.value(deviceId, nullptr);
        if (!session)
            return fail(QStringLiteral("CAMERA_OPERATION_SESSION_NOT_FOUND"));
        if (session->isBusy())
            return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));
        const PendingCameraOperation pending{deviceId,
                                             session->pluginId(),
                                             session->generation(),
                                             QStringLiteral("disconnect"),
                                             {},
                                             QJsonValue(QJsonValue::Undefined)};
        m_pendingCameraOperations.insert(requestId, pending);
        m_deviceOperations.insert(deviceId, requestId);
        CameraStatus& status = m_cameraStatuses[deviceId];
        status.deviceId = deviceId;
        status.lastError.clear();
    }
    emit cameraOperationStarted(deviceId, requestId, QStringLiteral("disconnect"));
    if (!session->startDisconnect(requestId, m_asyncDisconnectTimeoutMs)) {
        QMutexLocker locker(&m_mutex);
        m_pendingCameraOperations.remove(requestId);
        m_deviceOperations.remove(deviceId);
        return fail(QStringLiteral("CAMERA_OPERATION_BUSY"));
    }
    emit cameraStateChanged(deviceId, cameraState(deviceId));
    return true;
}

void CameraManager::handleCameraOperationFinished(const QString& deviceId, const QString& pluginId, quint64 generation,
                                                  quint64 requestId, const QString& operation, const QString& status,
                                                  const QString& errorCode, bool retained) {
    const auto pending = m_pendingCameraOperations.constFind(requestId);
    if (pending == m_pendingCameraOperations.cend() || pending->deviceId != deviceId || pending->pluginId != pluginId ||
        pending->generation != generation || pending->operation != operation)
        return;

    const bool stale = m_deviceGenerations.value(deviceId) != generation;
    const QString finalStatus = stale && operation == QLatin1String("connect") ? QStringLiteral("cancelled") : status;
    const QString finalError =
        stale && operation == QLatin1String("connect") ? QStringLiteral("CAMERA_OPERATION_CANCELLED") : errorCode;
    finishCameraOperation(requestId, finalStatus, finalError, m_cameraSessions.value(deviceId, nullptr), retained);
}

void CameraManager::finishCameraOperation(quint64 requestId, const QString& status, const QString& errorCode,
                                          CameraOperationSession* session, bool retained) {
    const auto pending = m_pendingCameraOperations.constFind(requestId);
    if (pending == m_pendingCameraOperations.cend())
        return;
    const PendingCameraOperation operation = pending.value();
    const QString deviceId = operation.deviceId;
    const QString operationName = operation.operation;
    bool removedSession = false;
    {
        QMutexLocker locker(&m_mutex);
        m_pendingCameraOperations.remove(requestId);
        if (m_deviceOperations.value(deviceId) == requestId)
            m_deviceOperations.remove(deviceId);
        if (status == QLatin1String("success")) {
            m_cameraStatuses[deviceId].state =
                operationName == QLatin1String("connect") ? CameraState::Connected : CameraState::Disconnected;
            m_cameraStatuses[deviceId].lastError.clear();
            if (operationName == QLatin1String("disconnect"))
                ++m_deviceGenerations[deviceId];
        } else if (status == QLatin1String("cancelled") && retained) {
            m_cameraStatuses[deviceId].state = CameraState::Error;
            m_cameraStatuses[deviceId].lastError = errorCode;
        } else if (status == QLatin1String("cancelled")) {
            m_cameraStatuses[deviceId].state = CameraState::Disconnected;
            m_cameraStatuses[deviceId].lastError = errorCode;
        } else {
            m_cameraStatuses[deviceId].state = retained ? CameraState::Error : CameraState::Disconnected;
            m_cameraStatuses[deviceId].lastError = errorCode;
        }
        if (!retained && session && m_cameraSessions.value(deviceId) == session) {
            m_cameraSessions.remove(deviceId);
            removedSession = true;
        }
        const CameraState currentState = m_cameraStatuses.value(deviceId).state;
        QJsonObject operationResult{{QStringLiteral("requestId"), QString::number(requestId)},
                                    {QStringLiteral("deviceId"), deviceId},
                                    {QStringLiteral("operation"), operationName},
                                    {QStringLiteral("status"), status},
                                    {QStringLiteral("errorCode"), errorCode},
                                    {QStringLiteral("cameraState"), static_cast<int>(currentState)},
                                    {QStringLiteral("isConnected"), currentState == CameraState::Connected}};
        if (operationName == QLatin1String("connect") && status != QLatin1String("success") && retained) {
            operationResult.insert(QStringLiteral("sideEffectStatus"), QStringLiteral("uncertain"));
            operationResult.insert(QStringLiteral("retryable"), false);
            operationResult.insert(QStringLiteral("connectionState"), QStringLiteral("uncertain"));
        } else if (operationName == QLatin1String("disconnect") && status == QLatin1String("success")) {
            operationResult.insert(QStringLiteral("connectionState"), QStringLiteral("disconnected"));
        } else if (operationName == QLatin1String("disconnect") && retained &&
                   (errorCode == QLatin1String("CAMERA_DISCONNECT_UNCONFIRMED") ||
                    errorCode == QLatin1String("CAMERA_DISCONNECT_TIMEOUT_UNCONFIRMED"))) {
            operationResult.insert(QStringLiteral("sideEffectStatus"), QStringLiteral("uncertain"));
            operationResult.insert(QStringLiteral("retryable"), false);
            operationResult.insert(QStringLiteral("connectionState"), QStringLiteral("uncertain"));
        }
        m_cameraOperationResults.insert(requestId, operationResult);
        m_cameraOperationResultOrder.removeAll(requestId);
        m_cameraOperationResultOrder.append(requestId);
        if (m_cameraOperationResultOrder.size() > 256) {
            const quint64 expiredRequestId = m_cameraOperationResultOrder.takeFirst();
            m_cameraOperationResults.remove(expiredRequestId);
        }
        if (m_disconnectAllInProgress && m_disconnectAllSweepComplete && m_pendingDisconnects.isEmpty() &&
            m_cameraLeaseCounts.isEmpty() && m_deviceOperations.isEmpty() && !hasBusyCameraSession()) {
            m_disconnectAllInProgress = false;
            m_disconnectAllSweepComplete = false;
        }
    }
    if (removedSession && session)
        session->deleteLater();

    if (operationName == QLatin1String("connect") || operationName == QLatin1String("disconnect"))
        invalidateCameraParameterBinding(deviceId);

    const CameraState state = cameraState(deviceId);
    emit cameraStateChanged(deviceId, state);
    if (status == QLatin1String("success")) {
        if (operationName == QLatin1String("connect"))
            emit cameraConnected(deviceId);
        else
            emit cameraDisconnected(deviceId);
    }
    emit cameraOperationSettled(deviceId, requestId, operationName, status, errorCode);
    maybeFinishShutdown();
}

void CameraManager::finishCameraDetailsQuery(const QString& deviceId, const QString& pluginId, quint64 generation,
                                             quint64 requestId, const QString& status, const QString& errorCode,
                                             const QJsonObject& details) {
    const auto pending = m_pendingCameraOperations.constFind(requestId);
    if (pending == m_pendingCameraOperations.cend() || pending->deviceId != deviceId || pending->pluginId != pluginId ||
        pending->generation != generation || pending->operation != QLatin1String("get_details"))
        return;

    const QString finalStatus =
        status == QLatin1String("success") && !details.isEmpty() ? status : QStringLiteral("error");
    const QString finalError = finalStatus == QLatin1String("success")
                                   ? QString()
                                   : (errorCode.isEmpty() ? QStringLiteral("CAMERA_DETAILS_READ_FAILED") : errorCode);
    m_pendingCameraOperations.remove(requestId);
    if (m_deviceOperations.value(deviceId) == requestId)
        m_deviceOperations.remove(deviceId);

    QJsonObject result{{QStringLiteral("requestId"), QString::number(requestId)},
                       {QStringLiteral("deviceId"), deviceId},
                       {QStringLiteral("operation"), QStringLiteral("get_details")},
                       {QStringLiteral("status"), finalStatus},
                       {QStringLiteral("errorCode"), finalError}};
    if (finalStatus == QLatin1String("success")) {
        QJsonObject versionedDetails = details;
        versionedDetails.insert(QStringLiteral("cameraSessionGeneration"),
                                QString::number(cameraParameterBindingVersion(deviceId)));
        result.insert(QStringLiteral("details"), versionedDetails);
    }
    m_cameraOperationResults.insert(requestId, result);
    m_cameraOperationResultOrder.removeAll(requestId);
    m_cameraOperationResultOrder.append(requestId);
    if (m_cameraOperationResultOrder.size() > 256) {
        const quint64 expiredRequestId = m_cameraOperationResultOrder.takeFirst();
        m_cameraOperationResults.remove(expiredRequestId);
    }

    emit cameraOperationSettled(deviceId, requestId, QStringLiteral("get_details"), finalStatus, finalError);
    if (m_shuttingDown) {
        QMetaObject::invokeMethod(
            this, [this]() { shutdownAsync(); }, Qt::QueuedConnection);
    } else if (m_disconnectAllInProgress) {
        QMetaObject::invokeMethod(
            this, [this]() { startIdleSessionDisconnects(); }, Qt::QueuedConnection);
    }
    maybeFinishShutdown();
}

void CameraManager::finishCameraParameterWrite(const QString& deviceId, const QString& pluginId, quint64 generation,
                                               quint64 requestId, const QJsonObject& writeResult) {
    const auto pending = m_pendingCameraOperations.constFind(requestId);
    if (pending == m_pendingCameraOperations.cend() || pending->deviceId != deviceId || pending->pluginId != pluginId ||
        pending->generation != generation || pending->operation != QLatin1String("set_parameter"))
        return;
    const PendingCameraOperation pendingOperation = pending.value();
    m_pendingCameraOperations.remove(requestId);
    if (m_deviceOperations.value(deviceId) == requestId)
        m_deviceOperations.remove(deviceId);

    QJsonObject result = writeResult;
    const auto parameterBinding = cameraParameterBindingFor(deviceId);
    const quint64 bindingVersion = parameterBinding ? parameterBinding->generation() : 0;
    result.remove(QStringLiteral("setterCalled"));
    result.insert(QStringLiteral("requestId"), QString::number(requestId));
    result.insert(QStringLiteral("deviceId"), deviceId);
    result.insert(QStringLiteral("operation"), QStringLiteral("set_parameter"));
    if (!result.contains(QStringLiteral("parameter")))
        result.insert(QStringLiteral("parameter"), pendingOperation.parameter);
    if (!result.contains(QStringLiteral("requested")) && !pendingOperation.requested.isUndefined())
        result.insert(QStringLiteral("requested"), pendingOperation.requested);
    if (!result.contains(QStringLiteral("sideEffectStatus")))
        result.insert(QStringLiteral("sideEffectStatus"), QStringLiteral("none"));
    result.insert(QStringLiteral("cameraSessionGeneration"), QString::number(bindingVersion));
    const QString errorCode = result.value(QStringLiteral("errorCode")).toString();
    if (errorCode.isEmpty())
        result.insert(QStringLiteral("status"), QStringLiteral("success"));
    else {
        result.insert(QStringLiteral("status"), QStringLiteral("error"));
        result.insert(QStringLiteral("retryable"), false);
    }
    m_cameraOperationResults.insert(requestId, result);
    m_cameraOperationResultOrder.removeAll(requestId);
    m_cameraOperationResultOrder.append(requestId);
    if (m_cameraOperationResultOrder.size() > 256) {
        const quint64 expiredRequestId = m_cameraOperationResultOrder.takeFirst();
        m_cameraOperationResults.remove(expiredRequestId);
    }
    emit cameraOperationSettled(deviceId, requestId, QStringLiteral("set_parameter"),
                                result.value(QStringLiteral("status")).toString(), errorCode);
    if (m_shuttingDown) {
        QMetaObject::invokeMethod(
            this, [this]() { shutdownAsync(); }, Qt::QueuedConnection);
    } else if (m_disconnectAllInProgress) {
        QMetaObject::invokeMethod(
            this, [this]() { startIdleSessionDisconnects(); }, Qt::QueuedConnection);
    }
    maybeFinishShutdown();
}

quint64 CameraManager::nextManagedRequestId() {
    quint64 requestId = m_nextManagedRequestId;
    while (requestId != 0 && requestIdInUse(requestId)) {
        if (requestId == 1)
            return 0;
        --requestId;
    }
    if (requestId == 0)
        return 0;
    m_nextManagedRequestId = requestId - 1;
    return requestId;
}

bool CameraManager::requestIdInUse(quint64 requestId) const {
    return requestId == 0 || m_pendingDiscoveries.contains(requestId) ||
           m_pendingCameraOperations.contains(requestId) || m_pendingCameraCaptures.contains(requestId) ||
           m_cameraOperationResults.contains(requestId) || m_pendingAgentDiscoveryBatches.contains(requestId) ||
           m_agentDiscoveryResults.contains(requestId) || m_reservedCameraDiscoveryChildIds.contains(requestId);
}

void CameraManager::maybeFinishShutdown() {
    if (!m_shuttingDown || m_shutdownSignalSent || !m_pendingDiscoveries.isEmpty() ||
        !m_pendingAgentDiscoveryBatches.isEmpty() || !m_pendingCameraOperations.isEmpty() ||
        !m_pendingCameraCaptures.isEmpty() || !m_cameraSessions.isEmpty())
        return;
    m_shutdownSignalSent = true;
    emit shutdownCompleted();
}

void CameraManager::startIdleSessionDisconnects() {
    if (QThread::currentThread() != thread())
        return;
    const auto sessions = m_cameraSessions;
    for (auto it = sessions.cbegin(); it != sessions.cend(); ++it) {
        CameraOperationSession* session = it.value();
        if (!session || session->isBusy())
            continue;
        QString error;
        const quint64 requestId = nextManagedRequestId();
        if (requestId == 0 || !beginSessionDisconnect(it.key(), requestId, &error))
            Logger::instance().warning(QString("Camera session disconnect rejected (%1): %2").arg(it.key(), error),
                                       "Camera");
    }
    if (m_disconnectAllInProgress && m_disconnectAllSweepComplete && m_pendingDisconnects.isEmpty() &&
        m_deviceOperations.isEmpty() && !hasBusyCameraSession()) {
        m_disconnectAllInProgress = false;
        m_disconnectAllSweepComplete = false;
    }
}

bool CameraManager::hasBusyCameraSession() const {
    for (auto it = m_cameraSessions.cbegin(); it != m_cameraSessions.cend(); ++it) {
        if (it.value() && it.value()->isBusy())
            return true;
    }
    return false;
}

void CameraManager::handleStaleDisconnectWorkerIdle(const QString& deviceId, const QString& pluginId,
                                                    quint64 generation) {
    if (QThread::currentThread() != thread())
        return;
    CameraOperationSession* session = m_cameraSessions.value(deviceId, nullptr);
    if (!session || session->pluginId() != pluginId || session->generation() != generation)
        return;
    if (m_shuttingDown)
        shutdownAsync();
    else if (m_disconnectAllInProgress)
        startIdleSessionDisconnects();
    maybeFinishShutdown();
}

void CameraManager::shutdownAsync() {
    if (QThread::currentThread() != thread())
        return;
    if (!m_shuttingDown) {
        m_shuttingDown = true;
        m_shutdownSignalSent = false;
    }
    const bool hasWork = !m_pendingDiscoveries.isEmpty() || !m_pendingAgentDiscoveryBatches.isEmpty() ||
                         !m_pendingCameraOperations.isEmpty() || !m_pendingCameraCaptures.isEmpty() ||
                         !m_cameraSessions.isEmpty();
    if (!hasWork) {
        maybeFinishShutdown();
        return;
    }
    for (auto it = m_pendingCameraOperations.cbegin(); it != m_pendingCameraOperations.cend(); ++it) {
        if (it->operation != QLatin1String("connect"))
            continue;
        ++m_deviceGenerations[it->deviceId];
        CameraOperationSession* session = m_cameraSessions.value(it->deviceId, nullptr);
        if (session)
            session->cancelConnectForShutdown();
    }
    const auto captureIds = m_pendingCameraCaptures.keys();
    for (quint64 requestId : captureIds)
        cancelCameraCapture(requestId, QStringLiteral("CAMERA_GRAB_CANCELLED"));
    const auto requestIds = m_pendingDiscoveries.keys();
    for (quint64 requestId : requestIds)
        requestDiscoveryCancel(requestId);

    startIdleSessionDisconnects();
    maybeFinishShutdown();
}

void CameraManager::handleDiscoveryCompleted(quint64 expectedRequestId, quint64 signaledRequestId,
                                             const QByteArray& payload) {
    auto it = m_pendingDiscoveries.find(expectedRequestId);
    if (it == m_pendingDiscoveries.end() || m_terminalDiscoveries.contains(expectedRequestId))
        return;
    m_terminalDiscoveries.insert(expectedRequestId);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    QJsonObject result;
    bool requestIdMismatch = signaledRequestId != expectedRequestId;
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result = discoveryError(expectedRequestId, QStringLiteral("CAMERA_DISCOVERY_RESULT_INVALID_JSON"));
    } else {
        result = document.object();
        const QJsonValue payloadRequestId = result.value(QStringLiteral("requestId"));
        requestIdMismatch = requestIdMismatch || (payloadRequestId.isString() &&
                                                  payloadRequestId.toString() != QString::number(expectedRequestId));
        const QString status = result.value(QStringLiteral("status")).toString();
        bool valid = result.value(QStringLiteral("schemaVersion")).toInt(-1) == 1 && payloadRequestId.isString() &&
                     !requestIdMismatch &&
                     (status == QStringLiteral("success") || status == QStringLiteral("error") ||
                      status == QStringLiteral("cancelled"));
        if (status == QStringLiteral("success")) {
            const QJsonValue cameras = result.value(QStringLiteral("cameras"));
            valid = valid && cameras.isArray();
            QSet<QString> deviceIds;
            if (cameras.isArray()) {
                for (const QJsonValue& camera : cameras.toArray()) {
                    valid = valid && validCameraObject(camera);
                    const QJsonObject cameraObject = camera.toObject();
                    const QString deviceId = cameraObject.value(QStringLiteral("deviceId")).toString();
                    valid = valid && cameraObject.value(QStringLiteral("pluginId")).toString() == it->pluginId &&
                            !deviceIds.contains(deviceId);
                    deviceIds.insert(deviceId);
                }
            }
        } else {
            valid = valid && result.value(QStringLiteral("errorCode")).isString() &&
                    !result.value(QStringLiteral("errorCode")).toString().isEmpty();
        }
        if (!valid)
            result = discoveryError(expectedRequestId, QStringLiteral("CAMERA_DISCOVERY_RESULT_INVALID_JSON"));
    }
    if (requestIdMismatch)
        result = discoveryError(expectedRequestId, QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_MISMATCH"));
    QString terminalStatus = result.value(QStringLiteral("status")).toString();
    QString terminalError = result.value(QStringLiteral("errorCode")).toString();
    if (terminalStatus == QLatin1String("success")) {
        if (applyAsyncDiscoveryResult(it->pluginId, result.value(QStringLiteral("cameras")).toArray()))
            emit cameraListChanged();
    }
    it->terminalStatus = terminalStatus;
    it->errorCode = terminalError;
    emit asyncDiscoveryFinished(expectedRequestId, result);
    if (it->job)
        it->job->deleteLater();
}

void CameraManager::finishDiscoveryAfterJobDestroyed(quint64 requestId) {
    auto it = m_pendingDiscoveries.find(requestId);
    if (it == m_pendingDiscoveries.end())
        return;
    const QString pluginId = it->pluginId;
    const QString terminalStatus = it->terminalStatus.isEmpty() ? QStringLiteral("error") : it->terminalStatus;
    const QString errorCode =
        terminalStatus == QLatin1String("success")
            ? QString()
            : (it->errorCode.isEmpty() ? QStringLiteral("CAMERA_DISCOVERY_JOB_DESTROYED") : it->errorCode);
    const quint64 batchRequestId = m_agentDiscoveryChildToBatch.value(requestId, 0);
    m_pendingDiscoveries.erase(it);
    m_terminalDiscoveries.remove(requestId);
    handleAgentDiscoveryChildSettled(pluginId, requestId, terminalStatus, errorCode, false);
    releaseDiscoveryReservation(pluginId, requestId);
    if (batchRequestId != 0)
        finishAgentDiscoveryBatchIfReady(batchRequestId);
    emit cameraDiscoverySettled(pluginId, requestId, terminalStatus, errorCode);
    maybeFinishShutdown();
}

void CameraManager::startManagedDiscovery(const QString& pluginId) {
    if (QThread::currentThread() != thread() || m_shuttingDown || pluginHasPendingDiscovery(pluginId))
        return;
    const quint64 requestId = nextManagedRequestId();
    if (requestId == 0)
        return;
    QString errorCode;
    bool startFailureSettled = false;
    if (!startDiscoveryAsyncImpl(pluginId, requestId, &startFailureSettled, &errorCode) && !startFailureSettled) {
        emit cameraDiscoveryError(pluginId, errorCode);
        emit cameraDiscoverySettled(pluginId, requestId, QStringLiteral("start_failed"), errorCode);
    }
}

bool CameraManager::pluginHasPendingDiscovery(const QString& pluginId) const {
    for (auto it = m_pendingDiscoveries.cbegin(); it != m_pendingDiscoveries.cend(); ++it)
        if (it->pluginId == pluginId)
            return true;
    return false;
}

bool CameraManager::applyAsyncDiscoveryResult(const QString& pluginId, const QJsonArray& cameras) {
    QMap<QString, CameraInfo> incoming;
    for (const QJsonValue& value : cameras) {
        const QJsonObject object = value.toObject();
        CameraInfo info;
        info.pluginId = object.value(QStringLiteral("pluginId")).toString();
        info.deviceId = object.value(QStringLiteral("deviceId")).toString();
        info.name = object.value(QStringLiteral("name")).toString();
        info.serialNumber = object.value(QStringLiteral("serialNumber")).toString();
        info.ipAddress = object.value(QStringLiteral("ipAddress")).toString();
        info.width = object.value(QStringLiteral("width")).toInt();
        info.height = object.value(QStringLiteral("height")).toInt();
        info.pixelFormat = object.value(QStringLiteral("pixelFormat")).toString();
        info.isConnected = object.value(QStringLiteral("isConnected")).toBool();
        info.rawInfo = object.value(QStringLiteral("rawInfo")).toObject().toVariantMap();
        if (info.pluginId != pluginId || info.deviceId.isEmpty() || incoming.contains(info.deviceId))
            continue;
        incoming.insert(info.deviceId, info);
    }

    bool changed = false;
    QList<QPair<QString, QString>> newlyDiscovered;
    {
        QMutexLocker locker(&m_mutex);
        const QSet<QString> previous = m_asyncPluginDevices.value(pluginId);
        QSet<QString> next;
        for (auto it = incoming.cbegin(); it != incoming.cend(); ++it)
            next.insert(it.key());
        QSet<QString> affected = previous;
        affected.unite(next);
        for (auto it = m_deviceOwners.cbegin(); it != m_deviceOwners.cend(); ++it)
            if (it.value() == pluginId)
                affected.insert(it.key());

        for (const QString& deviceId : previous) {
            if (next.contains(deviceId))
                continue;
            auto candidates = m_asyncDiscoveryCandidates.find(deviceId);
            if (candidates != m_asyncDiscoveryCandidates.end()) {
                candidates->remove(pluginId);
                if (candidates->isEmpty())
                    m_asyncDiscoveryCandidates.erase(candidates);
            }
        }
        for (auto it = incoming.cbegin(); it != incoming.cend(); ++it)
            m_asyncDiscoveryCandidates[it.key()].insert(pluginId, it.value());
        m_asyncPluginDevices.insert(pluginId, next);

        for (const QString& deviceId : affected) {
            const QString oldOwner = m_deviceOwners.value(deviceId);
            const CameraInfo oldInfo = m_discoveredCameraInfo.value(deviceId);
            const bool hadStatus = m_cameraStatuses.contains(deviceId);
            const bool active = m_cameras.contains(deviceId) || m_cameraSessions.contains(deviceId) ||
                                m_cameraLeaseCounts.value(deviceId) > 0;
            const auto candidates = m_asyncDiscoveryCandidates.value(deviceId);

            if (candidates.isEmpty()) {
                if (active && !oldOwner.isEmpty())
                    continue;
                if (m_asyncDiscoveryPlugins.contains(oldOwner)) {
                    m_deviceOwners.remove(deviceId);
                    m_discoveredCameraInfo.remove(deviceId);
                    m_cameraStatuses.remove(deviceId);
                    changed = changed || hadStatus;
                }
                continue;
            }

            QString owner = oldOwner;
            if (active && !owner.isEmpty()) {
                // Keep the owner of a live QObject/lease even if it is no longer reported.
                if (incoming.contains(deviceId) && incoming.value(deviceId).pluginId == owner) {
                    if (!sameCameraInfo(oldInfo, incoming.value(deviceId)))
                        changed = true;
                    m_discoveredCameraInfo.insert(deviceId, incoming.value(deviceId));
                    if (m_cameraStatuses.contains(deviceId))
                        m_cameraStatuses[deviceId].name = incoming.value(deviceId).name;
                }
                continue;
            }
            if (owner.isEmpty() || m_asyncDiscoveryPlugins.contains(owner))
                owner = candidates.firstKey(); // QMap ordering makes async duplicate ownership deterministic.
            else
                continue; // An existing legacy owner retains priority.

            const CameraInfo info = candidates.value(owner);
            const bool newDevice = !hadStatus;
            if (oldOwner != owner || !sameCameraInfo(oldInfo, info) || newDevice)
                changed = true;
            m_deviceOwners.insert(deviceId, owner);
            m_discoveredCameraInfo.insert(deviceId, info);
            CameraStatus status = m_cameraStatuses.value(deviceId);
            status.deviceId = deviceId;
            status.name = info.name;
            if (newDevice)
                status.state = CameraState::Disconnected;
            m_cameraStatuses.insert(deviceId, status);
            if (newDevice)
                newlyDiscovered.append(qMakePair(deviceId, info.name));
        }
    }

    for (const auto& device : newlyDiscovered)
        emit cameraDiscovered(device.first, device.second);
    return changed;
}

QList<ICameraPlugin*> CameraManager::plugins() const {
    QMutexLocker locker(&m_mutex);
    return m_plugins.values();
}

ICameraPlugin* CameraManager::findPlugin(const QString& pluginId) const {
    QMutexLocker locker(&m_mutex);
    return m_plugins.value(pluginId);
}

QList<CameraStatus> CameraManager::discoverCameras() {
    if (QThread::currentThread() == thread() && m_shuttingDown)
        return knownCameras();
    QList<QString> asyncPlugins;
    bool changed = false;
    if (QThread::currentThread() == thread()) {
        QMutexLocker locker(&m_mutex);
        for (auto it = m_plugins.cbegin(); it != m_plugins.cend(); ++it) {
            const QString pluginId = it.key();
            ICameraPlugin* plugin = it.value();
            if (m_asyncDiscoveryPlugins.contains(pluginId)) {
                asyncPlugins.append(pluginId);
                continue;
            }
            if (!plugin->isAvailable()) {
                Logger::instance().debug(
                    QString("Plugin not available: %1 - %2").arg(plugin->pluginName(), plugin->availabilityMessage()),
                    "Camera");
                continue;
            }
            for (CameraInfo info : plugin->discoverCameras()) {
                if (info.pluginId.isEmpty())
                    info.pluginId = pluginId;
                if (info.pluginId != pluginId || info.deviceId.isEmpty()) {
                    Logger::instance().warning(QString("Rejected invalid camera metadata from %1").arg(pluginId),
                                               "Camera");
                    continue;
                }
                const QString existingOwner = m_deviceOwners.value(info.deviceId);
                if (!existingOwner.isEmpty() && existingOwner != pluginId) {
                    Logger::instance().warning(QString("Rejected duplicate camera device ID %1 from %2 (owned by %3)")
                                                   .arg(info.deviceId, pluginId, existingOwner),
                                               "Camera");
                    continue;
                }
                const CameraInfo oldInfo = m_discoveredCameraInfo.value(info.deviceId);
                const bool newStatus = !m_cameraStatuses.contains(info.deviceId);
                m_deviceOwners.insert(info.deviceId, pluginId);
                m_discoveredCameraInfo.insert(info.deviceId, info);
                CameraStatus status = m_cameraStatuses.value(info.deviceId);
                status.deviceId = info.deviceId;
                status.name = info.name;
                if (!m_cameraStatuses.contains(info.deviceId))
                    status.state = CameraState::Disconnected;
                m_cameraStatuses.insert(info.deviceId, status);
                changed = changed || newStatus || !sameCameraInfo(oldInfo, info);
            }
        }
        locker.unlock();
    }

    if (QThread::currentThread() == thread())
        for (const QString& pluginId : asyncPlugins)
            startManagedDiscovery(pluginId);

    if (changed)
        emit cameraListChanged();

    const QList<CameraStatus> discovered = knownCameras();
    Logger::instance().info(QString("Discovered %1 camera(s)").arg(discovered.size()), "Camera");
    return discovered;
}

QList<CameraStatus> CameraManager::knownCameras() {
    QMutexLocker locker(&m_mutex);
    return m_cameraStatuses.values();
}

void CameraManager::refreshCameras() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this]() { refreshCameras(); }, Qt::QueuedConnection);
        return;
    }
    if (m_shuttingDown)
        return;

    bool changed = false;
    QMutexLocker locker(&m_mutex);
    QMap<QString, QList<QPair<QString, CameraInfo>>> reportedDevices;
    QStringList asyncPlugins;

    for (auto pluginIt = m_plugins.cbegin(); pluginIt != m_plugins.cend(); ++pluginIt) {
        const QString pluginId = pluginIt.key();
        ICameraPlugin* plugin = pluginIt.value();
        if (m_asyncDiscoveryPlugins.contains(pluginId)) {
            asyncPlugins.append(pluginId);
            continue;
        }
        if (!plugin || !plugin->isAvailable())
            continue;
        for (CameraInfo info : plugin->discoverCameras()) {
            if (info.pluginId.isEmpty())
                info.pluginId = pluginId;
            if (info.pluginId != pluginId || info.deviceId.isEmpty()) {
                Logger::instance().warning(QString("Rejected invalid camera metadata from %1").arg(pluginId), "Camera");
                continue;
            }
            reportedDevices[info.deviceId].append(qMakePair(pluginId, info));
        }
    }

    for (auto it = m_cameraStatuses.begin(); it != m_cameraStatuses.end();) {
        const QString owner = m_deviceOwners.value(it.key());
        const bool asyncOwnerStillReports = m_asyncDiscoveryCandidates.value(it.key()).contains(owner);
        const bool active = m_cameras.contains(it.key()) || m_cameraLeaseCounts.value(it.key()) > 0;
        if (it.value().state == CameraState::Disconnected && !reportedDevices.contains(it.key()) &&
            !asyncOwnerStillReports && !active) {
            m_deviceOwners.remove(it.key());
            m_discoveredCameraInfo.remove(it.key());
            it = m_cameraStatuses.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }

    for (auto it = reportedDevices.cbegin(); it != reportedDevices.cend(); ++it) {
        const QString deviceId = it.key();
        QString ownerId = m_deviceOwners.value(deviceId);
        bool ownerStillReports = false;
        for (const auto& report : it.value())
            ownerStillReports = ownerStillReports || report.first == ownerId;
        ownerStillReports = ownerStillReports || m_asyncDiscoveryCandidates.value(deviceId).contains(ownerId);
        if (!ownerStillReports && !m_cameras.contains(deviceId) && m_cameraLeaseCounts.value(deviceId) == 0)
            ownerId = it.value().first().first;
        m_deviceOwners.insert(deviceId, ownerId);

        for (const auto& report : it.value()) {
            if (report.first != ownerId)
                Logger::instance().warning(QString("Rejected duplicate camera device ID %1 from %2 (owned by %3)")
                                               .arg(deviceId, report.first, ownerId),
                                           "Camera");
        }
        const auto ownerReport = std::find_if(it.value().cbegin(), it.value().cend(),
                                              [&ownerId](const auto& report) { return report.first == ownerId; });
        if (ownerReport == it.value().cend())
            continue;

        CameraStatus status = m_cameraStatuses.value(deviceId);
        const CameraInfo oldInfo = m_discoveredCameraInfo.value(deviceId);
        const bool newStatus = !m_cameraStatuses.contains(deviceId);
        status.deviceId = deviceId;
        status.name = ownerReport->second.name;
        m_discoveredCameraInfo.insert(deviceId, ownerReport->second);
        if (!m_cameraStatuses.contains(deviceId))
            status.state = CameraState::Disconnected;
        m_cameraStatuses.insert(deviceId, status);
        changed = changed || newStatus || !sameCameraInfo(oldInfo, ownerReport->second);
    }
    locker.unlock();
    for (const QString& pluginId : asyncPlugins)
        startManagedDiscovery(pluginId);
    if (changed)
        emit cameraListChanged();
}

bool CameraManager::connectCamera(const QString& deviceId) {
    if (QThread::currentThread() != thread()) {
        QMutexLocker locker(&m_mutex);
        CameraStatus& status = m_cameraStatuses[deviceId];
        status.deviceId = deviceId;
        status.lastError = QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY");
        Logger::instance().error(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"), "Camera");
        return false;
    }

    QMutexLocker locker(&m_mutex);

    if (m_shuttingDown || m_cameraSessions.contains(deviceId)) {
        const QString error = m_shuttingDown ? QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN")
                                             : QStringLiteral("CAMERA_OPERATION_SESSION_MANAGED");
        m_cameraStatuses[deviceId].deviceId = deviceId;
        m_cameraStatuses[deviceId].lastError = error;
        m_queuedEvents.append({CameraManagerEvent::Type::Error, deviceId, error});
        locker.unlock();
        dispatchQueuedEvents();
        return false;
    }
    if (m_disconnectAllInProgress || m_pendingDisconnects.contains(deviceId))
        return false;

    // 如果已连接，直接返回
    if (m_cameras.contains(deviceId)) {
        ICamera* camera = m_cameras.value(deviceId);
        if (camera && camera->isConnected()) {
            return true;
        }
        if (m_cameraLeaseCounts.value(deviceId) > 0)
            return false;
        if (camera) {
            if (camera->isAcquiring())
                camera->stopAcquisition();
            camera->disconnect();
            delete camera;
        }
        m_cameras.remove(deviceId);
        if (m_cameraStatuses.contains(deviceId))
            m_cameraStatuses[deviceId].state = CameraState::Disconnected;
    }

    // Connect only through the driver that owns this globally unique device ID.
    ICameraPlugin* targetPlugin = nullptr;
    CameraInfo targetInfo;
    const QString ownerId = m_deviceOwners.value(deviceId);
    targetPlugin = m_plugins.value(ownerId, nullptr);
    const bool asyncPlugin = m_asyncDiscoveryPlugins.contains(ownerId);
    if (asyncPlugin) {
        targetInfo = m_discoveredCameraInfo.value(deviceId);
        if (targetInfo.deviceId.isEmpty() || targetInfo.pluginId != ownerId)
            targetPlugin = nullptr;
    } else if (targetPlugin && targetPlugin->isAvailable()) {
        const QList<CameraInfo> cameras = targetPlugin->discoverCameras();
        for (CameraInfo info : cameras) {
            if (info.pluginId.isEmpty())
                info.pluginId = ownerId;
            if (info.deviceId == deviceId && info.pluginId == ownerId) {
                targetInfo = info;
                break;
            }
        }
        if (targetInfo.deviceId.isEmpty())
            targetPlugin = nullptr;
    } else {
        targetPlugin = nullptr;
    }

    if (!targetPlugin) {
        QString error =
            QString(
                "Camera not found: %1. Ensure the camera plugin is loaded and the device is powered/on the network.")
                .arg(deviceId);
        Logger::instance().error(error, "Camera");
        if (m_cameraStatuses.contains(deviceId)) {
            m_cameraStatuses[deviceId].lastError = error;
        } else {
            CameraStatus st;
            st.deviceId = deviceId;
            st.lastError = error;
            m_cameraStatuses.insert(deviceId, st);
        }
        m_queuedEvents.append({CameraManagerEvent::Type::Error, deviceId, error});
        locker.unlock();
        dispatchQueuedEvents();
        return false;
    }

    // 创建相机实例
    QObject* obj = targetPlugin->createCamera(targetInfo);
    ICamera* camera = qobject_cast<ICamera*>(obj);

    if (!camera) {
        QString error = QString("Failed to create camera instance: %1").arg(deviceId);
        Logger::instance().error(error, "Camera");
        if (m_cameraStatuses.contains(deviceId)) {
            m_cameraStatuses[deviceId].lastError = error;
        }
        if (obj)
            delete obj;
        m_queuedEvents.append({CameraManagerEvent::Type::Error, deviceId, error});
        locker.unlock();
        dispatchQueuedEvents();
        return false;
    }

    // 连接相机
    bool connected = camera->connect();

    if (!connected) {
        QString error = QString("Failed to open/connect camera: %1").arg(deviceId);
        Logger::instance().error(error, "Camera");
        if (m_cameraStatuses.contains(deviceId)) {
            m_cameraStatuses[deviceId].lastError = error;
        }
        if (camera->isAcquiring())
            camera->stopAcquisition();
        camera->disconnect();
        delete camera;
        m_queuedEvents.append({CameraManagerEvent::Type::Error, deviceId, error});
        locker.unlock();
        dispatchQueuedEvents();
        return false;
    }

    m_cameras.insert(deviceId, camera);
    invalidateCameraParameterBinding(deviceId);

    // 更新状态
    if (m_cameraStatuses.contains(deviceId)) {
        m_cameraStatuses[deviceId].state = CameraState::Connected;
        m_cameraStatuses[deviceId].lastError.clear();
    }

    Logger::instance().success(QString("Camera connected: %1 (%2)").arg(targetInfo.name).arg(deviceId), "Camera");
    m_queuedEvents.append({CameraManagerEvent::Type::Connected, deviceId, {}, CameraState::Connected});
    m_queuedEvents.append({CameraManagerEvent::Type::StateChanged, deviceId, {}, CameraState::Connected});
    locker.unlock();
    dispatchQueuedEvents();

    return true;
}

void CameraManager::disconnectCamera(const QString& deviceId) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this, deviceId]() { disconnectCamera(deviceId); }, Qt::QueuedConnection);
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        if (m_cameraSessions.contains(deviceId)) {
            locker.unlock();
            const quint64 requestId = nextManagedRequestId();
            QString error;
            if (!beginSessionDisconnect(deviceId, requestId, &error) && !error.isEmpty())
                Logger::instance().warning(QString("Camera async disconnect rejected (%1): %2").arg(deviceId, error),
                                           "Camera");
            return;
        }
    }
    QMutexLocker locker(&m_mutex);

    ICamera* camera = m_cameras.value(deviceId);
    if (!camera) {
        return;
    }
    if (m_cameraLeaseCounts.value(deviceId) > 0) {
        m_pendingDisconnects.insert(deviceId);
        return;
    }

    if (camera->isAcquiring())
        camera->stopAcquisition();
    camera->disconnect();
    delete camera;
    m_cameras.remove(deviceId);
    m_pendingDisconnects.remove(deviceId);
    invalidateCameraParameterBinding(deviceId);

    // 更新状态
    if (m_cameraStatuses.contains(deviceId)) {
        m_cameraStatuses[deviceId].state = CameraState::Disconnected;
    }

    Logger::instance().info(QString("Camera disconnected: %1").arg(deviceId), "Camera");
    m_queuedEvents.append({CameraManagerEvent::Type::Disconnected, deviceId, {}, CameraState::Disconnected});
    m_queuedEvents.append({CameraManagerEvent::Type::StateChanged, deviceId, {}, CameraState::Disconnected});
    locker.unlock();
    dispatchQueuedEvents();
}

void CameraManager::disconnectAll() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this]() { disconnectAll(); }, Qt::QueuedConnection);
        return;
    }
    QStringList deviceIds;
    {
        QMutexLocker locker(&m_mutex);
        if (m_disconnectAllInProgress)
            return;
        m_disconnectAllInProgress = true;
        m_disconnectAllSweepComplete = false;
        deviceIds = m_cameras.keys();
    }
    for (const QString& deviceId : deviceIds)
        disconnectCamera(deviceId);
    startIdleSessionDisconnects();
    {
        QMutexLocker locker(&m_mutex);
        m_disconnectAllSweepComplete = true;
        if (m_pendingDisconnects.isEmpty() && m_deviceOperations.isEmpty() && !hasBusyCameraSession()) {
            m_disconnectAllInProgress = false;
            m_disconnectAllSweepComplete = false;
        }
    }
    Logger::instance().info("All cameras disconnected", "Camera");
}

ICamera* CameraManager::getCamera(const QString& deviceId) const {
    QMutexLocker locker(&m_mutex);
    return m_cameras.value(deviceId);
}

QList<ICamera*> CameraManager::allCameras() const {
    QMutexLocker locker(&m_mutex);
    return m_cameras.values();
}

CameraLease CameraManager::acquireCamera(const QString& deviceId) {
    if (QThread::currentThread() != thread()) {
        Logger::instance().error(QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"), "Camera");
        return {};
    }
    QMutexLocker locker(&m_mutex);
    if (m_disconnectAllInProgress || m_pendingDisconnects.contains(deviceId))
        return {};
    ICamera* camera = m_cameras.value(deviceId);
    if (!camera)
        return {};
    ++m_cameraLeaseCounts[deviceId];
    return CameraLease(this, deviceId, camera);
}

void CameraManager::releaseCameraLease(const QString& deviceId) {
    bool disconnectPending = false;
    bool retryDisconnectAll = false;
    {
        QMutexLocker locker(&m_mutex);
        const int remaining = m_cameraLeaseCounts.value(deviceId) - 1;
        if (remaining <= 0) {
            m_cameraLeaseCounts.remove(deviceId);
            disconnectPending = m_pendingDisconnects.contains(deviceId);
        } else {
            m_cameraLeaseCounts[deviceId] = remaining;
        }
    }

    if (disconnectPending) {
        if (QThread::currentThread() == thread())
            disconnectCamera(deviceId);
        else
            QMetaObject::invokeMethod(
                this, [this, deviceId]() { disconnectCamera(deviceId); }, Qt::QueuedConnection);
    }

    {
        QMutexLocker locker(&m_mutex);
        if (m_disconnectAllInProgress && m_disconnectAllSweepComplete && m_pendingDisconnects.isEmpty() &&
            m_cameraLeaseCounts.isEmpty()) {
            if (QThread::currentThread() == thread() && !hasBusyCameraSession()) {
                m_disconnectAllInProgress = false;
                m_disconnectAllSweepComplete = false;
            } else if (QThread::currentThread() != thread()) {
                retryDisconnectAll = true;
            }
        }
    }
    if (retryDisconnectAll)
        QMetaObject::invokeMethod(
            this, [this]() { startIdleSessionDisconnects(); }, Qt::QueuedConnection);
}

void CameraManager::dispatchQueuedEvents() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this]() { dispatchQueuedEvents(); }, Qt::QueuedConnection);
        return;
    }

    {
        QMutexLocker locker(&m_mutex);
        if (m_dispatchingEvents)
            return;
        m_dispatchingEvents = true;
    }

    while (true) {
        CameraManagerEvent event;
        {
            QMutexLocker locker(&m_mutex);
            if (m_queuedEvents.isEmpty()) {
                m_dispatchingEvents = false;
                return;
            }
            event = m_queuedEvents.takeFirst();
        }

        switch (event.type) {
        case CameraManagerEvent::Type::Error:
            emit errorOccurred(event.deviceId, event.message);
            break;
        case CameraManagerEvent::Type::Connected:
            emit cameraConnected(event.deviceId);
            break;
        case CameraManagerEvent::Type::Disconnected:
            emit cameraDisconnected(event.deviceId);
            break;
        case CameraManagerEvent::Type::StateChanged:
            emit cameraStateChanged(event.deviceId, event.state);
            break;
        }
    }
}

bool CameraManager::isConnected(const QString& deviceId) const {
    QMutexLocker locker(&m_mutex);
    if (m_cameraSessions.contains(deviceId))
        return m_cameraStatuses.value(deviceId).state == CameraState::Connected;
    ICamera* camera = m_cameras.value(deviceId);
    return camera && camera->isConnected();
}

CameraState CameraManager::cameraState(const QString& deviceId) const {
    QMutexLocker locker(&m_mutex);
    if (m_cameraSessions.contains(deviceId))
        return m_cameraStatuses.value(deviceId).state;
    if (m_cameras.contains(deviceId)) {
        ICamera* camera = m_cameras.value(deviceId);
        if (camera->isAcquiring())
            return CameraState::Grabbing;
        if (camera->isConnected())
            return CameraState::Connected;
    }

    if (m_cameraStatuses.contains(deviceId)) {
        return m_cameraStatuses.value(deviceId).state;
    }

    return CameraState::Disconnected;
}

QString CameraManager::lastError(const QString& deviceId) const {
    QMutexLocker locker(&m_mutex);
    if (m_cameraStatuses.contains(deviceId)) {
        return m_cameraStatuses.value(deviceId).lastError;
    }
    return QString();
}

quint64 CameraManager::cameraParameterBindingVersion(const QString& deviceId) const {
    if (deviceId.isEmpty())
        return 0;
    const auto binding = cameraParameterBindingFor(deviceId);
    return binding ? binding->generation() : 0;
}

quint64 CameraManager::invalidateCameraParameterBinding(const QString& deviceId) {
    if (deviceId.isEmpty())
        return 0;
    const auto binding = cameraParameterBindingFor(deviceId);
    return binding ? binding->invalidate() : 0;
}

void CameraManager::noteCameraParameterChanged(const QString& deviceId) {
    invalidateCameraParameterBinding(deviceId);
}

CameraParameterWriteResult CameraManager::writeCameraParameterIfCurrent(const QString& deviceId,
                                                                        quint64 expectedVersion, ICamera* camera,
                                                                        const QString& serialNumber,
                                                                        const QString& parameter,
                                                                        const QJsonValue& value) {
    const auto binding = cameraParameterBindingFor(deviceId);
    if (!binding) {
        CameraParameterWriteResult result;
        result.parameter = parameter;
        result.requested = value;
        result.errorCode = QStringLiteral("CAMERA_PARAMETER_BINDING_UNAVAILABLE");
        return result;
    }
    return binding->write(camera, expectedVersion, deviceId, serialNumber, parameter, value);
}

QSharedPointer<CameraParameterBinding> CameraManager::cameraParameterBindingFor(const QString& deviceId) const {
    if (deviceId.isEmpty())
        return {};
    QMutexLocker locker(&m_cameraParameterBindingsMutex);
    auto binding = m_cameraParameterBindings.value(deviceId);
    if (!binding) {
        binding = QSharedPointer<CameraParameterBinding>::create();
        m_cameraParameterBindings.insert(deviceId, binding);
    }
    return binding;
}

} // namespace DeepLux
