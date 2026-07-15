#include "SamAnnotatorDialog.h"

#include "../widgets/AnnotationOverlayWidget.h"
#include "../widgets/HImageWidget.h"
#include "core/io/LabelMeExporter.h"
#include "core/model/Annotation.h"
#include "core/common/Logger.h"

#include <QFileDialog>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QUuid>
#include <QWidget>

namespace DeepLux {

SamAnnotatorDialog::SamAnnotatorDialog(QWidget* parent)
    : QDialog(parent)
    , m_session(new AnnotationSession())
{
    setWindowTitle(tr("SAM 快速标注"));
    setMinimumSize(900, 600);
    setupUi();
    setupShortcuts();
    refreshOverlayCoordConverter();
    // 默认选择模式
    setToolMode(ToolMode::Select);
    syncOverlayMode();
}

SamAnnotatorDialog::~SamAnnotatorDialog() = default;

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
    m_overlay->raise();
    m_overlay->show();

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
    connect(m_overlay, &AnnotationOverlayWidget::widgetClicked,
            this, &SamAnnotatorDialog::onOverlayClicked);
    connect(m_overlay, &AnnotationOverlayWidget::dragEnded,
            this, &SamAnnotatorDialog::onOverlayDragEnded);

    // 对象列表选择
    connect(m_objectList, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem*, QListWidgetItem*) { onObjectSelectionChanged(); });
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
    if (!m_overlay) return;
    AnnotationOverlayWidget::Mode om = AnnotationOverlayWidget::Mode::Select;
    switch (m_toolMode) {
    case ToolMode::Select: om = AnnotationOverlayWidget::Mode::Select; break;
    case ToolMode::PositivePoint: om = AnnotationOverlayWidget::Mode::PositivePoint; break;
    case ToolMode::NegativePoint: om = AnnotationOverlayWidget::Mode::NegativePoint; break;
    case ToolMode::Box: om = AnnotationOverlayWidget::Mode::Box; break;
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
    if (!m_imageWidget || !m_overlay) return;
    // AnnotationOverlayWidget 期望 imageToWidget
    m_overlay->setCoordConverter(
        [this](const QPointF& imagePoint) -> QPointF {
            return m_imageWidget->imageToWidget(imagePoint);
        });
}

void SamAnnotatorDialog::onOverlayClicked(const QPointF& imagePoint, Qt::MouseButton button) {
    Q_UNUSED(button)
    // 由 overlay 传入的已经是 widget 坐标，此处需转为 image 坐标
    // 注：AnnotationOverlayWidget 当前直接 emit widget 坐标，这里由 HImageWidget 转换
    QPointF imgPt = m_imageWidget->widgetToImage(imagePoint);

    if (m_toolMode == ToolMode::PositivePoint) {
        m_positivePoints.append(imgPt);
        m_overlay->setPromptPoints(m_positivePoints, m_negativePoints);
    } else if (m_toolMode == ToolMode::NegativePoint) {
        m_negativePoints.append(imgPt);
        m_overlay->setPromptPoints(m_positivePoints, m_negativePoints);
    } else if (m_toolMode == ToolMode::Select) {
        // 选择模式：在对象列表中查找
        onObjectSelectionChanged();
    }
}

void SamAnnotatorDialog::onOverlayDragEnded(const QPointF& imageStart, const QPointF& imageEnd) {
    // imageStart/imageEnd 为 widget 坐标 → 转换为原图坐标
    QPointF p1 = m_imageWidget->widgetToImage(imageStart);
    QPointF p2 = m_imageWidget->widgetToImage(imageEnd);
    m_dragBox = QRectF(p1, p2).normalized();
    m_overlay->setPreviewBox(m_dragBox);
}

void SamAnnotatorDialog::onConfirm() {
    commitCurrentPromptAsObject();
}

void SamAnnotatorDialog::onCancel() {
    // 清空当前未确认的 prompt
    m_positivePoints.clear();
    m_negativePoints.clear();
    m_dragBox = QRectF();
    m_overlay->clearPromptPoints();
    m_overlay->clearPreview();
}

void SamAnnotatorDialog::onDeleteSelected() {
    auto* item = m_objectList->currentItem();
    if (!item) return;
    QString id = item->data(Qt::UserRole).toString();
    if (id.isEmpty()) return;
    m_session->removeById(id);
    refreshObjectList();
    m_overlay->setAnnotations(m_session->annotations);
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
    if (m_positivePoints.isEmpty() && m_negativePoints.isEmpty() && !m_dragBox.isValid()) {
        return;
    }

    AnnotationObject obj;
    obj.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    obj.label = m_categoryEdit->text().trimmed().isEmpty()
                    ? QStringLiteral("object")
                    : m_categoryEdit->text().trimmed();
    obj.prompts.pointsPos = m_positivePoints;
    obj.prompts.pointsNeg = m_negativePoints;
    if (m_dragBox.isValid()) {
        obj.prompts.box = m_dragBox;
        obj.bbox = m_dragBox;
    }

    // 第一期：用 prompt 推导占位 polygon（围绕正点或 box 边界向外扩张 5px）
    QList<QPointF> poly;
    if (m_dragBox.isValid()) {
        QRectF b = m_dragBox;
        poly = {b.topLeft(), QPointF(b.right(), b.top()), b.bottomRight(), QPointF(b.left(), b.bottom())};
    } else if (!m_positivePoints.isEmpty()) {
        QPointF c = m_positivePoints.first();
        poly = {QPointF(c.x() - 5, c.y() - 5), QPointF(c.x() + 5, c.y() - 5),
                QPointF(c.x() + 5, c.y() + 5), QPointF(c.x() - 5, c.y() + 5)};
    }
    obj.polygon = poly;
    obj.score = 1.0;
    obj.modelName = m_session->modelName.isEmpty() ? QStringLiteral("stub") : m_session->modelName;

    addConfirmedObject(obj);

    // 清空当前 prompt
    m_positivePoints.clear();
    m_negativePoints.clear();
    m_dragBox = QRectF();
    m_overlay->clearPromptPoints();
    m_overlay->clearPreview();
}

void SamAnnotatorDialog::updateSessionFromImage() {
    if (m_currentImage.isNull()) return;
    m_session->imagePath = m_imagePath;
    m_session->imageWidth = m_currentImage.width();
    m_session->imageHeight = m_currentImage.height();
    m_session->modelName = QStringLiteral("stub-v1");
}

void SamAnnotatorDialog::openImageFromFile(const QString& suggestedPath) {
    QString startDir = suggestedPath;
    if (startDir.isEmpty() && !m_imagePath.isEmpty()) {
        startDir = m_imagePath;
    }
    QString path = QFileDialog::getOpenFileName(
        this, tr("选择图片"), startDir,
        tr("图片文件 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

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
    emit imageLoaded(imagePath);
}

void SamAnnotatorDialog::onSaveSession() {
    if (m_session->annotations.isEmpty()) {
        QMessageBox::information(this, tr("保存"), tr("当前没有标注对象，无需保存"));
        return;
    }
    QString path = QFileDialog::getSaveFileName(
        this, tr("保存标注会话"),
        m_imagePath + ".annotation.json",
        tr("JSON (*.json)"));
    if (path.isEmpty()) return;

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
    QString path = QFileDialog::getSaveFileName(
        this, tr("导出 LabelMe"),
        m_imagePath + ".labelme.json",
        tr("JSON (*.json)"));
    if (path.isEmpty()) return;

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
