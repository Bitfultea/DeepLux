#include <QLineF>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QWheelEvent>
#include <QtTest/QtTest>
#include <cmath>
#include <ui/display/3d/CameraController.h>
#include <ui/display/3d/PointCloudRendererOpenGL.h>
#include <ui/display/3d/Viewport3DContent.h>

using namespace DeepLux;

class Test3DInteraction : public QObject {
    Q_OBJECT

private slots:
    void interactionModeKeepsCoarseLodUntilReleased();
    void heightRangeUsesFullObjectSpaceData();
    void heightShaderUsesObjectSpaceZ();
    void projectedPickReturnsOriginalWorldPointAfterCameraTransforms();
    void cancelledAndReplacedPointCloudDropsLateRelease();
};

static PointCloudData makePickCloud(const QVector3D& center) {
    PointCloudData cloud;
    const QList<QVector3D> points{center,
                                  center + QVector3D(2.0f, 0.0f, 0.0f),
                                  center - QVector3D(2.0f, 0.0f, 0.0f),
                                  center + QVector3D(0.0f, 2.0f, 0.0f),
                                  center - QVector3D(0.0f, 2.0f, 0.0f),
                                  center + QVector3D(0.0f, 0.0f, 2.0f),
                                  center - QVector3D(0.0f, 0.0f, 2.0f)};
    for (const QVector3D& point : points)
        cloud.points.emplace_back(point.x(), point.y(), point.z());
    return cloud;
}

static DisplayData displayDataForCloud(PointCloudData cloud) {
    DisplayData data;
    data.variant() = cloud;
    return data;
}

static QPointF projectedWidgetPoint(const CameraController& camera, const QVector3D& point, const QSize& size) {
    const QMatrix4x4 projection = camera.projectionMatrix(static_cast<float>(size.width()) / size.height());
    const QVector3D projected =
        point.project(camera.viewMatrix(), projection, QRect(0, 0, size.width(), size.height()));
    return QPointF(projected.x(), size.height() - projected.y());
}

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

void Test3DInteraction::heightRangeUsesFullObjectSpaceData() {
    PointCloudGPUBuffer buffer;
    for (int i = 0; i < 1000; ++i) {
        buffer.positions.push_back(static_cast<float>(i));
        buffer.positions.push_back(0.0f);
        buffer.positions.push_back(static_cast<float>(i - 500));
    }

    PointCloudRendererOpenGL renderer;
    renderer.setPointCloud(buffer, true);
    QCOMPARE(renderer.heightMinimum(), -500.0f);
    QCOMPARE(renderer.heightMaximum(), 499.0f);

    renderer.updateLODForDistance(20.0f);
    QVERIFY(renderer.currentLODLevel() > 0);
    QCOMPARE(renderer.heightMinimum(), -500.0f);
    QCOMPARE(renderer.heightMaximum(), 499.0f);
}

void Test3DInteraction::heightShaderUsesObjectSpaceZ() {
    const QString shader = QString::fromUtf8(POINT_CLOUD_VERTEX_SHADER);
    QVERIFY(shader.contains("heightColor(aPosition.z)"));
    QVERIFY(!shader.contains("heightColor(worldPos.z)"));
}

void Test3DInteraction::projectedPickReturnsOriginalWorldPointAfterCameraTransforms() {
    Viewport3DContent viewport;
    viewport.resize(640, 480);
    viewport.setPickMode(true);

    const QVector3D center(5.0f, -3.0f, 2.0f);
    const QVector3D target = center + QVector3D(2.0f, 0.0f, 0.0f);
    const PointCloudData cloud = makePickCloud(center);
    viewport.displayData(displayDataForCloud(cloud));
    QList<QVector3D> cloudPoints{center,
                                 center + QVector3D(2.0f, 0.0f, 0.0f),
                                 center - QVector3D(2.0f, 0.0f, 0.0f),
                                 center + QVector3D(0.0f, 2.0f, 0.0f),
                                 center - QVector3D(0.0f, 2.0f, 0.0f),
                                 center + QVector3D(0.0f, 0.0f, 2.0f),
                                 center - QVector3D(0.0f, 0.0f, 2.0f)};

    CameraController expectedCamera;
    expectedCamera.frameData(center - QVector3D(2.0f, 2.0f, 2.0f), center + QVector3D(2.0f, 2.0f, 2.0f));
    const QSize viewportSize = viewport.size();
    QSignalSpy pickSpy(&viewport, &Viewport3DContent::point3DClicked);
    QSignalSpy worldPickSpy(&viewport, &Viewport3DContent::point3DWorldClicked);
    auto pickAt = [&](const QPointF& screenPoint) {
        const QPoint position = screenPoint.toPoint();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(position), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&viewport, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(position), Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(&viewport, &release);
    };
    const auto verifyTargetPick = [&](const QPointF& expectedPosition) {
        for (const QVector3D& other : cloudPoints) {
            if (other == target)
                continue;
            QVERIFY(QLineF(expectedPosition, projectedWidgetPoint(expectedCamera, other, viewportSize)).length() >
                    12.0);
        }
        const int previousCount = pickSpy.count();
        const int previousWorldCount = worldPickSpy.count();
        pickAt(expectedPosition);
        QCOMPARE(pickSpy.count(), previousCount + 1);
        QCOMPARE(worldPickSpy.count(), previousWorldCount + 1);
        const QVector3D picked = pickSpy.last().first().value<QVector3D>();
        QVERIFY(QLineF(QPointF(picked.x(), picked.y()), QPointF(target.x(), target.y())).length() < 1e-6);
        QVERIFY(std::abs(picked.z() - target.z()) < 1e-6f);
        const QList<QVariant> world = worldPickSpy.last();
        QCOMPARE(world.at(0).toDouble(), static_cast<double>(target.x()));
        QCOMPARE(world.at(1).toDouble(), static_cast<double>(target.y()));
        QCOMPARE(world.at(2).toDouble(), static_cast<double>(target.z()));
    };

    const QPointF initialTargetPosition = projectedWidgetPoint(expectedCamera, target, viewportSize);
    verifyTargetPick(initialTargetPosition);

    QWheelEvent wheel(QPointF(viewport.rect().center()), viewport.mapToGlobal(viewport.rect().center()), QPoint(),
                      QPoint(0, 480), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&viewport, &wheel);
    expectedCamera.zoom(0.6f);
    const QPointF zoomedTargetPosition = projectedWidgetPoint(expectedCamera, target, viewportSize);
    QVERIFY(QLineF(initialTargetPosition, zoomedTargetPosition).length() > 2.0);
    verifyTargetPick(zoomedTargetPosition);

    const QPoint dragStart(100, 100);
    const QPoint dragEnd = dragStart + QPoint(120, 70);
    QMouseEvent orbitPress(QEvent::MouseButtonPress, QPointF(dragStart), Qt::LeftButton, Qt::LeftButton,
                           Qt::NoModifier);
    QApplication::sendEvent(&viewport, &orbitPress);
    QMouseEvent orbitMove(QEvent::MouseMove, QPointF(dragEnd), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&viewport, &orbitMove);
    QTest::qWait(20);
    QMouseEvent orbitRelease(QEvent::MouseButtonRelease, QPointF(dragEnd), Qt::LeftButton, Qt::NoButton,
                             Qt::NoModifier);
    QApplication::sendEvent(&viewport, &orbitRelease);
    expectedCamera.orbit(120.0f * 0.005f, -70.0f * 0.005f);
    const QPointF orbitedTargetPosition = projectedWidgetPoint(expectedCamera, target, viewportSize);
    QVERIFY(QLineF(zoomedTargetPosition, orbitedTargetPosition).length() > 2.0);
    verifyTargetPick(orbitedTargetPosition);
}

void Test3DInteraction::cancelledAndReplacedPointCloudDropsLateRelease() {
    Viewport3DContent viewport;
    viewport.resize(640, 480);
    viewport.setPickMode(true);
    viewport.displayData(displayDataForCloud(makePickCloud(QVector3D(5.0f, -3.0f, 2.0f))));
    QSignalSpy pickSpy(&viewport, &Viewport3DContent::point3DClicked);
    const QPoint center = viewport.rect().center();

    auto sendPress = [&]() {
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(center), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&viewport, &press);
    };
    auto sendRelease = [&]() {
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(center), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&viewport, &release);
    };

    sendPress();
    viewport.setPickMode(false);
    viewport.setPickMode(true);
    sendRelease();
    QCOMPARE(pickSpy.count(), 0);
    sendPress();
    sendRelease();
    QCOMPARE(pickSpy.count(), 1);

    sendPress();
    viewport.displayData(displayDataForCloud(makePickCloud(QVector3D(-4.0f, 6.0f, 3.0f))));
    sendRelease();
    QCOMPARE(pickSpy.count(), 1);
    sendPress();
    sendRelease();
    QCOMPARE(pickSpy.count(), 2);
    const QVector3D picked = pickSpy.last().first().value<QVector3D>();
    QCOMPARE(picked, QVector3D(-4.0f, 6.0f, 3.0f));

    viewport.clearDisplay();
    sendPress();
    sendRelease();
    QCOMPARE(pickSpy.count(), 2);
}

QTEST_MAIN(Test3DInteraction)
#include "test_3d_interaction.moc"
