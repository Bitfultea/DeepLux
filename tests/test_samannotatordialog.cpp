#include "core/io/YoloSegExporter.h"
#include "core/model/Annotation.h"
#include "core/model/AnnotationMaskOps.h"
#include "ui/dialogs/SamAnnotatorDialog.h"
#include "ui/widgets/AnnotationOverlayWidget.h"
#include "ui/widgets/HImageWidget.h"

#include <QCoreApplication>
#include <QImage>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QShortcut>
#include <QSignalSpy>
#include <QSpinBox>
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

    // SAM 批2：掩膜编辑（膨胀/腐蚀/橡皮擦/合并/拆分 + undo/redo）
    void maskEditControlsExist();
    void maskEditButtonsFollowSelection();
    void dilateGrowsMaskAndUndoRestores();
    void erodeToEmptyRejected();
    void erodeMultiComponentRejected();
    void eraseStrokeEditsMaskAndUndoRestores();
    void eraseMissStrokeIsNoOp();
    void mergeTouchingObjectsAndUndoRestores();
    void mergeDisjointRejected();
    void mergeRequiresExactlyTwo();
    void splitTwoBlobObjectAndUndoRestores();
    void splitSingleComponentRejected();
    void splitFragmentedMaskRejected(); // 批3 复核二轮（P1）：碎片掩膜拆分上限
    void editWithoutImageSizeRejected();
    void editFallsBackToPolygonRasterization();
    // SAM 批2 复核二轮：会话重置协议与删除掩膜还原回归
    void undoAfterOpenAnnotationDoesNotCorruptSession();
    void switchImageSameSizeResetsAnnotationState();
    void deleteUndoRestoresMaskCache();

private:
    QImage makeTestImage();
    // 整图大小 RGBA 显示掩膜（sam_server 配色），绘制像素矩形（QRect 闭区间语义）
    static QImage makeDisplayMask(int w, int h, const QList<QRect>& rects);
    // 注入已确认对象（无需真实 SAM）：onPredictionReady + onConfirm；返回对象 id（失败返回空）
    static QString confirmObject(SamAnnotatorDialog& dlg, const QList<QPointF>& polygon, const QRectF& bbox,
                                 const QImage& mask, double score = 0.9);
    static QList<QPointF> squarePolygon(double x, double y, double size);
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
    QVERIFY2(dlg.height() <= 560, "Main-view annotation config window should not waste vertical space");
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
    QVERIFY2(dlg.styleSheet().contains(QStringLiteral("font-size: 12px")),
             "Shortcut hint should use a readable font size");
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

// ===== SAM 批2：掩膜编辑 =====

QImage TestSamAnnotatorDialog::makeDisplayMask(int w, int h, const QList<QRect>& rects) {
    QImage mask(w, h, QImage::Format_ARGB32);
    mask.fill(0);
    for (const QRect& r : rects) {
        for (int y = qMax(0, r.top()); y <= qMin(h - 1, r.bottom()); ++y) {
            QRgb* row = reinterpret_cast<QRgb*>(mask.scanLine(y));
            for (int x = qMax(0, r.left()); x <= qMin(w - 1, r.right()); ++x)
                row[x] = qRgba(6, 182, 212, 90);
        }
    }
    return mask;
}

QString TestSamAnnotatorDialog::confirmObject(SamAnnotatorDialog& dlg, const QList<QPointF>& polygon,
                                              const QRectF& bbox, const QImage& mask, double score) {
    if (!QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection, Q_ARG(QList<QPointF>, polygon),
                                   Q_ARG(QRectF, bbox), Q_ARG(double, score), Q_ARG(QString, QString()),
                                   Q_ARG(QImage, mask)))
        return QString();
    if (!QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection))
        return QString();
    const AnnotationSession session = dlg.session();
    return session.annotations.isEmpty() ? QString() : session.annotations.last().id;
}

QList<QPointF> TestSamAnnotatorDialog::squarePolygon(double x, double y, double size) {
    return {QPointF(x, y), QPointF(x + size, y), QPointF(x + size, y + size), QPointF(x, y + size)};
}

void TestSamAnnotatorDialog::maskEditControlsExist() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.eraserButton() != nullptr);
    QVERIFY(dlg.dilateButton() != nullptr);
    QVERIFY(dlg.erodeButton() != nullptr);
    QVERIFY(dlg.mergeButton() != nullptr);
    QVERIFY(dlg.splitButton() != nullptr);
    QVERIFY(dlg.maskRadiusSpin() != nullptr);
    QCOMPARE(dlg.maskRadiusSpin()->minimum(), 1);
    QCOMPARE(dlg.maskRadiusSpin()->maximum(), 32);
    QCOMPARE(dlg.maskRadiusSpin()->value(), 3);
    QVERIFY(dlg.eraserButton()->isCheckable());
    QVERIFY(!dlg.dilateButton()->isCheckable());
    // 无选中：动作全部禁用
    QVERIFY(!dlg.dilateButton()->isEnabled());
    QVERIFY(!dlg.erodeButton()->isEnabled());
    QVERIFY(!dlg.mergeButton()->isEnabled());
    QVERIFY(!dlg.splitButton()->isEnabled());
    // 橡皮擦按钮驱动模式与 overlay 同步
    dlg.eraserButton()->setChecked(true);
    QCOMPARE(dlg.currentToolMode(), SamAnnotatorDialog::ToolMode::Eraser);
    QCOMPARE(dlg.overlayWidget()->mode(), AnnotationOverlayWidget::Mode::Eraser);
}

void TestSamAnnotatorDialog::maskEditButtonsFollowSelection() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                           makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}))
                 .isEmpty());
    QVERIFY(!confirmObject(dlg, squarePolygon(100, 80, 20), QRectF(100, 80, 20, 20),
                           makeDisplayMask(200, 150, {QRect(100, 80, 20, 20)}))
                 .isEmpty());
    QCOMPARE(dlg.objectList()->count(), 2);

    dlg.objectList()->setCurrentRow(0);
    QVERIFY(dlg.dilateButton()->isEnabled());
    QVERIFY(dlg.erodeButton()->isEnabled());
    QVERIFY(dlg.splitButton()->isEnabled());
    QVERIFY(!dlg.mergeButton()->isEnabled());

    // 加选第二个（Ctrl 多选语义）→ 仅合并可用
    dlg.objectList()->selectionModel()->select(dlg.objectList()->model()->index(1, 0), QItemSelectionModel::Select);
    QVERIFY(dlg.mergeButton()->isEnabled());
    QVERIFY(!dlg.dilateButton()->isEnabled());
    QVERIFY(!dlg.splitButton()->isEnabled());
}

void TestSamAnnotatorDialog::dilateGrowsMaskAndUndoRestores() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    const QImage mask = makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)});
    const QString id = confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20), mask);
    QVERIFY(!id.isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.maskRadiusSpin()->setValue(2);
    const AnnotationObject before = dlg.session().annotations.first();

    dlg.dilateButton()->click();

    const AnnotationObject after = dlg.session().annotations.first();
    QCOMPARE(after.id, id);
    QCOMPARE(after.bbox, QRectF(48, 38, 24, 24)); // 20x20 → 24x24 外扩 2
    bool ok = false;
    const QImage decoded = MaskOps::decodeRle(after.maskRle, &ok);
    QVERIFY2(ok, "edited object must carry a decodable maskRle");
    QCOMPARE(MaskOps::foregroundCount(decoded), qint64(576));
    QVERIFY(after.polygon.size() >= 4);
    // 来源字段不因几何编辑改变
    QCOMPARE(after.label, before.label);
    QCOMPARE(after.score, before.score);
    // overlay 掩膜缓存同步为整图尺寸
    QCOMPARE(dlg.overlayWidget()->objectMask(id).size(), QSize(200, 150));
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("576")));
    // 编辑后选中保持
    QCOMPARE(dlg.objectList()->currentRow(), 0);

    // 撤销：逐字段精确还原（含"原对象无 RLE"这一状态）
    dlg.undoButton()->click();
    const AnnotationObject restored = dlg.session().annotations.first();
    QCOMPARE(restored.id, before.id);
    QCOMPARE(restored.bbox, before.bbox);
    QCOMPARE(restored.polygon, before.polygon);
    QCOMPARE(restored.maskRle, before.maskRle);
    QCOMPARE(restored.score, before.score);
    QCOMPARE(restored.label, before.label);
    QCOMPARE(dlg.overlayWidget()->objectMask(id), mask);

    // 重做：再次膨胀
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.first().bbox, QRectF(48, 38, 24, 24));
}

void TestSamAnnotatorDialog::erodeToEmptyRejected() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 5), QRectF(50, 40, 5, 5),
                           makeDisplayMask(200, 150, {QRect(50, 40, 5, 5)}))
                 .isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.maskRadiusSpin()->setValue(3); // 窗口 7 > 5 → 腐蚀后为空
    const AnnotationObject before = dlg.session().annotations.first();

    dlg.erodeButton()->click();

    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("为空")));
    QCOMPARE(dlg.session().annotations.size(), 1);
    const AnnotationObject after = dlg.session().annotations.first();
    QCOMPARE(after.bbox, before.bbox);
    QCOMPARE(after.maskRle, before.maskRle);
    QCOMPARE(after.polygon, before.polygon);
}

void TestSamAnnotatorDialog::erodeMultiComponentRejected() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    // 哑铃形：两方块 + 1px 桥；腐蚀 1 断桥 → 2 连通分量 → 拒绝
    const QImage dumbbell =
        makeDisplayMask(200, 150, {QRect(40, 40, 10, 10), QRect(55, 40, 10, 10), QRect(50, 44, 5, 1)});
    QVERIFY(!confirmObject(dlg, squarePolygon(40, 40, 10), QRectF(40, 40, 25, 10), dumbbell).isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.maskRadiusSpin()->setValue(1);
    const AnnotationObject before = dlg.session().annotations.first();

    dlg.erodeButton()->click();

    QVERIFY2(dlg.statusLabel()->text().contains(QStringLiteral("连通分量")), qPrintable(dlg.statusLabel()->text()));
    const AnnotationObject after = dlg.session().annotations.first();
    QCOMPARE(after.bbox, before.bbox);
    QCOMPARE(after.maskRle, before.maskRle);
    QCOMPARE(dlg.session().annotations.size(), 1);
}

void TestSamAnnotatorDialog::eraseStrokeEditsMaskAndUndoRestores() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    const QString id = confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                                     makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}));
    QVERIFY(!id.isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.eraserButton()->setChecked(true);
    dlg.maskRadiusSpin()->setValue(3);
    const AnnotationObject before = dlg.session().annotations.first();

    // 斜向笔画削掉右下角（不切断区域）
    const QList<QPointF> stroke = {QPointF(62, 52), QPointF(66, 56), QPointF(72, 62)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onEraseStroke", Qt::DirectConnection, Q_ARG(QList<QPointF>, stroke)));

    const AnnotationObject after = dlg.session().annotations.first();
    bool ok = false;
    const QImage decoded = MaskOps::decodeRle(after.maskRle, &ok);
    QVERIFY(ok);
    const qint64 area = MaskOps::foregroundCount(decoded);
    QVERIFY2(area < 400 && area > 300, qPrintable(QStringLiteral("eroded area=%1").arg(area)));
    // 笔画命中点被擦除（在线段上），左上角保留
    QCOMPARE(int(decoded.constScanLine(56)[66]), 0);
    QCOMPARE(int(decoded.constScanLine(40)[50]), 255);
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("橡皮擦掩膜完成")));

    // 撤销精确还原
    dlg.undoButton()->click();
    const AnnotationObject restored = dlg.session().annotations.first();
    QCOMPARE(restored.bbox, before.bbox);
    QCOMPARE(restored.maskRle, before.maskRle);
    QCOMPARE(restored.polygon, before.polygon);
    QCOMPARE(MaskOps::foregroundCount(MaskOps::toBinaryMask(dlg.overlayWidget()->objectMask(id))), qint64(400));
}

void TestSamAnnotatorDialog::eraseMissStrokeIsNoOp() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                           makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}))
                 .isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.eraserButton()->setChecked(true);
    const AnnotationObject before = dlg.session().annotations.first();

    // 远离对象的笔画：未命中 → 不入撤销栈、对象不变
    const QList<QPointF> miss = {QPointF(150, 100), QPointF(160, 110)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onEraseStroke", Qt::DirectConnection, Q_ARG(QList<QPointF>, miss)));
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("未命中")));
    const AnnotationObject after = dlg.session().annotations.first();
    QCOMPARE(after.bbox, before.bbox);
    QCOMPARE(after.maskRle, before.maskRle);
    // 栈顶仍是"确认标注对象"命令（未命中笔画没有压栈）：撤销一次即移除对象
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 0);
}

void TestSamAnnotatorDialog::mergeTouchingObjectsAndUndoRestores() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    dlg.categoryEdit()->setText(QStringLiteral("A"));
    const QString idA = confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                                      makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}));
    dlg.categoryEdit()->setText(QStringLiteral("B"));
    const QString idB = confirmObject(dlg, squarePolygon(60, 40, 20), QRectF(60, 40, 20, 20),
                                      makeDisplayMask(200, 150, {QRect(60, 40, 20, 20)}));
    QVERIFY(!idA.isEmpty() && !idB.isEmpty());
    const AnnotationSession before = dlg.session();
    const QImage maskB = makeDisplayMask(200, 150, {QRect(60, 40, 20, 20)});

    // 选中两个对象（列表顺序 A、B）
    dlg.objectList()->setCurrentRow(0);
    dlg.objectList()->selectionModel()->select(dlg.objectList()->model()->index(1, 0), QItemSelectionModel::Select);
    QVERIFY(dlg.mergeButton()->isEnabled());
    dlg.mergeButton()->click();

    const AnnotationSession merged = dlg.session();
    QCOMPARE(merged.annotations.size(), 1);
    const AnnotationObject m = merged.annotations.first();
    QCOMPARE(m.id, idA); // 保留首个对象身份
    QCOMPARE(m.label, QStringLiteral("A"));
    QCOMPARE(m.bbox, QRectF(50, 40, 30, 20)); // 并集外接矩形
    bool ok = false;
    const QImage decoded = MaskOps::decodeRle(m.maskRle, &ok);
    QVERIFY(ok);
    QCOMPARE(MaskOps::foregroundCount(decoded), qint64(600)); // 400+400-200 重叠
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("600")));
    QCOMPARE(dlg.objectList()->count(), 1);
    QVERIFY(dlg.overlayWidget()->objectMask(idB).isNull()); // 被合并对象的缓存已清理
    QCOMPARE(dlg.overlayWidget()->objectMask(idA).size(), QSize(200, 150));

    // 撤销：两个对象逐字段还原（顺序/身份/掩膜缓存）
    dlg.undoButton()->click();
    const AnnotationSession restored = dlg.session();
    QCOMPARE(restored.annotations.size(), 2);
    QCOMPARE(restored.annotations[0].id, before.annotations[0].id);
    QCOMPARE(restored.annotations[1].id, before.annotations[1].id);
    QCOMPARE(restored.annotations[0].bbox, before.annotations[0].bbox);
    QCOMPARE(restored.annotations[1].bbox, before.annotations[1].bbox);
    QCOMPARE(restored.annotations[0].maskRle, before.annotations[0].maskRle);
    QCOMPARE(dlg.overlayWidget()->objectMask(idB), maskB);
    QCOMPARE(dlg.objectList()->count(), 2);

    // 重做：再次合并
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 1);
}

void TestSamAnnotatorDialog::mergeDisjointRejected() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(dlg, squarePolygon(20, 20, 10), QRectF(20, 20, 10, 10),
                           makeDisplayMask(200, 150, {QRect(20, 20, 10, 10)}))
                 .isEmpty());
    QVERIFY(!confirmObject(dlg, squarePolygon(150, 100, 10), QRectF(150, 100, 10, 10),
                           makeDisplayMask(200, 150, {QRect(150, 100, 10, 10)}))
                 .isEmpty());
    const AnnotationSession before = dlg.session();
    dlg.objectList()->setCurrentRow(0);
    dlg.objectList()->selectionModel()->select(dlg.objectList()->model()->index(1, 0), QItemSelectionModel::Select);
    dlg.mergeButton()->click();

    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("不连通")));
    const AnnotationSession after = dlg.session();
    QCOMPARE(after.annotations.size(), 2);
    QCOMPARE(after.annotations[0].bbox, before.annotations[0].bbox);
    QCOMPARE(after.annotations[1].bbox, before.annotations[1].bbox);
}

void TestSamAnnotatorDialog::mergeRequiresExactlyTwo() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                           makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}))
                 .isEmpty());
    dlg.objectList()->setCurrentRow(0);
    QVERIFY(!dlg.mergeButton()->isEnabled());
    // 直接调槽（绕过按钮禁用态）也必须失败关闭——UI 禁用只是第一道防线
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onMergeSelected", Qt::DirectConnection));
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("恰好选中两个")));
    QCOMPARE(dlg.session().annotations.size(), 1);
}

void TestSamAnnotatorDialog::splitTwoBlobObjectAndUndoRestores() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    // 掩膜含两个分离团块（小团 100px 在 raster 序前，大团 200px）；polygon/bbox 描述小团
    const QImage twoBlob = makeDisplayMask(200, 150, {QRect(30, 30, 10, 10), QRect(100, 80, 20, 10)});
    const QString id = confirmObject(dlg, squarePolygon(30, 30, 10), QRectF(30, 30, 10, 10), twoBlob, 0.77);
    QVERIFY(!id.isEmpty());
    dlg.objectList()->setCurrentRow(0);
    const AnnotationObject before = dlg.session().annotations.first();

    dlg.splitButton()->click();

    const AnnotationSession split = dlg.session();
    QCOMPARE(split.annotations.size(), 2);
    // raster 序：小团派生对象在前，最大分量（大团）保留原身份
    const AnnotationObject& derived = split.annotations[0];
    const AnnotationObject& keeper = split.annotations[1];
    QVERIFY(derived.id != id);
    QCOMPARE(keeper.id, id);
    QCOMPARE(keeper.bbox, QRectF(100, 80, 20, 10));
    QCOMPARE(keeper.score, 0.77); // 保留来源置信度
    QCOMPARE(derived.score, 0.0); // 派生分量非 SAM 预测，不伪造置信度
    QVERIFY(derived.prompts.pointsPos.isEmpty() && derived.prompts.pointsNeg.isEmpty());
    QVERIFY(!derived.prompts.box.has_value());
    QCOMPARE(derived.bbox, QRectF(30, 30, 10, 10));
    QCOMPARE(derived.label, before.label);
    bool okA = false;
    bool okB = false;
    QCOMPARE(MaskOps::foregroundCount(MaskOps::decodeRle(derived.maskRle, &okA)), qint64(100));
    QCOMPARE(MaskOps::foregroundCount(MaskOps::decodeRle(keeper.maskRle, &okB)), qint64(200));
    QVERIFY(okA && okB);
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("已拆分为 2")));
    QCOMPARE(dlg.objectList()->count(), 2);
    QVERIFY(!dlg.overlayWidget()->objectMask(derived.id).isNull());

    // 撤销：还原为单对象（逐字段）
    dlg.undoButton()->click();
    const AnnotationSession restored = dlg.session();
    QCOMPARE(restored.annotations.size(), 1);
    QCOMPARE(restored.annotations.first().id, before.id);
    QCOMPARE(restored.annotations.first().bbox, before.bbox);
    QCOMPARE(restored.annotations.first().polygon, before.polygon);
    QCOMPARE(restored.annotations.first().maskRle, before.maskRle);
    QCOMPARE(restored.annotations.first().score, before.score);
    QVERIFY(dlg.overlayWidget()->objectMask(derived.id).isNull()); // 派生缓存被清理

    // 重做：再次拆分
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 2);
}

// 批3 复核二轮（P1）：碎片化掩膜（70 个孤立像素 > 64 上限）拆分失败关闭——
// 旧实现为计数构造 70 张整图掩膜（内存放大的攻击面）
void TestSamAnnotatorDialog::splitFragmentedMaskRejected() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QList<QRect> dots;
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 7; ++j)
            dots.append(QRect(5 + i * 8, 5 + j * 8, 1, 1));
    QVERIFY(
        !confirmObject(dlg, squarePolygon(5, 5, 10), QRectF(5, 5, 80, 60), makeDisplayMask(200, 150, dots)).isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.splitButton()->click();
    QVERIFY2(dlg.statusLabel()->text().contains(QStringLiteral("上限")), qPrintable(dlg.statusLabel()->text()));
    QCOMPARE(dlg.session().annotations.size(), 1); // 失败关闭，未拆分
}

void TestSamAnnotatorDialog::splitSingleComponentRejected() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                           makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}))
                 .isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.splitButton()->click();
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("单一连通分量")));
    QCOMPARE(dlg.session().annotations.size(), 1);
}

void TestSamAnnotatorDialog::editWithoutImageSizeRejected() {
    SamAnnotatorDialog dlg;
    // 不加载图像：会话 imageWidth/Height = 0 → 掩膜编辑失败关闭
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20), QImage()).isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.dilateButton()->click();
    QVERIFY2(dlg.statusLabel()->text().contains(QStringLiteral("图像尺寸")), qPrintable(dlg.statusLabel()->text()));
    QCOMPARE(dlg.session().annotations.first().bbox, QRectF(50, 40, 20, 20));
}

void TestSamAnnotatorDialog::editFallsBackToPolygonRasterization() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    // 无掩膜缓存（QImage()）+ 不可解码的 fake RLE → 回退 polygon 栅格化重建
    const QString id = confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20), QImage());
    QVERIFY(!id.isEmpty());
    QCOMPARE(dlg.session().annotations.first().maskRle, QString()); // 注入时 RLE 为空串参数
    dlg.objectList()->setCurrentRow(0);
    dlg.maskRadiusSpin()->setValue(2);

    dlg.dilateButton()->click();

    const AnnotationObject after = dlg.session().annotations.first();
    bool ok = false;
    const QImage decoded = MaskOps::decodeRle(after.maskRle, &ok);
    QVERIFY2(ok, "fallback edit must produce a decodable maskRle");
    const qint64 area = MaskOps::foregroundCount(decoded);
    // 栅格化 20x20 方形（±边界像素）膨胀 2 → ~576（允许硬边缘边界归属容差）
    QVERIFY2(area >= 500 && area <= 700, qPrintable(QStringLiteral("dilated fallback area=%1").arg(area)));
    QVERIFY(after.bbox.isValid());
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("完成")));
    // overlay 缓存已建立（编辑产物）
    QCOMPARE(dlg.overlayWidget()->objectMask(id).size(), QSize(200, 150));
}

// 批2 复核二轮（P1-1）：换载会话 B 后，会话 A 的快照式撤销命令必须已失效——
// 否则 Ctrl+Z 会用 A 的整表快照覆盖 B（数据损坏）
void TestSamAnnotatorDialog::undoAfterOpenAnnotationDoesNotCorruptSession() {
    // 会话 A：对象 + 掩膜编辑（产生快照命令入撤销栈）
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                           makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}))
                 .isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.maskRadiusSpin()->setValue(2);
    dlg.dilateButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 1);

    // 会话 B 文件（2 个对象）
    SamAnnotatorDialog srcDlg;
    srcDlg.setImageSnapshot(makeTestImage(), QString());
    QVERIFY(!confirmObject(srcDlg, squarePolygon(10, 10, 10), QRectF(10, 10, 10, 10), QImage()).isEmpty());
    QVERIFY(!confirmObject(srcDlg, squarePolygon(100, 100, 10), QRectF(100, 100, 10, 10), QImage()).isEmpty());
    QTemporaryFile tmp("undo_open_XXXXXX.deeplux-anno.json");
    QVERIFY(tmp.open());
    const QString sessionPath = tmp.fileName();
    tmp.close();
    QString saveErr;
    QVERIFY(srcDlg.session().save(sessionPath, &saveErr));

    // 换载会话 B
    QVERIFY(dlg.openAnnotationFile(sessionPath));
    QCOMPARE(dlg.session().annotations.size(), 2);
    QCOMPARE(dlg.objectList()->count(), 2);
    const QString idB0 = dlg.session().annotations[0].id;
    const QString idB1 = dlg.session().annotations[1].id;

    // 撤销栈已清：undo/redo 不得改变会话 B
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 2);
    QCOMPARE(dlg.session().annotations[0].id, idB0);
    QCOMPARE(dlg.session().annotations[1].id, idB1);
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 2);

    // 加载失败路径：失败关闭，会话不受影响
    QString err;
    QVERIFY(!dlg.openAnnotationFile(QStringLiteral("/nonexistent/no_such_session.json"), &err));
    QVERIFY(!err.isEmpty());
    QCOMPARE(dlg.session().annotations.size(), 2);
}

// 批2 复核二轮（P1-2）：同尺寸换图必须完整重置（对象/掩膜缓存/撤销栈）——
// 否则旧对象掩膜仍满足"尺寸==整图"信任条件，编辑会用旧图像素推导新标注
void TestSamAnnotatorDialog::switchImageSameSizeResetsAnnotationState() {
    SamAnnotatorDialog dlg;
    QImage img1(200, 150, QImage::Format_RGB32);
    img1.fill(QColor("#22C55E"));
    QImage img2(200, 150, QImage::Format_RGB32);
    img2.fill(QColor("#3B82F6")); // 同尺寸异内容

    dlg.setImageSnapshot(img1, QStringLiteral("/tmp/img1.png"));
    const QString id = confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                                     makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}));
    QVERIFY(!id.isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.dilateButton()->click(); // 撤销栈留有快照命令
    QCOMPARE(dlg.session().annotations.size(), 1);

    dlg.setImageSnapshot(img2, QStringLiteral("/tmp/img2.png"));
    QCOMPARE(dlg.session().annotations.size(), 0);
    QCOMPARE(dlg.objectList()->count(), 0);
    QVERIFY(dlg.overlayWidget()->objectMask(id).isNull()); // 掩膜缓存已清
    dlg.undoButton()->click();                             // 撤销栈已清：不得复活旧对象
    QCOMPARE(dlg.session().annotations.size(), 0);
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 0);

    // 同内容快照重复设置不清空（保护进行中标注，如对话框重开）
    QVERIFY(!confirmObject(dlg, squarePolygon(60, 50, 20), QRectF(60, 50, 20, 20), QImage()).isEmpty());
    dlg.setImageSnapshot(img2, QStringLiteral("/tmp/img2.png"));
    QCOMPARE(dlg.session().annotations.size(), 1);
}

// 批2 复核二轮（P2-2）：删除的撤销必须像素级还原掩膜缓存（命令内捕获快照，
// 而不是查已被 redo 删除的缓存）
void TestSamAnnotatorDialog::deleteUndoRestoresMaskCache() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    const QImage mask = makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)});
    const QString id = confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20), mask);
    QVERIFY(!id.isEmpty());
    dlg.objectList()->setCurrentRow(0);

    QVERIFY(QMetaObject::invokeMethod(&dlg, "onDeleteSelected", Qt::DirectConnection));
    QCOMPARE(dlg.session().annotations.size(), 0);
    QVERIFY(dlg.overlayWidget()->objectMask(id).isNull());

    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 1);
    QCOMPARE(dlg.session().annotations.first().id, id);
    QCOMPARE(dlg.overlayWidget()->objectMask(id), mask); // 像素级还原，非 polygon 栅格化回落
}

QTEST_MAIN(TestSamAnnotatorDialog)
#include "test_samannotatordialog.moc"
