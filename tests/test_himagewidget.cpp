#include <QApplication>
#include <QLineF>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtTest/QtTest>
#include <cmath>
#include <ui/widgets/HImageWidget.h>

using namespace DeepLux;

class TestHImageWidget : public QObject {
    Q_OBJECT

private slots:
    void testEmptyStatePaintsReadableCenteredHint();
    void testEmptyStateUsesDarkStyledBackground();
    void testCurrentImageReturnsOriginalImage();
    void testClicksOutsideImageAreIgnoredAndRoiIsClipped();
    void testClickCoordinatesRoundTripAcrossDisplayTransforms();
    void testCancelPendingPanStopsLateMoves();
};

void TestHImageWidget::testEmptyStatePaintsReadableCenteredHint() {
    HImageWidget widget;
    widget.resize(360, 220);
    widget.setStyleSheet("HImageWidget { background-color: #ffffff; }");
    widget.show();
    QCoreApplication::processEvents();

    QImage image = widget.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY(!image.isNull());

    const QRect centerBand(40, image.height() / 2 - 24, image.width() - 80, 56);
    int darkPixels = 0;
    for (int y = centerBand.top(); y <= centerBand.bottom(); ++y) {
        for (int x = centerBand.left(); x <= centerBand.right(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() < 160 && color.green() < 160 && color.blue() < 160) {
                ++darkPixels;
            }
        }
    }

    QVERIFY2(darkPixels > 80,
             qPrintable(QString("Expected centered empty-state text, dark pixels=%1").arg(darkPixels)));
}

void TestHImageWidget::testEmptyStateUsesDarkStyledBackground() {
    HImageWidget widget;
    widget.resize(360, 220);
    QPalette palette = widget.palette();
    palette.setColor(QPalette::Window, QColor("#1a1a1a"));
    widget.setPalette(palette);
    widget.show();
    QCoreApplication::processEvents();

    QImage image = widget.grab().toImage().convertToFormat(QImage::Format_RGB32);
    QVERIFY(!image.isNull());

    const QColor background = image.pixelColor(24, 24);
    QVERIFY2(background.lightness() < 80,
             qPrintable(QString("Expected dark empty-state background, got %1").arg(background.name())));
}

void TestHImageWidget::testCurrentImageReturnsOriginalImage() {
    HImageWidget widget;
    QImage source(123, 45, QImage::Format_RGB32);
    source.fill(QColor("#22c55e"));

    widget.setImage(source);

    QImage current = widget.currentImage();
    QCOMPARE(current.size(), source.size());
    QCOMPARE(current.pixelColor(10, 10), QColor("#22c55e"));
}

void TestHImageWidget::testClicksOutsideImageAreIgnoredAndRoiIsClipped() {
    HImageWidget widget;
    widget.resize(240, 180);
    widget.setImage(QImage(80, 60, QImage::Format_RGB32));
    widget.show();
    QCoreApplication::processEvents();

    QSignalSpy clickSpy(&widget, &HImageWidget::imageClicked);
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
    QCOMPARE(clickSpy.count(), 0);
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, widget.imageToWidget(QPointF(20, 20)).toPoint());
    QCOMPARE(clickSpy.count(), 1);
    QVERIFY(clickSpy.first().first().toPointF().x() >= 0.0);
    QVERIFY(clickSpy.first().first().toPointF().y() >= 0.0);

    widget.setRoiMode(RoiType::Rectangle1);
    int roiCount = 0;
    RoiData capturedRoi;
    connect(&widget, &HImageWidget::roiCreated, &widget, [&](const RoiData& roi) {
        ++roiCount;
        capturedRoi = roi;
    });
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(1, 1));
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(20, 20));
    QCOMPARE(roiCount, 0);
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, widget.imageToWidget(QPointF(10, 10)).toPoint());
    QTest::mouseMove(&widget, QPoint(239, 179));
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier, QPoint(239, 179));
    QCOMPARE(roiCount, 1);
    QVERIFY(capturedRoi.x2 >= 0.0 && capturedRoi.x2 < widget.imageWidth());
    QVERIFY(capturedRoi.y2 >= 0.0 && capturedRoi.y2 < widget.imageHeight());
    QVERIFY(capturedRoi.x1 != capturedRoi.x2 || capturedRoi.y1 != capturedRoi.y2);

    const QPoint samePoint = widget.imageToWidget(QPointF(12, 12)).toPoint();
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, samePoint);
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier, samePoint);
    QCOMPARE(roiCount, 1);

    const QPoint lineStart = widget.imageToWidget(QPointF(12, 14)).toPoint();
    const QPoint lineEnd = widget.imageToWidget(QPointF(30, 14)).toPoint();
    QTest::mousePress(&widget, Qt::LeftButton, Qt::NoModifier, lineStart);
    QTest::mouseRelease(&widget, Qt::LeftButton, Qt::NoModifier, lineEnd);
    QCOMPARE(roiCount, 1);
}

void TestHImageWidget::testClickCoordinatesRoundTripAcrossDisplayTransforms() {
    HImageWidget widget;
    widget.resize(512, 384);
    QImage image(160, 100, QImage::Format_RGB32);
    image.fill(Qt::white);
    widget.setImage(image);
    widget.show();
    QCoreApplication::processEvents();

    QSignalSpy clickSpy(&widget, &HImageWidget::imageClicked);
    const QList<QPointF> testPoints{QPointF(1.0, 1.0), QPointF(37.0, 21.0), QPointF(97.0, 65.0), QPointF(158.0, 98.0)};
    auto verifyClicks = [&]() {
        QVERIFY(widget.zoom() >= 2.0 / 3.0);
        for (const QPointF& expected : testPoints) {
            const QPoint eventPosition = widget.imageToWidget(expected).toPoint();
            const int previousCount = clickSpy.count();
            QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, eventPosition);
            QCOMPARE(clickSpy.count(), previousCount + 1);
            const QPointF actual = clickSpy.last().first().toPointF();
            QVERIFY(std::abs(actual.x() - expected.x()) <= 0.75);
            QVERIFY(std::abs(actual.y() - expected.y()) <= 0.75);
        }
    };

    verifyClicks(); // Initial fit-to-window.

    widget.setZoom(1.4);
    verifyClicks(); // Programmatic zoom.

    const QPointF wheelAnchor(256.0, 192.0);
    const QPointF imageAtAnchor = widget.widgetToImage(wheelAnchor);
    const double zoomBeforeWheel = widget.zoom();
    QWheelEvent wheelEvent(wheelAnchor, widget.mapToGlobal(wheelAnchor.toPoint()), QPoint(), QPoint(0, 120),
                           Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &wheelEvent);
    QVERIFY(widget.zoom() > zoomBeforeWheel);
    QVERIFY(QLineF(widget.imageToWidget(imageAtAnchor), wheelAnchor).length() <= 0.01);
    verifyClicks(); // Mouse-wheel zoom anchored under the pointer.

    const QPoint panStart = widget.imageToWidget(QPointF(60.0, 50.0)).toPoint();
    QTest::mousePress(&widget, Qt::MiddleButton, Qt::NoModifier, panStart);
    const QPoint panEnd = panStart + QPoint(18, -10);
    QMouseEvent panMove(QEvent::MouseMove, QPointF(panEnd), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &panMove);
    QTest::mouseRelease(&widget, Qt::MiddleButton, Qt::NoModifier, panEnd);
    verifyClicks(); // Middle-button pan.

    widget.resize(620, 430);
    QCoreApplication::processEvents();
    verifyClicks(); // Resize refits the image without changing local coordinates.
}

void TestHImageWidget::testCancelPendingPanStopsLateMoves() {
    HImageWidget widget;
    widget.resize(320, 240);
    widget.setImage(QImage(160, 100, QImage::Format_RGB32));
    widget.show();
    QCoreApplication::processEvents();

    const QPoint start(160, 120);
    QTest::mousePress(&widget, Qt::MiddleButton, Qt::NoModifier, start);
    QCOMPARE(widget.cursor().shape(), Qt::ClosedHandCursor);

    const QPoint initialMove = start + QPoint(12, 8);
    QMouseEvent firstMove(QEvent::MouseMove, QPointF(initialMove), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &firstMove);
    widget.cancelPendingPointerInput();
    QVERIFY(widget.cursor().shape() != Qt::ClosedHandCursor);

    const QPointF imagePoint(40.0, 30.0);
    const QPointF mappingAfterCancel = widget.imageToWidget(imagePoint);
    const QPoint lateMove = initialMove + QPoint(40, 30);
    QMouseEvent lateMoveEvent(QEvent::MouseMove, QPointF(lateMove), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &lateMoveEvent);
    QCOMPARE(widget.imageToWidget(imagePoint), mappingAfterCancel);

    QTest::mouseRelease(&widget, Qt::MiddleButton, Qt::NoModifier, lateMove);
    QCOMPARE(widget.imageToWidget(imagePoint), mappingAfterCancel);
}

QTEST_MAIN(TestHImageWidget)
#include "test_himagewidget.moc"
