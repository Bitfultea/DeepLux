#include "core/model/AnnotationCoordinateMapping.h"

#include <QList>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestAnnotationCoordinateMapping : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void pointRoundTripSmall();
    void pointRoundTripLarge();
    void rectRoundTrip();
    void polygonRoundTrip();
    void largeImageError();
    void zeroOrigin();
    void preservesAspectRatioWithPadding();
};

void TestAnnotationCoordinateMapping::initTestCase() {}

void TestAnnotationCoordinateMapping::pointRoundTripSmall() {
    // 640x480 → 256x256
    AnnotationCoordinateMapping m(QSizeF(640, 480), QSizeF(256, 256));
    QPointF original(320, 240);
    QPointF model = m.toModel(original);
    QPointF restored = m.toOriginal(model);

    QVERIFY(qAbs(restored.x() - original.x()) < 1.0);
    QVERIFY(qAbs(restored.y() - original.y()) < 1.0);
}

void TestAnnotationCoordinateMapping::pointRoundTripLarge() {
    // 18200x501 → 1024x1024
    AnnotationCoordinateMapping m(QSizeF(18200, 501), QSizeF(1024, 1024));
    QPointF original(18000, 400);
    QPointF model = m.toModel(original);
    QPointF restored = m.toOriginal(model);

    // 往返误差不超过 1px
    QVERIFY(qAbs(restored.x() - original.x()) < 1.0);
    QVERIFY(qAbs(restored.y() - original.y()) < 1.0);
}

void TestAnnotationCoordinateMapping::rectRoundTrip() {
    AnnotationCoordinateMapping m(QSizeF(18200, 501), QSizeF(1024, 1024));
    QRectF original(1000, 100, 200, 50);
    QRectF model = m.toModel(original);
    QRectF restored = m.toOriginal(model);

    QVERIFY(qAbs(restored.x() - original.x()) < 1.0);
    QVERIFY(qAbs(restored.y() - original.y()) < 1.0);
    QVERIFY(qAbs(restored.width() - original.width()) < 1.0);
    QVERIFY(qAbs(restored.height() - original.height()) < 1.0);
}

void TestAnnotationCoordinateMapping::polygonRoundTrip() {
    AnnotationCoordinateMapping m(QSizeF(18200, 501), QSizeF(1024, 1024));
    QList<QPointF> original = {QPointF(1000, 100), QPointF(1200, 100), QPointF(1200, 150), QPointF(1000, 150)};
    QList<QPointF> model = m.toModel(original);
    QList<QPointF> restored = m.toOriginal(model);

    QCOMPARE(restored.size(), original.size());
    for (int i = 0; i < original.size(); ++i) {
        QVERIFY(qAbs(restored[i].x() - original[i].x()) < 1.0);
        QVERIFY(qAbs(restored[i].y() - original[i].y()) < 1.0);
    }
}

void TestAnnotationCoordinateMapping::largeImageError() {
    // 18200x501 长图，极端坐标
    AnnotationCoordinateMapping m(QSizeF(18200, 501), QSizeF(1024, 1024));

    // 测试多个点
    for (int x = 0; x <= 18200; x += 1000) {
        for (int y = 0; y <= 501; y += 100) {
            QPointF original(x, y);
            QPointF restored = m.toOriginal(m.toModel(original));
            QVERIFY2(qAbs(restored.x() - original.x()) < 1.0,
                     QString("x=%1 restored=%2").arg(original.x()).arg(restored.x()).toUtf8());
            QVERIFY2(qAbs(restored.y() - original.y()) < 1.0,
                     QString("y=%1 restored=%2").arg(original.y()).arg(restored.y()).toUtf8());
        }
    }
}

void TestAnnotationCoordinateMapping::zeroOrigin() {
    AnnotationCoordinateMapping m(QSizeF(640, 480), QSizeF(256, 256));
    QPointF origin(0, 0);
    QPointF model = m.toModel(origin);
    QPointF restored = m.toOriginal(model);

    QCOMPARE(restored.x(), 0.0);
    QCOMPARE(restored.y(), 0.0);
    QCOMPARE(model, QPointF(0, 32));
}

void TestAnnotationCoordinateMapping::preservesAspectRatioWithPadding() {
    AnnotationCoordinateMapping m(QSizeF(18200, 501), QSizeF(1024, 1024));

    QPointF topLeft = m.toModel(QPointF(0, 0));
    QPointF bottomRight = m.toModel(QPointF(18200, 501));

    QVERIFY(qAbs(topLeft.x()) < 0.001);
    QVERIFY(topLeft.y() > 400.0);
    QVERIFY(qAbs(bottomRight.x() - 1024.0) < 0.001);
    QVERIFY(bottomRight.y() < 600.0);
    QVERIFY(bottomRight.y() > topLeft.y());
}

QTEST_MAIN(TestAnnotationCoordinateMapping)
#include "test_annotation_coordinate_mapping.moc"
