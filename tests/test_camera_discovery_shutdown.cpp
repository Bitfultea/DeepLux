#include <QJsonDocument>
#include <QTimer>
#include <QtTest/QtTest>
#include <core/device/CameraDiscoveryJob.h>
#include <core/device/CameraManager.h>
#include <core/interface/ICameraDiscoveryAsync.h>
#include <core/interface/ICameraPlugin.h>
#include <functional>

using namespace DeepLux;

namespace {

class ShutdownDiscoveryJob final : public CameraDiscoveryJob {
    Q_OBJECT
public:
    ShutdownDiscoveryJob(quint64 requestId, QObject* parent) : CameraDiscoveryJob(parent) {
        QTimer::singleShot(20, this, [this, requestId]() {
            const QJsonObject result{{QStringLiteral("schemaVersion"), 1},
                                     {QStringLiteral("requestId"), QString::number(requestId)},
                                     {QStringLiteral("status"), QStringLiteral("cancelled")},
                                     {QStringLiteral("errorCode"), QStringLiteral("CAMERA_DISCOVERY_CANCELLED")}};
            completeOnce(requestId, QJsonDocument(result).toJson(QJsonDocument::Compact));
        });
    }
};

class ShutdownCameraPlugin final : public ICameraPlugin, public ICameraDiscoveryAsync {
    Q_OBJECT
    Q_INTERFACES(DeepLux::ICameraPlugin DeepLux::ICameraDiscoveryAsync)
public:
    explicit ShutdownCameraPlugin(QString id = QStringLiteral("test-camera-discovery-shutdown"))
        : m_id(std::move(id)) {}

    QString pluginId() const override {
        return m_id;
    }
    QString pluginName() const override {
        return pluginId();
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
        return {};
    }
    QObject* createCamera(const CameraInfo&) override {
        return nullptr;
    }
    CameraDiscoveryJob* startDiscovery(quint64 requestId, QString& errorCode) override {
        errorCode.clear();
        if (m_onStart)
            m_onStart();
        return new ShutdownDiscoveryJob(requestId, this);
    }
    void requestCancel(quint64 requestId) override {
        Q_UNUSED(requestId)
        ++m_cancelCalls;
    }
    int cancelCalls() const {
        return m_cancelCalls;
    }
    void setOnStart(std::function<void()> callback) {
        m_onStart = std::move(callback);
    }

private:
    QString m_id;
    int m_cancelCalls = 0;
    std::function<void()> m_onStart;
};

} // namespace

class TestCameraDiscoveryShutdown : public QObject {
    Q_OBJECT

private slots:
    void testShutdownCancelsAndDrainsDiscoveryJob() {
        CameraManager& manager = CameraManager::instance();
        ShutdownCameraPlugin beforeShutdownPlugin(QStringLiteral("test-camera-discovery-shutdown-before"));
        ShutdownCameraPlugin reentrantPlugin(QStringLiteral("test-camera-discovery-shutdown-reentrant"));
        reentrantPlugin.setOnStart([&manager]() { manager.shutdownAsync(); });
        QSignalSpy discoverySpy(&manager, &CameraManager::cameraDiscoverySettled);
        QSignalSpy shutdownSpy(&manager, &CameraManager::shutdownCompleted);

        manager.registerCameraPlugin(&beforeShutdownPlugin);
        QCOMPARE(manager.pendingDiscoveryCount(), 1);
        manager.registerCameraPlugin(&reentrantPlugin);
        QCOMPARE(manager.pendingDiscoveryCount(), 2);
        QCOMPARE(beforeShutdownPlugin.cancelCalls(), 1);
        QCOMPARE(reentrantPlugin.cancelCalls(), 1);
        QVERIFY(shutdownSpy.isEmpty());

        QTRY_COMPARE_WITH_TIMEOUT(discoverySpy.count(), 2, 1000);
        QCOMPARE(discoverySpy.first().at(2).toString(), QStringLiteral("cancelled"));
        QCOMPARE(discoverySpy.last().at(2).toString(), QStringLiteral("cancelled"));
        QTRY_COMPARE_WITH_TIMEOUT(shutdownSpy.count(), 1, 1000);
        QCOMPARE(manager.pendingDiscoveryCount(), 0);
        QCOMPARE(beforeShutdownPlugin.cancelCalls(), 1);
        QCOMPARE(reentrantPlugin.cancelCalls(), 1);
        manager.shutdownAsync();
        QCOMPARE(shutdownSpy.count(), 1);

        QString error;
        QVERIFY(manager.tryUnregisterCameraPlugin(beforeShutdownPlugin.pluginId(), &error));
        QVERIFY(manager.tryUnregisterCameraPlugin(reentrantPlugin.pluginId(), &error));
    }
};

QTEST_GUILESS_MAIN(TestCameraDiscoveryShutdown)
#include "test_camera_discovery_shutdown.moc"
