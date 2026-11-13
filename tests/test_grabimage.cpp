#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/device/CameraManager.h"
#include "core/interface/ICamera.h"
#include "core/interface/ICameraPlugin.h"
#include "core/model/ImageData.h"
#include "plugins/image_processing/GrabImage/GrabImagePlugin.h"

#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

namespace {

struct FakeCameraState {
    CameraManager* manager = nullptr;
    QString deviceId;
    bool provideImage = false;
    bool createSucceeds = true;
    bool connectSucceeds = true;
    bool throwOnGainSet = false;
    bool heldDuringDisconnect = false;
    int destroyed = 0;
    int connectCalls = 0;
    int triggerCalls = 0;
    double exposure = 0;
    double gain = 0;
};

class FakeCamera final : public ICamera {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICamera)

public:
    explicit FakeCamera(FakeCameraState* state) : m_state(state) {}
    ~FakeCamera() override {
        ++m_state->destroyed;
    }

    QString deviceId() const override {
        return m_state->deviceId;
    }
    QString name() const override {
        return QStringLiteral("fake camera");
    }
    QString serialNumber() const override {
        return QStringLiteral("fake-serial");
    }
    QString manufacturer() const override {
        return QStringLiteral("test");
    }
    CameraCapabilities capabilities() const override {
        return {};
    }
    bool isPlatformSupported() const override {
        return true;
    }
    bool connect() override {
        ++m_state->connectCalls;
        m_connected = m_state->connectSucceeds;
        return m_connected;
    }
    void disconnect() override {
        m_connected = false;
        m_acquiring = false;
    }
    bool isConnected() const override {
        return m_connected;
    }
    bool startAcquisition() override {
        m_acquiring = true;
        return true;
    }
    void stopAcquisition() override {
        m_acquiring = false;
    }
    bool isAcquiring() const override {
        return m_acquiring;
    }
    bool triggerSoftware() override {
        ++m_state->triggerCalls;
        FakeCamera* camera = this;
        FakeCameraState* state = m_state;
        const int destroyedBeforeDisconnect = state->destroyed;
        QTimer::singleShot(0, this, [camera, state, destroyedBeforeDisconnect] {
            state->manager->disconnectCamera(state->deviceId);
            if (state->manager->getCamera(state->deviceId) != camera)
                return;
            state->heldDuringDisconnect = state->destroyed == destroyedBeforeDisconnect;
            if (state->provideImage) {
                camera->m_image = QImage(3, 2, QImage::Format_RGB32);
                camera->m_image.fill(qRgb(17, 83, 149));
                emit camera->imageReceived(camera->m_image, {});
            }
        });
        return true;
    }
    void setTriggerMode(TriggerMode mode) override {
        m_triggerMode = mode;
    }
    TriggerMode triggerMode() const override {
        return m_triggerMode;
    }
    void setExposureTime(double value) override {
        m_state->exposure = value;
    }
    double exposureTime() const override {
        return m_state->exposure;
    }
    void setGain(double value) override {
        if (m_state->throwOnGainSet)
            throw std::runtime_error("fake gain setter failure after exposure update");
        m_state->gain = value;
    }
    double gain() const override {
        return m_state->gain;
    }
    void setFrameRate(double) override {}
    double frameRate() const override {
        return 0;
    }
    void setRoi(int, int, int, int) override {}
    QRect roi() const override {
        return {};
    }
    QImage lastImage() const override {
        return m_image;
    }
    QVariantMap lastImageMetadata() const override {
        return {};
    }
    int imageWidth() const override {
        return m_image.width();
    }
    int imageHeight() const override {
        return m_image.height();
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

private:
    FakeCameraState* m_state;
    bool m_connected = false;
    bool m_acquiring = false;
    TriggerMode m_triggerMode = TriggerMode::Software;
    QImage m_image;
};

class FakeCameraPlugin final : public ICameraPlugin {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICameraPlugin)

public:
    explicit FakeCameraPlugin(FakeCameraState* state) : m_state(state) {}
    QString pluginId() const override {
        return QStringLiteral("test.grabimage.camera");
    }
    QString pluginName() const override {
        return QStringLiteral("GrabImage test camera");
    }
    QString manufacturer() const override {
        return QStringLiteral("test");
    }
    bool isAvailable() const override {
        return true;
    }
    QString availabilityMessage() const override {
        return {};
    }
    QList<CameraInfo> discoverCameras() override {
        CameraInfo info;
        info.pluginId = pluginId();
        info.deviceId = m_state->deviceId;
        info.name = QStringLiteral("fake camera");
        return {info};
    }
    QObject* createCamera(const CameraInfo&) override {
        if (!m_state->createSucceeds)
            return nullptr;
        return new FakeCamera(m_state);
    }

private:
    FakeCameraState* m_state;
};

} // namespace

class TestableGrabImage final : public GrabImagePlugin {
public:
    using GrabImagePlugin::GrabImagePlugin;

    bool openFallbackCamera(int cameraIndex) override {
        ++fallbackOpenCalls;
        lastFallbackIndex = cameraIndex;
        return false;
    }

    int fallbackOpenCalls = 0;
    int lastFallbackIndex = -1;
};

class TestGrabImage : public QObject {
    Q_OBJECT

private slots:
    void readsFolderInNameOrder();
    void cameraFrameAndTimeoutRespectLeaseLifetime();
};

void TestGrabImage::cameraFrameAndTimeoutRespectLeaseLifetime() {
    CameraManager& manager = CameraManager::instance();
    const QString deviceId = QStringLiteral("test-grabimage-camera");
    FakeCameraState state{&manager, deviceId};
    FakeCameraPlugin cameraPlugin(&state);
    struct Cleanup {
        CameraManager& manager;
        QString pluginId;
        ~Cleanup() {
            manager.disconnectAll();
            manager.tryUnregisterCameraPlugin(pluginId);
        }
    };
    Cleanup cleanup{manager, cameraPlugin.pluginId()};

    manager.registerCameraPlugin(&cameraPlugin);
    QCOMPARE(manager.discoverCameras().size(), 1);

    TestableGrabImage plugin;
    QVERIFY(plugin.initialize());
    plugin.setParams(QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("Camera")},
                                 {QStringLiteral("cameraId"), deviceId},
                                 {QStringLiteral("grabTimeout"), 100}});
    ImageData input;
    ImageData output;
    QSignalSpy errors(&plugin, &ModuleBase::errorOccurred);

    state.createSucceeds = false;
    QVERIFY(!plugin.execute(input, output));
    QVERIFY(errors.count() >= 1);
    QCOMPARE(plugin.fallbackOpenCalls, 0);

    state.createSucceeds = true;
    state.connectSucceeds = false;
    QVERIFY(!plugin.execute(input, output));
    QVERIFY(errors.count() >= 2);
    QCOMPARE(state.destroyed, 1);
    QCOMPARE(plugin.fallbackOpenCalls, 0);
    state.connectSucceeds = true;

    state.provideImage = false;
    QElapsedTimer timeoutTimer;
    timeoutTimer.start();
    QVERIFY(!plugin.execute(input, output));
    QVERIFY(errors.count() >= 3);
    QVERIFY(timeoutTimer.elapsed() <= 350);
    QVERIFY(state.heldDuringDisconnect);
    QCOMPARE(state.destroyed, 2);
    QCOMPARE(plugin.fallbackOpenCalls, 0);
    QCOMPARE(manager.getCamera(deviceId), nullptr);
    QVERIFY(manager.tryUnregisterCameraPlugin(cameraPlugin.pluginId()));

    manager.registerCameraPlugin(&cameraPlugin);
    QCOMPARE(manager.discoverCameras().size(), 1);
    QVERIFY(manager.connectCamera(deviceId));
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    AgentActor actor;
    const QJsonObject details =
        actor.executeTool(QStringLiteral("get_camera_details"), {{QStringLiteral("deviceId"), deviceId}});
    QVERIFY2(!details.contains(QStringLiteral("error")), qPrintable(details.value(QStringLiteral("error")).toString()));
    const QString oldBinding = details.value(QStringLiteral("cameraSessionGeneration")).toString();
    QVERIFY(!oldBinding.isEmpty());
    state.throwOnGainSet = true;
    QVERIFY(!plugin.execute(input, output));
    state.throwOnGainSet = false;
    QVERIFY(manager.cameraParameterBindingVersion(deviceId) > oldBinding.toULongLong());
    const QJsonObject staleWrite = actor.executeTool(QStringLiteral("set_camera_parameter"),
                                                     {{QStringLiteral("deviceId"), deviceId},
                                                      {QStringLiteral("cameraSessionGeneration"), oldBinding},
                                                      {QStringLiteral("serialNumber"), QStringLiteral("fake-serial")},
                                                      {QStringLiteral("parameter"), QStringLiteral("exposureTime")},
                                                      {QStringLiteral("value"), 1000.0}});
    QCOMPARE(staleWrite.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_TARGET_CHANGED"));
    QCOMPARE(state.exposure, 10000.0);
    QVERIFY(UserManager::instance().login(QStringLiteral("operator"), QString()));
    manager.disconnectCamera(deviceId);
    QVERIFY(manager.tryUnregisterCameraPlugin(cameraPlugin.pluginId()));

    state.provideImage = true;
    state.heldDuringDisconnect = false;
    state.connectCalls = 0;
    state.triggerCalls = 0;
    manager.registerCameraPlugin(&cameraPlugin);
    QCOMPARE(manager.discoverCameras().size(), 1);

    QVERIFY(plugin.execute(input, output));
    QVERIFY(state.heldDuringDisconnect);
    QCOMPARE(state.destroyed, 4);
    QCOMPARE(plugin.fallbackOpenCalls, 0);
    QCOMPARE(manager.getCamera(deviceId), nullptr);
    QCOMPARE(state.connectCalls, 1);
    QCOMPARE(state.triggerCalls, 1);
    QCOMPARE(state.exposure, 10000.0);
    QCOMPARE(state.gain, 1.0);
    const QImage result = output.toQImage();
    QCOMPARE(result.size(), QSize(3, 2));
    QCOMPARE(result.pixelColor(0, 0), QColor(17, 83, 149));

    manager.disconnectAll();
    QVERIFY(manager.tryUnregisterCameraPlugin(cameraPlugin.pluginId()));
#ifdef DEEPLUX_HAS_OPENCV
    plugin.setParam(QStringLiteral("cameraId"), QStringLiteral("7"));
    QVERIFY(!plugin.execute(input, output));
    QCOMPARE(plugin.fallbackOpenCalls, 1);
    QCOMPARE(plugin.lastFallbackIndex, 7);
#endif
    plugin.shutdown();
}

void TestGrabImage::readsFolderInNameOrder() {
#ifndef DEEPLUX_HAS_OPENCV
    QSKIP("GrabImage file loading requires OpenCV");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage image(8, 6, QImage::Format_Mono);
    image.setColor(0, qRgb(255, 0, 0));
    image.fill(0);
    QVERIFY(image.save(directory.filePath(QStringLiteral("02.png"))));
    image.setColor(0, qRgb(0, 255, 0));
    QVERIFY(image.save(directory.filePath(QStringLiteral("01.png"))));

    GrabImagePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParam(QStringLiteral("filePath"), directory.path());
    plugin.setParam(QStringLiteral("folderLoop"), false);
    QCOMPARE(plugin.currentParams().value(QStringLiteral("grabSource")).toString(), QStringLiteral("Path"));

    ImageData input;
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data(QStringLiteral("source_file_name")).toString(), QStringLiteral("01.png"));
    QCOMPARE(output.data(QStringLiteral("folder_position")).toInt(), 1);
    QCOMPARE(output.data(QStringLiteral("folder_count")).toInt(), 2);
    QVERIFY(output.toQImage().pixelColor(0, 0).green() > 200);

    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data(QStringLiteral("source_file_name")).toString(), QStringLiteral("02.png"));
    QCOMPARE(output.data(QStringLiteral("folder_position")).toInt(), 2);
    QVERIFY(output.toQImage().pixelColor(0, 0).red() > 200);
    QVERIFY(!plugin.execute(input, output));

    plugin.setParam(QStringLiteral("folderLoop"), true);
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data(QStringLiteral("source_file_name")).toString(), QStringLiteral("01.png"));

    GrabImagePlugin legacyPlugin;
    legacyPlugin.setParams(
        {{QStringLiteral("grabSource"), QStringLiteral("Folder")}, {QStringLiteral("folderPath"), directory.path()}});
    QCOMPARE(legacyPlugin.currentParams().value(QStringLiteral("filePath")).toString(), directory.path());
    QCOMPARE(legacyPlugin.currentParams().value(QStringLiteral("grabSource")).toString(), QStringLiteral("Path"));
#endif
}

QTEST_MAIN(TestGrabImage)
#include "test_grabimage.moc"
