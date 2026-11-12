#include <QSignalSpy>
#include <QtTest/QtTest>
#include <core/device/CameraManager.h>

using namespace DeepLux;

class TestCameraManager : public QObject {
    Q_OBJECT

private slots:
    void testRefreshWithoutPluginsIsHardwareIndependentAndIdempotent();
};

void TestCameraManager::testRefreshWithoutPluginsIsHardwareIndependentAndIdempotent() {
    CameraManager& manager = CameraManager::instance();
    QVERIFY(manager.plugins().isEmpty());
    QVERIFY(manager.knownCameras().isEmpty());
    QSignalSpy discovered(&manager, &CameraManager::cameraDiscovered);

    manager.refreshCameras();
    QVERIFY(manager.knownCameras().isEmpty());
    QCOMPARE(manager.discoverCameras().size(), 0);
    manager.refreshCameras();

    QVERIFY(manager.knownCameras().isEmpty());
    QCOMPARE(discovered.count(), 0);
}

QTEST_MAIN(TestCameraManager)
#include "test_cameramanager.moc"
