#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/agent/GuiEvent.h"
#include "core/agent/ILLMClient.h"
#include "core/common/CancellationToken.h"
#include "core/common/UserManager.h"
#include "core/device/CameraDiscoveryJob.h"
#include "core/device/CameraManager.h"
#include "core/engine/FlowExecutionService.h"
#include "core/engine/RunEngine.h"
#include "core/engine/FlowExecutionService.h"
#include "core/interface/ICamera.h"
#include "core/interface/ICameraDiscoveryAsync.h"
#include "core/interface/ICameraParameterControl.h"
#include "core/interface/ICameraPlugin.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/ImageData.h"
#include "core/model/Project.h"
#include "plugins/image_processing/GrabImage/GrabImagePlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QWaitCondition>
#include <QtTest/QtTest>
#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <thread>

using namespace DeepLux;

namespace {

constexpr auto kPluginId = "com.deeplux.test.agent-camera-g5";
constexpr auto kDeviceId = "agent-camera-g5-device";
constexpr auto kLegacyPluginId = "com.deeplux.test.agent-camera-g5-legacy";
constexpr auto kLegacyAsyncPluginId = "com.deeplux.test.agent-camera-g5-legacy-async";
constexpr auto kLegacyDeviceId = "agent-camera-g5-legacy-device";

struct FakeCameraState {
    QMutex mutex;
    QWaitCondition triggerGate;
    QWaitCondition connectGate;
    QWaitCondition disconnectGate;
    int discoveryCalls = 0;
    int factoryCalls = 0;
    int connectCalls = 0;
    int connectReturnedCalls = 0;
    int disconnectCalls = 0;
    int disconnectReturnedCalls = 0;
    int detailsCalls = 0;
    int exposureSetCalls = 0;
    int gainSetCalls = 0;
    int triggerCalls = 0;
    int startCalls = 0;
    int stopCalls = 0;
    int destroyed = 0;
    int wrongThreadCalls = 0;
    bool blockTrigger = false;
    bool releaseTrigger = false;
    bool triggerEntered = false;
    bool blockConnect = false;
    bool releaseConnect = false;
    bool connectEntered = false;
    bool blockDisconnect = false;
    bool releaseDisconnect = false;
    bool disconnectEntered = false;
    bool emitFrame = true;
    bool triggerSucceeds = true;
    bool throwOnValidate = false;
    bool throwOnGainSet = false;
    bool throwOnConnectAfterConnecting = false;
    bool throwOnDisconnectAfterDisconnecting = false;
    bool connectReportsFailureAfterConnecting = false;
    bool clampGain = false;
    double exposure = 1000.0;
    double gain = 1.0;
    QString discoveredName = QStringLiteral("Agent lifecycle fake camera");
    QString pluginId = QString::fromLatin1(kPluginId);
    QString deviceId = QString::fromLatin1(kDeviceId);
    QString discoveredSerial = QStringLiteral("g5-fake-serial");
    QString discoveredIpAddress = QStringLiteral("192.0.2.10");
    bool discoveryEnabled = true;
    QImage image;
};

class FakeAsyncCamera : public ICamera, public ICameraParameterControl {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICamera DeepLux::ICameraParameterControl)

public:
    explicit FakeAsyncCamera(FakeCameraState* state) : m_state(state) {}
    ~FakeAsyncCamera() override {
        QMutexLocker locker(&m_state->mutex);
        ++m_state->destroyed;
    }

    bool validateParameter(const QString& parameter, const QJsonValue& value, QString* errorCode) const override {
        recordCall();
        {
            QMutexLocker locker(&m_state->mutex);
            if (m_state->throwOnValidate)
                throw std::runtime_error("fixture validation failure");
        }
        const double number = value.toDouble(std::numeric_limits<double>::quiet_NaN());
        const bool exposure = parameter == QLatin1String("exposureTime");
        const bool gain = parameter == QLatin1String("gain");
        const bool valid =
            (exposure && number >= 100.0 && number <= 20000.0) || (gain && number >= 0.0 && number <= 10.0);
        if (!valid && errorCode)
            *errorCode = QStringLiteral("CAMERA_PARAMETER_OUT_OF_RANGE");
        return valid;
    }

    QString deviceId() const override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        return m_state->deviceId;
    }
    QString name() const override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        return m_state->discoveredName;
    }
    QString serialNumber() const override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        return m_state->discoveredSerial;
    }
    QString manufacturer() const override {
        recordCall();
        return QStringLiteral("DeepLux test fixture");
    }
    CameraCapabilities capabilities() const override {
        recordCall();
        return {};
    }
    bool isPlatformSupported() const override {
        recordCall();
        return true;
    }
    QString platformNotSupportedMessage() const override {
        recordCall();
        return {};
    }
    bool connect() override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        ++m_state->connectCalls;
        m_state->connectEntered = true;
        m_state->connectGate.wakeAll();
        while (m_state->blockConnect && !m_state->releaseConnect)
            m_state->connectGate.wait(&m_state->mutex);
        m_connected = true;
        ++m_state->connectReturnedCalls;
        if (m_state->throwOnConnectAfterConnecting)
            throw std::runtime_error("fixture connect failure after state change");
        return !m_state->connectReportsFailureAfterConnecting;
    }
    void disconnect() override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        ++m_state->disconnectCalls;
        m_state->disconnectEntered = true;
        m_state->disconnectGate.wakeAll();
        while (m_state->blockDisconnect && !m_state->releaseDisconnect)
            m_state->disconnectGate.wait(&m_state->mutex);
        m_acquiring = false;
        m_connected = false;
        ++m_state->disconnectReturnedCalls;
        if (m_state->throwOnDisconnectAfterDisconnecting)
            throw std::runtime_error("fixture disconnect failure after state change");
    }
    bool isConnected() const override {
        recordCall();
        return m_connected;
    }
    bool startAcquisition() override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        ++m_state->startCalls;
        m_acquiring = true;
        QImage delayedPriorFrame(4, 3, QImage::Format_RGB32);
        delayedPriorFrame.fill(qRgb(1, 2, 3));
        emit imageReceived(delayedPriorFrame, {{QStringLiteral("fixture"), QStringLiteral("delayed-prior-frame")}});
        return true;
    }
    void stopAcquisition() override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        ++m_state->stopCalls;
        m_acquiring = false;
    }
    bool isAcquiring() const override {
        recordCall();
        return m_acquiring;
    }
    bool triggerSoftware() override {
        recordCall();
        int triggerNumber = 0;
        bool shouldEmit = false;
        bool succeeds = false;
        {
            QMutexLocker locker(&m_state->mutex);
            ++m_state->triggerCalls;
            triggerNumber = m_state->triggerCalls;
            m_state->triggerEntered = true;
            m_state->triggerGate.wakeAll();
            while (m_state->blockTrigger && !m_state->releaseTrigger)
                m_state->triggerGate.wait(&m_state->mutex);
            shouldEmit = m_state->emitFrame;
            succeeds = m_state->triggerSucceeds;
        }
        if (shouldEmit) {
            QImage frame(4, 3, QImage::Format_RGB32);
            frame.fill(qRgb(20 + triggerNumber, 80 + triggerNumber, 160 + triggerNumber));
            {
                QMutexLocker locker(&m_state->mutex);
                m_state->image = frame;
            }
            emit imageReceived(frame, {});
        }
        return succeeds;
    }
    void setTriggerMode(TriggerMode mode) override {
        recordCall();
        m_triggerMode = mode;
    }
    TriggerMode triggerMode() const override {
        recordCall();
        return m_triggerMode;
    }
    void setExposureTime(double value) override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        ++m_state->exposureSetCalls;
        m_state->exposure = value;
    }
    double exposureTime() const override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        return m_state->exposure;
    }
    void setGain(double value) override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        ++m_state->gainSetCalls;
        m_state->gain = value;
        if (m_state->throwOnGainSet)
            throw std::runtime_error("fixture gain write uncertainty");
        m_state->gain = m_state->clampGain ? value + 1.0 : value;
    }
    double gain() const override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        return m_state->gain;
    }
    void setFrameRate(double) override {
        recordCall();
    }
    double frameRate() const override {
        recordCall();
        return 30.0;
    }
    void setRoi(int, int, int, int) override {
        recordCall();
    }
    QRect roi() const override {
        recordCall();
        return QRect(0, 0, 4, 3);
    }
    QImage lastImage() const override {
        recordCall();
        QMutexLocker locker(&m_state->mutex);
        return m_state->image;
    }
    QVariantMap lastImageMetadata() const override {
        recordCall();
        return {};
    }
    int imageWidth() const override {
        recordCall();
        return 4;
    }
    int imageHeight() const override {
        recordCall();
        return 3;
    }
    QWidget* createConfigWidget() override {
        recordCall();
        return nullptr;
    }
    QJsonObject toJson() const override {
        recordCall();
        return {};
    }
    bool fromJson(const QJsonObject&) override {
        recordCall();
        return true;
    }

private:
    void recordCall() const {
        if (QThread::currentThread() == thread())
            return;
        QMutexLocker locker(&m_state->mutex);
        ++m_state->wrongThreadCalls;
    }

    FakeCameraState* m_state;
    bool m_connected = false;
    bool m_acquiring = false;
    TriggerMode m_triggerMode = TriggerMode::Software;
};

class FakeDiscoveryJob final : public CameraDiscoveryJob {
    Q_OBJECT
public:
    FakeDiscoveryJob(quint64 requestId, QJsonObject result, QObject* parent)
        : CameraDiscoveryJob(parent), m_requestId(requestId), m_result(std::move(result)) {
        QTimer::singleShot(
            0, this, [this]() { completeOnce(m_requestId, QJsonDocument(m_result).toJson(QJsonDocument::Compact)); });
    }

private:
    quint64 m_requestId;
    QJsonObject m_result;
};

class FakeAsyncCameraPlugin final : public ICameraPlugin, public ICameraDiscoveryAsync {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICameraPlugin DeepLux::ICameraDiscoveryAsync)

public:
    explicit FakeAsyncCameraPlugin(FakeCameraState* state) : m_state(state) {}
    QString pluginId() const override {
        QMutexLocker locker(&m_state->mutex);
        return m_state->pluginId;
    }
    QString pluginName() const override {
        return QStringLiteral("G5 agent lifecycle fake");
    }
    QString manufacturer() const override {
        return QStringLiteral("DeepLux test fixture");
    }
    bool isAvailable() const override {
        return true;
    }
    QString availabilityMessage() const override {
        return {};
    }
    QList<CameraInfo> discoverCameras() override {
        return {};
    }
    QObject* createCamera(const CameraInfo&) override {
        QMutexLocker locker(&m_state->mutex);
        ++m_state->factoryCalls;
        return new FakeAsyncCamera(m_state);
    }
    CameraDiscoveryJob* startDiscovery(quint64 requestId, QString& errorCode) override {
        QString discoveredName;
        QString discoveredSerial;
        QString discoveredIpAddress;
        QString discoveredPluginId;
        bool discoveryEnabled = false;
        {
            QMutexLocker locker(&m_state->mutex);
            ++m_state->discoveryCalls;
            discoveredName = m_state->discoveredName;
            discoveredSerial = m_state->discoveredSerial;
            discoveredIpAddress = m_state->discoveredIpAddress;
            discoveredPluginId = m_state->pluginId;
            discoveryEnabled = m_state->discoveryEnabled;
        }
        errorCode.clear();
        const QString deviceId = [&]() {
            QMutexLocker locker(&m_state->mutex);
            return m_state->deviceId;
        }();
        const QJsonObject camera{{QStringLiteral("pluginId"), discoveredPluginId},
                                 {QStringLiteral("deviceId"), deviceId},
                                 {QStringLiteral("name"), discoveredName},
                                 {QStringLiteral("serialNumber"), discoveredSerial},
                                 {QStringLiteral("ipAddress"), discoveredIpAddress},
                                 {QStringLiteral("width"), 4},
                                 {QStringLiteral("height"), 3},
                                 {QStringLiteral("pixelFormat"), QStringLiteral("RGB32")},
                                 {QStringLiteral("isConnected"), false},
                                 {QStringLiteral("rawInfo"), QJsonObject()}};
        const QJsonArray cameras = discoveryEnabled ? QJsonArray{camera} : QJsonArray{};
        return new FakeDiscoveryJob(requestId,
                                    {{QStringLiteral("schemaVersion"), 1},
                                     {QStringLiteral("requestId"), QString::number(requestId)},
                                     {QStringLiteral("status"), QStringLiteral("success")},
                                     {QStringLiteral("cameras"), cameras}},
                                    this);
    }
    void requestCancel(quint64) override {}

private:
    FakeCameraState* m_state;
};

class FakeLegacyCameraPlugin final : public ICameraPlugin {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICameraPlugin)

public:
    explicit FakeLegacyCameraPlugin(FakeCameraState* state) : m_state(state) {}

    QString pluginId() const override {
        return QString::fromLatin1(kLegacyPluginId);
    }
    QString pluginName() const override {
        return QStringLiteral("G5 legacy fake camera");
    }
    QString manufacturer() const override {
        return QStringLiteral("DeepLux test fixture");
    }
    bool isAvailable() const override {
        return true;
    }
    QString availabilityMessage() const override {
        return {};
    }
    QList<CameraInfo> discoverCameras() override {
        return {CameraInfo{QString::fromLatin1(kLegacyPluginId),
                           QString::fromLatin1(kLegacyDeviceId),
                           QStringLiteral("G5 legacy fake camera"),
                           QStringLiteral("legacy-serial"),
                           {},
                           4,
                           3,
                           QStringLiteral("RGB32"),
                           {},
                           false}};
    }
    QObject* createCamera(const CameraInfo&) override {
        QMutexLocker locker(&m_state->mutex);
        ++m_state->factoryCalls;
        m_state->deviceId = QString::fromLatin1(kLegacyDeviceId);
        return new FakeAsyncCamera(m_state);
    }

private:
    FakeCameraState* m_state;
};

class CameraConfirmationScriptedLLM final : public ILLMClient {
    Q_OBJECT

public:
    using ILLMClient::ILLMClient;

    void setApiKey(const QString&) override {}
    void setEndpoint(const QString&) override {}
    void setModel(const QString&) override {}
    void setTemperature(double) override {}
    void setMaxTokens(int) override {}
    void setToolsEnabled(bool) override {}
    void sendRequest(const AgentConversation& context, const QList<ToolDefinition>&) override {
        lastContext = context;
        const int turn = requestCount++;
        QTimer::singleShot(0, this, [this, turn, requestId = context.requestId]() {
            AgentResponse response;
            response.requestId = requestId;
            response.success = true;
            if (turn < toolCallTurns.size()) {
                response.content = QStringLiteral("camera confirmation test");
                response.toolCalls = toolCallTurns.at(turn);
            } else {
                response.content = QStringLiteral("finished");
            }
            emit responseReceived(response);
        });
    }

    QList<QJsonArray> toolCallTurns;
    int requestCount = 0;
    AgentConversation lastContext;
};

class ScopedControllerLLM {
public:
    ScopedControllerLLM(AgentController& controller, ILLMClient* replacement)
        : m_controller(controller), m_previous(controller.llmClient()) {
        m_controller.setLLMClient(replacement);
    }
    ~ScopedControllerLLM() {
        m_controller.setLLMClient(m_previous);
    }
    ScopedControllerLLM(const ScopedControllerLLM&) = delete;
    ScopedControllerLLM& operator=(const ScopedControllerLLM&) = delete;

private:
    AgentController& m_controller;
    ILLMClient* m_previous;
};

} // namespace

class TestAgentCameraLifecycleG5 final : public QObject {
    Q_OBJECT

private slots:
    void init() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
    }

    void testAgentAsyncSessionFeedsGrabImage() {
        QTemporaryDir appData;
        QVERIFY(appData.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", appData.path().toLocal8Bit());

        const QString grabImageDirectory = QDir(appData.path()).filePath(QStringLiteral("plugins/GrabImage"));
        QVERIFY(QDir().mkpath(grabImageDirectory));
        QVERIFY(QFile::copy(QStringLiteral(AGENT_CAMERA_G5_GRABIMAGE_METADATA),
                            QDir(grabImageDirectory).filePath(QStringLiteral("metadata.json"))));
        QVERIFY(QFile::copy(QStringLiteral(AGENT_CAMERA_G5_GRABIMAGE_LIBRARY),
                            QDir(grabImageDirectory)
                                .filePath(QFileInfo(QStringLiteral(AGENT_CAMERA_G5_GRABIMAGE_LIBRARY)).fileName())));
        const QString delayDirectory = QDir(appData.path()).filePath(QStringLiteral("plugins/Delay"));
        QVERIFY(QDir().mkpath(delayDirectory));
        QVERIFY(QFile::copy(QStringLiteral(AGENT_CAMERA_G5_DELAY_METADATA),
                            QDir(delayDirectory).filePath(QStringLiteral("metadata.json"))));
        QVERIFY(QFile::copy(
            QStringLiteral(AGENT_CAMERA_G5_DELAY_LIBRARY),
            QDir(delayDirectory).filePath(QFileInfo(QStringLiteral(AGENT_CAMERA_G5_DELAY_LIBRARY)).fileName())));
        PluginManager& modulePlugins = PluginManager::instance();
        QVERIFY(modulePlugins.initialize());
        QVERIFY(modulePlugins.loadPlugin(QStringLiteral("GrabImage")));
        QVERIFY(modulePlugins.loadPlugin(QStringLiteral("延时")));

        FakeCameraState state;
        auto* plugin = new FakeAsyncCameraPlugin(&state);
        CameraManager& manager = CameraManager::instance();
        manager.registerCameraPlugin(plugin);
        QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 2000);

        AgentController& controller = AgentController::instance();
        QVERIFY(controller.initialize());
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        controller.clearConversation();
        AgentActor* actor = controller.actor();
        QVERIFY(actor);

        ProjectManager::instance().closeProject();
        Project* implicitCameraProject = ProjectManager::instance().newProject();
        QVERIFY(implicitCameraProject);
        ModuleInstance implicitGrab;
        implicitGrab.id = QStringLiteral("g5-implicit-grab");
        implicitGrab.moduleId = QStringLiteral("GrabImage");
        implicitGrab.params = {{QStringLiteral("grabSource"), QStringLiteral("Camera")}};
        implicitCameraProject->addModule(implicitGrab);
        QSignalSpy implicitModuleStarted(&RunEngine::instance(), &RunEngine::moduleStarted);
        QSignalSpy implicitToolExecuted(actor, &AgentActor::toolExecuted);
        QSignalSpy implicitAudit(&controller, &AgentController::actionLogEntryAdded);
        int discoveryCallsBeforeImplicitReject = 0;
        int connectCallsBeforeImplicitReject = 0;
        {
            QMutexLocker locker(&state.mutex);
            discoveryCallsBeforeImplicitReject = state.discoveryCalls;
            connectCallsBeforeImplicitReject = state.connectCalls;
            QCOMPARE(connectCallsBeforeImplicitReject, 0);
        }
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(manager.cameraSessionCount(), 0);
        for (const QString& executionTool :
             {QStringLiteral("run_flow"), QStringLiteral("step_once"), QStringLiteral("resume_flow")}) {
            const QJsonObject implicitRejected =
                controller.handleToolCall(executionTool, {{QStringLiteral("mode"), QStringLiteral("once")}});
            QCOMPARE(implicitRejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
            QVERIFY(implicitRejected.value(QStringLiteral("error")).toString().contains(QStringLiteral("cameraId")));
            QVERIFY(implicitRejected.value(QStringLiteral("status")).toString() !=
                    QStringLiteral("pending_confirmation"));
            QVERIFY(!RunEngine::instance().isBusy());
            QVERIFY(!RunEngine::instance().isPaused());
        }
        QCOMPARE(implicitToolExecuted.count(), 0);
        QCOMPARE(implicitModuleStarted.count(), 0);
        QCOMPARE(manager.cameraSessionCount(), 0);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.discoveryCalls, discoveryCallsBeforeImplicitReject);
            QCOMPARE(state.connectCalls, connectCallsBeforeImplicitReject);
            QCOMPARE(state.triggerCalls, 0);
            QCOMPARE(state.startCalls, 0);
        }
        QCOMPARE(implicitAudit.count(), 3);
        for (const QList<QVariant>& args : implicitAudit)
            QCOMPARE(args.at(0).value<AgentActionLogEntry>().result, QStringLiteral("error"));
        ProjectManager::instance().closeProject();

        const QJsonObject targets = actor->executeTool(QStringLiteral("get_camera_discovery_targets"), {});
        QCOMPARE(targets.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
        const QJsonObject discoveryArgs{
            {QStringLiteral("registryGeneration"), targets.value(QStringLiteral("registryGeneration"))},
            {QStringLiteral("pluginIds"), targets.value(QStringLiteral("asyncPluginIds"))},
            {QStringLiteral("unsupportedPluginIds"), targets.value(QStringLiteral("unsupportedPluginIds"))}};
        QCOMPARE(targets.value(QStringLiteral("asyncPluginIds")).toArray(), QJsonArray{QString::fromLatin1(kPluginId)});
        const QList<CameraStatus> known = manager.knownCameras();
        QVERIFY(std::any_of(known.cbegin(), known.cend(),
                            [](const CameraStatus& s) { return s.deviceId == QLatin1String(kDeviceId); }));
        QStringList knownDeviceIdsBefore;
        for (const CameraStatus& camera : known)
            knownDeviceIdsBefore.append(camera.deviceId);
        knownDeviceIdsBefore.sort();

        int initialDiscoveryCalls = 0;
        {
            QMutexLocker locker(&state.mutex);
            initialDiscoveryCalls = state.discoveryCalls;
            QCOMPARE(initialDiscoveryCalls, 1); // registerCameraPlugin performs one initial async scan
        }
        QSignalSpy discoveryBatchSettled(&manager, &CameraManager::cameraDiscoveryBatchSettled);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QJsonObject rejectedDiscovery =
            controller.handleToolCall(QStringLiteral("discover_cameras"), discoveryArgs);
        QCOMPARE(rejectedDiscovery.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 QJsonArray{QStringLiteral("camera_discovery:%1").arg(QString::fromLatin1(kPluginId))});
        QCOMPARE(manager.pendingDiscoveryCount(), 0);
        QCOMPARE(discoveryBatchSettled.count(), 0);
        QStringList knownDeviceIdsWhilePending;
        for (const CameraStatus& camera : manager.knownCameras())
            knownDeviceIdsWhilePending.append(camera.deviceId);
        knownDeviceIdsWhilePending.sort();
        QCOMPARE(knownDeviceIdsWhilePending, knownDeviceIdsBefore);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.discoveryCalls, initialDiscoveryCalls);
        }
        const QString rejectedDiscoveryConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!rejectedDiscoveryConfirmationId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedDiscoveryConfirmationId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("discover_cameras"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(discoveryBatchSettled.count(), 0);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.discoveryCalls, initialDiscoveryCalls);
        }
        QStringList knownDeviceIdsAfterReject;
        for (const CameraStatus& camera : manager.knownCameras())
            knownDeviceIdsAfterReject.append(camera.deviceId);
        knownDeviceIdsAfterReject.sort();
        QCOMPARE(knownDeviceIdsAfterReject, knownDeviceIdsBefore);

        const QJsonObject approvedDiscovery =
            controller.handleToolCall(QStringLiteral("discover_cameras"), discoveryArgs);
        QCOMPARE(approvedDiscovery.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedDiscoveryConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!approvedDiscoveryConfirmationId.isEmpty());
        QVERIFY(approvedDiscoveryConfirmationId != rejectedDiscoveryConfirmationId);
        QVERIFY(controller.confirmPendingTools(approvedDiscoveryConfirmationId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.size() > 0 &&
                                     audit.last().at(0).value<AgentActionLogEntry>().result == QLatin1String("pending"),
                                 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("discover_cameras"));
        QTRY_COMPARE_WITH_TIMEOUT(discoveryBatchSettled.count(), 1, 2000);
        const quint64 approvedDiscoveryRequestId = discoveryBatchSettled.last().at(0).toULongLong();
        QVERIFY(approvedDiscoveryRequestId > 0);
        QCOMPARE(discoveryBatchSettled.last().at(1).toString(), QStringLiteral("success"));
        QCOMPARE(discoveryBatchSettled.last().at(2).toString(), QString());
        QJsonObject discoveryResult;
        QTRY_VERIFY_WITH_TIMEOUT((discoveryResult = actor->executeTool(
                                      QStringLiteral("get_camera_discovery_operation"),
                                      {{QStringLiteral("requestId"), QString::number(approvedDiscoveryRequestId)}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QStringLiteral("success"),
                                 2000);
        QCOMPARE(discoveryResult.value(QStringLiteral("requestId")).toString(),
                 QString::number(approvedDiscoveryRequestId));
        QCOMPARE(discoveryResult.value(QStringLiteral("cameras")).toArray().size(), 1);
        QCOMPARE(discoveryResult.value(QStringLiteral("cameras"))
                     .toArray()
                     .first()
                     .toObject()
                     .value(QStringLiteral("deviceId"))
                     .toString(),
                 QString::fromLatin1(kDeviceId));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.discoveryCalls, initialDiscoveryCalls + 1);
        }
        QStringList knownDeviceIdsAfterApprove;
        for (const CameraStatus& camera : manager.knownCameras())
            knownDeviceIdsAfterApprove.append(camera.deviceId);
        knownDeviceIdsAfterApprove.sort();
        QCOMPARE(knownDeviceIdsAfterApprove.size(), 1);
        QCOMPARE(knownDeviceIdsAfterApprove.first(), QString::fromLatin1(kDeviceId));

        QSignalSpy connectStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject connectPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(connectPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray connectEffects = controller.pendingConfirmationPayload()
                                              .value(QStringLiteral("context"))
                                              .toObject()
                                              .value(QStringLiteral("sideEffects"))
                                              .toArray();
        QVERIFY(connectEffects.first().toString().startsWith(
            QStringLiteral("camera_connect:%1 owner=%2")
                .arg(QString::fromLatin1(kDeviceId), QString::fromLatin1(kPluginId))));
        QVERIFY(connectEffects.first().toString().contains(QStringLiteral("serial=g5-fake-serial")));
        QVERIFY(connectEffects.first().toString().contains(QStringLiteral("state=disconnected")));
        QVERIFY(connectEffects.first().toString().contains(QStringLiteral("generation=")));
        QCOMPARE(manager.cameraSessionCount(), 0);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, 0);
        }
        const QString rejectedConnectConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!rejectedConnectConfirmationId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedConnectConfirmationId));
        QCOMPARE(manager.cameraSessionCount(), 0);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("connect_camera"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, 0);
        }

        const QJsonObject originalCameraSnapshot = manager.cameraLifecycleSnapshot(QString::fromLatin1(kDeviceId));
        const QJsonObject staleIdentityPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(staleIdentityPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        const QString staleIdentityId = controller.pendingConfirmationId();
        QVERIFY(!staleIdentityId.isEmpty());
        int connectToolExecuted = 0;
        QObject connectExecutionContext;
        connect(actor, &AgentActor::toolExecuted, &connectExecutionContext,
                [&connectToolExecuted](const QString& name, const QJsonObject&) {
                    if (name == QLatin1String("connect_camera"))
                        ++connectToolExecuted;
                });
        const int factoryCallsBeforeDrift = [&state]() {
            QMutexLocker locker(&state.mutex);
            return state.factoryCalls;
        }();
        const int connectCallsBeforeDrift = [&state]() {
            QMutexLocker locker(&state.mutex);
            return state.connectCalls;
        }();
        QSignalSpy staleConnectStarted(&manager, &CameraManager::cameraOperationStarted);
        {
            QMutexLocker locker(&state.mutex);
            state.discoveredName = QStringLiteral("Replacement fake camera");
            state.discoveredSerial = QStringLiteral("g5-replacement-serial");
        }
        QSignalSpy identityRescanSettled(&manager, &CameraManager::cameraDiscoverySettled);
        const quint64 identityRescanRequestId = 70001;
        QString identityRescanError;
        QVERIFY2(
            manager.startDiscoveryAsync(QString::fromLatin1(kPluginId), identityRescanRequestId, &identityRescanError),
            qPrintable(identityRescanError));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(identityRescanSettled.cbegin(), identityRescanSettled.cend(),
                                             [identityRescanRequestId](const QList<QVariant>& args) {
                                                 return args.at(1).toULongLong() == identityRescanRequestId &&
                                                        args.at(2).toString() == QLatin1String("success");
                                             }),
                                 2000);
        const QJsonObject changedCameraSnapshot = manager.cameraLifecycleSnapshot(QString::fromLatin1(kDeviceId));
        QCOMPARE(changedCameraSnapshot.value(QStringLiteral("serialNumber")).toString(),
                 QStringLiteral("g5-replacement-serial"));
        quint64 staleManagerRequestId = 999;
        QString staleManagerError;
        QVERIFY(!manager.requestConnectCameraAsync(QString::fromLatin1(kDeviceId), originalCameraSnapshot,
                                                   &staleManagerRequestId, &staleManagerError));
        QCOMPARE(staleManagerError, QStringLiteral("CAMERA_TARGET_CHANGED"));
        QCOMPARE(staleManagerRequestId, quint64(0));
        QVERIFY(controller.confirmPendingTools(staleIdentityId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(connectToolExecuted, 0);
        QCOMPARE(staleConnectStarted.count(), 0);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.factoryCalls, factoryCallsBeforeDrift);
            QCOMPARE(state.connectCalls, connectCallsBeforeDrift);
            state.discoveredName = QStringLiteral("Agent lifecycle fake camera");
            state.discoveredSerial = QStringLiteral("g5-fake-serial");
        }
        QSignalSpy identityRestoreSettled(&manager, &CameraManager::cameraDiscoverySettled);
        const quint64 identityRestoreRequestId = 70002;
        QString identityRestoreError;
        QVERIFY2(manager.startDiscoveryAsync(QString::fromLatin1(kPluginId), identityRestoreRequestId,
                                             &identityRestoreError),
                 qPrintable(identityRestoreError));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(identityRestoreSettled.cbegin(), identityRestoreSettled.cend(),
                                             [identityRestoreRequestId](const QList<QVariant>& args) {
                                                 return args.at(1).toULongLong() == identityRestoreRequestId &&
                                                        args.at(2).toString() == QLatin1String("success");
                                             }),
                                 2000);
        QCOMPARE(manager.cameraLifecycleSnapshot(QString::fromLatin1(kDeviceId)), originalCameraSnapshot);

        const QJsonObject staleAddressPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(staleAddressPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        const QString staleAddressId = controller.pendingConfirmationId();
        QVERIFY(!staleAddressId.isEmpty());
        {
            QMutexLocker locker(&state.mutex);
            state.discoveredIpAddress = QStringLiteral("192.0.2.11");
        }
        QSignalSpy addressRescanSettled(&manager, &CameraManager::cameraDiscoverySettled);
        const quint64 addressRescanRequestId = 70003;
        QString addressRescanError;
        QVERIFY2(
            manager.startDiscoveryAsync(QString::fromLatin1(kPluginId), addressRescanRequestId, &addressRescanError),
            qPrintable(addressRescanError));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(addressRescanSettled.cbegin(), addressRescanSettled.cend(),
                                             [addressRescanRequestId](const QList<QVariant>& args) {
                                                 return args.at(1).toULongLong() == addressRescanRequestId &&
                                                        args.at(2).toString() == QLatin1String("success");
                                             }),
                                 2000);
        const QJsonObject changedAddressSnapshot = manager.cameraLifecycleSnapshot(QString::fromLatin1(kDeviceId));
        QCOMPARE(changedAddressSnapshot.value(QStringLiteral("ipAddress")).toString(), QStringLiteral("192.0.2.11"));
        QJsonObject oldAddressFields = changedAddressSnapshot;
        oldAddressFields.remove(QStringLiteral("ipAddress"));
        QJsonObject originalAddressFields = originalCameraSnapshot;
        originalAddressFields.remove(QStringLiteral("ipAddress"));
        QCOMPARE(oldAddressFields, originalAddressFields);
        quint64 staleAddressRequestId = 999;
        QString staleAddressError;
        QVERIFY(!manager.requestConnectCameraAsync(QString::fromLatin1(kDeviceId), originalCameraSnapshot,
                                                   &staleAddressRequestId, &staleAddressError));
        QCOMPARE(staleAddressError, QStringLiteral("CAMERA_TARGET_CHANGED"));
        QCOMPARE(staleAddressRequestId, quint64(0));
        QVERIFY(controller.confirmPendingTools(staleAddressId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(staleConnectStarted.count(), 0);
        QCOMPARE(connectToolExecuted, 0);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.factoryCalls, factoryCallsBeforeDrift);
            QCOMPARE(state.connectCalls, connectCallsBeforeDrift);
            state.discoveredIpAddress = QStringLiteral("192.0.2.10");
        }
        QSignalSpy addressRestoreSettled(&manager, &CameraManager::cameraDiscoverySettled);
        const quint64 addressRestoreRequestId = 70004;
        QString addressRestoreError;
        QVERIFY2(
            manager.startDiscoveryAsync(QString::fromLatin1(kPluginId), addressRestoreRequestId, &addressRestoreError),
            qPrintable(addressRestoreError));
        QTRY_VERIFY_WITH_TIMEOUT(std::any_of(addressRestoreSettled.cbegin(), addressRestoreSettled.cend(),
                                             [addressRestoreRequestId](const QList<QVariant>& args) {
                                                 return args.at(1).toULongLong() == addressRestoreRequestId &&
                                                        args.at(2).toString() == QLatin1String("success");
                                             }),
                                 2000);
        QCOMPARE(manager.cameraLifecycleSnapshot(QString::fromLatin1(kDeviceId)), originalCameraSnapshot);

        const int auditCountBeforeConnect = audit.count();
        const QJsonObject approvedConnect = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(approvedConnect.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString connectConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!connectConfirmationId.isEmpty());
        QVERIFY(connectConfirmationId != rejectedConnectConfirmationId);
        QVERIFY(controller.confirmPendingTools(connectConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(audit.count(), auditCountBeforeConnect + 1, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("connect_camera"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("pending"));
        QTRY_COMPARE_WITH_TIMEOUT(connectStarted.count(), 1, 2000);
        const QString confirmedConnectRequestId = QString::number(connectStarted.last().at(1).toULongLong());
        QJsonObject confirmedConnectOperation;
        QTRY_VERIFY_WITH_TIMEOUT(
            (confirmedConnectOperation = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                            {{QStringLiteral("requestId"), confirmedConnectRequestId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, 1);
        }

        const QJsonObject detailsRequest = actor->executeTool(
            QStringLiteral("get_camera_details"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(detailsRequest.value(QStringLiteral("status")).toString(), QStringLiteral("pending"));
        const QString detailsId = detailsRequest.value(QStringLiteral("requestId")).toString();
        QJsonObject detailsOperation;
        QTRY_VERIFY_WITH_TIMEOUT((detailsOperation = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                                        {{QStringLiteral("requestId"), detailsId}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QStringLiteral("success"),
                                 2000);
        const QJsonObject details = detailsOperation.value(QStringLiteral("details")).toObject();
        QCOMPARE(details.value(QStringLiteral("serialNumber")).toString(), QStringLiteral("g5-fake-serial"));
        QCOMPARE(details.value(QStringLiteral("cameraSessionGeneration")).toString(),
                 QString::number(manager.cameraParameterBindingVersion(QString::fromLatin1(kDeviceId))));

        const auto agentConnect = [&]() {
            const QJsonObject response = actor->executeTool(
                QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
            if (response.value(QStringLiteral("status")).toString() != QLatin1String("pending"))
                return false;
            const QString requestId = response.value(QStringLiteral("requestId")).toString();
            QElapsedTimer timer;
            timer.start();
            while (timer.elapsed() < 2000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                const QJsonObject result = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                              {{QStringLiteral("requestId"), requestId}});
                const QString status = result.value(QStringLiteral("status")).toString();
                if (status == QLatin1String("success")) {
                    QElapsedTimer settleTimer;
                    settleTimer.start();
                    while (settleTimer.elapsed() < 2000) {
                        const QJsonObject snapshot = manager.cameraLifecycleSnapshot(QString::fromLatin1(kDeviceId));
                        const QString state = snapshot.value(QStringLiteral("state")).toString();
                        if ((state == QLatin1String("connected") || state == QLatin1String("grabbing")) &&
                            snapshot.value(QStringLiteral("hasSession")).toBool() &&
                            manager.hasAsyncCameraSession(QString::fromLatin1(kDeviceId)) &&
                            !snapshot.value(QStringLiteral("operationPending")).toBool())
                            return true;
                        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                        QTest::qWait(5);
                    }
                    return false;
                }
                if (status == QLatin1String("error"))
                    return false;
            }
            return false;
        };

        const QJsonObject parameterRequest{
            {QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)},
            {QStringLiteral("cameraSessionGeneration"), details.value(QStringLiteral("cameraSessionGeneration"))},
            {QStringLiteral("serialNumber"), details.value(QStringLiteral("serialNumber"))},
            {QStringLiteral("parameter"), QStringLiteral("exposureTime")},
            {QStringLiteral("value"), 2500.0}};
        const QJsonObject pendingWrite =
            controller.handleToolCall(QStringLiteral("set_camera_parameter"), parameterRequest);
        QCOMPARE(pendingWrite.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.exposureSetCalls, 0);
        }
        QSignalSpy operationStarted(&manager, &CameraManager::cameraOperationStarted);
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(operationStarted.count(), 1, 2000);
        const QString writeId = QString::number(operationStarted.first().at(1).toULongLong());
        QJsonObject writeResult;
        QTRY_VERIFY_WITH_TIMEOUT((writeResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                                   {{QStringLiteral("requestId"), writeId}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QStringLiteral("success"),
                                 2000);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.exposureSetCalls, 1);
            QCOMPARE(state.exposure, 2500.0);
        }

        const quint64 generationBeforeUncertainWrite =
            manager.cameraParameterBindingVersion(QString::fromLatin1(kDeviceId));
        const int gainSetCallsBeforeUncertainWrite = [&]() {
            QMutexLocker locker(&state.mutex);
            state.throwOnGainSet = true;
            return state.gainSetCalls;
        }();
        QJsonObject uncertainParameterRequest{
            {QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)},
            {QStringLiteral("cameraSessionGeneration"), QString::number(generationBeforeUncertainWrite)},
            {QStringLiteral("serialNumber"), details.value(QStringLiteral("serialNumber"))},
            {QStringLiteral("parameter"), QStringLiteral("gain")},
            {QStringLiteral("value"), 3.0}};
        const QJsonObject uncertainPending =
            controller.handleToolCall(QStringLiteral("set_camera_parameter"), uncertainParameterRequest);
        QCOMPARE(uncertainPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QCOMPARE(controller.pendingConfirmationPayload()
                     .value(QStringLiteral("context"))
                     .toObject()
                     .value(QStringLiteral("sideEffects"))
                     .toArray(),
                 QJsonArray{QStringLiteral("camera_parameter:%1:gain=3").arg(QString::fromLatin1(kDeviceId))});
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.gainSetCalls, gainSetCallsBeforeUncertainWrite);
        }
        const int operationsBeforeUncertainWrite = operationStarted.count();
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(operationStarted.count(), operationsBeforeUncertainWrite + 1, 2000);
        const QString uncertainWriteId = QString::number(operationStarted.last().at(1).toULongLong());
        QJsonObject uncertainWriteResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (uncertainWriteResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                       {{QStringLiteral("requestId"), uncertainWriteId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("error"),
            2000);
        QCOMPARE(uncertainWriteResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_PARAMETER_WRITE_UNCONFIRMED"));
        QCOMPARE(uncertainWriteResult.value(QStringLiteral("sideEffectStatus")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(uncertainWriteResult.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(uncertainWriteResult.value(QStringLiteral("before")).toDouble(), 1.0);
        QCOMPARE(uncertainWriteResult.value(QStringLiteral("requested")).toDouble(), 3.0);
        QCOMPARE(uncertainWriteResult.value(QStringLiteral("actual")).toDouble(), 3.0);
        const quint64 generationAfterUncertainWrite =
            manager.cameraParameterBindingVersion(QString::fromLatin1(kDeviceId));
        QVERIFY(generationAfterUncertainWrite > generationBeforeUncertainWrite);
        for (int poll = 0; poll < 3; ++poll) {
            const QJsonObject repeated = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                            {{QStringLiteral("requestId"), uncertainWriteId}});
            QCOMPARE(repeated, uncertainWriteResult);
        }
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.gainSetCalls, gainSetCallsBeforeUncertainWrite + 1);
            QCOMPARE(state.gain, 3.0);
            state.throwOnGainSet = false;
        }
        QCOMPARE(operationStarted.count(), operationsBeforeUncertainWrite + 1);

        QSignalSpy staleRetryExecuted(actor, &AgentActor::toolExecuted);
        const QJsonObject staleRetryPending =
            controller.handleToolCall(QStringLiteral("set_camera_parameter"), uncertainParameterRequest);
        QCOMPARE(staleRetryPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(staleRetryExecuted.count(), 1, 2000);
        const QJsonObject staleRetryResult = staleRetryExecuted.last().at(1).toJsonObject();
        QCOMPARE(staleRetryResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_TARGET_CHANGED"));
        QCOMPARE(operationStarted.count(), operationsBeforeUncertainWrite + 1);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.gainSetCalls, gainSetCallsBeforeUncertainWrite + 1);
        }

        const CameraCaptureResult noDevice =
            manager.captureAsyncSessionImage(QStringLiteral("missing-camera"), 3000.0, 2.0, 100);
        QCOMPARE(noDevice.errorCode, QStringLiteral("CAMERA_NOT_FOUND"));

        GrabImagePlugin grab;
        QVERIFY(grab.initialize());
        grab.setParams(QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Camera")},
                                   {QStringLiteral("cameraId"), QString::fromLatin1(kDeviceId)},
                                   {QStringLiteral("exposureTime"), 3000.0},
                                   {QStringLiteral("gain"), 2.0},
                                   {QStringLiteral("grabTimeout"), 1000}});
        ImageData output;
        std::atomic<bool> workerDone{false};
        bool workerSuccess = false;
        std::thread worker([&]() {
            workerSuccess = grab.execute(ImageData(), output);
            workerDone.store(true, std::memory_order_release);
        });
        QTRY_VERIFY_WITH_TIMEOUT(workerDone.load(std::memory_order_acquire), 2000);
        worker.join();
        QVERIFY(workerSuccess);
        const QImage actual = output.toQImage();
        QCOMPARE(actual.size(), QSize(4, 3));
        for (int y = 0; y < actual.height(); ++y)
            for (int x = 0; x < actual.width(); ++x)
                QCOMPARE(actual.pixel(x, y), qRgb(21, 81, 161));
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 2000);

        const CameraCaptureResult disconnected =
            manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3000.0, 2.0, 100);
        QCOMPARE(disconnected.errorCode, QStringLiteral("CAMERA_NOT_CONNECTED"));
        QVERIFY(agentConnect());
        const int settersBeforeValidationFailure = [&]() {
            QMutexLocker locker(&state.mutex);
            state.throwOnValidate = true;
            return state.exposureSetCalls + state.gainSetCalls;
        }();
        const CameraCaptureResult validationFailure =
            manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3000.0, 2.0, 500);
        QCOMPARE(validationFailure.errorCode, QStringLiteral("CAMERA_PARAMETER_VALIDATE_FAILED"));
        QCOMPARE(validationFailure.diagnostics.value(QStringLiteral("failedParameter")).toString(),
                 QStringLiteral("exposureTime"));
        QCOMPARE(validationFailure.diagnostics.value(QStringLiteral("sideEffectStatus")).toString(),
                 QStringLiteral("none"));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.exposureSetCalls + state.gainSetCalls, settersBeforeValidationFailure);
            QCOMPARE(state.triggerCalls, 1);
            state.throwOnValidate = false;
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);

        QVERIFY(agentConnect());
        const int settersBeforeRangeFailure = [&]() {
            QMutexLocker locker(&state.mutex);
            return state.exposureSetCalls + state.gainSetCalls;
        }();
        const CameraCaptureResult rangeFailure =
            manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3000.0, 99.0, 500);
        QCOMPARE(rangeFailure.errorCode, QStringLiteral("CAMERA_PARAMETER_OUT_OF_RANGE"));
        QVERIFY(rangeFailure.diagnostics.value(QStringLiteral("completedParameters")).toArray().isEmpty());
        QCOMPARE(rangeFailure.diagnostics.value(QStringLiteral("failedParameter")).toString(), QStringLiteral("gain"));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.exposureSetCalls + state.gainSetCalls, settersBeforeRangeFailure);
            QCOMPARE(state.triggerCalls, 1);
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);

        QVERIFY(agentConnect());
        {
            QMutexLocker locker(&state.mutex);
            state.throwOnGainSet = true;
        }
        const CameraCaptureResult writeUnconfirmed =
            manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3100.0, 3.0, 500);
        QCOMPARE(writeUnconfirmed.errorCode, QStringLiteral("CAMERA_PARAMETER_WRITE_UNCONFIRMED"));
        QCOMPARE(writeUnconfirmed.diagnostics.value(QStringLiteral("completedParameters")).toArray().size(), 1);
        QCOMPARE(writeUnconfirmed.diagnostics.value(QStringLiteral("failedParameter")).toString(),
                 QStringLiteral("gain"));
        {
            QMutexLocker locker(&state.mutex);
            QVERIFY(state.triggerCalls == 1);
            state.throwOnGainSet = false;
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);

        QVERIFY(agentConnect());
        {
            QMutexLocker locker(&state.mutex);
            state.clampGain = true;
        }
        const CameraCaptureResult readbackMismatch =
            manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3200.0, 3.0, 500);
        QCOMPARE(readbackMismatch.errorCode, QStringLiteral("CAMERA_PARAMETER_READBACK_MISMATCH"));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.triggerCalls, 1);
            state.clampGain = false;
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);

        QVERIFY(agentConnect());
        {
            QMutexLocker locker(&state.mutex);
            state.triggerSucceeds = false;
        }
        const CameraCaptureResult triggerFailure =
            manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3300.0, 3.0, 500);
        QCOMPARE(triggerFailure.errorCode, QStringLiteral("CAMERA_TRIGGER_FAILED"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        {
            QMutexLocker locker(&state.mutex);
            state.triggerSucceeds = true;
            state.emitFrame = false;
        }
        QVERIFY(agentConnect());
        const CameraCaptureResult frameTimeout =
            manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3400.0, 3.0, 80);
        QCOMPARE(frameTimeout.errorCode, QStringLiteral("CAMERA_GRAB_TIMEOUT"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        {
            QMutexLocker locker(&state.mutex);
            state.emitFrame = true;
        }

        QVERIFY(agentConnect());
        {
            QMutexLocker locker(&state.mutex);
            state.emitFrame = false;
            state.triggerEntered = false;
        }
        QMutex errorMutex;
        QStringList grabErrors;
        const QMetaObject::Connection errorConnection = QObject::connect(
            &grab, &ModuleBase::errorOccurred, &grab,
            [&errorMutex, &grabErrors](const QString& error) {
                QMutexLocker locker(&errorMutex);
                grabErrors.append(error);
            },
            Qt::DirectConnection);
        CancellationToken cancellationToken;
        grab.setCancellationToken(&cancellationToken);
        grab.setParam(QStringLiteral("grabTimeout"), 5000);
        std::atomic<bool> cancelWorkerDone{false};
        bool cancelWorkerSuccess = true;
        std::thread cancelWorker([&]() {
            ImageData cancelledOutput;
            cancelWorkerSuccess = grab.execute(ImageData(), cancelledOutput);
            cancelWorkerDone.store(true, std::memory_order_release);
        });
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                QMutexLocker locker(&state.mutex);
                return state.triggerEntered;
            }(),
            2000);
        cancellationToken.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(cancelWorkerDone.load(std::memory_order_acquire), 2000);
        cancelWorker.join();
        QVERIFY(!cancelWorkerSuccess);
        {
            QMutexLocker locker(&errorMutex);
            QVERIFY(std::any_of(grabErrors.cbegin(), grabErrors.cend(), [](const QString& error) {
                return error.contains(QStringLiteral("CAMERA_GRAB_CANCELLED"));
            }));
        }
        grab.setCancellationToken(nullptr);
        grab.setParam(QStringLiteral("grabTimeout"), 1000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        {
            QMutexLocker locker(&state.mutex);
            state.emitFrame = true;
        }

        QVERIFY(agentConnect());
        ProjectManager::instance().closeProject();
        RunEngine& engine = RunEngine::instance();
        const auto drainEngineOrFail = [&engine, &appData](const char* stage) {
            if (!engine.isBusy())
                return;
            engine.stop();
            QElapsedTimer deadline;
            deadline.start();
            while (engine.isBusy() && deadline.elapsed() < 5000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            if (engine.isBusy()) {
                appData.setAutoRemove(false);
                qFatal("RunEngine did not drain within 5000 ms during %s; preserving isolated plugin directory", stage);
            }
        };
        engine.stop();
        engine.clearModules();
        Project* engineProject = ProjectManager::instance().newProject();
        QVERIFY(engineProject);
        ModuleInstance grabInstance;
        grabInstance.id = QStringLiteral("g5-agent-grab");
        grabInstance.moduleId = QStringLiteral("GrabImage");
        grabInstance.name = QStringLiteral("G5 Agent GrabImage");
        grabInstance.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Camera")},
                                          {QStringLiteral("cameraId"), QString::fromLatin1(kDeviceId)},
                                          {QStringLiteral("exposureTime"), 3000.0},
                                          {QStringLiteral("gain"), 2.0},
                                          {QStringLiteral("grabTimeout"), 1000}};
        engineProject->addModule(grabInstance);

        QSignalSpy captureRunFinished(&engine, &RunEngine::runFinished);
        QSignalSpy captureModuleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy captureModuleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy captureCycleStarted(&engine, &RunEngine::cycleStarted);
        QSignalSpy captureCycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy captureToolExecuted(actor, &AgentActor::toolExecuted);
        QSignalSpy captureAudit(&controller, &AgentController::actionLogEntryAdded);
        const auto captureCounters = [&state]() {
            QMutexLocker locker(&state.mutex);
            return std::array<int, 4>{state.triggerCalls, state.startCalls, state.exposureSetCalls, state.gainSetCalls};
        };
        const auto connectionCounters = [&state]() {
            QMutexLocker locker(&state.mutex);
            return std::array<int, 3>{state.connectCalls, state.disconnectCalls, state.destroyed};
        };
        const auto captureRunFlowToolCount = [&captureToolExecuted]() {
            int count = 0;
            for (const QList<QVariant>& args : captureToolExecuted)
                count += args.at(0).toString() == QLatin1String("run_flow") ? 1 : 0;
            return count;
        };

        ModuleInstance secondCameraCapture = grabInstance;
        secondCameraCapture.id = QStringLiteral("g5-agent-grab-second");
        engineProject->addModule(secondCameraCapture);
        const int multiCaptureModulesBefore = captureModuleStarted.count();
        const int multiCaptureToolsBefore = captureToolExecuted.count();
        const QJsonObject multipleCaptures =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(multipleCaptures.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_SESSION_MULTIPLE_CAPTURES_UNSUPPORTED"));
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QCOMPARE(captureModuleStarted.count(), multiCaptureModulesBefore);
        QCOMPARE(captureToolExecuted.count(), multiCaptureToolsBefore);
        engineProject->removeModule(secondCameraCapture.id);

        // Remove the settled session, then prove Agent flow entry points fail closed
        // before confirmation or any Engine/module/tool execution.
        quint64 preflightDisconnectId = 0;
        QString preflightDisconnectError;
        QVERIFY2(manager.requestDisconnectCameraAsync(QString::fromLatin1(kDeviceId), &preflightDisconnectId,
                                                      &preflightDisconnectError),
                 qPrintable(preflightDisconnectError));
        QJsonObject preflightDisconnectResult;
        QTRY_VERIFY_WITH_TIMEOUT((preflightDisconnectResult = actor->executeTool(
                                      QStringLiteral("get_camera_operation"),
                                      {{QStringLiteral("requestId"), QString::number(preflightDisconnectId)}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QLatin1String("success"),
                                 2000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        const int noSessionModulesBefore = captureModuleStarted.count();
        const int noSessionRunsBefore = captureRunFinished.count();
        const int noSessionToolsBefore = captureToolExecuted.count();
        for (const QString& executionTool : {QStringLiteral("run_flow"), QStringLiteral("step_once")}) {
            const QJsonObject noSession = controller.handleToolCall(
                executionTool, executionTool == QLatin1String("run_flow")
                                   ? QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}}
                                   : QJsonObject());
            QCOMPARE(noSession.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("CAMERA_SESSION_REQUIRED"));
            QVERIFY(!noSession.value(QStringLiteral("retryable")).toBool());
            QVERIFY(controller.pendingConfirmationId().isEmpty());
            QVERIFY(!engine.isBusy());
        }
        QCOMPARE(captureModuleStarted.count(), noSessionModulesBefore);
        QCOMPARE(captureRunFinished.count(), noSessionRunsBefore);
        QCOMPARE(captureToolExecuted.count(), noSessionToolsBefore);
        QVERIFY(agentConnect());

        QCOMPARE(QThread::currentThread(), manager.thread());
        QVERIFY(manager.supportsAsyncOperations(QString::fromLatin1(kDeviceId)));
        QCOMPARE(manager.cameraSessionCount(), 1);
        const auto asyncCycleCounters = captureCounters();
        const auto asyncCycleConnections = connectionCounters();
        const int asyncCycleStartedBefore = captureModuleStarted.count();
        const int asyncCycleFinishedBefore = captureModuleFinished.count();
        const int asyncCycleRunsBefore = captureRunFinished.count();
        const int asyncCycleStartedSignalsBefore = captureCycleStarted.count();
        const int asyncCycleStoppedSignalsBefore = captureCycleStopped.count();
        const int asyncCycleToolsBefore = captureRunFlowToolCount();
        const int asyncCycleAllToolsBefore = captureToolExecuted.count();

        for (int attempt = 0; attempt < 2; ++attempt) {
            const QJsonObject unsupported = controller.handleToolCall(
                QStringLiteral("run_flow"),
                {{QStringLiteral("mode"), QStringLiteral("cycle")}, {QStringLiteral("maxCycles"), 2}});
            QCOMPARE(unsupported.value(QStringLiteral("errorCode")).toString(),
                     QStringLiteral("CAMERA_CYCLE_UNSUPPORTED"));
            QVERIFY(!unsupported.value(QStringLiteral("retryable")).toBool());
            QVERIFY(unsupported.value(QStringLiteral("details"))
                        .toObject()
                        .value(QStringLiteral("instanceIds"))
                        .toArray()
                        .contains(grabInstance.id));
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);
            QVERIFY(controller.pendingConfirmationId().isEmpty());
            QCOMPARE(engine.cycleSessionId(), QString());
            QVERIFY(!engine.isBusy());
        }

        QVERIFY(engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"), QString()));
        const QJsonObject implicitAsyncUnsupported =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(implicitAsyncUnsupported.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CYCLE_UNSUPPORTED"));
        QVERIFY(implicitAsyncUnsupported.value(QStringLiteral("details"))
                    .toObject()
                    .value(QStringLiteral("instanceIds"))
                    .toArray()
                    .contains(grabInstance.id));
        QVERIFY(
            engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"), QString::fromLatin1(kDeviceId)));

        engineProject->removeModule(grabInstance.id);
        ModuleInstance defaultSourceGrab = grabInstance;
        defaultSourceGrab.id = QStringLiteral("g5-default-source-grab");
        defaultSourceGrab.params.remove(QStringLiteral("grabSource"));
        engineProject->addModule(defaultSourceGrab);
        const QJsonObject defaultSourceUnsupported =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(defaultSourceUnsupported.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CYCLE_UNSUPPORTED"));
        QVERIFY(defaultSourceUnsupported.value(QStringLiteral("details"))
                    .toObject()
                    .value(QStringLiteral("instanceIds"))
                    .toArray()
                    .contains(defaultSourceGrab.id));
        engineProject->removeModule(defaultSourceGrab.id);
        engineProject->addModule(grabInstance);

        QVERIFY(engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"),
                                              QStringLiteral("unknown-g5-camera")));
        const QJsonObject unknownCameraUnsupported =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(unknownCameraUnsupported.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CYCLE_UNSUPPORTED"));
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QVERIFY(!engine.isBusy());
        QVERIFY(
            engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"), QString::fromLatin1(kDeviceId)));

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        const QJsonObject advisorUnsupported =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(advisorUnsupported.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CYCLE_UNSUPPORTED"));
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        CameraConfirmationScriptedLLM cycleLlm;
        cycleLlm.toolCallTurns = {QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("async_cycle_discovery")},
                        {QStringLiteral("name"), QStringLiteral("discover_cameras")},
                        {QStringLiteral("arguments"), discoveryArgs}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("async_cycle_connect")},
                        {QStringLiteral("name"), QStringLiteral("connect_camera")},
                        {QStringLiteral("arguments"),
                         QJsonObject{{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}}}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("async_cycle_rejected")},
                        {QStringLiteral("name"), QStringLiteral("run_flow")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                  {QStringLiteral("maxCycles"), 2}}}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("async_cycle_read")},
                        {QStringLiteral("name"), QStringLiteral("get_flow_state")},
                        {QStringLiteral("arguments"), QJsonObject()}},
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("async_cycle_edit")},
                {QStringLiteral("name"), QStringLiteral("rename_module")},
                {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("instanceId"), grabInstance.id},
                                                          {QStringLiteral("name"), QStringLiteral("must-not-run")}}}}}};
        QSignalSpy cycleBatchError(&controller, &AgentController::llmErrorOccurred);
        int discoveryCallsBeforeAsyncBatch = 0;
        {
            QMutexLocker locker(&state.mutex);
            discoveryCallsBeforeAsyncBatch = state.discoveryCalls;
        }
        {
            ScopedControllerLLM scopedLlm(controller, &cycleLlm);
            controller.clearConversation();
            controller.sendUserMessage(QStringLiteral("Run the async camera cycle batch"));
            QTRY_COMPARE_WITH_TIMEOUT(cycleBatchError.count(), 1, 2000);
            QVERIFY(!controller.hasPendingTools());
            QCOMPARE(controller.state(), AgentController::AgentState::Idle);
            const QString summary = cycleBatchError.takeFirst().at(0).toString();
            QVERIFY(summary.contains(QStringLiteral("async_cycle_discovery")));
            QVERIFY(summary.contains(QStringLiteral("async_cycle_connect")));
            QVERIFY(summary.contains(QStringLiteral("async_cycle_rejected")));
            QVERIFY(summary.contains(QStringLiteral("async_cycle_read")));
            QVERIFY(summary.contains(QStringLiteral("async_cycle_edit")));
            QVERIFY(summary.contains(QStringLiteral("CAMERA_CYCLE_UNSUPPORTED")));
            QVERIFY(summary.contains(QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR")));

            controller.sendUserMessage(QStringLiteral("Inspect the rejected batch results"));
            int resultCount = 0;
            QHash<QString, QJsonObject> resultsByCallId;
            for (const AgentMessage& message : cycleLlm.lastContext.messages) {
                if (message.role != QLatin1String("tool"))
                    continue;
                if (message.toolCallId.startsWith(QLatin1String("async_cycle_"))) {
                    ++resultCount;
                    QJsonParseError parseError;
                    const QJsonDocument parsed = QJsonDocument::fromJson(message.content.toUtf8(), &parseError);
                    QVERIFY(parseError.error == QJsonParseError::NoError && parsed.isObject());
                    resultsByCallId.insert(message.toolCallId, parsed.object());
                }
            }
            QCOMPARE(resultCount, 5);
            QCOMPARE(resultsByCallId.value(QStringLiteral("async_cycle_discovery"))
                         .value(QStringLiteral("errorCode"))
                         .toString(),
                     QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
            QCOMPARE(resultsByCallId.value(QStringLiteral("async_cycle_connect"))
                         .value(QStringLiteral("errorCode"))
                         .toString(),
                     QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
            QCOMPARE(resultsByCallId.value(QStringLiteral("async_cycle_rejected"))
                         .value(QStringLiteral("errorCode"))
                         .toString(),
                     QStringLiteral("CAMERA_CYCLE_UNSUPPORTED"));
            QCOMPARE(
                resultsByCallId.value(QStringLiteral("async_cycle_read")).value(QStringLiteral("errorCode")).toString(),
                QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
            QCOMPARE(
                resultsByCallId.value(QStringLiteral("async_cycle_edit")).value(QStringLiteral("errorCode")).toString(),
                QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 2000);
        }
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.discoveryCalls, discoveryCallsBeforeAsyncBatch);
        }
        QCOMPARE(manager.pendingDiscoveryCount(), 0);

        QVERIFY(captureCounters() == asyncCycleCounters);
        QVERIFY(connectionCounters() == asyncCycleConnections);
        QCOMPARE(captureModuleStarted.count(), asyncCycleStartedBefore);
        QCOMPARE(captureModuleFinished.count(), asyncCycleFinishedBefore);
        QCOMPARE(captureRunFinished.count(), asyncCycleRunsBefore);
        QCOMPARE(captureCycleStarted.count(), asyncCycleStartedSignalsBefore);
        QCOMPARE(captureCycleStopped.count(), asyncCycleStoppedSignalsBefore);
        QCOMPARE(captureRunFlowToolCount(), asyncCycleToolsBefore);
        QCOMPARE(captureToolExecuted.count(), asyncCycleAllToolsBefore);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QCOMPARE(engine.cycleSessionId(), QString());
        QVERIFY(!engine.isBusy());

        QJsonObject singleCyclePending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 1}, {QStringLiteral("wait"), false}});
        QVERIFY2(singleCyclePending.value(QStringLiteral("status")).toString() == QLatin1String("pending_confirmation"),
                 qPrintable(QString::fromUtf8(QJsonDocument(singleCyclePending).toJson(QJsonDocument::Compact))));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(captureCounters() == asyncCycleCounters);
        QVERIFY(connectionCounters() == asyncCycleConnections);
        QCOMPARE(captureModuleStarted.count(), asyncCycleStartedBefore);
        QCOMPARE(captureModuleFinished.count(), asyncCycleFinishedBefore);
        QCOMPARE(captureRunFinished.count(), asyncCycleRunsBefore);
        QCOMPARE(captureCycleStarted.count(), asyncCycleStartedSignalsBefore);
        QCOMPARE(captureCycleStopped.count(), asyncCycleStoppedSignalsBefore);
        QCOMPARE(captureRunFlowToolCount(), asyncCycleToolsBefore);
        QCOMPARE(captureToolExecuted.count(), asyncCycleAllToolsBefore);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QCOMPARE(engine.cycleSessionId(), QString());
        QVERIFY(!engine.isBusy());
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        singleCyclePending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 1}, {QStringLiteral("wait"), false}});
        QCOMPARE(singleCyclePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString singleCycleSessionConfirmation = controller.pendingConfirmationId();
        QVERIFY(!singleCycleSessionConfirmation.isEmpty());
        QVERIFY(controller.confirmPendingTools(singleCycleSessionConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(captureRunFinished.count(), asyncCycleRunsBefore + 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(captureCycleStopped.count(), asyncCycleStoppedSignalsBefore + 1, 5000);
        QCOMPARE(captureCycleStarted.count(), asyncCycleStartedSignalsBefore);
        QCOMPARE(captureModuleStarted.count(), asyncCycleStartedBefore + 1);
        QCOMPARE(captureModuleFinished.count(), asyncCycleFinishedBefore + 1);
        QTRY_COMPARE_WITH_TIMEOUT(captureRunFlowToolCount(), asyncCycleToolsBefore + 1, 2000);
        const QJsonObject singleCycleStarted = captureToolExecuted.last().at(1).toJsonObject();
        QCOMPARE(singleCycleStarted.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString singleCycleSessionId = singleCycleStarted.value(QStringLiteral("sessionId")).toString();
        QVERIFY(!singleCycleSessionId.isEmpty());
        QJsonObject singleCycleStatus;
        QTRY_COMPARE_WITH_TIMEOUT(
            (singleCycleStatus = actor->executeTool(QStringLiteral("get_run_results"),
                                                    {{QStringLiteral("runId"), singleCycleSessionId}}))
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("completed"), 2000);
        QCOMPARE(singleCycleStatus.value(QStringLiteral("maxCycles")).toInt(), 1);
        QCOMPARE(singleCycleStatus.value(QStringLiteral("completedCycles")).toInt(), 1);
        const std::array<int, 4> expectedSingleCycleCounters{asyncCycleCounters[0] + 1, asyncCycleCounters[1] + 1,
                                                             asyncCycleCounters[2] + 1, asyncCycleCounters[3] + 1};
        QVERIFY(captureCounters() == expectedSingleCycleCounters);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        for (int elapsed = 0; elapsed < 350; elapsed += 10) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QCOMPARE(captureModuleStarted.count(), asyncCycleStartedBefore + 1);
        QCOMPARE(captureModuleFinished.count(), asyncCycleFinishedBefore + 1);
        QCOMPARE(captureRunFinished.count(), asyncCycleRunsBefore + 1);
        QCOMPARE(captureCycleStarted.count(), asyncCycleStartedSignalsBefore);
        QCOMPARE(captureCycleStopped.count(), asyncCycleStoppedSignalsBefore + 1);
        QVERIFY(captureCounters() == expectedSingleCycleCounters);
        QVERIFY(!engine.isBusy());
        QVERIFY(engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"), QString()));
        const int callsBeforeMissingImplicitCamera = state.discoveryCalls;
        const int connectionsBeforeMissingImplicitCamera = state.connectCalls;
        const QJsonObject missingImplicitCamera =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(missingImplicitCamera.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MISSING_PARAM"));
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QCOMPARE(manager.cameraSessionCount(), 0);
        QCOMPARE(state.discoveryCalls, callsBeforeMissingImplicitCamera);
        QCOMPARE(state.connectCalls, connectionsBeforeMissingImplicitCamera);
        QVERIFY(!engine.isBusy());
        QVERIFY(
            engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"), QString::fromLatin1(kDeviceId)));
        QVERIFY(agentConnect());
        QCOMPARE(manager.cameraSessionCount(), 1);

        const std::array<int, 4> countersBeforeAgentCapture = captureCounters();
        int toolsBeforeAgentCapture = captureToolExecuted.count();

        QJsonObject capturePending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(capturePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonArray captureEffects = controller.pendingConfirmationPayload()
                                        .value(QStringLiteral("context"))
                                        .toObject()
                                        .value(QStringLiteral("sideEffects"))
                                        .toArray();
        QVERIFY(captureEffects.contains(QStringLiteral("camera_capture:%1@%2 exposure=3000 gain=2 timeout=1000ms")
                                            .arg(grabInstance.id, QString::fromLatin1(kDeviceId))));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(captureCounters() == countersBeforeAgentCapture);
        QCOMPARE(captureModuleStarted.count(), asyncCycleStartedBefore + 1);
        QVERIFY(!engine.isBusy());
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        QVERIFY(
            engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"), QString::fromLatin1(kDeviceId)));
        capturePending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(capturePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleCaptureConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!staleCaptureConfirmationId.isEmpty());
        QVERIFY(engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"),
                                              QStringLiteral("drifted-camera-target")));
        const int auditsBeforeStaleCaptureConfirmation = captureAudit.count();
        QVERIFY(controller.confirmPendingTools(staleCaptureConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(captureAudit.count(), auditsBeforeStaleCaptureConfirmation + 1, 5000);
        QVERIFY(captureCounters() == countersBeforeAgentCapture);
        QCOMPARE(captureModuleStarted.count(), asyncCycleStartedBefore + 1);
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));

        QVERIFY(
            engineProject->setModuleParam(grabInstance.id, QStringLiteral("cameraId"), QString::fromLatin1(kDeviceId)));
        capturePending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(capturePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString freshCaptureConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!freshCaptureConfirmationId.isEmpty());
        QVERIFY(freshCaptureConfirmationId != staleCaptureConfirmationId);
        captureEffects = controller.pendingConfirmationPayload()
                             .value(QStringLiteral("context"))
                             .toObject()
                             .value(QStringLiteral("sideEffects"))
                             .toArray();
        QVERIFY(captureEffects.contains(QStringLiteral("camera_capture:%1@%2 exposure=3000 gain=2 timeout=1000ms")
                                            .arg(grabInstance.id, QString::fromLatin1(kDeviceId))));
        const auto countersBeforeSessionDrift = captureCounters();
        const int modulesBeforeSessionDrift = captureModuleStarted.count();
        quint64 disconnectRequestId = 0;
        QString disconnectError;
        QVERIFY2(manager.requestDisconnectCameraAsync(QString::fromLatin1(kDeviceId), &disconnectRequestId,
                                                      &disconnectError),
                 qPrintable(disconnectError));
        QVERIFY(disconnectRequestId != 0);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QJsonObject disconnectResult;
        QTRY_VERIFY_WITH_TIMEOUT((disconnectResult = actor->executeTool(
                                      QStringLiteral("get_camera_operation"),
                                      {{QStringLiteral("requestId"), QString::number(disconnectRequestId)}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QLatin1String("success"),
                                 2000);
        const int auditsBeforeFreshCaptureConfirmation = captureAudit.count();
        QVERIFY(controller.confirmPendingTools(freshCaptureConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(captureAudit.count(), auditsBeforeFreshCaptureConfirmation + 1, 5000);
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(captureCounters() == countersBeforeSessionDrift);
        QCOMPARE(captureModuleStarted.count(), modulesBeforeSessionDrift);
        QVERIFY(!engine.isBusy());
        QVERIFY(agentConnect());
        capturePending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(capturePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString settledSessionConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!settledSessionConfirmationId.isEmpty());
        QVERIFY(settledSessionConfirmationId != freshCaptureConfirmationId);
        toolsBeforeAgentCapture = captureToolExecuted.count();
        QVERIFY(controller.confirmPendingTools(settledSessionConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(captureToolExecuted.count(), toolsBeforeAgentCapture + 1, 5000);
        const QJsonObject captureStarted = captureToolExecuted.last().at(1).toJsonObject();
        QCOMPARE(captureStarted.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString captureRunId = captureStarted.value(QStringLiteral("runId")).toString();
        QVERIFY(!captureRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(captureRunFinished.count(), asyncCycleRunsBefore + 2, 5000);
        const RunResult captureRunResult = qvariant_cast<RunResult>(captureRunFinished.last().at(0));
        QCOMPARE(captureRunResult.runId, captureRunId);
        QVERIFY2(captureRunResult.success, qPrintable(captureRunResult.errorMessage));
        QCOMPARE(captureModuleStarted.count(), asyncCycleStartedBefore + 2);
        const std::array<int, 4> expectedCaptureCounters{
            countersBeforeAgentCapture[0] + 1, countersBeforeAgentCapture[1] + 1, countersBeforeAgentCapture[2] + 1,
            countersBeforeAgentCapture[3] + 1};
        QVERIFY(captureCounters() == expectedCaptureCounters);
        const QJsonObject runStatus =
            controller.handleToolCall(QStringLiteral("get_run_results"), {{QStringLiteral("runId"), captureRunId}});
        QCOMPARE(runStatus.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        const QJsonObject imageOutput = controller.handleToolCall(
            QStringLiteral("get_module_output"),
            {{QStringLiteral("runId"), captureRunId}, {QStringLiteral("instanceId"), grabInstance.id}});
        QVERIFY2(imageOutput.value(QStringLiteral("status")).toString() == QLatin1String("available"),
                 qPrintable(QString::fromUtf8(QJsonDocument(imageOutput).toJson(QJsonDocument::Compact))));
        const QJsonObject imageSummary =
            imageOutput.value(QStringLiteral("summary")).toObject().value(QStringLiteral("image")).toObject();
        QCOMPARE(imageSummary.value(QStringLiteral("width")).toInt(), 4);
        QCOMPARE(imageSummary.value(QStringLiteral("height")).toInt(), 3);
        ImageData capturedImage;
        QMap<QString, QVariant> capturedNamedValues;
        PortValueMap capturedPorts;
        QVERIFY(
            engine.runOutputSnapshot(captureRunId, grabInstance.id, capturedImage, capturedNamedValues, capturedPorts));
        const QImage capturedPixels = capturedImage.toQImage();
        QCOMPARE(capturedPixels.size(), QSize(4, 3));
        const int triggeredFrameNumber = expectedCaptureCounters[0];
        const QRgb triggeredFrameColor =
            qRgb(20 + triggeredFrameNumber, 80 + triggeredFrameNumber, 160 + triggeredFrameNumber);
        const QRgb delayedPriorFrameColor = qRgb(1, 2, 3);
        for (int y = 0; y < capturedPixels.height(); ++y) {
            for (int x = 0; x < capturedPixels.width(); ++x) {
                QCOMPARE(capturedPixels.pixel(x, y), triggeredFrameColor);
                QVERIFY(capturedPixels.pixel(x, y) != delayedPriorFrameColor);
            }
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QVERIFY(agentConnect()); // wait for the one-shot run_flow session to retire before reconnecting
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        const auto countersBeforeStep = captureCounters();
        const auto connectionsBeforeStep = connectionCounters();
        const int startsBeforeStep = captureModuleStarted.count();
        const int finishesBeforeStep = captureModuleFinished.count();
        const int runsBeforeStep = captureRunFinished.count();
        const int toolsBeforeStep = captureToolExecuted.count();
        const QString captureEffect = QStringLiteral("camera_capture:%1@%2 exposure=3000 gain=2 timeout=1000ms")
                                          .arg(grabInstance.id, QString::fromLatin1(kDeviceId));
        QJsonObject stepPending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(stepPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(captureEffect));
        const QString rejectedStepId = controller.pendingConfirmationId();
        QVERIFY(!rejectedStepId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedStepId));
        QVERIFY(captureCounters() == countersBeforeStep);
        QVERIFY(connectionCounters() == connectionsBeforeStep);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(captureModuleStarted.count(), startsBeforeStep);
        QCOMPARE(captureModuleFinished.count(), finishesBeforeStep);
        QCOMPARE(captureRunFinished.count(), runsBeforeStep);
        QCOMPARE(captureToolExecuted.count(), toolsBeforeStep);
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));

        stepPending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(stepPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(captureEffect));
        const QString approvedStepId = controller.pendingConfirmationId();
        QVERIFY(!approvedStepId.isEmpty());
        QVERIFY(approvedStepId != rejectedStepId);
        QVERIFY(controller.confirmPendingTools(approvedStepId));
        QTRY_COMPARE_WITH_TIMEOUT(captureToolExecuted.count(), toolsBeforeStep + 1, 5000);
        const QJsonObject stepResult = captureToolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString stepRunId = stepResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!stepRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(captureRunFinished.count(), runsBeforeStep + 1, 5000);
        const RunResult stepRunResult = qvariant_cast<RunResult>(captureRunFinished.last().at(0));
        QCOMPARE(stepRunResult.runId, stepRunId);
        QVERIFY2(stepRunResult.success, qPrintable(stepRunResult.errorMessage));
        QCOMPARE(captureModuleStarted.count(), startsBeforeStep + 1);
        QCOMPARE(captureModuleStarted.last().at(0).toString(), grabInstance.id);
        QCOMPARE(captureModuleFinished.count(), finishesBeforeStep + 1);
        QCOMPARE(captureModuleFinished.last().at(0).toString(), grabInstance.id);
        const std::array<int, 4> expectedStepCounters{countersBeforeStep[0] + 1, countersBeforeStep[1] + 1,
                                                      countersBeforeStep[2] + 1, countersBeforeStep[3] + 1};
        QVERIFY(captureCounters() == expectedStepCounters);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        const std::array<int, 3> expectedStepConnections{connectionsBeforeStep[0], connectionsBeforeStep[1] + 1,
                                                         connectionsBeforeStep[2] + 1};
        QVERIFY(connectionCounters() == expectedStepConnections);
        const QJsonObject stepImage = controller.handleToolCall(
            QStringLiteral("get_module_output"),
            {{QStringLiteral("runId"), stepRunId}, {QStringLiteral("instanceId"), grabInstance.id}});
        QCOMPARE(stepImage.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(stepImage.value(QStringLiteral("runId")).toString(), stepRunId);
        const QJsonObject stepImageSummary =
            stepImage.value(QStringLiteral("summary")).toObject().value(QStringLiteral("image")).toObject();
        QCOMPARE(stepImageSummary.value(QStringLiteral("width")).toInt(), 4);
        QCOMPARE(stepImageSummary.value(QStringLiteral("height")).toInt(), 3);
        QCOMPARE(captureCounters(), expectedStepCounters);

        drainEngineOrFail("GrabImage step_once cleanup");
        engine.clearModules();
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        ProjectManager::instance().closeProject();

        Project* resumeProject = ProjectManager::instance().newProject();
        QVERIFY(resumeProject);
        ModuleInstance resumeGrabInstance = grabInstance;
        resumeGrabInstance.id = QStringLiteral("g5-agent-grab-resume");
        resumeGrabInstance.name = QStringLiteral("G5 Agent GrabImage Resume");
        resumeGrabInstance.breakpoint = true;
        resumeProject->addModule(resumeGrabInstance);
        QVERIFY(engine.loadProject(resumeProject));
        QVERIFY(agentConnect());
        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
        QString pausedCameraRunId = engine.runObservation().runId;
        QVERIFY(!pausedCameraRunId.isEmpty());
        quint64 resumeDisconnectRequestId = 0;
        QString resumeDisconnectError;
        QVERIFY2(manager.requestDisconnectCameraAsync(QString::fromLatin1(kDeviceId), &resumeDisconnectRequestId,
                                                      &resumeDisconnectError),
                 qPrintable(resumeDisconnectError));
        QJsonObject resumeDisconnectResult;
        QTRY_VERIFY_WITH_TIMEOUT((resumeDisconnectResult = actor->executeTool(
                                      QStringLiteral("get_camera_operation"),
                                      {{QStringLiteral("requestId"), QString::number(resumeDisconnectRequestId)}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QLatin1String("success"),
                                 2000);
        const auto countersBeforeMissingResumeSession = captureCounters();
        const int startsBeforeMissingResumeSession = captureModuleStarted.count();
        const int finishesBeforeMissingResumeSession = captureModuleFinished.count();
        const int runsBeforeMissingResumeSession = captureRunFinished.count();
        const int toolsBeforeMissingResumeSession = captureToolExecuted.count();
        const QJsonObject missingResumeSession = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(missingResumeSession.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_SESSION_REQUIRED"));
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedCameraRunId);
        QVERIFY(captureCounters() == countersBeforeMissingResumeSession);
        QCOMPARE(captureModuleStarted.count(), startsBeforeMissingResumeSession);
        QCOMPARE(captureModuleFinished.count(), finishesBeforeMissingResumeSession);
        QCOMPARE(captureRunFinished.count(), runsBeforeMissingResumeSession);
        QCOMPARE(captureToolExecuted.count(), toolsBeforeMissingResumeSession);
        QVERIFY(agentConnect()); // resume approval must reuse this existing async session
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        auto countersBeforeResume = captureCounters();
        auto connectionsBeforeResume = connectionCounters();
        int startsBeforeResume = captureModuleStarted.count();
        int finishesBeforeResume = captureModuleFinished.count();
        int runsBeforeResume = captureRunFinished.count();
        int toolsBeforeResume = captureToolExecuted.count();
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(countersBeforeResume, expectedStepCounters);
        QCOMPARE(startsBeforeResume, asyncCycleStartedBefore + 3); // cycle=1, run_flow once, approved step_once
        QCOMPARE(finishesBeforeResume, asyncCycleFinishedBefore + 3);
        QCOMPARE(runsBeforeResume, asyncCycleRunsBefore + 3);

        auto startResumePending = [&controller] {
            return controller.handleToolCall(QStringLiteral("resume_flow"), {});
        };
        auto expectedResumeEffects = [&resumeGrabInstance] {
            return QJsonArray{QStringLiteral("execution:resume_paused_session_may_touch_files_devices_network"),
                              QStringLiteral("camera_capture:%1@%2 exposure=3000 gain=2 timeout=1000ms")
                                  .arg(resumeGrabInstance.id, QString::fromLatin1(kDeviceId))};
        };
        QJsonObject resumePending = startResumePending();
        QCOMPARE(resumePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QJsonArray resumeEffects = controller.pendingConfirmationPayload()
                                       .value(QStringLiteral("context"))
                                       .toObject()
                                       .value(QStringLiteral("sideEffects"))
                                       .toArray();
        QCOMPARE(resumeEffects, expectedResumeEffects());
        const QString staleResumeId = controller.pendingConfirmationId();
        QVERIFY(!staleResumeId.isEmpty());
        QVERIFY(resumeProject->setModuleParam(resumeGrabInstance.id, QStringLiteral("cameraId"),
                                              QStringLiteral("drifted-camera-target")));
        const int auditBeforeStaleResumeConfirmation = captureAudit.count();
        QVERIFY(controller.confirmPendingTools(staleResumeId));
        QTRY_COMPARE_WITH_TIMEOUT(captureAudit.count(), auditBeforeStaleResumeConfirmation + 1, 5000);
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedCameraRunId);
        QCOMPARE(captureCounters(), countersBeforeResume);
        QCOMPARE(connectionCounters(), connectionsBeforeResume);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(captureModuleStarted.count(), startsBeforeResume);
        QCOMPARE(captureModuleFinished.count(), finishesBeforeResume);
        QCOMPARE(captureRunFinished.count(), runsBeforeResume);
        QCOMPARE(captureToolExecuted.count(), toolsBeforeResume);

        QVERIFY(resumeProject->setModuleParam(resumeGrabInstance.id, QStringLiteral("cameraId"),
                                              QString::fromLatin1(kDeviceId)));
        // setModuleParam updates the project modified timestamp even when the parameter is restored.
        // Start a fresh paused run so its snapshot matches the restored project deterministically.
        engine.stop();
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Stopped, 5000);
        engine.clearModules();
        QVERIFY(engine.loadProject(resumeProject));
        engine.runOnce();
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
        pausedCameraRunId = engine.runObservation().runId;
        QVERIFY(!pausedCameraRunId.isEmpty());
        countersBeforeResume = captureCounters();
        connectionsBeforeResume = connectionCounters();
        startsBeforeResume = captureModuleStarted.count();
        finishesBeforeResume = captureModuleFinished.count();
        runsBeforeResume = captureRunFinished.count();
        toolsBeforeResume = captureToolExecuted.count();
        resumePending = startResumePending();
        QVERIFY2(resumePending.value(QStringLiteral("status")).toString() == QLatin1String("pending_confirmation"),
                 qPrintable(QString::fromUtf8(QJsonDocument(resumePending).toJson(QJsonDocument::Compact))));
        resumeEffects = controller.pendingConfirmationPayload()
                            .value(QStringLiteral("context"))
                            .toObject()
                            .value(QStringLiteral("sideEffects"))
                            .toArray();
        QCOMPARE(resumeEffects, expectedResumeEffects());
        const QString rejectedResumeId = controller.pendingConfirmationId();
        QVERIFY(!rejectedResumeId.isEmpty());
        QVERIFY(rejectedResumeId != staleResumeId);
        QVERIFY(controller.rejectPendingTools(rejectedResumeId));
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedCameraRunId);
        QCOMPARE(captureCounters(), countersBeforeResume);
        QCOMPARE(connectionCounters(), connectionsBeforeResume);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(captureModuleStarted.count(), startsBeforeResume);
        QCOMPARE(captureModuleFinished.count(), finishesBeforeResume);
        QCOMPARE(captureRunFinished.count(), runsBeforeResume);
        QCOMPARE(captureToolExecuted.count(), toolsBeforeResume);

        resumePending = startResumePending();
        QCOMPARE(resumePending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedResumeId = controller.pendingConfirmationId();
        QVERIFY(!approvedResumeId.isEmpty());
        QVERIFY(approvedResumeId != rejectedResumeId);
        QVERIFY(controller.confirmPendingTools(approvedResumeId));
        QTRY_COMPARE_WITH_TIMEOUT(captureToolExecuted.count(), toolsBeforeResume + 1, 5000);
        const QJsonObject resumeResult = captureToolExecuted.last().at(1).toJsonObject();
        QCOMPARE(resumeResult.value(QStringLiteral("status")).toString(), QStringLiteral("resumed"));
        drainEngineOrFail("GrabImage resume_flow cleanup");
        QCOMPARE(engine.state(), RunState::Idle);
        QCOMPARE(engine.runObservation().runId, pausedCameraRunId);
        QTRY_COMPARE_WITH_TIMEOUT(captureRunFinished.count(), runsBeforeResume + 1, 5000);
        const RunResult resumedCameraResult = qvariant_cast<RunResult>(captureRunFinished.last().at(0));
        QCOMPARE(resumedCameraResult.runId, pausedCameraRunId);
        QVERIFY2(resumedCameraResult.success, qPrintable(resumedCameraResult.errorMessage));
        QCOMPARE(captureModuleStarted.count(), startsBeforeResume + 1);
        QCOMPARE(captureModuleStarted.last().at(0).toString(), resumeGrabInstance.id);
        QCOMPARE(captureModuleFinished.count(), finishesBeforeResume + 1);
        QCOMPARE(captureModuleFinished.last().at(0).toString(), resumeGrabInstance.id);
        const std::array<int, 4> expectedResumeCounters{countersBeforeResume[0] + 1, countersBeforeResume[1] + 1,
                                                        countersBeforeResume[2] + 1, countersBeforeResume[3] + 1};
        QVERIFY(captureCounters() == expectedResumeCounters);
        const std::array<int, 3> expectedResumeConnections{connectionsBeforeResume[0], connectionsBeforeResume[1] + 1,
                                                           connectionsBeforeResume[2] + 1};
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QVERIFY(connectionCounters() == expectedResumeConnections);
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!captureAudit.last().at(0).value<AgentActionLogEntry>().undoable);
        const QJsonObject resumedImage = controller.handleToolCall(
            QStringLiteral("get_module_output"),
            {{QStringLiteral("runId"), pausedCameraRunId}, {QStringLiteral("instanceId"), resumeGrabInstance.id}});
        QCOMPARE(resumedImage.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(resumedImage.value(QStringLiteral("runId")).toString(), pausedCameraRunId);
        const QJsonObject resumedImageSummary =
            resumedImage.value(QStringLiteral("summary")).toObject().value(QStringLiteral("image")).toObject();
        QCOMPARE(resumedImageSummary.value(QStringLiteral("width")).toInt(), 4);
        QCOMPARE(resumedImageSummary.value(QStringLiteral("height")).toInt(), 3);
        QVERIFY(captureCounters() == expectedResumeCounters);

        // A completed camera capture retires its one-shot session. If the paused
        // cursor is already past that camera and no camera is reachable later,
        // resume must remain usable without falling back to synchronous connect.
        engine.clearModules();
        ProjectManager::instance().closeProject();
        Project* compatibilityProject = ProjectManager::instance().newProject();
        QVERIFY(compatibilityProject);
        ModuleInstance priorCamera = grabInstance;
        priorCamera.id = QStringLiteral("g5-resume-prior-camera");
        ModuleInstance laterDelay;
        laterDelay.id = QStringLiteral("g5-resume-later-delay");
        laterDelay.moduleId = QStringLiteral("延时");
        laterDelay.name = QStringLiteral("G5 Resume Delay");
        laterDelay.params = {{QStringLiteral("delayMs"), 1}};
        laterDelay.breakpoint = true;
        compatibilityProject->addModule(priorCamera);
        compatibilityProject->addModule(laterDelay);
        ModuleConnection cameraToDelay;
        cameraToDelay.fromModuleId = priorCamera.id;
        cameraToDelay.toModuleId = laterDelay.id;
        cameraToDelay.fromPort = QStringLiteral("next");
        cameraToDelay.toPort = QStringLiteral("control");
        cameraToDelay.edgeType = QStringLiteral("control");
        compatibilityProject->addConnection(cameraToDelay);
        QVERIFY(engine.loadProject(compatibilityProject));
        QVERIFY(agentConnect());
        const auto beforePriorCamera = captureCounters();
        const auto connectionsBeforePriorCamera = connectionCounters();
        const int toolsBeforeConnectedStep = captureToolExecuted.count();
        QJsonObject connectedStep = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(connectedStep.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(QStringLiteral("camera_capture:%1@%2 exposure=3000 gain=2 timeout=1000ms")
                                  .arg(priorCamera.id, QString::fromLatin1(kDeviceId))));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(captureToolExecuted.count(), toolsBeforeConnectedStep + 1, 5000);
        QCOMPARE(captureToolExecuted.last().at(0).toString(), QStringLiteral("step_once"));
        QCOMPARE(captureToolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("started"));
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(!FlowExecutionService::instance().isActive(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
        engine.clearModules();
        QString compatibilityLoadError;
        const QMetaObject::Connection compatibilityLoadErrorConnection = QObject::connect(
            &engine, &RunEngine::errorOccurred, &engine,
            [&compatibilityLoadError](const QString& error) { compatibilityLoadError = error; });
        const bool compatibilityLoaded = engine.loadProject(compatibilityProject);
        QObject::disconnect(compatibilityLoadErrorConnection);
        QVERIFY2(compatibilityLoaded, qPrintable(compatibilityLoadError));
        QVERIFY(agentConnect());
        const int toolsBeforeConnectedRun = captureToolExecuted.count();
        QJsonObject connectedRun =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(connectedRun.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(QStringLiteral("camera_capture:%1@%2 exposure=3000 gain=2 timeout=1000ms")
                                  .arg(priorCamera.id, QString::fromLatin1(kDeviceId))));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(captureToolExecuted.count(), toolsBeforeConnectedRun + 1, 5000);
        QCOMPARE(captureToolExecuted.last().at(0).toString(), QStringLiteral("run_flow"));
        QCOMPARE(captureToolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("started"));
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Paused, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        const RunExecutionCandidates resumeCandidates = engine.resumeExecutionCandidates();
        QVERIFY(resumeCandidates.complete);
        QVERIFY(resumeCandidates.moduleIds.contains(laterDelay.id));
        QVERIFY(!resumeCandidates.moduleIds.contains(priorCamera.id));
        const auto beforeNonCameraResume = captureCounters();
        const auto connectionsBeforeNonCameraResume = connectionCounters();
        const int delayStartsBeforeResume = captureModuleStarted.count();
        QJsonObject nonCameraResume = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(nonCameraResume.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray nonCameraResumeEffects = controller.pendingConfirmationPayload()
                                                      .value(QStringLiteral("context"))
                                                      .toObject()
                                                      .value(QStringLiteral("sideEffects"))
                                                      .toArray();
        QVERIFY(
            !nonCameraResumeEffects.contains(QStringLiteral("camera_capture:%1@%2 exposure=3000 gain=2 timeout=1000ms")
                                                 .arg(priorCamera.id, QString::fromLatin1(kDeviceId))));
        const QString nonCameraResumeId = controller.pendingConfirmationId();
        QVERIFY(!nonCameraResumeId.isEmpty());
        QVERIFY(controller.confirmPendingTools(nonCameraResumeId));
        QTRY_VERIFY_WITH_TIMEOUT(controller.state() == AgentController::AgentState::Idle &&
                                     !FlowExecutionService::instance().isActive() && !engine.isBusy(),
                                 5000);
        QVERIFY(engine.lastRunResult().success);
        QVERIFY(captureCounters() == beforeNonCameraResume);
        QVERIFY(connectionCounters() == connectionsBeforeNonCameraResume);
        QCOMPARE(captureModuleStarted.count(), delayStartsBeforeResume + 1);
        QCOMPARE(captureModuleStarted.last().at(0).toString(), laterDelay.id);
        QCOMPARE(captureCounters()[0], beforePriorCamera[0] + 2);
        QCOMPARE(connectionCounters()[0], connectionsBeforePriorCamera[0] + 1);
        engine.clearModules();
        ProjectManager::instance().closeProject();

        QVERIFY(agentConnect());
        {
            QMutexLocker locker(&state.mutex);
            state.blockTrigger = true;
            state.releaseTrigger = false;
            state.triggerEntered = false;
        }
        grab.setParam(QStringLiteral("grabTimeout"), 80);
        {
            QMutexLocker locker(&errorMutex);
            grabErrors.clear();
        }
        ImageData timedOutOutput;
        std::atomic<bool> timeoutWorkerDone{false};
        bool timeoutWorkerSuccess = true;
        std::thread timeoutWorker([&]() {
            timeoutWorkerSuccess = grab.execute(ImageData(), timedOutOutput);
            timeoutWorkerDone.store(true, std::memory_order_release);
        });
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                QMutexLocker locker(&state.mutex);
                return state.triggerEntered;
            }(),
            2000);
        QTRY_VERIFY_WITH_TIMEOUT(timeoutWorkerDone.load(std::memory_order_acquire), 2000);
        timeoutWorker.join();
        QVERIFY(!timeoutWorkerSuccess);
        QVERIFY(timedOutOutput.toQImage().isNull());
        {
            QMutexLocker locker(&errorMutex);
            QVERIFY(std::any_of(grabErrors.cbegin(), grabErrors.cend(), [](const QString& error) {
                return error.contains(QStringLiteral("CAMERA_GRAB_TIMEOUT"));
            }));
        }
        QCOMPARE(manager.cameraSessionCount(), 1); // caller 返回超时，相机 worker 仍占用 session
        const QJsonObject busyDisconnect = actor->executeTool(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(busyDisconnect.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_OPERATION_BUSY"));
        {
            QMutexLocker locker(&state.mutex);
            state.releaseTrigger = true;
            state.triggerGate.wakeAll();
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        disconnect(errorConnection);
        grab.setParam(QStringLiteral("grabTimeout"), 1000);

        QVERIFY(agentConnect()); // request B uses a fresh session after timed-out request A drains
        ImageData afterLateFrameOutput;
        QVERIFY(grab.execute(ImageData(), afterLateFrameOutput));
        int expectedLateFrameNumber = 0;
        {
            QMutexLocker locker(&state.mutex);
            expectedLateFrameNumber = state.triggerCalls;
        }
        QCOMPARE(afterLateFrameOutput.toQImage().pixel(0, 0),
                 qRgb(20 + expectedLateFrameNumber, 80 + expectedLateFrameNumber, 160 + expectedLateFrameNumber));
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);

        QVERIFY(agentConnect());
        const int disconnectCallsBeforeConfirmation = [&state]() {
            QMutexLocker locker(&state.mutex);
            return state.disconnectCalls;
        }();
        QSignalSpy disconnectStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject disconnectPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(disconnectPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray disconnectEffects = controller.pendingConfirmationPayload()
                                                 .value(QStringLiteral("context"))
                                                 .toObject()
                                                 .value(QStringLiteral("sideEffects"))
                                                 .toArray();
        QVERIFY(disconnectEffects.first().toString().startsWith(
            QStringLiteral("camera_disconnect:%1 owner=%2")
                .arg(QString::fromLatin1(kDeviceId), QString::fromLatin1(kPluginId))));
        QVERIFY(disconnectEffects.first().toString().contains(QStringLiteral("serial=g5-fake-serial")));
        QVERIFY(disconnectEffects.first().toString().contains(QStringLiteral("state=connected")));
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        const QString rejectedDisconnectConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!rejectedDisconnectConfirmationId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedDisconnectConfirmationId));
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("disconnect_camera"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeConfirmation);
        }

        const QJsonObject approvedDisconnect = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(approvedDisconnect.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString disconnectConfirmationId = controller.pendingConfirmationId();
        QVERIFY(!disconnectConfirmationId.isEmpty());
        const QJsonObject confirmedCameraTarget = manager.cameraLifecycleSnapshot(QString::fromLatin1(kDeviceId));
        int agentDisconnectToolCalls = 0;
        QObject disconnectExecutionContext;
        connect(actor, &AgentActor::toolExecuted, &disconnectExecutionContext,
                [&agentDisconnectToolCalls](const QString& name, const QJsonObject&) {
                    if (name == QLatin1String("disconnect_camera"))
                        ++agentDisconnectToolCalls;
                });
        const int disconnectCallsBeforeExternal = [&state]() {
            QMutexLocker locker(&state.mutex);
            return state.disconnectCalls;
        }();
        quint64 externalDisconnectRequestId = 0;
        QString externalDisconnectError;
        QVERIFY2(manager.requestDisconnectCameraAsync(QString::fromLatin1(kDeviceId), &externalDisconnectRequestId,
                                                      &externalDisconnectError),
                 qPrintable(externalDisconnectError));
        QTRY_COMPARE_WITH_TIMEOUT(disconnectStarted.count(), 1, 2000);
        const QString externalDisconnectId = QString::number(disconnectStarted.last().at(1).toULongLong());
        QJsonObject externalDisconnectOperation;
        QTRY_VERIFY_WITH_TIMEOUT(
            (externalDisconnectOperation = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                              {{QStringLiteral("requestId"), externalDisconnectId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QCOMPARE(externalDisconnectRequestId, externalDisconnectId.toULongLong());
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        quint64 staleDisconnectRequestId = 999;
        QString staleDisconnectError;
        QVERIFY(!manager.requestDisconnectCameraAsync(QString::fromLatin1(kDeviceId), confirmedCameraTarget,
                                                      &staleDisconnectRequestId, &staleDisconnectError));
        QCOMPARE(staleDisconnectError, QStringLiteral("CAMERA_TARGET_CHANGED"));
        QCOMPARE(staleDisconnectRequestId, quint64(0));
        QCOMPARE(disconnectStarted.count(), 1);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeExternal + 1);
        }
        const int auditsBeforeExpiredDisconnect = audit.count();
        QVERIFY(controller.confirmPendingTools(disconnectConfirmationId));
        QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(audit.count(), auditsBeforeExpiredDisconnect + 1, 5000);
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("disconnect_camera"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(agentDisconnectToolCalls, 0);
        QCOMPARE(disconnectStarted.count(), 1);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeExternal + 1);
        }

        const QJsonObject freshConnectPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(freshConnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        const QString freshConnectId = controller.pendingConfirmationId();
        QVERIFY(!freshConnectId.isEmpty());
        QVERIFY(freshConnectId != disconnectConfirmationId);
        QVERIFY(controller.confirmPendingTools(freshConnectId));
        QTRY_COMPARE_WITH_TIMEOUT(disconnectStarted.count(), 2, 2000);
        const QString freshConnectOperationId = QString::number(disconnectStarted.last().at(1).toULongLong());
        QJsonObject freshConnectOperation;
        QTRY_VERIFY_WITH_TIMEOUT(
            (freshConnectOperation = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                        {{QStringLiteral("requestId"), freshConnectOperationId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        disconnectStarted.clear();

        const QJsonObject freshDisconnectPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(freshDisconnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        const QString freshDisconnectId = controller.pendingConfirmationId();
        QVERIFY(!freshDisconnectId.isEmpty());
        QVERIFY(freshDisconnectId != disconnectConfirmationId);
        const int auditsBeforeFreshDisconnect = audit.count();
        QVERIFY(controller.confirmPendingTools(freshDisconnectId));
        QTRY_COMPARE_WITH_TIMEOUT(audit.count(), auditsBeforeFreshDisconnect + 1, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("disconnect_camera"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("pending"));
        QTRY_COMPARE_WITH_TIMEOUT(disconnectStarted.count(), 1, 2000);
        const QString disconnectId = QString::number(disconnectStarted.last().at(1).toULongLong());
        QJsonObject disconnectOperation;
        QTRY_VERIFY_WITH_TIMEOUT(
            (disconnectOperation = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                      {{QStringLiteral("requestId"), disconnectId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeExternal + 2);
        }

        QVERIFY(agentConnect());
        int disconnectCallsBeforeThrownDisconnect = 0;
        {
            QMutexLocker locker(&state.mutex);
            disconnectCallsBeforeThrownDisconnect = state.disconnectCalls;
            state.throwOnDisconnectAfterDisconnecting = true;
        }
        QSignalSpy thrownDisconnectStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject rejectedThrownDisconnect = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(rejectedThrownDisconnect.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QVERIFY(manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(thrownDisconnectStarted.count(), 0);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeThrownDisconnect);
        }

        const QJsonObject thrownDisconnectPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(thrownDisconnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(thrownDisconnectStarted.count(), 1, 2000);
        const QString thrownDisconnectId = QString::number(thrownDisconnectStarted.last().at(1).toULongLong());
        QJsonObject thrownDisconnectResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (thrownDisconnectResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                         {{QStringLiteral("requestId"), thrownDisconnectId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("error"),
            2000);
        QCOMPARE(thrownDisconnectResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_DISCONNECT_UNCONFIRMED"));
        QCOMPARE(thrownDisconnectResult.value(QStringLiteral("sideEffectStatus")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(thrownDisconnectResult.value(QStringLiteral("connectionState")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(thrownDisconnectResult.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(thrownDisconnectResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), false);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QCOMPARE(manager.cameraState(QString::fromLatin1(kDeviceId)), CameraState::Error);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), thrownDisconnectId}}),
                 thrownDisconnectResult);

        QSignalSpy blockedConnectExecuted(actor, &AgentActor::toolExecuted);
        const QJsonObject blockedReconnect = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(blockedReconnect.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(blockedConnectExecuted.count(), 1, 2000);
        QCOMPARE(blockedConnectExecuted.last().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_OPERATION_BUSY"));
        QCOMPARE(thrownDisconnectStarted.count(), 1);
        QTest::qWait(300);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), thrownDisconnectId}}),
                 thrownDisconnectResult);
        QCOMPARE(thrownDisconnectStarted.count(), 1);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeThrownDisconnect + 1);
        }

        const QJsonObject thrownDisconnectCleanupPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(thrownDisconnectCleanupPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(thrownDisconnectStarted.count(), 2, 2000);
        const QString thrownDisconnectCleanupId = QString::number(thrownDisconnectStarted.last().at(1).toULongLong());
        QJsonObject thrownDisconnectCleanupResult;
        QTRY_VERIFY_WITH_TIMEOUT((thrownDisconnectCleanupResult =
                                      actor->executeTool(QStringLiteral("get_camera_operation"),
                                                         {{QStringLiteral("requestId"), thrownDisconnectCleanupId}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QStringLiteral("success"),
                                 2000);
        QCOMPARE(thrownDisconnectCleanupResult.value(QStringLiteral("connectionState")).toString(),
                 QStringLiteral("disconnected"));
        QCOMPARE(thrownDisconnectCleanupResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeThrownDisconnect + 1);
            state.throwOnDisconnectAfterDisconnecting = false;
        }

        manager.setAsyncConnectTimeoutForTesting(100);
        int connectCallsBeforeTimeout = 0;
        int connectReturnedCallsBeforeTimeout = 0;
        {
            QMutexLocker locker(&state.mutex);
            connectCallsBeforeTimeout = state.connectCalls;
            connectReturnedCallsBeforeTimeout = state.connectReturnedCalls;
            state.blockConnect = true;
            state.releaseConnect = false;
            state.connectEntered = false;
        }
        QSignalSpy timeoutConnectStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject rejectedTimeoutConnect = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(rejectedTimeoutConnect.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(timeoutConnectStarted.count(), 0);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, connectCallsBeforeTimeout);
        }

        const QJsonObject timeoutConnectPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(timeoutConnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(timeoutConnectStarted.count(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&state]() {
                QMutexLocker locker(&state.mutex);
                return state.connectEntered;
            }(),
            2000);
        const QString timeoutConnectId = QString::number(timeoutConnectStarted.last().at(1).toULongLong());
        QCOMPARE(
            actor
                ->executeTool(QStringLiteral("get_camera_operation"), {{QStringLiteral("requestId"), timeoutConnectId}})
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("pending"));
        QJsonObject timeoutConnectResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (timeoutConnectResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                       {{QStringLiteral("requestId"), timeoutConnectId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("error"),
            2000);
        QCOMPARE(timeoutConnectResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CONNECT_TIMEOUT_UNCONFIRMED"));
        QCOMPARE(timeoutConnectResult.value(QStringLiteral("sideEffectStatus")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(timeoutConnectResult.value(QStringLiteral("connectionState")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(timeoutConnectResult.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(manager.cameraSessionCount(), 1);

        QSignalSpy timeoutRetryExecuted(actor, &AgentActor::toolExecuted);
        const QJsonObject timeoutReconnect = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(timeoutReconnect.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(timeoutRetryExecuted.count(), 1, 2000);
        QCOMPARE(timeoutRetryExecuted.last().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_OPERATION_BUSY"));
        QCOMPARE(timeoutConnectStarted.count(), 1);

        QSignalSpy timeoutDisconnectStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject timeoutDisconnectPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(timeoutDisconnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(timeoutDisconnectStarted.count(), 1, 2000);
        const QString timeoutDisconnectId = QString::number(timeoutDisconnectStarted.last().at(1).toULongLong());
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("get_camera_operation"),
                                   {{QStringLiteral("requestId"), timeoutDisconnectId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("pending"));
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, connectCallsBeforeTimeout + 1);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeThrownDisconnect + 1);
            QCOMPARE(state.connectReturnedCalls, connectReturnedCallsBeforeTimeout);
            state.releaseConnect = true;
            state.connectGate.wakeAll();
        }

        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                QMutexLocker locker(&state.mutex);
                return state.connectReturnedCalls == connectReturnedCallsBeforeTimeout + 1;
            }(),
            2000);
        QJsonObject timeoutDisconnectResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (timeoutDisconnectResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                          {{QStringLiteral("requestId"), timeoutDisconnectId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QCOMPARE(timeoutDisconnectResult.value(QStringLiteral("connectionState")).toString(),
                 QStringLiteral("disconnected"));
        QCOMPARE(timeoutDisconnectResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), timeoutConnectId}}),
                 timeoutConnectResult);
        QCOMPARE(timeoutConnectStarted.count(), 2); // one connect plus the queued disconnect
        QCOMPARE(timeoutDisconnectStarted.count(), 1);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, connectCallsBeforeTimeout + 1);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeThrownDisconnect + 2);
            state.blockConnect = false;
            state.releaseConnect = false;
        }
        manager.setAsyncConnectTimeoutForTesting(30000);

        QVERIFY(agentConnect());
        manager.setAsyncDisconnectTimeoutForTesting(100);
        int disconnectCallsBeforeTimeout = 0;
        int disconnectReturnedCallsBeforeTimeout = 0;
        {
            QMutexLocker locker(&state.mutex);
            disconnectCallsBeforeTimeout = state.disconnectCalls;
            disconnectReturnedCallsBeforeTimeout = state.disconnectReturnedCalls;
            state.blockDisconnect = true;
            state.releaseDisconnect = false;
            state.disconnectEntered = false;
        }
        QSignalSpy timeoutDisconnectSettled(&manager, &CameraManager::cameraOperationSettled);
        QSignalSpy blockedDisconnectTimeoutStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject rejectedTimeoutDisconnect = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(rejectedTimeoutDisconnect.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.rejectPendingTools(controller.pendingConfirmationId()));
        QCOMPARE(blockedDisconnectTimeoutStarted.count(), 0);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeTimeout);
        }

        const QJsonObject timeoutDisconnectToolPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(timeoutDisconnectToolPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(blockedDisconnectTimeoutStarted.count(), 1, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&state]() {
                QMutexLocker locker(&state.mutex);
                return state.disconnectEntered;
            }(),
            2000);
        const QString timeoutDisconnectRequestId =
            QString::number(blockedDisconnectTimeoutStarted.last().at(1).toULongLong());
        QCOMPARE(actor
                     ->executeTool(QStringLiteral("get_camera_operation"),
                                   {{QStringLiteral("requestId"), timeoutDisconnectRequestId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("pending"));
        QJsonObject timeoutDisconnectOperation;
        QTRY_VERIFY_WITH_TIMEOUT((timeoutDisconnectOperation =
                                      actor->executeTool(QStringLiteral("get_camera_operation"),
                                                         {{QStringLiteral("requestId"), timeoutDisconnectRequestId}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QStringLiteral("error"),
                                 2000);
        QCOMPARE(timeoutDisconnectOperation.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_DISCONNECT_TIMEOUT_UNCONFIRMED"));
        QCOMPARE(timeoutDisconnectOperation.value(QStringLiteral("sideEffectStatus")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(timeoutDisconnectOperation.value(QStringLiteral("connectionState")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(timeoutDisconnectOperation.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(timeoutDisconnectOperation.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), false);
        QCOMPARE(timeoutDisconnectSettled.count(), 1);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), timeoutDisconnectRequestId}}),
                 timeoutDisconnectOperation);

        QSignalSpy disconnectBusyExecuted(actor, &AgentActor::toolExecuted);
        const QJsonObject blockedTimeoutReconnect = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(blockedTimeoutReconnect.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(disconnectBusyExecuted.count(), 1, 2000);
        QCOMPARE(disconnectBusyExecuted.last().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_OPERATION_BUSY"));
        const QJsonObject blockedTimeoutDisconnect = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(blockedTimeoutDisconnect.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(disconnectBusyExecuted.count(), 2, 2000);
        QCOMPARE(disconnectBusyExecuted.last().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_OPERATION_BUSY"));
        QCOMPARE(blockedDisconnectTimeoutStarted.count(), 1);
        QCOMPARE(timeoutDisconnectSettled.count(), 1);
        QTest::qWait(150);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), timeoutDisconnectRequestId}}),
                 timeoutDisconnectOperation);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeTimeout + 1);
            QCOMPARE(state.disconnectReturnedCalls, disconnectReturnedCallsBeforeTimeout);
            state.releaseDisconnect = true;
            state.disconnectGate.wakeAll();
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                QMutexLocker locker(&state.mutex);
                return state.disconnectReturnedCalls == disconnectReturnedCallsBeforeTimeout + 1;
            }(),
            2000);
        QTest::qWait(50); // consume the late session callback before accepting a new cleanup request
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), timeoutDisconnectRequestId}}),
                 timeoutDisconnectOperation);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QCOMPARE(timeoutDisconnectSettled.count(), 1);
        const QJsonObject confirmedTimeoutCleanup = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(confirmedTimeoutCleanup.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(blockedDisconnectTimeoutStarted.count(), 2, 2000);
        const QString confirmedTimeoutCleanupId =
            QString::number(blockedDisconnectTimeoutStarted.last().at(1).toULongLong());
        QJsonObject confirmedTimeoutCleanupResult;
        QTRY_VERIFY_WITH_TIMEOUT((confirmedTimeoutCleanupResult =
                                      actor->executeTool(QStringLiteral("get_camera_operation"),
                                                         {{QStringLiteral("requestId"), confirmedTimeoutCleanupId}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QStringLiteral("success"),
                                 2000);
        QCOMPARE(confirmedTimeoutCleanupResult.value(QStringLiteral("connectionState")).toString(),
                 QStringLiteral("disconnected"));
        QCOMPARE(confirmedTimeoutCleanupResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QCOMPARE(timeoutDisconnectSettled.count(), 2);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.disconnectCalls, disconnectCallsBeforeTimeout + 1);
            state.blockDisconnect = false;
            state.releaseDisconnect = false;
        }
        manager.setAsyncDisconnectTimeoutForTesting(30000);

        manager.setAsyncConnectTimeoutForTesting(100);
        int queuedConnectCallsBeforeTimeout = 0;
        int queuedConnectReturnsBeforeTimeout = 0;
        int queuedDisconnectCallsBeforeTimeout = 0;
        int queuedDisconnectReturnsBeforeTimeout = 0;
        {
            QMutexLocker locker(&state.mutex);
            queuedConnectCallsBeforeTimeout = state.connectCalls;
            queuedConnectReturnsBeforeTimeout = state.connectReturnedCalls;
            queuedDisconnectCallsBeforeTimeout = state.disconnectCalls;
            queuedDisconnectReturnsBeforeTimeout = state.disconnectReturnedCalls;
            state.blockConnect = true;
            state.releaseConnect = false;
            state.connectEntered = false;
        }
        QSignalSpy queuedConnectStarted(&manager, &CameraManager::cameraOperationStarted);
        QSignalSpy queuedOperationsSettled(&manager, &CameraManager::cameraOperationSettled);
        const QJsonObject queuedConnectPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(queuedConnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_VERIFY_WITH_TIMEOUT(
            [&state]() {
                QMutexLocker locker(&state.mutex);
                return state.connectEntered;
            }(),
            2000);
        const QString queuedConnectRequestId = QString::number(queuedConnectStarted.last().at(1).toULongLong());
        QJsonObject queuedConnectTimeoutResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (queuedConnectTimeoutResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                             {{QStringLiteral("requestId"), queuedConnectRequestId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("error"),
            2000);
        QCOMPARE(queuedConnectTimeoutResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CONNECT_TIMEOUT_UNCONFIRMED"));
        QCOMPARE(queuedOperationsSettled.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(manager.hasPendingCameraOperation(QString::fromLatin1(kDeviceId)), false, 2000);

        manager.setAsyncDisconnectTimeoutForTesting(100);
        QSignalSpy queuedDisconnectStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject queuedDisconnectPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(queuedDisconnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(queuedDisconnectStarted.count(), 1, 2000);
        const QString queuedDisconnectRequestId = QString::number(queuedDisconnectStarted.last().at(1).toULongLong());
        QJsonObject queuedDisconnectTimeoutResult;
        QTRY_VERIFY_WITH_TIMEOUT((queuedDisconnectTimeoutResult =
                                      actor->executeTool(QStringLiteral("get_camera_operation"),
                                                         {{QStringLiteral("requestId"), queuedDisconnectRequestId}}))
                                         .value(QStringLiteral("status"))
                                         .toString() == QStringLiteral("error"),
                                 2000);
        QCOMPARE(queuedDisconnectTimeoutResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_DISCONNECT_TIMEOUT_UNCONFIRMED"));
        QCOMPARE(queuedDisconnectTimeoutResult.value(QStringLiteral("sideEffectStatus")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(queuedDisconnectTimeoutResult.value(QStringLiteral("connectionState")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(queuedDisconnectTimeoutResult.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(queuedDisconnectTimeoutResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), false);
        QCOMPARE(queuedOperationsSettled.count(), 2);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, queuedConnectCallsBeforeTimeout + 1);
            QCOMPARE(state.disconnectCalls, queuedDisconnectCallsBeforeTimeout);
            QCOMPARE(state.connectReturnedCalls, queuedConnectReturnsBeforeTimeout);
            QCOMPARE(state.disconnectReturnedCalls, queuedDisconnectReturnsBeforeTimeout);
            state.releaseConnect = true;
            state.connectGate.wakeAll();
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                QMutexLocker locker(&state.mutex);
                return state.connectReturnedCalls == queuedConnectReturnsBeforeTimeout + 1 &&
                       state.disconnectReturnedCalls == queuedDisconnectReturnsBeforeTimeout + 1;
            }(),
            2000);
        QTest::qWait(50);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), queuedConnectRequestId}}),
                 queuedConnectTimeoutResult);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), queuedDisconnectRequestId}}),
                 queuedDisconnectTimeoutResult);
        QCOMPARE(manager.cameraSessionCount(), 1);
        QCOMPARE(queuedOperationsSettled.count(), 2);
        const QJsonObject queuedTimeoutCleanupPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(queuedTimeoutCleanupPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(queuedDisconnectStarted.count(), 2, 2000);
        const QString queuedTimeoutCleanupId = QString::number(queuedDisconnectStarted.last().at(1).toULongLong());
        QJsonObject queuedTimeoutCleanupResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (queuedTimeoutCleanupResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                             {{QStringLiteral("requestId"), queuedTimeoutCleanupId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QCOMPARE(queuedTimeoutCleanupResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QCOMPARE(queuedOperationsSettled.count(), 3);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), queuedConnectRequestId}}),
                 queuedConnectTimeoutResult);
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), queuedDisconnectRequestId}}),
                 queuedDisconnectTimeoutResult);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, queuedConnectCallsBeforeTimeout + 1);
            QCOMPARE(state.disconnectCalls, queuedDisconnectCallsBeforeTimeout + 1);
            state.blockConnect = false;
            state.releaseConnect = false;
        }
        manager.setAsyncConnectTimeoutForTesting(30000);
        manager.setAsyncDisconnectTimeoutForTesting(30000);

        QSignalSpy uncertainConnectStarted(&manager, &CameraManager::cameraOperationStarted);
        int connectCallsBeforeUncertain = 0;
        {
            QMutexLocker locker(&state.mutex);
            connectCallsBeforeUncertain = state.connectCalls;
            state.connectReportsFailureAfterConnecting = true;
        }
        const QJsonObject uncertainConnectPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(uncertainConnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(uncertainConnectStarted.count(), 1, 2000);
        const QString uncertainConnectId = QString::number(uncertainConnectStarted.last().at(1).toULongLong());
        QJsonObject uncertainConnectResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (uncertainConnectResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                         {{QStringLiteral("requestId"), uncertainConnectId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("error"),
            2000);
        QCOMPARE(uncertainConnectResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CONNECT_UNCONFIRMED"));
        QCOMPARE(uncertainConnectResult.value(QStringLiteral("sideEffectStatus")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(uncertainConnectResult.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(uncertainConnectResult.value(QStringLiteral("connectionState")).toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(manager.cameraSessionCount(), 1);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), uncertainConnectId}}),
                 uncertainConnectResult);

        QSignalSpy uncertainRetryExecuted(actor, &AgentActor::toolExecuted);
        const QJsonObject uncertainRetryPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(uncertainRetryPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(uncertainRetryExecuted.count(), 1, 2000);
        QCOMPARE(uncertainRetryExecuted.last().at(1).toJsonObject().value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_OPERATION_BUSY"));
        QCOMPARE(uncertainConnectStarted.count(), 1);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, connectCallsBeforeUncertain + 1);
        }

        QSignalSpy uncertainCleanupStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject uncertainCleanupPending = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(uncertainCleanupPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(uncertainCleanupStarted.count(), 1, 2000);
        const QString uncertainCleanupId = QString::number(uncertainCleanupStarted.last().at(1).toULongLong());
        QJsonObject uncertainCleanupResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (uncertainCleanupResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                         {{QStringLiteral("requestId"), uncertainCleanupId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QCOMPARE(uncertainCleanupResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QVERIFY(!manager.isConnected(QString::fromLatin1(kDeviceId)));
        {
            QMutexLocker locker(&state.mutex);
            state.connectReportsFailureAfterConnecting = false;
        }

        int connectCallsBeforeThrownConnect = 0;
        {
            QMutexLocker locker(&state.mutex);
            connectCallsBeforeThrownConnect = state.connectCalls;
            state.throwOnConnectAfterConnecting = true;
        }
        QSignalSpy thrownConnectStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject thrownConnectPending = controller.handleToolCall(
            QStringLiteral("connect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(thrownConnectPending.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(thrownConnectStarted.count(), 1, 2000);
        const QString thrownConnectId = QString::number(thrownConnectStarted.last().at(1).toULongLong());
        QJsonObject thrownConnectResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (thrownConnectResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                      {{QStringLiteral("requestId"), thrownConnectId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("error"),
            2000);
        QCOMPARE(thrownConnectResult.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_OPERATION_EXCEPTION"));
        QCOMPARE(thrownConnectResult.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(thrownConnectResult.value(QStringLiteral("retryable")).toBool(), false);
        QCOMPARE(thrownConnectResult.value(QStringLiteral("connectionState")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(actor->executeTool(QStringLiteral("get_camera_operation"),
                                    {{QStringLiteral("requestId"), thrownConnectId}}),
                 thrownConnectResult);
        QCOMPARE(manager.cameraSessionCount(), 1);

        QSignalSpy thrownConnectCleanupStarted(&manager, &CameraManager::cameraOperationStarted);
        const QJsonObject thrownConnectCleanup = controller.handleToolCall(
            QStringLiteral("disconnect_camera"), {{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}});
        QCOMPARE(thrownConnectCleanup.value(QStringLiteral("status")).toString(),
                 QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(thrownConnectCleanupStarted.count(), 1, 2000);
        const QString thrownConnectCleanupId = QString::number(thrownConnectCleanupStarted.last().at(1).toULongLong());
        QJsonObject thrownConnectCleanupResult;
        QTRY_VERIFY_WITH_TIMEOUT(
            (thrownConnectCleanupResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                             {{QStringLiteral("requestId"), thrownConnectCleanupId}}))
                    .value(QStringLiteral("status"))
                    .toString() == QStringLiteral("success"),
            2000);
        QCOMPARE(thrownConnectCleanupResult.value(QStringLiteral("hardwareDisconnectConfirmed")).toBool(), true);
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.connectCalls, connectCallsBeforeThrownConnect + 1);
            state.throwOnConnectAfterConnecting = false;
        }

        CameraConfirmationScriptedLLM chatLlm;
        const auto chatConnectCall = [](const QString& id) {
            return QJsonObject{{QStringLiteral("id"), id},
                               {QStringLiteral("type"), QStringLiteral("function")},
                               {QStringLiteral("name"), QStringLiteral("connect_camera")},
                               {QStringLiteral("arguments"),
                                QJsonObject{{QStringLiteral("deviceId"), QString::fromLatin1(kDeviceId)}}}};
        };
        chatLlm.toolCallTurns = {QJsonArray{chatConnectCall(QStringLiteral("camera-chat-stale"))},
                                 QJsonArray{chatConnectCall(QStringLiteral("camera-chat-fresh"))}, QJsonArray{}};
        {
            ScopedControllerLLM scopedLlm(controller, &chatLlm);
            QSignalSpy chatPending(&controller, &AgentController::toolsPendingConfirmation);
            QSignalSpy chatConnectStarted(&manager, &CameraManager::cameraOperationStarted);
            QSignalSpy chatToolExecuted(actor, &AgentActor::toolExecuted);
            int connectCallsBeforeChat = 0;
            {
                QMutexLocker locker(&state.mutex);
                connectCallsBeforeChat = state.connectCalls;
                QCOMPARE(state.discoveredIpAddress, QStringLiteral("192.0.2.10"));
            }

            controller.sendUserMessage(QStringLiteral("connect the fake camera"));
            QTRY_COMPARE_WITH_TIMEOUT(chatPending.count(), 1, 2000);
            QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
            const QString chatStaleId = controller.pendingConfirmationId();
            QVERIFY(!chatStaleId.isEmpty());
            {
                QMutexLocker locker(&state.mutex);
                state.discoveredIpAddress = QStringLiteral("192.0.2.12");
            }
            QSignalSpy chatDriftSettled(&manager, &CameraManager::cameraDiscoverySettled);
            const quint64 chatDriftRequestId = 70005;
            QString chatDriftError;
            QVERIFY2(manager.startDiscoveryAsync(QString::fromLatin1(kPluginId), chatDriftRequestId, &chatDriftError),
                     qPrintable(chatDriftError));
            QTRY_VERIFY_WITH_TIMEOUT(std::any_of(chatDriftSettled.cbegin(), chatDriftSettled.cend(),
                                                 [chatDriftRequestId](const QList<QVariant>& args) {
                                                     return args.at(1).toULongLong() == chatDriftRequestId &&
                                                            args.at(2).toString() == QLatin1String("success");
                                                 }),
                                     2000);
            const int auditsBeforeChatStaleConfirmation = audit.count();
            QVERIFY(controller.confirmPendingTools(chatStaleId));
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 5000);
            QTRY_COMPARE_WITH_TIMEOUT(audit.count(), auditsBeforeChatStaleConfirmation + 1, 5000);
            QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
            QCOMPARE(chatConnectStarted.count(), 0);
            QCOMPARE(chatToolExecuted.count(), 0);
            {
                QMutexLocker locker(&state.mutex);
                QCOMPARE(state.connectCalls, connectCallsBeforeChat);
                state.discoveredIpAddress = QStringLiteral("192.0.2.10");
            }
            QSignalSpy chatRestoreSettled(&manager, &CameraManager::cameraDiscoverySettled);
            const quint64 chatRestoreRequestId = 70006;
            QString chatRestoreError;
            QVERIFY2(
                manager.startDiscoveryAsync(QString::fromLatin1(kPluginId), chatRestoreRequestId, &chatRestoreError),
                qPrintable(chatRestoreError));
            QTRY_VERIFY_WITH_TIMEOUT(std::any_of(chatRestoreSettled.cbegin(), chatRestoreSettled.cend(),
                                                 [chatRestoreRequestId](const QList<QVariant>& args) {
                                                     return args.at(1).toULongLong() == chatRestoreRequestId &&
                                                            args.at(2).toString() == QLatin1String("success");
                                                 }),
                                     2000);

            controller.sendUserMessage(QStringLiteral("confirm the restored fake camera"));
            QTRY_COMPARE_WITH_TIMEOUT(chatPending.count(), 2, 2000);
            QCOMPARE(controller.state(), AgentController::AgentState::Confirming);
            const QString chatFreshId = controller.pendingConfirmationId();
            QVERIFY(!chatFreshId.isEmpty());
            const QJsonArray chatEffects = controller.pendingConfirmationPayload()
                                               .value(QStringLiteral("context"))
                                               .toObject()
                                               .value(QStringLiteral("sideEffects"))
                                               .toArray();
            QVERIFY(chatEffects.first().toString().contains(QStringLiteral("ip=192.0.2.10")));
            QVERIFY(controller.confirmPendingTools(chatFreshId));
            QTRY_COMPARE_WITH_TIMEOUT(chatConnectStarted.count(), 1, 2000);
            const QString chatConnectRequestId = QString::number(chatConnectStarted.first().at(1).toULongLong());
            QJsonObject chatConnectResult;
            QTRY_VERIFY_WITH_TIMEOUT(
                (chatConnectResult = actor->executeTool(QStringLiteral("get_camera_operation"),
                                                        {{QStringLiteral("requestId"), chatConnectRequestId}}))
                        .value(QStringLiteral("status"))
                        .toString() == QLatin1String("success"),
                2000);
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 2000);
            int chatConnectToolExecuted = 0;
            for (const QList<QVariant>& args : chatToolExecuted)
                chatConnectToolExecuted += args.at(0).toString() == QLatin1String("connect_camera") ? 1 : 0;
            QCOMPARE(chatConnectToolExecuted, 1);
            {
                QMutexLocker locker(&state.mutex);
                QCOMPARE(state.connectCalls, connectCallsBeforeChat + 1);
            }

            quint64 chatCleanupRequestId = 0;
            QString chatCleanupError;
            QVERIFY2(manager.requestDisconnectCameraAsync(QString::fromLatin1(kDeviceId), &chatCleanupRequestId,
                                                          &chatCleanupError),
                     qPrintable(chatCleanupError));
            QJsonObject chatCleanupResult;
            QTRY_VERIFY_WITH_TIMEOUT(manager.getCameraOperationResult(chatCleanupRequestId, &chatCleanupResult) &&
                                         chatCleanupResult.value(QStringLiteral("status")).toString() ==
                                             QLatin1String("success"),
                                     2000);
            QTRY_VERIFY_WITH_TIMEOUT(!manager.isConnected(QString::fromLatin1(kDeviceId)), 2000);
            QCOMPARE(chatLlm.requestCount, 3);
        }

        RunEngine& cycleEngine = RunEngine::instance();
        cycleEngine.stop();
        engine.clearModules();
        ProjectManager::instance().closeProject();
        FakeCameraState legacyState;
        legacyState.deviceId = QString::fromLatin1(kLegacyDeviceId);
        auto* legacyPlugin = new FakeLegacyCameraPlugin(&legacyState);
        manager.registerCameraPlugin(legacyPlugin);
        QVERIFY(!manager.supportsAsyncOperations(QString::fromLatin1(kLegacyDeviceId)));
        Project* legacyProject = ProjectManager::instance().newProject();
        QVERIFY(legacyProject);
        ModuleInstance legacyGrab = grabInstance;
        legacyGrab.id = QStringLiteral("g5-agent-legacy-grab");
        legacyGrab.name = QStringLiteral("G5 legacy camera cycle GrabImage");
        legacyGrab.params.insert(QStringLiteral("cameraId"), QString::fromLatin1(kLegacyDeviceId));
        legacyProject->addModule(legacyGrab);
        QSignalSpy legacyRunFinished(&cycleEngine, &RunEngine::runFinished);
        QSignalSpy legacyModuleStarted(&cycleEngine, &RunEngine::moduleStarted);
        QSignalSpy legacyModuleFinished(&cycleEngine, &RunEngine::moduleFinished);
        QSignalSpy legacyCycleStarted(&cycleEngine, &RunEngine::cycleStarted);
        QSignalSpy legacyCycleStopped(&cycleEngine, &RunEngine::cycleStopped);
        const auto legacyCounters = [&legacyState]() {
            QMutexLocker locker(&legacyState.mutex);
            return std::array<int, 5>{legacyState.factoryCalls, legacyState.connectCalls, legacyState.triggerCalls,
                                      legacyState.startCalls, legacyState.destroyed};
        };
        const auto legacyBeforeReject = legacyCounters();

        FakeCameraState pendingDiscoveryState;
        pendingDiscoveryState.deviceId = QStringLiteral("g5-pending-discovery-device");
        pendingDiscoveryState.pluginId = QStringLiteral("com.deeplux.test.agent-camera-g5-pending-discovery");
        pendingDiscoveryState.discoveryEnabled = false;
        auto* pendingDiscoveryPlugin = new FakeAsyncCameraPlugin(&pendingDiscoveryState);
        manager.registerCameraPlugin(pendingDiscoveryPlugin);
        QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 2000);
        QVERIFY(
            !manager.cameraLifecycleSnapshot(pendingDiscoveryState.deviceId).value(QStringLiteral("found")).toBool());
        {
            QMutexLocker locker(&pendingDiscoveryState.mutex);
            pendingDiscoveryState.discoveryEnabled = true;
        }

        const QJsonObject futureDiscoveryTargets =
            actor->executeTool(QStringLiteral("get_camera_discovery_targets"), {});
        const QJsonObject futureDiscoveryArgs{
            {QStringLiteral("registryGeneration"), futureDiscoveryTargets.value(QStringLiteral("registryGeneration"))},
            {QStringLiteral("pluginIds"), futureDiscoveryTargets.value(QStringLiteral("asyncPluginIds"))},
            {QStringLiteral("unsupportedPluginIds"),
             futureDiscoveryTargets.value(QStringLiteral("unsupportedPluginIds"))}};
        QVERIFY(
            legacyProject->setModuleParam(legacyGrab.id, QStringLiteral("cameraId"), pendingDiscoveryState.deviceId));
        CameraConfirmationScriptedLLM discoveryCycleLlm;
        discoveryCycleLlm.toolCallTurns = {QJsonArray{
            QJsonObject{{QStringLiteral("id"), QStringLiteral("future_discovery")},
                        {QStringLiteral("name"), QStringLiteral("discover_cameras")},
                        {QStringLiteral("arguments"), futureDiscoveryArgs}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("future_cycle")},
                        {QStringLiteral("name"), QStringLiteral("run_flow")},
                        {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                  {QStringLiteral("maxCycles"), 2}}}}}};
        QSignalSpy discoveryCycleError(&controller, &AgentController::llmErrorOccurred);
        int discoveryCallsBeforeBatch = 0;
        {
            QMutexLocker locker(&pendingDiscoveryState.mutex);
            discoveryCallsBeforeBatch = pendingDiscoveryState.discoveryCalls;
        }
        const int discoveryToolsBeforeBatch = captureToolExecuted.count();
        {
            ScopedControllerLLM scopedLlm(controller, &discoveryCycleLlm);
            controller.clearConversation();
            controller.sendUserMessage(QStringLiteral("Discover then run the unknown camera cycle"));
            QTRY_COMPARE_WITH_TIMEOUT(discoveryCycleError.count(), 1, 2000);
            const QString summary = discoveryCycleError.takeFirst().at(0).toString();
            QVERIFY(summary.contains(QStringLiteral("future_discovery")));
            QVERIFY(summary.contains(QStringLiteral("future_cycle")));
            QVERIFY(summary.contains(QStringLiteral("CAMERA_CYCLE_UNSUPPORTED")));
            controller.sendUserMessage(QStringLiteral("Verify the discovery batch result"));
            int batchResults = 0;
            for (const AgentMessage& message : discoveryCycleLlm.lastContext.messages) {
                if (message.role != QLatin1String("tool") || (message.toolCallId != QLatin1String("future_discovery") &&
                                                              message.toolCallId != QLatin1String("future_cycle")))
                    continue;
                ++batchResults;
                QJsonParseError parseError;
                const QJsonDocument parsed = QJsonDocument::fromJson(message.content.toUtf8(), &parseError);
                QVERIFY(parseError.error == QJsonParseError::NoError && parsed.isObject());
                QCOMPARE(parsed.object().value(QStringLiteral("errorCode")).toString(),
                         message.toolCallId == QLatin1String("future_cycle")
                             ? QStringLiteral("CAMERA_CYCLE_UNSUPPORTED")
                             : QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"));
            }
            QCOMPARE(batchResults, 2);
            QTRY_COMPARE_WITH_TIMEOUT(controller.state(), AgentController::AgentState::Idle, 2000);
        }
        QCOMPARE(manager.pendingDiscoveryCount(), 0);
        QVERIFY(
            !manager.cameraLifecycleSnapshot(pendingDiscoveryState.deviceId).value(QStringLiteral("found")).toBool());
        QCOMPARE(legacyCounters(), legacyBeforeReject);
        QCOMPARE(legacyRunFinished.count(), 0);
        QCOMPARE(legacyModuleStarted.count(), 0);
        QCOMPARE(legacyModuleFinished.count(), 0);
        QCOMPARE(legacyCycleStarted.count(), 0);
        QCOMPARE(legacyCycleStopped.count(), 0);
        QCOMPARE(captureToolExecuted.count(), discoveryToolsBeforeBatch);
        {
            QMutexLocker locker(&pendingDiscoveryState.mutex);
            QCOMPARE(pendingDiscoveryState.discoveryCalls, discoveryCallsBeforeBatch);
            QCOMPARE(pendingDiscoveryState.factoryCalls, 0);
            QCOMPARE(pendingDiscoveryState.connectCalls, 0);
            QCOMPARE(pendingDiscoveryState.triggerCalls, 0);
        }
        QVERIFY(!cycleEngine.isBusy());
        QVERIFY(legacyProject->setModuleParam(legacyGrab.id, QStringLiteral("cameraId"),
                                              QString::fromLatin1(kLegacyDeviceId)));
        const int legacyToolsBeforeReject = captureToolExecuted.count();

        QString pendingDiscoveryError;
        const quint64 pendingDiscoveryRequestId = 88001;
        QVERIFY2(manager.startDiscoveryAsync(pendingDiscoveryState.pluginId, pendingDiscoveryRequestId,
                                             &pendingDiscoveryError),
                 qPrintable(pendingDiscoveryError));
        QCOMPARE(manager.pendingDiscoveryCount(), 1);
        int discoveryCallsAtCyclePreflight = 0;
        {
            QMutexLocker locker(&pendingDiscoveryState.mutex);
            discoveryCallsAtCyclePreflight = pendingDiscoveryState.discoveryCalls;
        }
        const QJsonObject pendingDiscoveryCycleRejected =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(pendingDiscoveryCycleRejected.value(QStringLiteral("errorCode")).toString(),
                 QStringLiteral("CAMERA_CYCLE_UNSUPPORTED"));
        QVERIFY(controller.pendingConfirmationId().isEmpty());
        QCOMPARE(manager.pendingDiscoveryCount(), 1);
        QCOMPARE(legacyCounters(), legacyBeforeReject);
        QCOMPARE(legacyRunFinished.count(), 0);
        QCOMPARE(legacyModuleStarted.count(), 0);
        QCOMPARE(legacyModuleFinished.count(), 0);
        QCOMPARE(legacyCycleStarted.count(), 0);
        QCOMPARE(legacyCycleStopped.count(), 0);
        QCOMPARE(captureToolExecuted.count(), legacyToolsBeforeReject);
        {
            QMutexLocker locker(&pendingDiscoveryState.mutex);
            QCOMPARE(pendingDiscoveryState.discoveryCalls, discoveryCallsAtCyclePreflight);
            QCOMPARE(pendingDiscoveryState.factoryCalls, 0);
            QCOMPARE(pendingDiscoveryState.connectCalls, 0);
            QCOMPARE(pendingDiscoveryState.triggerCalls, 0);
        }
        QVERIFY(!cycleEngine.isBusy());
        QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 2000);
        QVERIFY(manager.tryUnregisterCameraPlugin(pendingDiscoveryState.pluginId));
        delete pendingDiscoveryPlugin;

        QJsonObject legacyPending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(legacyPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString legacyStaleConfirmation = controller.pendingConfirmationId();
        QVERIFY(!legacyStaleConfirmation.isEmpty());

        QVERIFY(manager.tryUnregisterCameraPlugin(QString::fromLatin1(kLegacyPluginId)));
        delete legacyPlugin;
        legacyPlugin = nullptr;
        {
            QMutexLocker locker(&legacyState.mutex);
            legacyState.pluginId = QString::fromLatin1(kLegacyAsyncPluginId);
        }
        auto* replacementAsyncPlugin = new FakeAsyncCameraPlugin(&legacyState);
        manager.registerCameraPlugin(replacementAsyncPlugin);
        QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 2000);
        QVERIFY(manager.supportsAsyncOperations(QString::fromLatin1(kLegacyDeviceId)));
        const int auditsBeforeLegacyExpiry = captureAudit.count();
        QVERIFY(controller.confirmPendingTools(legacyStaleConfirmation));
        QTRY_COMPARE_WITH_TIMEOUT(captureAudit.count(), auditsBeforeLegacyExpiry + 1, 5000);
        QCOMPARE(captureAudit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(legacyCounters(), legacyBeforeReject);
        QCOMPARE(legacyRunFinished.count(), 0);
        QCOMPARE(legacyModuleStarted.count(), 0);
        QCOMPARE(legacyModuleFinished.count(), 0);
        QCOMPARE(legacyCycleStarted.count(), 0);
        QCOMPARE(legacyCycleStopped.count(), 0);
        QCOMPARE(captureToolExecuted.count(), legacyToolsBeforeReject);
        QCOMPARE(cycleEngine.cycleSessionId(), QString());
        QVERIFY(!cycleEngine.isBusy());
        QVERIFY(manager.tryUnregisterCameraPlugin(QString::fromLatin1(kLegacyAsyncPluginId)));
        delete replacementAsyncPlugin;
        {
            QMutexLocker locker(&legacyState.mutex);
            legacyState.pluginId = QString::fromLatin1(kLegacyPluginId);
        }
        legacyPlugin = new FakeLegacyCameraPlugin(&legacyState);
        manager.registerCameraPlugin(legacyPlugin);
        QVERIFY(!manager.supportsAsyncOperations(QString::fromLatin1(kLegacyDeviceId)));

        legacyPending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(legacyPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString legacyRejectedId = controller.pendingConfirmationId();
        QVERIFY(!legacyRejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(legacyRejectedId));
        QCOMPARE(legacyCounters(), legacyBeforeReject);
        QCOMPARE(legacyRunFinished.count(), 0);
        QCOMPARE(legacyModuleStarted.count(), 0);
        QCOMPARE(legacyModuleFinished.count(), 0);
        QCOMPARE(legacyCycleStarted.count(), 0);
        QCOMPARE(legacyCycleStopped.count(), 0);
        QCOMPARE(captureToolExecuted.count(), legacyToolsBeforeReject);
        QCOMPARE(cycleEngine.cycleSessionId(), QString());
        QVERIFY(!cycleEngine.isBusy());

        legacyPending =
            controller.handleToolCall(QStringLiteral("run_flow"), {{QStringLiteral("mode"), QStringLiteral("cycle")},
                                                                   {QStringLiteral("maxCycles"), 2}, {QStringLiteral("wait"), false}});
        QCOMPARE(legacyPending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString legacyApprovedId = controller.pendingConfirmationId();
        QVERIFY(!legacyApprovedId.isEmpty());
        QVERIFY(legacyApprovedId != legacyRejectedId);
        QVERIFY(controller.confirmPendingTools(legacyApprovedId));
        QTRY_COMPARE_WITH_TIMEOUT(legacyRunFinished.count(), 2, 5000);
        QCOMPARE(legacyCycleStopped.count(), 1);
        QCOMPARE(legacyCycleStarted.count(), 0);
        QCOMPARE(legacyModuleStarted.count(), 2);
        QCOMPARE(legacyModuleFinished.count(), 2);
        QTRY_COMPARE_WITH_TIMEOUT(captureToolExecuted.count(), legacyToolsBeforeReject + 1, 2000);
        const QJsonObject legacyCycleStartedResult = captureToolExecuted.last().at(1).toJsonObject();
        QCOMPARE(legacyCycleStartedResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString legacySessionId = legacyCycleStartedResult.value(QStringLiteral("sessionId")).toString();
        QVERIFY(!legacySessionId.isEmpty());
        QJsonObject legacySession;
        QTRY_COMPARE_WITH_TIMEOUT((legacySession = actor->executeTool(QStringLiteral("get_run_results"),
                                                                      {{QStringLiteral("runId"), legacySessionId}}))
                                      .value(QStringLiteral("status"))
                                      .toString(),
                                  QStringLiteral("completed"), 2000);
        QCOMPARE(legacySession.value(QStringLiteral("maxCycles")).toInt(), 2);
        QCOMPARE(legacySession.value(QStringLiteral("completedCycles")).toInt(), 2);
        const auto legacyAfterCycle = legacyCounters();
        QCOMPARE(legacyAfterCycle[0], legacyBeforeReject[0] + 1);
        QCOMPARE(legacyAfterCycle[1], legacyBeforeReject[1] + 1);
        QCOMPARE(legacyAfterCycle[2], legacyBeforeReject[2] + 2);
        QVERIFY(legacyAfterCycle[3] >= legacyBeforeReject[3] + 1);
        QVERIFY(legacyAfterCycle[3] <= legacyBeforeReject[3] + 2);
        for (int elapsed = 0; elapsed < 350; elapsed += 10) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            QTest::qWait(10);
        }
        QCOMPARE(legacyCounters(), legacyAfterCycle);
        QTRY_COMPARE_WITH_TIMEOUT(legacyRunFinished.count(), 2, 5000);
        QCOMPARE(legacyCycleStopped.count(), 1);
        QCOMPARE(legacyModuleStarted.count(), 2);
        QCOMPARE(legacyModuleFinished.count(), 2);
        QVERIFY(!cycleEngine.isBusy());
        cycleEngine.clearModules();
        ProjectManager::instance().closeProject();
        manager.disconnectCamera(QString::fromLatin1(kLegacyDeviceId));
        QTRY_VERIFY_WITH_TIMEOUT(!manager.isConnected(QString::fromLatin1(kLegacyDeviceId)), 2000);
        QVERIFY(manager.tryUnregisterCameraPlugin(QString::fromLatin1(kLegacyPluginId)));
        delete legacyPlugin;

        QVERIFY(agentConnect());
        {
            QMutexLocker locker(&state.mutex);
            state.blockTrigger = true;
            state.releaseTrigger = false;
            state.triggerEntered = false;
        }
        QSignalSpy shutdownSpy(&manager, &CameraManager::shutdownCompleted);
        std::atomic<bool> shutdownCaptureDone{false};
        CameraCaptureResult shutdownCapture;
        std::thread shutdownCaptureWorker([&]() {
            shutdownCapture = manager.captureAsyncSessionImage(QString::fromLatin1(kDeviceId), 3600.0, 4.0, 5000);
            shutdownCaptureDone.store(true, std::memory_order_release);
        });
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                QMutexLocker locker(&state.mutex);
                return state.triggerEntered;
            }(),
            2000);
        manager.shutdownAsync();
        QTRY_VERIFY_WITH_TIMEOUT(shutdownCaptureDone.load(std::memory_order_acquire), 2000);
        shutdownCaptureWorker.join();
        QCOMPARE(shutdownCapture.errorCode, QStringLiteral("CAMERA_GRAB_CANCELLED"));
        QCOMPARE(shutdownSpy.count(), 0);
        QCOMPARE(manager.cameraSessionCount(), 1);
        {
            QMutexLocker locker(&state.mutex);
            state.releaseTrigger = true;
            state.triggerGate.wakeAll();
        }
        QTRY_COMPARE_WITH_TIMEOUT(manager.cameraSessionCount(), 0, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(shutdownSpy.count(), 1, 2000);
        QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 2000);
        {
            QMutexLocker locker(&state.mutex);
            QCOMPARE(state.triggerCalls, 13);
            QCOMPARE(state.exposureSetCalls, 16);
            QCOMPARE(state.gainSetCalls, 16);
            QCOMPARE(state.disconnectCalls, 29);
            QCOMPARE(state.destroyed, 29);
            QCOMPARE(state.wrongThreadCalls, 0);
            QCOMPARE(state.stopCalls, state.startCalls);
        }

        grab.shutdown();
        modulePlugins.shutdown();
        QVERIFY(manager.tryUnregisterCameraPlugin(QString::fromLatin1(kPluginId)));
        delete plugin;
        controller.clearConversation();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }
};

QTEST_MAIN(TestAgentCameraLifecycleG5)
#include "test_agent_camera_lifecycle_g5.moc"
