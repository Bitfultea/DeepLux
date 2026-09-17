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
    // SAM 批2：橡皮擦模式笔画采集
    void eraserStrokeCollectsImagePoints();
    void eraserClickIsSinglePointStroke();
    void eraserStrokeIgnoredInOtherModes();
};

void TestAnnotationOverlayWidget::initTestCase() {
    qRegisterMetaType<Qt::MouseButton>("Qt::MouseButton");
    qRegisterMetaType<QList<QPointF>>("QList<QPointF>");
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

    // 批2 复核二轮：smoke 升级为真实断言（往返一致 + 清除生效）
    const QList<QPointF> polygon = {QPointF(0, 0), QPointF(100, 0), QPointF(100, 100), QPointF(0, 100)};
    w.setPreviewPolygon(polygon);
    QCOMPARE(w.previewPolygon(), polygon);
    QImage mask(4, 4, QImage::Format_ARGB32);
    mask.fill(QColor(6, 182, 212, 90));
    w.setPreviewMask(mask);
    QCOMPARE(w.previewMask(), mask);
    w.clearPreview();
    QVERIFY(w.previewPolygon().isEmpty());
    QVERIFY(w.previewMask().isNull());
}

void TestAnnotationOverlayWidget::promptPointsSetClear() {
    AnnotationOverlayWidget w;
    w.setCoordConverter([](const QPointF& p) { return p; });

    // 批2 复核二轮：smoke 升级为真实断言（往返一致 + 清除生效）
    const QList<QPointF> positive = {QPointF(50, 50)};
    const QList<QPointF> negative = {QPointF(100, 100)};
    w.setPromptPoints(positive, negative);
    QCOMPARE(w.positivePromptPoints(), positive);
    QCOMPARE(w.negativePromptPoints(), negative);
    w.clearPromptPoints();
    QVERIFY(w.positivePromptPoints().isEmpty());
    QVERIFY(w.negativePromptPoints().isEmpty());
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

// SAM 批2：橡皮擦按下→拖动→释放收集完整笔画（原图坐标），一次性发出
void TestAnnotationOverlayWidget::eraserStrokeCollectsImagePoints() {
    AnnotationOverlayWidget w;
    w.resize(120, 80);
    w.setMode(AnnotationOverlayWidget::Mode::Eraser);
    w.setCoordConverter([](const QPointF& p) { return QPointF(p.x() * 2.0, p.y() * 2.0); });
    w.setInverseCoordConverter([](const QPointF& p) { return QPointF(p.x() / 2.0, p.y() / 2.0); });

    QSignalSpy spy(&w, &AnnotationOverlayWidget::eraseStrokeFinished);
    QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, QPoint(20, 10));
    QTest::mouseMove(&w, QPoint(40, 20));
    QTest::mouseMove(&w, QPoint(60, 30));
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, QPoint(60, 30));

    QCOMPARE(spy.count(), 1);
    const QList<QPointF> stroke = spy.at(0).at(0).value<QList<QPointF>>();
    QVERIFY2(stroke.size() >= 2, qPrintable(QStringLiteral("stroke size=%1").arg(stroke.size())));
    QCOMPARE(stroke.first(), QPointF(10, 5)); // 逆坐标变换：widget/2 → 原图
    QCOMPARE(stroke.last(), QPointF(30, 15));
    QVERIFY(w.currentEraseStroke().isEmpty()); // 笔画结束后预览清空
}

void TestAnnotationOverlayWidget::eraserClickIsSinglePointStroke() {
    AnnotationOverlayWidget w;
    w.resize(120, 80);
    w.setMode(AnnotationOverlayWidget::Mode::Eraser);
    w.setCoordConverter([](const QPointF& p) { return p; });
    w.setInverseCoordConverter([](const QPointF& p) { return p; });

    QSignalSpy spy(&w, &AnnotationOverlayWidget::eraseStrokeFinished);
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, QPoint(50, 40));
    QCOMPARE(spy.count(), 1);
    const QList<QPointF> stroke = spy.at(0).at(0).value<QList<QPointF>>();
    QVERIFY2(stroke.size() >= 1, "single click must produce a stroke");
    QCOMPARE(stroke.first(), QPointF(50, 40));
}

// 非橡皮擦模式的按下/拖动/释放不得发出笔画事件（模式守卫）
void TestAnnotationOverlayWidget::eraserStrokeIgnoredInOtherModes() {
    AnnotationOverlayWidget w;
    w.resize(120, 80);
    w.setCoordConverter([](const QPointF& p) { return p; });
    w.setInverseCoordConverter([](const QPointF& p) { return p; });

    for (const auto mode : {AnnotationOverlayWidget::Mode::Select, AnnotationOverlayWidget::Mode::PositivePoint,
                            AnnotationOverlayWidget::Mode::NegativePoint, AnnotationOverlayWidget::Mode::Box}) {
        w.setMode(mode);
        QSignalSpy spy(&w, &AnnotationOverlayWidget::eraseStrokeFinished);
        QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QTest::mouseMove(&w, QPoint(30, 30));
        QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, QPoint(30, 30));
        QCOMPARE(spy.count(), 0);
    }
}

QTEST_MAIN(TestAnnotationOverlayWidget)
#include "test_annotationoverlaywidget.moc"
