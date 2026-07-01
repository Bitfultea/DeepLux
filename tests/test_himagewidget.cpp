#include <QtTest/QtTest>

#include <ui/widgets/HImageWidget.h>

using namespace DeepLux;

class TestHImageWidget : public QObject {
    Q_OBJECT

private slots:
    void testEmptyStatePaintsReadableCenteredHint();
    void testEmptyStateUsesDarkStyledBackground();
};

void TestHImageWidget::testEmptyStatePaintsReadableCenteredHint()
{
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

    QVERIFY2(darkPixels > 80, qPrintable(QString("Expected centered empty-state text, dark pixels=%1").arg(darkPixels)));
}

void TestHImageWidget::testEmptyStateUsesDarkStyledBackground()
{
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

QTEST_MAIN(TestHImageWidget)
#include "test_himagewidget.moc"
