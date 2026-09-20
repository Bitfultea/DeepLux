#include "SamAnnotatorDialog.h"

#include "../ThemeManager.h"
#include "../widgets/AnnotationOverlayWidget.h"
#include "../widgets/HImageWidget.h"
#include "core/agent/SamBackendClient.h"
#include "core/common/Logger.h"
#include "core/io/LabelMeExporter.h"
#include "core/io/YoloSegExporter.h"
#include "core/manager/ConfigManager.h"
#include "core/model/Annotation.h"
#include "core/model/AnnotationMaskOps.h"

#include <QButtonGroup>
#include <QColor>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainterPath>
#include <QPushButton>
#include <QShortcut>
#include <QSizePolicy>
#include <QSpinBox>
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

namespace {

bool annotationContainsImagePoint(const AnnotationObject& obj, const QPointF& imagePoint) {
    if (obj.polygon.size() >= 3) {
        QPainterPath path;
        path.moveTo(obj.polygon.first());
        for (int i = 1; i < obj.polygon.size(); ++i)
            path.lineTo(obj.polygon.at(i));
        path.closeSubpath();
        if (path.contains(imagePoint))
            return true;
    }

    return obj.bbox.adjusted(-2, -2, 2, 2).contains(imagePoint);
}

} // namespace

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
    setObjectName(QStringLiteral("SamAnnotatorDialog"));
    setMinimumSize(900, 600);
    setupUi();
    setupShortcuts();
    applyTheme(false);
    refreshOverlayCoordConverter();
    initializeDefaultCategories();
    loadSavedModelPath();

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
        refreshSamControlState();
    });
    connect(m_samClient, &SamBackendClient::environmentInitializationStarted, this, [this]() {
        refreshSamControlState();
        setStatusText(tr("正在初始化 SAM 环境..."));
    });
    connect(m_samClient, &SamBackendClient::environmentInitializationProgress, this,
            [this](const QString& message) { setStatusText(message); });
    connect(m_samClient, &SamBackendClient::environmentInitializationFinished, this,
            [this](bool ok, const QString& message) {
                refreshSamControlState();
                setStatusText(message);
                if (ok && !m_backendImagePath.isEmpty() && samModelReadyForUse())
                    prepareBackendImage();
            });
    connect(m_samClient, &SamBackendClient::stateChanged, this, [this](SamBackendClient::State state) {
        if (state == SamBackendClient::State::Starting)
            setStatusText(tr("SAM 启动中..."));
        else if (state == SamBackendClient::State::Busy)
            setStatusText(tr("SAM 预测中..."));
        else if (state == SamBackendClient::State::LoadingModel)
            setStatusText(tr("SAM 加载图像..."));
        else if (state == SamBackendClient::State::Ready) {
            setStatusText(tr("SAM 已就绪"));
            if (m_samClient->currentEmbeddingId().isEmpty() && !m_backendImagePath.isEmpty()) {
                m_samClient->setImage(m_backendImagePath);
            }
        }
        refreshSamControlState();
    });

    refreshSamControlState();
    setToolMode(ToolMode::PositivePoint);
    syncOverlayMode();
}

SamAnnotatorDialog::~SamAnnotatorDialog() {
    if (m_externalImageWidget)
        m_externalImageWidget->removeEventFilter(this);
    if (m_imageWidget)
        m_imageWidget->removeEventFilter(this);
    if (m_overlay) {
        m_overlay->setParent(nullptr);
        delete m_overlay;
    }
    delete m_session;
}

bool SamAnnotatorDialog::eventFilter(QObject* watched, QEvent* event) {
    HImageWidget* target = activeImageWidget();
    if (watched == target && event->type() == QEvent::Resize && m_overlay) {
        m_overlay->setGeometry(target->rect());
        m_overlay->raise();
    }
    return QDialog::eventFilter(watched, event);
}

HImageWidget* SamAnnotatorDialog::activeImageWidget() const {
    return m_externalImageWidget ? static_cast<HImageWidget*>(m_externalImageWidget) : m_imageWidget;
}

void SamAnnotatorDialog::moveOverlayToImageWidget(HImageWidget* imageWidget) {
    if (!imageWidget || !m_overlay)
        return;
    m_overlay->setParent(imageWidget);
    m_overlay->setGeometry(imageWidget->rect());
    m_overlay->raise();
    m_overlay->show();
    imageWidget->installEventFilter(this);
    refreshOverlayCoordConverter();
}

void SamAnnotatorDialog::setStatusText(const QString& text) {
    if (m_statusLabel)
        m_statusLabel->setText(text);
}

void SamAnnotatorDialog::applyTheme(bool isDark) {
    const ThemePalette pal = ThemeManager::palette(isDark);
    const QString panelBg = isDark ? QStringLiteral("#252525") : QStringLiteral("#ffffff");
    const QString inputBg = isDark ? QStringLiteral("#333333") : QStringLiteral("#ffffff");
    const QString subtleText = isDark ? QStringLiteral("#d1d5db") : QStringLiteral("#64748b");
    const QString disabledBg = isDark ? QStringLiteral("#555555") : QStringLiteral("#cccccc");
    const QString modeBg = isDark ? QStringLiteral("#333333") : QStringLiteral("#f8fafc");
    const QString modeHoverBg = isDark ? QStringLiteral("#3a3a3a") : QStringLiteral("#eef2f7");
    const QString modeCheckedBg = isDark ? QStringLiteral("#0e7490") : QStringLiteral("#e2e8f0");
    const QString modeCheckedBorder = isDark ? QStringLiteral("#06b6d4") : QStringLiteral("#0f172a");
    const QString modeCheckedText = isDark ? QStringLiteral("#ffffff") : QStringLiteral("#0f172a");

    setStyleSheet(
        QStringLiteral("QDialog#SamAnnotatorDialog { background-color: %1; color: %2; }"
                       "QWidget#SamControlPanel { background-color: %3; color: %2; }"
                       "QLabel { background-color: transparent; color: %2; }"
                       "QLabel#SamStatusLabel, QLabel#SamShortcutHintLabel { color: %4; font-size: 12px; }"
                       "QLineEdit { background-color: %5; color: %2; border: 1px solid %6; padding: 2px 4px; }"
                       "QLineEdit:focus { border: 1px solid #0078d7; }"
                       "QListWidget { background-color: %5; color: %2; border: 1px solid %6; }"
                       "QListWidget::item:selected { background-color: #0078d7; color: #ffffff; }"
                       "QPushButton { background-color: #0078d7; color: #ffffff; border: 1px solid #005a9e; "
                       "padding: 2px 6px; font-weight: 600; }"
                       "QPushButton:hover { background-color: #1e8ad6; }"
                       "QPushButton:disabled { background-color: %7; color: #999999; border-color: %6; }"
                       "QToolButton { border: 1px solid %6; border-radius: 4px; padding: 2px 6px; "
                       "background-color: %8; color: %2; }"
                       "QToolButton:hover { background-color: %9; }"
                       "QToolButton:checked { border: 2px solid %10; background-color: %11; color: %12; "
                       "font-weight: 700; }")
            .arg(pal.bgColor)
            .arg(pal.textColor)
            .arg(panelBg)
            .arg(subtleText)
            .arg(inputBg)
            .arg(pal.borderColor)
            .arg(disabledBg)
            .arg(modeBg)
            .arg(modeHoverBg)
            .arg(modeCheckedBorder)
            .arg(modeCheckedBg)
            .arg(modeCheckedText));
}

void SamAnnotatorDialog::loadSavedModelPath() {
    ConfigManager::instance().initialize();
    auto useModelPath = [this](const QString& path) {
        if (path.isEmpty() || !QFileInfo::exists(path))
            return false;
        m_samClient->setModelPath(path);
        if (!m_samClient->unsupportedModelReason().isEmpty()) {
            m_samClient->setModelPath(QString());
            return false;
        }
        qputenv("DEEPLUX_SAM_MODEL", path.toLocal8Bit());
        return true;
    };

    const QString savedPath = ConfigManager::instance().groupString(QStringLiteral("sam"), QStringLiteral("modelPath"));
    if (useModelPath(savedPath))
        return;

    const QString downloadedPath = QDir::home().filePath(QStringLiteral("Downloads/sam_vit_b_01ec64.pth"));
    if (useModelPath(downloadedPath))
        saveModelPath(downloadedPath);
}

void SamAnnotatorDialog::saveModelPath(const QString& path) {
    ConfigManager::instance().initialize();
    ConfigManager::instance().setGroupValue(QStringLiteral("sam"), QStringLiteral("modelPath"), path);
    ConfigManager::instance().save();
}

bool SamAnnotatorDialog::samEnvironmentReadyForUse() const {
    return !qgetenv("SAM_SERVER_PYTHON").isEmpty() || m_samClient->managedEnvironmentReady();
}

bool SamAnnotatorDialog::samModelReadyForUse() const {
    return !m_samClient->modelPath().isEmpty() || !qgetenv("DEEPLUX_SAM_MODEL").isEmpty();
}

void SamAnnotatorDialog::refreshSamControlState() {
    const bool envOverride = !qgetenv("SAM_SERVER_PYTHON").isEmpty();
    const bool envReady = samEnvironmentReadyForUse();
    if (m_initEnvironmentButton) {
        const bool running = m_samClient->isEnvironmentInitializationRunning();
        m_initEnvironmentButton->setEnabled(!envOverride);
        m_initEnvironmentButton->setText(running ? tr("停止初始化") : (envReady ? tr("修复环境") : tr("初始化环境")));
    }
    if (m_restartServerButton)
        m_restartServerButton->setEnabled(envReady && samModelReadyForUse());
}

void SamAnnotatorDialog::setupUi() {
    // 主体：水平三栏 → 左侧面板 | 中间图像 | 底部工具栏
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(2);

    // hSplit 已去掉（leftPanel 整合进 controlPanel 后只剩图像容器，直接挂到 root 即可；
    // 窄窗口 hide+m_maxHeight0 后自身不占空间，不会再留下空白条）

    auto configureCompactButton = [](QPushButton* button) {
        QFont font = button->font();
        font.setPointSize(9);
        button->setFont(font);
        button->setMinimumHeight(26);
        button->setMaximumHeight(26);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };

    auto configureCompactLabel = [](QLabel* label) {
        label->setMinimumHeight(20);
        label->setMaximumHeight(20);
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    };

    auto configureCompactLineEdit = [](QLineEdit* edit) {
        QFont font = edit->font();
        font.setPointSize(9);
        edit->setFont(font);
        edit->setMinimumHeight(26);
        edit->setMaximumHeight(26);
        edit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };

    // ===== 中间：HImageWidget + 覆盖层（独占 hSplit）=====
    m_centerContainer = new QWidget();
    m_centerContainer->setObjectName("AnnotatorCenter");
    auto* centerLayout = new QVBoxLayout(m_centerContainer);
    centerLayout->setContentsMargins(0, 0, 0, 0);

    m_imageWidget = new HImageWidget(m_centerContainer);
    centerLayout->addWidget(m_imageWidget, 1);

    // AnnotationOverlayWidget 作为透明覆盖层，与 HImageWidget 同尺寸
    m_overlay = new AnnotationOverlayWidget(m_imageWidget);
    connect(m_overlay, &QObject::destroyed, this, [this]() { m_overlay = nullptr; });
    m_overlay->setGeometry(m_imageWidget->rect());
    m_overlay->raise();
    m_overlay->show();
    m_imageWidget->installEventFilter(this);

    root->addWidget(m_centerContainer);

    // ===== 底部：操作区（整合类别输入 + 对象列表 + 所有操作按钮，全宽，无上方留白）=====
    auto* controlPanel = new QWidget();
    controlPanel->setObjectName(QStringLiteral("SamControlPanel"));
    controlPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto* controlLayout = new QVBoxLayout(controlPanel);
    controlLayout->setContentsMargins(0, 0, 0, 0);
    controlLayout->setSpacing(2);
    root->addWidget(controlPanel, 1);

    auto* actionCol = new QVBoxLayout();
    actionCol->setSpacing(2);
    controlLayout->addLayout(actionCol, 1);

    // 行1：打开图片（单独全宽）
    m_openImageRowWidget = new QWidget();
    m_openImageRowWidget->setObjectName(QStringLiteral("SamOpenImageRowWidget"));
    auto* openRow = new QHBoxLayout(m_openImageRowWidget);
    openRow->setContentsMargins(0, 0, 0, 0);
    openRow->setSpacing(2);
    actionCol->addWidget(m_openImageRowWidget);

    m_openImageButton = new QPushButton(tr("打开图片"));
    m_openImageButton->setObjectName("SamOpenImageButton");
    configureCompactButton(m_openImageButton);
    openRow->addWidget(m_openImageButton);
    connect(m_openImageButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onOpenImage);

    // 类别
    auto* lblCategory = new QLabel(tr("类别"));
    lblCategory->setObjectName(QStringLiteral("SamCategoryLabel"));
    configureCompactLabel(lblCategory);
    actionCol->addWidget(lblCategory);

    // 类别输入（全宽）
    m_categoryEdit = new QLineEdit();
    m_categoryEdit->setPlaceholderText(tr("输入类别名，如 defect"));
    configureCompactLineEdit(m_categoryEdit);
    actionCol->addWidget(m_categoryEdit);

    // 类别列表 + 添加/删除按钮
    m_categoryList = new QListWidget();
    m_categoryList->setObjectName(QStringLiteral("SamCategoryList"));
    m_categoryList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_categoryList->setMinimumHeight(56);
    m_categoryList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_categoryList->setMaximumHeight(80);
    actionCol->addWidget(m_categoryList);

    auto* categoryButtonRow = new QHBoxLayout();
    categoryButtonRow->setSpacing(2);
    actionCol->addLayout(categoryButtonRow);

    m_addCategoryButton = new QPushButton(tr("添加类别"));
    configureCompactButton(m_addCategoryButton);
    categoryButtonRow->addWidget(m_addCategoryButton);
    connect(m_addCategoryButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onAddCategory);

    m_removeCategoryButton = new QPushButton(tr("删除类别"));
    configureCompactButton(m_removeCategoryButton);
    categoryButtonRow->addWidget(m_removeCategoryButton);
    connect(m_removeCategoryButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onRemoveCategory);

    connect(m_categoryList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* current) {
        if (current) {
            m_categoryEdit->setText(current->data(Qt::UserRole).toString());
        }
    });
    // Fix P1-8: 类别输入框 Enter 添加类别而非确认标注
    connect(m_categoryEdit, &QLineEdit::returnPressed, this, &SamAnnotatorDialog::onAddCategory);

    // 对象列表
    auto* lblObjects = new QLabel(tr("对象列表"));
    lblObjects->setObjectName(QStringLiteral("SamObjectListLabel"));
    configureCompactLabel(lblObjects);
    actionCol->addWidget(lblObjects);

    m_objectList = new QListWidget();
    // SAM 批2：多选用于"合并"（恰好选中两个对象时可用）
    m_objectList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_objectList->setMinimumHeight(72);
    m_objectList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    actionCol->addWidget(m_objectList);

    // 行：导入权重 + 初始化环境 + 重启 SAM（3 同宽；窄窗口测试要求三者等宽）
    auto* samActionRow = new QHBoxLayout();
    samActionRow->setSpacing(2);
    actionCol->addLayout(samActionRow);

    m_importModelButton = new QPushButton(tr("导入权重"));
    configureCompactButton(m_importModelButton);
    samActionRow->addWidget(m_importModelButton);
    connect(m_importModelButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onImportModel);

    m_initEnvironmentButton = new QPushButton(tr("初始化环境"));
    configureCompactButton(m_initEnvironmentButton);
    samActionRow->addWidget(m_initEnvironmentButton);
    connect(m_initEnvironmentButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onInitializeEnvironment);

    m_restartServerButton = new QPushButton(tr("重启 SAM"));
    configureCompactButton(m_restartServerButton);
    samActionRow->addWidget(m_restartServerButton);
    connect(m_restartServerButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onRestartSam);

    // 行1：打开标注 + 保存会话
    auto* saveExportRow1 = new QHBoxLayout();
    saveExportRow1->setSpacing(2);
    actionCol->addLayout(saveExportRow1);

    m_openAnnotationButton = new QPushButton(tr("打开标注"));
    configureCompactButton(m_openAnnotationButton);
    saveExportRow1->addWidget(m_openAnnotationButton);
    connect(m_openAnnotationButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onOpenAnnotation);

    auto* saveBtn = new QPushButton(tr("保存会话"));
    configureCompactButton(saveBtn);
    saveExportRow1->addWidget(saveBtn);
    connect(saveBtn, &QPushButton::clicked, this, &SamAnnotatorDialog::onSaveSession);

    // 行2：导出 LabelMe + 导出 YOLO
    auto* saveExportRow2 = new QHBoxLayout();
    saveExportRow2->setSpacing(2);
    actionCol->addLayout(saveExportRow2);

    auto* exportBtn = new QPushButton(tr("导出 LabelMe"));
    configureCompactButton(exportBtn);
    saveExportRow2->addWidget(exportBtn);
    connect(exportBtn, &QPushButton::clicked, this, &SamAnnotatorDialog::onExportLabelMe);

    m_exportYoloButton = new QPushButton(tr("导出 YOLO"));
    configureCompactButton(m_exportYoloButton);
    saveExportRow2->addWidget(m_exportYoloButton);
    connect(m_exportYoloButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onExportYoloSeg);

    m_statusLabel = new QLabel(tr("未加载图像"));
    m_statusLabel->setObjectName(QStringLiteral("SamStatusLabel"));
    m_statusLabel->setMinimumWidth(0);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    actionCol->addWidget(m_statusLabel);

    auto* commitRow = new QHBoxLayout();
    commitRow->setSpacing(2);
    actionCol->addLayout(commitRow);

    m_confirmButton = new QPushButton(tr("确认"));
    configureCompactButton(m_confirmButton);
    m_confirmButton->setEnabled(false);
    commitRow->addWidget(m_confirmButton);
    connect(m_confirmButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onConfirm);

    m_cancelButton = new QPushButton(tr("取消当前"));
    m_cancelButton->setObjectName(QStringLiteral("SamCancelButton"));
    configureCompactButton(m_cancelButton);
    commitRow->addWidget(m_cancelButton);
    connect(m_cancelButton, &QPushButton::clicked, this, &SamAnnotatorDialog::onCancel);

    auto* modeGrid = new QVBoxLayout();
    modeGrid->setSpacing(2);
    controlLayout->addLayout(modeGrid);

    auto* modeRow1 = new QHBoxLayout();
    auto* modeRow2 = new QHBoxLayout();
    auto* modeRow3 = new QHBoxLayout();
    auto* modeRow4 = new QHBoxLayout(); // SAM 批2：掩膜编辑动作行
    modeRow1->setSpacing(2);
    modeRow2->setSpacing(2);
    modeRow3->setSpacing(2);
    modeRow4->setSpacing(2);
    modeGrid->addLayout(modeRow1);
    modeGrid->addLayout(modeRow2);
    modeGrid->addLayout(modeRow3);
    modeGrid->addLayout(modeRow4);

    auto configureModeButton = [](QToolButton* button, const QString& text) {
        button->setText(text);
        button->setCheckable(true);
        QFont font = button->font();
        font.setPointSize(9);
        button->setFont(font);
        button->setMinimumHeight(26);
        button->setMaximumHeight(26);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };

    m_btnUndo = new QToolButton();
    configureModeButton(m_btnUndo, tr("撤销"));
    m_btnUndo->setCheckable(false);
    modeRow3->addWidget(m_btnUndo);
    connect(m_btnUndo, &QToolButton::clicked, this, &SamAnnotatorDialog::onUndo);

    m_btnRedo = new QToolButton();
    configureModeButton(m_btnRedo, tr("重做"));
    m_btnRedo->setCheckable(false);
    modeRow3->addWidget(m_btnRedo);
    connect(m_btnRedo, &QToolButton::clicked, this, &SamAnnotatorDialog::onRedo);

    // SAM 批2：形态学/橡皮擦共用半径
    m_maskRadiusSpin = new QSpinBox();
    m_maskRadiusSpin->setObjectName(QStringLiteral("SamMaskRadiusSpin"));
    m_maskRadiusSpin->setRange(1, 32);
    m_maskRadiusSpin->setValue(3);
    m_maskRadiusSpin->setPrefix(tr("半径"));
    m_maskRadiusSpin->setToolTip(tr("膨胀/腐蚀结构元半径与橡皮擦笔刷半径（像素）"));
    {
        QFont font = m_maskRadiusSpin->font();
        font.setPointSize(9);
        m_maskRadiusSpin->setFont(font);
    }
    m_maskRadiusSpin->setMinimumHeight(26);
    m_maskRadiusSpin->setMaximumHeight(26);
    m_maskRadiusSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    modeRow3->addWidget(m_maskRadiusSpin);

    // SAM 批2：掩膜编辑动作（作用于对象列表选中项，全部可撤销）
    m_btnDilate = new QToolButton();
    configureModeButton(m_btnDilate, tr("膨胀"));
    m_btnDilate->setCheckable(false);
    m_btnDilate->setToolTip(tr("选中对象掩膜形态学膨胀（半径见左）"));
    modeRow4->addWidget(m_btnDilate);
    connect(m_btnDilate, &QToolButton::clicked, this, &SamAnnotatorDialog::onDilateSelected);

    m_btnErode = new QToolButton();
    configureModeButton(m_btnErode, tr("腐蚀"));
    m_btnErode->setCheckable(false);
    m_btnErode->setToolTip(tr("选中对象掩膜形态学腐蚀（半径见上行）"));
    modeRow4->addWidget(m_btnErode);
    connect(m_btnErode, &QToolButton::clicked, this, &SamAnnotatorDialog::onErodeSelected);

    m_btnMerge = new QToolButton();
    configureModeButton(m_btnMerge, tr("合并"));
    m_btnMerge->setCheckable(false);
    m_btnMerge->setToolTip(tr("合并恰好两个选中对象（要求并集连通）"));
    modeRow4->addWidget(m_btnMerge);
    connect(m_btnMerge, &QToolButton::clicked, this, &SamAnnotatorDialog::onMergeSelected);

    m_btnSplit = new QToolButton();
    configureModeButton(m_btnSplit, tr("拆分"));
    m_btnSplit->setCheckable(false);
    m_btnSplit->setToolTip(tr("按连通分量拆分选中对象（最大分量保留原身份）"));
    modeRow4->addWidget(m_btnSplit);
    connect(m_btnSplit, &QToolButton::clicked, this, &SamAnnotatorDialog::onSplitSelected);

    m_btnSelect = new QToolButton();
    configureModeButton(m_btnSelect, tr("选择"));
    modeRow1->addWidget(m_btnSelect);

    m_btnPositivePoint = new QToolButton();
    configureModeButton(m_btnPositivePoint, tr("正点"));
    modeRow1->addWidget(m_btnPositivePoint);

    m_btnNegativePoint = new QToolButton();
    configureModeButton(m_btnNegativePoint, tr("负点"));
    modeRow2->addWidget(m_btnNegativePoint);

    m_btnBox = new QToolButton();
    configureModeButton(m_btnBox, tr("框选"));
    modeRow2->addWidget(m_btnBox);

    // SAM 批2：橡皮擦（对选中对象掩膜擦除笔画）
    m_btnEraser = new QToolButton();
    configureModeButton(m_btnEraser, tr("橡皮擦"));
    m_btnEraser->setToolTip(tr("在图像上涂抹擦除选中对象的掩膜（半径见撤销行）"));
    modeRow2->addWidget(m_btnEraser);

    auto* hint = new QLabel(tr("Enter 确认 / Esc 取消当前 / Del 删除"));
    hint->setObjectName("SamShortcutHintLabel");
    hint->setMinimumWidth(0);
    hint->setWordWrap(true);
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    controlLayout->addWidget(hint);

    // 模式按钮互斥
    m_modeButtonGroup = new QButtonGroup(this);
    m_modeButtonGroup->setExclusive(true);
    m_modeButtonGroup->addButton(m_btnSelect, static_cast<int>(ToolMode::Select));
    m_modeButtonGroup->addButton(m_btnPositivePoint, static_cast<int>(ToolMode::PositivePoint));
    m_modeButtonGroup->addButton(m_btnNegativePoint, static_cast<int>(ToolMode::NegativePoint));
    m_modeButtonGroup->addButton(m_btnBox, static_cast<int>(ToolMode::Box));
    m_modeButtonGroup->addButton(m_btnEraser, static_cast<int>(ToolMode::Eraser)); // SAM 批2
    connect(m_modeButtonGroup, &QButtonGroup::idToggled, this, [this](int id, bool checked) {
        if (checked) {
            setToolMode(static_cast<ToolMode>(id));
        }
    });

    // overlay 事件 → 转换到原图坐标
    connect(m_overlay, &AnnotationOverlayWidget::widgetClicked, this, &SamAnnotatorDialog::onOverlayClicked);
    connect(m_overlay, &AnnotationOverlayWidget::dragEnded, this, &SamAnnotatorDialog::onOverlayDragEnded);
    connect(m_overlay, &AnnotationOverlayWidget::eraseStrokeFinished, this, &SamAnnotatorDialog::onEraseStroke);

    // 对象列表选择（currentItem 驱动 overlay 高亮；selection 驱动掩膜编辑按钮可用态）
    connect(m_objectList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { onObjectSelectionChanged(); });
    connect(m_objectList, &QListWidget::itemSelectionChanged, this, [this]() { updateMaskEditButtonStates(); });
    updateMaskEditButtonStates();
}

void SamAnnotatorDialog::setupShortcuts() {
    auto makeAppShortcut = [](QShortcut* shortcut) { shortcut->setContext(Qt::ApplicationShortcut); };
    m_scConfirm = new QShortcut(QKeySequence(Qt::Key_Return), this);
    makeAppShortcut(m_scConfirm);
    connect(m_scConfirm, &QShortcut::activated, this, &SamAnnotatorDialog::onConfirm);
    // 同时绑定 Enter（Keypad）
    auto* scEnter = new QShortcut(QKeySequence(Qt::Key_Enter), this);
    makeAppShortcut(scEnter);
    connect(scEnter, &QShortcut::activated, this, &SamAnnotatorDialog::onConfirm);

    m_scCancel = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    makeAppShortcut(m_scCancel);
    connect(m_scCancel, &QShortcut::activated, this, &SamAnnotatorDialog::onCancel);

    m_scDelete = new QShortcut(QKeySequence(Qt::Key_Delete), this);
    makeAppShortcut(m_scDelete);
    connect(m_scDelete, &QShortcut::activated, this, &SamAnnotatorDialog::onDeleteSelected);

    m_scUndo = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Z")), this);
    makeAppShortcut(m_scUndo);
    connect(m_scUndo, &QShortcut::activated, this, &SamAnnotatorDialog::onUndo);

    m_scRedo = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Y")), this);
    makeAppShortcut(m_scRedo);
    connect(m_scRedo, &QShortcut::activated, this, &SamAnnotatorDialog::onRedo);
}

void SamAnnotatorDialog::setToolMode(ToolMode mode) {
    m_toolMode = mode;
    // 互斥按钮状态
    m_btnSelect->blockSignals(true);
    m_btnPositivePoint->blockSignals(true);
    m_btnNegativePoint->blockSignals(true);
    m_btnBox->blockSignals(true);
    m_btnEraser->blockSignals(true);

    m_btnSelect->setChecked(mode == ToolMode::Select);
    m_btnPositivePoint->setChecked(mode == ToolMode::PositivePoint);
    m_btnNegativePoint->setChecked(mode == ToolMode::NegativePoint);
    m_btnBox->setChecked(mode == ToolMode::Box);
    m_btnEraser->setChecked(mode == ToolMode::Eraser);

    m_btnSelect->blockSignals(false);
    m_btnPositivePoint->blockSignals(false);
    m_btnNegativePoint->blockSignals(false);
    m_btnBox->blockSignals(false);
    m_btnEraser->blockSignals(false);

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
    case ToolMode::Eraser:
        om = AnnotationOverlayWidget::Mode::Eraser;
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
    } else if (m_btnEraser->isChecked()) {
        setToolMode(ToolMode::Eraser);
    } else {
        setToolMode(ToolMode::Select);
    }
}

void SamAnnotatorDialog::refreshOverlayCoordConverter() {
    HImageWidget* target = activeImageWidget();
    if (!target || !m_overlay)
        return;
    m_overlay->setGeometry(target->rect());
    m_overlay->setCoordConverter(
        [target](const QPointF& imagePoint) -> QPointF { return target->imageToWidget(imagePoint); });
    m_overlay->setInverseCoordConverter(
        [target](const QPointF& widgetPoint) -> QPointF { return target->widgetToImage(widgetPoint); });
}

void SamAnnotatorDialog::onOverlayClicked(const QPointF& imagePoint, Qt::MouseButton button) {
    if (m_toolMode == ToolMode::Select) {
        QString selectedId;
        for (int i = m_session->annotations.size() - 1; i >= 0; --i) {
            const AnnotationObject& obj = m_session->annotations.at(i);
            if (annotationContainsImagePoint(obj, imagePoint)) {
                selectedId = obj.id;
                break;
            }
        }

        for (int row = 0; row < m_objectList->count(); ++row) {
            QListWidgetItem* item = m_objectList->item(row);
            if (item && item->data(Qt::UserRole).toString() == selectedId) {
                m_objectList->setCurrentRow(row);
                m_overlay->setSelectedId(selectedId);
                setStatusText(tr("已选择标注对象"));
                return;
            }
        }

        m_objectList->setCurrentRow(-1);
        m_overlay->setSelectedId(QString());
        setStatusText(tr("未命中标注对象"));
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
    m_samClient->cancelPendingPrediction();
    clearCurrentPrompt();
    if (m_undoStack)
        m_undoStack->clear();
    setStatusText(tr("已取消当前选择"));
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

    // 批2 复核二轮（P2-2）：掩膜缓存必须在压命令前捕获——redo 会把缓存删除，
    // undo 里再查 contains 恒为 false，导致恢复后回落 polygon 栅格化丢失像素精度
    const QImage cachedMask = m_objectMasks.value(id);
    m_undoStack->push(new LambdaUndoCommand(
        tr("删除标注对象"),
        [this, id]() {
            m_session->removeById(id);
            m_objectMasks.remove(id);
            if (m_overlay) {
                m_overlay->clearObjectMask(id);
                m_overlay->setAnnotations(m_session->annotations);
            }
            refreshObjectList();
        },
        [this, object, index, cachedMask]() {
            const int insertAt = qBound(0, index, m_session->annotations.size());
            m_session->annotations.insert(insertAt, object);
            // 恢复命令内捕获的掩膜缓存与 overlay（像素级还原）
            if (!cachedMask.isNull()) {
                m_objectMasks.insert(object.id, cachedMask);
                if (m_overlay)
                    m_overlay->setObjectMask(object.id, cachedMask);
            }
            if (m_overlay)
                m_overlay->setAnnotations(m_session->annotations);
            refreshObjectList();
        }));
}

void SamAnnotatorDialog::onUndo() {
    if (m_undoStack && m_undoStack->canUndo())
        m_undoStack->undo();
}

void SamAnnotatorDialog::onRedo() {
    if (m_undoStack && m_undoStack->canRedo())
        m_undoStack->redo();
}

// ===== SAM 批2：掩膜编辑（膨胀/腐蚀/橡皮擦/合并/拆分，全部入撤销栈）=====

SamAnnotatorDialog::MaskEditSnapshot SamAnnotatorDialog::currentSnapshot() const {
    return MaskEditSnapshot{m_session->annotations, m_objectMasks};
}

void SamAnnotatorDialog::applyEditSnapshot(const MaskEditSnapshot& snapshot, const QString& selectId) {
    if (m_overlay) {
        // overlay 掩膜缓存只增改不删：先清掉快照中已不存在的 id
        for (auto it = m_objectMasks.constBegin(); it != m_objectMasks.constEnd(); ++it) {
            if (!snapshot.displayMasks.contains(it.key()))
                m_overlay->clearObjectMask(it.key());
        }
        m_overlay->setAnnotations(snapshot.annotations);
        for (auto it = snapshot.displayMasks.constBegin(); it != snapshot.displayMasks.constEnd(); ++it)
            m_overlay->setObjectMask(it.key(), it.value());
    }
    m_session->annotations = snapshot.annotations;
    m_objectMasks = snapshot.displayMasks;
    refreshObjectList(selectId);
    updateMaskEditButtonStates();
}

void SamAnnotatorDialog::pushMaskEditCommand(const QString& text, const MaskEditSnapshot& before,
                                             const MaskEditSnapshot& after, const QString& selectId) {
    m_undoStack->push(new LambdaUndoCommand(
        text, [this, after, selectId]() { applyEditSnapshot(after, selectId); },
        [this, before, selectId]() { applyEditSnapshot(before, selectId); }));
}

bool SamAnnotatorDialog::binaryMaskForObject(const AnnotationObject& obj, QImage& outBinary, QString& error) const {
    const int w = m_session->imageWidth;
    const int h = m_session->imageHeight;
    if (w <= 0 || h <= 0) {
        error = tr("无法编辑掩膜：会话缺少图像尺寸（请先加载图像）");
        return false;
    }
    // 1) 内存显示掩膜缓存：尺寸==整图才可信（尺寸不符时 overlay 按整图拉伸会错位）
    const QImage cached = m_objectMasks.value(obj.id);
    if (!cached.isNull() && cached.width() == w && cached.height() == h) {
        outBinary = MaskOps::toBinaryMask(cached);
        return true;
    }
    // 2) maskRle（磁盘加载会话的像素来源）：期望尺寸传入解码器，
    // 不匹配在展开像素前快速失败（批2 复核二轮 P3）
    if (!obj.maskRle.isEmpty()) {
        bool ok = false;
        const QImage decoded = MaskOps::decodeRle(obj.maskRle, QSize(w, h), &ok);
        if (ok) {
            outBinary = decoded;
            return true;
        }
    }
    // 3) polygon 栅格化重建（无抗锯齿，确定性）
    if (obj.polygon.size() >= 3) {
        outBinary = MaskOps::rasterizePolygon(obj.polygon, w, h);
        return true;
    }
    // 4) bbox 矩形兜底（polygon<3 的退化对象）
    if (obj.bbox.isValid()) {
        const QRectF b = obj.bbox;
        const QList<QPointF> rect = {b.topLeft(), QPointF(b.right(), b.top()), b.bottomRight(),
                                     QPointF(b.left(), b.bottom())};
        outBinary = MaskOps::rasterizePolygon(rect, w, h);
        return true;
    }
    error = tr("无法编辑掩膜：对象缺少可用的掩膜/多边形/外接矩形");
    return false;
}

QStringList SamAnnotatorDialog::selectedObjectIds() const {
    QStringList ids;
    const QList<QListWidgetItem*> selected = m_objectList->selectedItems();
    if (selected.isEmpty())
        return ids;
    // 按标注列表顺序返回（与点击顺序无关，保证合并 a/b 语义确定）
    for (const AnnotationObject& obj : m_session->annotations) {
        for (const QListWidgetItem* item : selected) {
            if (item && item->data(Qt::UserRole).toString() == obj.id) {
                ids.append(obj.id);
                break;
            }
        }
    }
    return ids;
}

void SamAnnotatorDialog::updateMaskEditButtonStates() {
    const int n = selectedObjectIds().size();
    if (m_btnDilate)
        m_btnDilate->setEnabled(n == 1);
    if (m_btnErode)
        m_btnErode->setEnabled(n == 1);
    if (m_btnSplit)
        m_btnSplit->setEnabled(n == 1);
    if (m_btnMerge)
        m_btnMerge->setEnabled(n == 2);
}

void SamAnnotatorDialog::commitSingleObjectMaskEdit(const QString& targetId, const QImage& editedBinary,
                                                    const QString& commandText) {
    AnnotationObject* target = m_session->findById(targetId);
    if (!target) {
        setStatusText(tr("掩膜编辑失败：对象不存在"));
        return;
    }
    // 失败关闭：空结果需要显式删除意图（Delete）；多连通分量在单多边形对象模型
    // 下无法如实表达（导出器用 polygon，会静默丢像素），拒绝并提示
    if (MaskOps::foregroundCount(editedBinary) <= 0) {
        setStatusText(tr("编辑被拒绝：操作后区域为空（如需删除对象请用 Delete）"));
        return;
    }
    // 批3 复核二轮（P1）：只计数路径用早退计数（数到第 2 个分量即停），
    // 不再为判断"是否多于一个分量"构造全部分量的整图掩膜
    if (MaskOps::connectedComponentCount(editedBinary, 1) > 1) {
        setStatusText(tr("编辑被拒绝：结果含多个连通分量（对象模型为单多边形），请减小半径/笔画或先拆分"));
        return;
    }

    AnnotationObject updated = *target;
    updated.polygon = MaskOps::traceContour(editedBinary);
    updated.bbox = MaskOps::foregroundBoundingBox(editedBinary);
    updated.maskRle = MaskOps::encodeRle(editedBinary); // 旧 RLE 随编辑失效，重编码（sam_server 兼容）
    // id/label/score/prompts/modelName 保留：几何编辑不改变标注来源与置信度

    const MaskEditSnapshot before = currentSnapshot();
    MaskEditSnapshot after = before;
    for (AnnotationObject& obj : after.annotations) {
        if (obj.id == targetId) {
            obj = updated;
            break;
        }
    }
    after.displayMasks.insert(targetId, MaskOps::toDisplayMask(editedBinary));
    pushMaskEditCommand(commandText, before, after, targetId);
    setStatusText(tr("%1完成：面积 %2 px").arg(commandText).arg(MaskOps::foregroundCount(editedBinary)));
}

void SamAnnotatorDialog::onDilateSelected() {
    const QStringList ids = selectedObjectIds();
    if (ids.size() != 1) {
        setStatusText(tr("膨胀：请先在列表中选中一个对象"));
        return;
    }
    AnnotationObject* obj = m_session->findById(ids.first());
    if (!obj)
        return;
    QImage binary;
    QString error;
    if (!binaryMaskForObject(*obj, binary, error)) {
        setStatusText(error);
        return;
    }
    const int radius = m_maskRadiusSpin ? m_maskRadiusSpin->value() : 3;
    commitSingleObjectMaskEdit(ids.first(), MaskOps::dilate(binary, radius), tr("膨胀掩膜"));
}

void SamAnnotatorDialog::onErodeSelected() {
    const QStringList ids = selectedObjectIds();
    if (ids.size() != 1) {
        setStatusText(tr("腐蚀：请先在列表中选中一个对象"));
        return;
    }
    AnnotationObject* obj = m_session->findById(ids.first());
    if (!obj)
        return;
    QImage binary;
    QString error;
    if (!binaryMaskForObject(*obj, binary, error)) {
        setStatusText(error);
        return;
    }
    const int radius = m_maskRadiusSpin ? m_maskRadiusSpin->value() : 3;
    commitSingleObjectMaskEdit(ids.first(), MaskOps::erode(binary, radius), tr("腐蚀掩膜"));
}

void SamAnnotatorDialog::onEraseStroke(const QList<QPointF>& imagePoints) {
    if (m_toolMode != ToolMode::Eraser || imagePoints.isEmpty())
        return;
    const QStringList ids = selectedObjectIds();
    if (ids.size() != 1) {
        setStatusText(tr("橡皮擦：请先选中一个要擦除的对象"));
        return;
    }
    AnnotationObject* obj = m_session->findById(ids.first());
    if (!obj)
        return;
    QImage binary;
    QString error;
    if (!binaryMaskForObject(*obj, binary, error)) {
        setStatusText(error);
        return;
    }
    const double radius = m_maskRadiusSpin ? m_maskRadiusSpin->value() : 3.0;
    const QImage edited = MaskOps::eraseStroke(binary, imagePoints, radius);
    if (MaskOps::foregroundCount(edited) == MaskOps::foregroundCount(binary)) {
        setStatusText(tr("橡皮擦：笔画未命中区域，未产生编辑"));
        return; // 无操作不入撤销栈
    }
    commitSingleObjectMaskEdit(ids.first(), edited, tr("橡皮擦掩膜"));
}

void SamAnnotatorDialog::onMergeSelected() {
    const QStringList ids = selectedObjectIds();
    if (ids.size() != 2) {
        setStatusText(tr("合并：请恰好选中两个对象（Ctrl/Shift 多选）"));
        return;
    }
    AnnotationObject* a = m_session->findById(ids.at(0));
    AnnotationObject* b = m_session->findById(ids.at(1));
    if (!a || !b)
        return;
    QImage binA;
    QImage binB;
    QString error;
    if (!binaryMaskForObject(*a, binA, error) || !binaryMaskForObject(*b, binB, error)) {
        setStatusText(error);
        return;
    }
    if (MaskOps::foregroundCount(binA) <= 0 || MaskOps::foregroundCount(binB) <= 0) {
        setStatusText(tr("合并被拒绝：对象掩膜为空"));
        return;
    }
    QImage unionMask = binA; // binaryMaskForObject 保证与整图同尺寸
    for (int y = 0; y < unionMask.height(); ++y) {
        const uchar* src = binB.constScanLine(y);
        uchar* dst = unionMask.scanLine(y);
        for (int x = 0; x < unionMask.width(); ++x) {
            if (src[x])
                dst[x] = 255;
        }
    }
    if (MaskOps::connectedComponentCount(unionMask, 1) != 1) { // 批3 复核二轮（P1）：早退计数
        setStatusText(tr("合并被拒绝：两区域不连通，单多边形对象无法如实表达"));
        return;
    }

    AnnotationObject merged;
    merged.id = a->id; // 保留首个对象（列表序）的身份，第二个对象被移除
    merged.label = a->label;
    merged.prompts.pointsPos = a->prompts.pointsPos + b->prompts.pointsPos;
    merged.prompts.pointsNeg = a->prompts.pointsNeg + b->prompts.pointsNeg;
    merged.prompts.box = std::nullopt; // 合并后原框 prompt 来源不再真实，置空
    merged.polygon = MaskOps::traceContour(unionMask);
    merged.bbox = MaskOps::foregroundBoundingBox(unionMask);
    merged.maskRle = MaskOps::encodeRle(unionMask);
    merged.score = a->score; // 保留首对象来源置信度（合并区域非 SAM 重新预测）
    merged.modelName = a->modelName;

    const MaskEditSnapshot before = currentSnapshot();
    MaskEditSnapshot after = before;
    for (AnnotationObject& obj : after.annotations) {
        if (obj.id == a->id)
            obj = merged;
    }
    for (int i = after.annotations.size() - 1; i >= 0; --i) {
        if (after.annotations[i].id == b->id)
            after.annotations.removeAt(i);
    }
    after.displayMasks.remove(b->id);
    after.displayMasks.insert(merged.id, MaskOps::toDisplayMask(unionMask));
    pushMaskEditCommand(tr("合并标注对象"), before, after, merged.id);
    setStatusText(tr("已合并：面积 %1 px").arg(MaskOps::foregroundCount(unionMask)));
}

void SamAnnotatorDialog::onSplitSelected() {
    const QStringList ids = selectedObjectIds();
    if (ids.size() != 1) {
        setStatusText(tr("拆分：请先在列表中选中一个对象"));
        return;
    }
    const QString id = ids.first();
    int index = -1;
    for (int i = 0; i < m_session->annotations.size(); ++i) {
        if (m_session->annotations[i].id == id) {
            index = i;
            break;
        }
    }
    if (index < 0)
        return;
    const AnnotationObject original = m_session->annotations[index];
    QImage binary;
    QString error;
    if (!binaryMaskForObject(original, binary, error)) {
        setStatusText(error);
        return;
    }
    // 批3 复核二轮（P1）：先用早退计数限定分量数——connectedComponents 为每个
    // 分量分配整图掩膜，碎片化区域（如上万孤立像素）直接构造会耗尽内存
    constexpr int kMaxSplitComponents = 64; // 拆出 >64 个对象不是合理的标注操作
    const int componentCount = MaskOps::connectedComponentCount(binary, kMaxSplitComponents);
    if (componentCount < 2) {
        setStatusText(tr("拆分被拒绝：区域为单一连通分量，无可拆分"));
        return;
    }
    if (componentCount > kMaxSplitComponents) {
        setStatusText(
            tr("拆分被拒绝：连通分量数超过上限 %1（碎片化区域请先用形态学/橡皮擦整理）").arg(kMaxSplitComponents));
        return;
    }
    // 批3 复核三轮（P1-3）：物化前内存预算门禁——connectedComponents 为每个
    // 分量分配整图二值掩膜（1 字节/像素），派生对象显示掩膜为 ARGB32
    // （4 字节/像素），峰值按 分量数×5 字节/像素 估算；超预算拒绝，消除
    // "分量数合规但大图×分量数"的确定性内存峰值（4K×64 ≈ 530MB，
    // 6400 万像素图可接近 4GB）
    constexpr qint64 kMaxSplitMaterializeBytes = 256LL * 1024 * 1024;
    const qint64 estimateBytes = qint64(componentCount) * qint64(binary.width()) * qint64(binary.height()) * 5;
    if (estimateBytes > kMaxSplitMaterializeBytes) {
        setStatusText(tr("拆分被拒绝：预计物化内存约 %1 MB，超过预算 %2 MB（分量过多或图像过大）")
                          .arg(estimateBytes / (1024 * 1024))
                          .arg(kMaxSplitMaterializeBytes / (1024 * 1024)));
        return;
    }
    const QList<QImage> components = MaskOps::connectedComponents(binary);
    // 最大分量（像素数，平局取 raster 序首个）保留原对象身份，其余为派生对象
    int largest = 0;
    qint64 largestCount = -1;
    for (int i = 0; i < components.size(); ++i) {
        const qint64 count = MaskOps::foregroundCount(components[i]);
        if (count > largestCount) {
            largestCount = count;
            largest = i;
        }
    }
    QList<AnnotationObject> parts;
    QList<QImage> partDisplays;
    for (int i = 0; i < components.size(); ++i) {
        AnnotationObject part;
        if (i == largest) {
            part = original; // 保留 id/label/prompts/score/modelName
        } else {
            part.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            part.label = original.label;
            part.prompts = AnnotationPrompt{}; // 派生分量无 SAM prompt 来源
            part.score = 0.0;                  // 非 SAM 预测，不伪造置信度
            part.modelName = original.modelName;
        }
        part.polygon = MaskOps::traceContour(components[i]);
        part.bbox = MaskOps::foregroundBoundingBox(components[i]);
        part.maskRle = MaskOps::encodeRle(components[i]);
        parts.append(part);
        partDisplays.append(MaskOps::toDisplayMask(components[i]));
    }

    const MaskEditSnapshot before = currentSnapshot();
    MaskEditSnapshot after = before;
    after.annotations.removeAt(index);
    for (int i = parts.size() - 1; i >= 0; --i)
        after.annotations.insert(index, parts[i]);
    for (int i = 0; i < parts.size(); ++i)
        after.displayMasks.insert(parts[i].id, partDisplays[i]);
    pushMaskEditCommand(tr("拆分标注对象"), before, after, original.id);
    setStatusText(tr("已拆分为 %1 个对象").arg(parts.size()));
}

void SamAnnotatorDialog::onObjectSelectionChanged() {
    auto* item = m_objectList->currentItem();
    QString id;
    if (item) {
        id = item->data(Qt::UserRole).toString();
    }
    m_overlay->setSelectedId(id);
    updateMaskEditButtonStates();
}

void SamAnnotatorDialog::refreshObjectList(const QString& selectIdAfter) {
    m_objectList->blockSignals(true);
    m_objectList->clear();
    for (const AnnotationObject& obj : m_session->annotations) {
        auto* item = new QListWidgetItem(QStringLiteral("%1  [%2]").arg(obj.label, obj.id));
        item->setData(Qt::UserRole, obj.id);
        m_objectList->addItem(item);
    }
    m_objectList->blockSignals(false);
    // SAM 批2：编辑后恢复选中（setCurrentRow 会触发 currentItemChanged →
    // overlay 高亮与按钮可用态联动）
    if (!selectIdAfter.isEmpty()) {
        for (int row = 0; row < m_objectList->count(); ++row) {
            QListWidgetItem* item = m_objectList->item(row);
            if (item && item->data(Qt::UserRole).toString() == selectIdAfter) {
                m_objectList->setCurrentRow(row);
                break;
            }
        }
    }
    // blockSignals 抑制了重建期间的选择信号：显式同步按钮可用态防陈旧
    updateMaskEditButtonStates();
}

void SamAnnotatorDialog::addConfirmedObject(const AnnotationObject& obj, const QImage& maskImage) {
    m_session->annotations.append(obj);
    refreshObjectList();
    m_overlay->setAnnotations(m_session->annotations);
    if (!maskImage.isNull()) {
        m_objectMasks.insert(obj.id, maskImage);
        m_overlay->setObjectMask(obj.id, maskImage);
    }
}

void SamAnnotatorDialog::commitCurrentPromptAsObject() {
    if (!m_hasPrediction || m_previewPolygon.isEmpty()) {
        setStatusText(tr("请先完成一次 SAM 预测"));
        return;
    }

    AnnotationObject obj;
    obj.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    obj.label = currentCategoryLabel();
    obj.prompts.pointsPos = m_positivePoints;
    obj.prompts.pointsNeg = m_negativePoints;
    if (m_dragBox.isValid())
        obj.prompts.box = m_dragBox;
    obj.polygon = m_previewPolygon;
    obj.bbox = m_previewBbox;
    obj.score = m_previewScore;
    obj.maskRle = m_previewMaskRle;
    obj.modelName = m_samClient->modelName().isEmpty() ? QStringLiteral("sam") : m_samClient->modelName();

    const QImage maskForObj = m_previewMask;
    // Fix P1-8: 确认对象不清空撤销栈，允许撤销刚确认的对象
    m_undoStack->push(new LambdaUndoCommand(
        tr("确认标注对象"),
        [this, obj, maskForObj]() {
            m_session->annotations.append(obj);
            refreshObjectList();
            if (m_overlay) {
                m_overlay->setAnnotations(m_session->annotations);
                if (!maskForObj.isNull()) {
                    m_objectMasks.insert(obj.id, maskForObj);
                    m_overlay->setObjectMask(obj.id, maskForObj);
                }
            }
        },
        [this, obj]() {
            m_session->removeById(obj.id);
            m_objectMasks.remove(obj.id);
            if (m_overlay) {
                m_overlay->clearObjectMask(obj.id);
                m_overlay->setAnnotations(m_session->annotations);
            }
            refreshObjectList();
        }));
    clearCurrentPrompt();
}

void SamAnnotatorDialog::refreshPromptAfterEdit(bool triggerPrediction) {
    m_hasPrediction = false;
    if (m_confirmButton)
        m_confirmButton->setEnabled(false);
    m_previewPolygon.clear();
    m_previewBbox = QRectF();
    m_previewMaskRle.clear();
    m_previewScore = 0.0;
    m_previewMask = QImage();
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
    m_previewMask = QImage();
    m_previewScore = 0.0;
    m_hasPrediction = false;
    if (m_confirmButton)
        m_confirmButton->setEnabled(false);
    m_overlay->clearPromptPoints();
    m_overlay->clearPreview();
}

void SamAnnotatorDialog::resetAnnotationState() {
    // Fix P1-6 完整重置协议（批2 复核二轮 P1-1/P1-2 提取）：换图/重开会话必须
    // 清 prompt、掩膜缓存、overlay 状态、撤销栈与标注列表——快照式撤销命令是
    // 整表覆盖，任何残留都会把旧会话数据写回新会话（数据损坏）
    clearCurrentPrompt();
    m_objectMasks.clear();
    if (m_overlay) {
        for (const AnnotationObject& obj : m_session ? m_session->annotations : QList<AnnotationObject>{})
            m_overlay->clearObjectMask(obj.id);
        m_overlay->setAnnotations({});
        m_overlay->setSelectedId(QString());
    }
    if (m_undoStack)
        m_undoStack->clear();
    if (m_session) {
        m_session->annotations.clear();
        m_session->imagePath.clear();
    }
    refreshObjectList();
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

    if (!samEnvironmentReadyForUse()) {
        setStatusText(tr("SAM 环境未初始化，请点击初始化环境"));
        refreshSamControlState();
        return;
    }
    if (!samModelReadyForUse()) {
        setStatusText(tr("请先导入 SAM 权重"));
        refreshSamControlState();
        return;
    }

    setStatusText(tr("SAM 加载图像..."));
    if (m_samClient->state() == SamBackendClient::State::Ready) {
        m_samClient->setImage(m_backendImagePath);
    } else {
        if (m_samClient->state() == SamBackendClient::State::Error)
            m_samClient->stopServerProcess();
        m_samClient->startServerProcess();
    }
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
                                           const QString& maskRle, const QImage& maskImage) {
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
    m_previewMask = maskImage;
    m_hasPrediction = true;
    m_overlay->setPreviewPolygon(m_previewPolygon);
    if (m_previewBbox.isValid())
        m_overlay->setPreviewBox(m_previewBbox);
    if (!m_previewMask.isNull())
        m_overlay->setPreviewMask(m_previewMask);
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
    // 批3 复核三轮（P1-1）：新图绑定路径同步写入像素摘要——保存的会话文件
    // 自始携带摘要，打开/Agent 追加时才能校验图像未被替换
    m_session->imageDigest = AnnotationSession::computeImageDigest(m_currentImage);
    m_session->modelName = m_samClient->modelName().isEmpty() ? QStringLiteral("sam") : m_samClient->modelName();
}

bool SamAnnotatorDialog::saveAnnotationFile(const QString& filePath, QString* error) {
    if (m_session->annotations.isEmpty()) {
        const QString message = tr("当前没有标注对象，无需保存");
        if (error)
            *error = message;
        setStatusText(message);
        return false;
    }
    // 仅当内存图像与会话绑定路径一致时才允许回写会话元数据（含摘要补记）；
    // 防止"打开会话但图像缺失"时用陈旧内存图像覆盖已加载会话的字段
    if (m_session->imagePath.isEmpty() || m_session->imagePath == m_imagePath)
        updateSessionFromImage();
    QString saveError;
    if (!m_session->save(filePath, &saveError)) {
        if (error)
            *error = saveError;
        setStatusText(tr("保存失败：%1").arg(saveError));
        return false;
    }
    setStatusText(tr("标注会话已保存（%1 个对象）").arg(m_session->annotations.size()));
    Logger::instance().info(tr("标注会话已保存至 %1").arg(filePath), "Annotation");
    return true;
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
    // 批2 复核二轮（P1-2）：换图（内容变化）必须走完整重置协议——否则同尺寸
    // 异图时旧对象掩膜缓存仍满足"尺寸==整图"信任条件，编辑会用旧图像素推导
    // 新标注（静默错图）。同内容重开不清（保护进行中标注）。
    if (img != m_currentImage)
        resetAnnotationState();
    m_currentImage = img;
    m_imagePath = path;
    HImageWidget* target = activeImageWidget();
    if (target) {
        target->setImage(img);
        target->fitToWindow();
    }
    refreshOverlayCoordConverter();
    updateSessionFromImage();
    prepareBackendImage();
    emit imageLoaded(path);
}

void SamAnnotatorDialog::attachToImageWidget(HImageWidget* imageWidget, const QString& imagePath) {
    if (!imageWidget || !imageWidget->hasImage()) {
        setStatusText(tr("主视图没有可标注图像"));
        return;
    }

    if (m_externalImageWidget)
        m_externalImageWidget->disconnect(this);
    m_externalImageWidget = imageWidget;
    connect(imageWidget, &QObject::destroyed, this, [this, imageWidget]() {
        if (m_externalImageWidget == imageWidget)
            m_externalImageWidget = nullptr;
    });
    // Fix 2: 监听图像变化，重置标注会话（批2 复核二轮 P1-2：统一走
    // resetAnnotationState 协议，与 openImageFromFile/setImageSnapshot 一致）
    connect(imageWidget, &HImageWidget::imageLoaded, this, [this](const QImage& newImage) {
        if (m_externalImageWidget && m_externalImageWidget->currentImage() != m_currentImage) {
            m_currentImage = newImage;
            m_imagePath.clear();
            resetAnnotationState();
            if (m_session) {
                m_session->imageWidth = newImage.width();
                m_session->imageHeight = newImage.height();
            }
            if (m_samClient)
                m_samClient->unloadImage();
            refreshOverlayCoordConverter();
            prepareBackendImage();
        }
    });
    m_currentImage = imageWidget->currentImage();
    m_imagePath = imagePath;
    if (m_centerContainer) {
        m_centerContainer->setFixedSize(0, 0);
        m_centerContainer->hide();
    }
    if (m_openImageButton)
        m_openImageButton->hide();
    if (auto* openRowWidget = findChild<QWidget*>(QStringLiteral("SamOpenImageRowWidget")))
        openRowWidget->hide();
    if (m_objectList) {
        m_objectList->setMinimumHeight(72);
        // 不设 maximumHeight，让 Expanding 生效，对象列表拉伸填满可用空间
    }
    setMinimumSize(220, 520);
    resize(300, 530);
    moveOverlayToImageWidget(imageWidget);
    updateSessionFromImage();
    prepareBackendImage();
    emit imageLoaded(imagePath);
}

void SamAnnotatorDialog::onOpenImage() {
    openImageFromFile();
}

void SamAnnotatorDialog::setImageSnapshot(const QImage& image, const QString& imagePath) {
    // 批2 复核二轮（P1-2）：快照换图 = 新标注对象，复用完整重置协议；
    // 同内容快照（如对话框重开）不清，保护进行中标注
    if (!image.isNull() && image != m_currentImage)
        resetAnnotationState();
    m_currentImage = image;
    m_imagePath = imagePath;
    if (!image.isNull()) {
        HImageWidget* target = activeImageWidget();
        if (target) {
            target->setImage(image);
            target->fitToWindow();
        }
    }
    refreshOverlayCoordConverter();
    updateSessionFromImage();
    prepareBackendImage();
    emit imageLoaded(imagePath);
}

void SamAnnotatorDialog::onImportModel() {
    const QString path = QFileDialog::getOpenFileName(this, tr("导入 SAM 权重"), QString(),
                                                      tr("SAM 权重 (*.pth *.pt *.ckpt);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;

    m_samClient->setModelPath(path);
    const QString modelError = m_samClient->unsupportedModelReason();
    if (!modelError.isEmpty()) {
        m_samClient->setModelPath(QString());
        setStatusText(tr("SAM 错误：%1").arg(modelError));
        refreshSamControlState();
        return;
    }
    qputenv("DEEPLUX_SAM_MODEL", path.toLocal8Bit());
    saveModelPath(path);
    setStatusText(tr("已导入权重：%1").arg(QFileInfo(path).fileName()));
    refreshSamControlState();
    if (!m_backendImagePath.isEmpty()) {
        m_samClient->stopServerProcess();
        prepareBackendImage();
    }
}

void SamAnnotatorDialog::onInitializeEnvironment() {
    if (m_samClient->isEnvironmentInitializationRunning()) {
        m_samClient->cancelEnvironmentInitialization();
        return;
    }
    m_samClient->initializeManagedEnvironment();
}

void SamAnnotatorDialog::onRestartSam() {
    m_samClient->stopServerProcess();
    if (!m_backendImagePath.isEmpty()) {
        prepareBackendImage();
        return;
    }
    if (!samEnvironmentReadyForUse()) {
        setStatusText(tr("SAM 环境未初始化，请点击初始化环境"));
        return;
    }
    if (!samModelReadyForUse()) {
        setStatusText(tr("请先导入 SAM 权重"));
        return;
    }
    setStatusText(tr("SAM 启动中..."));
    m_samClient->startServerProcess();
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
    if (!saveAnnotationFile(path, &err))
        QMessageBox::warning(this, tr("保存失败"), err);
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
    // 批3 复核三轮（P1-1）：仅内存图像与会话绑定一致时才回写会话元数据，
    // 防止"打开会话但图像缺失"时陈旧图像覆盖已加载会话字段（含摘要）
    if (m_session->imagePath.isEmpty() || m_session->imagePath == m_imagePath)
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

// === 类别管理 ===

void SamAnnotatorDialog::initializeDefaultCategories() {
    m_categories.clear();
    m_categoryColors.clear();
    m_categories << QStringLiteral("缺陷") << QStringLiteral("划痕") << QStringLiteral("凹坑")
                 << QStringLiteral("正常");
    const QColor palette[] = {QColor(239, 68, 68), QColor(245, 158, 11), QColor(168, 85, 247), QColor(34, 197, 94)};
    for (int i = 0; i < m_categories.size(); ++i)
        m_categoryColors.insert(m_categories[i], palette[i % 4]);

    if (m_categoryList) {
        m_categoryList->blockSignals(true);
        m_categoryList->clear();
        for (const QString& name : m_categories) {
            auto* item = new QListWidgetItem(name);
            item->setData(Qt::UserRole, name);
            const QColor c = m_categoryColors.value(name);
            item->setForeground(c);
            m_categoryList->addItem(item);
        }
        m_categoryList->blockSignals(false);
    }
}

QStringList SamAnnotatorDialog::categories() const {
    return m_categories;
}

QString SamAnnotatorDialog::currentCategoryLabel() const {
    if (m_categoryList && m_categoryList->currentItem()) {
        const QString label = m_categoryList->currentItem()->data(Qt::UserRole).toString();
        if (!label.isEmpty())
            return label;
    }
    const QString typed = m_categoryEdit ? m_categoryEdit->text().trimmed() : QString();
    return typed.isEmpty() ? QStringLiteral("object") : typed;
}

QColor SamAnnotatorDialog::categoryColor(const QString& label) const {
    return m_categoryColors.value(label, QColor(6, 182, 212));
}

void SamAnnotatorDialog::addCategory(const QString& label) {
    const QString trimmed = label.trimmed();
    if (trimmed.isEmpty())
        return;
    if (m_categories.contains(trimmed))
        return;
    m_categories.append(trimmed);
    const QColor preset[] = {QColor(239, 68, 68),  QColor(245, 158, 11), QColor(168, 85, 247), QColor(34, 197, 94),
                             QColor(6, 182, 212),  QColor(234, 179, 8),  QColor(99, 102, 241), QColor(236, 72, 153),
                             QColor(20, 184, 166), QColor(249, 115, 22)};
    m_categoryColors.insert(trimmed, preset[(m_categories.size() - 1) % 10]);
    if (m_categoryList) {
        auto* item = new QListWidgetItem(trimmed);
        item->setData(Qt::UserRole, trimmed);
        item->setForeground(m_categoryColors.value(trimmed));
        m_categoryList->addItem(item);
    }
}

void SamAnnotatorDialog::removeCategory(const QString& label) {
    const int idx = m_categories.indexOf(label);
    if (idx < 0)
        return;
    m_categories.removeAt(idx);
    m_categoryColors.remove(label);
    if (m_categoryList) {
        for (int i = 0; i < m_categoryList->count(); ++i) {
            QListWidgetItem* item = m_categoryList->item(i);
            if (item && item->data(Qt::UserRole).toString() == label) {
                delete m_categoryList->takeItem(i);
                break;
            }
        }
    }
}

void SamAnnotatorDialog::onAddCategory() {
    const QString text = m_categoryEdit ? m_categoryEdit->text().trimmed() : QString();
    if (text.isEmpty()) {
        QMessageBox::information(this, tr("添加类别"), tr("请在类别输入框中填写名称"));
        return;
    }
    addCategory(text);
}

void SamAnnotatorDialog::onRemoveCategory() {
    if (!m_categoryList)
        return;
    auto* item = m_categoryList->currentItem();
    if (!item) {
        QMessageBox::information(this, tr("删除类别"), tr("请先在类别列表中选中要删除的项"));
        return;
    }
    removeCategory(item->data(Qt::UserRole).toString());
}

// === 会话重开 ===

void SamAnnotatorDialog::onOpenAnnotation() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开标注会话"), QString(), tr("DeepLux 标注 (*.deeplux-anno.json);;JSON (*.json);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString err;
    if (!openAnnotationFile(path, &err))
        QMessageBox::warning(this, tr("打开失败"), err);
}

bool SamAnnotatorDialog::openAnnotationFile(const QString& filePath, QString* error) {
    const auto failOpen = [this, error](const QString& message) {
        if (error)
            *error = message;
        setStatusText(message);
        return false;
    };
    QString loadError;
    const AnnotationSession loaded = AnnotationSession::load(filePath, &loadError);
    if (!loadError.isEmpty())
        return failOpen(tr("打开标注失败：%1").arg(loadError));

    // 批3 复核三轮（P1-1）：先校验后换态——校验失败时当前会话/撤销栈/overlay
    // 完全不动（旧实现先替换状态再加载图像，校验失败已污染对话框）
    QImage sessionImage;
    if (!loaded.imagePath.isEmpty() && QFileInfo::exists(loaded.imagePath))
        sessionImage = QImage(loaded.imagePath);
    bool legacyUnverified = false;
    if (!loaded.annotations.isEmpty()) {
        // 批3 复核四轮（P1-1）：非空会话必须有可读的绑定图像——图像缺失/不可读
        // 时若仍换入会话，标注会被显示与编辑在陈旧的内存图像上（静默错位）
        if (loaded.imagePath.isEmpty() || sessionImage.isNull())
            return failOpen(tr("打开标注失败：会话绑定图像不可读（%1），拒绝在无关图像上显示标注")
                                .arg(loaded.imagePath.isEmpty() ? tr("未记录路径") : loaded.imagePath));
        // 尺寸先行核对（legacy 无摘要会话至少还有尺寸可查）
        if (loaded.imageWidth > 0 && loaded.imageHeight > 0 &&
            (loaded.imageWidth != sessionImage.width() || loaded.imageHeight != sessionImage.height()))
            return failOpen(tr("打开标注失败：会话尺寸 %1x%2 与绑定图像 %3x%4 不符")
                                .arg(loaded.imageWidth)
                                .arg(loaded.imageHeight)
                                .arg(sessionImage.width())
                                .arg(sessionImage.height()));
        if (!loaded.imageDigest.isEmpty()) {
            const QString computed = AnnotationSession::computeImageDigest(sessionImage);
            if (computed != loaded.imageDigest)
                return failOpen(tr("打开标注失败：会话图像摘要与原路径文件内容不符（图像可能已被替换），拒绝打开"));
        } else {
            // 旧版无摘要会话：尺寸已核对，内容无法校验。不拒绝打开（历史数据
            // 不应被锁死，错位由用户在叠加层可见），但不在打开时静默补摘要——
            // 用户核对后走保存路径，由保存以内存图像补记摘要
            legacyUnverified = true;
        }
    } else if (!sessionImage.isNull() && !loaded.imageDigest.isEmpty()) {
        // 空会话带摘要：同口径校验（防止把会话绑到已被替换的图像上继续标注）
        const QString computed = AnnotationSession::computeImageDigest(sessionImage);
        if (computed != loaded.imageDigest)
            return failOpen(tr("打开标注失败：会话图像摘要与原路径文件内容不符（图像可能已被替换），拒绝打开"));
    }

    // 批2 复核二轮（P1-1）：会话重开必须清旧撤销命令——批2 快照命令为整表
    // 覆盖，残留命令会在 Ctrl+Z 时用旧会话数据整体替换新会话（数据损坏）
    if (m_undoStack)
        m_undoStack->clear();
    clearCurrentPrompt();
    // 清理旧会话 id 的 overlay 对象掩膜缓存与预览
    if (m_overlay) {
        if (m_session) {
            for (const AnnotationObject& obj : m_session->annotations)
                m_overlay->clearObjectMask(obj.id);
        }
        m_overlay->clearPreview();
        m_overlay->setSelectedId(QString());
    }
    m_objectMasks.clear();

    // 替换当前会话
    delete m_session;
    m_session = new AnnotationSession(loaded);

    if (!sessionImage.isNull()) {
        m_currentImage = sessionImage;
        m_imagePath = loaded.imagePath;
        HImageWidget* target = activeImageWidget();
        if (target) {
            target->setImage(sessionImage);
            target->fitToWindow();
        }
        refreshOverlayCoordConverter();
    }
    // 批3 复核三轮（P1-1）：不再调用 updateSessionFromImage()——已加载会话的
    // imagePath/尺寸/modelName/摘要以文件为准；图像缺失时严禁用陈旧的内存
    // 图像覆盖会话绑定字段（那会把会话静默重绑到无关图像）

    refreshObjectList();
    if (m_overlay)
        m_overlay->setAnnotations(m_session->annotations);
    prepareBackendImage();
    if (legacyUnverified)
        setStatusText(tr("已打开标注会话（%1 个对象）——警告：旧版会话无图像摘要，无法校验图像是否被替换，"
                         "请核对标注对齐后再保存")
                          .arg(m_session->annotations.size()));
    else
        setStatusText(tr("已打开标注会话（%1 个对象）").arg(m_session->annotations.size()));
    Logger::instance().info(tr("已打开标注会话 %1").arg(filePath), "Annotation");
    return true;
}

// === YOLO Seg 导出 ===

void SamAnnotatorDialog::onExportYoloSeg() {
    if (m_session->annotations.isEmpty()) {
        QMessageBox::information(this, tr("导出"), tr("当前没有标注对象，无需导出"));
        return;
    }
    const QString defaultPath = m_imagePath.isEmpty()
                                    ? QStringLiteral("labels.txt")
                                    : QFileInfo(m_imagePath).absolutePath() + QStringLiteral("/labels.txt");
    const QString path =
        QFileDialog::getSaveFileName(this, tr("导出 YOLO Seg"), defaultPath, tr("文本 (*.txt);;所有文件 (*.*)"));
    if (path.isEmpty())
        return;
    QString err;
    // 批3 复核三轮（P1-1）：同 LabelMe 导出的一致性守卫
    if (m_session->imagePath.isEmpty() || m_session->imagePath == m_imagePath)
        updateSessionFromImage();
    if (!YoloSegExporter::exportToFile(*m_session, path, m_categories, &err)) {
        QMessageBox::warning(this, tr("导出失败"), err);
        return;
    }
    Logger::instance().info(tr("YOLO Seg 已导出至 %1").arg(path), "Annotation");
}

} // namespace DeepLux
