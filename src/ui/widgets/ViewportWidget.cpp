#include "ViewportWidget.h"

#include "AppIconProvider.h"
#include "HImageWidget.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPalette>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

#include "core/display/DisplayData.h"
#include "ui/display/3d/Viewport3DContent.h"

namespace DeepLux {

ViewportWidget::ViewportWidget(const QString& id, QWidget* parent)
    : QFrame(parent), m_viewportId(id), m_title(tr("Viewport")), m_imageWidget(nullptr), m_3dContent(nullptr),
      m_toolbar(nullptr), m_displayMode(DisplayMode::Auto2D) {
    setFrameStyle(QFrame::NoFrame);
    setupUi();
    createActions();
    applyTheme(false); // 默认浅色主题，MainWindow 会在后续调 applyTheme(m_isDarkTheme)
}

ViewportWidget::~ViewportWidget() {}

void ViewportWidget::setupUi() {
    // Main layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Title bar container — 独立 widget 以支持整行背景和底边框
    QWidget* titleBarWidget = new QWidget();
    titleBarWidget->setObjectName("ViewportTitleBar");
    QHBoxLayout* titleLayout = new QHBoxLayout(titleBarWidget);
    titleLayout->setContentsMargins(10, 4, 6, 4);
    titleLayout->setSpacing(4);

    m_titleBar = new QLabel(m_title);
    m_titleBar->setObjectName("ViewportTitle");

    titleLayout->addWidget(m_titleBar);
    m_contentInfoLabel = new QLabel();
    m_contentInfoLabel->setObjectName("ViewportContentInfo");
    titleLayout->addWidget(m_contentInfoLabel);
    titleLayout->addStretch();

    m_toolbar = new QToolBar(titleBarWidget);
    m_toolbar->setObjectName("ViewportToolBar");
    m_toolbar->setIconSize(QSize(18, 18));
    m_toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    m_zoomOutAction =
        new QAction(AppIconProvider::icon(AppIconProvider::Icon::ZoomOut, 18, QColor("#6B7280")), tr("缩小"), this);
    m_zoomOutAction->setObjectName("ViewportZoomOutAction");
    m_zoomOutAction->setToolTip(tr("缩小"));
    m_toolbar->addAction(m_zoomOutAction);
    connect(m_zoomOutAction, &QAction::triggered, this, &ViewportWidget::onZoomOut);

    m_zoomInAction =
        new QAction(AppIconProvider::icon(AppIconProvider::Icon::ZoomIn, 18, QColor("#6B7280")), tr("放大"), this);
    m_zoomInAction->setObjectName("ViewportZoomInAction");
    m_zoomInAction->setToolTip(tr("放大"));
    m_toolbar->addAction(m_zoomInAction);
    connect(m_zoomInAction, &QAction::triggered, this, &ViewportWidget::onZoomIn);

    m_zoomLabel = new QLabel(tr("100%"));
    m_zoomLabel->setObjectName("ViewportZoomLabel");
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->setMinimumWidth(52);
    m_toolbar->addWidget(m_zoomLabel);

    m_fitWindowAction =
        new QAction(AppIconProvider::icon(AppIconProvider::Icon::FitWindow, 18, QColor("#6B7280")), tr("适应"), this);
    m_fitWindowAction->setObjectName("ViewportFitAction");
    m_fitWindowAction->setToolTip(tr("适应窗口"));
    m_toolbar->addAction(m_fitWindowAction);
    connect(m_fitWindowAction, &QAction::triggered, this, &ViewportWidget::onFitWindow);

    m_actualSizeAction = new QAction(AppIconProvider::icon(AppIconProvider::Icon::ActualSize, 18, QColor("#6B7280")),
                                     tr("实际大小"), this);
    m_actualSizeAction->setObjectName("ViewportActualSizeAction");
    m_actualSizeAction->setToolTip(tr("实际像素大小 (1:1)"));
    m_toolbar->addAction(m_actualSizeAction);
    connect(m_actualSizeAction, &QAction::triggered, this, &ViewportWidget::onActualSize);

    // 2D/3D 切换
    m_toggleViewAction = new QAction(tr("3D"), this);
    m_toggleViewAction->setObjectName("ViewportToggleViewAction");
    m_toggleViewAction->setToolTip(tr("切换到 3D 视图"));
    m_toggleViewAction->setVisible(false);
    m_toolbar->addAction(m_toggleViewAction);
    connect(m_toggleViewAction, &QAction::triggered, this, &ViewportWidget::onToggleView);

    m_contentSeparatorAction = m_toolbar->addSeparator();

    m_snapshotAction =
        new QAction(AppIconProvider::icon(AppIconProvider::Icon::Camera, 18, QColor("#0891B2")), tr("保存快照"), this);
    m_snapshotAction->setObjectName("ViewportSnapshotAction");
    m_snapshotAction->setToolTip(tr("保存当前视图快照"));
    m_toolbar->addAction(m_snapshotAction);
    connect(m_snapshotAction, &QAction::triggered, this, &ViewportWidget::onSaveSnapshot);

    m_closeAction =
        new QAction(AppIconProvider::icon(AppIconProvider::Icon::Close, 18, QColor("#6B7280")), tr("关闭"), this);
    m_closeAction->setObjectName("ViewportCloseAction");
    m_closeAction->setToolTip(tr("关闭"));
    m_toolbar->addAction(m_closeAction);
    connect(m_closeAction, &QAction::triggered, this, &ViewportWidget::onCloseClicked);

    titleLayout->addWidget(m_toolbar);

    // Add title bar to main layout
    mainLayout->addWidget(titleBarWidget);
    mainLayout->setStretch(0, 0);
    // HImageWidget for image display (2D mode)
    m_imageWidget = new HImageWidget();
    m_imageWidget->setStyleSheet(R"(
        background-color: #1a1a1a;
        border: none;
    )");
    mainLayout->addWidget(m_imageWidget);
    mainLayout->setStretch(1, 1);

    // Forward 2D image clicks for coordinate picking
    connect(m_imageWidget, &HImageWidget::imageClicked, this, &ViewportWidget::point2DClicked);
    connect(m_imageWidget, &HImageWidget::zoomChanged, this, &ViewportWidget::updateZoomLabel);
    updateToolbarState();
}

void ViewportWidget::createActions() {
    // Actions are created in setupUi() and connections made there
}

void ViewportWidget::setTitle(const QString& title) {
    if (m_title != title) {
        m_title = title;
        m_titleBar->setText(title);
        emit titleChanged(m_viewportId, title);
    }
}

void ViewportWidget::displayData(const DisplayData& data) {
    if (data.pointCloudData() && !data.pointCloudData()->isEmpty()) {
        m_cachedCloudData = data;
        m_hasCachedCloud = true;
        switchTo3D();
        if (m_3dContent) {
            m_3dContent->displayData(data);
        }
        m_contentInfoLabel->setText(tr("%1 点").arg(data.pointCloudData()->size()));
        QString dsName = data.metadata().value("dataSourceName").toString();
        if (!dsName.isEmpty()) {
            setTitle(dsName);
        }
    } else if (data.imageData()) {
        m_cachedImage = data.imageData()->toQImage();
        m_hasCachedImage = true;
        switchTo2D();
        m_imageWidget->setImage(m_cachedImage);
        m_contentInfoLabel->setText(tr("%1 × %2").arg(m_cachedImage.width()).arg(m_cachedImage.height()));
        emit imageDisplayed();
        QString dsName = data.metadata().value("dataSourceName").toString();
        if (!dsName.isEmpty()) {
            setTitle(dsName);
        }
    }
    updateToggleAction();
}

QImage ViewportWidget::currentImage() const {
    if (m_hasCachedImage)
        return m_cachedImage;
    return m_imageWidget ? m_imageWidget->currentImage() : QImage();
}

void ViewportWidget::displayImage(const QImage& image) {
    m_cachedImage = image;
    m_hasCachedImage = true;
    switchTo2D();
    m_imageWidget->setImage(image);
    m_contentInfoLabel->setText(tr("%1 × %2").arg(image.width()).arg(image.height()));
    emit imageDisplayed();
    updateToggleAction();
}

void ViewportWidget::setDualData(const QImage& image, const DisplayData& cloudData) {
    m_cachedImage = image;
    m_hasCachedImage = true;
    m_cachedCloudData = cloudData;
    m_hasCachedCloud = true;
    // 默认显示 3D
    switchTo3D();
    if (m_3dContent) {
        m_3dContent->displayData(cloudData);
    }
    if (cloudData.pointCloudData()) {
        m_contentInfoLabel->setText(tr("%1 点").arg(cloudData.pointCloudData()->size()));
    }
    QString dsName = cloudData.metadata().value("dataSourceName").toString();
    if (!dsName.isEmpty()) {
        setTitle(dsName);
    }
    updateToggleAction();
}

void ViewportWidget::onToggleView() {
    if (m_displayMode == DisplayMode::Auto3D && m_hasCachedImage) {
        switchTo2D();
        m_imageWidget->setImage(m_cachedImage);
        emit imageDisplayed();
    } else if (m_displayMode == DisplayMode::Auto2D && m_hasCachedCloud) {
        switchTo3D();
        if (m_3dContent) {
            m_3dContent->displayData(m_cachedCloudData);
        }
    }
    updateToggleAction();
}

void ViewportWidget::updateToggleAction() {
    if (m_toggleViewAction) {
        m_toggleViewAction->setVisible(m_hasCachedImage && m_hasCachedCloud);
        if (m_displayMode == DisplayMode::Auto3D) {
            m_toggleViewAction->setText(QStringLiteral("2D"));
            m_toggleViewAction->setToolTip(QStringLiteral("切换到 2D 视图"));
        } else {
            m_toggleViewAction->setText(QStringLiteral("3D"));
            m_toggleViewAction->setToolTip(QStringLiteral("切换到 3D 视图"));
        }
    }
    updateToolbarState();
}

void ViewportWidget::clearDisplay() {
    if (m_3dContent) {
        m_3dContent->clearDisplay();
    }
    m_imageWidget->clearImage();
    m_cachedImage = QImage();
    m_cachedCloudData = DisplayData();
    m_hasCachedImage = false;
    m_hasCachedCloud = false;
    m_contentInfoLabel->clear();
    updateZoomLabel(1.0);
    updateToggleAction();
}

void ViewportWidget::ensure3DContent() {
    if (m_3dContent)
        return;

    m_3dContent = new Viewport3DContent(this);
    m_3dContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_3dContent->setMinimumSize(100, 100);

    connect(m_3dContent, &Viewport3DContent::point3DClicked, this, &ViewportWidget::point3DClicked);

    // 高: 通知外部安装事件过滤器
    emit contentWidgetCreated(m_3dContent);

    // Replace in layout
    QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(this->layout());
    if (layout && m_imageWidget) {
        int index = layout->indexOf(m_imageWidget);
        if (index >= 0) {
            layout->removeWidget(m_imageWidget);
            m_imageWidget->hide();
            layout->insertWidget(index, m_3dContent);
            m_3dContent->show();
        }
    }
}

void ViewportWidget::switchTo2D() {
    if (m_displayMode == DisplayMode::Auto2D && m_imageWidget->isVisible()) {
        return; // Already in 2D mode
    }

    m_displayMode = DisplayMode::Auto2D;

    if (m_3dContent) {
        m_3dContent->hide();
    }
    m_imageWidget->show();
    if (!m_cachedImage.isNull()) {
        m_contentInfoLabel->setText(tr("%1 × %2").arg(m_cachedImage.width()).arg(m_cachedImage.height()));
    }

    updateToolbarState();
}

void ViewportWidget::switchTo3D() {
    if (m_displayMode == DisplayMode::Auto3D && m_3dContent && m_3dContent->isVisible()) {
        return; // Already in 3D mode
    }

    ensure3DContent();

    m_displayMode = DisplayMode::Auto3D;

    // Switch content
    m_imageWidget->hide();
    if (m_3dContent) {
        m_3dContent->show();
    }
    if (m_cachedCloudData.pointCloudData()) {
        m_contentInfoLabel->setText(tr("%1 点").arg(m_cachedCloudData.pointCloudData()->size()));
    }

    updateToolbarState();
}

void ViewportWidget::updateToolbarState() {
    const bool imageMode = m_displayMode == DisplayMode::Auto2D;
    const bool canZoom = imageMode && m_hasCachedImage;

    m_zoomOutAction->setVisible(imageMode);
    m_zoomInAction->setVisible(imageMode);
    m_fitWindowAction->setVisible(imageMode);
    m_actualSizeAction->setVisible(imageMode);
    m_zoomLabel->setVisible(imageMode);

    m_zoomOutAction->setEnabled(canZoom);
    m_zoomInAction->setEnabled(canZoom);
    m_fitWindowAction->setEnabled(canZoom);
    m_actualSizeAction->setEnabled(canZoom);
    m_contentSeparatorAction->setVisible(imageMode || m_toggleViewAction->isVisible());
    m_snapshotAction->setEnabled(m_hasCachedImage || m_hasCachedCloud);
}

void ViewportWidget::zoomIn() {
    if (m_displayMode == DisplayMode::Auto3D)
        return;
    double currentZoom = m_imageWidget->zoom();
    m_imageWidget->setZoom(currentZoom * 1.25);
}

void ViewportWidget::zoomOut() {
    if (m_displayMode == DisplayMode::Auto3D)
        return;
    double currentZoom = m_imageWidget->zoom();
    m_imageWidget->setZoom(currentZoom / 1.25);
}

void ViewportWidget::fitToWindow() {
    if (m_displayMode == DisplayMode::Auto3D)
        return;
    m_imageWidget->fitToWindow();
}

void ViewportWidget::actualSize() {
    if (m_displayMode == DisplayMode::Auto3D)
        return;
    m_imageWidget->actualSize();
}

void ViewportWidget::onCloseClicked() {
    emit viewportClosed(m_viewportId);
}

void ViewportWidget::onZoomIn() {
    zoomIn();
}

void ViewportWidget::onZoomOut() {
    zoomOut();
}

void ViewportWidget::onFitWindow() {
    fitToWindow();
}

void ViewportWidget::onActualSize() {
    actualSize();
}

void ViewportWidget::updateZoomLabel(double factor) {
    m_zoomLabel->setText(tr("%1%").arg(qRound(factor * 100.0)));
}

void ViewportWidget::onSaveSnapshot() {
    QImage snapshot;
    if (m_displayMode == DisplayMode::Auto3D && m_3dContent && m_3dContent->isValid()) {
        snapshot = m_3dContent->grabFramebuffer();
    } else if (m_imageWidget && m_imageWidget->hasImage()) {
        snapshot = m_imageWidget->grab().toImage();
    }
    if (snapshot.isNull()) {
        return;
    }

    const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString defaultPath =
        QDir(picturesDir)
            .filePath(QStringLiteral("DeepLux_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    QString filePath = QFileDialog::getSaveFileName(this, tr("保存视图快照"), defaultPath, tr("PNG 图像 (*.png)"));
    if (filePath.isEmpty()) {
        return;
    }
    if (!filePath.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        filePath += QStringLiteral(".png");
    }
    if (!snapshot.save(filePath, "PNG")) {
        QMessageBox::warning(this, tr("保存视图快照"), tr("无法保存到：%1").arg(filePath));
    }
}

void ViewportWidget::setRenderMode(int mode) {
    if (m_3dContent) {
        m_3dContent->setRenderMode(static_cast<ColorMode>(mode));
    }
}

int ViewportWidget::renderMode() const {
    return m_3dContent ? static_cast<int>(m_3dContent->renderMode()) : 5;
}

void ViewportWidget::setPickMode(bool enabled) {
    if (m_3dContent) {
        m_3dContent->setPickMode(enabled);
    }
}

void ViewportWidget::applyTheme(bool isDark) {
    const QColor toolColor = isDark ? QColor("#D1D5DB") : QColor("#4B5563");
    m_zoomOutAction->setIcon(AppIconProvider::icon(AppIconProvider::Icon::ZoomOut, 18, toolColor));
    m_zoomInAction->setIcon(AppIconProvider::icon(AppIconProvider::Icon::ZoomIn, 18, toolColor));
    m_fitWindowAction->setIcon(AppIconProvider::icon(AppIconProvider::Icon::FitWindow, 18, toolColor));
    m_actualSizeAction->setIcon(AppIconProvider::icon(AppIconProvider::Icon::ActualSize, 18, toolColor));
    m_closeAction->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 18, toolColor));

    QPalette imagePalette = m_imageWidget->palette();
    imagePalette.setColor(QPalette::Window, isDark ? QColor("#1a1a1a") : QColor("#ffffff"));
    m_imageWidget->setPalette(imagePalette);

    if (isDark) {
        setStyleSheet(R"(
            ViewportWidget {
                background-color: #1e1e1e;
                border: none;
            }
            QWidget#ViewportTitleBar {
                background-color: #2d2d2d;
                border: none;
                border-bottom: 1px solid #444444;
            }
        )");
        m_titleBar->setStyleSheet(R"(
            QLabel {
                color: #ffffff;
                font-size: 14px;
                font-weight: 600;
                background-color: transparent;
            }
        )");
        m_imageWidget->setStyleSheet(R"(
            background-color: #1e1e1e;
            border: none;
        )");
    } else {
        setStyleSheet(R"(
            ViewportWidget {
                background-color: #ffffff;
                border: none;
            }
            QWidget#ViewportTitleBar {
                background-color: #e8e8e8;
                border: none;
                border-bottom: 1px solid #cccccc;
            }
        )");
        m_titleBar->setStyleSheet(R"(
            QLabel {
                color: #212121;
                font-size: 14px;
                font-weight: 600;
                background-color: transparent;
            }
        )");
        m_imageWidget->setStyleSheet(R"(
            background-color: #ffffff;
            border: none;
        )");
    }

    if (m_3dContent) {
        m_3dContent->applyTheme(isDark);
    }
}

} // namespace DeepLux
