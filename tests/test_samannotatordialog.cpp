#include <QtTest/QtTest>
#include <QTest>
#include <QImage>
#include <QSignalSpy>
#include <QToolButton>
#include <QListWidget>
#include <QLineEdit>
#include <QShortcut>
#include <QTemporaryFile>

#include "ui/dialogs/SamAnnotatorDialog.h"
#include "ui/widgets/HImageWidget.h"
#include "ui/widgets/AnnotationOverlayWidget.h"
#include "core/model/Annotation.h"

using namespace DeepLux;

class TestSamAnnotatorDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void modeButtonsExist();
    void shortcutsBound();
    void canOpenFromFile();
    void canAcceptSnapshot();
    void modeSwitchUpdatesOverlay();
    void categoryEditWorks();
    void objectListStartsEmpty();

private:
    QImage makeTestImage();
};

void TestSamAnnotatorDialog::initTestCase() {}
void TestSamAnnotatorDialog::cleanup() {}

QImage TestSamAnnotatorDialog::makeTestImage() {
    QImage img(200, 150, QImage::Format_RGB32);
    img.fill(QColor("#22C55E"));
    return img;
}

void TestSamAnnotatorDialog::modeButtonsExist() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.positivePointButton());
    QVERIFY(dlg.negativePointButton());
    QVERIFY(dlg.boxButton());
    QVERIFY(dlg.selectButton());
    QVERIFY(dlg.positivePointButton()->isCheckable());
    QVERIFY(dlg.negativePointButton()->isCheckable());
    QVERIFY(dlg.boxButton()->isCheckable());
    QVERIFY(dlg.selectButton()->isCheckable());
}

void TestSamAnnotatorDialog::shortcutsBound() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.confirmShortcut() != nullptr);
    QVERIFY(dlg.cancelShortcut() != nullptr);
    QVERIFY(dlg.deleteShortcut() != nullptr);

    QCOMPARE(dlg.cancelShortcut()->key(), QKeySequence(Qt::Key_Escape));
    QCOMPARE(dlg.deleteShortcut()->key(), QKeySequence(Qt::Key_Delete));
}

void TestSamAnnotatorDialog::canOpenFromFile() {
    SamAnnotatorDialog dlg;
    QImage img = makeTestImage();
    QTemporaryFile tmp("sam_test_XXXXXX.png");
    QVERIFY(tmp.open());
    QVERIFY(img.save(tmp.fileName(), "PNG"));
    tmp.close();

    // 通过 setImageSnapshot 模拟从文件打开的结果
    dlg.setImageSnapshot(img, tmp.fileName());
    QVERIFY(dlg.imageWidget()->hasImage());
    QCOMPARE(dlg.imageWidget()->imageWidth(), 200);
    QCOMPARE(dlg.imageWidget()->imageHeight(), 150);
}

void TestSamAnnotatorDialog::canAcceptSnapshot() {
    SamAnnotatorDialog dlg;
    QImage img = makeTestImage();
    QSignalSpy spy(&dlg, &SamAnnotatorDialog::imageLoaded);
    dlg.setImageSnapshot(img, "/tmp/test_snap.png");
    QVERIFY(dlg.imageWidget()->hasImage());
    QCOMPARE(spy.count(), 1);
    auto args = spy.takeFirst();
    QCOMPARE(args.at(0).toString(), QStringLiteral("/tmp/test_snap.png"));

    AnnotationSession s = dlg.session();
    QCOMPARE(s.imageWidth, 200);
    QCOMPARE(s.imageHeight, 150);
    QCOMPARE(s.imagePath, QStringLiteral("/tmp/test_snap.png"));
}

void TestSamAnnotatorDialog::modeSwitchUpdatesOverlay() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), "/tmp/test.png");

    dlg.positivePointButton()->setChecked(true);
    QCOMPARE(dlg.currentToolMode(), SamAnnotatorDialog::ToolMode::PositivePoint);
    QCOMPARE(dlg.overlayWidget()->mode(), AnnotationOverlayWidget::Mode::PositivePoint);

    dlg.boxButton()->setChecked(true);
    QCOMPARE(dlg.currentToolMode(), SamAnnotatorDialog::ToolMode::Box);
    QCOMPARE(dlg.overlayWidget()->mode(), AnnotationOverlayWidget::Mode::Box);

    dlg.selectButton()->setChecked(true);
    QCOMPARE(dlg.currentToolMode(), SamAnnotatorDialog::ToolMode::Select);
    QCOMPARE(dlg.overlayWidget()->mode(), AnnotationOverlayWidget::Mode::Select);
}

void TestSamAnnotatorDialog::categoryEditWorks() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.categoryEdit() != nullptr);
    dlg.categoryEdit()->setText("defect");
    QCOMPARE(dlg.categoryEdit()->text(), QStringLiteral("defect"));
}

void TestSamAnnotatorDialog::objectListStartsEmpty() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.objectList() != nullptr);
    QCOMPARE(dlg.objectList()->count(), 0);
}

QTEST_MAIN(TestSamAnnotatorDialog)
#include "test_samannotatordialog.moc"
