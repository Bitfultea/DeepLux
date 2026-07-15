#include "core/model/Annotation.h"
#include "ui/widgets/AnnotationOverlayWidget.h"

#include <QTest>
#include <QtTest/QtTest>

Q_DECLARE_METATYPE(Qt::MouseButton)

using namespace DeepLux;

class TestAnnotationOverlayWidget : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void setAnnotationsUpdates();
    void previewPolygonRoundTrip();
    void promptPointsSetClear();
    void selectedIdChanges();
    void modeSwitch();
    void paintWithoutConverterDoesNotCrash();
    void paintWithConverterRenders();
    void emitsImageCoordinatesWhenInverseConverterIsSet();
};

void TestAnnotationOverlayWidget::initTestCase() {
    qRegisterMetaType<Qt::MouseButton>("Qt::MouseButton");
}

void TestAnnotationOverlayWidget::cleanup() {}

void TestAnnotationOverlayWidget::setAnnotationsUpdates() {
    AnnotationOverlayWidget w;
    QList<AnnotationObject> objs;
    AnnotationObject obj;
    obj.id = "ann_001";
    obj.label = "defect";
    obj.polygon = {QPointF(10, 10), QPointF(50, 10), QPointF(50, 50), QPointF(10, 50)};
    objs.append(obj);

    w.setCoordConverter([](const QPointF& p) { return p; });
    w.setAnnotations(objs);
    // Verify the widget is still valid
    QVERIFY(w.isVisible() == false); // not shown yet
}

void TestAnnotationOverlayWidget::previewPolygonRoundTrip() {
    AnnotationOverlayWidget w;
    w.setCoordConverter([](const QPointF& p) { return p; });

    QList<QPointF> polygon = {QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100)};
    w.setPreviewPolygon(polygon);
    w.clearPreview();
    // Should not crash after clear
    QVERIFY(true);
}

void TestAnnotationOverlayWidget::promptPointsSetClear() {
    AnnotationOverlayWidget w;
    w.setCoordConverter([](const QPointF& p) { return p; });

    w.setPromptPoints({QPointF(50, 50)}, {QPointF(100, 100)});
    w.clearPromptPoints();
    QVERIFY(true);
}

void TestAnnotationOverlayWidget::selectedIdChanges() {
    AnnotationOverlayWidget w;
    w.setSelectedId("ann_001");
    QCOMPARE(w.selectedId(), QStringLiteral("ann_001"));
    w.setSelectedId("ann_002");
    QCOMPARE(w.selectedId(), QStringLiteral("ann_002"));
    w.setSelectedId(QString());
    QVERIFY(w.selectedId().isEmpty());
}

void TestAnnotationOverlayWidget::modeSwitch() {
    AnnotationOverlayWidget w;
    QCOMPARE(w.mode(), AnnotationOverlayWidget::Mode::Select);
    w.setMode(AnnotationOverlayWidget::Mode::PositivePoint);
    QCOMPARE(w.mode(), AnnotationOverlayWidget::Mode::PositivePoint);
    w.setMode(AnnotationOverlayWidget::Mode::NegativePoint);
    QCOMPARE(w.mode(), AnnotationOverlayWidget::Mode::NegativePoint);
    w.setMode(AnnotationOverlayWidget::Mode::Box);
    QCOMPARE(w.mode(), AnnotationOverlayWidget::Mode::Box);
}

void TestAnnotationOverlayWidget::paintWithoutConverterDoesNotCrash() {
    AnnotationOverlayWidget w;
    w.setAnnotations({});
    w.setPreviewPolygon({QPointF(0, 0), QPointF(10, 10)});
    w.setPromptPoints({QPointF(5, 5)}, {});

    w.resize(100, 100);
    w.show();
    QTest::qWait(50);
    w.hide();
    QVERIFY(true);
}

void TestAnnotationOverlayWidget::paintWithConverterRenders() {
    AnnotationOverlayWidget w;
    w.setCoordConverter([](const QPointF& p) { return QPointF(p.x() * 2, p.y() * 2); });

    QList<AnnotationObject> objs;
    AnnotationObject obj;
    obj.id = "ann_001";
    obj.label = "defect";
    obj.polygon = {QPointF(10, 10), QPointF(50, 10), QPointF(50, 50)};
    objs.append(obj);
    w.setAnnotations(objs);
    w.setSelectedId("ann_001");
    w.setPreviewPolygon({QPointF(20, 20), QPointF(40, 20), QPointF(40, 40)});
    w.setPromptPoints({QPointF(30, 30)}, {QPointF(5, 5)});

    w.resize(200, 200);
    w.show();
    QTest::qWait(50);

    // Verify the widget renders without crashing
    QVERIFY(w.isVisible());
    w.hide();
}

void TestAnnotationOverlayWidget::emitsImageCoordinatesWhenInverseConverterIsSet() {
    AnnotationOverlayWidget w;
    w.resize(120, 80);
    w.setMode(AnnotationOverlayWidget::Mode::PositivePoint);
    w.setCoordConverter([](const QPointF& p) { return QPointF(p.x() * 2.0, p.y() * 2.0); });
    w.setInverseCoordConverter([](const QPointF& p) { return QPointF(p.x() / 2.0, p.y() / 2.0); });

    QSignalSpy spy(&w, &AnnotationOverlayWidget::widgetClicked);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(40, 20));

    QCOMPARE(spy.count(), 1);
    const auto args = spy.takeFirst();
    QCOMPARE(args.at(0).toPointF(), QPointF(20, 10));
    QCOMPARE(args.at(1).value<Qt::MouseButton>(), Qt::LeftButton);
}

QTEST_MAIN(TestAnnotationOverlayWidget)
#include "test_annotationoverlaywidget.moc"
