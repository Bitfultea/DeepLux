#include <QtTest/QtTest>
#include <core/device/CameraManager.h>

using namespace DeepLux;

class TestCameraShutdown : public QObject {
    Q_OBJECT

private slots:
    void testEmptyShutdownClosesAdmissionOnce() {
        CameraManager& manager = CameraManager::instance();
        QSignalSpy shutdownSpy(&manager, &CameraManager::shutdownCompleted);

        manager.shutdownAsync();
        QCOMPARE(shutdownSpy.count(), 1);
        manager.shutdownAsync();
        QCOMPARE(shutdownSpy.count(), 1);

        QString error;
        QVERIFY(!manager.startDiscoveryAsync(QStringLiteral("missing"), 1, &error));
        QCOMPARE(error, QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
        QVERIFY(!manager.connectCameraAsync(QStringLiteral("missing"), 2, &error));
        QCOMPARE(error, QStringLiteral("CAMERA_MANAGER_SHUTTING_DOWN"));
        manager.refreshCameras();
        QCOMPARE(shutdownSpy.count(), 1);
    }
};

QTEST_GUILESS_MAIN(TestCameraShutdown)
#include "test_camera_shutdown.moc"
