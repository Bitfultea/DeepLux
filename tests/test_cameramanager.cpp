#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <atomic>
#include <core/agent/AgentActor.h>
#include <core/agent/AgentController.h>
#include <core/agent/GuiEvent.h>
#include <core/agent/ToolSchema.h>
#include <core/base/ModuleBase.h>
#include <core/common/UserManager.h>
#include <core/device/CameraDiscoveryJob.h>
#include <core/device/CameraManager.h>
#include <core/engine/RunEngine.h>
#include <core/interface/ICamera.h>
#include <core/interface/ICameraDiscoveryAsync.h>
#include <core/interface/ICameraParameterControl.h>
#include <core/interface/ICameraPlugin.h>
#include <core/model/Project.h>
#include <functional>
#include <limits>
#include <manager/PluginManager.h>
#include <manager/ProjectManager.h>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <ui/views/CameraSetView.h>
#include <ui/views/MainWindow.h>

using namespace DeepLux;

namespace {

struct CameraCounters {
    int connectCalls = 0;
    int disconnectCalls = 0;
    int stopCalls = 0;
    int startCalls = 0;
    int triggerCalls = 0;
    int parameterSetCalls = 0;
    int parameterValidationCalls = 0;
    bool parameterValidationThrows = false;
    bool parameterOutOfRange = false;
    bool parameterSetterThrows = false;
    bool parameterReadbackThrows = false;
    bool parameterReadbackThrowsAfterWrite = false;
    bool clampExposureReadback = false;
    mutable std::atomic_int isConnectedQueries{0};
    mutable std::atomic_int isAcquiringQueries{0};
    int destroyedCalls = 0;
    bool connectSucceeds = true;
    bool connectThrows = false;
    bool isConnectedThrows = false;
    bool reportDisconnectedForDetails = false;
    bool detailsGetterThrows = false;
    bool detailsValuesNonFinite = false;
    bool disconnectSucceeds = true;
    bool createSucceeds = true;
    bool enforceWorkerCalls = false;
    QThread* managerThread = QThread::currentThread();
    QSemaphore* connectGate = nullptr;
    QSemaphore* disconnectGate = nullptr;
    QSemaphore* detailsGate = nullptr;
    QSemaphore* parameterValidationGate = nullptr;
    std::atomic_int callsOnManagerThread{0};
    std::atomic_int wrongWorkerThreadCalls{0};
    std::atomic_int connectEntered{0};
    std::atomic_int disconnectEntered{0};
    std::atomic_bool disconnectStartedSignalSeen{false};
    std::atomic_int disconnectBeforeStartedSignal{0};
    std::atomic_int detailsGetterCalls{0};
    std::atomic_int detailsGateEntries{0};
    std::atomic_int parameterValidationGateEntries{0};
    std::atomic_int destroyedThreadCalls{0};
    QStringList callOrder;
};

class BlockingCameraGateModule final : public ModuleBase {
protected:
    bool process(const ImageData& input, ImageData& output) override {
        QThread::msleep(300);
        output = input;
        return true;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class FakeCamera : public ICamera {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICamera)

public:
    explicit FakeCamera(QString id, CameraCounters* counters) : m_id(std::move(id)), m_counters(counters) {}
    ~FakeCamera() override {
        noteThread();
        ++m_counters->destroyedCalls;
        ++m_counters->destroyedThreadCalls;
    }

    QString deviceId() const override {
        noteDetailsGetter();
        return m_id;
    }
    QString name() const override {
        noteDetailsGetter();
        return m_id;
    }
    QString serialNumber() const override {
        noteDetailsGetter();
        return QStringLiteral("serial-%1").arg(m_id);
    }
    QString manufacturer() const override {
        noteDetailsGetter();
        return QStringLiteral("test");
    }
    CameraCapabilities capabilities() const override {
        noteDetailsGetter();
        if (m_counters->detailsGate) {
            ++m_counters->detailsGateEntries;
            m_counters->detailsGate->acquire();
        }
        if (m_counters->detailsGetterThrows)
            throw std::runtime_error("fake camera details failure");
        return m_capabilities;
    }
    bool isPlatformSupported() const override {
        return true;
    }
    bool connect() override {
        noteThread();
        ++m_counters->connectEntered;
        if (m_counters->connectGate)
            m_counters->connectGate->acquire();
        ++m_counters->connectCalls;
        m_counters->callOrder.append(QStringLiteral("connect"));
        if (m_counters->connectThrows)
            throw std::runtime_error("fake connect failure");
        m_connected = m_counters->connectSucceeds;
        return m_connected;
    }
    void disconnect() override {
        noteThread();
        if (!m_counters->disconnectStartedSignalSeen.load())
            ++m_counters->disconnectBeforeStartedSignal;
        ++m_counters->disconnectEntered;
        ++m_counters->disconnectCalls;
        m_counters->callOrder.append(QStringLiteral("disconnect"));
        if (m_counters->disconnectGate)
            m_counters->disconnectGate->acquire();
        if (m_counters->disconnectSucceeds) {
            m_connected = false;
            m_acquiring = false;
        }
    }
    bool isConnected() const override {
        noteThread();
        ++m_counters->isConnectedQueries;
        if (m_counters->isConnectedThrows)
            throw std::runtime_error("fake state query failure");
        return m_connected && !m_counters->reportDisconnectedForDetails;
    }
    bool startAcquisition() override {
        ++m_counters->startCalls;
        m_acquiring = true;
        return true;
    }
    void stopAcquisition() override {
        noteThread();
        ++m_counters->stopCalls;
        m_counters->callOrder.append(QStringLiteral("stop"));
        m_acquiring = false;
    }
    bool isAcquiring() const override {
        ++m_counters->isAcquiringQueries;
        return m_acquiring;
    }
    bool triggerSoftware() override {
        ++m_counters->triggerCalls;
        return true;
    }
    void setTriggerMode(TriggerMode mode) override {
        noteThread();
        ++m_counters->parameterSetCalls;
        m_triggerMode = mode;
    }
    TriggerMode triggerMode() const override {
        noteDetailsGetter();
        return m_triggerMode;
    }
    void setExposureTime(double value) override {
        noteThread();
        ++m_counters->parameterSetCalls;
        if (m_counters->parameterSetterThrows)
            throw std::runtime_error("fake setter failure");
        if (m_counters->parameterReadbackThrowsAfterWrite)
            m_throwExposureReadbackOnce = true;
        m_exposure = m_counters->clampExposureReadback ? value * 0.5 : value;
    }
    double exposureTime() const override {
        noteDetailsGetter();
        if (m_throwExposureReadbackOnce) {
            m_throwExposureReadbackOnce = false;
            throw std::runtime_error("fake post-write readback failure");
        }
        if (m_counters->parameterReadbackThrows && m_counters->parameterSetCalls > 0)
            throw std::runtime_error("fake readback failure");
        return m_counters->detailsValuesNonFinite ? std::numeric_limits<double>::quiet_NaN() : m_exposure;
    }
    void setGain(double value) override {
        noteThread();
        ++m_counters->parameterSetCalls;
        m_gain = value;
    }
    double gain() const override {
        noteDetailsGetter();
        return m_gain;
    }
    void setFrameRate(double value) override {
        noteThread();
        ++m_counters->parameterSetCalls;
        m_frameRate = value;
    }
    double frameRate() const override {
        noteDetailsGetter();
        return m_frameRate;
    }
    void setRoi(int x, int y, int width, int height) override {
        noteThread();
        ++m_counters->parameterSetCalls;
        m_roi = QRect(x, y, width, height);
    }
    QRect roi() const override {
        noteDetailsGetter();
        return m_roi;
    }
    QImage lastImage() const override {
        return {};
    }
    QVariantMap lastImageMetadata() const override {
        return {};
    }
    int imageWidth() const override {
        noteDetailsGetter();
        return 0;
    }
    int imageHeight() const override {
        noteDetailsGetter();
        return 0;
    }
    QWidget* createConfigWidget() override {
        return nullptr;
    }
    QJsonObject toJson() const override {
        return {};
    }
    bool fromJson(const QJsonObject&) override {
        return true;
    }
    bool validateParameterValue(const QJsonValue& value, QString* errorCode) const {
        noteThread();
        ++m_counters->parameterValidationCalls;
        if (m_counters->parameterValidationGate) {
            ++m_counters->parameterValidationGateEntries;
            m_counters->parameterValidationGate->acquire();
        }
        if (m_counters->parameterValidationThrows)
            throw std::runtime_error("fake validation failure");
        if (m_counters->parameterOutOfRange || (value.isDouble() && value.toDouble() > 100000.0)) {
            if (errorCode)
                *errorCode = QStringLiteral("CAMERA_PARAMETER_OUT_OF_RANGE");
            return false;
        }
        if (errorCode)
            errorCode->clear();
        return true;
    }
    void setCapabilities(const CameraCapabilities& capabilities) {
        m_capabilities = capabilities;
    }

private:
    void noteDetailsGetter() const {
        noteThread();
        ++m_counters->detailsGetterCalls;
    }

    void noteThread() const {
        if (!m_counters->enforceWorkerCalls)
            return;
        QThread* current = QThread::currentThread();
        if (current == m_counters->managerThread) {
            ++m_counters->callsOnManagerThread;
            return;
        }
        if (!m_cameraCallThread)
            m_cameraCallThread = current;
        else if (m_cameraCallThread != current)
            ++m_counters->wrongWorkerThreadCalls;
    }

    QString m_id;
    CameraCounters* m_counters;
    bool m_connected = false;
    bool m_acquiring = false;
    TriggerMode m_triggerMode = TriggerMode::Software;
    double m_exposure = 0;
    double m_gain = 0;
    double m_frameRate = 0;
    QRect m_roi;
    CameraCapabilities m_capabilities;
    mutable bool m_throwExposureReadbackOnce = false;
    mutable QThread* m_cameraCallThread = nullptr;
};

class FakeParameterControlCamera final : public FakeCamera, public ICameraParameterControl {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICameraParameterControl)

public:
    using FakeCamera::FakeCamera;

    bool validateParameter(const QString&, const QJsonValue& value, QString* errorCode) const override {
        return validateParameterValue(value, errorCode);
    }
};

class FakeCameraPlugin final : public ICameraPlugin {
public:
    FakeCameraPlugin(QString id, QList<CameraInfo> cameras, CameraCounters* counters)
        : m_id(std::move(id)), m_cameras(std::move(cameras)), m_counters(counters) {}

    QString pluginId() const override {
        ++m_virtualCalls;
        return m_id;
    }
    QString pluginName() const override {
        ++m_virtualCalls;
        return m_id;
    }
    QString manufacturer() const override {
        ++m_virtualCalls;
        return QStringLiteral("test");
    }
    bool isAvailable() const override {
        ++m_virtualCalls;
        return true;
    }
    QString availabilityMessage() const override {
        ++m_virtualCalls;
        return {};
    }
    QList<CameraInfo> discoverCameras() override {
        ++m_virtualCalls;
        ++m_discoverCalls;
        return m_cameras;
    }
    QObject* createCamera(const CameraInfo& info) override {
        ++m_virtualCalls;
        ++m_createCalls;
        m_lastCreatePluginId = info.pluginId;
        if (!m_counters->createSucceeds)
            return nullptr;
        return m_parameterControlEnabled
                   ? static_cast<QObject*>(new FakeParameterControlCamera(info.deviceId, m_counters))
                   : static_cast<QObject*>(new FakeCamera(info.deviceId, m_counters));
    }

    int createCalls() const {
        return m_createCalls;
    }
    QString lastCreatePluginId() const {
        return m_lastCreatePluginId;
    }
    int discoverCalls() const {
        return m_discoverCalls;
    }
    int virtualCallCount() const {
        return m_virtualCalls;
    }
    void setCameras(QList<CameraInfo> cameras) {
        m_cameras = std::move(cameras);
    }
    void setParameterControlEnabled(bool enabled) {
        m_parameterControlEnabled = enabled;
    }

private:
    QString m_id;
    QList<CameraInfo> m_cameras;
    CameraCounters* m_counters;
    int m_createCalls = 0;
    int m_discoverCalls = 0;
    mutable int m_virtualCalls = 0;
    QString m_lastCreatePluginId;
    bool m_parameterControlEnabled = false;
};

class FakeDiscoveryJob final : public CameraDiscoveryJob {
    Q_OBJECT
public:
    FakeDiscoveryJob(quint64 signaledId, QByteArray payload, int delayMs, bool duplicateTerminal, QObject* parent)
        : CameraDiscoveryJob(parent), m_signaledId(signaledId), m_payload(std::move(payload)), m_delayMs(delayMs),
          m_duplicateTerminal(duplicateTerminal) {
        QTimer::singleShot(m_delayMs, this, [this]() {
            completeOnce(m_signaledId, m_payload);
            if (m_duplicateTerminal)
                emit completed(m_signaledId, m_payload); // Defend the Manager against a misbehaving plugin.
        });
    }

private:
    quint64 m_signaledId;
    QByteArray m_payload;
    int m_delayMs;
    bool m_duplicateTerminal;
};

class FakeAsyncCameraPlugin final : public ICameraPlugin, public ICameraDiscoveryAsync {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICameraPlugin DeepLux::ICameraDiscoveryAsync)
public:
    explicit FakeAsyncCameraPlugin(QString id) : m_id(std::move(id)) {}
    ~FakeAsyncCameraPlugin() override {
        m_factoryThread.quit();
        m_factoryThread.wait();
    }
    QString pluginId() const override {
        ++m_nonStartVirtualCalls;
        return m_id;
    }
    QString pluginName() const override {
        ++m_nonStartVirtualCalls;
        return m_id;
    }
    QString manufacturer() const override {
        ++m_nonStartVirtualCalls;
        return QStringLiteral("test");
    }
    bool isAvailable() const override {
        ++m_nonStartVirtualCalls;
        ++m_availableCalls;
        return true;
    }
    QString availabilityMessage() const override {
        ++m_nonStartVirtualCalls;
        return {};
    }
    QList<CameraInfo> discoverCameras() override {
        ++m_nonStartVirtualCalls;
        ++m_syncDiscoverCalls;
        return {};
    }
    QObject* createCamera(const CameraInfo& info) override {
        ++m_nonStartVirtualCalls;
        ++m_createCalls;
        if (m_parentedNonCamera) {
            m_lastFactoryObject = new QObject(this);
            return m_lastFactoryObject;
        }
        const QString cameraId = m_useInfoDeviceId ? info.deviceId : m_id;
        FakeCamera* camera = m_parameterControlEnabled
                                 ? static_cast<FakeCamera*>(new FakeParameterControlCamera(cameraId, &m_cameraCounters))
                                 : new FakeCamera(cameraId, &m_cameraCounters);
        if (m_factoryWrongThread) {
            connect(
                camera, &QObject::destroyed, this,
                [this]() { m_wrongThreadFactoryDestroyed = QThread::currentThread() == &m_factoryThread; },
                Qt::DirectConnection);
            m_factoryThread.start();
            camera->moveToThread(&m_factoryThread);
        }
        m_lastFactoryObject = camera;
        return camera;
    }
    CameraDiscoveryJob* startDiscovery(quint64 requestId, QString& errorCode) override {
        ++m_asyncStarts;
        if (m_startCallback)
            m_startCallback(requestId);
        errorCode = m_startError;
        if (!m_startError.isEmpty())
            return nullptr;
        QJsonArray cameras;
        QStringList deviceIds{m_deviceId};
        deviceIds.append(m_additionalDeviceIds);
        for (const QString& deviceId : deviceIds) {
            cameras.append(QJsonObject{{QStringLiteral("pluginId"), m_id},
                                       {QStringLiteral("deviceId"), deviceId},
                                       {QStringLiteral("name"), m_cameraName},
                                       {QStringLiteral("serialNumber"), QString()},
                                       {QStringLiteral("ipAddress"), QString()},
                                       {QStringLiteral("width"), 640},
                                       {QStringLiteral("height"), 480},
                                       {QStringLiteral("pixelFormat"), QStringLiteral("Mono8")},
                                       {QStringLiteral("isConnected"), false},
                                       {QStringLiteral("rawInfo"), QJsonObject{}}});
        }
        QJsonObject result{{QStringLiteral("schemaVersion"), 1},
                           {QStringLiteral("requestId"), QString::number(requestId)},
                           {QStringLiteral("status"), m_terminalStatus},
                           {QStringLiteral("cameras"), m_reportsCamera ? cameras : QJsonArray{}}};
        if (m_terminalStatus != QLatin1String("success"))
            result.insert(QStringLiteral("errorCode"), m_terminalError);
        if (!m_payloadOverride.isEmpty())
            result = QJsonDocument::fromJson(m_payloadOverride).object();
        auto* job = new FakeDiscoveryJob(
            requestId + m_signalIdDelta,
            m_payloadOverride.isEmpty() ? QJsonDocument(result).toJson(QJsonDocument::Compact) : m_payloadOverride,
            m_delayMs, m_duplicateTerminal, m_invalidJobParent ? nullptr : this);
        m_lastJob = job;
        return job;
    }
    void requestCancel(quint64 requestId) override {
        Q_UNUSED(requestId)
        ++m_cancelCalls;
    }
    int syncCallCount() const {
        return m_availableCalls + m_syncDiscoverCalls;
    }
    int nonStartVirtualCallCount() const {
        return m_nonStartVirtualCalls;
    }
    int asyncStarts() const {
        return m_asyncStarts;
    }
    int createCalls() const {
        return m_createCalls;
    }
    CameraCounters& cameraCounters() {
        return m_cameraCounters;
    }
    void setUseInfoDeviceId(bool value) {
        m_useInfoDeviceId = value;
    }
    void setParameterControlEnabled(bool value) {
        m_parameterControlEnabled = value;
    }
    void setParentedNonCamera(bool value) {
        m_parentedNonCamera = value;
    }
    void setFactoryWrongThread(bool value) {
        m_factoryWrongThread = value;
    }
    QObject* lastFactoryObject() const {
        return m_lastFactoryObject;
    }
    bool wrongThreadFactoryDestroyed() const {
        return m_wrongThreadFactoryDestroyed.load();
    }
    int cancelCalls() const {
        return m_cancelCalls;
    }
    void setDelayMs(int value) {
        m_delayMs = value;
    }

    void setDeviceId(QString value) {
        m_deviceId = std::move(value);
    }
    void setAdditionalDeviceIds(QStringList value) {
        m_additionalDeviceIds = std::move(value);
    }
    void setReportsCamera(bool value) {
        m_reportsCamera = value;
    }
    void setCameraName(QString value) {
        m_cameraName = std::move(value);
    }
    void setStartError(QString value) {
        m_startError = std::move(value);
    }
    void setTerminalStatus(QString status, QString errorCode = QStringLiteral("CAMERA_DISCOVERY_CANCELLED")) {
        m_terminalStatus = std::move(status);
        m_terminalError = std::move(errorCode);
    }
    void setSignalIdDelta(quint64 value) {
        m_signalIdDelta = value;
    }
    void setDuplicateTerminal(bool value) {
        m_duplicateTerminal = value;
    }
    void setPayloadOverride(QByteArray value) {
        m_payloadOverride = std::move(value);
    }
    void setStartCallback(std::function<void(quint64)> callback) {
        m_startCallback = std::move(callback);
    }
    void setInvalidJobParent(bool value) {
        m_invalidJobParent = value;
    }

private:
    QString m_id;
    mutable int m_availableCalls = 0;
    int m_syncDiscoverCalls = 0;
    int m_asyncStarts = 0;
    mutable int m_nonStartVirtualCalls = 0;
    QString m_deviceId = QStringLiteral("fake-device");
    QStringList m_additionalDeviceIds;
    bool m_reportsCamera = true;
    QString m_cameraName = QStringLiteral("Fake Camera");
    QString m_startError;
    QString m_terminalStatus = QStringLiteral("success");
    QString m_terminalError = QStringLiteral("CAMERA_DISCOVERY_CANCELLED");
    int m_createCalls = 0;
    bool m_useInfoDeviceId = false;
    bool m_parameterControlEnabled = false;
    bool m_parentedNonCamera = false;
    bool m_factoryWrongThread = false;
    std::atomic_bool m_wrongThreadFactoryDestroyed{false};
    QThread m_factoryThread;
    QPointer<QObject> m_lastFactoryObject;
    CameraCounters m_cameraCounters;
    int m_cancelCalls = 0;
    int m_delayMs = 80;
    quint64 m_signalIdDelta = 0;
    bool m_duplicateTerminal = false;
    QByteArray m_payloadOverride;
    FakeDiscoveryJob* m_lastJob = nullptr;
    std::function<void(quint64)> m_startCallback;
    bool m_invalidJobParent = false;
};

CameraInfo cameraInfo(const QString& pluginId, const QString& deviceId, const QString& name = {}) {
    CameraInfo info;
    info.pluginId = pluginId;
    info.deviceId = deviceId;
    info.name = name.isEmpty() ? deviceId : name;
    return info;
}

} // namespace

class TestCameraManager : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void testRefreshCamerasReturnsWithoutPlugins();
    void testDriverOwnershipAndSafeUnregister();
    void testAcquiringCameraStopsBeforeDisconnectAll();
    void testCameraLeaseDefersDestruction();
    void testDisconnectAllWaitsForLeaseThenReopensConnections();
    void testMetadataNormalizationAndDuplicateIds();
    void testRefreshReleasesStaleDeviceOwnership();
    void testFailedCameraCreationAndConnection();
    void testReconnectAndDisconnectAllReentrancy();
    void testLifecycleSignalReentrancyAndThreadAffinity();
    void testReentrantCameraTransitionsDoNotEmitStaleState();
    void testConcurrentCameraTransitionSignalOrder();
    void testAgentListsEmptyCameraCacheWithoutDiscovery();
    void testAgentListsCachedCameraStatusesWithoutDiscovery();
    void testAgentReadsCameraDetailsThroughLease();
    void testLegacyAgentCameraParameterWriteContract();
    void testAgentCameraLifecycleConfirmationAndLeaseDelay();
    void testAgentCameraLifecycleRejectsWrongThreadAndBusyEngine();
    void testAsyncDiscoveryRequiresOptionalInterfaceAndOwnerThread();
    void testAsyncDiscoveryUsesQueuedResultAndHoldsPluginLease();
    void testAsyncDuplicateOwnerIsCompletionOrderIndependent();
    void testAsyncMissingDeviceIsRetainedUntilLeaseReleaseAndRefresh();
    void testCameraSetViewShowsAsyncDiscoveryAndRefreshResults();
    void testCameraSetViewDeleteAndApplyRemainGuiOnly();
    void testCameraSetViewRestoresAgentPendingCameraOperations();
    void testCameraSetViewStartsAsyncOperationsAndRecoversErrors();
    void testCameraSetViewRequiresPermissionsAndRejectsLateSignals();
    void testAsyncCameraDetailsQueryIsQueuedAndLifecycleSafe();
    void testAsyncAgentCameraParameterWriteContract();
    void testManagedDiscoveryStartFailureSettlesOnce();
    void testAgentDiscoveryTargetSnapshotDoesNotCallDrivers();
    void testAgentDiscoveryBatchAggregatesAndCachesBeforeSettled();
    void testAgentDiscoveryBatchNullStartFailureIsQueryable();
    void testAgentDiscoveryBatchWaitsForAllSynchronousStartFailures();
    void testAgentDiscoveryBatchCancelledChildIsQueryable();
    void testAgentDiscoveryRejectsLegacyOnlyTargets();
    void testAgentDiscoveryObserverRejectsWithoutStartingPlugins();
    void testAgentDiscoveryBatchRejectsWhenAnyTargetIsBusy();
    void testAgentDiscoveryBatchRejectsWrongThreadAndBusyEngine();
    void testAgentDiscoveryBatchInvalidJobHoldsLeaseUntilDestroyed();
    void testAgentDiscoveryBatchReservesPluginsBeforeReentrantStart();
    void testAgentDiscoveryRequestIdsCannotAliasCameraOperations();
    void testAgentDiscoveryConfirmationBindsExactRegistrySnapshot();
    void testAgentDiscoveryHistoryUsesBoundedFifo();
    void testAgentCameraOperationsAreAsyncAndQueryable();
    void testAgentCameraOperationHistoryBoundedFifo();
    void testAsyncCameraSessionLifecycleAndWindowShutdown();

private:
    QStringList m_registeredIds;
};

void TestCameraManager::init() {
    m_registeredIds.clear();
    UserManager::instance().login(QStringLiteral("developer"), QString());
}

void TestCameraManager::cleanup() {
    CameraManager& manager = CameraManager::instance();
    manager.disconnectAll();
    for (const QString& id : m_registeredIds)
        manager.tryUnregisterCameraPlugin(id);
}

void TestCameraManager::testRefreshCamerasReturnsWithoutPlugins() {
    CameraManager::instance().refreshCameras();
    QVERIFY(true);
}

void TestCameraManager::testAgentListsEmptyCameraCacheWithoutDiscovery() {
    QVERIFY(CameraManager::instance().knownCameras().isEmpty());
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    const QJsonObject result = actor.executeTool(QStringLiteral("list_cameras"), {});
    QCOMPARE(result.value(QStringLiteral("discoveryPerformed")).toBool(), false);
    QVERIFY(result.value(QStringLiteral("cameras")).toArray().isEmpty());
}

void TestCameraManager::testAgentListsCachedCameraStatusesWithoutDiscovery() {
    ToolSchema::instance().registerDefaultTools();
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-agent-list"),
                            {cameraInfo({}, QStringLiteral("agent-camera-a"), QStringLiteral("Camera A")),
                             cameraInfo({}, QStringLiteral("agent-camera-b"), QStringLiteral("Camera B"))},
                            &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    struct CleanupRegisteredPlugin {
        CameraManager& manager;
        FakeCameraPlugin& plugin;
        ~CleanupRegisteredPlugin() {
            manager.disconnectAll();
            manager.tryUnregisterCameraPlugin(plugin.pluginId());
        }
    } cleanup{manager, plugin};
    QVERIFY(manager.connectCamera(QStringLiteral("agent-camera-a")));
    ICamera* connectedCamera = manager.getCamera(QStringLiteral("agent-camera-a"));
    QVERIFY(connectedCamera);
    connectedCamera->setExposureTime(1234.5);
    connectedCamera->setGain(2.25);

    const int discoveryCallsBefore = plugin.discoverCalls();
    const QList<CameraStatus> before = manager.knownCameras();
    QCOMPARE(before.size(), 2);
    const bool wasAcquiring = connectedCamera->isAcquiring();
    ProjectManager& projectManager = ProjectManager::instance();
    const Project* projectBefore = projectManager.currentProject();
    const bool projectWasModified = projectBefore && projectBefore->isModified();

    AgentActor actor;
    const QJsonObject result = actor.executeTool(QStringLiteral("list_cameras"), {});
    QCOMPARE(plugin.discoverCalls(), discoveryCallsBefore);
    QCOMPARE(result.value(QStringLiteral("discoveryPerformed")).toBool(), false);
    QCOMPARE(result.value(QStringLiteral("source")).toString(), QStringLiteral("CameraManager.knownCameras"));
    const QJsonArray cameras = result.value(QStringLiteral("cameras")).toArray();
    QCOMPARE(cameras.size(), 2);

    QMap<QString, QJsonObject> byId;
    for (const QJsonValue& value : cameras) {
        const QJsonObject camera = value.toObject();
        byId.insert(camera.value(QStringLiteral("deviceId")).toString(), camera);
    }
    QCOMPARE(byId.value(QStringLiteral("agent-camera-a")).value(QStringLiteral("name")).toString(),
             QStringLiteral("Camera A"));
    QCOMPARE(byId.value(QStringLiteral("agent-camera-a")).value(QStringLiteral("state")).toString(),
             QStringLiteral("connected"));
    QCOMPARE(byId.value(QStringLiteral("agent-camera-b")).value(QStringLiteral("state")).toString(),
             QStringLiteral("disconnected"));
    QVERIFY(byId.value(QStringLiteral("agent-camera-a")).value(QStringLiteral("lastError")).toString().isEmpty());
    QCOMPARE(manager.knownCameras().size(), before.size());
    QCOMPARE(connectedCamera->isConnected(), true);
    QCOMPARE(connectedCamera->isAcquiring(), wasAcquiring);
    QCOMPARE(connectedCamera->exposureTime(), 1234.5);
    QCOMPARE(connectedCamera->gain(), 2.25);
    QCOMPARE(projectManager.currentProject(), projectBefore);
    QCOMPARE(projectBefore && projectBefore->isModified(), projectWasModified);
    QCOMPARE(counters.connectCalls, 1);
    QCOMPARE(counters.stopCalls, 0);
}

void TestCameraManager::testAgentReadsCameraDetailsThroughLease() {
    ToolSchema::instance().registerDefaultTools();
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-agent-details"),
                            {cameraInfo({}, QStringLiteral("detail-camera"), QStringLiteral("Detail Camera")),
                             cameraInfo({}, QStringLiteral("offline-camera"), QStringLiteral("Offline Camera"))},
                            &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    struct CleanupRegisteredPlugin {
        CameraManager& manager;
        FakeCameraPlugin& plugin;
        ~CleanupRegisteredPlugin() {
            manager.disconnectAll();
            manager.tryUnregisterCameraPlugin(plugin.pluginId());
        }
    } cleanup{manager, plugin};
    QVERIFY(manager.connectCamera(QStringLiteral("detail-camera")));

    auto* fakeCamera = dynamic_cast<FakeCamera*>(manager.getCamera(QStringLiteral("detail-camera")));
    QVERIFY(fakeCamera);
    CameraCapabilities capabilities;
    capabilities.softwareTrigger = false;
    capabilities.hardwareTrigger = true;
    capabilities.continuousMode = false;
    capabilities.exposureControl = true;
    capabilities.gainControl = false;
    capabilities.frameRateControl = true;
    capabilities.roiControl = false;
    capabilities.pixelFormatControl = true;
    capabilities.whiteBalance = true;
    capabilities.depthData = true;
    capabilities.supportedOnWindows = false;
    capabilities.supportedOnLinux = true;
    fakeCamera->setCapabilities(capabilities);
    fakeCamera->setTriggerMode(TriggerMode::Hardware);
    fakeCamera->setExposureTime(7654.25);
    fakeCamera->setGain(3.5);
    fakeCamera->setFrameRate(47.5);
    fakeCamera->setRoi(11, 13, 640, 480);

    AgentActor actor;
    const int discoveryCalls = plugin.discoverCalls();
    const int createCalls = plugin.createCalls();
    const int connectCalls = counters.connectCalls;
    const int disconnectCalls = counters.disconnectCalls;
    const int startCalls = counters.startCalls;
    const int stopCalls = counters.stopCalls;
    const int triggerCalls = counters.triggerCalls;
    const int parameterSetCalls = counters.parameterSetCalls;
    const bool acquiring = fakeCamera->isAcquiring();
    const Project* projectBefore = ProjectManager::instance().currentProject();
    const bool projectWasModified = projectBefore && projectBefore->isModified();

    const QJsonObject details =
        actor.executeTool(QStringLiteral("get_camera_details"), {{"deviceId", QStringLiteral("detail-camera")}});
    QVERIFY(!details.contains(QStringLiteral("error")));
    QCOMPARE(details.value(QStringLiteral("deviceId")).toString(), QStringLiteral("detail-camera"));
    QCOMPARE(details.value(QStringLiteral("name")).toString(), QStringLiteral("detail-camera"));
    QCOMPARE(details.value(QStringLiteral("manufacturer")).toString(), QStringLiteral("test"));
    const QJsonObject actualCapabilities = details.value(QStringLiteral("capabilities")).toObject();
    QCOMPARE(actualCapabilities.value(QStringLiteral("softwareTrigger")).toBool(), false);
    QCOMPARE(actualCapabilities.value(QStringLiteral("hardwareTrigger")).toBool(), true);
    QCOMPARE(actualCapabilities.value(QStringLiteral("continuousMode")).toBool(), false);
    QCOMPARE(actualCapabilities.value(QStringLiteral("exposureControl")).toBool(), true);
    QCOMPARE(actualCapabilities.value(QStringLiteral("gainControl")).toBool(), false);
    QCOMPARE(actualCapabilities.value(QStringLiteral("frameRateControl")).toBool(), true);
    QCOMPARE(actualCapabilities.value(QStringLiteral("roiControl")).toBool(), false);
    QCOMPARE(actualCapabilities.value(QStringLiteral("pixelFormatControl")).toBool(), true);
    QCOMPARE(actualCapabilities.value(QStringLiteral("whiteBalance")).toBool(), true);
    QCOMPARE(actualCapabilities.value(QStringLiteral("depthData")).toBool(), true);
    QCOMPARE(actualCapabilities.value(QStringLiteral("supportedOnWindows")).toBool(), false);
    QCOMPARE(actualCapabilities.value(QStringLiteral("supportedOnLinux")).toBool(), true);
    const QJsonObject parameters = details.value(QStringLiteral("parameters")).toObject();
    QCOMPARE(parameters.value(QStringLiteral("triggerMode")).toString(), QStringLiteral("hardware"));
    QCOMPARE(parameters.value(QStringLiteral("exposureTime")).toDouble(), 7654.25);
    QCOMPARE(parameters.value(QStringLiteral("gain")).toDouble(), 3.5);
    QCOMPARE(parameters.value(QStringLiteral("frameRate")).toDouble(), 47.5);
    const QJsonObject roi = parameters.value(QStringLiteral("roi")).toObject();
    QCOMPARE(roi.value(QStringLiteral("x")).toInt(), 11);
    QCOMPARE(roi.value(QStringLiteral("y")).toInt(), 13);
    QCOMPARE(roi.value(QStringLiteral("width")).toInt(), 640);
    QCOMPARE(roi.value(QStringLiteral("height")).toInt(), 480);

    QCOMPARE(actor.executeTool(QStringLiteral("get_camera_details"), {{"deviceId", QString()}})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("MISSING_PARAM"));
    QCOMPARE(actor.executeTool(QStringLiteral("get_camera_details"), {{"deviceId", QStringLiteral("missing-camera")}})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_NOT_FOUND"));
    QCOMPARE(actor.executeTool(QStringLiteral("get_camera_details"), {{"deviceId", QStringLiteral("offline-camera")}})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_NOT_CONNECTED"));

    QJsonObject offThreadResult;
    std::thread otherThread([&actor, &offThreadResult]() {
        offThreadResult =
            actor.executeTool(QStringLiteral("get_camera_details"), {{"deviceId", QStringLiteral("detail-camera")}});
    });
    otherThread.join();
    QCOMPARE(offThreadResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_THREAD_AFFINITY"));
    QCOMPARE(counters.disconnectCalls, disconnectCalls);

    fakeCamera->disconnect(); // Manager still owns the instance, but the device connection is gone.
    const int disconnectCallsAfterExternalDrop = counters.disconnectCalls;
    QCOMPARE(actor.executeTool(QStringLiteral("get_camera_details"), {{"deviceId", QStringLiteral("detail-camera")}})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_NOT_CONNECTED"));
    QCOMPARE(counters.disconnectCalls, disconnectCallsAfterExternalDrop);

    QCOMPARE(plugin.discoverCalls(), discoveryCalls);
    QCOMPARE(plugin.createCalls(), createCalls);
    QCOMPARE(counters.connectCalls, connectCalls);
    QCOMPARE(counters.startCalls, startCalls);
    QCOMPARE(counters.stopCalls, stopCalls);
    QCOMPARE(counters.triggerCalls, triggerCalls);
    QCOMPARE(counters.parameterSetCalls, parameterSetCalls);
    QCOMPARE(fakeCamera->exposureTime(), 7654.25);
    QCOMPARE(fakeCamera->gain(), 3.5);
    QCOMPARE(fakeCamera->frameRate(), 47.5);
    QCOMPARE(fakeCamera->roi(), QRect(11, 13, 640, 480));
    QCOMPARE(fakeCamera->isAcquiring(), acquiring);
    QCOMPARE(ProjectManager::instance().currentProject(), projectBefore);
    QCOMPARE(projectBefore && projectBefore->isModified(), projectWasModified);
}

void TestCameraManager::testLegacyAgentCameraParameterWriteContract() {
    ToolSchema::instance().registerDefaultTools();
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-parameter-legacy"),
                            {cameraInfo({}, QStringLiteral("parameter-legacy-device"))}, &counters);
    plugin.setParameterControlEnabled(true);
    FakeCameraPlugin oldPlugin(QStringLiteral("test-camera-parameter-legacy-old"),
                               {cameraInfo({}, QStringLiteral("parameter-old-device"))}, &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    manager.registerCameraPlugin(&oldPlugin);
    m_registeredIds.append(plugin.pluginId());
    m_registeredIds.append(oldPlugin.pluginId());
    QVERIFY(manager.connectCamera(QStringLiteral("parameter-legacy-device")));
    QVERIFY(manager.connectCamera(QStringLiteral("parameter-old-device")));
    auto* camera = dynamic_cast<FakeCamera*>(manager.getCamera(QStringLiteral("parameter-legacy-device")));
    QVERIFY(camera);
    camera->setRoi(0, 0, 640, 480);

    AgentActor actor;
    const auto details = actor.executeTool(QStringLiteral("get_camera_details"),
                                           {{QStringLiteral("deviceId"), QStringLiteral("parameter-legacy-device")}});
    const QString generation = details.value(QStringLiteral("cameraSessionGeneration")).toString();
    const QString serial = details.value(QStringLiteral("serialNumber")).toString();
    QVERIFY(!generation.isEmpty());
    QVERIFY(!serial.isEmpty());
    const auto oldDetails = actor.executeTool(QStringLiteral("get_camera_details"),
                                              {{QStringLiteral("deviceId"), QStringLiteral("parameter-old-device")}});
    const int settersBeforeOldPluginWrite = counters.parameterSetCalls;
    const auto oldPluginWrite = actor.executeTool(
        QStringLiteral("set_camera_parameter"),
        {{QStringLiteral("deviceId"), QStringLiteral("parameter-old-device")},
         {QStringLiteral("cameraSessionGeneration"), oldDetails.value(QStringLiteral("cameraSessionGeneration"))},
         {QStringLiteral("serialNumber"), oldDetails.value(QStringLiteral("serialNumber"))},
         {QStringLiteral("parameter"), QStringLiteral("exposureTime")},
         {QStringLiteral("value"), 2000.0}});
    QCOMPARE(oldPluginWrite.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_PARAMETER_RANGE_UNAVAILABLE"));
    QCOMPARE(counters.parameterSetCalls, settersBeforeOldPluginWrite);
    auto exposureWrite = [&generation, &serial]() {
        return QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("parameter-legacy-device")},
                           {QStringLiteral("cameraSessionGeneration"), generation},
                           {QStringLiteral("serialNumber"), serial},
                           {QStringLiteral("parameter"), QStringLiteral("exposureTime")},
                           {QStringLiteral("value"), 4200.0}};
    };

    CameraCapabilities capabilities = camera->capabilities();
    capabilities.gainControl = false;
    camera->setCapabilities(capabilities);
    const int initialSetterCount = counters.parameterSetCalls;
    const auto unsupported =
        actor.executeTool(QStringLiteral("set_camera_parameter"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("parameter-legacy-device")},
                                      {QStringLiteral("cameraSessionGeneration"), generation},
                                      {QStringLiteral("serialNumber"), serial},
                                      {QStringLiteral("parameter"), QStringLiteral("gain")},
                                      {QStringLiteral("value"), 2.0}});
    QCOMPARE(unsupported.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_PARAMETER_UNSUPPORTED"));
    QCOMPARE(counters.parameterSetCalls, initialSetterCount);
    capabilities.gainControl = true;
    camera->setCapabilities(capabilities);
    counters.parameterOutOfRange = true;
    QCOMPARE(actor.executeTool(QStringLiteral("set_camera_parameter"), exposureWrite())
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_PARAMETER_OUT_OF_RANGE"));
    QCOMPARE(counters.parameterSetCalls, initialSetterCount);
    counters.parameterOutOfRange = false;
    auto invalidType = exposureWrite();
    invalidType.insert(QStringLiteral("value"), QStringLiteral("4200"));
    QCOMPARE(actor.executeTool(QStringLiteral("set_camera_parameter"), invalidType)
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("INVALID_PARAM"));
    QCOMPARE(counters.parameterSetCalls, initialSetterCount);
    counters.parameterValidationThrows = true;
    QCOMPARE(actor.executeTool(QStringLiteral("set_camera_parameter"), exposureWrite())
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_PARAMETER_VALIDATE_FAILED"));
    QCOMPARE(counters.parameterSetCalls, initialSetterCount);
    counters.parameterValidationThrows = false;

    AgentController& controller = AgentController::instance();
    qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
    controller.clearConversation();
    controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
    const auto denied = controller.handleToolCall(QStringLiteral("set_camera_parameter"), exposureWrite());
    QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
    QCOMPARE(counters.parameterSetCalls, initialSetterCount);

    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
    const double exposureBeforeConfirmation = camera->exposureTime();
    const auto rejectedConfirmation =
        controller.handleToolCall(QStringLiteral("set_camera_parameter"), exposureWrite());
    QCOMPARE(rejectedConfirmation.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    const QJsonArray effects = controller.pendingConfirmationPayload()
                                   .value(QStringLiteral("context"))
                                   .toObject()
                                   .value(QStringLiteral("sideEffects"))
                                   .toArray();
    QCOMPARE(effects.size(), 1);
    QCOMPARE(effects.first().toString(), QStringLiteral("camera_parameter:parameter-legacy-device:exposureTime=4200"));
    QCOMPARE(counters.parameterSetCalls, initialSetterCount);
    QCOMPARE(camera->exposureTime(), exposureBeforeConfirmation);
    const QString rejectedConfirmationId = controller.pendingConfirmationId();
    QVERIFY(!rejectedConfirmationId.isEmpty());
    QVERIFY(controller.rejectPendingTools(rejectedConfirmationId));
    QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("set_camera_parameter"));
    QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
    QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
    QCOMPARE(counters.parameterSetCalls, initialSetterCount);
    QCOMPARE(camera->exposureTime(), exposureBeforeConfirmation);

    const auto confirmation = controller.handleToolCall(QStringLiteral("set_camera_parameter"), exposureWrite());
    QCOMPARE(confirmation.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    const QString approvedConfirmationId = controller.pendingConfirmationId();
    QVERIFY(!approvedConfirmationId.isEmpty());
    QVERIFY(approvedConfirmationId != rejectedConfirmationId);
    QVERIFY(controller.confirmPendingTools(approvedConfirmationId));
    QTRY_COMPARE_WITH_TIMEOUT(counters.parameterSetCalls, initialSetterCount + 1, 1000);
    QCOMPARE(camera->exposureTime(), 4200.0);
    QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("set_camera_parameter"));
    QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
    QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
    const auto refreshed = actor.executeTool(QStringLiteral("get_camera_details"),
                                             {{QStringLiteral("deviceId"), QStringLiteral("parameter-legacy-device")}});
    QVERIFY(refreshed.value(QStringLiteral("cameraSessionGeneration")).toString() != generation);
    const QString refreshedGeneration = refreshed.value(QStringLiteral("cameraSessionGeneration")).toString();
    auto refreshedWrite = exposureWrite();
    refreshedWrite.insert(QStringLiteral("cameraSessionGeneration"), refreshedGeneration);
    refreshedWrite.insert(QStringLiteral("value"), 4500.0);
    counters.clampExposureReadback = true;
    const auto mismatch = actor.executeTool(QStringLiteral("set_camera_parameter"), refreshedWrite);
    QCOMPARE(mismatch.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_PARAMETER_READBACK_MISMATCH"));
    QCOMPARE(mismatch.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("changed_or_clamped"));
    QCOMPARE(mismatch.value(QStringLiteral("actual")).toDouble(), 2250.0);
    QCOMPARE(mismatch.value(QStringLiteral("retryable")).toBool(), false);
    QCOMPARE(counters.parameterSetCalls, initialSetterCount + 2);
    counters.clampExposureReadback = false;
    refreshedWrite.insert(QStringLiteral("cameraSessionGeneration"),
                          mismatch.value(QStringLiteral("cameraSessionGeneration")).toString());
    counters.parameterSetterThrows = true;
    const auto uncertain = actor.executeTool(QStringLiteral("set_camera_parameter"), refreshedWrite);
    QCOMPARE(uncertain.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_PARAMETER_WRITE_UNCONFIRMED"));
    QCOMPARE(uncertain.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
    QCOMPARE(uncertain.value(QStringLiteral("retryable")).toBool(), false);
    QCOMPARE(uncertain.value(QStringLiteral("actual")).toDouble(), 2250.0);
    QCOMPARE(counters.parameterSetCalls, initialSetterCount + 3);
    counters.parameterSetterThrows = false;
    refreshedWrite.insert(QStringLiteral("cameraSessionGeneration"),
                          uncertain.value(QStringLiteral("cameraSessionGeneration")).toString());
    counters.parameterReadbackThrowsAfterWrite = true;
    const auto unreadable = actor.executeTool(QStringLiteral("set_camera_parameter"), refreshedWrite);
    QCOMPARE(unreadable.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_PARAMETER_WRITE_UNCONFIRMED"));
    QCOMPARE(unreadable.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
    QVERIFY(unreadable.value(QStringLiteral("actual")).isUndefined());
    QCOMPARE(counters.parameterSetCalls, initialSetterCount + 4);
    counters.parameterReadbackThrowsAfterWrite = false;

    auto setWithFreshBinding = [&actor, &serial](const QString& parameter, const QJsonValue& value) {
        const QJsonObject current =
            actor.executeTool(QStringLiteral("get_camera_details"),
                              {{QStringLiteral("deviceId"), QStringLiteral("parameter-legacy-device")}});
        const QJsonObject response = actor.executeTool(
            QStringLiteral("set_camera_parameter"),
            {{QStringLiteral("deviceId"), QStringLiteral("parameter-legacy-device")},
             {QStringLiteral("cameraSessionGeneration"), current.value(QStringLiteral("cameraSessionGeneration"))},
             {QStringLiteral("serialNumber"), serial},
             {QStringLiteral("parameter"), parameter},
             {QStringLiteral("value"), value}});
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        QCOMPARE(response.value(QStringLiteral("actual")), value);
    };
    setWithFreshBinding(QStringLiteral("gain"), 2.5);
    setWithFreshBinding(QStringLiteral("frameRate"), 25.0);
    setWithFreshBinding(QStringLiteral("triggerMode"), QStringLiteral("hardware"));
    setWithFreshBinding(QStringLiteral("roi"), QJsonObject{{QStringLiteral("x"), 0},
                                                           {QStringLiteral("y"), 0},
                                                           {QStringLiteral("width"), 320},
                                                           {QStringLiteral("height"), 240}});
    QCOMPARE(counters.parameterSetCalls, initialSetterCount + 8);

    QList<AgentActor::ToolInvocation> duplicateWrites{
        {QStringLiteral("param-a"), QStringLiteral("set_camera_parameter"), exposureWrite()},
        {QStringLiteral("param-b"), QStringLiteral("set_camera_parameter"), exposureWrite()}};
    const auto duplicateBatch = actor.executeTools(duplicateWrites, QStringLiteral("duplicate writes"));
    QCOMPARE(duplicateBatch.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_OPERATION_BATCH_CONFLICT"));
    QCOMPARE(duplicateBatch.value(QStringLiteral("executed")).toInt(), 0);
    QCOMPARE(duplicateBatch.value(QStringLiteral("results")).toArray().size(), 2);
    QCOMPARE(counters.parameterSetCalls, initialSetterCount + 8);

    auto staleWrite = exposureWrite();
    staleWrite.insert(QStringLiteral("cameraSessionGeneration"), generation);
    QCOMPARE(actor.executeTool(QStringLiteral("set_camera_parameter"), staleWrite)
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_TARGET_CHANGED"));
    QCOMPARE(counters.parameterSetCalls, initialSetterCount + 8);

    manager.disconnectAll();
    QString error;
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    QVERIFY(manager.tryUnregisterCameraPlugin(oldPlugin.pluginId(), &error));
    m_registeredIds.removeAll(plugin.pluginId());
    m_registeredIds.removeAll(oldPlugin.pluginId());
}

void TestCameraManager::testAgentCameraLifecycleConfirmationAndLeaseDelay() {
    ToolSchema::instance().registerDefaultTools();
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-agent-lifecycle"),
                            {cameraInfo({}, QStringLiteral("lifecycle-camera"), QStringLiteral("Lifecycle Camera")),
                             cameraInfo({}, QStringLiteral("failing-camera"), QStringLiteral("Failing Camera"))},
                            &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    struct CleanupRegisteredPlugin {
        CameraManager& manager;
        FakeCameraPlugin& plugin;
        ~CleanupRegisteredPlugin() {
            manager.disconnectAll();
            manager.tryUnregisterCameraPlugin(plugin.pluginId());
        }
    } cleanup{manager, plugin};

    AgentActor actor;
    AgentController& controller = AgentController::instance();
    controller.clearConversation();
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    QSignalSpy connectedSpy(&manager, &CameraManager::cameraConnected);
    QSignalSpy disconnectedSpy(&manager, &CameraManager::cameraDisconnected);
    const int initialDiscoverCalls = plugin.discoverCalls();

    const QJsonObject unknown = actor.executeTool("connect_camera", {{"deviceId", "missing-camera"}});
    QCOMPARE(unknown.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_NOT_FOUND"));
    const QJsonObject missingDisconnect = actor.executeTool("disconnect_camera", {});
    QCOMPARE(missingDisconnect.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
    QCOMPARE(missingDisconnect.value(QStringLiteral("details"))
                 .toObject()
                 .value(QStringLiteral("hardwareDisconnectConfirmed"))
                 .toBool(),
             false);
    QCOMPARE(plugin.discoverCalls(), initialDiscoverCalls);
    QCOMPARE(plugin.createCalls(), 0);
    QCOMPARE(counters.connectCalls, 0);

    const QJsonObject rejected = controller.handleToolCall("connect_camera", {{"deviceId", "lifecycle-camera"}});
    QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QCOMPARE(plugin.discoverCalls(), initialDiscoverCalls);
    QCOMPARE(counters.connectCalls, 0);
    const QJsonArray connectEffects = controller.pendingConfirmationPayload()
                                          .value(QStringLiteral("context"))
                                          .toObject()
                                          .value(QStringLiteral("sideEffects"))
                                          .toArray();
    QCOMPARE(connectEffects.size(), 1);
    QVERIFY(connectEffects.first().toString().startsWith(QStringLiteral("camera_connect:lifecycle-camera")));
    QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
    QCOMPARE(plugin.discoverCalls(), initialDiscoverCalls);
    QCOMPARE(counters.connectCalls, 0);
    QCOMPARE(connectedSpy.count(), 0);

    const QJsonObject approved = controller.handleToolCall("connect_camera", {{"deviceId", "lifecycle-camera"}});
    QCOMPARE(approved.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QCOMPARE(plugin.discoverCalls(), initialDiscoverCalls);
    QCOMPARE(plugin.createCalls(), 0);
    QCOMPARE(counters.connectCalls, 0);

    const QJsonObject duplicateConnect = actor.executeTool("connect_camera", {{"deviceId", "lifecycle-camera"}});
    QCOMPARE(duplicateConnect.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_SESSION_REQUIRED"));
    QVERIFY(manager.connectCamera(QStringLiteral("lifecycle-camera")));
    QCOMPARE(plugin.discoverCalls(), initialDiscoverCalls + 1);
    QCOMPARE(plugin.createCalls(), 1);
    QCOMPARE(counters.connectCalls, 1);
    QCOMPARE(connectedSpy.count(), 1);

    CameraLease heldLease = manager.acquireCamera(QStringLiteral("lifecycle-camera"));
    QVERIFY(heldLease);
    const QJsonObject disconnectPending =
        controller.handleToolCall("disconnect_camera", {{"deviceId", "lifecycle-camera"}});
    QCOMPARE(disconnectPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    const QJsonArray disconnectEffects = controller.pendingConfirmationPayload()
                                             .value(QStringLiteral("context"))
                                             .toObject()
                                             .value(QStringLiteral("sideEffects"))
                                             .toArray();
    QCOMPARE(disconnectEffects.size(), 1);
    QVERIFY(disconnectEffects.first().toString().startsWith(QStringLiteral("camera_disconnect:lifecycle-camera")));
    QCOMPARE(counters.disconnectCalls, 0);
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QVERIFY(manager.isConnected(QStringLiteral("lifecycle-camera")));
    QCOMPARE(counters.disconnectCalls, 0);

    const QJsonObject duplicatePending =
        controller.handleToolCall("disconnect_camera", {{"deviceId", "lifecycle-camera"}});
    QCOMPARE(duplicatePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QCOMPARE(counters.disconnectCalls, 0);
    const QJsonObject cachedWhilePending = actor.executeTool("list_cameras", {});
    bool stillReportedConnected = false;
    for (const QJsonValue& value : cachedWhilePending.value(QStringLiteral("cameras")).toArray()) {
        const QJsonObject camera = value.toObject();
        if (camera.value(QStringLiteral("deviceId")).toString() == QLatin1String("lifecycle-camera"))
            stillReportedConnected = camera.value(QStringLiteral("state")).toString() == QLatin1String("connected");
    }
    QVERIFY(stillReportedConnected);
    const QJsonObject repeatedPending = actor.executeTool("disconnect_camera", {{"deviceId", "lifecycle-camera"}});
    QCOMPARE(repeatedPending.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_SESSION_REQUIRED"));
    QCOMPARE(repeatedPending.value(QStringLiteral("details"))
                 .toObject()
                 .value(QStringLiteral("hardwareDisconnectConfirmed"))
                 .toBool(),
             false);
    QCOMPARE(counters.disconnectCalls, 0);

    manager.disconnectCamera(QStringLiteral("lifecycle-camera"));
    QVERIFY(manager.isConnected(QStringLiteral("lifecycle-camera")));
    heldLease = CameraLease();
    QVERIFY(!manager.isConnected(QStringLiteral("lifecycle-camera")));
    QCOMPARE(counters.disconnectCalls, 1);
    QCOMPARE(disconnectedSpy.count(), 1);
    const QJsonObject alreadyDisconnected = actor.executeTool("disconnect_camera", {{"deviceId", "lifecycle-camera"}});
    QCOMPARE(alreadyDisconnected.value(QStringLiteral("status")).toString(), QStringLiteral("already_disconnected"));
    QCOMPARE(alreadyDisconnected.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), false);
    QCOMPARE(counters.disconnectCalls, 1);

    counters.connectSucceeds = false;
    const QJsonObject connectFailure = actor.executeTool("connect_camera", {{"deviceId", "failing-camera"}});
    QCOMPARE(connectFailure.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_SESSION_REQUIRED"));
    QCOMPARE(counters.connectCalls, 1);
    QCOMPARE(counters.disconnectCalls, 1);
    QCOMPARE(connectedSpy.count(), 1);
    controller.clearConversation();
}

void TestCameraManager::testAgentCameraLifecycleRejectsWrongThreadAndBusyEngine() {
    ToolSchema::instance().registerDefaultTools();
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-agent-gates"),
                            {cameraInfo({}, QStringLiteral("gated-camera"), QStringLiteral("Gated Camera"))},
                            &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    struct CleanupRegisteredPlugin {
        CameraManager& manager;
        FakeCameraPlugin& plugin;
        ~CleanupRegisteredPlugin() {
            manager.disconnectAll();
            manager.tryUnregisterCameraPlugin(plugin.pluginId());
        }
    } cleanup{manager, plugin};
    AgentActor actor;

    QJsonObject wrongThreadConnect;
    std::thread worker([&actor, &wrongThreadConnect]() {
        wrongThreadConnect = actor.executeTool("connect_camera", {{"deviceId", "gated-camera"}});
    });
    worker.join();
    QCOMPARE(wrongThreadConnect.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    QJsonObject wrongThreadDisconnect;
    std::thread secondWorker([&actor, &wrongThreadDisconnect]() {
        wrongThreadDisconnect = actor.executeTool("disconnect_camera", {{"deviceId", "gated-camera"}});
    });
    secondWorker.join();
    QCOMPARE(wrongThreadDisconnect.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    QCOMPARE(wrongThreadDisconnect.value(QStringLiteral("details"))
                 .toObject()
                 .value(QStringLiteral("hardwareDisconnectConfirmed"))
                 .toBool(),
             false);
    QCOMPARE(plugin.discoverCalls(), 1);
    QCOMPARE(plugin.createCalls(), 0);
    QCOMPARE(counters.connectCalls, 0);
    QCOMPARE(counters.disconnectCalls, 0);

    Project* busyProject = ProjectManager::instance().newProject();
    QVERIFY(busyProject);
    ModuleInstance blockingInstance;
    blockingInstance.id = QStringLiteral("blocking_camera_gate");
    blockingInstance.moduleId = QStringLiteral("com.deeplux.test.blockingcameragate");
    busyProject->addModule(blockingInstance);
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(busyProject, [](const ModuleInstance&) { return new BlockingCameraGateModule(); }));
    QVERIFY(engine.runOnceAsync());
    QTRY_VERIFY_WITH_TIMEOUT(engine.isBusy(), 1000);
    QCOMPARE(actor.executeTool("connect_camera", {{"deviceId", "gated-camera"}})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("ENGINE_BUSY"));
    const QJsonObject busyDisconnect = actor.executeTool("disconnect_camera", {{"deviceId", "gated-camera"}});
    QCOMPARE(busyDisconnect.value(QStringLiteral("errorCode")).toString(), QStringLiteral("ENGINE_BUSY"));
    QCOMPARE(busyDisconnect.value(QStringLiteral("details"))
                 .toObject()
                 .value(QStringLiteral("hardwareDisconnectConfirmed"))
                 .toBool(),
             false);
    QCOMPARE(plugin.discoverCalls(), 1);
    QCOMPARE(plugin.createCalls(), 0);
    QCOMPARE(counters.connectCalls, 0);
    QCOMPARE(counters.disconnectCalls, 0);
    engine.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 2000);
    ProjectManager::instance().closeProject();
}

void TestCameraManager::testDriverOwnershipAndSafeUnregister() {
    CameraCounters countersA;
    CameraCounters countersB;
    FakeCameraPlugin pluginA(QStringLiteral("test-camera-owner-a"), {cameraInfo({}, QStringLiteral("device-a"))},
                             &countersA);
    FakeCameraPlugin pluginB(QStringLiteral("test-camera-owner-b"), {cameraInfo({}, QStringLiteral("device-b"))},
                             &countersB);
    CameraManager& manager = CameraManager::instance();

    manager.registerCameraPlugin(&pluginA);
    m_registeredIds.append(pluginA.pluginId());
    manager.registerCameraPlugin(&pluginB);
    m_registeredIds.append(pluginB.pluginId());
    QVERIFY(manager.connectCamera(QStringLiteral("device-a")));
    QVERIFY(manager.connectCamera(QStringLiteral("device-b")));
    QVERIFY(manager.getCamera(QStringLiteral("device-a")));
    QVERIFY(manager.getCamera(QStringLiteral("device-b")));

    QString error;
    QVERIFY(!manager.tryUnregisterCameraPlugin(pluginA.pluginId(), &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_IN_USE"));
    QCOMPARE(manager.findPlugin(pluginA.pluginId()), &pluginA);
    QVERIFY(manager.getCamera(QStringLiteral("device-a")));
    QVERIFY(manager.getCamera(QStringLiteral("device-b")));

    manager.disconnectCamera(QStringLiteral("device-a"));
    QVERIFY(manager.tryUnregisterCameraPlugin(pluginA.pluginId(), &error));
    QVERIFY(!manager.findPlugin(pluginA.pluginId()));
    QCOMPARE(manager.cameraState(QStringLiteral("device-a")), CameraState::Disconnected);
    QVERIFY(manager.getCamera(QStringLiteral("device-b")));
    QCOMPARE(manager.findPlugin(pluginB.pluginId()), &pluginB);
    QCOMPARE(countersA.disconnectCalls, 1);

    QVERIFY(!manager.tryUnregisterCameraPlugin(QStringLiteral("missing-camera-plugin"), &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_NOT_FOUND"));
    QVERIFY(!manager.tryUnregisterCameraPlugin(pluginA.pluginId(), &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_NOT_FOUND"));
    manager.disconnectAll();
}

void TestCameraManager::testAcquiringCameraStopsBeforeDisconnectAll() {
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-acquiring"), {cameraInfo({}, QStringLiteral("device-grab"))},
                            &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QVERIFY(manager.connectCamera(QStringLiteral("device-grab")));
    ICamera* camera = manager.getCamera(QStringLiteral("device-grab"));
    QVERIFY(camera);
    QVERIFY(camera->startAcquisition());

    QString error;
    QVERIFY(!manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_IN_USE"));
    manager.disconnectAll();
    QCOMPARE(counters.stopCalls, 1);
    QCOMPARE(counters.disconnectCalls, 1);
    QCOMPARE(counters.callOrder.mid(counters.callOrder.size() - 2),
             QStringList({QStringLiteral("stop"), QStringLiteral("disconnect")}));
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
}

void TestCameraManager::testCameraLeaseDefersDestruction() {
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-lease"), {cameraInfo({}, QStringLiteral("device-lease"))},
                            &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QVERIFY(manager.connectCamera(QStringLiteral("device-lease")));

    CameraLease lease = manager.acquireCamera(QStringLiteral("device-lease"));
    QVERIFY(lease);
    manager.disconnectCamera(QStringLiteral("device-lease"));
    QVERIFY(manager.getCamera(QStringLiteral("device-lease")));
    QVERIFY(!manager.acquireCamera(QStringLiteral("device-lease")));
    QVERIFY(!manager.connectCamera(QStringLiteral("device-lease")));
    QCOMPARE(counters.disconnectCalls, 0);

    lease = CameraLease();
    QVERIFY(!manager.getCamera(QStringLiteral("device-lease")));
    QCOMPARE(counters.disconnectCalls, 1);
    QCOMPARE(counters.destroyedCalls, 1);
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
}

void TestCameraManager::testDisconnectAllWaitsForLeaseThenReopensConnections() {
    CameraCounters counters;
    FakeCameraPlugin plugin(
        QStringLiteral("test-camera-disconnect-all-lease"),
        {cameraInfo({}, QStringLiteral("device-lease-held")), cameraInfo({}, QStringLiteral("device-lease-peer"))},
        &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QVERIFY(manager.connectCamera(QStringLiteral("device-lease-held")));
    QVERIFY(manager.connectCamera(QStringLiteral("device-lease-peer")));
    CameraLease lease = manager.acquireCamera(QStringLiteral("device-lease-held"));
    QVERIFY(lease);

    manager.disconnectAll();
    QVERIFY(manager.getCamera(QStringLiteral("device-lease-held")));
    QVERIFY(!manager.getCamera(QStringLiteral("device-lease-peer")));
    QVERIFY(!manager.acquireCamera(QStringLiteral("device-lease-peer")));
    QVERIFY(!manager.connectCamera(QStringLiteral("device-lease-peer")));

    lease = CameraLease();
    QVERIFY(!manager.getCamera(QStringLiteral("device-lease-held")));
    QVERIFY(manager.connectCamera(QStringLiteral("device-lease-peer")));
    manager.disconnectAll();
}

void TestCameraManager::testMetadataNormalizationAndDuplicateIds() {
    CameraCounters countersA;
    CameraCounters countersB;
    FakeCameraPlugin pluginA(QStringLiteral("test-camera-duplicate-a"),
                             {cameraInfo({}, QStringLiteral("shared-device"))}, &countersA);
    FakeCameraPlugin pluginB(QStringLiteral("test-camera-duplicate-b"),
                             {cameraInfo({}, QStringLiteral("shared-device"))}, &countersB);
    CameraManager& manager = CameraManager::instance();
    QSignalSpy discoveredSpy(&manager, &CameraManager::cameraDiscovered);
    QSignalSpy registeredSpy(&manager, &CameraManager::pluginRegistered);

    CameraCounters invalidCounters;
    FakeCameraPlugin emptyIdPlugin({}, {cameraInfo({}, QStringLiteral("invalid-device"))}, &invalidCounters);
    manager.registerCameraPlugin(&emptyIdPlugin);
    QCOMPARE(registeredSpy.count(), 0);
    QCOMPARE(discoveredSpy.count(), 0);

    manager.registerCameraPlugin(&pluginA);
    m_registeredIds.append(pluginA.pluginId());
    manager.registerCameraPlugin(&pluginB);
    m_registeredIds.append(pluginB.pluginId());
    QCOMPARE(discoveredSpy.count(), 1);
    QCOMPARE(registeredSpy.count(), 2);
    QVERIFY(manager.connectCamera(QStringLiteral("shared-device")));
    QCOMPARE(pluginA.createCalls(), 1);
    QCOMPARE(pluginA.lastCreatePluginId(), pluginA.pluginId());
    QCOMPARE(pluginB.createCalls(), 0);
    manager.disconnectAll();
}

void TestCameraManager::testRefreshReleasesStaleDeviceOwnership() {
    CameraCounters countersA;
    CameraCounters countersB;
    FakeCameraPlugin pluginA(QStringLiteral("test-camera-refresh-a"), {cameraInfo({}, QStringLiteral("device-reused"))},
                             &countersA);
    FakeCameraPlugin pluginB(QStringLiteral("test-camera-refresh-b"), {cameraInfo({}, QStringLiteral("device-reused"))},
                             &countersB);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&pluginA);
    m_registeredIds.append(pluginA.pluginId());
    pluginA.setCameras({});
    manager.refreshCameras();

    manager.registerCameraPlugin(&pluginB);
    m_registeredIds.append(pluginB.pluginId());
    QVERIFY(manager.connectCamera(QStringLiteral("device-reused")));
    QCOMPARE(pluginA.createCalls(), 0);
    QCOMPARE(pluginB.createCalls(), 1);
    manager.disconnectAll();

    CameraCounters countersOwner;
    CameraCounters countersDuplicate;
    FakeCameraPlugin zOwner(QStringLiteral("zz-camera-owner"), {cameraInfo({}, QStringLiteral("stable-device"))},
                            &countersOwner);
    FakeCameraPlugin aDuplicate(QStringLiteral("aa-camera-duplicate"),
                                {cameraInfo({}, QStringLiteral("stable-device"))}, &countersDuplicate);
    manager.registerCameraPlugin(&zOwner);
    m_registeredIds.append(zOwner.pluginId());
    manager.registerCameraPlugin(&aDuplicate);
    m_registeredIds.append(aDuplicate.pluginId());
    manager.refreshCameras();
    QVERIFY(manager.connectCamera(QStringLiteral("stable-device")));
    QCOMPARE(zOwner.createCalls(), 1);
    QCOMPARE(aDuplicate.createCalls(), 0);
    manager.disconnectAll();
}

void TestCameraManager::testFailedCameraCreationAndConnection() {
    CameraCounters creationCounters;
    creationCounters.createSucceeds = false;
    FakeCameraPlugin creationPlugin(QStringLiteral("test-camera-create-failure"),
                                    {cameraInfo({}, QStringLiteral("device-create-fail"))}, &creationCounters);
    CameraManager& manager = CameraManager::instance();
    QSignalSpy errorSpy(&manager, &CameraManager::errorOccurred);
    manager.registerCameraPlugin(&creationPlugin);
    m_registeredIds.append(creationPlugin.pluginId());
    QVERIFY(!manager.connectCamera(QStringLiteral("device-create-fail")));
    QVERIFY(!manager.getCamera(QStringLiteral("device-create-fail")));
    QCOMPARE(errorSpy.count(), 1);
    QVERIFY(manager.tryUnregisterCameraPlugin(creationPlugin.pluginId()));
    m_registeredIds.removeLast();

    CameraCounters connectionCounters;
    connectionCounters.connectSucceeds = false;
    FakeCameraPlugin connectionPlugin(QStringLiteral("test-camera-connect-failure"),
                                      {cameraInfo({}, QStringLiteral("device-connect-fail"))}, &connectionCounters);
    manager.registerCameraPlugin(&connectionPlugin);
    m_registeredIds.append(connectionPlugin.pluginId());
    bool retrySucceeded = false;
    const auto retryConnection =
        connect(&manager, &CameraManager::errorOccurred, &manager, [&](const QString& id, const QString&) {
            if (id == QStringLiteral("device-connect-fail")) {
                connectionCounters.connectSucceeds = true;
                retrySucceeded = manager.connectCamera(id);
            }
        });
    QVERIFY(!manager.connectCamera(QStringLiteral("device-connect-fail")));
    disconnect(retryConnection);
    QVERIFY(retrySucceeded);
    QVERIFY(manager.getCamera(QStringLiteral("device-connect-fail")));
    QCOMPARE(connectionCounters.disconnectCalls, 1);
    QCOMPARE(errorSpy.count(), 2);
    manager.disconnectAll();
}

void TestCameraManager::testReconnectAndDisconnectAllReentrancy() {
    CameraCounters counters;
    FakeCameraPlugin plugin(
        QStringLiteral("test-camera-reconnect"),
        {cameraInfo({}, QStringLiteral("device-reconnect")), cameraInfo({}, QStringLiteral("device-reconnect-batch"))},
        &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QVERIFY(manager.connectCamera(QStringLiteral("device-reconnect")));
    QVERIFY(manager.connectCamera(QStringLiteral("device-reconnect-batch")));
    ICamera* oldCamera = manager.getCamera(QStringLiteral("device-reconnect"));
    QVERIFY(oldCamera);
    oldCamera->disconnect();
    QVERIFY(manager.connectCamera(QStringLiteral("device-reconnect")));
    QCOMPARE(plugin.createCalls(), 3);
    QCOMPARE(counters.destroyedCalls, 1);

    bool reentrantConnectResult = true;
    bool pendingLeaseResult = true;
    const auto connection = connect(&manager, &CameraManager::cameraDisconnected, &manager, [&](const QString& id) {
        if (id == QStringLiteral("device-reconnect"))
            reentrantConnectResult = manager.connectCamera(id);
        if (id == QStringLiteral("device-reconnect"))
            pendingLeaseResult = static_cast<bool>(manager.acquireCamera(QStringLiteral("device-reconnect-batch")));
    });
    manager.disconnectAll();
    disconnect(connection);
    QVERIFY(!reentrantConnectResult);
    QVERIFY(!pendingLeaseResult);
    QVERIFY(!manager.getCamera(QStringLiteral("device-reconnect")));
    QVERIFY(!manager.getCamera(QStringLiteral("device-reconnect-batch")));
    QCOMPARE(counters.destroyedCalls, 3);
}

void TestCameraManager::testLifecycleSignalReentrancyAndThreadAffinity() {
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-reentrant"),
                            {cameraInfo({}, QStringLiteral("device-reentrant"))}, &counters);
    CameraManager& manager = CameraManager::instance();
    QString reentrantError;
    QSignalSpy registeredSpy(&manager, &CameraManager::pluginRegistered);
    const auto connection =
        connect(&manager, &CameraManager::cameraDiscovered, &manager, [&](const QString&, const QString&) {
            QVERIFY(!manager.tryUnregisterCameraPlugin(plugin.pluginId(), &reentrantError));
        });
    manager.registerCameraPlugin(&plugin);
    disconnect(connection);
    m_registeredIds.append(plugin.pluginId());
    QCOMPARE(reentrantError, QStringLiteral("CAMERA_MANAGER_LIFECYCLE_REENTRANT"));
    QCOMPARE(manager.findPlugin(plugin.pluginId()), &plugin);
    QCOMPARE(registeredSpy.count(), 1);

    QString threadError;
    std::atomic_bool offThreadRegistered{false};
    std::thread worker([&]() {
        manager.registerCameraPlugin(&plugin);
        threadError.clear();
        offThreadRegistered.store(manager.tryUnregisterCameraPlugin(plugin.pluginId(), &threadError));
    });
    worker.join();
    QVERIFY(!offThreadRegistered.load());
    QCOMPARE(threadError, QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    QCOMPARE(manager.findPlugin(plugin.pluginId()), &plugin);
    QCOMPARE(registeredSpy.count(), 1);

    int reentrantRegisterAttempts = 0;
    const auto unregisterConnection =
        connect(&manager, &CameraManager::pluginUnregistered, &manager, [&](const QString& id) {
            if (id == plugin.pluginId()) {
                ++reentrantRegisterAttempts;
                manager.registerCameraPlugin(&plugin);
            }
        });
    QString error;
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    disconnect(unregisterConnection);
    QCOMPARE(reentrantRegisterAttempts, 1);
    QVERIFY(!manager.findPlugin(plugin.pluginId()));
}

void TestCameraManager::testReentrantCameraTransitionsDoNotEmitStaleState() {
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-transition"),
                            {cameraInfo({}, QStringLiteral("device-transition"))}, &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QList<CameraState> states;
    const auto stateConnection = connect(&manager, &CameraManager::cameraStateChanged, &manager,
                                         [&](const QString&, CameraState state) { states.append(state); });

    const auto connectedSlot = connect(&manager, &CameraManager::cameraConnected, &manager,
                                       [&](const QString& id) { manager.disconnectCamera(id); });
    QVERIFY(manager.connectCamera(QStringLiteral("device-transition")));
    disconnect(connectedSlot);
    QCOMPARE(states, QList<CameraState>({CameraState::Connected, CameraState::Disconnected}));

    QVERIFY(manager.connectCamera(QStringLiteral("device-transition")));
    states.clear();
    const auto disconnectedSlot = connect(&manager, &CameraManager::cameraDisconnected, &manager,
                                          [&](const QString& id) { manager.connectCamera(id); });
    manager.disconnectCamera(QStringLiteral("device-transition"));
    disconnect(disconnectedSlot);
    QCOMPARE(states, QList<CameraState>({CameraState::Disconnected, CameraState::Connected}));
    manager.disconnectAll();
    disconnect(stateConnection);
}

void TestCameraManager::testConcurrentCameraTransitionSignalOrder() {
    CameraCounters counters;
    FakeCameraPlugin plugin(QStringLiteral("test-camera-concurrent-transition"),
                            {cameraInfo({}, QStringLiteral("device-concurrent-transition"))}, &counters);
    CameraManager& manager = CameraManager::instance();
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());

    QList<CameraState> states;
    std::mutex statesMutex;
    const auto stateConnection = connect(
        &manager, &CameraManager::cameraStateChanged, &manager,
        [&](const QString&, CameraState state) {
            std::lock_guard<std::mutex> lock(statesMutex);
            states.append(state);
        },
        Qt::DirectConnection);
    std::atomic_bool workerStarted{false};
    std::thread worker;
    const auto connectedConnection =
        connect(&manager, &CameraManager::cameraConnected, &manager, [&](const QString& id) {
            worker = std::thread([&, id]() {
                workerStarted.store(true);
                manager.disconnectCamera(id);
            });
            while (!workerStarted.load())
                std::this_thread::yield();
        });
    QVERIFY(manager.connectCamera(QStringLiteral("device-concurrent-transition")));
    worker.join();
    QCoreApplication::processEvents();
    disconnect(connectedConnection);
    disconnect(stateConnection);

    std::lock_guard<std::mutex> lock(statesMutex);
    QCOMPARE(states, QList<CameraState>({CameraState::Connected, CameraState::Disconnected}));
}

void TestCameraManager::testAsyncDiscoveryUsesQueuedResultAndHoldsPluginLease() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QCOMPARE(plugin.asyncStarts(), 1); // Registration performs one managed initial scan.
    const QList<CameraStatus> initialCameras = manager.knownCameras();
    QVERIFY(std::find_if(initialCameras.cbegin(), initialCameras.cend(), [](const CameraStatus& status) {
                return status.deviceId == QStringLiteral("fake-device");
            }) != initialCameras.cend());
    const int syncCallsAfterRegistration = plugin.syncCallCount();
    QSignalSpy finishedSpy(&manager, &CameraManager::asyncDiscoveryFinished);
    QSignalSpy settledSpy(&manager, &CameraManager::cameraDiscoverySettled);

    QString error;
    QVERIFY(manager.startDiscoveryAsync(plugin.pluginId(), 1001, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(manager.pendingDiscoveryCount(), 1);
    QCOMPARE(plugin.asyncStarts(), 2);
    QVERIFY(!manager.startDiscoveryAsync(plugin.pluginId(), 1002, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_DISCOVERY_BUSY"));

    QVERIFY(manager.requestDiscoveryCancel(1001, &error));
    QCOMPARE(plugin.cancelCalls(), 1);
    QVERIFY(manager.requestDiscoveryCancel(1001, &error));
    QCOMPARE(plugin.cancelCalls(), 1);
    QVERIFY(!manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_IN_USE"));
    QVERIFY(finishedSpy.isEmpty()); // completion is queued and the delayed worker is still pending

    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    const QJsonObject result = qvariant_cast<QJsonObject>(finishedSpy.first().at(1));
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QCOMPARE(result.value(QStringLiteral("requestId")).toString(), QStringLiteral("1001"));
    QCOMPARE(plugin.syncCallCount(), syncCallsAfterRegistration);
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QCOMPARE(settledSpy.count(), 1);
    QCOMPARE(settledSpy.at(0).at(1).toULongLong(), quint64(1001));
    QCOMPARE(settledSpy.at(0).at(2).toString(), QStringLiteral("success"));
    QVERIFY(manager.connectCamera(QStringLiteral("fake-device")));
    QCOMPARE(plugin.createCalls(), 1);
    QCOMPARE(plugin.syncCallCount(), syncCallsAfterRegistration); // Async cameras connect from cached CameraInfo.
    bool workerConnect = true;
    bool workerLease = true;
    std::thread cameraCaller([&]() {
        workerConnect = manager.connectCamera(QStringLiteral("fake-device"));
        workerLease = static_cast<bool>(manager.acquireCamera(QStringLiteral("fake-device")));
        manager.disconnectCamera(QStringLiteral("fake-device"));
    });
    cameraCaller.join();
    QVERIFY(!workerConnect);
    QVERIFY(!workerLease);
    QCOMPARE(manager.lastError(QStringLiteral("fake-device")), QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    QVERIFY(manager.isConnected(QStringLiteral("fake-device"))); // Disconnect was queued to the manager thread.
    QCoreApplication::processEvents();
    QVERIFY(!manager.isConnected(QStringLiteral("fake-device")));
    QCOMPARE(plugin.syncCallCount(), syncCallsAfterRegistration);

    QJsonObject malformedCamera{{QStringLiteral("pluginId"), plugin.pluginId()},
                                {QStringLiteral("deviceId"), QStringLiteral("fake-device")},
                                {QStringLiteral("name"), QStringLiteral("Fake Camera")},
                                {QStringLiteral("serialNumber"), QString()},
                                {QStringLiteral("ipAddress"), QString()},
                                {QStringLiteral("width"), 640},
                                {QStringLiteral("height"), 480},
                                {QStringLiteral("pixelFormat"), QStringLiteral("Mono8")},
                                {QStringLiteral("isConnected"), false},
                                {QStringLiteral("rawInfo"), QJsonArray{QStringLiteral("invalid")}}};
    const QByteArray invalidPayload =
        QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                  {QStringLiteral("requestId"), QStringLiteral("1002")},
                                  {QStringLiteral("status"), QStringLiteral("success")},
                                  {QStringLiteral("cameras"), QJsonArray{malformedCamera}}})
            .toJson(QJsonDocument::Compact);
    plugin.setPayloadOverride(invalidPayload);
    QVERIFY(manager.startDiscoveryAsync(plugin.pluginId(), 1002, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 2, 1000);
    const QJsonObject invalidResult = qvariant_cast<QJsonObject>(finishedSpy.last().at(1));
    QCOMPARE(invalidResult.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_DISCOVERY_RESULT_INVALID_JSON"));
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QCOMPARE(settledSpy.count(), 2);
    QCOMPARE(settledSpy.at(1).at(2).toString(), QStringLiteral("error"));
    QCOMPARE(settledSpy.at(1).at(3).toString(), QStringLiteral("CAMERA_DISCOVERY_RESULT_INVALID_JSON"));

    plugin.setPayloadOverride({});
    plugin.setSignalIdDelta(1);
    QVERIFY(manager.startDiscoveryAsync(plugin.pluginId(), 1003, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 3, 1000);
    const QJsonObject mismatchedResult = qvariant_cast<QJsonObject>(finishedSpy.last().at(1));
    QCOMPARE(mismatchedResult.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_MISMATCH"));
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QCOMPARE(settledSpy.count(), 3);
    QCOMPARE(settledSpy.at(2).at(2).toString(), QStringLiteral("error"));
    QCOMPARE(settledSpy.at(2).at(3).toString(), QStringLiteral("CAMERA_DISCOVERY_REQUEST_ID_MISMATCH"));

    plugin.setSignalIdDelta(0);
    plugin.setDuplicateTerminal(true);
    QVERIFY(manager.startDiscoveryAsync(plugin.pluginId(), 1004, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 4, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QTest::qWait(30);
    QCOMPARE(finishedSpy.count(), 4); // Duplicate plugin terminal signal is ignored.
    QCOMPARE(settledSpy.count(), 4);  // Duplicate terminal signals settle only once.
    QCOMPARE(settledSpy.at(3).at(2).toString(), QStringLiteral("success"));

    plugin.setTerminalStatus(QStringLiteral("cancelled"));
    QVERIFY(manager.startDiscoveryAsync(plugin.pluginId(), 1005, &error));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 5, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QCOMPARE(settledSpy.count(), 5);
    QCOMPARE(settledSpy.at(4).at(2).toString(), QStringLiteral("cancelled"));
    QCOMPARE(settledSpy.at(4).at(3).toString(), QStringLiteral("CAMERA_DISCOVERY_CANCELLED"));
    plugin.setTerminalStatus(QStringLiteral("success"));

    QList<CameraStatus> workerDiscovery;
    std::thread discoverCaller([&]() { workerDiscovery = manager.discoverCameras(); });
    discoverCaller.join();
    QVERIFY(!workerDiscovery.isEmpty());
    QCOMPARE(plugin.asyncStarts(), 6); // Worker discovery only reads the current cache.
    std::thread refreshCaller([&]() { manager.refreshCameras(); });
    refreshCaller.join();
    QCOMPARE(plugin.asyncStarts(), 6); // Worker refresh is queued to the manager thread.
    QCoreApplication::processEvents();
    QCOMPARE(plugin.asyncStarts(), 7);
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QCOMPARE(settledSpy.count(), 6);
    QCOMPARE(settledSpy.at(5).at(2).toString(), QStringLiteral("success"));
    QCOMPARE(plugin.syncCallCount(), syncCallsAfterRegistration);
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testAsyncDiscoveryRequiresOptionalInterfaceAndOwnerThread() {
    CameraManager& manager = CameraManager::instance();
    CameraCounters counters;
    FakeCameraPlugin legacy(QStringLiteral("test-legacy-camera"), {}, &counters);
    manager.registerCameraPlugin(&legacy);
    m_registeredIds.append(legacy.pluginId());
    QString error;
    QVERIFY(!manager.startDiscoveryAsync(legacy.pluginId(), 3001, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));

    bool started = true;
    QString threadError;
    std::thread caller([&]() { started = manager.startDiscoveryAsync(legacy.pluginId(), 3002, &threadError); });
    caller.join();
    QVERIFY(!started);
    QCOMPARE(threadError, QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    QCOMPARE(manager.pendingDiscoveryCount(), 0);

    FakeAsyncCameraPlugin offThread(QStringLiteral("test-async-camera-wrong-thread"));
    manager.registerCameraPlugin(&offThread);
    m_registeredIds.append(offThread.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QThread pluginThread;
    pluginThread.start();
    offThread.moveToThread(&pluginThread);
    QVERIFY(!manager.startDiscoveryAsync(offThread.pluginId(), 3003, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_THREAD_AFFINITY"));
    QVERIFY(QMetaObject::invokeMethod(
        &offThread, [&offThread, &manager]() { offThread.moveToThread(manager.thread()); },
        Qt::BlockingQueuedConnection));
    pluginThread.quit();
    QVERIFY(pluginThread.wait(1000));
    QVERIFY(!manager.hasPendingDiscovery(3003));
}

void TestCameraManager::testAsyncDuplicateOwnerIsCompletionOrderIndependent() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin alphaSlow(QStringLiteral("test-camera-owner-alpha-slow"));
    FakeAsyncCameraPlugin zetaFast(QStringLiteral("test-camera-owner-zeta-fast"));
    alphaSlow.setDeviceId(QStringLiteral("camera-order-one"));
    zetaFast.setDeviceId(QStringLiteral("camera-order-one"));
    alphaSlow.setDelayMs(100);
    zetaFast.setDelayMs(10);
    manager.registerCameraPlugin(&alphaSlow);
    manager.registerCameraPlugin(&zetaFast);
    m_registeredIds << alphaSlow.pluginId() << zetaFast.pluginId();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QVERIFY(manager.connectCamera(QStringLiteral("camera-order-one")));
    QCOMPARE(manager.getCamera(QStringLiteral("camera-order-one"))->name(), alphaSlow.pluginId());
    manager.disconnectCamera(QStringLiteral("camera-order-one"));
    QVERIFY(manager.tryUnregisterCameraPlugin(alphaSlow.pluginId()));
    QVERIFY(manager.tryUnregisterCameraPlugin(zetaFast.pluginId()));
    m_registeredIds.removeAll(alphaSlow.pluginId());
    m_registeredIds.removeAll(zetaFast.pluginId());

    FakeAsyncCameraPlugin alphaFast(QStringLiteral("test-camera-owner-alpha-fast"));
    FakeAsyncCameraPlugin zetaSlow(QStringLiteral("test-camera-owner-zeta-slow"));
    alphaFast.setDeviceId(QStringLiteral("camera-order-two"));
    zetaSlow.setDeviceId(QStringLiteral("camera-order-two"));
    alphaFast.setDelayMs(10);
    zetaSlow.setDelayMs(100);
    manager.registerCameraPlugin(&alphaFast);
    manager.registerCameraPlugin(&zetaSlow);
    m_registeredIds << alphaFast.pluginId() << zetaSlow.pluginId();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QVERIFY(manager.connectCamera(QStringLiteral("camera-order-two")));
    QCOMPARE(manager.getCamera(QStringLiteral("camera-order-two"))->name(), alphaFast.pluginId());
    manager.disconnectCamera(QStringLiteral("camera-order-two"));
    QVERIFY(manager.tryUnregisterCameraPlugin(alphaFast.pluginId()));
    QVERIFY(manager.tryUnregisterCameraPlugin(zetaSlow.pluginId()));
    m_registeredIds.removeAll(alphaFast.pluginId());
    m_registeredIds.removeAll(zetaSlow.pluginId());
}

void TestCameraManager::testAsyncMissingDeviceIsRetainedUntilLeaseReleaseAndRefresh() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera-active-missing"));
    plugin.setDeviceId(QStringLiteral("active-missing-device"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QVERIFY(manager.connectCamera(QStringLiteral("active-missing-device")));
    CameraLease lease = manager.acquireCamera(QStringLiteral("active-missing-device"));
    QVERIFY(lease);
    manager.disconnectCamera(QStringLiteral("active-missing-device"));

    plugin.setReportsCamera(false);
    manager.refreshCameras();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QVERIFY(manager.getCamera(QStringLiteral("active-missing-device")) != nullptr);
    QVERIFY(manager.isConnected(QStringLiteral("active-missing-device")));
    QVERIFY(!manager.knownCameras().isEmpty());

    lease = CameraLease{};
    QVERIFY(manager.getCamera(QStringLiteral("active-missing-device")) == nullptr);
    manager.refreshCameras();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const auto cameras = manager.knownCameras();
    QVERIFY(std::find_if(cameras.cbegin(), cameras.cend(), [](const CameraStatus& status) {
                return status.deviceId == QStringLiteral("active-missing-device");
            }) == cameras.cend());
}

void TestCameraManager::testCameraSetViewShowsAsyncDiscoveryAndRefreshResults() {
    UserManager::instance().login(QStringLiteral("developer"), QString());
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin first(QStringLiteral("test-async-camera-set-view-first"));
    FakeAsyncCameraPlugin last(QStringLiteral("test-async-camera-set-view-last"));
    first.setDeviceId(QStringLiteral("camera-set-view-device-a"));
    last.setDeviceId(QStringLiteral("camera-set-view-device-z"));
    first.setCameraName(QStringLiteral("First Before Refresh"));
    last.setCameraName(QStringLiteral("Last Before Refresh"));
    first.setDelayMs(80);
    last.setDelayMs(250);
    QSignalSpy settledSpy(&manager, &CameraManager::cameraDiscoverySettled);
    manager.registerCameraPlugin(&first);
    manager.registerCameraPlugin(&last);
    m_registeredIds << first.pluginId() << last.pluginId();

    CameraSetView view;
    view.show();
    if (!PluginManager::instance().isInitialized())
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));
    auto* status = view.findChild<QLabel*>(QStringLiteral("cameraDiscoveryStatus"));
    auto* table = view.findChild<QTableWidget*>();
    QVERIFY(status);
    QVERIFY(table);
    QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(status->isVisible(), 1000);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(manager.pendingDiscoveryCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 1000);
    QCOMPARE(table->item(0, 0)->text(), QStringLiteral("First Before Refresh"));
    QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 2, 1000);
    QCOMPARE(table->item(1, 0)->text(), QStringLiteral("Last Before Refresh"));
    QTRY_VERIFY_WITH_TIMEOUT(!status->isVisible(), 1000);

    first.setCameraName(QStringLiteral("First After Refresh"));
    last.setCameraName(QStringLiteral("Last After Refresh"));
    auto* refresh = view.findChild<QPushButton*>(QStringLiteral("cameraRefreshButton"));
    QVERIFY(refresh);
    QTest::mouseClick(refresh, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 3, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(status->isVisible(), 1000);
    QCOMPARE(manager.pendingDiscoveryCount(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 0)->text(), QStringLiteral("First After Refresh"), 1000);
    QCOMPARE(status->isVisible(), true);
    QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 4, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(1, 0)->text(), QStringLiteral("Last After Refresh"), 1000);
    QTRY_VERIFY_WITH_TIMEOUT(!status->isVisible(), 1000);

    table->setCurrentCell(0, 0);
    first.cameraCounters().isConnectedQueries = 0;
    first.cameraCounters().isAcquiringQueries = 0;
    QVERIFY(manager.connectCamera(QStringLiteral("camera-set-view-device-a")));
    first.cameraCounters().isConnectedQueries = 0;
    first.cameraCounters().isAcquiringQueries = 0;
    table->setCurrentCell(-1, 0);
    table->setCurrentCell(0, 0);
    QCOMPARE(first.cameraCounters().isConnectedQueries.load(), 0);
    QCOMPARE(first.cameraCounters().isAcquiringQueries.load(), 0);
    CameraLease lease = manager.acquireCamera(QStringLiteral("camera-set-view-device-a"));
    QVERIFY(lease);
    manager.disconnectCamera(QStringLiteral("camera-set-view-device-a"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("Connected"));
    first.cameraCounters().isConnectedQueries = 0;
    first.cameraCounters().isAcquiringQueries = 0;
    lease = CameraLease{};
    first.cameraCounters().isConnectedQueries = 0;
    first.cameraCounters().isAcquiringQueries = 0;
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Disconnected"), 1000);
    QCOMPARE(first.cameraCounters().isConnectedQueries.load(), 0);
    QCOMPARE(first.cameraCounters().isAcquiringQueries.load(), 0);
    view.close();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
}

void TestCameraManager::testCameraSetViewDeleteAndApplyRemainGuiOnly() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-camera-set-view-gui-only-config"));
    plugin.setDeviceId(QStringLiteral("camera-set-view-gui-only-device"));
    plugin.setCameraName(QStringLiteral("GUI-only camera"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());

    CameraSetView view;
    view.show();
    if (!PluginManager::instance().isInitialized())
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));

    auto* cameraTable = view.findChild<QTableWidget*>();
    auto* applyButton = view.findChild<QPushButton*>(QStringLiteral("cameraApplyButton"));
    auto* ipEdit = view.findChild<QLineEdit*>(QStringLiteral("cameraIpEdit"));
    auto* portSpin = view.findChild<QSpinBox*>(QStringLiteral("cameraPortSpin"));
    QVERIFY(cameraTable);
    QVERIFY(applyButton);
    QVERIFY(ipEdit);
    QVERIFY(portSpin);
    QTRY_COMPARE_WITH_TIMEOUT(cameraTable->rowCount(), 1, 1000);
    QCOMPARE(manager.knownCameras().size(), 1);
    QCOMPARE(manager.knownCameras().first().deviceId, QStringLiteral("camera-set-view-gui-only-device"));
    const CameraStatus expectedStatus = manager.knownCameras().first();
    const auto verifyCameraStatePreserved = [&]() {
        const QList<CameraStatus> cameras = manager.knownCameras();
        QCOMPARE(cameras.size(), 1);
        const CameraStatus& actual = cameras.first();
        QCOMPARE(actual.deviceId, expectedStatus.deviceId);
        QCOMPARE(actual.name, expectedStatus.name);
        QCOMPARE(actual.state, expectedStatus.state);
        QCOMPARE(actual.lastGrabTime, expectedStatus.lastGrabTime);
        QCOMPARE(actual.lastError, expectedStatus.lastError);
        QVERIFY(manager.findPlugin(plugin.pluginId()) == &plugin);
    };

    QPushButton* deleteButton = nullptr;
    for (QPushButton* candidate : view.findChildren<QPushButton*>()) {
        if (candidate->text() == view.tr("Delete")) {
            deleteButton = candidate;
            break;
        }
    }
    QVERIFY(deleteButton);
    const int discoveryStarts = plugin.asyncStarts();
    cameraTable->setCurrentCell(0, 0);
    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QCOMPARE(cameraTable->rowCount(), 0);
    verifyCameraStatePreserved();
    QCOMPARE(plugin.asyncStarts(), discoveryStarts);

    ipEdit->setText(QStringLiteral("192.0.2.25"));
    portSpin->setValue(8123);
    QSignalSpy acceptedSpy(&view, &QDialog::accepted);
    QTest::mouseClick(applyButton, Qt::LeftButton);
    QCOMPARE(acceptedSpy.count(), 1);
    verifyCameraStatePreserved();
    QCOMPARE(plugin.asyncStarts(), discoveryStarts);
    QCOMPARE(plugin.cameraCounters().connectCalls, 0);
    QCOMPARE(plugin.cameraCounters().disconnectCalls, 0);
    QCOMPARE(plugin.cameraCounters().parameterSetCalls, 0);
    QCOMPARE(plugin.cameraCounters().startCalls, 0);
    QCOMPARE(plugin.cameraCounters().stopCalls, 0);
    QCOMPARE(plugin.cameraCounters().triggerCalls, 0);
}

void TestCameraManager::testCameraSetViewRestoresAgentPendingCameraOperations() {
    UserManager::instance().login(QStringLiteral("developer"), QString());
    ToolSchema::instance().registerDefaultTools();
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera-view-agent-pending"));
    plugin.setDeviceId(QStringLiteral("camera-view-agent-device"));
    plugin.setUseInfoDeviceId(true);
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 1, 1000);

    CameraCounters& counters = plugin.cameraCounters();
    QSemaphore connectGate;
    counters.connectGate = &connectGate;
    AgentActor actor;
    const QJsonObject connectResult =
        actor.executeTool(QStringLiteral("connect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("camera-view-agent-device")}});
    QCOMPARE(connectResult.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(counters.connectEntered.load(), 1, 1000);

    CameraSetView view;
    view.show();
    if (!PluginManager::instance().isInitialized())
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));
    auto* table = view.findChild<QTableWidget*>();
    auto* connectButton = view.findChild<QPushButton*>(QStringLiteral("cameraConnectButton"));
    auto* disconnectButton = view.findChild<QPushButton*>(QStringLiteral("cameraDisconnectButton"));
    QVERIFY(table);
    QVERIFY(connectButton);
    QVERIFY(disconnectButton);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 1000);
    table->setCurrentCell(0, 0);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Connecting…"), 1000);
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());
    connectGate.release();
    counters.connectGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Connected"), 1000);

    QSemaphore disconnectGate;
    counters.disconnectGate = &disconnectGate;
    const QJsonObject disconnectResult =
        actor.executeTool(QStringLiteral("disconnect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("camera-view-agent-device")}});
    QCOMPARE(disconnectResult.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(counters.disconnectEntered.load(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Disconnecting…"), 1000);
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());
    disconnectGate.release();
    counters.disconnectGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Disconnected"), 1000);
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testCameraSetViewStartsAsyncOperationsAndRecoversErrors() {
    UserManager::instance().login(QStringLiteral("developer"), QString());
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera-view-clicks"));
    plugin.setDeviceId(QStringLiteral("camera-view-click-device"));
    plugin.setUseInfoDeviceId(true);
    plugin.cameraCounters().enforceWorkerCalls = true;
    plugin.cameraCounters().connectSucceeds = false;
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 1, 1000);

    CameraSetView view;
    view.show();
    if (!PluginManager::instance().isInitialized())
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));
    auto* table = view.findChild<QTableWidget*>();
    auto* connectButton = view.findChild<QPushButton*>(QStringLiteral("cameraConnectButton"));
    auto* disconnectButton = view.findChild<QPushButton*>(QStringLiteral("cameraDisconnectButton"));
    auto* refreshButton = view.findChild<QPushButton*>(QStringLiteral("cameraRefreshButton"));
    QVERIFY(table);
    QVERIFY(connectButton);
    QVERIFY(disconnectButton);
    QVERIFY(refreshButton);
    QTRY_COMPARE_WITH_TIMEOUT(table->rowCount(), 1, 1000);
    table->setCurrentCell(0, 0);

    QSemaphore connectGate;
    plugin.cameraCounters().connectGate = &connectGate;
    QTest::mouseClick(connectButton, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(plugin.cameraCounters().connectEntered.load(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Connecting…"), 1000);
    bool eventLoopResponded = false;
    QTimer::singleShot(0, &manager, [&eventLoopResponded]() { eventLoopResponded = true; });
    QTRY_VERIFY_WITH_TIMEOUT(eventLoopResponded, 1000);
    connectGate.release();
    plugin.cameraCounters().connectGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Error"), 1000);
    QCOMPARE(table->item(0, 3)->toolTip(), QStringLiteral("CAMERA_CONNECT_NOT_CONFIRMED"));
    QVERIFY(connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());

    QTest::mouseClick(refreshButton, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(0, 3)->text(), QStringLiteral("Error"), 1000);
    QCOMPARE(table->item(0, 3)->toolTip(), QStringLiteral("CAMERA_CONNECT_NOT_CONFIRMED"));
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QCOMPARE(manager.knownCameras().size(), 1);
    view.close();

    CameraSetView reopened;
    reopened.show();
    if (!PluginManager::instance().isInitialized())
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));
    auto* reopenedTable = reopened.findChild<QTableWidget*>();
    auto* reopenedConnect = reopened.findChild<QPushButton*>(QStringLiteral("cameraConnectButton"));
    auto* reopenedDisconnect = reopened.findChild<QPushButton*>(QStringLiteral("cameraDisconnectButton"));
    QVERIFY(reopenedTable);
    QTRY_COMPARE_WITH_TIMEOUT(reopenedTable->rowCount(), 1, 1000);
    reopenedTable->setCurrentCell(0, 0);
    QCOMPARE(reopenedTable->item(0, 3)->text(), QStringLiteral("Error"));
    QCOMPARE(reopenedTable->item(0, 3)->toolTip(), QStringLiteral("CAMERA_CONNECT_NOT_CONFIRMED"));
    QVERIFY(reopenedConnect->isEnabled());
    QVERIFY(!reopenedDisconnect->isEnabled());

    plugin.cameraCounters().connectSucceeds = true;
    QTest::mouseClick(reopenedConnect, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(reopenedTable->item(0, 3)->text(), QStringLiteral("Connected"), 1000);
    QCOMPARE(reopenedTable->item(0, 3)->toolTip(), QString());

    plugin.cameraCounters().disconnectSucceeds = false;
    QTest::mouseClick(reopenedDisconnect, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(reopenedTable->item(0, 3)->text(), QStringLiteral("Error"), 1000);
    QCOMPARE(reopenedTable->item(0, 3)->toolTip(), QStringLiteral("CAMERA_DISCONNECT_NOT_CONFIRMED"));
    QVERIFY(!reopenedConnect->isEnabled());
    QVERIFY(reopenedDisconnect->isEnabled());

    plugin.cameraCounters().disconnectSucceeds = true;
    QSemaphore retryDisconnectGate;
    plugin.cameraCounters().disconnectGate = &retryDisconnectGate;
    QTest::mouseClick(reopenedDisconnect, Qt::LeftButton);
    QTRY_COMPARE_WITH_TIMEOUT(plugin.cameraCounters().disconnectEntered.load(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(reopenedTable->item(0, 3)->text(), QStringLiteral("Disconnecting…"), 1000);
    QCOMPARE(reopenedTable->item(0, 3)->toolTip(), QString());
    retryDisconnectGate.release();
    plugin.cameraCounters().disconnectGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(reopenedTable->item(0, 3)->text(), QStringLiteral("Disconnected"), 1000);
    QCOMPARE(reopenedTable->item(0, 3)->toolTip(), QString());
    QCOMPARE(manager.cameraSessionCount(), 0);
    QCOMPARE(plugin.cameraCounters().callsOnManagerThread.load(), 0);
    QCOMPARE(plugin.cameraCounters().wrongWorkerThreadCalls.load(), 0);
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testCameraSetViewRequiresPermissionsAndRejectsLateSignals() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-camera-view-permission"));
    plugin.setDeviceId(QStringLiteral("camera-view-permission-device"));
    plugin.setUseInfoDeviceId(true);
    plugin.setDelayMs(80);
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());

    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const int startsBeforeUnauthorizedViews = plugin.asyncStarts();
    const int pendingBefore = manager.pendingDiscoveryCount();
    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    const int knownBefore = manager.knownCameras().size();
    for (const QString& user :
         {QStringLiteral("administrator"), QStringLiteral("operator"), QStringLiteral("guest-session")}) {
        UserManager::instance().login(user, QString());
        CameraSetView deniedView;
        auto* deniedTable = deniedView.findChild<QTableWidget*>();
        auto* deniedRefresh = deniedView.findChild<QPushButton*>(QStringLiteral("cameraRefreshButton"));
        auto* deniedApply = deniedView.findChild<QPushButton*>(QStringLiteral("cameraApplyButton"));
        QVERIFY(deniedTable);
        QVERIFY(deniedRefresh);
        QVERIFY(deniedApply);
        QCOMPARE(deniedTable->rowCount(), 0);
        QVERIFY(!deniedTable->isEnabled());
        QVERIFY(!deniedRefresh->isEnabled());
        QVERIFY(!deniedApply->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(&deniedView, "onRefreshClicked", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(&deniedView, "onConnectClicked", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(&deniedView, "onDisconnectClicked", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(&deniedView, "onDeleteClicked", Qt::DirectConnection));
        QVERIFY(QMetaObject::invokeMethod(&deniedView, "onApplyClicked", Qt::DirectConnection));
        QCOMPARE(discoverySpy.count(), 0);
        QCOMPARE(plugin.asyncStarts(), startsBeforeUnauthorizedViews);
        QCOMPARE(manager.pendingDiscoveryCount(), pendingBefore);
        QCOMPARE(manager.knownCameras().size(), knownBefore);
    }
    UserManager::instance().logout();
    CameraSetView loggedOutView;
    QCOMPARE(loggedOutView.findChild<QTableWidget*>()->rowCount(), 0);
    QVERIFY(!loggedOutView.findChild<QPushButton*>(QStringLiteral("cameraRefreshButton"))->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&loggedOutView, "onRefreshClicked", Qt::DirectConnection));
    QCOMPARE(discoverySpy.count(), 0);
    QCOMPARE(plugin.asyncStarts(), startsBeforeUnauthorizedViews);
    QCOMPARE(manager.pendingDiscoveryCount(), pendingBefore);
    QCOMPARE(manager.knownCameras().size(), knownBefore);

    const bool pluginsAlreadyLoaded = PluginManager::instance().isInitialized();
    const int startsBeforeAuthorizedView = plugin.asyncStarts();
    UserManager::instance().login(QStringLiteral("developer"), QString());
    CameraSetView revokedView;
    auto* table = revokedView.findChild<QTableWidget*>();
    QVERIFY(table);
    UserManager::instance().login(QStringLiteral("operator"), QString());
    if (!pluginsAlreadyLoaded)
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));
    if (pluginsAlreadyLoaded)
        QTRY_VERIFY_WITH_TIMEOUT(discoverySpy.count() > 0, 1000);
    else
        QCoreApplication::processEvents();
    QCOMPARE(plugin.asyncStarts(), startsBeforeAuthorizedView + (pluginsAlreadyLoaded ? 1 : 0));
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(revokedView.findChild<QLineEdit*>(QStringLiteral("cameraIpEdit"))->text().isEmpty());
    QVERIFY(!revokedView.findChild<QPushButton*>(QStringLiteral("cameraRefreshButton"))->isEnabled());
    QVERIFY(revokedView.findChild<QLineEdit*>(QStringLiteral("cameraIpEdit"))->isEnabled() == false);
    QCOMPARE(revokedView.findChild<QSpinBox*>(QStringLiteral("cameraPortSpin"))->value(), 1);
    QCOMPARE(revokedView.findChild<QComboBox*>(QStringLiteral("cameraProtocolCombo"))->currentIndex(), -1);

    manager.cameraListChanged();
    QCOMPARE(table->rowCount(), 0);
    QCOMPARE(manager.knownCameras().size(), knownBefore + (pluginsAlreadyLoaded ? 1 : 0));

    const int settledBeforeRestore = discoverySpy.count();
    UserManager::instance().login(QStringLiteral("developer"), QString());
    QCOMPARE(discoverySpy.count(), settledBeforeRestore);
    QCOMPARE(plugin.asyncStarts(), startsBeforeAuthorizedView + (pluginsAlreadyLoaded ? 1 : 0));
    QCOMPARE(table->rowCount(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
}

void TestCameraManager::testAsyncCameraDetailsQueryIsQueuedAndLifecycleSafe() {
    UserManager::instance().login(QStringLiteral("developer"), QString());
    ToolSchema::instance().registerDefaultTools();
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera-details"));
    plugin.setDeviceId(QStringLiteral("async-camera-details-device"));
    plugin.setUseInfoDeviceId(true);
    CameraCounters& counters = plugin.cameraCounters();
    counters.enforceWorkerCalls = true;
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());

    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 1, 1000);
    QSignalSpy operationSpy(&manager, &CameraManager::cameraOperationSettled);
    QObject signalReceiver;
    bool detailsTerminalVisibleInsideSignal = false;
    connect(&manager, &CameraManager::cameraOperationSettled, &signalReceiver,
            [&manager, &detailsTerminalVisibleInsideSignal](const QString&, quint64 requestId, const QString& operation,
                                                            const QString& status, const QString&) {
                if (operation != QLatin1String("get_details") || status == QLatin1String("pending"))
                    return;
                QJsonObject result;
                detailsTerminalVisibleInsideSignal = manager.getCameraOperationResult(requestId, &result) &&
                                                     result.value(QStringLiteral("status")).toString() == status;
            });
    int cameraStateChanges = 0;
    connect(&manager, &CameraManager::cameraStateChanged, &signalReceiver,
            [&cameraStateChanges](const QString&, CameraState) { ++cameraStateChanges; });
    QSignalSpy connectedSpy(&manager, &CameraManager::cameraConnected);
    AgentActor actor;
    const QJsonObject connectResponse =
        actor.executeTool(QStringLiteral("connect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-camera-details-device")}});
    QCOMPARE(connectResponse.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    const QString connectId = connectResponse.value(QStringLiteral("requestId")).toString();
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
    QJsonObject connectResult;
    QVERIFY(manager.getCameraOperationResult(connectId.toULongLong(), &connectResult));
    QCOMPARE(connectResult.value(QStringLiteral("status")).toString(), QStringLiteral("success"));

    CameraSetView view;
    view.show();
    if (!PluginManager::instance().isInitialized())
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));
    auto* table = view.findChild<QTableWidget*>();
    auto* connectButton = view.findChild<QPushButton*>(QStringLiteral("cameraConnectButton"));
    auto* disconnectButton = view.findChild<QPushButton*>(QStringLiteral("cameraDisconnectButton"));
    QVERIFY(table);
    QVERIFY(connectButton);
    QVERIFY(disconnectButton);
    auto rowForDevice = [table]() {
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 1) && table->item(row, 1)->text() == QLatin1String("async-camera-details-device"))
                return row;
        return -1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(rowForDevice() >= 0, 1000);
    table->setCurrentCell(rowForDevice(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(table->item(rowForDevice(), 3)->text(), QStringLiteral("Connected"), 1000);

    const int stateChangesBeforeQuery = cameraStateChanges;
    const int connectedSignalsBeforeQuery = connectedSpy.count();
    const int gettersBeforeQuery = counters.detailsGetterCalls.load();
    QSemaphore detailsGate;
    counters.detailsGate = &detailsGate;
    const QJsonObject detailsResponse =
        actor.executeTool(QStringLiteral("get_camera_details"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-camera-details-device")}});
    QCOMPARE(detailsResponse.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    const QString detailsId = detailsResponse.value(QStringLiteral("requestId")).toString();
    QTRY_COMPARE_WITH_TIMEOUT(counters.detailsGateEntries.load(), 1, 1000);
    CameraOperationSnapshot pending;
    QVERIFY(manager.pendingCameraOperation(QStringLiteral("async-camera-details-device"), &pending));
    QCOMPARE(pending.operation, QStringLiteral("get_details"));
    QTRY_COMPARE_WITH_TIMEOUT(table->item(rowForDevice(), 3)->text(), QStringLiteral("Reading details…"), 1000);
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());

    bool managerEventLoopResponded = false;
    QTimer::singleShot(0, &manager, [&managerEventLoopResponded]() { managerEventLoopResponded = true; });
    QTRY_VERIFY_WITH_TIMEOUT(managerEventLoopResponded, 1000);
    const QJsonObject repeatedQuery =
        actor.executeTool(QStringLiteral("get_camera_details"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-camera-details-device")}});
    QCOMPARE(repeatedQuery.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_OPERATION_BUSY"));
    quint64 disconnectId = 0;
    QString operationError;
    QVERIFY(!manager.requestDisconnectCameraAsync(QStringLiteral("async-camera-details-device"), &disconnectId,
                                                  &operationError));
    QCOMPARE(operationError, QStringLiteral("CAMERA_OPERATION_BUSY"));

    detailsGate.release();
    counters.detailsGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 2, 1000);
    QJsonObject detailsResult;
    QVERIFY(manager.getCameraOperationResult(detailsId.toULongLong(), &detailsResult));
    QVERIFY(detailsTerminalVisibleInsideSignal);
    QCOMPARE(detailsResult.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QCOMPARE(detailsResult.value(QStringLiteral("operation")).toString(), QStringLiteral("get_details"));
    const QJsonObject details = detailsResult.value(QStringLiteral("details")).toObject();
    QCOMPARE(details.value(QStringLiteral("deviceId")).toString(), QStringLiteral("async-camera-details-device"));
    QCOMPARE(details.value(QStringLiteral("manufacturer")).toString(), QStringLiteral("test"));
    QCOMPARE(details.value(QStringLiteral("capabilities")).toObject().size(), 12);
    QCOMPARE(details.value(QStringLiteral("parameters")).toObject().size(), 7);
    QCOMPARE(cameraStateChanges, stateChangesBeforeQuery);
    QCOMPARE(connectedSpy.count(), connectedSignalsBeforeQuery);
    const auto cachedStatus = [&manager]() {
        const QList<CameraStatus> statuses = manager.knownCameras();
        const auto status = std::find_if(statuses.cbegin(), statuses.cend(), [](const CameraStatus& value) {
            return value.deviceId == QLatin1String("async-camera-details-device");
        });
        return status == statuses.cend() ? CameraStatus{} : *status;
    };
    QVERIFY(!cachedStatus().deviceId.isEmpty());
    QCOMPARE(cachedStatus().lastError, QString());
    QCOMPARE(counters.callsOnManagerThread.load(), 0);
    QCOMPARE(counters.wrongWorkerThreadCalls.load(), 0);
    QVERIFY(counters.detailsGetterCalls.load() > gettersBeforeQuery);
    QCOMPARE(table->item(rowForDevice(), 3)->text(), QStringLiteral("Connected"));

    counters.reportDisconnectedForDetails = true;
    const int detailGettersBeforeDisconnectedQuery = counters.detailsGetterCalls.load();
    const QJsonObject disconnectedResponse =
        actor.executeTool(QStringLiteral("get_camera_details"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-camera-details-device")}});
    QCOMPARE(disconnectedResponse.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 3, 1000);
    QJsonObject disconnectedResult;
    QVERIFY(manager.getCameraOperationResult(
        disconnectedResponse.value(QStringLiteral("requestId")).toString().toULongLong(), &disconnectedResult));
    QCOMPARE(disconnectedResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_NOT_CONNECTED"));
    QVERIFY(!disconnectedResult.contains(QStringLiteral("details")));
    QCOMPARE(counters.detailsGetterCalls.load(), detailGettersBeforeDisconnectedQuery);
    QCOMPARE(cachedStatus().state, CameraState::Connected);
    counters.reportDisconnectedForDetails = false;

    counters.detailsGetterThrows = true;
    const QJsonObject exceptionResponse =
        actor.executeTool(QStringLiteral("get_camera_details"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-camera-details-device")}});
    QCOMPARE(exceptionResponse.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 4, 1000);
    QJsonObject exceptionResult;
    QVERIFY(manager.getCameraOperationResult(
        exceptionResponse.value(QStringLiteral("requestId")).toString().toULongLong(), &exceptionResult));
    QCOMPARE(exceptionResult.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_DETAILS_READ_FAILED"));
    QVERIFY(!exceptionResult.contains(QStringLiteral("details")));
    counters.detailsGetterThrows = false;

    counters.detailsValuesNonFinite = true;
    const QJsonObject nonFiniteResponse =
        actor.executeTool(QStringLiteral("get_camera_details"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-camera-details-device")}});
    QCOMPARE(nonFiniteResponse.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 5, 1000);
    QJsonObject nonFiniteResult;
    QVERIFY(manager.getCameraOperationResult(
        nonFiniteResponse.value(QStringLiteral("requestId")).toString().toULongLong(), &nonFiniteResult));
    QCOMPARE(nonFiniteResult.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_DETAILS_INVALID_VALUE"));
    QVERIFY(!nonFiniteResult.contains(QStringLiteral("details")));
    counters.detailsValuesNonFinite = false;

    QSemaphore drainGate;
    counters.detailsGate = &drainGate;
    const QJsonObject drainResponse =
        actor.executeTool(QStringLiteral("get_camera_details"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-camera-details-device")}});
    QCOMPARE(drainResponse.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(counters.detailsGateEntries.load(), 2, 1000);
    manager.disconnectAll();
    QCOMPARE(counters.disconnectEntered.load(), 0);
    drainGate.release();
    counters.detailsGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(counters.disconnectEntered.load(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 1000);
    QCOMPARE(cachedStatus().state, CameraState::Disconnected);
    view.close();
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testAsyncAgentCameraParameterWriteContract() {
    ToolSchema::instance().registerDefaultTools();
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera-parameter"));
    plugin.setDeviceId(QStringLiteral("async-camera-parameter-device"));
    plugin.setUseInfoDeviceId(true);
    plugin.setParameterControlEnabled(true);
    CameraCounters& counters = plugin.cameraCounters();
    counters.enforceWorkerCalls = true;
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 1, 1000);
    QCOMPARE(discoverySpy.first().at(2).toString(), QStringLiteral("success"));

    AgentActor actor;
    const auto connectResponse =
        actor.executeTool(QStringLiteral("connect_camera"),
                          {{QStringLiteral("deviceId"), QStringLiteral("async-camera-parameter-device")}});
    QCOMPARE(connectResponse.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_VERIFY_WITH_TIMEOUT(manager.isConnected(QStringLiteral("async-camera-parameter-device")), 1000);

    auto queryDetails = [&actor]() {
        return actor.executeTool(QStringLiteral("get_camera_details"),
                                 {{QStringLiteral("deviceId"), QStringLiteral("async-camera-parameter-device")}});
    };
    const auto detailsStarted = queryDetails();
    QCOMPARE(detailsStarted.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QJsonObject detailsResult;
    QTRY_VERIFY_WITH_TIMEOUT(
        CameraManager::instance().getCameraOperationResult(
            detailsStarted.value(QStringLiteral("requestId")).toString().toULongLong(), &detailsResult) &&
            detailsResult.value(QStringLiteral("status")).toString() != QLatin1String("pending"),
        1000);
    const QJsonObject details = detailsResult.value(QStringLiteral("details")).toObject();
    QVERIFY(!details.value(QStringLiteral("cameraSessionGeneration")).toString().isEmpty());
    const QString serial = details.value(QStringLiteral("serialNumber")).toString();
    auto parameterWrite = [&details, &serial]() {
        return QJsonObject{
            {QStringLiteral("deviceId"), QStringLiteral("async-camera-parameter-device")},
            {QStringLiteral("cameraSessionGeneration"), details.value(QStringLiteral("cameraSessionGeneration"))},
            {QStringLiteral("serialNumber"), serial},
            {QStringLiteral("parameter"), QStringLiteral("exposureTime")},
            {QStringLiteral("value"), 1800.0}};
    };

    CameraSetView parameterView;
    parameterView.show();
    if (!PluginManager::instance().isInitialized())
        QVERIFY(QMetaObject::invokeMethod(&PluginManager::instance(), "allPluginsLoaded", Qt::DirectConnection));
    auto* cameraTable = parameterView.findChild<QTableWidget*>();
    auto* connectButton = parameterView.findChild<QPushButton*>(QStringLiteral("cameraConnectButton"));
    auto* disconnectButton = parameterView.findChild<QPushButton*>(QStringLiteral("cameraDisconnectButton"));
    QVERIFY(cameraTable);
    QVERIFY(connectButton);
    QVERIFY(disconnectButton);
    auto rowForParameterCamera = [cameraTable]() {
        for (int row = 0; row < cameraTable->rowCount(); ++row)
            if (cameraTable->item(row, 1) &&
                cameraTable->item(row, 1)->text() == QLatin1String("async-camera-parameter-device"))
                return row;
        return -1;
    };
    QTRY_VERIFY_WITH_TIMEOUT(rowForParameterCamera() >= 0, 1000);
    cameraTable->setCurrentCell(rowForParameterCamera(), 0);

    QSignalSpy operationSpy(&manager, &CameraManager::cameraOperationSettled);
    QObject signalReceiver;
    bool terminalVisibleInsideSignal = false;
    connect(&manager, &CameraManager::cameraOperationSettled, &signalReceiver,
            [&manager, &terminalVisibleInsideSignal](const QString&, quint64 id, const QString& operation,
                                                     const QString& status, const QString&) {
                if (operation != QLatin1String("set_parameter") || status == QLatin1String("pending"))
                    return;
                QJsonObject result;
                terminalVisibleInsideSignal = manager.getCameraOperationResult(id, &result) &&
                                              result.value(QStringLiteral("status")).toString() == status;
            });
    QSemaphore validationGate;
    counters.parameterValidationGate = &validationGate;
    const auto writeStarted = actor.executeTool(QStringLiteral("set_camera_parameter"), parameterWrite());
    QCOMPARE(writeStarted.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QCOMPARE(writeStarted.value(QStringLiteral("operation")).toString(), QStringLiteral("set_parameter"));
    const QString writeId = writeStarted.value(QStringLiteral("requestId")).toString();
    QTRY_COMPARE_WITH_TIMEOUT(counters.parameterValidationGateEntries.load(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(cameraTable->item(rowForParameterCamera(), 3)->text(),
                              QStringLiteral("Updating parameter…"), 1000);
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(!disconnectButton->isEnabled());
    QCOMPARE(actor
                 .executeTool(QStringLiteral("get_camera_details"),
                              {{QStringLiteral("deviceId"), QStringLiteral("async-camera-parameter-device")}})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_OPERATION_BUSY"));
    QCOMPARE(actor
                 .executeTool(QStringLiteral("disconnect_camera"),
                              {{QStringLiteral("deviceId"), QStringLiteral("async-camera-parameter-device")}})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("CAMERA_OPERATION_BUSY"));
    bool managerLoopResponded = false;
    QTimer::singleShot(0, &manager, [&managerLoopResponded]() { managerLoopResponded = true; });
    QTRY_VERIFY_WITH_TIMEOUT(managerLoopResponded, 1000);
    validationGate.release();
    counters.parameterValidationGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
    QJsonObject writeResult;
    QVERIFY(actor.executeTool(QStringLiteral("get_camera_operation"), {{QStringLiteral("requestId"), writeId}})
                .value(QStringLiteral("status"))
                .toString() == QLatin1String("success"));
    QVERIFY(manager.getCameraOperationResult(writeId.toULongLong(), &writeResult));
    QCOMPARE(writeResult.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QCOMPARE(writeResult.value(QStringLiteral("operation")).toString(), QStringLiteral("set_parameter"));
    QCOMPARE(writeResult.value(QStringLiteral("actual")).toDouble(), 1800.0);
    QCOMPARE(counters.parameterSetCalls, 1);
    QVERIFY(terminalVisibleInsideSignal);
    QCOMPARE(counters.callsOnManagerThread.load(), 0);
    QCOMPARE(counters.wrongWorkerThreadCalls.load(), 0);
    QCOMPARE(cameraTable->item(rowForParameterCamera(), 3)->text(), QStringLiteral("Connected"));

    const auto refreshedStarted = queryDetails();
    QCOMPARE(refreshedStarted.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_VERIFY_WITH_TIMEOUT(
        manager.getCameraOperationResult(refreshedStarted.value(QStringLiteral("requestId")).toString().toULongLong(),
                                         &detailsResult) &&
            detailsResult.value(QStringLiteral("status")).toString() == QLatin1String("success"),
        1000);
    const QJsonObject refreshedDetails = detailsResult.value(QStringLiteral("details")).toObject();
    QVERIFY(refreshedDetails.value(QStringLiteral("cameraSessionGeneration")).toString() !=
            details.value(QStringLiteral("cameraSessionGeneration")).toString());

    operationSpy.clear();
    QMetaObject::Connection invalidateOnStart = connect(
        &manager, &CameraManager::cameraOperationStarted, &manager,
        [&manager](const QString& deviceId, quint64, const QString& operation) {
            if (deviceId == QLatin1String("async-camera-parameter-device") &&
                operation == QLatin1String("set_parameter"))
                manager.invalidateCameraParameterBinding(deviceId);
        },
        Qt::DirectConnection);
    QJsonObject staleWhilePending = parameterWrite();
    staleWhilePending.insert(QStringLiteral("cameraSessionGeneration"),
                             refreshedDetails.value(QStringLiteral("cameraSessionGeneration")));
    staleWhilePending.insert(QStringLiteral("parameter"), QStringLiteral("gain"));
    staleWhilePending.insert(QStringLiteral("value"), 2.5);
    const auto staleWhilePendingStarted = actor.executeTool(QStringLiteral("set_camera_parameter"), staleWhilePending);
    QCOMPARE(staleWhilePendingStarted.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
    QObject::disconnect(invalidateOnStart);
    const quint64 staleWhilePendingId =
        staleWhilePendingStarted.value(QStringLiteral("requestId")).toString().toULongLong();
    QVERIFY(manager.getCameraOperationResult(staleWhilePendingId, &writeResult));
    QCOMPARE(writeResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_TARGET_CHANGED"));
    QCOMPARE(counters.parameterSetCalls, 1);

    const auto afterRaceDetailsStarted = queryDetails();
    QCOMPARE(afterRaceDetailsStarted.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_VERIFY_WITH_TIMEOUT(
        manager.getCameraOperationResult(
            afterRaceDetailsStarted.value(QStringLiteral("requestId")).toString().toULongLong(), &detailsResult) &&
            detailsResult.value(QStringLiteral("status")).toString() == QLatin1String("success"),
        1000);
    const QJsonObject afterRaceDetails = detailsResult.value(QStringLiteral("details")).toObject();

    operationSpy.clear();
    AgentController& controller = AgentController::instance();
    controller.clearConversation();
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    QJsonObject staleAfterDisconnect{
        {QStringLiteral("deviceId"), QStringLiteral("async-camera-parameter-device")},
        {QStringLiteral("cameraSessionGeneration"), afterRaceDetails.value(QStringLiteral("cameraSessionGeneration"))},
        {QStringLiteral("serialNumber"), serial},
        {QStringLiteral("parameter"), QStringLiteral("gain")},
        {QStringLiteral("value"), 2.0}};
    counters.reportDisconnectedForDetails = true;
    const auto confirmation = controller.handleToolCall(QStringLiteral("set_camera_parameter"), staleAfterDisconnect);
    QCOMPARE(confirmation.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
    const quint64 disconnectedWriteId = operationSpy.last().at(1).toULongLong();
    QVERIFY(manager.getCameraOperationResult(disconnectedWriteId, &writeResult));
    QCOMPARE(writeResult.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_NOT_CONNECTED"));
    QCOMPARE(counters.parameterSetCalls, 1);
    QCOMPARE(writeResult.value(QStringLiteral("cameraSessionGeneration")).toString(),
             afterRaceDetails.value(QStringLiteral("cameraSessionGeneration")).toString());
    counters.reportDisconnectedForDetails = false;

    parameterView.close();
    manager.disconnectAll();
    QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 1000);
    QString error;
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testManagedDiscoveryStartFailureSettlesOnce() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera-start-failure"));
    plugin.setStartError(QStringLiteral("CAMERA_DISCOVERY_START_FAILED"));
    QSignalSpy settledSpy(&manager, &CameraManager::cameraDiscoverySettled);
    QSignalSpy errorSpy(&manager, &CameraManager::cameraDiscoveryError);
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QCOMPARE(manager.pendingDiscoveryCount(), 0);
    QCOMPARE(settledSpy.count(), 1);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(settledSpy.first().at(2).toString(), QStringLiteral("start_failed"));
    QCOMPARE(settledSpy.first().at(3).toString(), QStringLiteral("CAMERA_DISCOVERY_START_FAILED"));
    QString error;
    QVERIFY(!manager.startDiscoveryAsync(plugin.pluginId(), 4001, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_DISCOVERY_START_FAILED"));
    QCOMPARE(settledSpy.count(), 2);
    QCOMPARE(errorSpy.count(), 2);
    QCOMPARE(settledSpy.last().at(1).toULongLong(), quint64(4001));
    QCOMPARE(settledSpy.last().at(2).toString(), QStringLiteral("start_failed"));
    QCOMPARE(settledSpy.last().at(3).toString(), QStringLiteral("CAMERA_DISCOVERY_START_FAILED"));
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testAgentDiscoveryTargetSnapshotDoesNotCallDrivers() {
    CameraManager& manager = CameraManager::instance();
    CameraCounters legacyCounters;
    FakeCameraPlugin legacy(QStringLiteral("test-agent-discovery-targets-legacy"), {}, &legacyCounters);
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-targets"));
    const QString legacyId = legacy.pluginId();
    const QString asyncId = plugin.pluginId();
    manager.registerCameraPlugin(&legacy);
    manager.registerCameraPlugin(&plugin);
    m_registeredIds << legacyId << asyncId;
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const int asyncStarts = plugin.asyncStarts();
    const int asyncOtherCalls = plugin.nonStartVirtualCallCount();
    const int asyncCancelCalls = plugin.cancelCalls();
    const int legacyCalls = legacy.virtualCallCount();
    const int legacyDiscoveryCalls = legacy.discoverCalls();
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    QCOMPARE(targets.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QVERIFY(targets.value(QStringLiteral("registryGeneration")).isString());
    QCOMPARE(targets.value(QStringLiteral("asyncPluginIds")).toArray(), QJsonArray{QJsonValue(asyncId)});
    QCOMPARE(targets.value(QStringLiteral("unsupportedPluginIds")).toArray(), QJsonArray{QJsonValue(legacyId)});
    QCOMPARE(targets.value(QStringLiteral("discoveryPerformed")).toBool(), false);
    QCOMPARE(plugin.asyncStarts(), asyncStarts);
    QCOMPARE(plugin.nonStartVirtualCallCount(), asyncOtherCalls);
    QCOMPARE(plugin.cancelCalls(), asyncCancelCalls);
    QCOMPARE(legacy.virtualCallCount(), legacyCalls);
    QCOMPARE(legacy.discoverCalls(), legacyDiscoveryCalls);

    quint64 requestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), {asyncId}, {legacyId},
                 &requestId, &error),
             qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1500);
    QCOMPARE(plugin.asyncStarts(), asyncStarts + 1);
    QCOMPARE(plugin.nonStartVirtualCallCount(), asyncOtherCalls);
    QCOMPARE(plugin.cancelCalls(), asyncCancelCalls);
    QCOMPARE(legacy.virtualCallCount(), legacyCalls);
    QCOMPARE(legacy.discoverCalls(), legacyDiscoveryCalls);
}

void TestCameraManager::testAgentDiscoveryBatchAggregatesAndCachesBeforeSettled() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin successPlugin(QStringLiteral("test-agent-discovery-batch-a"));
    FakeAsyncCameraPlugin failedPlugin(QStringLiteral("test-agent-discovery-batch-b"));
    failedPlugin.setTerminalStatus(QStringLiteral("error"), QStringLiteral("FAKE_DISCOVERY_FAILURE"));
    successPlugin.setDuplicateTerminal(true);
    manager.registerCameraPlugin(&successPlugin);
    manager.registerCameraPlugin(&failedPlugin);
    m_registeredIds << successPlugin.pluginId() << failedPlugin.pluginId();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    successPlugin.setDelayMs(20);
    failedPlugin.setDelayMs(300);

    const QJsonObject targets = manager.cameraDiscoveryTargets();
    QStringList pluginIds;
    for (const QJsonValue& value : targets.value(QStringLiteral("asyncPluginIds")).toArray())
        pluginIds.append(value.toString());
    QStringList unsupportedPluginIds;
    for (const QJsonValue& value : targets.value(QStringLiteral("unsupportedPluginIds")).toArray())
        unsupportedPluginIds.append(value.toString());
    QSignalSpy settledSpy(&manager, &CameraManager::cameraDiscoveryBatchSettled);
    bool queryWorkedInSettled = false;
    QJsonObject resultInSettled;
    const auto settledConnection = connect(
        &manager, &CameraManager::cameraDiscoveryBatchSettled, &manager,
        [&](quint64 requestId, const QString&, const QString&) {
            queryWorkedInSettled = manager.getCameraDiscoveryOperationResult(requestId, &resultInSettled);
        },
        Qt::DirectConnection);
    quint64 requestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), pluginIds,
                 unsupportedPluginIds, &requestId, &error),
             qPrintable(error));
    QVERIFY(requestId != 0);
    QJsonObject operation;
    QVERIFY(manager.getCameraDiscoveryOperationResult(requestId, &operation));
    QCOMPARE(operation.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTest::qWait(80); // the first child completes, but its job lease has not settled the aggregate yet
    QVERIFY(manager.getCameraDiscoveryOperationResult(requestId, &operation));
    QCOMPARE(operation.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    for (const QJsonValue& value : operation.value(QStringLiteral("pluginResults")).toArray())
        QCOMPARE(value.toObject().value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 1500);
    QVERIFY(queryWorkedInSettled);
    QCOMPARE(resultInSettled.value(QStringLiteral("status")).toString(), QStringLiteral("partial"));
    QCOMPARE(resultInSettled.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_PARTIAL"));
    QCOMPARE(resultInSettled.value(QStringLiteral("requestId")).toString(), QString::number(requestId));
    QCOMPARE(resultInSettled.value(QStringLiteral("pluginResults")).toArray().size(), 2);
    const QJsonArray settledPluginResults = resultInSettled.value(QStringLiteral("pluginResults")).toArray();
    QCOMPARE(settledPluginResults.at(0).toObject().value(QStringLiteral("status")).toString(),
             QStringLiteral("success"));
    QVERIFY(!settledPluginResults.at(0).toObject().contains(QStringLiteral("childRequestId")));
    QVERIFY(!settledPluginResults.at(0).toObject().contains(QStringLiteral("errorCode")));
    QVERIFY(!settledPluginResults.at(1).toObject().contains(QStringLiteral("childRequestId")));
    QCOMPARE(settledPluginResults.at(1).toObject().value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("FAKE_DISCOVERY_FAILURE"));
    QCOMPARE(resultInSettled.value(QStringLiteral("cameras")).toArray().size(), 1);
    const QJsonArray successCameras = settledPluginResults.at(0).toObject().value(QStringLiteral("cameras")).toArray();
    QCOMPARE(successCameras.size(), 1);
    QCOMPARE(successCameras.first().toObject().value(QStringLiteral("pluginId")).toString(), successPlugin.pluginId());
    bool cachedCameraFound = false;
    for (const CameraStatus& camera : manager.knownCameras())
        cachedCameraFound = cachedCameraFound || camera.deviceId == QStringLiteral("fake-device");
    QVERIFY(cachedCameraFound);
    QCOMPARE(resultInSettled.value(QStringLiteral("unsupportedPluginIds")).toArray().size(),
             unsupportedPluginIds.size());
    QVERIFY(manager.getCameraDiscoveryOperationResult(requestId, &operation));
    QCOMPARE(operation, resultInSettled);
    QTest::qWait(30);
    QCOMPARE(settledSpy.count(), 1);
    QVERIFY(manager.getCameraDiscoveryOperationResult(requestId, &operation));
    QCOMPARE(operation, resultInSettled);
    disconnect(settledConnection);
}

void TestCameraManager::testAgentDiscoveryBatchNullStartFailureIsQueryable() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-null-start"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    plugin.setStartError(QStringLiteral("CAMERA_DISCOVERY_START_FAILED"));
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    QSignalSpy settledSpy(&manager, &CameraManager::cameraDiscoveryBatchSettled);
    quint64 requestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), {plugin.pluginId()}, {},
                 &requestId, &error),
             qPrintable(error));
    QVERIFY(requestId != 0);
    QCOMPARE(settledSpy.count(), 1);
    QJsonObject result;
    QVERIFY(manager.getCameraDiscoveryOperationResult(requestId, &result));
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_FAILED"));
    const QJsonObject child = result.value(QStringLiteral("pluginResults")).toArray().first().toObject();
    QCOMPARE(child.value(QStringLiteral("status")).toString(), QStringLiteral("start_failed"));
    QCOMPARE(child.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_START_FAILED"));
    QVERIFY(child.value(QStringLiteral("cameras")).toArray().isEmpty());
}

void TestCameraManager::testAgentDiscoveryBatchWaitsForAllSynchronousStartFailures() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin first(QStringLiteral("test-agent-discovery-start-failure-a"));
    FakeAsyncCameraPlugin second(QStringLiteral("test-agent-discovery-start-failure-b"));
    first.setStartError(QStringLiteral("FIRST_START_FAILED"));
    second.setStartError(QStringLiteral("SECOND_START_FAILED"));
    manager.registerCameraPlugin(&first);
    manager.registerCameraPlugin(&second);
    m_registeredIds << first.pluginId() << second.pluginId();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);

    QSignalSpy settledSpy(&manager, &CameraManager::cameraDiscoveryBatchSettled);
    int startCallbacks = 0;
    bool settledBeforeAllStarts = false;
    const auto observeStart = [&settledSpy, &startCallbacks, &settledBeforeAllStarts](quint64) {
        ++startCallbacks;
        settledBeforeAllStarts = settledBeforeAllStarts || settledSpy.count() != 0;
    };
    first.setStartCallback(observeStart);
    second.setStartCallback(observeStart);
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    quint64 requestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(),
                 {first.pluginId(), second.pluginId()}, {}, &requestId, &error),
             qPrintable(error));

    QCOMPARE(startCallbacks, 2);
    QVERIFY(!settledBeforeAllStarts);
    QCOMPARE(settledSpy.count(), 1);
    QJsonObject result;
    QVERIFY(manager.getCameraDiscoveryOperationResult(requestId, &result));
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    const QJsonArray pluginResults = result.value(QStringLiteral("pluginResults")).toArray();
    QCOMPARE(pluginResults.size(), 2);
    QCOMPARE(pluginResults.at(0).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("start_failed"));
    QCOMPARE(pluginResults.at(1).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("start_failed"));
}

void TestCameraManager::testAgentDiscoveryBatchCancelledChildIsQueryable() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-cancelled"));
    plugin.setTerminalStatus(QStringLiteral("cancelled"), QStringLiteral("CAMERA_DISCOVERY_CANCELLED"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    quint64 requestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), {plugin.pluginId()}, {},
                 &requestId, &error),
             qPrintable(error));
    QJsonObject result;
    QTRY_VERIFY_WITH_TIMEOUT(manager.getCameraDiscoveryOperationResult(requestId, &result) &&
                                 result.value(QStringLiteral("status")).toString() != QLatin1String("pending"),
                             1000);
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_FAILED"));
    const QJsonObject child = result.value(QStringLiteral("pluginResults")).toArray().first().toObject();
    QCOMPARE(child.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(child.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_CANCELLED"));
}

void TestCameraManager::testAgentDiscoveryBatchRejectsWhenAnyTargetIsBusy() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin busyPlugin(QStringLiteral("test-agent-discovery-busy-a"));
    FakeAsyncCameraPlugin untouchedPlugin(QStringLiteral("test-agent-discovery-busy-b"));
    manager.registerCameraPlugin(&busyPlugin);
    manager.registerCameraPlugin(&untouchedPlugin);
    m_registeredIds << busyPlugin.pluginId() << untouchedPlugin.pluginId();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    busyPlugin.setDelayMs(300);
    untouchedPlugin.setDelayMs(300);
    QString error;
    QVERIFY(manager.startDiscoveryAsync(busyPlugin.pluginId(), 908070, &error));
    const int untouchedStarts = untouchedPlugin.asyncStarts();
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    QStringList pluginIds;
    for (const QJsonValue& value : targets.value(QStringLiteral("asyncPluginIds")).toArray())
        pluginIds.append(value.toString());
    QStringList unsupportedPluginIds;
    for (const QJsonValue& value : targets.value(QStringLiteral("unsupportedPluginIds")).toArray())
        unsupportedPluginIds.append(value.toString());
    quint64 requestId = 0;
    QVERIFY(!manager.requestCameraDiscoveryBatchAsync(
        targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), pluginIds, unsupportedPluginIds,
        &requestId, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_DISCOVERY_BUSY"));
    QCOMPARE(requestId, quint64(0));
    QCOMPARE(untouchedPlugin.asyncStarts(), untouchedStarts); // no earlier target starts before all admission checks
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1500);
}

void TestCameraManager::testAgentDiscoveryBatchRejectsWrongThreadAndBusyEngine() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-gates"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    const quint64 generation = targets.value(QStringLiteral("registryGeneration")).toString().toULongLong();
    const QStringList pluginIds{plugin.pluginId()};
    const int startsBeforeWrongThreadCall = plugin.asyncStarts();
    quint64 wrongThreadRequestId = 0;
    QString wrongThreadError;
    std::thread worker([&manager, generation, pluginIds, &wrongThreadRequestId, &wrongThreadError]() {
        manager.requestCameraDiscoveryBatchAsync(generation, pluginIds, {}, &wrongThreadRequestId, &wrongThreadError);
    });
    worker.join();
    QCOMPARE(wrongThreadError, QStringLiteral("CAMERA_MANAGER_THREAD_AFFINITY"));
    QCOMPARE(wrongThreadRequestId, quint64(0));
    QCOMPARE(plugin.asyncStarts(), startsBeforeWrongThreadCall);

    Project* busyProject = ProjectManager::instance().newProject();
    QVERIFY(busyProject);
    ModuleInstance blockingInstance;
    blockingInstance.id = QStringLiteral("blocking_agent_discovery_gate");
    blockingInstance.moduleId = QStringLiteral("com.deeplux.test.blockingcameragate");
    busyProject->addModule(blockingInstance);
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(busyProject, [](const ModuleInstance&) { return new BlockingCameraGateModule(); }));
    QVERIFY(engine.runOnceAsync());
    QTRY_VERIFY_WITH_TIMEOUT(engine.isBusy(), 1000);
    const int startsBefore = plugin.asyncStarts();
    quint64 busyRequestId = 0;
    QString busyError;
    QVERIFY(!manager.requestCameraDiscoveryBatchAsync(generation, pluginIds, {}, &busyRequestId, &busyError));
    QCOMPARE(busyError, QStringLiteral("ENGINE_BUSY"));
    QCOMPARE(busyRequestId, quint64(0));
    QCOMPARE(plugin.asyncStarts(), startsBefore);
    engine.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 2000);
    ProjectManager::instance().closeProject();
}

void TestCameraManager::testAgentDiscoveryRejectsLegacyOnlyTargets() {
    CameraManager& manager = CameraManager::instance();
    CameraCounters counters;
    FakeCameraPlugin legacy(QStringLiteral("test-agent-discovery-legacy-only"), {}, &counters);
    manager.registerCameraPlugin(&legacy);
    FakeAsyncCameraPlugin async(QStringLiteral("test-agent-discovery-legacy-with-async"));
    manager.registerCameraPlugin(&async);
    m_registeredIds << legacy.pluginId() << async.pluginId();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    QVERIFY(!targets.value(QStringLiteral("asyncPluginIds")).toArray().isEmpty());
    QCOMPARE(targets.value(QStringLiteral("unsupportedPluginIds")).toArray(),
             QJsonArray{QJsonValue(legacy.pluginId())});

    QString error;
    quint64 requestId = 0;
    QVERIFY(!manager.requestCameraDiscoveryBatchAsync(
        targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), {legacy.pluginId()},
        {legacy.pluginId()}, &requestId, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
    QCOMPARE(requestId, quint64(0));

    QVERIFY(!manager.requestCameraDiscoveryBatchAsync(
        targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), {}, {legacy.pluginId()},
        &requestId, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
    QCOMPARE(requestId, quint64(0));

    AgentController& controller = AgentController::instance();
    controller.clearConversation();
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    const QJsonObject agentResult = controller.handleToolCall(
        QStringLiteral("discover_cameras"),
        {{QStringLiteral("registryGeneration"), targets.value(QStringLiteral("registryGeneration"))},
         {QStringLiteral("pluginIds"), QJsonArray{legacy.pluginId()}},
         {QStringLiteral("unsupportedPluginIds"), QJsonArray{legacy.pluginId()}}});
    QCOMPARE(agentResult.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
    QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    QVERIFY(manager.tryUnregisterCameraPlugin(legacy.pluginId()));
    m_registeredIds.removeAll(legacy.pluginId());
    QVERIFY(manager.tryUnregisterCameraPlugin(async.pluginId()));
    m_registeredIds.removeAll(async.pluginId());
}

void TestCameraManager::testAgentDiscoveryObserverRejectsWithoutStartingPlugins() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-observer"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const int startsBefore = plugin.asyncStarts();
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    AgentController& controller = AgentController::instance();
    controller.clearConversation();
    controller.setPermissionLevel(AgentController::PermissionLevel::Observer);
    const QJsonObject denied = controller.handleToolCall(
        QStringLiteral("discover_cameras"),
        {{QStringLiteral("registryGeneration"), targets.value(QStringLiteral("registryGeneration"))},
         {QStringLiteral("pluginIds"), targets.value(QStringLiteral("asyncPluginIds"))},
         {QStringLiteral("unsupportedPluginIds"), targets.value(QStringLiteral("unsupportedPluginIds"))}});
    QCOMPARE(denied.value(QStringLiteral("errorCode")).toString(), QStringLiteral("PERMISSION_DENIED"));
    QCOMPARE(plugin.asyncStarts(), startsBefore);
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    const QJsonObject confirmation = controller.handleToolCall(
        QStringLiteral("discover_cameras"),
        {{QStringLiteral("registryGeneration"), targets.value(QStringLiteral("registryGeneration"))},
         {QStringLiteral("pluginIds"), targets.value(QStringLiteral("asyncPluginIds"))},
         {QStringLiteral("unsupportedPluginIds"), targets.value(QStringLiteral("unsupportedPluginIds"))}});
    QCOMPARE(confirmation.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
    QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    QCOMPARE(plugin.asyncStarts(), startsBefore);
    controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
}

void TestCameraManager::testAgentDiscoveryBatchInvalidJobHoldsLeaseUntilDestroyed() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-invalid-job"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    plugin.setInvalidJobParent(true);
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    QSignalSpy settledSpy(&manager, &CameraManager::cameraDiscoveryBatchSettled);
    quint64 requestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), {plugin.pluginId()}, {},
                 &requestId, &error),
             qPrintable(error));
    QCOMPARE(manager.pendingDiscoveryCount(), 1);
    QVERIFY(!manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_IN_USE"));
    QCOMPARE(settledSpy.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 1500);
    QCOMPARE(manager.pendingDiscoveryCount(), 0);
    QJsonObject result;
    QVERIFY(manager.getCameraDiscoveryOperationResult(requestId, &result));
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    const QJsonObject child = result.value(QStringLiteral("pluginResults")).toArray().first().toObject();
    QCOMPARE(child.value(QStringLiteral("status")).toString(), QStringLiteral("start_failed"));
    QCOMPARE(child.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_JOB_INVALID"));
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testAgentDiscoveryBatchReservesPluginsBeforeReentrantStart() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin first(QStringLiteral("test-agent-discovery-reentrant-a"));
    FakeAsyncCameraPlugin second(QStringLiteral("test-agent-discovery-reentrant-b"));
    manager.registerCameraPlugin(&first);
    manager.registerCameraPlugin(&second);
    m_registeredIds << first.pluginId() << second.pluginId();
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    int reentrantStartCalls = 0;
    QString reentrantStartError;
    QString reentrantUnregisterError;
    first.setStartCallback([&](quint64) {
        QString startError;
        if (!manager.startDiscoveryAsync(second.pluginId(), 887766, &startError)) {
            ++reentrantStartCalls;
            reentrantStartError = startError;
        }
        manager.tryUnregisterCameraPlugin(second.pluginId(), &reentrantUnregisterError);
    });
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    QStringList pluginIds;
    for (const QJsonValue& value : targets.value(QStringLiteral("asyncPluginIds")).toArray())
        pluginIds.append(value.toString());
    quint64 requestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), pluginIds, {},
                 &requestId, &error),
             qPrintable(error));
    QCOMPARE(reentrantStartCalls, 1);
    QCOMPARE(reentrantStartError, QStringLiteral("CAMERA_DISCOVERY_BUSY"));
    QCOMPARE(reentrantUnregisterError, QStringLiteral("CAMERA_PLUGIN_IN_USE"));
    QCOMPARE(first.asyncStarts(), 2);
    QCOMPARE(second.asyncStarts(), 2); // all targets were reserved before the first plugin callback
    bool unregisteredAtBatchSettled = false;
    const QMetaObject::Connection settledConnection =
        connect(&manager, &CameraManager::cameraDiscoveryBatchSettled, &manager,
                [&](quint64 settledId, const QString&, const QString&) {
                    if (settledId != requestId)
                        return;
                    QString firstError;
                    QString secondError;
                    unregisteredAtBatchSettled = manager.tryUnregisterCameraPlugin(first.pluginId(), &firstError) &&
                                                 manager.tryUnregisterCameraPlugin(second.pluginId(), &secondError);
                });
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1500);
    QJsonObject result;
    QTRY_VERIFY_WITH_TIMEOUT(manager.getCameraDiscoveryOperationResult(requestId, &result), 1500);
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QVERIFY(unregisteredAtBatchSettled);
    m_registeredIds.removeAll(first.pluginId());
    m_registeredIds.removeAll(second.pluginId());
    disconnect(settledConnection);
}

void TestCameraManager::testAgentDiscoveryRequestIdsCannotAliasCameraOperations() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-id-collision"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    plugin.setDelayMs(180);
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    quint64 batchRequestId = 0;
    QString error;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 targets.value(QStringLiteral("registryGeneration")).toString().toULongLong(), {plugin.pluginId()}, {},
                 &batchRequestId, &error),
             qPrintable(error));
    const int factoryCalls = plugin.createCalls();
    QVERIFY(!manager.connectCameraAsync(QStringLiteral("fake-device"), batchRequestId, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_OPERATION_REQUEST_ID_IN_USE"));
    QVERIFY(!manager.disconnectCameraAsync(QStringLiteral("fake-device"), batchRequestId, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_OPERATION_REQUEST_ID_IN_USE"));
    QJsonObject result;
    QTRY_VERIFY_WITH_TIMEOUT(manager.getCameraDiscoveryOperationResult(batchRequestId, &result) &&
                                 result.value(QStringLiteral("status")).toString() != QLatin1String("pending"),
                             1500);
    QVERIFY(!manager.connectCameraAsync(QStringLiteral("fake-device"), batchRequestId, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_OPERATION_REQUEST_ID_IN_USE"));
    QVERIFY(!manager.disconnectCameraAsync(QStringLiteral("fake-device"), batchRequestId, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_OPERATION_REQUEST_ID_IN_USE"));
    QCOMPARE(plugin.createCalls(), factoryCalls);
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testAgentDiscoveryConfirmationBindsExactRegistrySnapshot() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin first(QStringLiteral("test-agent-confirm-discovery-a"));
    manager.registerCameraPlugin(&first);
    m_registeredIds.append(first.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    AgentController& controller = AgentController::instance();
    controller.clearConversation();
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    AgentActor actor;
    QJsonObject targets = actor.executeTool(QStringLiteral("get_camera_discovery_targets"), {});
    QJsonObject callParams{
        {QStringLiteral("registryGeneration"), targets.value(QStringLiteral("registryGeneration"))},
        {QStringLiteral("pluginIds"), targets.value(QStringLiteral("asyncPluginIds"))},
        {QStringLiteral("unsupportedPluginIds"), targets.value(QStringLiteral("unsupportedPluginIds"))}};

    FakeAsyncCameraPlugin second(QStringLiteral("test-agent-confirm-discovery-b"));
    manager.registerCameraPlugin(&second);
    m_registeredIds.append(second.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    const QJsonObject staleBeforeConfirmation =
        controller.handleToolCall(QStringLiteral("discover_cameras"), callParams);
    QCOMPARE(staleBeforeConfirmation.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_TARGET_CHANGED"));
    QCOMPARE(controller.state(), AgentController::AgentState::Idle);

    targets = actor.executeTool(QStringLiteral("get_camera_discovery_targets"), {});
    callParams = {{QStringLiteral("registryGeneration"), targets.value(QStringLiteral("registryGeneration"))},
                  {QStringLiteral("pluginIds"), targets.value(QStringLiteral("asyncPluginIds"))},
                  {QStringLiteral("unsupportedPluginIds"), targets.value(QStringLiteral("unsupportedPluginIds"))}};
    const QJsonObject pending = controller.handleToolCall(QStringLiteral("discover_cameras"), callParams);
    QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    const QJsonObject confirmationContext =
        controller.pendingConfirmationPayload().value(QStringLiteral("context")).toObject();
    QVERIFY(confirmationContext.value(QStringLiteral("summary")).toString().contains(first.pluginId()));
    QVERIFY(confirmationContext.value(QStringLiteral("summary")).toString().contains(second.pluginId()));
    const QJsonArray effects = confirmationContext.value(QStringLiteral("sideEffects")).toArray();
    QCOMPARE(effects.size(), 2);
    QCOMPARE(effects.at(0).toString(), QStringLiteral("camera_discovery:%1").arg(first.pluginId()));
    QCOMPARE(effects.at(1).toString(), QStringLiteral("camera_discovery:%1").arg(second.pluginId()));

    const int firstStarts = first.asyncStarts();
    const int secondStarts = second.asyncStarts();
    FakeAsyncCameraPlugin third(QStringLiteral("test-agent-confirm-discovery-c"));
    manager.registerCameraPlugin(&third);
    m_registeredIds.append(third.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QCOMPARE(controller.state(), AgentController::AgentState::Idle);
    QCOMPARE(first.asyncStarts(), firstStarts);
    QCOMPARE(second.asyncStarts(), secondStarts);
    QVERIFY(manager.tryUnregisterCameraPlugin(third.pluginId()));
    m_registeredIds.removeAll(third.pluginId());

    targets = actor.executeTool(QStringLiteral("get_camera_discovery_targets"), {});
    callParams = {{QStringLiteral("registryGeneration"), targets.value(QStringLiteral("registryGeneration"))},
                  {QStringLiteral("pluginIds"), targets.value(QStringLiteral("asyncPluginIds"))},
                  {QStringLiteral("unsupportedPluginIds"), targets.value(QStringLiteral("unsupportedPluginIds"))}};
    QSignalSpy batchSettledSpy(&manager, &CameraManager::cameraDiscoveryBatchSettled);
    const QJsonObject approved = controller.handleToolCall(QStringLiteral("discover_cameras"), callParams);
    QCOMPARE(approved.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QTRY_COMPARE_WITH_TIMEOUT(batchSettledSpy.count(), 1, 1500);
    QCOMPARE(first.asyncStarts(), firstStarts + 1);
    QCOMPARE(second.asyncStarts(), secondStarts + 1);
    const quint64 approvedRequestId = batchSettledSpy.first().at(0).toULongLong();
    QJsonObject approvedResult;
    QVERIFY(manager.getCameraDiscoveryOperationResult(approvedRequestId, &approvedResult));
    QCOMPARE(approvedResult.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    const QJsonObject queriedThroughAgent =
        actor.executeTool(QStringLiteral("get_camera_discovery_operation"),
                          {{QStringLiteral("requestId"), QString::number(approvedRequestId)}});
    QCOMPARE(queriedThroughAgent, approvedResult);
}

void TestCameraManager::testAgentDiscoveryHistoryUsesBoundedFifo() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-agent-discovery-history"));
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    plugin.setStartError(QStringLiteral("CAMERA_DISCOVERY_START_FAILED"));
    const QJsonObject targets = manager.cameraDiscoveryTargets();
    const quint64 generation = targets.value(QStringLiteral("registryGeneration")).toString().toULongLong();
    QList<quint64> requestIds;
    requestIds.reserve(257);
    QString error;
    for (int i = 0; i < 257; ++i) {
        quint64 requestId = 0;
        QVERIFY2(manager.requestCameraDiscoveryBatchAsync(generation, {plugin.pluginId()}, {}, &requestId, &error),
                 qPrintable(error));
        QVERIFY(requestId != 0);
        requestIds.append(requestId);
    }
    QJsonObject result;
    QVERIFY(!manager.getCameraDiscoveryOperationResult(requestIds.first(), &result));
    QCOMPARE(result, QJsonObject());
    for (int i = 1; i < requestIds.size(); ++i) {
        QVERIFY2(manager.getCameraDiscoveryOperationResult(requestIds.at(i), &result), qPrintable(QString::number(i)));
        QCOMPARE(result.value(QStringLiteral("requestId")).toString(), QString::number(requestIds.at(i)));
    }
}

void TestCameraManager::testAsyncCameraSessionLifecycleAndWindowShutdown() {
    ToolSchema::instance().registerDefaultTools();
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-camera-shutdown"));
    plugin.setDeviceId(QStringLiteral("async-session-device"));
    plugin.setUseInfoDeviceId(true);
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QString error;
    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 1, 1000);
    QCOMPARE(discoverySpy.first().at(2).toString(), QStringLiteral("success"));
    QCOMPARE(manager.pendingDiscoveryCount(), 0);

    CameraCounters& counters = plugin.cameraCounters();
    const QMetaObject::Connection startedConnection =
        connect(&manager, &CameraManager::cameraOperationStarted, &manager,
                [&counters](const QString& deviceId, quint64, const QString& operation) {
                    Q_UNUSED(operation);
                    if (deviceId == QLatin1String("async-session-device"))
                        counters.disconnectStartedSignalSeen = true;
                });
    counters.enforceWorkerCalls = true;
    QSignalSpy operationSpy(&manager, &CameraManager::cameraOperationSettled);
    plugin.setParentedNonCamera(true);
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-session-device"), 4999, &error));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 1, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QCOMPARE(operationSpy.last().at(4).toString(), QStringLiteral("CAMERA_FACTORY_INVALID"));
    QTRY_VERIFY_WITH_TIMEOUT(plugin.lastFactoryObject() == nullptr, 1000);
    QCOMPARE(manager.cameraSessionCount(), 0);
    plugin.setParentedNonCamera(false);
    plugin.setFactoryWrongThread(true);
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-session-device"), 4998, &error));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 2, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QTRY_VERIFY_WITH_TIMEOUT(plugin.lastFactoryObject() == nullptr, 1000);
    QVERIFY(plugin.wrongThreadFactoryDestroyed());
    QCOMPARE(manager.cameraSessionCount(), 0);
    plugin.setFactoryWrongThread(false);

    counters.connectSucceeds = false;
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-session-device"), 5000, &error));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 3, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QCOMPARE(manager.cameraSessionCount(), 0);
    QCOMPARE(counters.destroyedThreadCalls.load(), 2);

    counters.connectSucceeds = true;
    counters.connectThrows = true;
    counters.isConnectedThrows = true;
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-session-device"), 5001, &error));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 4, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QCOMPARE(manager.cameraState(QStringLiteral("async-session-device")), CameraState::Error);
    QCOMPARE(manager.cameraSessionCount(), 1);
    counters.connectThrows = false;
    counters.isConnectedThrows = false;
    QVERIFY(manager.disconnectCameraAsync(QStringLiteral("async-session-device"), 5002, &error));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 5, 1000);
    QCOMPARE(operationSpy.last().at(2).toString(), QStringLiteral("disconnect"));
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("success"));
    QCOMPARE(manager.cameraSessionCount(), 0);
    QCOMPARE(counters.destroyedThreadCalls.load(), 3);

    QSemaphore connectGate;
    counters.connectGate = &connectGate;
    const quint64 collidingRequestId = std::numeric_limits<quint64>::max() - 1;
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-session-device"), collidingRequestId, &error));
    QVERIFY(error.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(counters.connectEntered.load(), 3, 1000);
    QCOMPARE(manager.cameraSessionCount(), 1);
    QCOMPARE(manager.cameraState(QStringLiteral("async-session-device")), CameraState::Connecting);
    QVERIFY(!manager.getCamera(QStringLiteral("async-session-device")));
    QVERIFY(!manager.acquireCamera(QStringLiteral("async-session-device")));
    QVERIFY(!manager.connectCameraAsync(QStringLiteral("async-session-device"), 5002 + 10, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_OPERATION_BUSY"));
    const int discoverySettledBeforeRefresh = discoverySpy.count();
    manager.refreshCameras();
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), discoverySettledBeforeRefresh + 1, 1000);
    QCOMPARE(discoverySpy.last().at(2).toString(), QStringLiteral("success"));
    bool managerEventLoopResponded = false;
    QTimer::singleShot(0, &manager, [&managerEventLoopResponded]() { managerEventLoopResponded = true; });
    QTRY_VERIFY_WITH_TIMEOUT(managerEventLoopResponded, 1000);
    connectGate.release();
    counters.connectGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 6, 1000);
    QCOMPARE(operationSpy.last().at(2).toString(), QStringLiteral("connect"));
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("success"));
    QVERIFY(manager.isConnected(QStringLiteral("async-session-device")));
    QVERIFY(!manager.connectCameraAsync(QStringLiteral("async-session-device"), 5013, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_ALREADY_CONNECTED"));
    QCOMPARE(counters.callsOnManagerThread.load(), 0);

    counters.disconnectSucceeds = false;
    counters.disconnectStartedSignalSeen = false;
    std::thread disconnectAllCaller([&manager]() { manager.disconnectAll(); });
    disconnectAllCaller.join();
    QTRY_COMPARE_WITH_TIMEOUT(counters.disconnectEntered.load(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 7, 1000);
    QCOMPARE(operationSpy.last().at(2).toString(), QStringLiteral("disconnect"));
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QCOMPARE(counters.disconnectBeforeStartedSignal.load(), 0);
    QCOMPARE(manager.cameraState(QStringLiteral("async-session-device")), CameraState::Error);
    QCOMPARE(manager.cameraSessionCount(), 1);
    QVERIFY(!manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    QCOMPARE(error, QStringLiteral("CAMERA_PLUGIN_IN_USE"));

    counters.disconnectSucceeds = true;
    counters.disconnectStartedSignalSeen = false;
    QVERIFY(manager.disconnectCameraAsync(QStringLiteral("async-session-device"), 5014, &error));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 8, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("success"));
    QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 1000);
    QCOMPARE(counters.destroyedThreadCalls.load(), 4);
    QCOMPARE(counters.disconnectBeforeStartedSignal.load(), 0);
    QCOMPARE(counters.callsOnManagerThread.load(), 0);
    QCOMPARE(counters.wrongWorkerThreadCalls.load(), 0);

    FakeAsyncCameraPlugin lateDisconnectPlugin(QStringLiteral("test-async-camera-late-disconnect"));
    lateDisconnectPlugin.setDeviceId(QStringLiteral("async-late-disconnect-device"));
    lateDisconnectPlugin.setUseInfoDeviceId(true);
    manager.registerCameraPlugin(&lateDisconnectPlugin);
    m_registeredIds.append(lateDisconnectPlugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    CameraCounters& lateDisconnectCounters = lateDisconnectPlugin.cameraCounters();
    lateDisconnectCounters.enforceWorkerCalls = true;
    QSemaphore disconnectAllGate;
    lateDisconnectCounters.disconnectGate = &disconnectAllGate;
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-late-disconnect-device"), 5016, &error));
    const int settledBeforeDisconnectAllConnect = operationSpy.count();
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), settledBeforeDisconnectAllConnect + 1, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("success"));
    manager.setAsyncDisconnectTimeoutForTesting(100);
    const int settledBeforeDisconnectAll = operationSpy.count();
    manager.disconnectAll();
    QTRY_COMPARE_WITH_TIMEOUT(lateDisconnectCounters.disconnectEntered.load(), 1, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), settledBeforeDisconnectAll + 1, 1000);
    QCOMPARE(operationSpy.last().at(2).toString(), QStringLiteral("disconnect"));
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QCOMPARE(operationSpy.last().at(4).toString(), QStringLiteral("CAMERA_DISCONNECT_TIMEOUT_UNCONFIRMED"));
    const quint64 disconnectAllTimeoutId = operationSpy.last().at(1).toULongLong();
    QJsonObject disconnectAllTimeoutResult;
    QVERIFY(manager.getCameraOperationResult(disconnectAllTimeoutId, &disconnectAllTimeoutResult));
    QCOMPARE(disconnectAllTimeoutResult.value(QStringLiteral("retryable")).toBool(), false);
    QVERIFY(!manager.acquireCamera(QStringLiteral("async-late-disconnect-device")));
    manager.setAsyncDisconnectTimeoutForTesting(30000);
    QCOMPARE(manager.cameraSessionCount(), 1);
    disconnectAllGate.release();
    QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), settledBeforeDisconnectAll + 2, 2000);
    QJsonObject stableDisconnectAllTimeoutResult;
    QVERIFY(manager.getCameraOperationResult(disconnectAllTimeoutId, &stableDisconnectAllTimeoutResult));
    QCOMPARE(stableDisconnectAllTimeoutResult, disconnectAllTimeoutResult);
    QCOMPARE(lateDisconnectCounters.disconnectCalls, 1);
    lateDisconnectCounters.disconnectGate = nullptr;

    QSemaphore shutdownLateDisconnectGate;
    lateDisconnectCounters.disconnectGate = &shutdownLateDisconnectGate;
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-late-disconnect-device"), 5017, &error));
    const int settledBeforeShutdownTimeoutConnect = operationSpy.count();
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), settledBeforeShutdownTimeoutConnect + 1, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("success"));
    manager.setAsyncDisconnectTimeoutForTesting(100);
    const int settledBeforeShutdownTimeout = operationSpy.count();
    quint64 shutdownDisconnectTimeoutId = 0;
    QVERIFY(manager.requestDisconnectCameraAsync(QStringLiteral("async-late-disconnect-device"),
                                                 &shutdownDisconnectTimeoutId, &error));
    QVERIFY2(shutdownDisconnectTimeoutId != 0, qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(lateDisconnectCounters.disconnectEntered.load(), 2, 1000);
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), settledBeforeShutdownTimeout + 1, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QCOMPARE(operationSpy.last().at(4).toString(), QStringLiteral("CAMERA_DISCONNECT_TIMEOUT_UNCONFIRMED"));
    QJsonObject shutdownDisconnectTimeoutResult;
    QVERIFY(manager.getCameraOperationResult(shutdownDisconnectTimeoutId, &shutdownDisconnectTimeoutResult));
    manager.setAsyncDisconnectTimeoutForTesting(30000);

    FakeAsyncCameraPlugin detailsPlugin(QStringLiteral("test-async-camera-shutdown-details"));
    detailsPlugin.setDeviceId(QStringLiteral("async-shutdown-details-device"));
    detailsPlugin.setUseInfoDeviceId(true);
    CameraCounters& detailsCounters = detailsPlugin.cameraCounters();
    detailsCounters.enforceWorkerCalls = true;
    manager.registerCameraPlugin(&detailsPlugin);
    m_registeredIds.append(detailsPlugin.pluginId());
    auto detailsDeviceDiscovered = [&manager]() {
        const QList<CameraStatus> statuses = manager.knownCameras();
        return std::any_of(statuses.cbegin(), statuses.cend(), [](const CameraStatus& status) {
            return status.deviceId == QLatin1String("async-shutdown-details-device");
        });
    };
    QTRY_VERIFY_WITH_TIMEOUT(detailsDeviceDiscovered(), 1000);
    const QMetaObject::Connection detailsStartedConnection =
        connect(&manager, &CameraManager::cameraOperationStarted, &manager,
                [&detailsCounters](const QString& deviceId, quint64, const QString&) {
                    if (deviceId == QLatin1String("async-shutdown-details-device"))
                        detailsCounters.disconnectStartedSignalSeen = true;
                });
    AgentActor actor;
    const int settledBeforeDetailsPluginConnect = operationSpy.count();
    const QJsonObject detailsPluginConnect =
        actor.executeTool(QStringLiteral("connect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-shutdown-details-device")}});
    QVERIFY2(detailsPluginConnect.value(QStringLiteral("status")).toString() == QLatin1String("pending"),
             qPrintable(QString::fromUtf8(QJsonDocument(detailsPluginConnect).toJson(QJsonDocument::Compact))));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), settledBeforeDetailsPluginConnect + 1, 1000);
    QSemaphore shutdownDetailsGate;
    detailsCounters.detailsGate = &shutdownDetailsGate;
    const QJsonObject shutdownDetails =
        actor.executeTool(QStringLiteral("get_camera_details"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-shutdown-details-device")}});
    QCOMPARE(shutdownDetails.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    const quint64 shutdownDetailsId = shutdownDetails.value(QStringLiteral("requestId")).toString().toULongLong();
    QTRY_COMPARE_WITH_TIMEOUT(detailsCounters.detailsGateEntries.load(), 1, 1000);

    QSemaphore shutdownConnectGate;
    QSemaphore shutdownDisconnectGate;
    counters.connectGate = &shutdownConnectGate;
    counters.disconnectGate = &shutdownDisconnectGate;
    counters.disconnectStartedSignalSeen = false;
    QVERIFY(manager.connectCameraAsync(QStringLiteral("async-session-device"), 5015, &error));
    QTRY_COMPARE_WITH_TIMEOUT(counters.connectEntered.load(), 4, 1000);
    QCOMPARE(manager.cameraSessionCount(), 3);
    QCOMPARE(manager.pendingDiscoveryCount(), 0);

    FakeAsyncCameraPlugin discoveryDrainPlugin(QStringLiteral("test-async-camera-shutdown-discovery-batch"));
    discoveryDrainPlugin.setDelayMs(20);
    manager.registerCameraPlugin(&discoveryDrainPlugin);
    m_registeredIds.append(discoveryDrainPlugin.pluginId());
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 1000);
    discoveryDrainPlugin.setDelayMs(500);
    const QJsonObject discoveryTargets = manager.cameraDiscoveryTargets();
    QStringList discoveryPluginIds;
    for (const QJsonValue& value : discoveryTargets.value(QStringLiteral("asyncPluginIds")).toArray())
        discoveryPluginIds.append(value.toString());
    QStringList unsupportedDiscoveryPluginIds;
    for (const QJsonValue& value : discoveryTargets.value(QStringLiteral("unsupportedPluginIds")).toArray())
        unsupportedDiscoveryPluginIds.append(value.toString());
    QStringList shutdownOrder;
    const QMetaObject::Connection batchSettledConnection = connect(
        &manager, &CameraManager::cameraDiscoveryBatchSettled, &manager,
        [&shutdownOrder](quint64, const QString&, const QString&) { shutdownOrder.append(QStringLiteral("batch")); });
    quint64 discoveryBatchId = 0;
    QVERIFY2(manager.requestCameraDiscoveryBatchAsync(
                 discoveryTargets.value(QStringLiteral("registryGeneration")).toString().toULongLong(),
                 discoveryPluginIds, unsupportedDiscoveryPluginIds, &discoveryBatchId, &error),
             qPrintable(error));

    const int settledBeforeWindowClose = operationSpy.count();
    MainWindow window;
    window.show();
    QSignalSpy shutdownSpy(&manager, &CameraManager::shutdownCompleted);
    const QMetaObject::Connection shutdownCompletedConnection =
        connect(&manager, &CameraManager::shutdownCompleted, &manager,
                [&shutdownOrder]() { shutdownOrder.append(QStringLiteral("shutdown")); });
    QVERIFY(!window.close());
    QVERIFY(window.isVisible());
    QVERIFY(shutdownSpy.isEmpty());
    QTest::qWait(150);
    QVERIFY(shutdownSpy.isEmpty());
    QCOMPARE(lateDisconnectCounters.disconnectEntered.load(), 2);
    shutdownConnectGate.release();
    QTRY_COMPARE_WITH_TIMEOUT(counters.disconnectEntered.load(), 3, 1000);
    QCOMPARE(counters.disconnectBeforeStartedSignal.load(), 0);
    QVERIFY(shutdownSpy.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(window.isVisible(), 1000);
    QCOMPARE(detailsCounters.disconnectEntered.load(), 0);
    shutdownDetailsGate.release();
    detailsCounters.detailsGate = nullptr;
    QTRY_COMPARE_WITH_TIMEOUT(detailsCounters.disconnectEntered.load(), 1, 1000);
    QCOMPARE(detailsCounters.disconnectBeforeStartedSignal.load(), 0);
    shutdownLateDisconnectGate.release();
    QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 2, 2000);
    QCOMPARE(lateDisconnectCounters.disconnectEntered.load(), 2);
    shutdownDisconnectGate.release();
    QTRY_COMPARE_WITH_TIMEOUT(shutdownSpy.count(), 1, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 1000);
    QCOMPARE(manager.cameraSessionCount(), 0);
    QCOMPARE(operationSpy.count(), settledBeforeWindowClose + 4);
    bool shutdownConnectSettledCancelled = false;
    for (const QList<QVariant>& args : operationSpy) {
        if (args.at(1).toULongLong() == 5015 && args.at(2).toString() == QLatin1String("connect") &&
            args.at(3).toString() == QLatin1String("cancelled"))
            shutdownConnectSettledCancelled = true;
    }
    QVERIFY(shutdownConnectSettledCancelled);
    const QJsonObject cancelledResult = actor.executeTool(
        QStringLiteral("get_camera_operation"), QJsonObject{{QStringLiteral("requestId"), QStringLiteral("5015")}});
    QCOMPARE(cancelledResult.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(counters.destroyedThreadCalls.load(), 5);
    QCOMPARE(counters.callsOnManagerThread.load(), 0);
    QCOMPARE(counters.wrongWorkerThreadCalls.load(), 0);
    QCOMPARE(detailsCounters.destroyedThreadCalls.load(), 1);
    QCOMPARE(detailsCounters.callsOnManagerThread.load(), 0);
    QCOMPARE(detailsCounters.wrongWorkerThreadCalls.load(), 0);
    QJsonObject stableShutdownDisconnectTimeoutResult;
    QVERIFY(manager.getCameraOperationResult(shutdownDisconnectTimeoutId, &stableShutdownDisconnectTimeoutResult));
    QCOMPARE(stableShutdownDisconnectTimeoutResult, shutdownDisconnectTimeoutResult);
    QCOMPARE(lateDisconnectCounters.disconnectCalls, 2);
    QCOMPARE(lateDisconnectCounters.destroyedThreadCalls.load(), 2);
    QTRY_VERIFY_WITH_TIMEOUT(shutdownOrder.contains(QStringLiteral("batch")), 1000);
    QVERIFY(shutdownOrder.indexOf(QStringLiteral("batch")) < shutdownOrder.indexOf(QStringLiteral("shutdown")));
    QJsonObject shutdownDetailsResult;
    QVERIFY(manager.getCameraOperationResult(shutdownDetailsId, &shutdownDetailsResult));
    QCOMPARE(shutdownDetailsResult.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QJsonObject discoveryBatchResult;
    QVERIFY(manager.getCameraDiscoveryOperationResult(discoveryBatchId, &discoveryBatchResult));
    QVERIFY(discoveryBatchResult.value(QStringLiteral("status")).toString() != QLatin1String("pending"));
    QCOMPARE(discoveryDrainPlugin.cancelCalls(), 1);
    QCOMPARE(discoveryBatchResult.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QCOMPARE(discoveryBatchResult.value(QStringLiteral("pluginResults"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("success"));
    manager.shutdownAsync();
    QCOMPARE(shutdownSpy.count(), 1);
    QVERIFY(!manager.connectCameraAsync(QStringLiteral("async-session-device"), 5016, &error));
    QCOMPARE(error, QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));

    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId(), &error));
    m_registeredIds.removeAll(plugin.pluginId());
    QVERIFY(manager.tryUnregisterCameraPlugin(detailsPlugin.pluginId(), &error));
    m_registeredIds.removeAll(detailsPlugin.pluginId());
    QVERIFY(manager.tryUnregisterCameraPlugin(discoveryDrainPlugin.pluginId(), &error));
    m_registeredIds.removeAll(discoveryDrainPlugin.pluginId());
    disconnect(startedConnection);
    disconnect(detailsStartedConnection);
    disconnect(batchSettledConnection);
    disconnect(shutdownCompletedConnection);
}

void TestCameraManager::testAgentCameraOperationsAreAsyncAndQueryable() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-agent-camera"));
    plugin.setDeviceId(QStringLiteral("async-agent-device"));
    plugin.setUseInfoDeviceId(true);
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 1, 1000);
    QCOMPARE(discoverySpy.first().at(2).toString(), QStringLiteral("success"));

    CameraCounters& counters = plugin.cameraCounters();
    QSignalSpy operationSpy(&manager, &CameraManager::cameraOperationSettled);
    bool terminalVisibleInsideSignal = false;
    const QMetaObject::Connection terminalConnection =
        connect(&manager, &CameraManager::cameraOperationSettled, &manager,
                [&manager, &terminalVisibleInsideSignal](const QString&, quint64 id, const QString&,
                                                         const QString& status, const QString&) {
                    if (status != QLatin1String("pending")) {
                        QJsonObject result;
                        terminalVisibleInsideSignal = manager.getCameraOperationResult(id, &result) &&
                                                      result.value(QStringLiteral("status")).toString() == status;
                    }
                });
    QSemaphore connectGate;
    counters.connectGate = &connectGate;
    AgentActor actor;
    const QJsonObject started =
        actor.executeTool(QStringLiteral("connect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    const QString requestId = started.value(QStringLiteral("requestId")).toString();
    QVERIFY(!requestId.isEmpty());
    QCOMPARE(manager.cameraState(QStringLiteral("async-agent-device")), CameraState::Connecting);
    QJsonObject operation = actor.executeTool(QStringLiteral("get_camera_operation"),
                                              QJsonObject{{QStringLiteral("requestId"), requestId}});
    QCOMPARE(operation.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    const QJsonObject oppositeWhileConnecting =
        actor.executeTool(QStringLiteral("disconnect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QCOMPARE(oppositeWhileConnecting.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_OPERATION_BUSY"));
    QCOMPARE(oppositeWhileConnecting.value(QStringLiteral("details"))
                 .toObject()
                 .value(QStringLiteral("hardwareDisconnectConfirmed"))
                 .toBool(),
             false);
    QTRY_COMPARE_WITH_TIMEOUT(counters.connectEntered.load(), 1, 1000);
    bool managerLoopResponded = false;
    QTimer::singleShot(0, &manager, [&managerLoopResponded]() { managerLoopResponded = true; });
    QTRY_VERIFY_WITH_TIMEOUT(managerLoopResponded, 1000);
    connectGate.release();
    counters.connectGate = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(manager.cameraState(QStringLiteral("async-agent-device")) == CameraState::Connected, 1000);
    operation = actor.executeTool(QStringLiteral("get_camera_operation"),
                                  QJsonObject{{QStringLiteral("requestId"), requestId}});
    QCOMPARE(operation.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QCOMPARE(operation.value(QStringLiteral("isConnected")).toBool(), true);

    counters.disconnectSucceeds = false;
    QSemaphore disconnectGate;
    counters.disconnectGate = &disconnectGate;
    const QJsonObject disconnectStarted =
        actor.executeTool(QStringLiteral("disconnect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QCOMPARE(disconnectStarted.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
    const QString disconnectId = disconnectStarted.value(QStringLiteral("requestId")).toString();
    QTRY_COMPARE_WITH_TIMEOUT(counters.disconnectEntered.load(), 1, 1000);
    const QJsonObject oppositeWhileDisconnecting =
        actor.executeTool(QStringLiteral("connect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QCOMPARE(oppositeWhileDisconnecting.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("CAMERA_OPERATION_BUSY"));
    disconnectGate.release();
    counters.disconnectGate = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(manager.cameraState(QStringLiteral("async-agent-device")) == CameraState::Error, 1000);
    operation = actor.executeTool(QStringLiteral("get_camera_operation"),
                                  QJsonObject{{QStringLiteral("requestId"), disconnectId}});
    QCOMPARE(operation.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QCOMPARE(operation.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), false);
    counters.disconnectSucceeds = true;
    const QJsonObject retryDisconnect =
        actor.executeTool(QStringLiteral("disconnect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QVERIFY(retryDisconnect.value(QStringLiteral("requestId")).toString() != disconnectId);
    QTRY_VERIFY_WITH_TIMEOUT(manager.cameraState(QStringLiteral("async-agent-device")) == CameraState::Disconnected,
                             1000);
    operation = actor.executeTool(
        QStringLiteral("get_camera_operation"),
        QJsonObject{{QStringLiteral("requestId"), retryDisconnect.value(QStringLiteral("requestId")).toString()}});
    QCOMPARE(operation.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QCOMPARE(operation.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);

    AgentController& controller = AgentController::instance();
    qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
    controller.clearConversation();
    controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
    QSignalSpy auditSpy(&controller, &AgentController::actionLogEntryAdded);
    QJsonObject confirmedConnect =
        controller.handleToolCall(QStringLiteral("connect_camera"),
                                  QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QCOMPARE(confirmedConnect.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 4, 1000);
    QCOMPARE(operationSpy.at(3).at(3).toString(), QStringLiteral("success"));
    QCOMPARE(auditSpy.size(), 1);
    QCOMPARE(auditSpy.first().at(0).value<AgentActionLogEntry>().result, QStringLiteral("pending"));
    QVERIFY(!auditSpy.first().at(0).value<AgentActionLogEntry>().undoable);
    QVERIFY(terminalVisibleInsideSignal);

    QJsonObject confirmedDisconnect =
        controller.handleToolCall(QStringLiteral("disconnect_camera"),
                                  QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QCOMPARE(confirmedDisconnect.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
    QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 5, 1000);
    QCOMPARE(operationSpy.at(4).at(2).toString(), QStringLiteral("disconnect"));
    QCOMPARE(operationSpy.at(4).at(3).toString(), QStringLiteral("success"));
    QCOMPARE(auditSpy.size(), 2);
    QCOMPARE(auditSpy.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("pending"));
    QVERIFY(!auditSpy.last().at(0).value<AgentActionLogEntry>().undoable);
    const QString confirmedDisconnectId = QString::number(operationSpy.at(4).at(1).toULongLong());
    operation = actor.executeTool(QStringLiteral("get_camera_operation"),
                                  QJsonObject{{QStringLiteral("requestId"), confirmedDisconnectId}});
    QCOMPARE(operation.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);

    QVERIFY(manager.connectCamera(QStringLiteral("async-agent-device")));
    const QJsonObject legacyDisconnect =
        actor.executeTool(QStringLiteral("disconnect_camera"),
                          QJsonObject{{QStringLiteral("deviceId"), QStringLiteral("async-agent-device")}});
    QCOMPARE(legacyDisconnect.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_SESSION_REQUIRED"));
    manager.disconnectCamera(QStringLiteral("async-agent-device"));

    for (const QString& invalidId : {QStringLiteral("0"), QStringLiteral("+1"), QStringLiteral(" 1"),
                                     QStringLiteral("01"), QStringLiteral("18446744073709551616")}) {
        const QJsonObject invalid = actor.executeTool(QStringLiteral("get_camera_operation"),
                                                      QJsonObject{{QStringLiteral("requestId"), invalidId}});
        QCOMPARE(invalid.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    }
    const QJsonObject missing = actor.executeTool(QStringLiteral("get_camera_operation"),
                                                  QJsonObject{{QStringLiteral("requestId"), QStringLiteral("77")}});
    QCOMPARE(missing.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_OPERATION_NOT_FOUND"));
    QObject::disconnect(terminalConnection);
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

void TestCameraManager::testAgentCameraOperationHistoryBoundedFifo() {
    CameraManager& manager = CameraManager::instance();
    FakeAsyncCameraPlugin plugin(QStringLiteral("test-async-agent-camera-history"));
    plugin.setDeviceId(QStringLiteral("async-agent-history-device"));
    plugin.setUseInfoDeviceId(true);
    plugin.cameraCounters().connectSucceeds = false;
    manager.registerCameraPlugin(&plugin);
    m_registeredIds.append(plugin.pluginId());
    QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 1, 1000);
    QCOMPARE(discoverySpy.first().at(2).toString(), QStringLiteral("success"));

    plugin.setAdditionalDeviceIds({QStringLiteral("async-agent-history-fast-device")});
    manager.refreshCameras();
    QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 2, 1000);
    QSignalSpy operationSpy(&manager, &CameraManager::cameraOperationSettled);
    bool slowResultVisibleInsideSignal = false;
    const QMetaObject::Connection historyConnection =
        connect(&manager, &CameraManager::cameraOperationSettled, &manager,
                [&manager, &slowResultVisibleInsideSignal](const QString&, quint64 id, const QString&,
                                                           const QString& status, const QString&) {
                    if (status == QLatin1String("error")) {
                        QJsonObject result;
                        slowResultVisibleInsideSignal = manager.getCameraOperationResult(id, &result) &&
                                                        result.value(QStringLiteral("status")).toString() == status;
                    }
                });
    QSemaphore slowGate;
    plugin.cameraCounters().connectGate = &slowGate;
    quint64 slowRequestId = 0;
    QString error;
    QVERIFY(manager.requestConnectCameraAsync(QStringLiteral("async-agent-history-device"), &slowRequestId, &error));
    QTRY_COMPARE_WITH_TIMEOUT(plugin.cameraCounters().connectEntered.load(), 1, 1000);
    plugin.cameraCounters().connectGate = nullptr;

    QList<quint64> requestIds;
    for (int i = 0; i < 256; ++i) {
        quint64 requestId = 0;
        QVERIFY2(
            manager.requestConnectCameraAsync(QStringLiteral("async-agent-history-fast-device"), &requestId, &error),
            qPrintable(error));
        QVERIFY(requestId != 0);
        requestIds.append(requestId);
        QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), i + 1, 1000);
    }
    slowGate.release();
    QTRY_COMPARE_WITH_TIMEOUT(operationSpy.count(), 257, 1000);
    QCOMPARE(operationSpy.last().at(3).toString(), QStringLiteral("error"));
    QVERIFY(slowResultVisibleInsideSignal);

    QJsonObject result;
    QVERIFY(!manager.getCameraOperationResult(requestIds.first(), &result));
    QCOMPARE(result, QJsonObject());
    for (int i = 1; i < requestIds.size(); ++i) {
        QVERIFY2(manager.getCameraOperationResult(requestIds.at(i), &result), qPrintable(QString::number(i)));
        QCOMPARE(result.value(QStringLiteral("requestId")).toString(), QString::number(requestIds.at(i)));
    }
    QVERIFY(manager.getCameraOperationResult(slowRequestId, &result));
    QCOMPARE(result.value(QStringLiteral("requestId")).toString(), QString::number(slowRequestId));
    QObject::disconnect(historyConnection);
    QVERIFY(manager.tryUnregisterCameraPlugin(plugin.pluginId()));
    m_registeredIds.removeAll(plugin.pluginId());
}

QTEST_MAIN(TestCameraManager)
#include "test_cameramanager.moc"
