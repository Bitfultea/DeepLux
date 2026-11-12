#include "core/agent/SamNativeSettings.h"
#include "core/agent/SamPredictor.h"
#include "core/io/LabelMeExporter.h"
#include "core/io/YoloSegExporter.h"
#include "core/manager/ConfigManager.h"
#include "core/model/Annotation.h"
#include "core/model/AnnotationMaskOps.h"
#include "helpers/SamTestServer.h"
#include "ui/ThemeManager.h"
#include "ui/dialogs/SamAnnotatorDialog.h"
#include "ui/widgets/AnnotationOverlayWidget.h"
#include "ui/widgets/HImageWidget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSemaphore>
#include <QShortcut>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>

using namespace DeepLux;
using DeepLuxTest::SamTestServer;

class TestSamAnnotatorDialog : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void environmentSetupOnlyStatusIsSanitizedAndCapturesGuiStates();
    void modeButtonsExist();
    void themeCanMatchMainWindowPalette();
    void shortcutsBound();
    void canOpenFromFile();
    void testUserOpensImageFromSamDialogFilePicker();
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
    void mainViewOverlayCoordinatesMapThroughCropProvenance();
    void guiPromptPredictionSessionRoundTripAndLabelMeExportPreserveLineage();
    void concaveAnnotationPersistsCropLocalCoordinatesAndSourceBbox();
    void mainViewModeUsesReadableConfigLayout();
    void compactCommitButtonsDoNotOverlapModeRow();
    void categoryAndBackendRowsShareCompactSizing();
    void categoryDropdownUsesOneCompactRow();
    void modelImportButtonExists();
    void environmentControlButtonsExist();
    void predictionMaskIsForwardedToOverlay();
    void undoRedoButtonsExist();
    void redoShortcutBound();
    void categoryDropdownPrepopulated();
    void categoryCanBeAdded();
    void categoryCanBeRemoved();
    void testUserAddsAndRemovesSamCategoryThroughGui();
    void currentCategoryLabelUsesComboSelection();
    void currentCategoryLabelPrefersDropdownSelection();
    void openAnnotationButtonExists();
    void exportYoloButtonExists();
    void exportCocoButtonExists(); // SAM 批4
    void yoloSegExportWritesFile();
    void openAnnotationReloadsSession();

    // SAM 批2：掩膜编辑（膨胀/腐蚀/橡皮擦/合并/拆分 + undo/redo）
    void maskEditControlsExist();
    void maskEditMenuIsContextual();
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
    void agentMaskOperationsValidateAndUndo();
    void activeAnnotationObjectsReportSourceBboxWhenVerified();
    void sharedImageProvenanceRebindUpdatesSessionAndKeepsLocalState();
    void sharedViewMousePromptsCancelWithoutLosingConfirmedAnnotation();
    void sessionSaveAndReopenPreserveVerifiedSourceCoordinates();
    void agentSplitMemoryBudgetRejectedBeforeMaterialization();
    void agentMaskToolsRejectMissingImageAndUnavailableMasks();
    void agentCategoryEditsAndObjectRelabelingPreserveSelectionAndUndo();
    // SAM 批2 复核二轮：会话重置协议与删除掩膜还原回归
    void undoAfterOpenAnnotationDoesNotCorruptSession();
    void switchImageSameSizeResetsAnnotationState();
    void deleteUndoRestoresMaskCache();
    // SAM 批3 复核三轮：图像摘要链路（保存写入/打开校验/legacy 警告+保存补记）
    // 与拆分物化内存预算门禁
    void saveWritesDigestAndOpenVerifiesIt();
    void legacySessionOpensWithWarningAndBackfillsOnSave();
    void splitMemoryBudgetRejected();
    // SAM 批3 复核四轮（P1-1）：绑定图像不可读/尺寸不符的非空会话拒绝打开
    void openRejectsSessionWithUnreadableImage();
    void openRejectsLegacySessionDimsMismatch();
    // SAM 批3 复核五轮（P1-2）：绑定规则统一——imagePath 非空必须可读
    // （空会话不豁免）；仅"无路径且无标注"允许作为未绑定草稿
    void openRejectsEmptySessionWithLostImage();
    // SAM 批3 复核六轮（P1）：无摘要非空 legacy 会话必须记录正数宽高
    void openRejectsLegacySessionWithoutValidDims();
    // SAM 批4（C）：多掩膜候选循环——◀/▶ 切换预览、确认所用为所选、
    // candidates<=1 时循环行隐藏、确认/取消后候选清空
    void candidateCyclingSelectsAlternateRegion();
    void agentPromptCancelPreservesUndoAndRejectsLatePrediction();
    void agentPromptCancelStopsInvalidEmbeddingRetry();
    void agentOpenSessionCancelsOldPredictionAndPinsNewImage();
    void agentOpenSessionReplacesCancelledEmbeddingRetry();
    void standaloneImageViewportRemainsVisible();
#ifdef DEEPLUX_HAS_ONNXRUNTIME
    void nativeModelSettingsDialogSavesSelection();
    void nativeSettingsHonorsEnvironmentOverride();
    void nativeBackendPredictsAndConfirmsSelectedCandidate();
    void nativeBackendAcceptsNegativePointOnly();
    void closingWithNativeRequestQueuedDoesNotBlockUi();
#endif

private:
    QTemporaryDir m_testConfigDir;
    QByteArray m_previousAppDataDir;
    QByteArray m_previousNativePackage;
    QByteArray m_previousNativeDevice;
    bool m_hadAppDataDir = false;
    bool m_hadNativePackage = false;
    bool m_hadNativeDevice = false;
    QImage makeTestImage();
    // 整图大小 RGBA 显示掩膜（sam_server 配色），绘制像素矩形（QRect 闭区间语义）
    static QImage makeDisplayMask(int w, int h, const QList<QRect>& rects);
    // 注入已确认对象（无需真实 SAM）：onPredictionReady + onConfirm；返回对象 id（失败返回空）
    static QString confirmObject(SamAnnotatorDialog& dlg, const QList<QPointF>& polygon, const QRectF& bbox,
                                 const QImage& mask, double score = 0.9);
    static QList<QPointF> squarePolygon(double x, double y, double size);
};

void TestSamAnnotatorDialog::initTestCase() {
    QVERIFY(m_testConfigDir.isValid());
    m_hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
    m_hadNativePackage = qEnvironmentVariableIsSet("DEEPLUX_SAM_PACKAGE");
    m_hadNativeDevice = qEnvironmentVariableIsSet("DEEPLUX_SAM_DEVICE");
    m_previousAppDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
    m_previousNativePackage = qgetenv("DEEPLUX_SAM_PACKAGE");
    m_previousNativeDevice = qgetenv("DEEPLUX_SAM_DEVICE");
    qputenv("DEEPLUX_APP_DATA_DIR", QFile::encodeName(m_testConfigDir.path()));
    qunsetenv("DEEPLUX_SAM_PACKAGE");
    qunsetenv("DEEPLUX_SAM_DEVICE");
}

void TestSamAnnotatorDialog::cleanupTestCase() {
    auto restore = [](const char* name, bool wasSet, const QByteArray& value) {
        if (wasSet)
            qputenv(name, value);
        else
            qunsetenv(name);
    };
    restore("DEEPLUX_APP_DATA_DIR", m_hadAppDataDir, m_previousAppDataDir);
    restore("DEEPLUX_SAM_PACKAGE", m_hadNativePackage, m_previousNativePackage);
    restore("DEEPLUX_SAM_DEVICE", m_hadNativeDevice, m_previousNativeDevice);
}
void TestSamAnnotatorDialog::cleanup() {}

QImage TestSamAnnotatorDialog::makeTestImage() {
    QImage img(200, 150, QImage::Format_RGB32);
    img.fill(QColor("#22C55E"));
    return img;
}

void TestSamAnnotatorDialog::modeButtonsExist() {
    SamAnnotatorDialog dlg;
    auto* backendMode = dlg.findChild<QComboBox*>(QStringLiteral("SamBackendMode"));
    QVERIFY(backendMode);
    QCOMPARE(backendMode->currentData().toString(), QStringLiteral("http"));
#ifndef DEEPLUX_HAS_ONNXRUNTIME
    QCOMPARE(backendMode->count(), 1);
#endif
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

void TestSamAnnotatorDialog::testUserOpensImageFromSamDialogFilePicker() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("manual-sam-image.png"));
    QImage image(36, 24, QImage::Format_RGB32);
    image.fill(QColor(QStringLiteral("#2a6fdb")));
    QVERIFY(image.save(imagePath));

    SamAnnotatorDialog dlg;
    dlg.show();
    auto* openButton = dlg.findChild<QPushButton*>(QStringLiteral("SamOpenImageButton"));
    QVERIFY(openButton != nullptr);
    QVERIFY(openButton->isVisibleTo(&dlg));
    QSignalSpy imageLoadedSpy(&dlg, &SamAnnotatorDialog::imageLoaded);

    bool chooserShown = false;
    QTimer::singleShot(0, &dlg, [&]() {
        auto* chooser = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        if (!chooser)
            return;
        chooserShown = true;
        chooser->selectFile(imagePath);
        QVERIFY(QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection));
    });
    QTest::mouseClick(openButton, Qt::LeftButton);

    QVERIFY(chooserShown);
    QCOMPARE(imageLoadedSpy.count(), 1);
    QCOMPARE(imageLoadedSpy.first().at(0).toString(), imagePath);
    QVERIFY(dlg.imageWidget()->hasImage());
    QCOMPARE(dlg.imageWidget()->imageWidth(), image.width());
    QCOMPARE(dlg.imageWidget()->imageHeight(), image.height());
    QCOMPARE(dlg.session().imagePath, imagePath);
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
    QVERIFY(dlg.categoryCombo() != nullptr);
    QVERIFY(dlg.categoryCombo()->isEditable());
    dlg.categoryCombo()->setEditText(QStringLiteral("defect"));
    QCOMPARE(dlg.categoryCombo()->currentText(), QStringLiteral("defect"));
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
    dlg.categoryCombo()->setEditText(QStringLiteral("defect"));
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
    mainView.setFixedSize(320, 240);
    mainView.setImage(makeTestImage());

    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QStringLiteral("/tmp/main_view.png"));
    mainView.show();
    QCoreApplication::processEvents();
    mainView.actualSize();
    mainView.setZoom(2.0);

    // 中键拖动由覆盖层转发给 HImageWidget；窗口偏移由固定尺寸和平移事件独立得出。
    QTest::mousePress(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(10, 10));
    QMouseEvent panMove(QEvent::MouseMove, QPointF(25, 5), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &panMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(25, 5));

    QCOMPARE(dlg.overlayWidget()->parentWidget(), &mainView);
    QCOMPARE(dlg.overlayWidget()->geometry(), mainView.rect());
    dlg.positivePointButton()->click();
    const QPointF expectedImagePoint(80.0, 60.0);
    const QPoint pointWidget(235, 160); // (60,45) + (15,-5) + 2*(80,60)
    QVERIFY(dlg.overlayWidget()->rect().contains(pointWidget));
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, pointWidget);
    QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), QList<QPointF>{expectedImagePoint});

    dlg.boxButton()->click();
    QSignalSpy dragSpy(dlg.overlayWidget(), &AnnotationOverlayWidget::dragEnded);
    const QPoint boxStartWidget(155, 100); // (60,45) + (15,-5) + 2*(40,30)
    const QPoint boxEndWidget(275, 220);   // (60,45) + (15,-5) + 2*(100,90)
    QTest::mousePress(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, boxStartWidget);
    QMouseEvent boxMove(QEvent::MouseMove, boxEndWidget, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &boxMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, boxEndWidget);
    QCOMPARE(dragSpy.size(), 1);
    const QList<QVariant> dragCoordinates = dragSpy.takeFirst();
    QCOMPARE(dragCoordinates.at(0).toPointF(), QPointF(40.0, 30.0));
    QCOMPARE(dragCoordinates.at(1).toPointF(), QPointF(100.0, 90.0));
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
}

void TestSamAnnotatorDialog::mainViewOverlayCoordinatesMapThroughCropProvenance() {
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setFixedSize(320, 240);

    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("slice164-parent");
    provenance.sourceSize = QSize(320, 240);
    provenance.localOriginInSource = QPointF(50.0, 40.0);
    QVERIFY(provenance.isValid());
    const QImage image = makeTestImage();
    QCOMPARE(image.size(), QSize(200, 150));
    mainView.setImage(image, provenance);

    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QStringLiteral("crop.png"));
    mainView.show();
    QCoreApplication::processEvents();
    mainView.actualSize();
    mainView.setZoom(2.0);

    // At zoom 2, the 200x150 image is centered in the 320x240 view at (60,45).
    // The real middle-button drag adds (15,-5), so this screen click maps to local (80,60).
    QTest::mousePress(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(10, 10));
    QMouseEvent panMove(QEvent::MouseMove, QPointF(25, 5), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &panMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(25, 5));

    dlg.positivePointButton()->click();
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(235, 160));
    QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), QList<QPointF>{QPointF(80.0, 60.0)});

    const auto sourcePoint = mainView.coordinateProvenance().mapLocalToSource(QPointF(80.0, 60.0));
    QVERIFY(sourcePoint.has_value());
    QCOMPARE(sourcePoint->x(), 130.0);
    QCOMPARE(sourcePoint->y(), 100.0);
    QCOMPARE(mainView.coordinateProvenance().sourceImageId, QStringLiteral("slice164-parent"));

    dlg.boxButton()->click();
    QSignalSpy dragSpy(dlg.overlayWidget(), &AnnotationOverlayWidget::dragEnded);
    QTest::mousePress(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(155, 100));
    QMouseEvent boxMove(QEvent::MouseMove, QPointF(275, 220), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &boxMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(275, 220));
    QCOMPARE(dragSpy.size(), 1);
    const QList<QVariant> dragCoordinates = dragSpy.takeFirst();
    const QPointF localStart = dragCoordinates.at(0).toPointF();
    const QPointF localEnd = dragCoordinates.at(1).toPointF();
    QCOMPARE(localStart, QPointF(40.0, 30.0));
    QCOMPARE(localEnd, QPointF(100.0, 90.0));

    const auto sourceBox =
        mainView.coordinateProvenance().mapRectToSourceBounds(QRectF(localStart, localEnd).normalized());
    QVERIFY(sourceBox.has_value());
    QCOMPARE(*sourceBox, QRectF(90.0, 70.0, 60.0, 60.0));
}

void TestSamAnnotatorDialog::guiPromptPredictionSessionRoundTripAndLabelMeExportPreserveLineage() {
    SamTestServer server;
    QVERIFY(server.listen());
    server.includePredictCandidates = false;

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("slice195-source.png"));
    const QImage image = makeTestImage();
    QCOMPARE(image.size(), QSize(200, 150));
    QVERIFY(image.save(imagePath, "PNG"));

    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setFixedSize(320, 240);
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("slice195-source-image");
    provenance.sourceSize = QSize(800, 600);
    const ImageCoordinateProvenance::Matrix3 localToSource{2.0, 0.0, 30.0, 0.0, 3.0, 40.0, 0.0, 0.0, 1.0};
    QVERIFY(provenance.composeLocalToSource(localToSource));
    QVERIFY(provenance.isValid());
    mainView.setImage(image, provenance);
    mainView.show();

    SamAnnotatorDialog dialog;
    dialog.attachToImageWidget(&mainView, imagePath);
    dialog.show();
    QCoreApplication::processEvents();
    mainView.actualSize();
    mainView.setZoom(2.0);

    SamBackendClient* client = dialog.backendClient();
    QVERIFY(client);
    client->setTimeoutMs(3000);
    client->setServerUrl(server.url());
    QCOMPARE(dialog.backendImagePathForTest(), imagePath);
    client->setImage(dialog.backendImagePathForTest());
    QVERIFY2(DeepLuxTest::waitFor([&]() {
                 return !client->currentEmbeddingId().isEmpty() && client->state() == SamBackendClient::State::Ready;
             }),
             "fake SAM image embedding must become ready");
    QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
    QCOMPARE(server.lastSetImagePath, imagePath);
    QVERIFY(server.lastSetImageReadable);

    dialog.addCategory(QStringLiteral("slice195-defect"));
    dialog.categoryCombo()->setCurrentIndex(dialog.categoryCombo()->count() - 1);
    QCOMPARE(dialog.currentCategoryLabel(), QStringLiteral("slice195-defect"));
    dialog.positivePointButton()->click();
    QCOMPARE(dialog.overlayWidget()->parentWidget(), &mainView);
    const QPoint promptScreenPoint(140, 115); // centered actual-size origin (60,45) + 2 * local (40,35)
    QCOMPARE(mainView.widgetToImage(promptScreenPoint), QPointF(40.0, 35.0));
    const auto sourcePrompt = provenance.mapLocalToSource(QPointF(40.0, 35.0));
    QVERIFY(sourcePrompt.has_value());
    QCOMPARE(*sourcePrompt, QPointF(110.0, 145.0));
    QTest::mouseClick(dialog.overlayWidget(), Qt::LeftButton, Qt::NoModifier, promptScreenPoint);
    QCOMPARE(dialog.overlayWidget()->positivePromptPoints(), QList<QPointF>{QPointF(40.0, 35.0)});
    QTRY_VERIFY_WITH_TIMEOUT(!dialog.overlayWidget()->previewPolygon().isEmpty(), 3000);
    QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
    const QJsonObject predictBody = server.lastPredictBody;
    QCOMPARE(predictBody.value(QStringLiteral("embedding_id")).toString(), client->currentEmbeddingId());
    const QJsonArray expectedPositiveRequest{QJsonValue(QJsonArray{40.0, 35.0})};
    QCOMPARE(predictBody.value(QStringLiteral("points_pos")).toArray(), expectedPositiveRequest);
    QCOMPARE(predictBody.value(QStringLiteral("points_neg")).toArray(), QJsonArray{});
    QVERIFY(!predictBody.contains(QStringLiteral("box")));

    const QList<QPointF> expectedPolygon{QPointF(10, 10), QPointF(20, 10), QPointF(20, 20), QPointF(10, 20)};
    QCOMPARE(dialog.overlayWidget()->previewPolygon(), expectedPolygon);
    const QImage previewMask = dialog.overlayWidget()->previewMask();
    QCOMPARE(previewMask.size(), QSize(2, 2));
    QCOMPARE(previewMask.format(), QImage::Format_ARGB32);
    for (int y = 0; y < previewMask.height(); ++y) {
        for (int x = 0; x < previewMask.width(); ++x)
            QCOMPARE(previewMask.pixelColor(x, y), QColor(Qt::white));
    }
    QPushButton* confirmButton = nullptr;
    for (QPushButton* button : dialog.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("确认")) {
            confirmButton = button;
            break;
        }
    }
    QVERIFY(confirmButton);
    QVERIFY(confirmButton->isEnabled());
    QTest::mouseClick(confirmButton, Qt::LeftButton);

    const AnnotationSession confirmed = dialog.session();
    QCOMPARE(confirmed.annotations.size(), 1);
    const AnnotationObject object = confirmed.annotations.first();
    QVERIFY(!object.id.isEmpty());
    QCOMPARE(object.label, QStringLiteral("slice195-defect"));
    QCOMPARE(object.polygon, expectedPolygon);
    QCOMPARE(object.bbox, QRectF(10, 10, 10, 10));
    QCOMPARE(object.prompts.pointsPos, QList<QPointF>{QPointF(40.0, 35.0)});
    QVERIFY(object.prompts.pointsNeg.isEmpty());
    QVERIFY(!object.prompts.box.has_value());
    QCOMPARE(object.score, 0.97);
    QVERIFY(object.maskRle.isEmpty());
    QCOMPARE(dialog.overlayWidget()->objectMask(object.id), previewMask);
    QCOMPARE(confirmed.sourceCoordinateProvenance.sourceImageId, QStringLiteral("slice195-source-image"));
    QCOMPARE(confirmed.sourceCoordinateProvenance.sourceSize, QSize(800, 600));
    QCOMPARE(confirmed.sourceCoordinateProvenance.localToSourceMatrix(), localToSource);

    const QString sessionPath = directory.filePath(QStringLiteral("slice195.deeplux-anno.json"));
    QString error;
    QVERIFY2(dialog.saveAnnotationFile(sessionPath, &error), qPrintable(error));
    const AnnotationSession saved = AnnotationSession::load(sessionPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(saved.imageDigest, AnnotationSession::computeImageDigest(image));
    QCOMPARE(saved.imageWidth, 200);
    QCOMPARE(saved.imageHeight, 150);
    QCOMPARE(saved.sourceCoordinateProvenance.sourceImageId, QStringLiteral("slice195-source-image"));
    QCOMPARE(saved.sourceCoordinateProvenance.sourceSize, QSize(800, 600));
    QCOMPARE(saved.sourceCoordinateProvenance.localToSourceMatrix(), localToSource);
    QCOMPARE(saved.annotations.size(), 1);
    QCOMPARE(saved.annotations.first().id, object.id);
    QCOMPARE(saved.annotations.first().label, QStringLiteral("slice195-defect"));
    QCOMPARE(saved.annotations.first().polygon, expectedPolygon);
    QCOMPARE(saved.annotations.first().bbox, QRectF(10, 10, 10, 10));
    QCOMPARE(saved.annotations.first().prompts.pointsPos, QList<QPointF>{QPointF(40.0, 35.0)});

    SamAnnotatorDialog reopened;
    QVERIFY2(reopened.openAnnotationFile(sessionPath, &error), qPrintable(error));
    const AnnotationSession reopenedSession = reopened.session();
    QCOMPARE(reopenedSession.imageDigest, saved.imageDigest);
    QCOMPARE(reopenedSession.sourceCoordinateProvenance.sourceImageId, QStringLiteral("slice195-source-image"));
    QCOMPARE(reopenedSession.sourceCoordinateProvenance.sourceSize, QSize(800, 600));
    QCOMPARE(reopenedSession.sourceCoordinateProvenance.localToSourceMatrix(), localToSource);
    QCOMPARE(reopenedSession.annotations.size(), 1);
    QCOMPARE(reopenedSession.annotations.first().id, object.id);
    QCOMPARE(reopenedSession.annotations.first().label, QStringLiteral("slice195-defect"));
    QCOMPARE(reopenedSession.annotations.first().polygon, expectedPolygon);
    QCOMPARE(reopenedSession.annotations.first().bbox, QRectF(10, 10, 10, 10));
    QCOMPARE(reopenedSession.annotations.first().prompts.pointsPos, QList<QPointF>{QPointF(40.0, 35.0)});

    const QString labelMePath = directory.filePath(QStringLiteral("slice195.labelme.json"));
    QVERIFY2(LabelMeExporter::exportToFile(reopenedSession, labelMePath, &error), qPrintable(error));
    QFile labelMeFile(labelMePath);
    QVERIFY(labelMeFile.open(QIODevice::ReadOnly));
    QJsonParseError labelMeParseError;
    const QJsonObject labelMe = QJsonDocument::fromJson(labelMeFile.readAll(), &labelMeParseError).object();
    QCOMPARE(labelMeParseError.error, QJsonParseError::NoError);
    QCOMPARE(labelMe.value(QStringLiteral("imagePath")).toString(), QStringLiteral("slice195-source.png"));
    QCOMPARE(labelMe.value(QStringLiteral("imageWidth")).toInt(), 200);
    QCOMPARE(labelMe.value(QStringLiteral("imageHeight")).toInt(), 150);
    const QJsonArray shapes = labelMe.value(QStringLiteral("shapes")).toArray();
    QCOMPARE(shapes.size(), 1);
    const QJsonObject shape = shapes.first().toObject();
    QCOMPARE(shape.value(QStringLiteral("label")).toString(), QStringLiteral("slice195-defect"));
    QCOMPARE(shape.value(QStringLiteral("shape_type")).toString(), QStringLiteral("polygon"));
    const QJsonArray exportPoints = shape.value(QStringLiteral("points")).toArray();
    QCOMPARE(exportPoints.size(), expectedPolygon.size());
    for (qsizetype index = 0; index < exportPoints.size(); ++index) {
        const QJsonArray point = exportPoints.at(index).toArray();
        QCOMPARE(point.size(), 2);
        QCOMPARE(point.at(0).toDouble(), expectedPolygon.at(index).x());
        QCOMPARE(point.at(1).toDouble(), expectedPolygon.at(index).y());
    }
    QVERIFY(!labelMe.contains(QStringLiteral("sourceCoordinateProvenance")));
}

void TestSamAnnotatorDialog::concaveAnnotationPersistsCropLocalCoordinatesAndSourceBbox() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("crop.png"));
    const QImage image = makeTestImage();
    QVERIFY(image.save(imagePath, "PNG"));

    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setFixedSize(320, 240);
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("slice187-parent-image");
    provenance.sourceSize = QSize(800, 600);
    const ImageCoordinateProvenance::Matrix3 expectedSourceMatrix{{2.0, 0.0, 10.0, 0.0, 3.0, 5.0, 0.0, 0.0, 1.0}};
    QVERIFY(provenance.composeLocalToSource(expectedSourceMatrix));
    mainView.setImage(image, provenance);
    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, imagePath);
    mainView.show();
    QCoreApplication::processEvents();
    mainView.actualSize();
    mainView.setZoom(2.0);

    QTest::mousePress(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(10, 10));
    QMouseEvent panMove(QEvent::MouseMove, QPointF(25, 5), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &panMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(25, 5));

    dlg.positivePointButton()->click();
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(235, 160));
    QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), QList<QPointF>{QPointF(80.0, 60.0)});

    dlg.boxButton()->click();
    QSignalSpy dragSpy(dlg.overlayWidget(), &AnnotationOverlayWidget::dragEnded);
    QTest::mousePress(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(155, 100));
    QMouseEvent boxMove(QEvent::MouseMove, QPointF(275, 220), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &boxMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(275, 220));
    QCOMPARE(dragSpy.size(), 1);
    const QList<QVariant> boxCoordinates = dragSpy.takeFirst();
    QCOMPARE(boxCoordinates.at(0).toPointF(), QPointF(40.0, 30.0));
    QCOMPARE(boxCoordinates.at(1).toPointF(), QPointF(100.0, 90.0));

    const QList<QPointF> polygon{QPointF(20, 20), QPointF(60, 20), QPointF(60, 30),
                                 QPointF(35, 30), QPointF(35, 55), QPointF(20, 55)};
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection, Q_ARG(QList<QPointF>, polygon),
                                      Q_ARG(QRectF, QRectF(20, 20, 40, 35)), Q_ARG(double, 0.97),
                                      Q_ARG(QString, QStringLiteral("fixture-rle")), Q_ARG(QImage, QImage())));
    QCOMPARE(dlg.overlayWidget()->previewPolygon(), polygon);
    if (dlg.categoryCombo()->count() > 0)
        dlg.categoryCombo()->setCurrentIndex(0);
    const QString label = dlg.currentCategoryLabel();
    QPushButton* confirm = nullptr;
    for (QPushButton* button : dlg.findChildren<QPushButton*>()) {
        if (button->text() == QStringLiteral("确认")) {
            confirm = button;
            break;
        }
    }
    QVERIFY(confirm);
    QVERIFY(confirm->isEnabled());
    QTest::mouseClick(confirm, Qt::LeftButton);
    QCOMPARE(dlg.session().annotations.size(), 1);
    const AnnotationObject beforeSave = dlg.session().annotations.first();
    QCOMPARE(beforeSave.label, label);
    QCOMPARE(beforeSave.bbox, QRectF(20, 20, 40, 35));
    QCOMPARE(beforeSave.polygon, polygon);
    QCOMPARE(beforeSave.prompts.pointsPos, QList<QPointF>{QPointF(80.0, 60.0)});
    QVERIFY(beforeSave.prompts.box.has_value());
    QCOMPARE(*beforeSave.prompts.box, QRectF(40, 30, 60, 60));

    const QString sessionPath = dir.filePath(QStringLiteral("concave.deeplux-anno.json"));
    QString error;
    QVERIFY2(dlg.saveAnnotationFile(sessionPath, &error), qPrintable(error));
    const AnnotationSession saved = AnnotationSession::load(sessionPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(saved.imageWidth, 200);
    QCOMPARE(saved.imageHeight, 150);
    QVERIFY(saved.sourceCoordinateProvenance.isValid());
    QCOMPARE(saved.sourceCoordinateProvenance.sourceImageId, QStringLiteral("slice187-parent-image"));
    QCOMPARE(saved.sourceCoordinateProvenance.sourceSize, QSize(800, 600));
    QCOMPARE(saved.sourceCoordinateProvenance.localToSourceMatrix(), expectedSourceMatrix);
    QCOMPARE(saved.annotations.size(), 1);
    QCOMPARE(saved.annotations.first().id, beforeSave.id);
    QCOMPARE(saved.annotations.first().label, label);
    QCOMPARE(saved.annotations.first().bbox, QRectF(20, 20, 40, 35));
    QCOMPARE(saved.annotations.first().polygon, polygon);
    QCOMPARE(saved.annotations.first().prompts.pointsPos, QList<QPointF>{QPointF(80.0, 60.0)});
    QVERIFY(saved.annotations.first().prompts.box.has_value());
    QCOMPARE(*saved.annotations.first().prompts.box, QRectF(40, 30, 60, 60));

    SamAnnotatorDialog reopened;
    QVERIFY2(reopened.openAnnotationFile(sessionPath, &error), qPrintable(error));
    const AnnotationSession reopenedSession = reopened.session();
    QVERIFY(reopenedSession.sourceCoordinateProvenance.isValid());
    QCOMPARE(reopenedSession.sourceCoordinateProvenance.sourceImageId, QStringLiteral("slice187-parent-image"));
    QCOMPARE(reopenedSession.sourceCoordinateProvenance.sourceSize, QSize(800, 600));
    QCOMPARE(reopenedSession.sourceCoordinateProvenance.localToSourceMatrix(), expectedSourceMatrix);
    QCOMPARE(reopenedSession.annotations.size(), 1);
    const AnnotationObject& reopenedObject = reopenedSession.annotations.first();
    QCOMPARE(reopenedObject.id, beforeSave.id);
    QCOMPARE(reopenedObject.label, label);
    QCOMPARE(reopenedObject.bbox, QRectF(20, 20, 40, 35));
    QCOMPARE(reopenedObject.polygon, polygon);
    QCOMPARE(reopenedObject.prompts.pointsPos, QList<QPointF>{QPointF(80.0, 60.0)});
    QVERIFY(reopenedObject.prompts.box.has_value());
    QCOMPARE(*reopenedObject.prompts.box, QRectF(40, 30, 60, 60));

    const QJsonObject objectsPage = reopened.listAgentAnnotationObjects(0, 1);
    QCOMPARE(objectsPage.value(QStringLiteral("coordinateFrame")).toString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(objectsPage.value(QStringLiteral("sourceCoordinateAvailable")).toBool());
    const QJsonArray objects = objectsPage.value(QStringLiteral("objects")).toArray();
    QCOMPARE(objects.size(), 1);
    const QJsonObject object = objects.first().toObject();
    QCOMPARE(object.value(QStringLiteral("objectId")).toString(), beforeSave.id);
    QCOMPARE(object.value(QStringLiteral("label")).toString(), label);
    QCOMPARE(object.value(QStringLiteral("bbox")).toObject().value(QStringLiteral("x")).toDouble(), 20.0);
    QCOMPARE(object.value(QStringLiteral("bbox")).toObject().value(QStringLiteral("y")).toDouble(), 20.0);
    QCOMPARE(object.value(QStringLiteral("sourceImageId")).toString(), QStringLiteral("slice187-parent-image"));
    QCOMPARE(object.value(QStringLiteral("sourceImageSize")).toObject().value(QStringLiteral("width")).toInt(), 800);
    QCOMPARE(object.value(QStringLiteral("sourceImageSize")).toObject().value(QStringLiteral("height")).toInt(), 600);
    const QJsonObject sourceBbox = object.value(QStringLiteral("sourceBbox")).toObject();
    QCOMPARE(sourceBbox.value(QStringLiteral("x")).toDouble(), 50.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("y")).toDouble(), 65.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("width")).toDouble(), 80.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("height")).toDouble(), 105.0);
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
    QVERIFY2(dlg.categoryCombo()->maximumHeight() <= 26, "Category dropdown should be compact single-line height");
    QVERIFY2(qAbs(dlg.categoryCombo()->geometry().center().y() - categoryLabel->geometry().center().y()) <= 2,
             "Category label and dropdown should share one row");
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

void TestSamAnnotatorDialog::compactCommitButtonsDoNotOverlapModeRow() {
    struct AppStyleRestore {
        QString oldStyle = qApp->styleSheet();
        ~AppStyleRestore() {
            qApp->setStyleSheet(oldStyle);
        }
    } restore;
    qApp->setStyleSheet(ThemeManager::styleSheet(false));

    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setImage(makeTestImage());
    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QString());
    dlg.resize(320, 530);
    dlg.show();
    QCoreApplication::processEvents();

    auto* confirm = dlg.findChild<QPushButton*>(QStringLiteral("SamConfirmButton"));
    auto* cancel = dlg.findChild<QPushButton*>(QStringLiteral("SamCancelButton"));
    QVERIFY(confirm);
    QVERIFY(cancel);
    auto verifyRows = [&]() {
        const int modeTop = dlg.positivePointButton()->mapTo(&dlg, QPoint(0, 0)).y();
        for (QPushButton* button : {confirm, cancel}) {
            if (button->height() != 26 || button->mapTo(&dlg, QPoint(0, button->height())).y() + 2 > modeTop)
                return false;
        }
        return true;
    };
    QVERIFY2(verifyRows(), "Commit buttons must fit above the mode-button row under the app stylesheet");
    const QString capturePath = qEnvironmentVariable("DEEPLUX_TEST_SAM_COMMIT_CAPTURE");
    if (!capturePath.isEmpty())
        QVERIFY(dlg.grab().save(capturePath));

    qApp->setStyleSheet(ThemeManager::styleSheet(true));
    dlg.applyTheme(true);
    QCoreApplication::processEvents();
    QVERIFY2(verifyRows(), "Commit buttons must also fit in the dark theme");
    if (!capturePath.isEmpty())
        QVERIFY(dlg.grab().save(capturePath + QStringLiteral(".dark.png")));
}

void TestSamAnnotatorDialog::categoryAndBackendRowsShareCompactSizing() {
    struct AppStyleRestore {
        QString oldStyle = qApp->styleSheet();
        ~AppStyleRestore() {
            qApp->setStyleSheet(oldStyle);
        }
    } restore;
    qApp->setStyleSheet(ThemeManager::styleSheet(false));
    QVERIFY2(!qApp->styleSheet().contains(QStringLiteral("QComboBox::down-arrow")) &&
                 !qApp->styleSheet().contains(QStringLiteral("QComboBox::drop-down")),
             "Leave combo arrow painting to the Qt style; stylesheet border triangles render as blocks");

    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setImage(makeTestImage());
    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QString());
    dlg.resize(320, 530);
    dlg.show();
    QCoreApplication::processEvents();

    auto* categoryLabel = dlg.findChild<QLabel*>(QStringLiteral("SamCategoryLabel"));
    auto* objectLabel = dlg.findChild<QLabel*>(QStringLiteral("SamObjectListLabel"));
    auto* backendLabel = dlg.findChild<QLabel*>(QStringLiteral("SamBackendLabel"));
    auto* backend = dlg.findChild<QComboBox*>(QStringLiteral("SamBackendMode"));
    QVERIFY(categoryLabel);
    QVERIFY(objectLabel);
    QVERIFY(backendLabel);
    QVERIFY(backend);

    auto verifyRows = [&]() {
        const auto centerY = [&dlg](QWidget* widget) {
            return widget->mapTo(&dlg, QPoint(0, widget->height() / 2)).y();
        };
        return dlg.categoryCombo()->height() == 26 && backend->height() == 26 &&
               dlg.addCategoryButton()->height() == 26 && dlg.removeCategoryButton()->height() == 26 &&
               qAbs(centerY(categoryLabel) - centerY(dlg.categoryCombo())) <= 2 &&
               qAbs(centerY(objectLabel) - centerY(backend)) <= 2 &&
               qAbs(centerY(backendLabel) - centerY(backend)) <= 2 && categoryLabel->font().pointSize() == 9 &&
               objectLabel->font().pointSize() == 9 && backendLabel->font().pointSize() == 9 &&
               dlg.categoryCombo()->font().pointSize() == 9 &&
               dlg.categoryCombo()->lineEdit()->font().pointSize() == 9 && backend->font().pointSize() == 9;
    };
    QVERIFY2(verifyRows(), "SAM config rows must share compact height, centerline and font under app theme");
    const QString capturePath = qEnvironmentVariable("DEEPLUX_TEST_SAM_HEADER_CAPTURE");
    if (!capturePath.isEmpty())
        QVERIFY(dlg.grab().save(capturePath));
    backend->showPopup();
    QCoreApplication::processEvents();
    QCOMPARE(backend->view()->font().pointSize(), 9);
    const QRect lastOption = backend->view()->visualRect(backend->model()->index(backend->count() - 1, 0));
    QVERIFY2(lastOption.bottom() < backend->view()->viewport()->height(),
             "The backend popup must show every option without clipping");
    if (!capturePath.isEmpty())
        QVERIFY(backend->view()->grab().save(capturePath + QStringLiteral(".backend-menu.png")));
    if (!capturePath.isEmpty())
        QVERIFY(backend->view()->window()->grab().save(capturePath + QStringLiteral(".backend-popup.png")));
    backend->hidePopup();
    QCoreApplication::processEvents();
    dlg.categoryCombo()->showPopup();
    QCoreApplication::processEvents();
    QCOMPARE(dlg.categoryCombo()->view()->font().pointSize(), 9);
    const QRect lastCategory = dlg.categoryCombo()->view()->visualRect(
        dlg.categoryCombo()->model()->index(dlg.categoryCombo()->count() - 1, 0));
    QVERIFY2(lastCategory.bottom() < dlg.categoryCombo()->view()->viewport()->height(),
             "The category popup must show every preset without clipping");
    if (!capturePath.isEmpty())
        QVERIFY(
            dlg.categoryCombo()->view()->window()->grab().save(capturePath + QStringLiteral(".category-popup.png")));
    dlg.categoryCombo()->hidePopup();
    QCoreApplication::processEvents();

    dlg.resize(220, 530);
    QCoreApplication::processEvents();
    auto* scroll = dlg.findChild<QScrollArea*>(QStringLiteral("SamControlScroll"));
    auto* content = dlg.findChild<QWidget*>(QStringLiteral("SamControlScrollContent"));
    QVERIFY2(content->width() <= scroll->viewport()->width(),
             "Compact SAM controls must fit the viewport without horizontal clipping");
    QVERIFY(!backendLabel->isVisibleTo(&dlg));
    QVERIFY(dlg.categoryCombo()->height() == 26 && backend->height() == 26);
    const auto leftX = [&dlg](QWidget* widget) { return widget->mapTo(&dlg, QPoint(0, 0)).x(); };
    const auto rightX = [&dlg](QWidget* widget) { return widget->mapTo(&dlg, QPoint(widget->width(), 0)).x(); };
    QVERIFY(rightX(categoryLabel) + 2 <= leftX(dlg.categoryCombo()));
    QVERIFY(rightX(dlg.categoryCombo()) + 2 <= leftX(dlg.addCategoryButton()));
    QVERIFY(rightX(objectLabel) + 2 <= leftX(backend));
    QVERIFY2(rightX(backend) <= rightX(content), "Backend selector must remain inside the narrow viewport");
    QVERIFY2(rightX(dlg.removeCategoryButton()) <= rightX(content),
             "Category actions must remain inside the narrow viewport");
    QVERIFY(backend->fontMetrics().horizontalAdvance(backend->currentText()) <= backend->width() - 24);
    if (!capturePath.isEmpty())
        QVERIFY(dlg.grab().save(capturePath + QStringLiteral(".narrow.png")));

    qApp->setStyleSheet(ThemeManager::styleSheet(true));
    dlg.applyTheme(true);
    QCoreApplication::processEvents();
    QVERIFY(!backendLabel->isVisibleTo(&dlg));
    QVERIFY(dlg.categoryCombo()->height() == 26 && backend->height() == 26);
    if (!capturePath.isEmpty())
        QVERIFY(dlg.grab().save(capturePath + QStringLiteral(".narrow.dark.png")));
    dlg.resize(300, 530);
    QCoreApplication::processEvents();
    QVERIFY(backendLabel->isVisibleTo(&dlg));
    QVERIFY2(verifyRows(), "SAM config rows must remain aligned in the dark theme");
    if (!capturePath.isEmpty())
        QVERIFY(dlg.grab().save(capturePath + QStringLiteral(".dark.png")));
}

void TestSamAnnotatorDialog::categoryDropdownUsesOneCompactRow() {
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setImage(makeTestImage());

    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QString());
    dlg.show();
    QCoreApplication::processEvents();

    auto* combo = dlg.findChild<QComboBox*>(QStringLiteral("SamCategoryCombo"));
    auto* label = dlg.findChild<QLabel*>(QStringLiteral("SamCategoryLabel"));
    QVERIFY(combo);
    QVERIFY(label);
    QVERIFY(combo->isEditable());
    QVERIFY(combo->count() >= 4);
    QVERIFY(qAbs(combo->geometry().center().y() - label->geometry().center().y()) <= 2);
    QVERIFY(qAbs(combo->geometry().center().y() - dlg.addCategoryButton()->geometry().center().y()) <= 2);
    QVERIFY(qAbs(combo->geometry().center().y() - dlg.removeCategoryButton()->geometry().center().y()) <= 2);
    QVERIFY(!dlg.findChild<QListWidget*>(QStringLiteral("SamCategoryList")));
    const QString capturePath = qEnvironmentVariable("DEEPLUX_TEST_SAM_CAPTURE");
    if (!capturePath.isEmpty()) {
        QVERIFY(dlg.grab().save(capturePath));
        dlg.applyTheme(true);
        QCoreApplication::processEvents();
        QVERIFY(dlg.grab().save(capturePath + QStringLiteral(".dark.png")));
    }
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

void TestSamAnnotatorDialog::environmentSetupOnlyStatusIsSanitizedAndCapturesGuiStates() {
    struct EnvironmentRestore {
        QByteArray python = qgetenv("SAM_SERVER_PYTHON");
        QByteArray model = qgetenv("DEEPLUX_SAM_MODEL");
        ~EnvironmentRestore() {
            python.isEmpty() ? qunsetenv("SAM_SERVER_PYTHON") : qputenv("SAM_SERVER_PYTHON", python);
            model.isEmpty() ? qunsetenv("DEEPLUX_SAM_MODEL") : qputenv("DEEPLUX_SAM_MODEL", model);
        }
    } restore;
    qunsetenv("SAM_SERVER_PYTHON");
    qunsetenv("DEEPLUX_SAM_MODEL");

    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    static QTemporaryDir isolatedAppData;
    QVERIFY(isolatedAppData.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", isolatedAppData.path().toLocal8Bit());
    ConfigManager& config = ConfigManager::instance();
    const bool configWasInitialized = config.isInitialized();
    const QString configPath = config.configFilePath();
    const bool configFileExisted = QFileInfo::exists(configPath);
    QVERIFY(!configWasInitialized);
    QVERIFY(!configFileExisted);
    QByteArray configBytesBefore;
    if (configFileExisted) {
        QFile configFile(configPath);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        configBytesBefore = configFile.readAll();
    }
#ifdef Q_OS_WIN
    // Windows 以扩展名判定可执行文件，测试桩必须带 .exe 才会被环境检查视为有效外部 Python。
    const QString pythonPath = temp.filePath(QStringLiteral("python.exe"));
#else
    const QString pythonPath = temp.filePath(QStringLiteral("python"));
#endif
    QFile python(pythonPath);
    QVERIFY(python.open(QIODevice::WriteOnly));
    python.write("#!/bin/sh\nexit 0\n");
    python.close();
    QVERIFY(python.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    const QString modelPath = temp.filePath(QStringLiteral("sam_vit_b.pth"));
    QFile model(modelPath);
    QVERIFY(model.open(QIODevice::WriteOnly));
    model.write("test weights metadata only");
    model.close();

    SamAnnotatorDialog dialog(nullptr, true);
    QCOMPARE(config.isInitialized(), configWasInitialized);
    QCOMPARE(QFileInfo::exists(configPath), configFileExisted);
    if (configFileExisted) {
        QFile configFile(configPath);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        QCOMPARE(configFile.readAll(), configBytesBefore);
    }
    QCOMPARE(qgetenv("SAM_SERVER_PYTHON"), QByteArray());
    QCOMPARE(qgetenv("DEEPLUX_SAM_MODEL"), QByteArray());
    auto* client = dialog.findChild<SamBackendClient*>(QStringLiteral("SamBackendClient"));
    QVERIFY(client);
    client->setModelPath(QString());
    dialog.show();
    QCoreApplication::processEvents();
    const QJsonObject initial = dialog.environmentHandoffStatus();
    QVERIFY(initial.value(QStringLiteral("userActionRequired")).toBool());
    QVERIFY(!initial.value(QStringLiteral("modelLoaded")).toBool());
    QCOMPARE(initial.value(QStringLiteral("environmentSource")).toString(), QStringLiteral("missing"));
    QCOMPARE(initial.value(QStringLiteral("modelPathConfigured")).toBool(), false);
    QCOMPARE(initial.value(QStringLiteral("nextAction")).toString(), QStringLiteral("initialize_environment"));
    QLabel* handoffLabel = dialog.findChild<QLabel*>(QStringLiteral("SamEnvironmentHandoffLabel"));
    QPushButton* initButton = dialog.findChild<QPushButton*>(QStringLiteral("SamInitializeEnvironmentButton"));
    QPushButton* restartButton = dialog.findChild<QPushButton*>(QStringLiteral("SamRestartServerButton"));
    QVERIFY(handoffLabel);
    QVERIFY(initButton);
    QVERIFY(restartButton);
    QVERIFY(handoffLabel->text().contains(QStringLiteral("初始化环境")));
    QVERIFY(initButton->isEnabled());
    QVERIFY(!restartButton->isEnabled());
    QVERIFY(!initial.contains(QStringLiteral("pythonPath")));
    QVERIFY(!initial.contains(QStringLiteral("modelPath")));
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("SamEnvironmentHandoffLabel")));

    const QString evidenceDir = QString::fromLocal8Bit(qgetenv("DEEPLUX_SLICE40_EVIDENCE_DIR"));
    if (!evidenceDir.isEmpty()) {
        QDir().mkpath(evidenceDir);
        QVERIFY(dialog.grab().save(QDir(evidenceDir).filePath(QStringLiteral("sam-setup-before.png"))));
    }

    qputenv("SAM_SERVER_PYTHON", temp.filePath(QStringLiteral("missing-python")).toLocal8Bit());
    QCOMPARE(dialog.environmentHandoffStatus().value(QStringLiteral("nextAction")).toString(),
             QStringLiteral("fix_external_python"));
    const bool repeatedCallConfigInitialized = config.isInitialized();
    const bool repeatedCallConfigExisted = QFileInfo::exists(configPath);
    QByteArray repeatedCallConfigBytes;
    if (repeatedCallConfigExisted) {
        QFile configFile(configPath);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        repeatedCallConfigBytes = configFile.readAll();
    }
    {
        SamAnnotatorDialog invalidPythonDialog(nullptr, true);
        const QJsonObject invalidPythonStatus = invalidPythonDialog.environmentHandoffStatus();
        QCOMPARE(invalidPythonStatus.value(QStringLiteral("nextAction")).toString(),
                 QStringLiteral("fix_external_python"));
        QLabel* invalidLabel = invalidPythonDialog.findChild<QLabel*>(QStringLiteral("SamEnvironmentHandoffLabel"));
        QPushButton* invalidInit =
            invalidPythonDialog.findChild<QPushButton*>(QStringLiteral("SamInitializeEnvironmentButton"));
        QPushButton* invalidRestart =
            invalidPythonDialog.findChild<QPushButton*>(QStringLiteral("SamRestartServerButton"));
        QPushButton* importModel = invalidPythonDialog.findChild<QPushButton*>(QStringLiteral("SamImportModelButton"));
        QVERIFY(invalidLabel);
        QVERIFY(invalidInit);
        QVERIFY(invalidRestart);
        QVERIFY(importModel);
        QVERIFY(invalidLabel->text().contains(QStringLiteral("修正外部 Python")));
        QVERIFY(!invalidInit->isEnabled());
        QVERIFY(!invalidRestart->isEnabled());
        QVERIFY(importModel->isEnabled());
    }
    QCOMPARE(config.isInitialized(), repeatedCallConfigInitialized);
    QCOMPARE(QFileInfo::exists(configPath), repeatedCallConfigExisted);
    if (repeatedCallConfigExisted) {
        QFile configFile(configPath);
        QVERIFY(configFile.open(QIODevice::ReadOnly));
        QCOMPARE(configFile.readAll(), repeatedCallConfigBytes);
    }
    qputenv("SAM_SERVER_PYTHON", pythonPath.toLocal8Bit());
    client->setModelPath(temp.filePath(QStringLiteral("missing-sam_vit_b.pth")));
    const QJsonObject missingModel = dialog.environmentHandoffStatus();
    QVERIFY(!missingModel.value(QStringLiteral("modelFileAvailable")).toBool());
    QCOMPARE(missingModel.value(QStringLiteral("nextAction")).toString(), QStringLiteral("import_model"));
    const QString unsupportedModelPath = temp.filePath(QStringLiteral("sam3-vit-b.pth"));
    QFile unsupportedModel(unsupportedModelPath);
    QVERIFY(unsupportedModel.open(QIODevice::WriteOnly));
    unsupportedModel.write("metadata only");
    unsupportedModel.close();
    client->setModelPath(unsupportedModelPath);
    QCOMPARE(dialog.environmentHandoffStatus().value(QStringLiteral("modelSupported")).toString(),
             QStringLiteral("unsupported"));
    QCOMPARE(dialog.environmentHandoffStatus().value(QStringLiteral("nextAction")).toString(),
             QStringLiteral("import_model"));

    auto* view = new HImageWidget;
    view->setImage(makeTestImage());
    dialog.attachToImageWidgetForEnvironmentSetup(view);
    QVERIFY(dialog.environmentHandoffStatus().value(QStringLiteral("imageAvailable")).toBool());
    QCOMPARE(dialog.backendImagePathForTest(), QString());

    client->setModelPath(modelPath);
    SamTestServer server;
    QVERIFY(server.listen());
    client->setServerUrl(server.url());
    server.healthMode = QStringLiteral("sam");
    client->healthCheck();
    QTRY_COMPARE_WITH_TIMEOUT(client->state(), SamBackendClient::State::Ready, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(!client->healthCheckInFlight(), 1000);
    const QJsonObject ready = dialog.environmentHandoffStatus();
    QVERIFY(ready.value(QStringLiteral("environmentConfigured")).toBool());
    QVERIFY(ready.value(QStringLiteral("modelFileAvailable")).toBool());
    QVERIFY(ready.value(QStringLiteral("modelLoaded")).toBool());
    QCOMPARE(ready.value(QStringLiteral("serverMode")).toString(), QStringLiteral("sam"));
    QVERIFY(!ready.contains(QStringLiteral("pythonPath")));
    QVERIFY(!ready.contains(QStringLiteral("modelPath")));
    QCOMPARE(ready.value(QStringLiteral("nextAction")).toString(), QStringLiteral("none"));
    if (!evidenceDir.isEmpty())
        QVERIFY(dialog.grab().save(QDir(evidenceDir).filePath(QStringLiteral("sam-setup-after.png"))));

    view->clearImage();
    const QJsonObject cleared = dialog.environmentHandoffStatus();
    QVERIFY(!cleared.value(QStringLiteral("imageAvailable")).toBool());
    QCOMPARE(cleared.value(QStringLiteral("nextAction")).toString(), QStringLiteral("open_image"));
    view->setImage(makeTestImage());
    QCoreApplication::processEvents();
    QCOMPARE(dialog.backendImagePathForTest(), QString());
    QVERIFY(!dialog.session().imageDigest.isEmpty());
    QVERIFY(handoffLabel->text().contains(QStringLiteral("环境、模型和服务已就绪")));
    delete view;
    const QJsonObject destroyed = dialog.environmentHandoffStatus();
    QVERIFY(!destroyed.value(QStringLiteral("imageAvailable")).toBool());
    QCOMPARE(destroyed.value(QStringLiteral("nextAction")).toString(), QStringLiteral("open_image"));

    server.healthMode = QStringLiteral("stub");
    client->healthCheck();
    QTRY_COMPARE_WITH_TIMEOUT(client->state(), SamBackendClient::State::Ready, 3000);
    const QJsonObject stub = dialog.environmentHandoffStatus();
    QCOMPARE(stub.value(QStringLiteral("serverMode")).toString(), QStringLiteral("stub"));
    QVERIFY(!stub.value(QStringLiteral("modelLoaded")).toBool());
    QCOMPARE(stub.value(QStringLiteral("nextAction")).toString(), QStringLiteral("verify_model_service"));

    server.delayMs.insert(QStringLiteral("/health"), 100);
    client->healthCheck();
    const QJsonObject checking = dialog.environmentHandoffStatus();
    QVERIFY(checking.value(QStringLiteral("healthCheckInFlight")).toBool());
    QCOMPARE(checking.value(QStringLiteral("nextAction")).toString(), QStringLiteral("wait_for_status"));
    QTRY_VERIFY_WITH_TIMEOUT(!client->healthCheckInFlight(), 3000);

    QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 0);
    server.healthMode = QStringLiteral("sam");
    client->healthCheck();
    QTRY_COMPARE_WITH_TIMEOUT(client->state(), SamBackendClient::State::Ready, 3000);
    QSignalSpy embeddingReadySpy(client, &SamBackendClient::embeddingReady);
    client->setImage(QStringLiteral("temporary-test-image.png"));
    QTRY_VERIFY_WITH_TIMEOUT(embeddingReadySpy.count() == 1, 3000);
    server.delayMs.insert(QStringLiteral("/predict"), 300);
    client->predict({QPointF(5, 5)}, {}, QRectF());
    QCOMPARE(client->state(), SamBackendClient::State::Busy);
    const QJsonObject busy = dialog.environmentHandoffStatus();
    QCOMPARE(busy.value(QStringLiteral("serverState")).toString(), QStringLiteral("busy"));
    QCOMPARE(busy.value(QStringLiteral("nextAction")).toString(), QStringLiteral("wait_for_status"));
    QVERIFY(handoffLabel->text().contains(QStringLiteral("等待完成")));
    QTRY_COMPARE_WITH_TIMEOUT(client->state(), SamBackendClient::State::Ready, 3000);
    server.delayMs.remove(QStringLiteral("/predict"));

    server.healthStatus = QStringLiteral("error");
    server.delayMs.remove(QStringLiteral("/health"));
    client->healthCheck();
    QTRY_COMPARE_WITH_TIMEOUT(client->state(), SamBackendClient::State::Error, 3000);
    const QJsonObject unavailable = dialog.environmentHandoffStatus();
    QCOMPARE(unavailable.value(QStringLiteral("serverState")).toString(), QStringLiteral("error"));
    QCOMPARE(unavailable.value(QStringLiteral("nextAction")).toString(), QStringLiteral("restart_sam"));

    QCOMPARE(server.requestLog.count(QStringLiteral("POST /set_image")), 1);
    QVERIFY(!server.requestLog.contains(QStringLiteral("POST /unload_image")));

    QCOMPARE(qgetenv("DEEPLUX_SAM_MODEL"), QByteArray()); // setup-only never writes the process environment
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

void TestSamAnnotatorDialog::categoryDropdownPrepopulated() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.categoryCombo() != nullptr);
    QVERIFY(dlg.categoryCombo()->count() >= 4);
    QVERIFY(dlg.categoryCombo()->itemData(0, Qt::DecorationRole).canConvert<QColor>());
    QVERIFY(dlg.categories().contains(QStringLiteral("缺陷")));
    QVERIFY(dlg.categories().contains(QStringLiteral("划痕")));
    QVERIFY(dlg.categories().contains(QStringLiteral("凹坑")));
    QVERIFY(dlg.categories().contains(QStringLiteral("正常")));
}

void TestSamAnnotatorDialog::categoryCanBeAdded() {
    SamAnnotatorDialog dlg;
    const int initial = dlg.categories().size();
    dlg.categoryCombo()->setEditText(QStringLiteral("裂纹"));
    QTest::keyClick(dlg.categoryCombo()->lineEdit(), Qt::Key_Return);
    QVERIFY(dlg.categories().contains(QStringLiteral("裂纹")));
    QCOMPARE(dlg.categories().size(), initial + 1);
    QCOMPARE(dlg.categoryCombo()->currentText(), QStringLiteral("裂纹"));
    QVERIFY(dlg.categoryColor(QStringLiteral("裂纹")).isValid());
}

void TestSamAnnotatorDialog::categoryCanBeRemoved() {
    SamAnnotatorDialog dlg;
    dlg.addCategory(QStringLiteral("临时"));
    QVERIFY(dlg.categories().contains(QStringLiteral("临时")));
    dlg.removeCategoryButton()->click();
    QVERIFY(!dlg.categories().contains(QStringLiteral("临时")));
    QCOMPARE(dlg.categoryCombo()->findText(QStringLiteral("临时")), -1);
}

void TestSamAnnotatorDialog::testUserAddsAndRemovesSamCategoryThroughGui() {
    SamAnnotatorDialog dlg;
    const QString category = QStringLiteral("slice22-临时类别");
    const int initialCount = dlg.categoryCombo()->count();

    dlg.categoryCombo()->setEditText(category);
    dlg.addCategoryButton()->click();

    QCOMPARE(dlg.categoryCombo()->count(), initialCount + 1);
    QVERIFY(dlg.categories().contains(category));
    QCOMPARE(dlg.categoryCombo()->itemText(initialCount), category);

    dlg.categoryCombo()->setCurrentIndex(initialCount);
    dlg.removeCategoryButton()->click();

    QCOMPARE(dlg.categoryCombo()->count(), initialCount);
    QVERIFY(!dlg.categories().contains(category));
    for (int row = 0; row < dlg.categoryCombo()->count(); ++row)
        QVERIFY(dlg.categoryCombo()->itemText(row) != category);
}

void TestSamAnnotatorDialog::currentCategoryLabelUsesComboSelection() {
    SamAnnotatorDialog dlg;
    if (dlg.categoryCombo()->count() > 0) {
        dlg.categoryCombo()->setCurrentIndex(0);
        QVERIFY(!dlg.currentCategoryLabel().isEmpty());
    }
}

void TestSamAnnotatorDialog::currentCategoryLabelPrefersDropdownSelection() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.categoryCombo()->count() > 1);
    dlg.categoryCombo()->setCurrentIndex(1);
    QCOMPARE(dlg.currentCategoryLabel(), dlg.categoryCombo()->itemText(1));
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

// SAM 批4：COCO 导出入口（导出逻辑由 test_cocoexporter 覆盖）
void TestSamAnnotatorDialog::exportCocoButtonExists() {
    SamAnnotatorDialog dlg;
    QVERIFY(dlg.exportCocoButton() != nullptr);
    QVERIFY(dlg.exportCocoButton()->text().contains(QStringLiteral("COCO")));
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

    QTemporaryDir sessionDir;
    QVERIFY(sessionDir.isValid());
    const QString sessionPath = sessionDir.filePath(QStringLiteral("anno.deeplux-anno.json"));
    QString err;
    QVERIFY2(srcDlg.session().save(sessionPath, &err), qPrintable(err));

    // Now open the saved session in a fresh dialog via onOpenAnnotation (we invoke directly with a path)
    SamAnnotatorDialog dlg;
    QString loadErr;
    AnnotationSession loaded = AnnotationSession::load(sessionPath, &loadErr);
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
    auto* menuButton = dlg.findChild<QToolButton*>(QStringLiteral("SamMaskEditMenuButton"));
    QVERIFY(menuButton);
    QVERIFY(menuButton->menu());
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

void TestSamAnnotatorDialog::maskEditMenuIsContextual() {
    struct AppStyleRestore {
        QString oldStyle = qApp->styleSheet();
        ~AppStyleRestore() {
            qApp->setStyleSheet(oldStyle);
        }
    } restore;
    qApp->setStyleSheet(QStringLiteral("QWidget { background-color: #2b2b2b; color: #ffffff; }"));
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setImage(makeTestImage());
    SamAnnotatorDialog dlg;
    dlg.attachToImageWidget(&mainView, QString());
    dlg.show();
    QCoreApplication::processEvents();
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                           makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}))
                 .isEmpty());
    QVERIFY(!confirmObject(dlg, squarePolygon(100, 80, 20), QRectF(100, 80, 20, 20),
                           makeDisplayMask(200, 150, {QRect(100, 80, 20, 20)}))
                 .isEmpty());

    auto* menuButton = dlg.findChild<QToolButton*>(QStringLiteral("SamMaskEditMenuButton"));
    QVERIFY(menuButton);
    dlg.objectList()->setCurrentRow(0);
    QVERIFY(menuButton->isEnabled());
    QVERIFY(!dlg.eraserButton()->parentWidget()->isHidden());
    QVERIFY(dlg.mergeButton()->isHidden());
    auto* menu = menuButton->menu();
    menu->popup(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
    QCoreApplication::processEvents();
    QVERIFY(menu->isVisible());
    auto* menuContent = menu->findChild<QWidget*>(QStringLiteral("SamMaskEditMenuContent"));
    QVERIFY(menuContent);
    const QColor lightBg = menuContent->grab().toImage().pixelColor(1, 1);
    QVERIFY2(lightBg.lightness() > 200, qPrintable(QStringLiteral("Light menu background: %1").arg(lightBg.name())));
    const QColor singleBg =
        dlg.eraserButton()->parentWidget()->grab().toImage().pixelColor(5, dlg.eraserButton()->height() + 2);
    QVERIFY2(singleBg.lightness() > 200, qPrintable(QStringLiteral("Light mask controls: %1").arg(singleBg.name())));
    const int singleMenuHeight = menu->height();
    const QString capturePath = qEnvironmentVariable("DEEPLUX_TEST_SAM_MENU_CAPTURE");
    if (!capturePath.isEmpty()) {
        QVERIFY(dlg.grab().save(capturePath));
        QVERIFY(menu->grab().save(capturePath + QStringLiteral(".menu.png")));
    }
    menu->hide();
    dlg.eraserButton()->click();
    QCOMPARE(dlg.currentToolMode(), SamAnnotatorDialog::ToolMode::Eraser);

    dlg.objectList()->selectionModel()->select(dlg.objectList()->model()->index(1, 0), QItemSelectionModel::Select);
    QVERIFY(menuButton->isEnabled());
    QVERIFY(dlg.eraserButton()->parentWidget()->isHidden());
    QVERIFY(!dlg.mergeButton()->isHidden());
    QCOMPARE(dlg.currentToolMode(), SamAnnotatorDialog::ToolMode::Select);
    menu->popup(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
    QCoreApplication::processEvents();
    QVERIFY2(menu->height() < singleMenuHeight, "Two-object menu should shrink to the merge action");
    if (!capturePath.isEmpty())
        QVERIFY(menu->grab().save(capturePath + QStringLiteral(".pair.png")));
    menu->hide();

    dlg.objectList()->setCurrentRow(0);
    dlg.applyTheme(true);
    menu->popup(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
    QCoreApplication::processEvents();
    const QColor darkBg = menuContent->grab().toImage().pixelColor(1, 1);
    QVERIFY2(darkBg.lightness() < 100, qPrintable(QStringLiteral("Dark menu background: %1").arg(darkBg.name())));
    if (!capturePath.isEmpty())
        QVERIFY(menu->grab().save(capturePath + QStringLiteral(".dark.png")));
    menu->hide();
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
    dlg.categoryCombo()->setEditText(QStringLiteral("A"));
    const QString idA = confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                                      makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}));
    dlg.categoryCombo()->setEditText(QStringLiteral("B"));
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

void TestSamAnnotatorDialog::agentMaskOperationsValidateAndUndo() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    const QString firstId = confirmObject(dlg, squarePolygon(20, 20, 20), QRectF(20, 20, 20, 20),
                                          makeDisplayMask(200, 150, {QRect(20, 20, 20, 20)}));
    const QString secondId = confirmObject(dlg, squarePolygon(38, 20, 20), QRectF(38, 20, 20, 20),
                                           makeDisplayMask(200, 150, {QRect(38, 20, 20, 20)}));
    const QImage splitMask = makeDisplayMask(200, 150, {QRect(80, 30, 10, 10), QRect(100, 30, 10, 10)});
    const QString splitId = confirmObject(dlg, squarePolygon(80, 30, 10), QRectF(80, 30, 30, 10), splitMask);
    QVERIFY(!firstId.isEmpty() && !secondId.isEmpty() && !splitId.isEmpty());

    const QJsonObject firstPage = dlg.listAgentAnnotationObjects(0, 2);
    QCOMPARE(firstPage.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(firstPage.value(QStringLiteral("total")).toInt(), 3);
    QCOMPARE(firstPage.value(QStringLiteral("objects")).toArray().size(), 2);
    QVERIFY(firstPage.value(QStringLiteral("hasMore")).toBool());
    QCOMPARE(firstPage.value(QStringLiteral("nextOffset")).toInt(), 2);
    const QJsonObject lastPage =
        dlg.listAgentAnnotationObjects(firstPage.value(QStringLiteral("nextOffset")).toInt(), 2);
    QCOMPARE(lastPage.value(QStringLiteral("objects")).toArray().size(), 1);
    QVERIFY(!lastPage.value(QStringLiteral("hasMore")).toBool());
    QCOMPARE(
        dlg.listAgentAnnotationObjects(std::numeric_limits<int>::max(), 50).value(QStringLiteral("status")).toString(),
        QStringLiteral("ok"));

    const AnnotationSession before = dlg.session();
    auto edit = [&dlg](const QString& operation, const QJsonArray& ids, const QJsonObject& extra = {}) {
        QJsonObject params{{QStringLiteral("operation"), operation}, {QStringLiteral("objectIds"), ids}};
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
            params.insert(it.key(), it.value());
        return dlg.applyAgentMaskEdit(params);
    };
    const QJsonArray firstObject{firstId};
    QCOMPARE(edit(QStringLiteral("dilate"), firstObject, {{QStringLiteral("radiusPx"), 2}})
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("updated"));
    QCOMPARE(dlg.session().annotations.first().bbox, QRectF(18, 18, 24, 24));
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.first().maskRle, before.annotations.first().maskRle);
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.first().bbox, QRectF(18, 18, 24, 24));

    const AnnotationSession beforeErode = dlg.session();
    QCOMPARE(edit(QStringLiteral("erode"), firstObject, {{QStringLiteral("radiusPx"), 1}})
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("updated"));
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.first().maskRle, beforeErode.annotations.first().maskRle);
    dlg.redoButton()->click();

    const AnnotationSession beforeErase = dlg.session();
    const QJsonArray stroke{QJsonArray{25, 25}, QJsonArray{29, 25}, QJsonArray{33, 25}};
    QCOMPARE(edit(QStringLiteral("erase_stroke"), firstObject,
                  {{QStringLiteral("radiusPx"), 2}, {QStringLiteral("strokePoints"), stroke}})
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("updated"));
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.first().maskRle, beforeErase.annotations.first().maskRle);
    dlg.redoButton()->click();

    const QJsonObject merged = edit(QStringLiteral("merge"), QJsonArray{firstId, secondId});
    QCOMPARE(merged.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
    QCOMPARE(dlg.session().annotations.size(), 2);
    QCOMPARE(dlg.session().annotations.first().id, firstId);
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 3);
    QCOMPARE(dlg.session().annotations.at(1).id, secondId);
    dlg.redoButton()->click();
    dlg.undoButton()->click();

    const QJsonObject split = edit(QStringLiteral("split"), QJsonArray{splitId});
    QCOMPARE(split.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
    QCOMPARE(split.value(QStringLiteral("newObjectIds")).toArray().size(), 1);
    QCOMPARE(dlg.session().annotations.size(), 4);
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 3);
    QCOMPARE(dlg.session().annotations.at(2).id, splitId);
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 4);

    QList<QRect> componentRects;
    for (int i = 0; i < 64; ++i)
        componentRects.append(QRect(2 + (i % 8) * 4, 80 + (i / 8) * 4, 1, 1));
    const QImage sixtyFourMask = makeDisplayMask(200, 150, componentRects);
    const QString sixtyFourId = confirmObject(dlg, squarePolygon(2, 80, 1), QRectF(2, 80, 29, 29), sixtyFourMask);
    const int beforeSixtyFourSplit = dlg.session().annotations.size();
    const QJsonObject sixtyFourSplit = edit(QStringLiteral("split"), QJsonArray{sixtyFourId});
    QCOMPARE(sixtyFourSplit.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
    QCOMPARE(sixtyFourSplit.value(QStringLiteral("newObjectIds")).toArray().size(), 63);
    QCOMPARE(dlg.session().annotations.size(), beforeSixtyFourSplit + 63);
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), beforeSixtyFourSplit);

    componentRects.append(QRect(34, 80, 1, 1));
    const QImage sixtyFiveMask = makeDisplayMask(200, 150, componentRects);
    const QString sixtyFiveId = confirmObject(dlg, squarePolygon(2, 80, 1), QRectF(2, 80, 33, 29), sixtyFiveMask);
    const int beforeRejectedSplit = dlg.session().annotations.size();
    const QString selectionBeforeRejectedSplit = dlg.overlayWidget()->selectedId();
    const QImage firstOverlayBeforeRejectedSplit = dlg.overlayWidget()->objectMask(firstId);
    const bool undoEnabledBeforeRejectedSplit = dlg.undoButton()->isEnabled();
    const QJsonObject sixtyFiveSplit = edit(QStringLiteral("split"), QJsonArray{sixtyFiveId});
    QCOMPARE(sixtyFiveSplit.value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("MASK_RESOURCE_LIMIT_EXCEEDED"));
    QCOMPARE(dlg.session().annotations.size(), beforeRejectedSplit);
    QCOMPARE(dlg.overlayWidget()->selectedId(), selectionBeforeRejectedSplit);
    QCOMPARE(dlg.overlayWidget()->objectMask(firstId), firstOverlayBeforeRejectedSplit);
    QCOMPARE(dlg.undoButton()->isEnabled(), undoEnabledBeforeRejectedSplit);

    const AnnotationSession beforeRejected = dlg.session();
    const QString selectionBeforeRejected = dlg.overlayWidget()->selectedId();
    const QImage firstOverlayBeforeRejected = dlg.overlayWidget()->objectMask(firstId);
    const bool undoEnabledBeforeRejected = dlg.undoButton()->isEnabled();
    const QJsonObject unknownParam = edit(QStringLiteral("dilate"), firstObject,
                                          {{QStringLiteral("radiusPx"), 2}, {QStringLiteral("unexpected"), true}});
    QCOMPARE(unknownParam.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject badRadius = edit(QStringLiteral("dilate"), firstObject, {{QStringLiteral("radiusPx"), 0}});
    QCOMPARE(badRadius.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject missingRadius = edit(QStringLiteral("dilate"), firstObject);
    QCOMPARE(missingRadius.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject wrongRadiusType =
        edit(QStringLiteral("dilate"), firstObject, {{QStringLiteral("radiusPx"), QStringLiteral("2")}});
    QCOMPARE(wrongRadiusType.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject badStroke =
        edit(QStringLiteral("erase_stroke"), firstObject,
             {{QStringLiteral("radiusPx"), 2}, {QStringLiteral("strokePoints"), QJsonArray{QJsonValue(QJsonArray{-1, 25})}}});
    QCOMPARE(badStroke.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    QCOMPARE(edit(QStringLiteral("merge"), QJsonArray{firstId, firstId}).value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("INVALID_PARAM"));
    QCOMPARE(edit(QStringLiteral("split"), QJsonArray{QStringLiteral("missing-object")})
                 .value(QStringLiteral("errorCode"))
                 .toString(),
             QStringLiteral("ANNOTATION_OBJECT_NOT_FOUND"));
    QCOMPARE(edit(QStringLiteral("merge"), QJsonArray{firstId, splitId}).value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("MASK_OPERATION_REJECTED"));

    const QJsonObject nonIntegerRadius =
        edit(QStringLiteral("dilate"), firstObject, {{QStringLiteral("radiusPx"), 1.5}});
    QCOMPARE(nonIntegerRadius.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject malformedPoint =
        edit(QStringLiteral("erase_stroke"), firstObject,
             {{QStringLiteral("radiusPx"), 2}, {QStringLiteral("strokePoints"), QJsonArray{QJsonValue(QJsonArray{25})}}});
    QCOMPARE(malformedPoint.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject emptyStroke =
        edit(QStringLiteral("erase_stroke"), firstObject,
             {{QStringLiteral("radiusPx"), 2}, {QStringLiteral("strokePoints"), QJsonArray{}}});
    QCOMPARE(emptyStroke.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    QJsonArray maxStroke;
    for (int i = 0; i < 4096; ++i)
        maxStroke.append(QJsonArray{5, 5}); // valid image coordinates; misses the object
    const QJsonObject maxStrokeResult =
        dlg.applyAgentMaskEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("erase_stroke")},
                                           {QStringLiteral("objectIds"), firstObject},
                                           {QStringLiteral("radiusPx"), 2},
                                           {QStringLiteral("strokePoints"), maxStroke}},
                               true);
    QCOMPARE(maxStrokeResult.value(QStringLiteral("status")).toString(), QStringLiteral("unchanged"));
    maxStroke.append(QJsonArray{5, 5});
    const QJsonObject tooManyStrokePoints =
        dlg.applyAgentMaskEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("erase_stroke")},
                                           {QStringLiteral("objectIds"), firstObject},
                                           {QStringLiteral("radiusPx"), 2},
                                           {QStringLiteral("strokePoints"), maxStroke}},
                               true);
    QCOMPARE(tooManyStrokePoints.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject nonFiniteStroke = dlg.applyAgentMaskEdit(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("erase_stroke")},
                    {QStringLiteral("objectIds"), firstObject},
                    {QStringLiteral("radiusPx"), 2},
                    {QStringLiteral("strokePoints"),
                     QJsonArray{QJsonValue(QJsonArray{QJsonValue(std::numeric_limits<double>::quiet_NaN()), 25})}}},
        true);
    QCOMPARE(nonFiniteStroke.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject infiniteStroke = dlg.applyAgentMaskEdit(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("erase_stroke")},
                    {QStringLiteral("objectIds"), firstObject},
                    {QStringLiteral("radiusPx"), 2},
                    {QStringLiteral("strokePoints"),
                     QJsonArray{QJsonValue(QJsonArray{QJsonValue(std::numeric_limits<double>::infinity()), 25})}}},
        true);
    QCOMPARE(infiniteStroke.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    QCOMPARE(dlg.session().annotations.size(), beforeRejected.annotations.size());
    for (int i = 0; i < beforeRejected.annotations.size(); ++i) {
        QCOMPARE(dlg.session().annotations.at(i).id, beforeRejected.annotations.at(i).id);
        QCOMPARE(dlg.session().annotations.at(i).maskRle, beforeRejected.annotations.at(i).maskRle);
    }
    QCOMPARE(dlg.overlayWidget()->selectedId(), selectionBeforeRejected);
    QCOMPARE(dlg.overlayWidget()->objectMask(firstId), firstOverlayBeforeRejected);
    QCOMPARE(dlg.undoButton()->isEnabled(), undoEnabledBeforeRejected);
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), beforeRejected.annotations.size() - 1);
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), beforeRejected.annotations.size());

    const QString polygonOnlyId = confirmObject(dlg, squarePolygon(120, 90, 15), QRectF(120, 90, 15, 15), QImage());
    QVERIFY(!polygonOnlyId.isEmpty());
    const QString originalPolygonOnlyRle = dlg.session().annotations.last().maskRle;
    QCOMPARE(originalPolygonOnlyRle, QString());
    const QJsonObject polygonFallback =
        edit(QStringLiteral("dilate"), QJsonArray{polygonOnlyId}, {{QStringLiteral("radiusPx"), 1}});
    QCOMPARE(polygonFallback.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
    QVERIFY(!dlg.session().annotations.last().maskRle.isEmpty());
    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.last().maskRle, originalPolygonOnlyRle);
    dlg.redoButton()->click();

    dlg.categoryCombo()->setCurrentIndex(-1);
    dlg.categoryCombo()->setEditText(QString(32760, QLatin1Char('a')));
    QVERIFY(!confirmObject(dlg, squarePolygon(150, 20, 5), QRectF(150, 20, 5, 5),
                           makeDisplayMask(200, 150, {QRect(150, 20, 5, 5)}))
                 .isEmpty());
    dlg.categoryCombo()->setEditText(QString(32760, QLatin1Char('b')));
    QVERIFY(!confirmObject(dlg, squarePolygon(160, 20, 5), QRectF(160, 20, 5, 5),
                           makeDisplayMask(200, 150, {QRect(160, 20, 5, 5)}))
                 .isEmpty());
    const QJsonObject cappedPage = dlg.listAgentAnnotationObjects(0, 50);
    QCOMPARE(cappedPage.value(QStringLiteral("errorCode")).toString(), QStringLiteral("RESPONSE_LIMIT_EXCEEDED"));
    QCOMPARE(dlg.listAgentAnnotationObjects(0, 1).value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
}

void TestSamAnnotatorDialog::activeAnnotationObjectsReportSourceBboxWhenVerified() {
    const QImage image = makeTestImage();
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("source-image-fixture");
    provenance.sourceSize = QSize(320, 240);
    provenance.localOriginInSource = QPointF(10, 20);
    HImageWidget viewportImage;
    viewportImage.setImage(image, provenance);
    SamAnnotatorDialog dlg(nullptr, true);
    dlg.attachToImageWidgetForEnvironmentSetup(&viewportImage);
    QVERIFY(!confirmObject(dlg, squarePolygon(20, 30, 10), QRectF(20, 30, 10, 10),
                           makeDisplayMask(200, 150, {QRect(20, 30, 10, 10)}))
                 .isEmpty());

    QJsonObject result = dlg.listAgentAnnotationObjects(0, 10);
    QCOMPARE(result.value(QStringLiteral("coordinateFrame")).toString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(result.value(QStringLiteral("sourceCoordinateAvailable")).toBool());
    const QJsonObject object = result.value(QStringLiteral("objects")).toArray().first().toObject();
    QCOMPARE(object.value(QStringLiteral("bbox")).toObject().value(QStringLiteral("x")).toDouble(), 20.0);
    QCOMPARE(object.value(QStringLiteral("bbox")).toObject().value(QStringLiteral("y")).toDouble(), 30.0);
    QCOMPARE(object.value(QStringLiteral("sourceImageId")).toString(), QStringLiteral("source-image-fixture"));
    QCOMPARE(object.value(QStringLiteral("sourceImageSize")).toObject().value(QStringLiteral("width")).toInt(), 320);
    const QJsonObject sourceBbox = object.value(QStringLiteral("sourceBbox")).toObject();
    QCOMPARE(sourceBbox.value(QStringLiteral("x")).toDouble(), 30.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("y")).toDouble(), 50.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("width")).toDouble(), 10.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("height")).toDouble(), 10.0);

    viewportImage.setImage(image, ImageCoordinateProvenance());
    result = dlg.listAgentAnnotationObjects(0, 10);
    QVERIFY(!result.value(QStringLiteral("sourceCoordinateAvailable")).toBool());
    const QJsonObject unavailableObject = result.value(QStringLiteral("objects")).toArray().first().toObject();
    QVERIFY(!unavailableObject.contains(QStringLiteral("sourceImageId")));
    QVERIFY(!unavailableObject.contains(QStringLiteral("sourceImageSize")));
    QVERIFY(!unavailableObject.contains(QStringLiteral("sourceBbox")));

    ImageCoordinateProvenance affineProvenance;
    affineProvenance.sourceImageId = QStringLiteral("affine-source");
    affineProvenance.sourceSize = QSize(320, 240);
    QVERIFY(affineProvenance.composeLocalToSource({{2.0, 0.0, 5.0, 0.0, 3.0, 7.0, 0.0, 0.0, 1.0}}));
    viewportImage.setImage(image, affineProvenance);
    result = dlg.listAgentAnnotationObjects(0, 10);
    const QJsonObject affineObject = result.value(QStringLiteral("objects")).toArray().first().toObject();
    const QJsonObject affineSourceBbox = affineObject.value(QStringLiteral("sourceBbox")).toObject();
    QCOMPARE(affineSourceBbox.value(QStringLiteral("x")).toDouble(), 45.0);
    QCOMPARE(affineSourceBbox.value(QStringLiteral("y")).toDouble(), 97.0);
    QCOMPARE(affineSourceBbox.value(QStringLiteral("width")).toDouble(), 20.0);
    QCOMPARE(affineSourceBbox.value(QStringLiteral("height")).toDouble(), 30.0);

    ImageCoordinateProvenance horizonProvenance;
    horizonProvenance.sourceImageId = QStringLiteral("horizon-source");
    horizonProvenance.sourceSize = QSize(320, 240);
    QVERIFY(horizonProvenance.composeLocalToSource({{1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -25.0}}));
    QVERIFY(horizonProvenance.mapLocalToSource(QPointF(20.0, 30.0)));
    QVERIFY(horizonProvenance.mapLocalToSource(QPointF(30.0, 40.0)));
    QVERIFY(!horizonProvenance.mapRectToSourceBounds(QRectF(20.0, 30.0, 10.0, 10.0)));
    viewportImage.setImage(image, horizonProvenance);
    result = dlg.listAgentAnnotationObjects(0, 10);
    const QJsonObject horizonObject = result.value(QStringLiteral("objects")).toArray().first().toObject();
    QVERIFY(horizonObject.value(QStringLiteral("bbox")).isObject());
    QVERIFY(!horizonObject.contains(QStringLiteral("sourceBbox")));
}

void TestSamAnnotatorDialog::sharedImageProvenanceRebindUpdatesSessionAndKeepsLocalState() {
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setFixedSize(320, 240);

    const QImage image = makeTestImage();
    QImage changedImage = image.copy();
    changedImage.setPixelColor(0, 0, QColor(Qt::magenta));

    ImageCoordinateProvenance provenanceA;
    provenanceA.sourceImageId = QStringLiteral("slice165-source-A");
    provenanceA.sourceSize = QSize(320, 240);
    provenanceA.localOriginInSource = QPointF(50.0, 40.0);
    QVERIFY(provenanceA.isValid());
    mainView.setImage(image, provenanceA);

    SamAnnotatorDialog dlg(nullptr, true);
    dlg.attachToImageWidgetForEnvironmentSetup(&mainView);
    mainView.show();
    QCoreApplication::processEvents();

    QString oldObjectId;
    QList<QPointF> expectedPrompts;
    bool expectPreview = true;
    QJsonObject expectedPromptSnapshot;
    QImage expectedMask;
    int expectedUndoCount = 0;
    int expectedUndoIndex = 0;
    const auto prepareOldState = [&]() {
        expectedPrompts.clear();
        expectPreview = true;
        oldObjectId = confirmObject(dlg, squarePolygon(40, 30, 20), QRectF(40, 30, 20, 20),
                                    makeDisplayMask(200, 150, {QRect(40, 30, 20, 20)}));
        QVERIFY(!oldObjectId.isEmpty());
        QVERIFY(dlg.undoStackCount() > 0);

        mainView.actualSize();
        mainView.setZoom(2.0);
        QTest::mousePress(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(10, 10));
        QMouseEvent panMove(QEvent::MouseMove, QPointF(25, 5), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
        QApplication::sendEvent(dlg.overlayWidget(), &panMove);
        QTest::mouseRelease(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(25, 5));

        dlg.positivePointButton()->click();
        QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(235, 160));
        expectedPrompts.append(QPointF(80.0, 60.0));
        QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), expectedPrompts);
        dlg.boxButton()->click();
        QSignalSpy dragSpy(dlg.overlayWidget(), &AnnotationOverlayWidget::dragEnded);
        QTest::mousePress(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(155, 100));
        QMouseEvent boxMove(QEvent::MouseMove, QPointF(275, 220), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dlg.overlayWidget(), &boxMove);
        QTest::mouseRelease(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(275, 220));
        QCOMPARE(dragSpy.size(), 1);

        QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection,
                                          Q_ARG(QList<QPointF>, squarePolygon(40, 30, 20)),
                                          Q_ARG(QRectF, QRectF(40, 30, 20, 20)), Q_ARG(double, 0.9),
                                          Q_ARG(QString, QStringLiteral("preview-rle")), Q_ARG(QImage, QImage())));
        QVERIFY(!dlg.overlayWidget()->previewPolygon().isEmpty());
        QCOMPARE(dlg.session().annotations.size(), 1);
        expectedPromptSnapshot = dlg.agentPromptStateSnapshot();
        expectedMask = dlg.overlayWidget()->objectMask(oldObjectId);
        expectedUndoCount = dlg.undoStackCount();
        expectedUndoIndex = dlg.undoStackIndex();
    };
    const auto verifyOldStatePreserved = [&]() {
        QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), expectedPrompts);
        QCOMPARE(dlg.agentPromptStateSnapshot(), expectedPromptSnapshot);
        QCOMPARE(dlg.session().annotations.size(), 1);
        QCOMPARE(dlg.undoStackCount(), expectedUndoCount);
        QCOMPARE(dlg.undoStackIndex(), expectedUndoIndex);
        QCOMPARE(!dlg.overlayWidget()->previewPolygon().isEmpty(), expectPreview);
        QCOMPARE(dlg.overlayWidget()->objectMask(oldObjectId), expectedMask);
    };
    const auto verifyOldStateCleared = [&]() {
        QVERIFY(dlg.overlayWidget()->positivePromptPoints().isEmpty());
        QVERIFY(dlg.overlayWidget()->negativePromptPoints().isEmpty());
        QVERIFY(dlg.overlayWidget()->previewPolygon().isEmpty());
        QCOMPARE(dlg.session().annotations.size(), 0);
        QVERIFY(dlg.overlayWidget()->objectMask(oldObjectId).isNull());
        QCOMPARE(dlg.undoStackCount(), 0);
        QCOMPARE(dlg.undoStackIndex(), 0);
    };

    prepareOldState();

    ImageCoordinateProvenance equivalentA;
    equivalentA.sourceImageId = provenanceA.sourceImageId;
    equivalentA.sourceSize = provenanceA.sourceSize;
    QVERIFY(equivalentA.composeLocalToSource({{1.0, 0.0, 50.0, 0.0, 1.0, 40.0, 0.0, 0.0, 1.0}}));
    QCOMPARE(equivalentA.localToSourceMatrix(), provenanceA.localToSourceMatrix());
    mainView.setImage(image, equivalentA);
    verifyOldStatePreserved();

    mainView.setImage(changedImage, equivalentA);
    verifyOldStateCleared();
    QCOMPARE(dlg.session().sourceCoordinateProvenance.sourceImageId, equivalentA.sourceImageId);
    QCOMPARE(dlg.session().imageDigest, AnnotationSession::computeImageDigest(changedImage));

    mainView.setImage(image, equivalentA);
    prepareOldState();
    ImageCoordinateProvenance changedSize = equivalentA;
    changedSize.sourceSize = QSize(400, 300);
    mainView.setImage(image, changedSize);
    verifyOldStatePreserved();
    QCOMPARE(dlg.session().sourceCoordinateProvenance.sourceSize, QSize(400, 300));

    ImageCoordinateProvenance changedTransform = changedSize;
    QVERIFY(changedTransform.composeLocalToSource({{1.0, 0.0, 10.0, 0.0, 1.0, 5.0, 0.0, 0.0, 1.0}}));
    mainView.setImage(image, changedTransform);
    verifyOldStatePreserved();
    QCOMPARE(dlg.session().sourceCoordinateProvenance.localOriginInSource, QPointF(60.0, 45.0));
    QJsonObject remappedObjects = dlg.listAgentAnnotationObjects(0, 10);
    QJsonObject remappedSourceBbox = remappedObjects.value(QStringLiteral("objects"))
                                         .toArray()
                                         .first()
                                         .toObject()
                                         .value(QStringLiteral("sourceBbox"))
                                         .toObject();
    QCOMPARE(remappedSourceBbox.value(QStringLiteral("x")).toDouble(), 100.0);
    QCOMPARE(remappedSourceBbox.value(QStringLiteral("y")).toDouble(), 75.0);

    ImageCoordinateProvenance changedId = changedTransform;
    changedId.sourceImageId = QStringLiteral("slice165-source-B");
    mainView.setImage(image, changedId);
    verifyOldStatePreserved();
    QCOMPARE(dlg.session().sourceCoordinateProvenance.sourceImageId, changedId.sourceImageId);
    remappedObjects = dlg.listAgentAnnotationObjects(0, 10);
    remappedSourceBbox = remappedObjects.value(QStringLiteral("objects"))
                             .toArray()
                             .first()
                             .toObject()
                             .value(QStringLiteral("sourceBbox"))
                             .toObject();
    QCOMPARE(remappedObjects.value(QStringLiteral("objects"))
                 .toArray()
                 .first()
                 .toObject()
                 .value(QStringLiteral("sourceImageId"))
                 .toString(),
             changedId.sourceImageId);
    QCOMPARE(remappedSourceBbox.value(QStringLiteral("x")).toDouble(), 100.0);
    QCOMPARE(remappedSourceBbox.value(QStringLiteral("y")).toDouble(), 75.0);

    mainView.actualSize();
    mainView.setZoom(2.0);
    QTest::mousePress(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(10, 10));
    QMouseEvent newPanMove(QEvent::MouseMove, QPointF(25, 5), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &newPanMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(25, 5));
    dlg.positivePointButton()->click();
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(255, 180));
    expectedPrompts.append(QPointF(90.0, 70.0));
    QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), expectedPrompts);
    expectPreview = false;
    expectedPromptSnapshot = dlg.agentPromptStateSnapshot();
    expectedMask = dlg.overlayWidget()->objectMask(oldObjectId);
    expectedUndoCount = dlg.undoStackCount();
    expectedUndoIndex = dlg.undoStackIndex();
    const auto sourcePointB = mainView.coordinateProvenance().mapLocalToSource(QPointF(90.0, 70.0));
    QVERIFY(sourcePointB.has_value());
    QCOMPARE(sourcePointB->x(), 150.0);
    QCOMPARE(sourcePointB->y(), 115.0);

    mainView.setImage(image, ImageCoordinateProvenance());
    verifyOldStatePreserved();
    QVERIFY(!dlg.session().sourceCoordinateProvenance.isValid());
    const QJsonObject invalidSourceObjects = dlg.listAgentAnnotationObjects(0, 10);
    QVERIFY(!invalidSourceObjects.value(QStringLiteral("sourceCoordinateAvailable")).toBool());

    mainView.setImage(image, provenanceA);
    verifyOldStatePreserved();
    QCOMPARE(dlg.session().sourceCoordinateProvenance.sourceImageId, provenanceA.sourceImageId);

    mainView.setImage(changedImage, provenanceA);
    verifyOldStateCleared();
    dlg.undoButton()->click();
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 0);

    prepareOldState();
    mainView.setImage(image, changedId);
    verifyOldStateCleared();
    QCOMPARE(dlg.session().sourceCoordinateProvenance.sourceImageId, changedId.sourceImageId);
    QCOMPARE(dlg.session().imageDigest, AnnotationSession::computeImageDigest(image));
}

void TestSamAnnotatorDialog::sharedViewMousePromptsCancelWithoutLosingConfirmedAnnotation() {
    HImageWidget mainView;
    mainView.resize(320, 240);
    mainView.setFixedSize(320, 240);

    const QImage image = makeTestImage();
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("slice166-parent");
    provenance.sourceSize = QSize(320, 240);
    provenance.localOriginInSource = QPointF(50.0, 40.0);
    QVERIFY(provenance.isValid());
    mainView.setImage(image, provenance);

    SamAnnotatorDialog dlg(nullptr, true);
    dlg.attachToImageWidgetForEnvironmentSetup(&mainView);
    mainView.show();
    QCoreApplication::processEvents();

    const QString objectId = confirmObject(dlg, squarePolygon(40, 30, 20), QRectF(40, 30, 20, 20),
                                           makeDisplayMask(200, 150, {QRect(40, 30, 20, 20)}));
    QVERIFY(!objectId.isEmpty());
    const QImage confirmedMask = dlg.overlayWidget()->objectMask(objectId);
    QVERIFY(!confirmedMask.isNull());
    QVERIFY(dlg.undoStackCount() > 0);

    mainView.actualSize();
    mainView.setZoom(2.0);
    QTest::mousePress(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(10, 10));
    QMouseEvent panMove(QEvent::MouseMove, QPointF(25, 5), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &panMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::MiddleButton, Qt::NoModifier, QPoint(25, 5));

    dlg.positivePointButton()->click();
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(235, 160));
    QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), QList<QPointF>{QPointF(80.0, 60.0)});
    const auto positiveSource = provenance.mapLocalToSource(QPointF(80.0, 60.0));
    QVERIFY(positiveSource.has_value());
    QCOMPARE(*positiveSource, QPointF(130.0, 100.0));

    dlg.negativePointButton()->click();
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(215, 140));
    QCOMPARE(dlg.overlayWidget()->negativePromptPoints(), QList<QPointF>{QPointF(70.0, 50.0)});
    const auto negativeSource = provenance.mapLocalToSource(QPointF(70.0, 50.0));
    QVERIFY(negativeSource.has_value());
    QCOMPARE(*negativeSource, QPointF(120.0, 90.0));

    dlg.boxButton()->click();
    QSignalSpy dragSpy(dlg.overlayWidget(), &AnnotationOverlayWidget::dragEnded);
    QTest::mousePress(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(155, 100));
    QMouseEvent boxMove(QEvent::MouseMove, QPointF(275, 220), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(dlg.overlayWidget(), &boxMove);
    QTest::mouseRelease(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(275, 220));
    QCOMPARE(dragSpy.size(), 1);
    const QList<QVariant> dragCoordinates = dragSpy.takeFirst();
    const QPointF boxStart = dragCoordinates.at(0).toPointF();
    const QPointF boxEnd = dragCoordinates.at(1).toPointF();
    QCOMPARE(boxStart, QPointF(40.0, 30.0));
    QCOMPARE(boxEnd, QPointF(100.0, 90.0));
    const auto sourceBox = provenance.mapRectToSourceBounds(QRectF(boxStart, boxEnd).normalized());
    QVERIFY(sourceBox.has_value());
    QCOMPARE(*sourceBox, QRectF(90.0, 70.0, 60.0, 60.0));

    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection,
                                      Q_ARG(QList<QPointF>, squarePolygon(40, 30, 20)),
                                      Q_ARG(QRectF, QRectF(40, 30, 20, 20)), Q_ARG(double, 0.9),
                                      Q_ARG(QString, QStringLiteral("slice166-preview")), Q_ARG(QImage, QImage())));
    QVERIFY(!dlg.overlayWidget()->previewPolygon().isEmpty());
    QCOMPARE(dlg.session().annotations.size(), 1);
    const int undoCountBeforeCancel = dlg.undoStackCount();
    const int undoIndexBeforeCancel = dlg.undoStackIndex();

    QCOMPARE(dlg.cancelAgentPrompt().value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QVERIFY(dlg.overlayWidget()->positivePromptPoints().isEmpty());
    QVERIFY(dlg.overlayWidget()->negativePromptPoints().isEmpty());
    QVERIFY(dlg.overlayWidget()->previewPolygon().isEmpty());
    QCOMPARE(dlg.previewCandidateCount(), 0);
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("predictionInFlight")).toBool());
    QCOMPARE(dlg.cancelAgentPrompt().value(QStringLiteral("status")).toString(), QStringLiteral("unchanged"));

    QCOMPARE(dlg.session().annotations.size(), 1);
    QCOMPARE(dlg.session().annotations.first().id, objectId);
    QCOMPARE(dlg.overlayWidget()->objectMask(objectId), confirmedMask);
    QCOMPARE(dlg.undoStackCount(), undoCountBeforeCancel);
    QCOMPARE(dlg.undoStackIndex(), undoIndexBeforeCancel);

    dlg.positivePointButton()->click();
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, QPoint(255, 180));
    QCOMPARE(dlg.overlayWidget()->positivePromptPoints(), QList<QPointF>{QPointF(90.0, 70.0)});
    const auto postCancelSource = mainView.coordinateProvenance().mapLocalToSource(QPointF(90.0, 70.0));
    QVERIFY(postCancelSource.has_value());
    QCOMPARE(*postCancelSource, QPointF(140.0, 110.0));
    QCOMPARE(dlg.session().annotations.size(), 1);
    QCOMPARE(dlg.session().annotations.first().id, objectId);
}

void TestSamAnnotatorDialog::sessionSaveAndReopenPreserveVerifiedSourceCoordinates() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("lineage.png"));
    const QImage image = makeTestImage();
    QVERIFY(image.save(imagePath, "PNG"));

    SamAnnotatorDialog sourceDialog;
    sourceDialog.setImageSnapshot(image, imagePath);
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("crop-calibrated-parent");
    provenance.sourceSize = QSize(800, 600);
    QVERIFY(provenance.composeLocalToSource(
        ImageCoordinateProvenance::Matrix3{{2.0, 0.0, 10.0, 0.0, 3.0, 5.0, 0.0, 0.0, 1.0}}));
    sourceDialog.imageWidget()->setImage(image, provenance);
    QVERIFY(!confirmObject(sourceDialog, squarePolygon(20, 30, 10), QRectF(20, 30, 10, 10),
                           makeDisplayMask(image.width(), image.height(), {QRect(20, 30, 10, 10)}))
                 .isEmpty());

    const QString sessionPath = dir.filePath(QStringLiteral("lineage.deeplux-anno.json"));
    QString error;
    QVERIFY2(sourceDialog.saveAnnotationFile(sessionPath, &error), qPrintable(error));
    const AnnotationSession saved = AnnotationSession::load(sessionPath, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(saved.sourceCoordinateProvenance.isValid());

    SamAnnotatorDialog reopened;
    QVERIFY2(reopened.openAnnotationFile(sessionPath, &error), qPrintable(error));
    const QJsonObject item =
        reopened.listAgentAnnotationObjects(0, 1).value(QStringLiteral("objects")).toArray().first().toObject();
    QCOMPARE(item.value(QStringLiteral("bbox")).toObject().value(QStringLiteral("x")).toDouble(), 20.0);
    QCOMPARE(item.value(QStringLiteral("sourceImageId")).toString(), QStringLiteral("crop-calibrated-parent"));
    QCOMPARE(item.value(QStringLiteral("sourceImageSize")).toObject().value(QStringLiteral("width")).toInt(), 800);
    const QJsonObject sourceBbox = item.value(QStringLiteral("sourceBbox")).toObject();
    QCOMPARE(sourceBbox.value(QStringLiteral("x")).toDouble(), 50.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("y")).toDouble(), 95.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("width")).toDouble(), 20.0);
    QCOMPARE(sourceBbox.value(QStringLiteral("height")).toDouble(), 30.0);

    QJsonObject legacy = saved.toJson();
    legacy.remove(QStringLiteral("sourceCoordinates"));
    const QString legacyPath = dir.filePath(QStringLiteral("legacy.deeplux-anno.json"));
    QFile legacyFile(legacyPath);
    QVERIFY(legacyFile.open(QIODevice::WriteOnly));
    QVERIFY(legacyFile.write(QJsonDocument(legacy).toJson()) > 0);
    legacyFile.close();
    SamAnnotatorDialog legacyDialog;
    QVERIFY2(legacyDialog.openAnnotationFile(legacyPath, &error), qPrintable(error));
    const QJsonObject legacyItem =
        legacyDialog.listAgentAnnotationObjects(0, 1).value(QStringLiteral("objects")).toArray().first().toObject();
    QVERIFY(!legacyItem.contains(QStringLiteral("sourceBbox")));
    QVERIFY(!legacyItem.contains(QStringLiteral("sourceImageId")));

    legacy.insert(QStringLiteral("imageDigest"), QString());
    legacy.insert(QStringLiteral("sourceCoordinates"), saved.toJson().value(QStringLiteral("sourceCoordinates")));
    const QString noDigestPath = dir.filePath(QStringLiteral("no-digest.deeplux-anno.json"));
    QFile noDigestFile(noDigestPath);
    QVERIFY(noDigestFile.open(QIODevice::WriteOnly));
    QVERIFY(noDigestFile.write(QJsonDocument(legacy).toJson()) > 0);
    noDigestFile.close();
    SamAnnotatorDialog noDigestDialog;
    QVERIFY2(noDigestDialog.openAnnotationFile(noDigestPath, &error), qPrintable(error));
    const QJsonObject noDigestItem =
        noDigestDialog.listAgentAnnotationObjects(0, 1).value(QStringLiteral("objects")).toArray().first().toObject();
    QVERIFY(!noDigestItem.contains(QStringLiteral("sourceBbox")));
    QVERIFY(!noDigestItem.contains(QStringLiteral("sourceImageId")));

    QJsonObject horizonSession = saved.toJson();
    QJsonObject horizonCoordinates = horizonSession.value(QStringLiteral("sourceCoordinates")).toObject();
    horizonCoordinates.insert(QStringLiteral("localToSource"),
                              QJsonArray{2.0, 0.0, 10.0, 0.0, 3.0, 5.0, 0.0, 0.1, -3.5});
    horizonSession.insert(QStringLiteral("sourceCoordinates"), horizonCoordinates);
    const QString horizonPath = dir.filePath(QStringLiteral("horizon.deeplux-anno.json"));
    QFile horizonFile(horizonPath);
    QVERIFY(horizonFile.open(QIODevice::WriteOnly));
    QVERIFY(horizonFile.write(QJsonDocument(horizonSession).toJson()) > 0);
    horizonFile.close();
    SamAnnotatorDialog horizonDialog;
    QVERIFY2(horizonDialog.openAnnotationFile(horizonPath, &error), qPrintable(error));
    const QJsonObject horizonItem =
        horizonDialog.listAgentAnnotationObjects(0, 1).value(QStringLiteral("objects")).toArray().first().toObject();
    QCOMPARE(horizonItem.value(QStringLiteral("bbox")).toObject().value(QStringLiteral("x")).toDouble(), 20.0);
    QVERIFY(!horizonItem.contains(QStringLiteral("sourceBbox")));
}

void TestSamAnnotatorDialog::agentMaskToolsRejectMissingImageAndUnavailableMasks() {
    const QJsonObject editParams{{QStringLiteral("operation"), QStringLiteral("dilate")},
                                 {QStringLiteral("objectIds"), QJsonArray{QStringLiteral("object-1")}},
                                 {QStringLiteral("radiusPx"), 1}};
    SamAnnotatorDialog noImage;
    QCOMPARE(noImage.listAgentAnnotationObjects(0, 20).value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("NO_IMAGE"));
    QCOMPARE(noImage.applyAgentMaskEdit(editParams).value(QStringLiteral("errorCode")).toString(),
             QStringLiteral("NO_IMAGE"));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("bound.png"));
    QVERIFY(makeTestImage().save(imagePath, "PNG"));
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), imagePath);
    QCOMPARE(dlg.listAgentAnnotationObjects(0, 20).value(QStringLiteral("total")).toInt(), 0);
    AnnotationSession invalid = dlg.session();
    AnnotationObject object;
    object.id = QStringLiteral("no-mask-object");
    object.label = QStringLiteral("no-mask");
    object.maskRle = QStringLiteral("invalid-rle");
    invalid.annotations.append(object);
    const QString sessionPath = directory.filePath(QStringLiteral("invalid-mask.deeplux-anno.json"));
    QString error;
    QVERIFY2(invalid.save(sessionPath, &error), qPrintable(error));
    QVERIFY2(dlg.openAnnotationFile(sessionPath, &error), qPrintable(error));
    const bool undoEnabledBeforeRejectedEdit = dlg.undoButton()->isEnabled();
    QJsonObject invalidEditParams = editParams;
    invalidEditParams.insert(QStringLiteral("objectIds"), QJsonArray{object.id});
    const QJsonObject result = dlg.applyAgentMaskEdit(invalidEditParams);
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("ANNOTATION_MASK_UNAVAILABLE"));
    QCOMPARE(dlg.session().annotations.size(), 1);
    QCOMPARE(dlg.session().annotations.first().id, object.id);
    QCOMPARE(dlg.undoButton()->isEnabled(), undoEnabledBeforeRejectedEdit);
}

void TestSamAnnotatorDialog::agentCategoryEditsAndObjectRelabelingPreserveSelectionAndUndo() {
    SamAnnotatorDialog withoutImage;
    QCOMPARE(withoutImage.listAgentAnnotationCategories(0, 20).value(QStringLiteral("status")).toString(),
             QStringLiteral("ok"));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("image.png"));
    const QImage image = makeTestImage();
    QVERIFY(image.save(imagePath, "PNG"));

    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(image, imagePath);
    AnnotationSession session = dlg.session();
    session.imagePath = imagePath;
    session.imageWidth = image.width();
    session.imageHeight = image.height();
    session.imageDigest = AnnotationSession::computeImageDigest(image);
    AnnotationObject first;
    first.id = QStringLiteral("category-object-1");
    first.label = QStringLiteral("缺陷");
    first.bbox = QRectF(4, 5, 8, 9);
    first.score = 0.75;
    first.maskRle = QStringLiteral("first-rle");
    first.prompts.pointsPos = {QPointF(7, 8)};
    AnnotationObject second = first;
    second.id = QStringLiteral("category-object-2");
    second.label = QStringLiteral("划痕");
    second.score = 0.9;
    second.maskRle = QStringLiteral("second-rle");
    session.annotations = {first, second};
    const QString sessionPath = directory.filePath(QStringLiteral("categories.deeplux-anno.json"));
    QString error;
    QVERIFY2(session.save(sessionPath, &error), qPrintable(error));
    QVERIFY2(dlg.openAnnotationFile(sessionPath, &error), qPrintable(error));

    const QJsonObject listed = dlg.listAgentAnnotationCategories(0, 1);
    QCOMPARE(listed.value(QStringLiteral("status")).toString(), QStringLiteral("ok"));
    QCOMPARE(listed.value(QStringLiteral("categories")).toArray().size(), dlg.categories().size());
    QCOMPARE(listed.value(QStringLiteral("total")).toInt(), 2);
    QCOMPARE(listed.value(QStringLiteral("hasMore")).toBool(), true);
    QCOMPARE(listed.value(QStringLiteral("nextOffset")).toInt(), 1);
    QVERIFY(!QJsonDocument(listed).toJson().contains("maskRle"));

    const QJsonObject added =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("add_category")},
                                               {QStringLiteral("category"), QStringLiteral("  新类别  ")}});
    QCOMPARE(added.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
    QVERIFY(dlg.categories().contains(QStringLiteral("新类别")));
    const QJsonObject duplicate =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("add_category")},
                                               {QStringLiteral("category"), QStringLiteral("新类别")}});
    QCOMPARE(duplicate.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CATEGORY_ALREADY_EXISTS"));
    const QJsonObject tooLong =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("add_category")},
                                               {QStringLiteral("category"), QString(43, QChar(0x754c))}});
    QCOMPARE(tooLong.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject utf8Boundary =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("add_category")},
                                               {QStringLiteral("category"), QString(42, QChar(0x754c))}});
    QCOMPARE(utf8Boundary.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));

    dlg.objectList()->setCurrentRow(1);
    dlg.objectList()->selectionModel()->select(dlg.objectList()->model()->index(0, 0), QItemSelectionModel::Select);
    const QJsonObject changed =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("set_object_label")},
                                               {QStringLiteral("objectIds"), QJsonArray{first.id, second.id}},
                                               {QStringLiteral("label"), QStringLiteral("  缺陷  ")}});
    QCOMPARE(changed.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
    QCOMPARE(changed.value(QStringLiteral("affectedObjectIds")).toArray().size(), 1);
    QCOMPARE(changed.value(QStringLiteral("affectedObjectIds")).toArray().first().toString(), second.id);
    QCOMPARE(dlg.session().annotations.at(0).label, QStringLiteral("缺陷"));
    QCOMPARE(dlg.session().annotations.at(1).label, QStringLiteral("缺陷"));
    QCOMPARE(dlg.session().annotations.at(1).score, second.score);
    QCOMPARE(dlg.session().annotations.at(1).maskRle, second.maskRle);
    QCOMPARE(dlg.session().annotations.at(1).prompts.pointsPos, second.prompts.pointsPos);
    QCOMPARE(dlg.objectList()->selectedItems().size(), 2);
    QCOMPARE(dlg.objectList()->currentItem()->data(Qt::UserRole).toString(), second.id);

    const int undoIndexBeforeUnchanged = dlg.undoStackIndex();
    const bool undoAvailable = dlg.undoButton()->isEnabled();
    const QJsonObject unchanged =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("set_object_label")},
                                               {QStringLiteral("objectIds"), QJsonArray{first.id, second.id}},
                                               {QStringLiteral("label"), QStringLiteral("缺陷")}});
    QCOMPARE(unchanged.value(QStringLiteral("status")).toString(), QStringLiteral("unchanged"));
    QCOMPARE(dlg.undoStackIndex(), undoIndexBeforeUnchanged);
    QCOMPARE(dlg.undoButton()->isEnabled(), undoAvailable);

    dlg.undoButton()->click();
    QCOMPARE(dlg.session().annotations.at(1).label, second.label);
    QCOMPARE(dlg.objectList()->selectedItems().size(), 2);
    QCOMPARE(dlg.objectList()->currentItem()->data(Qt::UserRole).toString(), second.id);
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.at(1).label, QStringLiteral("缺陷"));
    QCOMPARE(dlg.objectList()->selectedItems().size(), 2);
    QCOMPARE(dlg.objectList()->currentItem()->data(Qt::UserRole).toString(), second.id);

    const QJsonObject removed =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("remove_category")},
                                               {QStringLiteral("category"), QStringLiteral(" 缺陷 ")}});
    QCOMPARE(removed.value(QStringLiteral("status")).toString(), QStringLiteral("updated"));
    QCOMPARE(dlg.session().annotations.first().label, QStringLiteral("缺陷"));
    const QJsonObject missing =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("remove_category")},
                                               {QStringLiteral("category"), QStringLiteral("缺陷")}});
    QCOMPARE(missing.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CATEGORY_NOT_FOUND"));
    const QJsonObject invalidLabel =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("set_object_label")},
                                               {QStringLiteral("objectIds"), QJsonArray{first.id}},
                                               {QStringLiteral("label"), QStringLiteral("not a category")}});
    QCOMPARE(invalidLabel.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CATEGORY_NOT_FOUND"));
    const QJsonObject duplicateIds =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("set_object_label")},
                                               {QStringLiteral("objectIds"), QJsonArray{first.id, first.id}},
                                               {QStringLiteral("label"), QStringLiteral("划痕")}});
    QCOMPARE(duplicateIds.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    for (int index = 0; index < 251; ++index) {
        QCOMPARE(dlg.applyAgentCategoryEdit(
                        QJsonObject{{QStringLiteral("operation"), QStringLiteral("add_category")},
                                    {QStringLiteral("category"), QStringLiteral("category-%1").arg(index)}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("updated"));
    }
    const QJsonObject categoryLimit =
        dlg.applyAgentCategoryEdit(QJsonObject{{QStringLiteral("operation"), QStringLiteral("add_category")},
                                               {QStringLiteral("category"), QStringLiteral("category-over-limit")}});
    QCOMPARE(categoryLimit.value(QStringLiteral("errorCode")).toString(), QStringLiteral("CATEGORY_LIMIT_EXCEEDED"));
}

void TestSamAnnotatorDialog::agentSplitMemoryBudgetRejectedBeforeMaterialization() {
    QImage image(2000, 1500, QImage::Format_RGB32);
    image.fill(Qt::black);
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(image, QString());
    QList<QRect> dots;
    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 5; ++j)
            dots.append(QRect(10 + i * 40, 10 + j * 40, 2, 2));
    }
    const QString id =
        confirmObject(dlg, squarePolygon(10, 10, 20), QRectF(10, 10, 500, 200), makeDisplayMask(2000, 1500, dots));
    QVERIFY(!id.isEmpty());
    const AnnotationSession before = dlg.session();
    const QJsonObject result = dlg.applyAgentMaskEdit(QJsonObject{
        {QStringLiteral("operation"), QStringLiteral("split")}, {QStringLiteral("objectIds"), QJsonArray{id}}});
    QCOMPARE(result.value(QStringLiteral("errorCode")).toString(), QStringLiteral("MASK_RESOURCE_LIMIT_EXCEEDED"));
    QCOMPARE(dlg.session().annotations.size(), before.annotations.size());
    QCOMPARE(dlg.session().annotations.first().id, before.annotations.first().id);
    QCOMPARE(dlg.session().annotations.first().maskRle, before.annotations.first().maskRle);
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

    // 会话 B 文件（2 个对象）——批3 复核四轮（P1-1）：非空会话必须绑定可读
    // 图像才能被打开，srcDlg 需绑定真实存在的图像文件
    QTemporaryDir srcDir;
    QVERIFY(srcDir.isValid());
    const QString srcImagePath = srcDir.filePath(QStringLiteral("b.png"));
    QVERIFY(makeTestImage().save(srcImagePath, "PNG"));
    SamAnnotatorDialog srcDlg;
    srcDlg.setImageSnapshot(makeTestImage(), srcImagePath);
    QVERIFY(!confirmObject(srcDlg, squarePolygon(10, 10, 10), QRectF(10, 10, 10, 10), QImage()).isEmpty());
    QVERIFY(!confirmObject(srcDlg, squarePolygon(100, 100, 10), QRectF(100, 100, 10, 10), QImage()).isEmpty());
    // Windows: QSaveFile 无法原子替换一个仍由 QTemporaryFile 管理/占用的
    // 路径（commit 报 Access denied），使用 QTemporaryDir 下的新路径。
    QTemporaryDir sessionDir;
    QVERIFY(sessionDir.isValid());
    const QString sessionPath = sessionDir.filePath(QStringLiteral("undo_open.deeplux-anno.json"));
    QString saveErr;
    QVERIFY2(srcDlg.session().save(sessionPath, &saveErr), qPrintable(saveErr));

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

// 批3 复核三轮（P1-1）：GUI 摘要链路闭合——新图会话内存即携带摘要、保存写入
// 文件；同路径换图后打开被拒且对话框状态不被污染；恢复原图后打开成功、
// 再保存摘要稳定
void TestSamAnnotatorDialog::saveWritesDigestAndOpenVerifiesIt() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("img.png"));
    QImage img(200, 150, QImage::Format_RGB32);
    img.fill(QColor("#22C55E"));
    QVERIFY(img.save(imagePath, "PNG"));

    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(img, imagePath);
    QVERIFY(!confirmObject(dlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20),
                           makeDisplayMask(200, 150, {QRect(50, 40, 20, 20)}))
                 .isEmpty());
    // 新图绑定路径内存会话即带摘要
    QCOMPARE(dlg.session().imageDigest, AnnotationSession::computeImageDigest(img));

    const QString sessionPath = dir.filePath(QStringLiteral("s.deeplux-anno.json"));
    QVERIFY(dlg.saveAnnotationFile(sessionPath));
    QString err;
    const AnnotationSession saved = AnnotationSession::load(sessionPath, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(saved.imageDigest, AnnotationSession::computeImageDigest(img));

    // 同路径换同尺寸异内容图 → 打开被拒（先校验后换态），对话框状态不被污染
    QImage img2(200, 150, QImage::Format_RGB32);
    img2.fill(QColor("#3B82F6"));
    QVERIFY(img2.save(imagePath, "PNG"));
    SamAnnotatorDialog dlg2;
    QString openErr;
    QVERIFY(!dlg2.openAnnotationFile(sessionPath, &openErr));
    QVERIFY2(openErr.contains(QStringLiteral("摘要")), qPrintable(openErr));
    QCOMPARE(dlg2.session().annotations.size(), 0);

    // 恢复原图 → 打开成功，摘要原样保留
    QVERIFY(img.save(imagePath, "PNG"));
    SamAnnotatorDialog dlg3;
    QVERIFY(dlg3.openAnnotationFile(sessionPath));
    QCOMPARE(dlg3.session().annotations.size(), 1);
    QCOMPARE(dlg3.session().imageDigest, AnnotationSession::computeImageDigest(img));
    // 再保存：摘要稳定（内存图像重算一致）
    const QString sessionPath2 = dir.filePath(QStringLiteral("s2.deeplux-anno.json"));
    QVERIFY(dlg3.saveAnnotationFile(sessionPath2));
    const AnnotationSession resaved = AnnotationSession::load(sessionPath2, &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(resaved.imageDigest, AnnotationSession::computeImageDigest(img));
}

// 批3 复核三轮（P1-1）：legacy 无摘要会话——打开放行但显式警告且不静默补摘要；
// 用户核对后走保存路径，由保存以内存图像补记
void TestSamAnnotatorDialog::legacySessionOpensWithWarningAndBackfillsOnSave() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("legacy_img.png"));
    QImage img(200, 150, QImage::Format_RGB32);
    img.fill(QColor("#22C55E"));
    QVERIFY(img.save(imagePath, "PNG"));

    const QString sessionPath = dir.filePath(QStringLiteral("legacy.deeplux-anno.json"));
    {
        const QJsonObject obj{
            {"id", QStringLiteral("legacy1")},
            {"label", QStringLiteral("old")},
            {"bbox", QJsonArray{50, 40, 20, 20}},
            {"polygon", QJsonArray{QJsonArray{50, 40}, QJsonArray{70, 40}, QJsonArray{70, 60}, QJsonArray{50, 60}}},
            {"mask_rle", QString()},
            {"score", 0.5},
            {"model_name", QStringLiteral("sam")},
            {"prompts", QJsonObject{{"points_pos", QJsonArray{}}, {"points_neg", QJsonArray{}}}}};
        const QJsonObject session{{"imagePath", imagePath},
                                  {"imageWidth", 200},
                                  {"imageHeight", 150},
                                  {"modelName", QStringLiteral("sam")},
                                  {"annotations", QJsonArray{obj}}};
        QFile f(sessionPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(session).toJson());
        f.close();
    }

    SamAnnotatorDialog dlg;
    QVERIFY(dlg.openAnnotationFile(sessionPath));
    QCOMPARE(dlg.session().annotations.size(), 1);
    QVERIFY2(dlg.statusLabel()->text().contains(QStringLiteral("警告")), qPrintable(dlg.statusLabel()->text()));
    QCOMPARE(dlg.session().imageDigest, QString()); // 打开不静默补摘要

    // 用户确认路径：保存 → 以内存图像补记摘要（imagePath 一致守卫通过）
    const QString outPath = dir.filePath(QStringLiteral("backfilled.deeplux-anno.json"));
    QVERIFY(dlg.saveAnnotationFile(outPath));
    QString err;
    const AnnotationSession saved = AnnotationSession::load(outPath, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(saved.imageDigest, AnnotationSession::computeImageDigest(img));
    QCOMPARE(saved.annotations.size(), 1);
}

// 批3 复核三轮（P1-3）：分量数合规（60 ≤ 64）但 分量数×整图字节 超物化预算
// → 拆分失败关闭，不构造任何分量图像
void TestSamAnnotatorDialog::splitMemoryBudgetRejected() {
    QImage big(2000, 1500, QImage::Format_RGB32);
    big.fill(Qt::black);
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(big, QStringLiteral("/tmp/deeplux_split_budget.png"));
    QList<QRect> dots;
    for (int i = 0; i < 12; ++i)
        for (int j = 0; j < 5; ++j)
            dots.append(QRect(10 + i * 40, 10 + j * 40, 2, 2)); // 60 个孤立小块
    QVERIFY(!confirmObject(dlg, squarePolygon(10, 10, 20), QRectF(10, 10, 500, 200), makeDisplayMask(2000, 1500, dots))
                 .isEmpty());
    dlg.objectList()->setCurrentRow(0);
    dlg.splitButton()->click();
    // 60 × 2000×1500 × 5B ≈ 858MB > 256MB 预算
    QVERIFY2(dlg.statusLabel()->text().contains(QStringLiteral("内存")), qPrintable(dlg.statusLabel()->text()));
    QCOMPARE(dlg.session().annotations.size(), 1); // 失败关闭，未拆分
}

// 批3 复核四轮（P1-1）：绑定图像被删除后打开非空会话必须拒绝——
// 否则新会话标注被显示、编辑在陈旧的内存图像上（静默错位）
void TestSamAnnotatorDialog::openRejectsSessionWithUnreadableImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("bound.png"));
    QVERIFY(makeTestImage().save(imagePath, "PNG"));

    SamAnnotatorDialog srcDlg;
    srcDlg.setImageSnapshot(makeTestImage(), imagePath);
    QVERIFY(!confirmObject(srcDlg, squarePolygon(50, 40, 20), QRectF(50, 40, 20, 20), QImage()).isEmpty());
    const QString sessionPath = dir.filePath(QStringLiteral("bound.deeplux-anno.json"));
    QVERIFY(srcDlg.saveAnnotationFile(sessionPath));

    QVERIFY(QFile::remove(imagePath)); // 绑定图像不可读

    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), dir.filePath(QStringLiteral("unrelated.png")));
    const QString currentId = confirmObject(dlg, squarePolygon(10, 10, 10), QRectF(10, 10, 10, 10), QImage());
    QVERIFY(!currentId.isEmpty());

    QString err;
    QVERIFY(!dlg.openAnnotationFile(sessionPath, &err));
    QVERIFY2(err.contains(QStringLiteral("不可读")), qPrintable(err));
    // 当前会话零污染
    QCOMPARE(dlg.session().annotations.size(), 1);
    QCOMPARE(dlg.session().annotations.first().id, currentId);
}

// 批3 复核四轮（P1-1）：legacy 无摘要会话至少先核对尺寸——尺寸不符拒绝打开
void TestSamAnnotatorDialog::openRejectsLegacySessionDimsMismatch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("legacy.png"));
    QVERIFY(makeTestImage().save(imagePath, "PNG")); // 200x150

    const QString sessionPath = dir.filePath(QStringLiteral("legacy_dims.deeplux-anno.json"));
    {
        const QJsonObject obj{
            {"id", QStringLiteral("l1")},
            {"label", QStringLiteral("old")},
            {"bbox", QJsonArray{50, 40, 20, 20}},
            {"polygon", QJsonArray{QJsonArray{50, 40}, QJsonArray{70, 40}, QJsonArray{70, 60}, QJsonArray{50, 60}}},
            {"mask_rle", QString()},
            {"score", 0.5},
            {"model_name", QStringLiteral("sam")},
            {"prompts", QJsonObject{{"points_pos", QJsonArray{}}, {"points_neg", QJsonArray{}}}}};
        const QJsonObject session{{"imagePath", imagePath},
                                  {"imageWidth", 300}, // 与真实图像 200x150 不符
                                  {"imageHeight", 200},
                                  {"modelName", QStringLiteral("sam")},
                                  {"annotations", QJsonArray{obj}}};
        QFile f(sessionPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(session).toJson());
        f.close();
    }

    SamAnnotatorDialog dlg;
    QString err;
    QVERIFY(!dlg.openAnnotationFile(sessionPath, &err));
    QVERIFY2(err.contains(QStringLiteral("尺寸")), qPrintable(err));
    QCOMPARE(dlg.session().annotations.size(), 0);
}

// 批3 复核五轮（P1-2）：绑定图像丢失的空会话同样拒绝——旧实现放行后窗口
// 保留之前的图像，后续标注可能经保存守卫之外的路径绑定到错误坐标系
void TestSamAnnotatorDialog::openRejectsEmptySessionWithLostImage() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString lostPath = dir.filePath(QStringLiteral("gone.png")); // 不存在
    const QString sessionPath = dir.filePath(QStringLiteral("empty_lost.deeplux-anno.json"));
    {
        const QJsonObject session{
            {"imagePath", lostPath}, {"imageWidth", 200}, {"imageHeight", 150}, {"annotations", QJsonArray{}}};
        QFile f(sessionPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(session).toJson());
        f.close();
    }

    SamAnnotatorDialog dlg;
    const QString windowImagePath = dir.filePath(QStringLiteral("window.png"));
    dlg.setImageSnapshot(makeTestImage(), windowImagePath);
    QString err;
    QVERIFY(!dlg.openAnnotationFile(sessionPath, &err));
    QVERIFY2(err.contains(QStringLiteral("不可读")), qPrintable(err));
    // 当前会话绑定未被污染
    QCOMPARE(dlg.session().imagePath, windowImagePath);

    // 未绑定草稿（无路径且无标注）→ 允许打开，保存路径负责绑定当前图像
    const QString draftPath = dir.filePath(QStringLiteral("draft.deeplux-anno.json"));
    {
        QFile f(draftPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"({"annotations":[]})");
        f.close();
    }
    QVERIFY(dlg.openAnnotationFile(draftPath));
    QCOMPARE(dlg.session().annotations.size(), 0);
    QVERIFY(dlg.session().imagePath.isEmpty());
}

// 批3 复核六轮（P1）：无摘要的非空 legacy 会话——宽高缺失或单边为 0 时
// 尺寸核对被跳过，打开后保存会以当前图像补记摘要、固化无从校验的绑定。
// 修复后：必须记录正数宽高，否则失败关闭；宽高齐备则走 legacy 警告路径
void TestSamAnnotatorDialog::openRejectsLegacySessionWithoutValidDims() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imagePath = dir.filePath(QStringLiteral("legacy.png"));
    QVERIFY(makeTestImage().save(imagePath, "PNG")); // 200x150

    auto writeLegacy = [&](const QString& name, const QJsonObject& extra) -> QString {
        const QJsonObject obj{
            {"id", QStringLiteral("l1")},
            {"label", QStringLiteral("old")},
            {"bbox", QJsonArray{50, 40, 20, 20}},
            {"polygon", QJsonArray{QJsonArray{50, 40}, QJsonArray{70, 40}, QJsonArray{70, 60}, QJsonArray{50, 60}}},
            {"mask_rle", QString()},
            {"score", 0.5},
            {"model_name", QStringLiteral("sam")},
            {"prompts", QJsonObject{{"points_pos", QJsonArray{}}, {"points_neg", QJsonArray{}}}}};
        QJsonObject session{{"imagePath", imagePath}, {"annotations", QJsonArray{obj}}};
        for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
            session.insert(it.key(), it.value());
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(QJsonDocument(session).toJson());
        f.close();
        return path;
    };

    SamAnnotatorDialog dlg;
    QString err;
    // (a) 宽高完全缺失 → 拒绝
    const QString noDims = writeLegacy(QStringLiteral("nodims.json"), QJsonObject{});
    QVERIFY(!noDims.isEmpty());
    QVERIFY(!dlg.openAnnotationFile(noDims, &err));
    QVERIFY2(err.contains(QStringLiteral("宽高")), qPrintable(err));
    QCOMPARE(dlg.session().annotations.size(), 0); // 状态零污染
    // (b) 单边为 0 → 拒绝
    err.clear();
    const QString zeroH =
        writeLegacy(QStringLiteral("zeroH.json"), QJsonObject{{"imageWidth", 200}, {"imageHeight", 0}});
    QVERIFY(!zeroH.isEmpty());
    QVERIFY(!dlg.openAnnotationFile(zeroH, &err));
    QVERIFY2(err.contains(QStringLiteral("宽高")), qPrintable(err));
    // 对照：宽高齐备且与图像一致 → legacy 警告路径放行
    const QString okDims =
        writeLegacy(QStringLiteral("okdims.json"), QJsonObject{{"imageWidth", 200}, {"imageHeight", 150}});
    QVERIFY(!okDims.isEmpty());
    QVERIFY(dlg.openAnnotationFile(okDims));
    QCOMPARE(dlg.session().annotations.size(), 1);
    QVERIFY(dlg.statusLabel()->text().contains(QStringLiteral("警告")));
    QCOMPARE(dlg.session().imageDigest, QString()); // 打开仍不静默补摘要
}

// ===== SAM 批4（C）：多掩膜候选循环 =====

void TestSamAnnotatorDialog::standaloneImageViewportRemainsVisible() {
    SamAnnotatorDialog dlg;
    dlg.resize(1280, 1050);
    dlg.show();
    dlg.setImageSnapshot(makeTestImage());
    QCoreApplication::processEvents();

    QVERIFY(dlg.imageWidget()->height() >= 200);
    QCOMPARE(dlg.overlayWidget()->size(), dlg.imageWidget()->size());

    dlg.resize(900, 600);
    QCoreApplication::processEvents();
    QVERIFY(dlg.imageWidget()->height() >= 180);
    QCOMPARE(dlg.overlayWidget()->size(), dlg.imageWidget()->size());
    auto* cancel = dlg.findChild<QPushButton*>(QStringLiteral("SamCancelButton"));
    QVERIFY(cancel);
    QVERIFY(cancel->isVisible());
    QVERIFY(cancel->mapTo(&dlg, cancel->rect().bottomRight()).y() < dlg.height());
}

void TestSamAnnotatorDialog::candidateCyclingSelectsAlternateRegion() {
    SamAnnotatorDialog dlg;
    dlg.setImageSnapshot(makeTestImage(), QString());
    dlg.positivePointButton()->setChecked(true);
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(20, 20)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));

    auto* row = dlg.findChild<QWidget*>(QStringLiteral("SamCandidateRow"));
    QVERIFY(row != nullptr);

    // 注入预测：客户端未连测试服务 → lastCandidates 为空 → 无候选态，行隐藏
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection,
                                      Q_ARG(QList<QPointF>, squarePolygon(10, 10, 10)),
                                      Q_ARG(QRectF, QRectF(10, 10, 10, 10)), Q_ARG(double, 0.97),
                                      Q_ARG(QString, QStringLiteral("best_rle")), Q_ARG(QImage, QImage())));
    QCOMPARE(dlg.previewCandidateCount(), 0);
    QVERIFY(row->isHidden());
    QCOMPARE(dlg.candidateLabel()->text(), QStringLiteral("候选 0/0"));

    // 注入两个候选（c0=best 区域 10,10；c1=备选区域 30,30）
    SamCandidate c0;
    c0.polygon = squarePolygon(10, 10, 10);
    c0.bbox = QRectF(10, 10, 10, 10);
    c0.score = 0.97;
    c0.maskRle = QStringLiteral("rle0");
    SamCandidate c1;
    c1.polygon = squarePolygon(30, 30, 10);
    c1.bbox = QRectF(30, 30, 10, 10);
    c1.score = 0.9;
    c1.maskRle = QStringLiteral("rle1");
    dlg.setPreviewCandidates({c0, c1});
    QCOMPARE(dlg.previewCandidateCount(), 2);
    QCOMPARE(dlg.previewCandidateIndex(), 0);
    QVERIFY(!row->isHidden()); // candidates>1 才显示循环行
    QCOMPARE(dlg.candidateLabel()->text(), QStringLiteral("候选 1/2 score=0.97"));

    // ▶ 切到备选候选；再 ▶ 循环回 0；◀ 循环到 1
    dlg.nextCandidateButton()->click();
    QCOMPARE(dlg.previewCandidateIndex(), 1);
    QCOMPARE(dlg.candidateLabel()->text(), QStringLiteral("候选 2/2 score=0.90"));
    dlg.nextCandidateButton()->click();
    QCOMPARE(dlg.previewCandidateIndex(), 0);
    dlg.prevCandidateButton()->click();
    QCOMPARE(dlg.previewCandidateIndex(), 1);

    // 确认：写入会话的是所选候选（30,30 区域 + rle1），而非顶层 best
    dlg.categoryCombo()->setEditText(QStringLiteral("defect"));
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection));
    const AnnotationSession session = dlg.session();
    QCOMPARE(session.annotations.size(), 1);
    const AnnotationObject& obj = session.annotations.first();
    QCOMPARE(obj.bbox, QRectF(30, 30, 10, 10));
    QVERIFY(qAbs(obj.score - 0.9) < 1e-9);
    QCOMPARE(obj.polygon.first(), QPointF(30, 30));
    QCOMPARE(obj.maskRle, QStringLiteral("rle1"));
    // 确认后候选随 prompt 清空、行恢复隐藏
    QCOMPARE(dlg.previewCandidateCount(), 0);
    QVERIFY(row->isHidden());

    // 单一候选：行隐藏（无循环意义），预览即该候选；◀/▶ 对 n<2 为 no-op
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(20, 20)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onPredictionReady", Qt::DirectConnection,
                                      Q_ARG(QList<QPointF>, squarePolygon(10, 10, 10)),
                                      Q_ARG(QRectF, QRectF(10, 10, 10, 10)), Q_ARG(double, 0.97),
                                      Q_ARG(QString, QStringLiteral("best_rle")), Q_ARG(QImage, QImage())));
    dlg.setPreviewCandidates({c1});
    QCOMPARE(dlg.previewCandidateCount(), 1);
    QVERIFY(row->isHidden());
    QCOMPARE(dlg.candidateLabel()->text(), QStringLiteral("候选 1/1 score=0.90"));
    dlg.nextCandidateButton()->click();
    QCOMPARE(dlg.previewCandidateIndex(), 0);

    // 取消：候选清空、行隐藏（状态零残留）
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onCancel", Qt::DirectConnection));
    QCOMPARE(dlg.previewCandidateCount(), 0);
    QVERIFY(row->isHidden());
    QCOMPARE(dlg.candidateLabel()->text(), QStringLiteral("候选 0/0"));
}

void TestSamAnnotatorDialog::agentPromptCancelPreservesUndoAndRejectsLatePrediction() {
    SamTestServer server;
    QVERIFY(server.listen());
    server.delayMs.insert(QStringLiteral("/predict"), 250);

    SamAnnotatorDialog dlg;
    const QImage image = makeTestImage();
    dlg.setImageSnapshot(image, QString());
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString imagePath = directory.filePath(QStringLiteral("agent-prompt.png"));
    QVERIFY(image.save(imagePath));

    const QString confirmedObjectId =
        confirmObject(dlg, squarePolygon(6, 7, 8), QRectF(6, 7, 8, 8), makeDisplayMask(200, 150, {QRect(6, 7, 8, 8)}));
    QVERIFY(!confirmedObjectId.isEmpty());
    dlg.objectList()->setCurrentRow(0);
    const QImage confirmedMaskBefore = dlg.overlayWidget()->objectMask(confirmedObjectId);
    QVERIFY(!confirmedMaskBefore.isNull());

    SamBackendClient* client = dlg.backendClient();
    QVERIFY(client != nullptr);
    client->setServerUrl(server.url());
    client->setTimeoutMs(3000);
    client->setImage(imagePath);
    QTRY_VERIFY_WITH_TIMEOUT(!client->currentEmbeddingId().isEmpty(), 3000);

    server.delayMs.insert(QStringLiteral("/set_image"), 300);
    dlg.positivePointButton()->setChecked(true);
    const auto clickPrompt = [&dlg](const QPointF& point, Qt::MouseButton button) {
        return QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, point),
                                         Q_ARG(Qt::MouseButton, button));
    };
    const auto dragPromptBox = [&dlg](const QPointF& start, const QPointF& end) {
        return QMetaObject::invokeMethod(&dlg, "onOverlayDragEnded", Qt::DirectConnection, Q_ARG(QPointF, start),
                                         Q_ARG(QPointF, end));
    };
    QVERIFY(clickPrompt(QPointF(20, 20), Qt::LeftButton));
    QVERIFY(clickPrompt(QPointF(30, 31), Qt::RightButton));
    QVERIFY(dragPromptBox(QPointF(40, 41), QPointF(50, 51)));
    QTRY_VERIFY_WITH_TIMEOUT(server.requestLog.count(QStringLiteral("POST /predict")) >= 1, 3000);
    const QJsonObject initialStatus = dlg.agentPromptStatus();
    QVERIFY(initialStatus.value(QStringLiteral("hasPrompt")).toBool());
    QVERIFY(initialStatus.value(QStringLiteral("predictionInFlight")).toBool());
    const QJsonObject firstRequestState = dlg.agentPromptStateSnapshot();
    const qint64 oldRequestId = firstRequestState.value(QStringLiteral("requestId")).toString().toLongLong();
    QVERIFY(oldRequestId > 0);
    const QString oldGeneration = firstRequestState.value(QStringLiteral("promptGeneration")).toString();
    const int stackIndex = dlg.undoStackIndex();
    const int stackCount = dlg.undoStackCount();

    QCOMPARE(dlg.cancelAgentPrompt().value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(dlg.undoStackIndex(), stackIndex);
    QCOMPARE(dlg.undoStackCount(), stackCount);
    QCOMPARE(dlg.session().annotations.size(), 1);
    QCOMPARE(dlg.session().annotations.first().id, confirmedObjectId);
    QCOMPARE(dlg.overlayWidget()->selectedId(), confirmedObjectId);
    QCOMPARE(dlg.overlayWidget()->objectMask(confirmedObjectId), confirmedMaskBefore);
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("predictionInFlight")).toBool());

    // 取消前的点、负点、框命令仍占据栈位置，但取消 epoch 使其 undo/redo 回调成为 no-op。
    for (int index = 0; index < 3; ++index)
        dlg.undoButton()->click();
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    for (int index = 0; index < 3; ++index)
        dlg.redoButton()->click();
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    for (int index = 0; index < 3; ++index)
        dlg.undoButton()->click();
    dlg.undoButton()->click();
    QVERIFY(dlg.session().annotations.isEmpty());
    dlg.redoButton()->click();
    QCOMPARE(dlg.session().annotations.size(), 1);
    QCOMPARE(dlg.session().annotations.first().id, confirmedObjectId);
    for (int index = 0; index < 3; ++index)
        dlg.redoButton()->click();
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());

    // 以相同坐标重新创建提示，必须使用新 generation 和请求 ID。
    QVERIFY(clickPrompt(QPointF(20, 20), Qt::LeftButton));
    QVERIFY(clickPrompt(QPointF(30, 31), Qt::RightButton));
    QVERIFY(dragPromptBox(QPointF(40, 41), QPointF(50, 51)));
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    dlg.undoButton()->click(); // 框、负点、正点按栈序撤销。
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    dlg.undoButton()->click();
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    dlg.undoButton()->click();
    QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    dlg.redoButton()->click();
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    dlg.redoButton()->click();
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    dlg.redoButton()->click();
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    const QJsonObject newRequestState = dlg.agentPromptStateSnapshot();
    QVERIFY(newRequestState.value(QStringLiteral("promptGeneration")).toString() != oldGeneration);
    const qint64 newRequestId = newRequestState.value(QStringLiteral("requestId")).toString().toLongLong();
    QVERIFY(newRequestId > oldRequestId);
    const QString stateDigestBeforeStale = newRequestState.value(QStringLiteral("stateDigest")).toString();

    qRegisterMetaType<QList<QPointF>>("QList<QPointF>");
    const QList<QPointF> stalePolygon{QPointF(1, 2), QPointF(10, 2), QPointF(10, 12), QPointF(1, 12)};
    QVERIFY(QMetaObject::invokeMethod(client, "predictionReadyForRequest", Qt::DirectConnection,
                                      Q_ARG(qint64, oldRequestId), Q_ARG(QList<QPointF>, stalePolygon),
                                      Q_ARG(QRectF, QRectF(1, 2, 9, 10)), Q_ARG(double, 0.91),
                                      Q_ARG(QString, QStringLiteral("stale_private_rle")), Q_ARG(QImage, QImage())));
    QCOMPARE(dlg.agentPromptStateSnapshot().value(QStringLiteral("stateDigest")).toString(), stateDigestBeforeStale);
    QCOMPARE(dlg.previewCandidateCount(), 0);
    QCOMPARE(client->pendingPredictionRequestId(), newRequestId);
    const QString statusTextBeforeStaleError = dlg.statusLabel()->text();
    QVERIFY(QMetaObject::invokeMethod(client, "predictionErrorForRequest", Qt::DirectConnection,
                                      Q_ARG(qint64, oldRequestId), Q_ARG(QString, QStringLiteral("stale failure"))));
    QCOMPARE(dlg.statusLabel()->text(), statusTextBeforeStaleError);
    QCOMPARE(client->pendingPredictionRequestId(), newRequestId);

    // /predict 的真实新响应最终可更新当前代次。
    QTRY_COMPARE_WITH_TIMEOUT(dlg.previewCandidateCount(), 3, 3000);
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
    QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("candidateCount")).toInt() == 3);
}

void TestSamAnnotatorDialog::agentPromptCancelStopsInvalidEmbeddingRetry() {
    const QByteArray oldAppData = qgetenv("DEEPLUX_APP_DATA_DIR");
    const bool hadAppData = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");
    const QByteArray oldServerPython = qgetenv("SAM_SERVER_PYTHON");
    const bool hadServerPython = qEnvironmentVariableIsSet("SAM_SERVER_PYTHON");
    QTemporaryDir isolatedAppData;
    QVERIFY(isolatedAppData.isValid());
    struct EnvironmentRestore {
        QByteArray appData;
        bool hadAppData = false;
        QByteArray serverPython;
        bool hadServerPython = false;
        ~EnvironmentRestore() {
            if (hadAppData)
                qputenv("DEEPLUX_APP_DATA_DIR", appData);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
            if (hadServerPython)
                qputenv("SAM_SERVER_PYTHON", serverPython);
            else
                qunsetenv("SAM_SERVER_PYTHON");
        }
    } environmentRestore{oldAppData, hadAppData, oldServerPython, hadServerPython};
    qputenv("DEEPLUX_APP_DATA_DIR", isolatedAppData.path().toLocal8Bit());
    qunsetenv("SAM_SERVER_PYTHON");

    for (const bool failRetry : {false, true}) {
        SamTestServer server;
        QVERIFY(server.listen());
        server.predictStatus = QStringLiteral("invalid_embedding");

        SamAnnotatorDialog dlg;
        const QImage image = makeTestImage();
        dlg.setImageSnapshot(image, QString());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString imagePath = directory.filePath(QStringLiteral("agent-prompt-retry.png"));
        QVERIFY(image.save(imagePath));

        SamBackendClient* client = dlg.backendClient();
        QVERIFY(client != nullptr);
        client->stopServerProcess();
        client->setServerUrl(server.url());
        client->setTimeoutMs(3000);
        client->setImage(imagePath);
        QTRY_VERIFY_WITH_TIMEOUT(!client->currentEmbeddingId().isEmpty(), 3000);
        server.delayMs.insert(QStringLiteral("/set_image"), 500);
        if (failRetry)
            server.failSetImageRequests.insert(2);

        dlg.positivePointButton()->setChecked(true);
        QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection,
                                          Q_ARG(QPointF, QPointF(20, 20)), Q_ARG(Qt::MouseButton, Qt::LeftButton)));
        QTRY_COMPARE_WITH_TIMEOUT(server.requestLog.count(QStringLiteral("POST /set_image")), 2, 3000);
        QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
        QCOMPARE(client->pendingPredictionRequestId(), qint64(0));
        QVERIFY(!dlg.agentPromptStatus().value(QStringLiteral("predictionInFlight")).toBool());
        QCOMPARE(dlg.cancelAgentPrompt().value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));

        QSignalSpy embeddingReadySpy(client, &SamBackendClient::embeddingReady);
        QSignalSpy errorSpy(client, &SamBackendClient::errorOccurred);
        QTRY_COMPARE_WITH_TIMEOUT(server.responseLog.count(QStringLiteral("POST /set_image")), 2, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!client->hasPendingSetImageRequest(), 3000);
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
        if (failRetry) {
            QVERIFY(client->currentEmbeddingId().isEmpty());
            QCOMPARE(client->state(), SamBackendClient::State::Error);
            QCOMPARE(embeddingReadySpy.count(), 0);
            QCOMPARE(errorSpy.count(), 1);

            // 旧 embedding 已因 invalid_embedding 失效；失败后必须显式重新加载当前图。
            client->setImage(imagePath);
            QTRY_VERIFY_WITH_TIMEOUT(!client->currentEmbeddingId().isEmpty(), 3000);
            QCOMPARE(embeddingReadySpy.count(), 1);
        } else {
            QVERIFY(!client->currentEmbeddingId().isEmpty());
            QCOMPARE(client->state(), SamBackendClient::State::Ready);
            QCOMPARE(embeddingReadySpy.count(), 1);
            QCOMPARE(errorSpy.count(), 0);
        }

        // 新一代提示只在有效 embedding 就绪后发起；已取消提示的 retry 不得重放。
        server.predictStatus = QStringLiteral("ok");
        const QString activeEmbedding = client->currentEmbeddingId();
        QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection,
                                          Q_ARG(QPointF, QPointF(22, 22)), Q_ARG(Qt::MouseButton, Qt::LeftButton)));
        QTRY_COMPARE_WITH_TIMEOUT(dlg.previewCandidateCount(), 3, 3000);
        const QJsonObject newPromptSnapshot = dlg.agentPromptStateSnapshot();
        QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 2);
        QCOMPARE(server.lastPredictBody.value(QStringLiteral("embedding_id")).toString(), activeEmbedding);
        QCOMPARE(dlg.agentPromptStateSnapshot(), newPromptSnapshot);
        QCOMPARE(dlg.previewCandidateCount(), 3);
        QVERIFY(dlg.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
        QCOMPARE(client->currentEmbeddingId(), activeEmbedding);
        QCOMPARE(client->pendingPredictionRequestId(), qint64(0));
    }
}

void TestSamAnnotatorDialog::agentOpenSessionCancelsOldPredictionAndPinsNewImage() {
    SamTestServer server;
    QVERIFY(server.listen());
    server.delayMs.insert(QStringLiteral("/predict"), 600);

    SamAnnotatorDialog dialog;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString oldImagePath = directory.filePath(QStringLiteral("old-image.png"));
    const QString newImagePath = directory.filePath(QStringLiteral("new-image.png"));
    const QString guiImagePath = directory.filePath(QStringLiteral("gui-image.png"));
    QImage oldImage(64, 48, QImage::Format_RGB32);
    oldImage.fill(QColor("#1b6ac9"));
    QImage newImage(64, 48, QImage::Format_RGB32);
    newImage.fill(QColor("#d94332"));
    QImage guiImage(64, 48, QImage::Format_RGB32);
    guiImage.fill(QColor("#27a85c"));
    QVERIFY(oldImage.save(oldImagePath));
    QVERIFY(newImage.save(newImagePath));
    QVERIFY(guiImage.save(guiImagePath));
    const QString modelPath = directory.filePath(QStringLiteral("sam_vit_b_test-placeholder.pth"));
    QFile modelFile(modelPath);
    QVERIFY(modelFile.open(QIODevice::WriteOnly));
    QCOMPARE(modelFile.write("test model placeholder"), qint64(22));
    modelFile.close();
    dialog.setImageSnapshot(oldImage, oldImagePath);

    SamBackendClient* client = dialog.backendClient();
    QVERIFY(client != nullptr);
    client->stopServerProcess();
    client->setModelPath(modelPath);
    client->setServerUrl(server.url());
    client->setTimeoutMs(3000);
    client->setImage(oldImagePath);
    QTRY_VERIFY_WITH_TIMEOUT(!client->currentEmbeddingId().isEmpty(), 3000);
    const int initialSetImageCount = server.setImageCount;
    QCOMPARE(client->currentEmbeddingId(), QStringLiteral("emb-%1").arg(initialSetImageCount));

    dialog.positivePointButton()->setChecked(true);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOverlayClicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(20, 20)), Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QTRY_COMPARE_WITH_TIMEOUT(server.requestLog.count(QStringLiteral("POST /predict")), 1, 3000);
    QVERIFY(dialog.agentPromptStatus().value(QStringLiteral("predictionInFlight")).toBool());
    QVERIFY(client->pendingPredictionRequestId() > 0);

    const QByteArray previousServerPython = qgetenv("SAM_SERVER_PYTHON");
    const bool hadServerPython = qEnvironmentVariableIsSet("SAM_SERVER_PYTHON");
    const QByteArray previousSamModel = qgetenv("DEEPLUX_SAM_MODEL");
    const bool hadSamModel = qEnvironmentVariableIsSet("DEEPLUX_SAM_MODEL");
    struct EnvironmentRestore {
        QByteArray serverPython;
        bool hadServerPython = false;
        QByteArray model;
        bool hadModel = false;
        ~EnvironmentRestore() {
            if (hadServerPython)
                qputenv("SAM_SERVER_PYTHON", serverPython);
            else
                qunsetenv("SAM_SERVER_PYTHON");
            if (hadModel)
                qputenv("DEEPLUX_SAM_MODEL", model);
            else
                qunsetenv("DEEPLUX_SAM_MODEL");
        }
    } environmentRestore{previousServerPython, hadServerPython, previousSamModel, hadSamModel};
    qputenv("SAM_SERVER_PYTHON", QCoreApplication::applicationFilePath().toLocal8Bit());
    qputenv("DEEPLUX_SAM_MODEL", modelPath.toLocal8Bit());

    AnnotationSession openedSession;
    openedSession.imagePath = newImagePath;
    openedSession.imageWidth = newImage.width();
    openedSession.imageHeight = newImage.height();
    openedSession.imageDigest = AnnotationSession::computeImageDigest(newImage);
    QString error;
    QVERIFY2(
        dialog.openAgentAnnotationSessionSnapshot(openedSession, newImage, true, false, QStringLiteral("test"), &error),
        qPrintable(error));
    QCOMPARE(client->pendingPredictionRequestId(), qint64(0));
    QTRY_COMPARE_WITH_TIMEOUT(client->currentEmbeddingId(), QStringLiteral("emb-2"), 3000);
    QCOMPARE(server.setImageCount, 2);
    QCOMPARE(client->state(), SamBackendClient::State::Ready);
    QCOMPARE(server.lastSetImage, newImage);
    const QString agentPinnedPath = dialog.backendImagePathForTest();
    QVERIFY(!agentPinnedPath.isEmpty());
    QVERIFY(QFileInfo::exists(agentPinnedPath));
    QVERIFY(QFileInfo(agentPinnedPath).canonicalFilePath() != QFileInfo(newImagePath).canonicalFilePath());
    QCOMPARE(server.lastSetImagePath, agentPinnedPath);

    // 即使旧 /predict 在服务端延迟返回，其结果也不能重新污染刚打开的会话。
    QTest::qWait(700);
    QCOMPARE(client->currentEmbeddingId(), QStringLiteral("emb-2"));
    QCOMPARE(client->pendingPredictionRequestId(), qint64(0));
    QCOMPARE(client->state(), SamBackendClient::State::Ready);
    QCOMPARE(server.setImageCount, 2);
    QCOMPARE(dialog.previewCandidateCount(), 0);
    QVERIFY(!dialog.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());

    // 新提示必须携带新 embedding，证明后续推理使用的是已打开会话的像素快照。
    server.delayMs.remove(QStringLiteral("/predict"));
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOverlayClicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(22, 22)), Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QTRY_COMPARE_WITH_TIMEOUT(server.requestLog.count(QStringLiteral("POST /predict")), 2, 3000);
    QCOMPARE(server.lastPredictBody.value(QStringLiteral("embedding_id")).toString(), QStringLiteral("emb-2"));
    QTRY_COMPARE_WITH_TIMEOUT(dialog.previewCandidateCount(), 3, 3000);

    // 离开 Agent 固定快照回到 GUI 原图时，旧临时 PNG 应立即释放。
    const int setImageCountBeforeGuiSwitch = server.setImageCount;
    server.delayMs.insert(QStringLiteral("/set_image"), 400);
    dialog.setImageSnapshot(guiImage, guiImagePath);
    QVERIFY(!QFileInfo::exists(agentPinnedPath));
    QCOMPARE(dialog.backendImagePathForTest(), guiImagePath);
    QTRY_COMPARE_WITH_TIMEOUT(server.setImageCount, setImageCountBeforeGuiSwitch + 1, 3000);
    QVERIFY(client->currentEmbeddingId().isEmpty());
    QCOMPARE(client->state(), SamBackendClient::State::LoadingModel);

    // 在 GUI 图像 set_image 尚未完成时批准的新快照必须取代该请求。
    server.delayMs.remove(QStringLiteral("/set_image"));
    QVERIFY2(dialog.openAgentAnnotationSessionSnapshot(openedSession, newImage, true, false,
                                                       QStringLiteral("test-clear"), &error),
             qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(server.setImageCount, setImageCountBeforeGuiSwitch + 2, 3000);
    const QString clearedSnapshotPath = dialog.backendImagePathForTest();
    QVERIFY(QFileInfo::exists(clearedSnapshotPath));
    QCOMPARE(server.lastSetImagePath, clearedSnapshotPath);
    QCOMPARE(server.lastSetImage, newImage);
    QTRY_COMPARE_WITH_TIMEOUT(client->currentEmbeddingId(), QStringLiteral("emb-%1").arg(server.setImageCount), 3000);
    QTest::qWait(450); // 迟到的 GUI 图像 set_image 响应不得覆盖新的 embedding。
    QCOMPARE(client->currentEmbeddingId(), QStringLiteral("emb-%1").arg(server.setImageCount));

    dialog.setImageSnapshot(QImage(), QString());
    QVERIFY(!QFileInfo::exists(clearedSnapshotPath));
    QVERIFY(dialog.backendImagePathForTest().isEmpty());
}

void TestSamAnnotatorDialog::agentOpenSessionReplacesCancelledEmbeddingRetry() {
    SamTestServer server;
    QVERIFY(server.listen());
    server.predictStatus = QStringLiteral("invalid_embedding");
    server.delayMs.insert(QStringLiteral("/set_image"), 400);

    SamAnnotatorDialog dialog;
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString oldImagePath = directory.filePath(QStringLiteral("retry-old-image.png"));
    const QString newImagePath = directory.filePath(QStringLiteral("retry-new-image.png"));
    QImage oldImage(64, 48, QImage::Format_RGB32);
    oldImage.fill(QColor("#3277cf"));
    QImage newImage(64, 48, QImage::Format_RGB32);
    newImage.fill(QColor("#e34a31"));
    QVERIFY(oldImage.save(oldImagePath));
    QVERIFY(newImage.save(newImagePath));
    const QString modelPath = directory.filePath(QStringLiteral("sam_vit_b_test-placeholder.pth"));
    QFile modelFile(modelPath);
    QVERIFY(modelFile.open(QIODevice::WriteOnly));
    QCOMPARE(modelFile.write("test model placeholder"), qint64(22));
    modelFile.close();
    dialog.setImageSnapshot(oldImage, oldImagePath);

    SamBackendClient* client = dialog.backendClient();
    QVERIFY(client != nullptr);
    client->stopServerProcess();
    client->setModelPath(modelPath);
    client->setServerUrl(server.url());
    client->setTimeoutMs(3000);
    client->setImage(oldImagePath);
    QTRY_VERIFY_WITH_TIMEOUT(!client->currentEmbeddingId().isEmpty(), 3000);
    const int initialSetImageCount = server.setImageCount;
    const int delayedSetImageResponses = server.delayedResponseLog.count(QStringLiteral("POST /set_image"));
    QCOMPARE(client->currentEmbeddingId(), QStringLiteral("emb-%1").arg(initialSetImageCount));

    dialog.positivePointButton()->setChecked(true);
    QVERIFY(QMetaObject::invokeMethod(&dialog, "onOverlayClicked", Qt::DirectConnection,
                                      Q_ARG(QPointF, QPointF(20, 20)), Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QTRY_COMPARE_WITH_TIMEOUT(server.requestLog.count(QStringLiteral("POST /predict")), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(server.setImageCount, initialSetImageCount + 1, 3000);
    QVERIFY(client->currentEmbeddingId().isEmpty());
    QVERIFY(client->hasPendingSetImageRequest());
    QCOMPARE(dialog.cancelAgentPrompt().value(QStringLiteral("status")).toString(), QStringLiteral("cancelled"));
    QCOMPARE(client->state(), SamBackendClient::State::NotStarted);
    QVERIFY(client->hasPendingSetImageRequest());

    const QByteArray oldServerPython = qgetenv("SAM_SERVER_PYTHON");
    const bool hadServerPython = qEnvironmentVariableIsSet("SAM_SERVER_PYTHON");
    const QByteArray oldSamModel = qgetenv("DEEPLUX_SAM_MODEL");
    const bool hadSamModel = qEnvironmentVariableIsSet("DEEPLUX_SAM_MODEL");
    struct EnvironmentRestore {
        QByteArray serverPython;
        bool hadServerPython = false;
        QByteArray model;
        bool hadModel = false;
        ~EnvironmentRestore() {
            if (hadServerPython)
                qputenv("SAM_SERVER_PYTHON", serverPython);
            else
                qunsetenv("SAM_SERVER_PYTHON");
            if (hadModel)
                qputenv("DEEPLUX_SAM_MODEL", model);
            else
                qunsetenv("DEEPLUX_SAM_MODEL");
        }
    } environmentRestore{oldServerPython, hadServerPython, oldSamModel, hadSamModel};
    qputenv("SAM_SERVER_PYTHON", QCoreApplication::applicationFilePath().toLocal8Bit());
    qputenv("DEEPLUX_SAM_MODEL", modelPath.toLocal8Bit());

    server.delayMs.remove(QStringLiteral("/set_image"));
    AnnotationSession openedSession;
    openedSession.imagePath = newImagePath;
    openedSession.imageWidth = newImage.width();
    openedSession.imageHeight = newImage.height();
    openedSession.imageDigest = AnnotationSession::computeImageDigest(newImage);
    QString error;
    QVERIFY2(dialog.openAgentAnnotationSessionSnapshot(openedSession, newImage, true, false,
                                                       QStringLiteral("test-retry-replacement"), &error),
             qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(server.setImageCount, initialSetImageCount + 2, 3000);
    const QString pinnedPath = dialog.backendImagePathForTest();
    QVERIFY(QFileInfo::exists(pinnedPath));
    QCOMPARE(server.lastSetImagePath, pinnedPath);
    QCOMPARE(server.lastSetImage, newImage);
    QTRY_COMPARE_WITH_TIMEOUT(client->currentEmbeddingId(), QStringLiteral("emb-%1").arg(initialSetImageCount + 2),
                              3000);
    // 服务端明确记录旧 retry 的延迟响应定时器触发（即便客户端已断开），embedding 仍不能回滚。
    QTRY_COMPARE_WITH_TIMEOUT(server.delayedResponseLog.count(QStringLiteral("POST /set_image")),
                              delayedSetImageResponses + 1, 3000);
    QCOMPARE(client->currentEmbeddingId(), QStringLiteral("emb-%1").arg(initialSetImageCount + 2));
    QCOMPARE(client->state(), SamBackendClient::State::Ready);
    QCOMPARE(server.requestLog.count(QStringLiteral("POST /predict")), 1);
    QVERIFY(!dialog.agentPromptStatus().value(QStringLiteral("hasPrompt")).toBool());
}
#ifdef DEEPLUX_HAS_ONNXRUNTIME
void TestSamAnnotatorDialog::nativeModelSettingsDialogSavesSelection() {
    const QString packagePath = QStringLiteral(TEST_SAM_PACKAGE_DIR);
    QVERIFY2(QFile::exists(packagePath + QStringLiteral("/manifest.json")), qPrintable(packagePath));

    SamAnnotatorDialog dlg;
    auto* backend = dlg.findChild<QComboBox*>(QStringLiteral("SamBackendMode"));
    auto* settingsButton = dlg.importModelButton();
    QVERIFY(backend);
    QVERIFY(settingsButton);
    backend->setCurrentIndex(backend->findData(QStringLiteral("native")));
    QVERIFY(!settingsButton->isHidden());
    QCOMPARE(settingsButton->text(), QStringLiteral("模型设置…"));

    bool dialogCompleted = false;
    QTimer::singleShot(0, &dlg, [&]() {
        auto* settingsDialog = dlg.findChild<QDialog*>(QStringLiteral("SamNativeModelSettingsDialog"));
        if (!settingsDialog)
            return;
        auto* path = settingsDialog->findChild<QLineEdit*>(QStringLiteral("SamNativePackagePath"));
        auto* device = settingsDialog->findChild<QComboBox*>(QStringLiteral("SamNativeDevice"));
        auto* apply = settingsDialog->findChild<QPushButton*>(QStringLiteral("SamNativeApplySettings"));
        if (!path || !device || !apply)
            return;
        path->setText(packagePath);
        device->setCurrentIndex(device->findData(QStringLiteral("cpu")));
        const QString capture = qEnvironmentVariable("DEEPLUX_TEST_SAM_NATIVE_SETTINGS_CAPTURE");
        if (!capture.isEmpty())
            settingsDialog->grab().save(capture);
        apply->click();
        dialogCompleted = settingsDialog->result() == QDialog::Accepted;
    });
    settingsButton->click();
    QVERIFY(dialogCompleted);
    const SamNativeConfig saved = SamNativeSettings::snapshot();
    QCOMPARE(saved.packageDir, QFileInfo(packagePath).canonicalFilePath());
    QCOMPARE(saved.device, QStringLiteral("cpu"));
    QVERIFY(saved.error.isEmpty());

    dlg.applyTheme(true);
    bool darkDialogCancelled = false;
    QTimer::singleShot(0, &dlg, [&]() {
        auto* settingsDialog = dlg.findChild<QDialog*>(QStringLiteral("SamNativeModelSettingsDialog"));
        if (!settingsDialog)
            return;
        const QString capture = qEnvironmentVariable("DEEPLUX_TEST_SAM_NATIVE_SETTINGS_CAPTURE");
        if (!capture.isEmpty())
            settingsDialog->grab().save(capture + QStringLiteral(".dark.png"));
        auto* cancel = settingsDialog->findChild<QPushButton*>(QStringLiteral("SamNativeCancelSettings"));
        if (!cancel)
            return;
        cancel->click();
        darkDialogCancelled = settingsDialog->result() == QDialog::Rejected;
    });
    QVERIFY(QMetaObject::invokeMethod(&dlg, "showNativeModelSettings", Qt::DirectConnection));
    QVERIFY(darkDialogCancelled);
}

void TestSamAnnotatorDialog::nativeSettingsHonorsEnvironmentOverride() {
    QTemporaryDir invalidOverride;
    QVERIFY(invalidOverride.isValid());
    qputenv("DEEPLUX_SAM_PACKAGE", QFile::encodeName(invalidOverride.filePath(QStringLiteral("missing"))));
    const SamNativeConfig overridden = SamNativeSettings::snapshot();
    QVERIFY(overridden.packageFromEnvironment);
    QVERIFY(!overridden.error.isEmpty());
    QVERIFY(overridden.error.contains(QStringLiteral("目录不存在")));
    qunsetenv("DEEPLUX_SAM_PACKAGE");
    const SamNativeConfig restored = SamNativeSettings::snapshot();
    QVERIFY(restored.error.isEmpty());
    QVERIFY(!restored.packageFromEnvironment);
}

void TestSamAnnotatorDialog::nativeBackendPredictsAndConfirmsSelectedCandidate() {
    const QString packageDir = QStringLiteral(TEST_SAM_PACKAGE_DIR);
    QVERIFY2(QFile::exists(packageDir + QStringLiteral("/manifest.json")), qPrintable(packageDir));
    qputenv("DEEPLUX_SAM_PACKAGE", QFile::encodeName(packageDir));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage image(640, 480, QImage::Format_RGB888);
    image.fill(QColor(96, 110, 100));
    QPainter painter(&image);
    painter.fillRect(QRect(200, 150, 80, 60), Qt::white);
    painter.end();
    const QString imagePath = dir.filePath(QStringLiteral("native.png"));
    QVERIFY(image.save(imagePath));

    SamAnnotatorDialog dlg;
    auto* mode = dlg.findChild<QComboBox*>(QStringLiteral("SamBackendMode"));
    QVERIFY(mode);
    QCOMPARE(mode->currentData().toString(), QStringLiteral("http"));
    const int nativeIndex = mode->findData(QStringLiteral("native"));
    QVERIFY(nativeIndex >= 0);
    mode->setCurrentIndex(nativeIndex);
    QCOMPARE(dlg.backendClient()->serverUrl(), QStringLiteral("native"));
    dlg.show();
    QCoreApplication::processEvents();
    const QString capturePath = qEnvironmentVariable("DEEPLUX_SAM_UI_CAPTURE");
    if (!capturePath.isEmpty())
        QVERIFY(dlg.grab().save(capturePath));
    dlg.setImageSnapshot(image, imagePath);
    QCOMPARE(dlg.backendClient()->currentEmbeddingId(), imagePath);
    const QPoint clickPoint = dlg.imageWidget()->imageToWidget(QPointF(240, 180)).toPoint();
    QVERIFY(dlg.overlayWidget()->rect().contains(clickPoint));
    QTest::mouseClick(dlg.overlayWidget(), Qt::LeftButton, Qt::NoModifier, clickPoint);
    QTRY_COMPARE_WITH_TIMEOUT(dlg.previewCandidateCount(), 3, 30000);
    const QList<SamCandidate> candidates = dlg.backendClient()->lastCandidates();
    QCOMPARE(candidates.size(), 3);
    QVERIFY(!candidates[1].maskRle.isEmpty());
    QVERIFY(candidates[0].maskRle != candidates[1].maskRle);
    dlg.nextCandidateButton()->click();
    QCOMPARE(dlg.previewCandidateIndex(), 1);
    dlg.categoryCombo()->setEditText(QStringLiteral("defect"));
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onConfirm", Qt::DirectConnection));
    const AnnotationSession session = dlg.session();
    QCOMPARE(session.annotations.size(), 1);
    QCOMPARE(session.annotations.first().maskRle, candidates[1].maskRle);
    QCOMPARE(session.annotations.first().bbox, candidates[1].bbox);

    QSignalSpy predictions(dlg.backendClient(), &SamBackendClient::predictionReady);
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(240, 180)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QImage nextImage = image;
    nextImage.fill(QColor(96, 110, 100));
    QPainter nextPainter(&nextImage);
    nextPainter.fillRect(QRect(300, 150, 80, 60), Qt::white);
    nextPainter.end();
    const QString nextPath = dir.filePath(QStringLiteral("next.png"));
    QVERIFY(nextImage.save(nextPath));
    dlg.setImageSnapshot(nextImage, nextPath);
    QCOMPARE(dlg.backendClient()->currentEmbeddingId(), nextPath);
    QCOMPARE(dlg.previewCandidateCount(), 0);
    QVERIFY(dlg.backendClient()->lastCandidates().isEmpty());
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(340, 180)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QTRY_COMPARE_WITH_TIMEOUT(dlg.previewCandidateCount(), 3, 30000);
    QCOMPARE(predictions.size(), 1); // 旧图在途结果不得覆盖新图

    mode->setCurrentIndex(0);
    QCOMPARE(dlg.backendClient()->serverUrl(), QStringLiteral("http://127.0.0.1:0"));
    QCOMPARE(dlg.previewCandidateCount(), 0);
}

void TestSamAnnotatorDialog::nativeBackendAcceptsNegativePointOnly() {
    qputenv("DEEPLUX_SAM_PACKAGE", QFile::encodeName(QStringLiteral(TEST_SAM_PACKAGE_DIR)));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage image(640, 480, QImage::Format_RGB888);
    image.fill(Qt::white);
    const QString imagePath = dir.filePath(QStringLiteral("negative-only.png"));
    QVERIFY(image.save(imagePath));

    SamAnnotatorDialog dlg;
    auto* mode = dlg.findChild<QComboBox*>(QStringLiteral("SamBackendMode"));
    QVERIFY(mode);
    mode->setCurrentIndex(mode->findData(QStringLiteral("native")));
    dlg.setImageSnapshot(image, imagePath);
    dlg.negativePointButton()->click();
    QSignalSpy predictions(dlg.backendClient(), &SamBackendClient::predictionReady);
    QSignalSpy errors(dlg.backendClient(), &SamBackendClient::errorOccurred);
    QVERIFY(QMetaObject::invokeMethod(&dlg, "onOverlayClicked", Qt::DirectConnection, Q_ARG(QPointF, QPointF(240, 180)),
                                      Q_ARG(Qt::MouseButton, Qt::LeftButton)));
    QTRY_VERIFY_WITH_TIMEOUT(!predictions.isEmpty() || !errors.isEmpty(), 30000);
    QVERIFY2(!predictions.isEmpty(),
             errors.isEmpty() ? "原生 SAM 未返回负点预测" : qPrintable(errors.first().first().toString()));
    for (const QList<QVariant>& error : errors)
        QVERIFY2(!error.first().toString().contains(QStringLiteral("至少提供一个")),
                 qPrintable(error.first().toString()));
}

void TestSamAnnotatorDialog::closingWithNativeRequestQueuedDoesNotBlockUi() {
    qputenv("DEEPLUX_SAM_PACKAGE", QFile::encodeName(QStringLiteral(TEST_SAM_PACKAGE_DIR)));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage image(640, 480, QImage::Format_RGB888);
    image.fill(Qt::white);
    const QString imagePath = dir.filePath(QStringLiteral("close.png"));
    QVERIFY(image.save(imagePath));
    auto dlg = std::make_unique<SamAnnotatorDialog>();
    auto* mode = dlg->findChild<QComboBox*>(QStringLiteral("SamBackendMode"));
    QVERIFY(mode);
    mode->setCurrentIndex(mode->findData(QStringLiteral("native")));
    dlg->setImageSnapshot(image, imagePath);
    QThreadPool* pool = samNativeThreadPool();
    QVERIFY(pool);
    QTRY_COMPARE_WITH_TIMEOUT(pool->activeThreadCount(), 0, 30000);
    auto gate = std::make_shared<QSemaphore>();
    auto entered = std::make_shared<QSemaphore>();
    pool->start([gate, entered] {
        entered->release();
        gate->acquire();
    });
    if (!entered->tryAcquire(1, 2000)) {
        gate->release();
        QFAIL("共享原生线程池的阻塞任务未在 2 秒内启动");
    }
    const bool invoked =
        QMetaObject::invokeMethod(dlg.get(), "onOverlayClicked", Qt::DirectConnection,
                                  Q_ARG(QPointF, QPointF(240, 180)), Q_ARG(Qt::MouseButton, Qt::LeftButton));
    const bool requestBusy = invoked && dlg->backendClient()->state() == SamBackendClient::State::Busy;
    std::atomic<bool> gateReleased{false};
    std::thread watchdog([gate, &gateReleased] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        gateReleased.store(true, std::memory_order_release);
        gate->release();
    });
    QElapsedTimer timer;
    timer.start();
    dlg.reset();
    const qint64 closeMs = timer.elapsed();
    const bool releasedBeforeClose = gateReleased.load(std::memory_order_acquire);
    const bool poolStillActive = pool->activeThreadCount() > 0;
    watchdog.join();
    QVERIFY(invoked);
    QVERIFY(requestBusy);
    QVERIFY(!releasedBeforeClose);
    QVERIFY(poolStillActive);
    QVERIFY2(closeMs < 1000, qPrintable(QStringLiteral("关闭耗时 %1 ms").arg(closeMs)));
}
#endif

QTEST_MAIN(TestSamAnnotatorDialog)
#include "test_samannotatordialog.moc"
