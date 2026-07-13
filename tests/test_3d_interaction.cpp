#include <QtTest/QtTest>
#include <ui/display/3d/PointCloudRendererOpenGL.h>

using namespace DeepLux;

class Test3DInteraction : public QObject {
    Q_OBJECT

private slots:
    void interactionModeKeepsCoarseLodUntilReleased();
};

void Test3DInteraction::interactionModeKeepsCoarseLodUntilReleased() {
    PointCloudGPUBuffer buffer;
    for (int i = 0; i < 1000; ++i) {
        buffer.positions.push_back(static_cast<float>(i));
        buffer.positions.push_back(0.0f);
        buffer.positions.push_back(0.0f);
    }

    PointCloudRendererOpenGL renderer;
    renderer.setPointCloud(buffer, true);
    QCOMPARE(renderer.currentLODLevel(), 0);

    renderer.updateLODForDistance(6.0f);
    QCOMPARE(renderer.currentLODLevel(), 1);

    renderer.setInteractionActive(true);
    QVERIFY(renderer.isInteractionActive());
    QVERIFY(renderer.currentLODLevel() >= 1);
    const int interactionLevel = renderer.currentLODLevel();

    renderer.updateLODForDistance(0.2f);
    QCOMPARE(renderer.currentLODLevel(), interactionLevel);

    renderer.setInteractionActive(false);
    QVERIFY(!renderer.isInteractionActive());
    renderer.updateLODForDistance(0.2f);
    QCOMPARE(renderer.currentLODLevel(), 0);
}

QTEST_MAIN(Test3DInteraction)
#include "test_3d_interaction.moc"
