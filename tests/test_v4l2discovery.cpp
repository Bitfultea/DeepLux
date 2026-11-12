#include "V4L2CameraPlugin.h"
#include "core/agent/AgentActor.h"
#include "core/agent/ToolSchema.h"
#include "core/common/UserManager.h"
#include "core/device/CameraDiscoveryJob.h"
#include "core/device/CameraManager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSemaphore>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>
#include <atomic>
#include <memory>
#include <stdexcept>

using namespace DeepLux;

namespace {

class FakeV4L2System final : public IV4L2DiscoverySystem {
public:
    QList<QString> paths{QStringLiteral("/fake/video0"), QStringLiteral("/fake/video1")};
    QHash<QString, bool> characterDevice;
    QHash<QString, bool> statResult;
    QHash<QString, int> openResult;
    QHash<int, V4L2DeviceInfo> deviceInfo;
    QHash<int, bool> capabilitiesResult;
    QHash<int, bool> formatResult;
    bool blockOpen = false;
    bool blockCapabilities = false;
    QSemaphore openEntered;
    QSemaphore releaseOpen;
    QSemaphore capabilitiesEntered;
    QSemaphore releaseCapabilities;
    std::atomic_int closeCalls{0};
    std::atomic_int queryCalls{0};
    std::atomic_int formatCalls{0};
    std::atomic_int openCalls{0};
    std::atomic_int statCalls{0};
    bool throwOnQuery = false;
    Qt::HANDLE workerThreadId = nullptr;

    QList<QString> listVideoDevicePaths(QString& errorCode) override {
        workerThreadId = QThread::currentThreadId();
        errorCode.clear();
        return paths;
    }
    bool statDevice(const QString& path, bool& isCharacterDevice, QString& errorCode) override {
        workerThreadId = QThread::currentThreadId();
        ++statCalls;
        errorCode.clear();
        isCharacterDevice = characterDevice.value(path, true);
        const bool success = statResult.value(path, true);
        if (!success)
            errorCode = QStringLiteral("FAKE_STAT_FAILED");
        return success;
    }
    int openDevice(const QString& path, QString& errorCode) override {
        workerThreadId = QThread::currentThreadId();
        ++openCalls;
        if (blockOpen) {
            openEntered.release();
            releaseOpen.acquire();
        }
        errorCode.clear();
        return openResult.value(path, 10 + openCalls.load());
    }
    bool queryCapabilities(int fd, V4L2DeviceInfo& info, QString& errorCode) override {
        workerThreadId = QThread::currentThreadId();
        ++queryCalls;
        if (blockCapabilities) {
            capabilitiesEntered.release();
            releaseCapabilities.acquire();
        }
        if (throwOnQuery)
            throw std::runtime_error("fake query failure");
        errorCode.clear();
        info = deviceInfo.value(fd);
        if (info.card.isEmpty()) {
            info.isV4L2 = true;
            info.card = QStringLiteral("Fake camera");
            info.busInfo = QStringLiteral("usb-fake-serial");
            info.driver = QStringLiteral("fake-v4l2");
        }
        return capabilitiesResult.value(fd, true);
    }
    bool queryFormat(int fd, V4L2DeviceInfo& info, QString& errorCode) override {
        workerThreadId = QThread::currentThreadId();
        ++formatCalls;
        errorCode.clear();
        if (!formatResult.value(fd, true))
            return false;
        const V4L2DeviceInfo format = deviceInfo.value(fd);
        if (format.width == 0 && format.height == 0) {
            info.width = 640;
            info.height = 480;
            info.pixelFormat = QStringLiteral("YUYV");
        } else {
            info.width = format.width;
            info.height = format.height;
            info.pixelFormat = format.pixelFormat;
        }
        return true;
    }
    void closeDevice(int) override {
        workerThreadId = QThread::currentThreadId();
        ++closeCalls;
    }
};

class RegisteredV4L2Plugin final {
public:
    explicit RegisteredV4L2Plugin(CameraManager& manager, std::shared_ptr<IV4L2DiscoverySystem> system)
        : m_manager(manager), m_plugin(std::make_unique<V4L2CameraPlugin>(std::move(system))) {}

    ~RegisteredV4L2Plugin() {
        if (m_registered) {
            QString error;
            // Keep the plugin alive if the singleton manager refuses to unregister it.
            if (!m_manager.tryUnregisterCameraPlugin(QStringLiteral("v4l2"), &error))
                m_plugin.release();
        }
    }

    bool registerPlugin() {
        m_manager.registerCameraPlugin(m_plugin.get());
        m_registered = m_manager.plugins().contains(m_plugin.get());
        return m_registered;
    }

    bool unregisterPlugin(QString* error) {
        if (!m_registered || !m_manager.tryUnregisterCameraPlugin(QStringLiteral("v4l2"), error))
            return false;
        m_registered = false;
        return true;
    }

    V4L2CameraPlugin* get() const {
        return m_plugin.get();
    }

    void reset() {
        m_plugin.reset();
    }

private:
    CameraManager& m_manager;
    std::unique_ptr<V4L2CameraPlugin> m_plugin;
    bool m_registered = false;
};

QJsonObject resultObject(const QSignalSpy& spy) {
    return QJsonDocument::fromJson(qvariant_cast<QByteArray>(spy.first().at(1))).object();
}

} // namespace

class TestV4L2Discovery : public QObject {
    Q_OBJECT

private slots:
    void testWorkerReturnsValueResultAndClosesEveryFd();
    void testFailedOpenAndNonV4L2AreSkippedWithoutLeaks();
    void testStatAndIoctlFailuresDoNotLeakDescriptors();
    void testCancellationAfterOpenClosesFdBeforeQuery();
    void testCancellationWaitsForCurrentSystemCallAndClosesFd();
    void testBackendExceptionBecomesStructuredError();
    void testAgentListsFakeV4L2DiscoveryAndReleasesSkippedDeviceFd();
};

namespace {

class SemaphoreReleaseGuard {
public:
    explicit SemaphoreReleaseGuard(QSemaphore& semaphore) : m_semaphore(semaphore) {}
    ~SemaphoreReleaseGuard() {
        release();
    }
    void release() {
        if (!m_released) {
            m_semaphore.release();
            m_released = true;
        }
    }

private:
    QSemaphore& m_semaphore;
    bool m_released = false;
};

} // namespace

void TestV4L2Discovery::testWorkerReturnsValueResultAndClosesEveryFd() {
    auto system = std::make_shared<FakeV4L2System>();
    V4L2DeviceInfo first;
    first.isV4L2 = true;
    first.card = QStringLiteral("Camera A");
    first.busInfo = QStringLiteral("serial-a");
    first.driver = QStringLiteral("fake");
    first.width = 800;
    first.height = 600;
    first.pixelFormat = QStringLiteral("MJPG");
    system->deviceInfo.insert(11, first);
    system->deviceInfo.insert(12, first);
    V4L2CameraPlugin plugin(system);

    QString errorCode;
    CameraDiscoveryJob* job = plugin.startDiscovery(0xF000000000000001ULL, errorCode);
    QVERIFY2(job != nullptr, qPrintable(errorCode));
    QVERIFY(errorCode.isEmpty());
    QSignalSpy completedSpy(job, &CameraDiscoveryJob::completed);
    QVERIFY(completedSpy.isValid());
    QVERIFY(completedSpy.isEmpty());
    CameraDiscoveryJob* duplicate = plugin.startDiscovery(0xF000000000000001ULL, errorCode);
    QVERIFY(duplicate == nullptr);
    QCOMPARE(errorCode, QStringLiteral("CAMERA_DISCOVERY_BUSY"));
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);

    QCOMPARE(completedSpy.first().at(0).toULongLong(), 0xF000000000000001ULL);
    const QJsonObject result = resultObject(completedSpy);
    QCOMPARE(result.value(QStringLiteral("schemaVersion")).toInt(), 1);
    QCOMPARE(result.value(QStringLiteral("requestId")).toString(), QStringLiteral("17293822569102704641"));
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    const QJsonArray cameras = result.value(QStringLiteral("cameras")).toArray();
    QCOMPARE(cameras.size(), 2);
    QCOMPARE(cameras.at(0).toObject().value(QStringLiteral("deviceId")).toString(), QStringLiteral("/fake/video0"));
    QCOMPARE(
        cameras.at(0).toObject().value(QStringLiteral("rawInfo")).toObject().value(QStringLiteral("driver")).toString(),
        QStringLiteral("fake"));
    QCOMPARE(system->closeCalls.load(), 2);
    QVERIFY(system->workerThreadId != QThread::currentThreadId());
    QVERIFY(job->isTerminal());
    delete job;
}

void TestV4L2Discovery::testFailedOpenAndNonV4L2AreSkippedWithoutLeaks() {
    auto system = std::make_shared<FakeV4L2System>();
    system->paths = {QStringLiteral("/fake/not-char"), QStringLiteral("/fake/denied"), QStringLiteral("/fake/camera")};
    system->characterDevice.insert(QStringLiteral("/fake/not-char"), false);
    system->openResult.insert(QStringLiteral("/fake/denied"), -1);
    V4L2DeviceInfo notCamera;
    notCamera.isV4L2 = false;
    notCamera.card = QStringLiteral("not a V4L2 device");
    system->deviceInfo.insert(12, notCamera);
    V4L2CameraPlugin plugin(system);

    QString errorCode;
    CameraDiscoveryJob* job = plugin.startDiscovery(22, errorCode);
    QVERIFY2(job != nullptr, qPrintable(errorCode));
    QSignalSpy completedSpy(job, &CameraDiscoveryJob::completed);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    const QJsonObject result = resultObject(completedSpy);
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    QVERIFY(result.value(QStringLiteral("cameras")).toArray().isEmpty());
    QCOMPARE(system->statCalls.load(), 3);
    QCOMPARE(system->openCalls.load(), 2);
    QCOMPARE(system->closeCalls.load(), 1);
    delete job;
}

void TestV4L2Discovery::testStatAndIoctlFailuresDoNotLeakDescriptors() {
    auto system = std::make_shared<FakeV4L2System>();
    system->paths = {QStringLiteral("/fake/stat-fail"), QStringLiteral("/fake/querycap-fail"),
                     QStringLiteral("/fake/format-fail")};
    system->statResult.insert(QStringLiteral("/fake/stat-fail"), false);
    system->capabilitiesResult.insert(11, false);
    system->formatResult.insert(12, false);
    V4L2CameraPlugin plugin(system);

    QString errorCode;
    CameraDiscoveryJob* job = plugin.startDiscovery(23, errorCode);
    QVERIFY2(job != nullptr, qPrintable(errorCode));
    QSignalSpy completedSpy(job, &CameraDiscoveryJob::completed);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    const QJsonObject result = resultObject(completedSpy);
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("success"));
    const QJsonArray cameras = result.value(QStringLiteral("cameras")).toArray();
    QCOMPARE(cameras.size(), 1);
    QCOMPARE(cameras.first().toObject().value(QStringLiteral("deviceId")).toString(),
             QStringLiteral("/fake/format-fail"));
    QCOMPARE(cameras.first().toObject().value(QStringLiteral("width")).toInt(), 0);
    QCOMPARE(system->statCalls.load(), 3);
    QCOMPARE(system->openCalls.load(), 2);
    QCOMPARE(system->queryCalls.load(), 2);
    QCOMPARE(system->formatCalls.load(), 1);
    QCOMPARE(system->closeCalls.load(), 2);
    delete job;
}

void TestV4L2Discovery::testCancellationWaitsForCurrentSystemCallAndClosesFd() {
    auto system = std::make_shared<FakeV4L2System>();
    system->paths = {QStringLiteral("/fake/slow"), QStringLiteral("/fake/never-reached")};
    system->blockCapabilities = true;
    V4L2CameraPlugin plugin(system);
    SemaphoreReleaseGuard release(system->releaseCapabilities);

    QString errorCode;
    CameraDiscoveryJob* job = plugin.startDiscovery(33, errorCode);
    QVERIFY2(job != nullptr, qPrintable(errorCode));
    QSignalSpy completedSpy(job, &CameraDiscoveryJob::completed);
    QTRY_VERIFY_WITH_TIMEOUT(system->capabilitiesEntered.available() > 0, 1000);
    QVERIFY(system->capabilitiesEntered.tryAcquire());
    plugin.requestCancel(33);
    release.release();
    QVERIFY(completedSpy.isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    const QJsonObject result = resultObject(completedSpy);
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_CANCELLED"));
    QCOMPARE(system->closeCalls.load(), 1);
    QCOMPARE(system->queryCalls.load(), 1);
    QCOMPARE(system->formatCalls.load(), 0);
    delete job;
}

void TestV4L2Discovery::testCancellationAfterOpenClosesFdBeforeQuery() {
    auto system = std::make_shared<FakeV4L2System>();
    system->paths = {QStringLiteral("/fake/slow-open")};
    system->blockOpen = true;
    V4L2CameraPlugin plugin(system);
    SemaphoreReleaseGuard release(system->releaseOpen);

    QString errorCode;
    CameraDiscoveryJob* job = plugin.startDiscovery(34, errorCode);
    QVERIFY2(job != nullptr, qPrintable(errorCode));
    QSignalSpy completedSpy(job, &CameraDiscoveryJob::completed);
    QTRY_VERIFY_WITH_TIMEOUT(system->openEntered.available() > 0, 1000);
    QVERIFY(system->openEntered.tryAcquire());
    plugin.requestCancel(34);
    release.release();
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    const QJsonObject result = resultObject(completedSpy);
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(system->queryCalls.load(), 0);
    QCOMPARE(system->formatCalls.load(), 0);
    QCOMPARE(system->closeCalls.load(), 1);
    delete job;
}

void TestV4L2Discovery::testBackendExceptionBecomesStructuredError() {
    auto system = std::make_shared<FakeV4L2System>();
    system->paths = {QStringLiteral("/fake/throw")};
    system->throwOnQuery = true;
    V4L2CameraPlugin plugin(system);

    QString errorCode;
    CameraDiscoveryJob* job = plugin.startDiscovery(44, errorCode);
    QVERIFY2(job != nullptr, qPrintable(errorCode));
    QSignalSpy completedSpy(job, &CameraDiscoveryJob::completed);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 2000);
    const QJsonObject result = resultObject(completedSpy);
    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CAMERA_DISCOVERY_BACKEND_FAILED"));
    QCOMPARE(system->closeCalls.load(), 1);
    delete job;
}

void TestV4L2Discovery::testAgentListsFakeV4L2DiscoveryAndReleasesSkippedDeviceFd() {
    auto system = std::make_shared<FakeV4L2System>();
    system->openResult.insert(QStringLiteral("/fake/video1"), -1);
    CameraManager& manager = CameraManager::instance();
    RegisteredV4L2Plugin registration(manager, system);
    QVERIFY(!manager.plugins().contains(registration.get()));
    QVERIFY(registration.registerPlugin());
    QVERIFY(manager.plugins().contains(registration.get()));
    QTRY_COMPARE_WITH_TIMEOUT(manager.pendingDiscoveryCount(), 0, 2000);

    ToolSchema::instance().registerDefaultTools();
    QVERIFY(UserManager::instance().login(QStringLiteral("developer"), QString()));
    AgentActor actor;
    const QJsonObject listed = actor.executeTool(QStringLiteral("list_cameras"), {});
    QCOMPARE(listed.value(QStringLiteral("discoveryPerformed")).toBool(), false);
    QCOMPARE(listed.value(QStringLiteral("source")).toString(), QStringLiteral("CameraManager.knownCameras"));
    const QJsonArray cameras = listed.value(QStringLiteral("cameras")).toArray();
    QCOMPARE(cameras.size(), 1);
    const QJsonObject camera = cameras.first().toObject();
    QCOMPARE(camera.value(QStringLiteral("deviceId")).toString(), QStringLiteral("/fake/video0"));
    QCOMPARE(camera.value(QStringLiteral("name")).toString(), QStringLiteral("Fake camera"));
    QCOMPARE(camera.value(QStringLiteral("state")).toString(), QStringLiteral("disconnected"));
    QCOMPARE(system->openCalls.load(), 2);
    QCOMPARE(system->closeCalls.load(), 1);

    QString error;
    QVERIFY2(registration.unregisterPlugin(&error), qPrintable(error));
    QVERIFY(!manager.plugins().contains(registration.get()));
    const QJsonObject afterUnregister = actor.executeTool(QStringLiteral("list_cameras"), {});
    QVERIFY(afterUnregister.value(QStringLiteral("cameras")).toArray().isEmpty());
    UserManager::instance().logout();
    registration.reset();
}

QTEST_GUILESS_MAIN(TestV4L2Discovery)
#include "test_v4l2discovery.moc"
