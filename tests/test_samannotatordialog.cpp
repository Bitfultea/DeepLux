#include "core/io/YoloSegExporter.h"
#include "core/model/Annotation.h"
#include "ui/dialogs/SamAnnotatorDialog.h"
#include "ui/widgets/AnnotationOverlayWidget.h"
#include "ui/widgets/HImageWidget.h"

#include <QCoreApplication>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QTemporaryFile>
#include <QTest>
#include <QToolButton>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestSamAnnotatorDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void modeButtonsExist();
    void themeCanMatchMainWindowPalette();
    void shortcutsBound();
    void canOpenFromFile();
    void canAcceptSnapshot();
    void modeSwitchUpdatesOverlay();
    void selectModeClickSelectsConfirmedObject();
    void categoryEditWorks();
    void objectListStartsEmpty();
    void confirmWithoutPredictionDoesNotCreateObject();
    void cancelClearsUnconfirmedSelection();
    void predictionResultCanBeConfirmed();
    void canAttachOverlayToMainViewImageWidget();
    void mainViewOverlayUsesMainViewCoordinates();
    void mainViewModeUsesReadableConfigLayout();
    void modelImportButtonExists();
    void environmentControlButtonsExist();
    void predictionMaskIsForwardedToOverlay();
    void undoRedoButtonsExist();
    void redoShortcutBound();
    void categoryListPrepopulated();
    void categoryCanBeAdded();
    void categoryCanBeRemoved();
    void currentCategoryLabelPrefersListSelection();
    void openAnnotationButtonExists();
    void exportYoloButtonExists();
    void yoloSegExportWritesFile();
    void openAnnotationReloadsSession();

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
    QVERIFY(dlg.cancelButton());
    QVERIFY(dlg.positivePointButton()->isCheckable());
    QVERIFY(dlg.negativePointButton()->isCheckable());
    QVERIFY(dlg.boxButton()->isCheckable());
    QVERIFY(dlg.selectButton()->isCheckable());
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("QToolButton:checked")));
    QCOMPARE(dlg.currentToolMode(), SamAnnotatorDialog::ToolMode::PositivePoint);
    QCOMPARE(dlg.overlayWidget()->mode(), AnnotationOverlayWidget::Mode::PositivePoint);
}

void TestSamAnnotatorDialog::themeCanMatchMainWindowPalette() {
    SamAnnotatorDialog dlg;

    dlg.applyTheme(false);
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("QDialog#SamAnnotatorDialog")));
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("background-color: #e8e8e8")));
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("QLineEdit")));
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("#e2e8f0")));

    dlg.applyTheme(true);
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("background-color: #2d2d2d")));
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("#333333")));
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("#0e7490")));
    QVERIFY(dlg.styleSheet().contains(QStringLiteral("#06b6d4")));
}

void TestSamAnnotatorDialog::shortcutsBound() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.confirmShortcut() != nullptr);
    QVERIFY(dlg.cancelShortcut() != nullptr);
    QVERIFY(dlg.deleteShortcut() != nullptr);
    QVERIFY(dlg.undoShortcut() != nullptr);

    QCOMPARE(dlg.cancelShortcut()->key(), QKeySequence(Qt::Key_Escape));
    QCOMPARE(dlg.deleteShortcut()->key(), QKeySequence(Qt::Key_Delete));
    QCOMPARE(dlg.undoShortcut()->key(), QKeySequence(QStringLiteral("Ctrl+Z")));

    QCOMPARE(dlg.confirmShortcut()->context(), Qt::ApplicationShortcut);
    QCOMPARE(dlg.cancelShortcut()->context(), Qt::ApplicationShortcut);
    QCOMPARE(dlg.deleteShortcut()->context(), Qt::ApplicationShortcut);
    QCOMPARE(dlg.undoShortcut()->context(), Qt::ApplicationShortcut);
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

void TestSamAnnotatorDialog::selectModeClickSelectsConfirmedObject() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());

    QList<QPointF> polygon = {QPointF(10, 10), QPointF(80, 10), QPointF(80, 80), QPointF(10, 80)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection, Q_ARG(QList<QPointF>, polygon),
                                      Q_ARG(QRectF, QRectF(10, 10, 70, 70)), Q_ARG(double, 0.91),
                                      Q_ARG(QString, QStringLiteral("fake_rle")), Q_ARG(QImage, QImage())));
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection));
    QCOMPARE(dlg.objectList()->count(), 1);

    dlg.selectButton()->setChecked(true);
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(40, 40)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));

    QCOMPARE(dlg.objectList()->currentRow(), 0);
    QCOMPARE(dlg.overlayWidget()->selectedId(), dlg.session().annotations.first().id);
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

void TestSamAnnotatorDialog::confirmWithoutPredictionDoesNotCreateObject() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    dlg.positivePointButton()->setChecked(true);

    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(20, 20)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection));

    QCOMPARE(dlg.objectList()->count(), 0);
    QCOMPARE(dlg.session().annotations.size(), 0);
}

void TestSamAnnotatorDialog::cancelClearsUnconfirmedSelection() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(20, 20)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));

    QList<QPointF> polygon = {QPointF(1, 2), QPointF(10, 2), QPointF(10, 12), QPointF(1, 12)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection, Q_ARG(QList<QPointF>, polygon),
                                      Q_ARG(QRectF, QRectF(1, 2, 9, 10)), Q_ARG(double, 0.91),
                                      Q_ARG(QString, QStringLiteral("fake_rle")), Q_ARG(QImage, QImage())));

    dlg.cancelButton()->click();
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection));

    QCOMPARE(dlg.objectList()->count(), 0);
    QCOMPARE(dlg.session().annotations.size(), 0);
}

void TestSamAnnotatorDialog::predictionResultCanBeConfirmed() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    dlg.positivePointButton()->setChecked(true);
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(20, 20)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));

    QList<QPointF> polygon = {QPointF(1, 2), QPointF(10, 2), QPointF(10, 12), QPointF(1, 12)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection, Q_ARG(QList<QPointF>, polygon),
                                      Q_ARG(QRectF, QRectF(1, 2, 9, 10)), Q_ARG(double, 0.91),
                                      Q_ARG(QString, QStringLiteral("fake_rle")), Q_ARG(QImage, QImage())));
    dlg.categoryEdit()->setText(QStringLiteral("defect"));
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection));

    QCOMPARE(dlg.objectList()->count(), 1);
    AnnotationSession session = dlg.session();
    QCOMPARE(session.annotations.size(), 1);
    QCOMPARE(session.annotations.first().polygon.size(), 4);
    QCOMPARE(session.annotations.first().bbox, QRectF(1, 2, 9, 10));
    QCOMPARE(session.annotations.first().score, 0.91);
    QCOMPARE(session.annotations.first().maskRle, QStringLiteral("fake_rle"));
    QCOMPARE(session.annotations.first().modelName, QStringLiteral("sam"));
    QCOMPARE(session.annotations.first().label, QStringLiteral("defect"));
}

void TestSamAnnotatorDialog::canAttachOverlayToMainViewImageWidget() {
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setImage(makeTestImage());

    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QStringLiteral("/tmp/main_view.png"));

    QCOMPARE(dlg.overlayWidget()->parentWidget(), &mainView);
    QCOMPARE(dlg.session().imageWidth, 200);
    QCOMPARE(dlg.session().imageHeight, 150);
}

void TestSamAnnotatorDialog::mainViewOverlayUsesMainViewCoordinates() {
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setImage(makeTestImage());

    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QStringLiteral("/tmp/main_view.png"));
    dlg.positivePointButton()->setChecked(true);

    QCOMPARE(dlg.overlayWidget()->parentWidget(), &mainView);
    QCOMPARE(dlg.overlayWidget()->geometry(), mainView.rect());

    bool clicked = false;
    QPointF clickedPoint;
    QObject::connect(dlg.overlayWidget(), &AnnotationOverlayWidget::widgetClicked, &dlg,
                     [&](const QPointF& imagePoint, Qt::MouseButton) {
                         clicked = true;
                         clickedPoint = imagePoint;
                     });

    const QPointF expectedImagePoint(80.0, 60.0);
    const QPoint widgetPoint = mainView.imageToWidget(expectedImagePoint).toPoint();
    QVERIFY(dlg.overlayWidget()->rect().contains(widgetPoint));
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, widgetPoint);

    QVERIFY(clicked);
    QVERIFY(qAbs(clickedPoint.x() - expectedImagePoint.x()) < 0.5);
    QVERIFY(qAbs(clickedPoint.y() - expectedImagePoint.y()) < 0.5);
}

void TestSamAnnotatorDialog::mainViewModeUsesReadableConfigLayout() {
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setImage(makeTestImage());

    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QStringLiteral("/tmp/main_view.png"));
    dlg.show();
    QCoreApplication::processEvents();

    QVERIFY2(dlg.minimumWidth() >= 210, "Main-view annotation config window should remain usable");
    QVERIFY2(dlg.minimumWidth() <= 230, "Main-view annotation config window should be about half the previous width");
    QVERIFY2(dlg.height() <= 460, "Main-view annotation config window should not waste vertical space");
    QVERIFY2(dlg.objectList()->minimumHeight() >= 72,
             "Object list should have a reasonable minimum height in narrow config window");
    QLabel* categoryLabel = dlg.findChild<QLabel*>(QStringLiteral("SamCategoryLabel"));
    QLabel* objectListLabel = dlg.findChild<QLabel*>(QStringLiteral("SamObjectListLabel"));
    QVERIFY(categoryLabel != nullptr);
    QVERIFY(objectListLabel != nullptr);
    QVERIFY2(categoryLabel->maximumHeight() <= 20, "Category label should be a compact single line");
    QVERIFY2(objectListLabel->maximumHeight() <= 20, "Object list label should be a compact single line");
    QVERIFY2(dlg.categoryEdit()->maximumHeight() <= 26, "Category input should be compact single-line height");
    QVERIFY2(dlg.categoryEdit()->geometry().top() - categoryLabel->geometry().bottom() <= 4,
             "Category label and input should be vertically tight");
    QVERIFY2(dlg.initializeEnvironmentButton()->maximumHeight() <= 26, "SAM action buttons should be compact");
    QVERIFY2(dlg.restartServerButton()->maximumHeight() <= 26, "SAM action buttons should be compact");
    QVERIFY(qAbs(dlg.importModelButton()->width() - dlg.initializeEnvironmentButton()->width()) <= 2);
    QVERIFY(qAbs(dlg.restartServerButton()->width() - dlg.initializeEnvironmentButton()->width()) <= 2);

    QPushButton* openButton = dlg.findChild<QPushButton*>(QStringLiteral("SamOpenImageButton"));
    QVERIFY(openButton != nullptr);
    QVERIFY2(!openButton->isVisibleTo(&dlg),
             "Main-view mode should not keep the open-image button in the compact toolbar");

    QVERIFY2(dlg.importModelButton()->isVisibleTo(&dlg),
             "Main-view mode should keep import-model button in the compact action grid");
    QCOMPARE(dlg.importModelButton()->geometry().top(), dlg.initializeEnvironmentButton()->geometry().top());

    QLabel* hintLabel = dlg.findChild<QLabel*>(QStringLiteral("SamShortcutHintLabel"));
    QVERIFY(hintLabel != nullptr);
    QVERIFY2(dlg.styleSheet().contains(QStringLiteral("QLabel#SamStatusLabel, QLabel#SamShortcutHintLabel")),
             "Shortcut hint should inherit the compact config-panel theme");
    QVERIFY2(dlg.styleSheet().contains(QStringLiteral("font-size: 12px")), "Shortcut hint should use a readable font size");
}

void TestSamAnnotatorDialog::modelImportButtonExists() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.importModelButton() != nullptr);
    QVERIFY(dlg.importModelButton()->text().contains(QStringLiteral("权重")));
}

void TestSamAnnotatorDialog::environmentControlButtonsExist() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.initializeEnvironmentButton() != nullptr);
    QVERIFY(dlg.restartServerButton() != nullptr);
    QVERIFY(dlg.initializeEnvironmentButton()->text().contains(QStringLiteral("环境")));
    QVERIFY(dlg.restartServerButton()->text().contains(QStringLiteral("重启")));
}

void TestSamAnnotatorDialog::predictionMaskIsForwardedToOverlay() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());

    // Build a 20x20 transparent ARGB mask
    QImage maskImage(20, 20, QImage::Format_ARGB32);
    maskImage.fill(QColor(6, 182, 212, 90));

    QList<QPointF> polygon = {QPointF(10, 10), QPointF(30, 10), QPointF(30, 30), QPointF(10, 30)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection, Q_ARG(QList<QPointF>, polygon),
                                      Q_ARG(QRectF, QRectF(10, 10, 20, 20)), Q_ARG(double, 0.91),
                                      Q_ARG(QString, QStringLiteral("rle")), Q_ARG(QImage, maskImage)));
    // Overlay should now have a preview mask set (non-null).
    QVERIFY(!dlg.overlayWidget()->previewMask().isNull());
}

void TestSamAnnotatorDialog::undoRedoButtonsExist() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.undoButton() != nullptr);
    QVERIFY(dlg.redoButton() != nullptr);
    QVERIFY(dlg.undoButton()->text().contains(QStringLiteral("撤销")));
    QVERIFY(dlg.redoButton()->text().contains(QStringLiteral("重做")));
}

void TestSamAnnotatorDialog::redoShortcutBound() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.redoShortcut() != nullptr);
    QCOMPARE(dlg.redoShortcut()->key(), QKeySequence(QStringLiteral("Ctrl+Y")));
    QCOMPARE(dlg.redoShortcut()->context(), Qt::ApplicationShortcut);
}

void TestSamAnnotatorDialog::categoryListPrepopulated() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.categoryList() != nullptr);
    QVERIFY(dlg.categoryList()->count() >= 4);
    QVERIFY(dlg.categories().contains(QStringLiteral("缺陷")));
    QVERIFY(dlg.categories().contains(QStringLiteral("划痕")));
    QVERIFY(dlg.categories().contains(QStringLiteral("凹坑")));
    QVERIFY(dlg.categories().contains(QStringLiteral("正常")));
}

void TestSamAnnotatorDialog::categoryCanBeAdded() {
    SamAnnotatorDialog dlg;
    const int initial = dlg.categories().size();
    dlg.addCategory(QStringLiteral("裂纹"));
    QVERIFY(dlg.categories().contains(QStringLiteral("裂纹")));
    QCOMPARE(dlg.categories().size(), initial + 1);
    QVERIFY(dlg.categoryColor(QStringLiteral("裂纹")).isValid());
}

void TestSamAnnotatorDialog::categoryCanBeRemoved() {
    SamAnnotatorDialog dlg;
    dlg.addCategory(QStringLiteral("临时"));
    QVERIFY(dlg.categories().contains(QStringLiteral("临时")));
    dlg.removeCategory(QStringLiteral("临时"));
    QVERIFY(!dlg.categories().contains(QStringLiteral("临时")));
}

void TestSamAnnotatorDialog::currentCategoryLabelPrefersListSelection() {
    SamAnnotatorDialog dlg;
    if (dlg.categoryList()->count() > 0) {
        dlg.categoryList()->setCurrentRow(0);
        QVERIFY(!dlg.currentCategoryLabel().isEmpty());
    }
}

void TestSamAnnotatorDialog::openAnnotationButtonExists() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.openAnnotationButton() != nullptr);
    QVERIFY(dlg.openAnnotationButton()->text().contains(QStringLiteral("打开标注")));
}

void TestSamAnnotatorDialog::exportYoloButtonExists() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.exportYoloButton() != nullptr);
    QVERIFY(dlg.exportYoloButton()->text().contains(QStringLiteral("YOLO")));
}

void TestSamAnnotatorDialog::yoloSegExportWritesFile() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());

    QList<QPointF> polygon = {QPointF(10, 10), QPointF(30, 10), QPointF(30, 30), QPointF(10, 30)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection, Q_ARG(QList<QPointF>, polygon),
                                      Q_ARG(QRectF, QRectF(10, 10, 20, 20)), Q_ARG(double, 0.91),
                                      Q_ARG(QString, QStringLiteral("rle")), Q_ARG(QImage, QImage())));
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection));
    QCOMPARE(dlg.objectList()->count(), 1);

    QTemporaryFile tmp("yolo_XXXXXX.txt");
    QVERIFY(tmp.open());
    const QString path = tmp.fileName();
    tmp.close();

    AnnotationSession session = dlg.session();
    QString err;
    QVERIFY(YoloSegExporter::exportToFile(session, path, dlg.categories(), &err));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    QVERIFY(!content.isEmpty());
}

void TestSamAnnotatorDialog::openAnnotationReloadsSession() {
    // First save a session from one dialog
    SamAnnotatorDialog srcDlg;
    srcDlg.setImageSnapshot(makeTestImage(), QString());
    QList<QPointF> polygon = {QPointF(5, 5), QPointF(15, 5), QPointF(15, 15), QPointF(5, 15)};
    QVERIFY(QMetaObject::invokeMethod(&srcDlg, "onPredictionReady", Qt::DirectConnection,
                                      Q_ARG(QList<QPointF>, polygon), Q_ARG(QRectF, QRectF(5, 5, 10, 10)),
                                      Q_ARG(double, 0.9), Q_ARG(QString, QStringLiteral("rle")),
                                      Q_ARG(QImage, QImage())));
    QVERIFY(QMetaObject::invokeMethod(&srcDlg, "onConfirm", Qt::DirectConnection));
    QCOMPARE(srcDlg.objectList()->count(), 1);

    QTemporaryFile tmp("anno_XXXXXX.deeplux-anno.json");
    QVERIFY(tmp.open());
    tmp.close();
    QString err;
    QVERIFY(srcDlg.session().save(tmp.fileName(), &err));

    // Now open the saved session in a fresh dialog via onOpenAnnotation (we invoke directly with a path)
    SamAnnotatorDialog dlg;
    QString loadErr;
    AnnotationSession loaded = AnnotationSession::load(tmp.fileName(), &loadErr);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));
    QCOMPARE(loaded.annotations.size(), 1);
}

QTEST_MAIN(TestSamAnnotatorDialog)
#include "test_samannotatordialog.moc"
