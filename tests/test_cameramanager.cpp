#include <QtTest/QtTest>
#include <core/device/CameraManager.h>

using namespace DeepLux;

class TestCameraManager : public QObject {
    Q_OBJECT

private slots:
    void testRefreshCamerasReturnsWithoutPlugins();
};

void TestCameraManager::testRefreshCamerasReturnsWithoutPlugins() {
    CameraManager::instance().refreshCameras();
    QVERIFY(true);
}

QTEST_MAIN(TestCameraManager)
#include "test_cameramanager.moc"
