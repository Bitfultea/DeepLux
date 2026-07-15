#include "SamAnnotatorDialog.h"

#include "../widgets/AnnotationOverlayWidget.h"
#include "../widgets/HImageWidget.h"
#include "core/agent/SamBackendClient.h"
#include "core/common/Logger.h"
#include "core/io/LabelMeExporter.h"
#include "core/model/Annotation.h"

#include <QButtonGroup>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QTemporaryFile>
#include <QToolButton>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>

namespace DeepLux {

class LambdaUndoCommand : public QUndoCommand {
public:
    LambdaUndoCommand(const QString& text, std::function<void()> redoFn, std::function<void()> undoFn)
        : QUndoCommand(text), m_redo(std::move(redoFn)), m_undo(std::move(undoFn)) {}

    void redo() override {
        if (m_redo)
            m_redo();
    }
    void undo() override {
        if (m_undo)
            m_undo();
    }

private:
    std::function<void()> m_redo;
    std::function<void()> m_undo;
};

SamAnnotatorDialog::SamAnnotatorDialog(QWidget* parent)
    : QDialog(parent), m_session(new AnnotationSession()), m_samClient(new SamBackendClient(this)),
      m_undoStack(new QUndoStack(this)) {
    setWindowTitle(tr("SAM 快速标注"));
    setMinimumSize(900, 600);
    setupUi();
    setupShortcuts();
    refreshOverlayCoordConverter();

    connect(m_samClient, &SamBackendClient::predictionReady, this, &SamAnnotatorDialog::onPredictionReady);
    connect(m_samClient, &SamBackendClient::embeddingReady, this, [this](const QString&) {
        setStatusText(tr("SAM 已就绪"));
        if (!m_positivePoints.isEmpty() || !m_negativePoints.isEmpty() || m_dragBox.isValid()) {
            requestPrediction();
        }
    });
    connect(m_samClient, &SamBackendClient::errorOccurred, this, [this](const QString& message) {
        m_hasPrediction = false;
        if (m_confirmButton)
            m_confirmButton->setEnabled(false);
        setStatusText(tr("SAM 错误：%1").arg(message));
    });
    connect(m_samClient, &SamBackendClient::stateChanged, this, [this](SamBackendClient::State state) {
        if (state == SamBackendClient::State::Busy)
            setStatusText(tr("SAM 预测中..."));
        else if (state == SamBackendClient::State::LoadingModel)
            setStatusText(tr("SAM 加载图像..."));
        else if (state == SamBackendClient::State::Ready)
            setStatusText(tr("SAM 已就绪"));
    });

    setToolMode(ToolMode::Select);
    syncOverlayMode();
}

SamAnnotatorDialog::~SamAnnotatorDialog() {
    delete m_session;
}

bool SamAnnotatorDialog::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_imageWidget && event->type() == QEvent::Resize && m_overlay) {
        m_overlay->setGeometry(m_imageWidget->rect());
        m_overlay->raise();
    }
    return QDialog::eventFilter(watched, event);
}

void SamAnnotatorDialog::setStatusText(const QString& text) {
    if (m_statusLabel)
        m_statusLabel->setText(text);
}

void SamAnnotatorDialog::setupUi() {
    // 主体：水平三栏 → 左侧面板 | 中间图像 | 底部工具栏
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);

    auto* hSplit = new QHBoxLayout();
    hSplit->setSpacing(4);
    root->addLayout(hSplit, 1);

    // ===== 左侧：类别输入 + 对象列表 =====
    auto* leftPanel = new QWidget();
    leftPanel->setMinimumWidth(220);
    auto* leftLayout = new QVBoxLayout(leftPanel);
    leftLayout->setContentsMargins(0, 0, 0, 0);

    auto* lblCategory = new QLabel(tr("类别"));
    leftLayout->addWidget(lblCategory);

    m_categoryEdit = new QLineEdit();
    m_categoryEdit->setPlaceholderText(tr("输入类别名，如 defect"));
    leftLayout->addWidget(m_categoryEdit);

    auto* lblObjects = new QLabel(tr("对象列表"));
    leftLayout->addWidget(lblObjects);

    m_objectList = new QListWidget();
    m_objectList->setSelectionMode(QAbstractItemView::SingleSelection);
    leftLayout->addWidget(m_objectList, 1);

    auto* saveBtn = new QPushButton(tr("保存会话"));
    auto* exportBtn = new QPushButton(tr("导出 LabelMe"));
    auto* btnRow = new QHBoxLayout();
    btnRow->addWidget(saveBtn);
    btnRow->addWidget(exportBtn);
    leftLayout->addLayout(btnRow);

    connect(saveBtn, &QPushButton::clicked, this, &SamAnnotatorDialog::onSaveSession);
    connect(exportBtn, &QPushButton::clicked, this, &SamAnnotatorDialog::onExportLabelMe);

    hSplit->addWidget(leftPanel);

    // ===== 中间：HImageWidget + 覆盖层 =====
    auto* centerContainer = new QWidget();
    centerContainer->setObjectName("AnnotatorCenter");
    auto* centerLayout = new QVBoxLayout(centerContainer);
    centerLayout->setContentsMargins(0, 0, 0, 0);

    m_imageWidget = new HImageWidget(centerContainer);
    centerLayout->addWidget(m_imageWidget, 1);

    // AnnotationOverlayWidget 作为透明覆盖层，与 HImageWidget 同尺寸
    m_overlay = new AnnotationOverlayWidget(m_imageWidget);
    m_overlay->setGeometry(m_imageWidget->rect());
    m_overlay->raise();
    m_overlay->show();
    m_imageWidget->installEventFilter(this);

    hSplit->addWidget(centerContainer, 1);

    // ===== 底部：工具栏（4 个模式按钮 + 打开） =====
    auto* toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);
    root->addLayout(toolbar);

    auto* openBtn = new QPushButton(tr("打开图片"));
    toolbar->addWidget(openBtn);
    connect(openBtn, &QPushButton::clicked, this, &SamAnnotatorDialog::onOpenImage);

    toolbar->addSpacing(10);

    m_btnSelect = new QToolButton();
    m_btnSelect->setText(tr("选择"));
    m_btnSelect->setCheckable(true);
    toolbar->addWidget(m_btnSelect);

    m_btnPositivePoint = new QToolButton();
    m_btnPositivePoint->setText(tr("正点"));
    m_btnPositivePoint->setCheckable(true);
    toolbar->addWidget(m_btnPositivePoint);

    m_btnNegativePoint = new QToolButton();
    m_btnNegativePoint->setText(tr("负点"));
    m_btnNegativePoint->setCheckable(true);
    toolbar->addWidget(m_btnNegativePoint);

    m_btnBox = new QToolButton();
    m_btnBox->setText(tr("框选"));
    m_btnBox->setCheckable(true);
    toolbar->addWidget(m_btnBox);

    toolbar->addStretch(1);

    m_statusLabel = new QLabel(tr("未加载图像"));
    m_statusLabel->setStyleSheet("color:#64748B;");
    toolbar->addWidget(m_statusLabel);

    m_confirmButton = new QPushButton(tr("确认"));
    m_confirmButton->setEnabled(false);
    toolbar->addWidget(m_confirmButton);
    connect(m_confirmButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onConfirm);

    auto* hint = new QLabel(tr("Enter=确认  Esc=取消  Delete=删除选中"));
    hint->setStyleSheet("color:#64748B;");
    toolbar->addWidget(hint);

    // 模式按钮互斥
    m_modeButtonGroup = new QButtonGroup(this);
    m_modeButtonGroup->setExclusive(true);
    m_modeButtonGroup->addButton(m_btnSelect, static_cast<int>(ToolMode::Select));
    m_modeButtonGroup->addButton(m_btnPositivePoint, static_cast<int>(ToolMode::PositivePoint));
    m_modeButtonGroup->addButton(m_btnNegativePoint, static_cast<int>(ToolMode::NegativePoint));
    m_modeButtonGroup->addButton(m_btnBox, static_cast<int>(ToolMode::Box));
    connect(m_modeButtonGroup, &QButtonGroup::idToggled, this, [this](int id, bool checked) {
        if (checked) {
            setToolMode(static_cast<ToolMode>(id));
        }
    });

    // overlay 事件 → 转换到原图坐标
    connect(m_overlay, &AnnotationOverlayWidget::widgetClicked, this, &SamAnnotatorDialog::onOverlayClicked);
    connect(m_overlay, &AnnotationOverlayWidget::dragEnded, this, &SamAnnotatorDialog::onOverlayDragEnded);

    // 对象列表选择
    connect(m_objectList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { onObjectSelectionChanged(); });
}

void SamAnnotatorDialog::setupShortcuts() {
    m_scConfirm = new QShortcut(QKeySequence(Qt::Key_Return), this);
    connect(m_scConfirm, &QShortcut::activated, this, &SamAnnotatorDialog::onConfirm);
    // 同时绑定 Enter（Keypad）
    auto* scEnter = new QShortcut(QKeySequence(Qt::Key_Enter), this);
    connect(scEnter, &QShortcut::activated, this, &SamAnnotatorDialog::onConfirm);

    m_scCancel = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(m_scCancel, &QShortcut::activated, this, &SamAnnotatorDialog::onCancel);

    m_scDelete = new QShortcut(QKeySequence(Qt::Key_Delete), this);
    connect(m_scDelete, &QShortcut::activated, this, &SamAnnotatorDialog::onDeleteSelected);

    m_scUndo = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Z")), this);
    connect(m_scUndo, &QShortcut::activated, this, &SamAnnotatorDialog::onUndo);
}

void SamAnnotatorDialog::setToolMode(ToolMode mode) {
    m_toolMode = mode;
    // 互斥按钮状态
    m_btnSelect->blockSignals(true);
    m_btnPositivePoint->blockSignals(true);
    m_btnNegativePoint->blockSignals(true);
    m_btnBox->blockSignals(true);

    m_btnSelect->setChecked(mode == ToolMode::Select);
    m_btnPositivePoint->setChecked(mode == ToolMode::PositivePoint);
    m_btnNegativePoint->setChecked(mode == ToolMode::NegativePoint);
    m_btnBox->setChecked(mode == ToolMode::Box);

    m_btnSelect->blockSignals(false);
    m_btnPositivePoint->blockSignals(false);
    m_btnNegativePoint->blockSignals(false);
    m_btnBox->blockSignals(false);

    syncOverlayMode();
}

void SamAnnotatorDialog::syncOverlayMode() {
    if (!m_overlay)
        return;
    AnnotationOverlayWidget::Mode om = AnnotationOverlayWidget::Mode::Select;
    switch (m_toolMode) {
    case ToolMode::Select:
        om = AnnotationOverlayWidget::Mode::Select;
        break;
    case ToolMode::PositivePoint:
        om = AnnotationOverlayWidget::Mode::PositivePoint;
        break;
    case ToolMode::NegativePoint:
        om = AnnotationOverlayWidget::Mode::NegativePoint;
        break;
    case ToolMode::Box:
        om = AnnotationOverlayWidget::Mode::Box;
        break;
    }
    m_overlay->setMode(om);
}

void SamAnnotatorDialog::onModeButtonToggled() {
    // 取最后一个按下的按钮决定模式
    if (m_btnPositivePoint->isChecked()) {
        setToolMode(ToolMode::PositivePoint);
    } else if (m_btnNegativePoint->isChecked()) {
        setToolMode(ToolMode::NegativePoint);
    } else if (m_btnBox->isChecked()) {
        setToolMode(ToolMode::Box);
    } else {
        setToolMode(ToolMode::Select);
    }
}

void SamAnnotatorDialog::refreshOverlayCoordConverter() {
    if (!m_imageWidget || !m_overlay)
        return;
    m_overlay->setGeometry(m_imageWidget->rect());
    m_overlay->setCoordConverter(
        [this](const QPointF& imagePoint) -> QPointF { return m_imageWidget->imageToWidget(imagePoint); });
    m_overlay->setInverseCoordConverter(
        [this](const QPointF& widgetPoint) -> QPointF { return m_imageWidget->widgetToImage(widgetPoint); });
}

void SamAnnotatorDialog::onOverlayClicked(const QPointF& imagePoint, Qt::MouseButton button) {
    if (m_toolMode == ToolMode::Select) {
        onObjectSelectionChanged();
        return;
    }

    const bool negative = button == Qt::RightButton || m_toolMode == ToolMode::NegativePoint;
    m_undoStack->push(new LambdaUndoCommand(
        tr("添加提示点"),
        [this, imagePoint, negative]() {
            if (negative)
                m_negativePoints.append(imagePoint);
            else
                m_positivePoints.append(imagePoint);
            refreshPromptAfterEdit(true);
        },
        [this, negative]() {
            if (negative && !m_negativePoints.isEmpty())
                m_negativePoints.removeLast();
            else if (!negative && !m_positivePoints.isEmpty())
                m_positivePoints.removeLast();
            refreshPromptAfterEdit(true);
        }));
}

void SamAnnotatorDialog::onOverlayDragEnded(const QPointF& imageStart, const QPointF& imageEnd) {
    const QRectF previousBox = m_dragBox;
    const QRectF nextBox = QRectF(imageStart, imageEnd).normalized();
    m_undoStack->push(new LambdaUndoCommand(
        tr("设置框选"),
        [this, nextBox]() {
            m_dragBox = nextBox;
            refreshPromptAfterEdit(true);
        },
        [this, previousBox]() {
            m_dragBox = previousBox;
            refreshPromptAfterEdit(true);
        }));
}

void SamAnnotatorDialog::onConfirm() {
    commitCurrentPromptAsObject();
}

void SamAnnotatorDialog::onCancel() {
    clearCurrentPrompt();
    if (m_undoStack)
        m_undoStack->clear();
}

void SamAnnotatorDialog::onDeleteSelected() {
    auto* item = m_objectList->currentItem();
    if (!item)
        return;
    const QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty())
        return;

    int index = -1;
    AnnotationObject object;
    for (int i = 0; i < m_session->annotations.size(); ++i) {
        if (m_session->annotations[i].id == id) {
            index = i;
            object = m_session->annotations[i];
            break;
        }
    }
    if (index < 0)
        return;

    m_undoStack->push(new LambdaUndoCommand(
        tr("删除标注对象"),
        [this, id]() {
            m_session->removeById(id);
            refreshObjectList();
            m_overlay->setAnnotations(m_session->annotations);
        },
        [this, object, index]() {
            const int insertAt = qBound(0, index, m_session->annotations.size());
            m_session->annotations.insert(insertAt, object);
            refreshObjectList();
            m_overlay->setAnnotations(m_session->annotations);
        }));
}

void SamAnnotatorDialog::onUndo() {
    if (m_undoStack && m_undoStack->canUndo())
        m_undoStack->undo();
}

void SamAnnotatorDialog::onObjectSelectionChanged() {
    auto* item = m_objectList->currentItem();
    QString id;
    if (item) {
        id = item->data(Qt::UserRole).toString();
    }
    m_overlay->setSelectedId(id);
}

void SamAnnotatorDialog::refreshObjectList() {
    m_objectList->blockSignals(true);
    m_objectList->clear();
    for (const AnnotationObject& obj : m_session->annotations) {
        auto* item = new QListWidgetItem(QStringLiteral("%1  [%2]").arg(obj.label, obj.id));
        item->setData(Qt::UserRole, obj.id);
        m_objectList->addItem(item);
    }
    m_objectList->blockSignals(false);
}

void SamAnnotatorDialog::addConfirmedObject(const AnnotationObject& obj) {
    m_session->annotations.append(obj);
    refreshObjectList();
    m_overlay->setAnnotations(m_session->annotations);
}

void SamAnnotatorDialog::commitCurrentPromptAsObject() {
    if (!m_hasPrediction || m_previewPolygon.isEmpty()) {
        setStatusText(tr("请先完成一次 SAM 预测"));
        return;
    }

    AnnotationObject obj;
    obj.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    obj.label =
        m_categoryEdit->text().trimmed().isEmpty() ? QStringLiteral("object") : m_categoryEdit->text().trimmed();
    obj.prompts.pointsPos = m_positivePoints;
    obj.prompts.pointsNeg = m_negativePoints;
    if (m_dragBox.isValid())
        obj.prompts.box = m_dragBox;
    obj.polygon = m_previewPolygon;
    obj.bbox = m_previewBbox;
    obj.score = m_previewScore;
    obj.maskRle = m_previewMaskRle;
    obj.modelName = m_samClient->modelName().isEmpty() ? QStringLiteral("sam") : m_samClient->modelName();

    addConfirmedObject(obj);
    clearCurrentPrompt();
    if (m_undoStack)
        m_undoStack->clear();
}

void SamAnnotatorDialog::refreshPromptAfterEdit(bool triggerPrediction) {
    m_hasPrediction = false;
    if (m_confirmButton)
        m_confirmButton->setEnabled(false);
    m_previewPolygon.clear();
    m_previewBbox = QRectF();
    m_previewMaskRle.clear();
    m_previewScore = 0.0;
    m_overlay->clearPreview();
    m_overlay->setPromptPoints(m_positivePoints, m_negativePoints);
    if (m_dragBox.isValid())
        m_overlay->setPreviewBox(m_dragBox);
    if (triggerPrediction)
        requestPrediction();
}

void SamAnnotatorDialog::clearCurrentPrompt() {
    m_positivePoints.clear();
    m_negativePoints.clear();
    m_dragBox = QRectF();
    m_previewPolygon.clear();
    m_previewBbox = QRectF();
    m_previewMaskRle.clear();
    m_previewScore = 0.0;
    m_hasPrediction = false;
    if (m_confirmButton)
        m_confirmButton->setEnabled(false);
    m_overlay->clearPromptPoints();
    m_overlay->clearPreview();
}

void SamAnnotatorDialog::prepareBackendImage() {
    m_backendImagePath.clear();
    if (m_currentImage.isNull())
        return;

    if (!m_imagePath.isEmpty() && QFileInfo::exists(m_imagePath)) {
        m_backendImagePath = m_imagePath;
    } else {
        m_tempImageFile.reset(new QTemporaryFile(QDir::tempPath() + QStringLiteral("/deeplux_sam_XXXXXX.png")));
        m_tempImageFile->setAutoRemove(true);
        if (!m_tempImageFile->open()) {
            setStatusText(tr("无法创建临时标注图像"));
            return;
        }
        m_backendImagePath = m_tempImageFile->fileName();
        m_tempImageFile->close();
        if (!m_currentImage.save(m_backendImagePath, "PNG")) {
            setStatusText(tr("无法写入临时标注图像"));
            m_backendImagePath.clear();
            return;
        }
    }

    if (m_backendImagePath.isEmpty()) {
        setStatusText(tr("无可用原图路径，SAM 暂不可用"));
        return;
    }

    setStatusText(tr("SAM 加载图像..."));
    m_samClient->setImage(m_backendImagePath);
}

void SamAnnotatorDialog::requestPrediction() {
    if (m_positivePoints.isEmpty() && m_negativePoints.isEmpty() && !m_dragBox.isValid())
        return;
    m_hasPrediction = false;
    if (m_confirmButton)
        m_confirmButton->setEnabled(false);

    if (m_samClient->currentEmbeddingId().isEmpty()) {
        setStatusText(tr("SAM 尚未就绪"));
        return;
    }

    setStatusText(tr("SAM 预测中..."));
    m_samClient->predict(m_positivePoints, m_negativePoints, m_dragBox);
}

void SamAnnotatorDialog::onPredictionReady(const QList<QPointF>& polygon, const QRectF& bbox, double score,
                                           const QString& maskRle) {
    if (polygon.isEmpty()) {
        m_hasPrediction = false;
        if (m_confirmButton)
            m_confirmButton->setEnabled(false);
        setStatusText(tr("SAM 未返回有效轮廓"));
        return;
    }

    m_previewPolygon = polygon;
    m_previewBbox = bbox;
    m_previewScore = score;
    m_previewMaskRle = maskRle;
    m_hasPrediction = true;
    m_overlay->setPreviewPolygon(m_previewPolygon);
    if (m_previewBbox.isValid())
        m_overlay->setPreviewBox(m_previewBbox);
    if (m_confirmButton)
        m_confirmButton->setEnabled(true);
    setStatusText(tr("SAM 预测完成 score=%1").arg(score, 0, 'f', 2));
}

void SamAnnotatorDialog::updateSessionFromImage() {
    if (m_currentImage.isNull())
        return;
    m_session->imagePath = m_imagePath;
    m_session->imageWidth = m_currentImage.width();
    m_session->imageHeight = m_currentImage.height();
    m_session->modelName = m_samClient->modelName().isEmpty() ? QStringLiteral("sam") : m_samClient->modelName();
}

void SamAnnotatorDialog::openImageFromFile(const QString& suggestedPath) {
    QString startDir = suggestedPath;
    if (startDir.isEmpty() && !m_imagePath.isEmpty()) {
        startDir = m_imagePath;
    }
    QString path = QFileDialog::getOpenFileName(this, tr("选择图片"), startDir,
                                                tr("图片文件 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    QImage img(path);
    if (img.isNull()) {
        QMessageBox::warning(this, tr("打开失败"), tr("无法加载图片：") + path);
        return;
    }
    m_currentImage = img;
    m_imagePath = path;
    m_imageWidget->setImage(img);
    m_imageWidget->fitToWindow();
    refreshOverlayCoordConverter();
    updateSessionFromImage();
    prepareBackendImage();
    emit imageLoaded(path);
}

void SamAnnotatorDialog::onOpenImage() {
    openImageFromFile();
}

void SamAnnotatorDialog::setImageSnapshot(const QImage& image, const QString& imagePath) {
    m_currentImage = image;
    m_imagePath = imagePath;
    if (!image.isNull()) {
        m_imageWidget->setImage(image);
        m_imageWidget->fitToWindow();
    }
    refreshOverlayCoordConverter();
    updateSessionFromImage();
    prepareBackendImage();
    emit imageLoaded(imagePath);
}

void SamAnnotatorDialog::onSaveSession() {
    if (m_session->annotations.isEmpty()) {
        QMessageBox::information(this, tr("保存"), tr("当前没有标注对象，无需保存"));
        return;
    }
    const QFileInfo imageInfo(m_imagePath);
    const QString defaultPath = imageInfo.exists()
                                    ? imageInfo.absolutePath() + QStringLiteral("/") + imageInfo.completeBaseName() +
                                          QStringLiteral(".deeplux-anno.json")
                                    : QStringLiteral("annotations.deeplux-anno.json");
    QString path = QFileDialog::getSaveFileName(this, tr("保存标注会话"), defaultPath, tr("JSON (*.json)"));
    if (path.isEmpty())
        return;

    QString err;
    updateSessionFromImage();
    if (!m_session->save(path, &err)) {
        QMessageBox::warning(this, tr("保存失败"), err);
        return;
    }
    Logger::instance().info(tr("标注会话已保存至 %1").arg(path), "Annotation");
}

void SamAnnotatorDialog::onExportLabelMe() {
    if (m_session->annotations.isEmpty()) {
        QMessageBox::information(this, tr("导出"), tr("当前没有标注对象，无需导出"));
        return;
    }
    QString path =
        QFileDialog::getSaveFileName(this, tr("导出 LabelMe"), m_imagePath + ".labelme.json", tr("JSON (*.json)"));
    if (path.isEmpty())
        return;

    QString err;
    updateSessionFromImage();
    if (!LabelMeExporter::exportToFile(*m_session, path, &err)) {
        QMessageBox::warning(this, tr("导出失败"), err);
        return;
    }
    Logger::instance().info(tr("LabelMe 已导出至 %1").arg(path), "Annotation");
}

AnnotationSession SamAnnotatorDialog::session() const {
    return *m_session;
}

} // namespace DeepLux
