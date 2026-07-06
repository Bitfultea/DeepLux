#include "ViewportWidget.h"
#include "HImageWidget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QToolButton>
#include <QCloseEvent>
#include <QApplication>
#include <QPalette>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

#include "core/display/DisplayData.h"
#include "ui/display/3d/Viewport3DContent.h"

namespace DeepLux {

ViewportWidget::ViewportWidget(const QString& id, QWidget* parent)
    : QFrame(parent)
    , m_viewportId(id)
    , m_title(tr("Viewport"))
    , m_imageWidget(nullptr)
    , m_3dContent(nullptr)
    , m_toolbar(nullptr)
    , m_3dToolbar(nullptr)
    , m_displayMode(DisplayMode::Auto2D)
{
    setFrameStyle(QFrame::NoFrame);
    setupUi();
    createActions();
    applyTheme(false);  // 默认浅色主题，MainWindow 会在后续调 applyTheme(m_isDarkTheme)
}

ViewportWidget::~ViewportWidget()
{
}

void ViewportWidget::setupUi()
{
    // Main layout
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Title bar
    QHBoxLayout* titleLayout = new QHBoxLayout();
    titleLayout->setContentsMargins(8, 3, 8, 3);
    titleLayout->setSpacing(4);

    m_titleBar = new QLabel(m_title);
    m_titleBar->setStyleSheet(R"(
        QLabel {
            color: #e0e0e0;
            font-size: 13px;
            font-weight: 600;
            background-color: transparent;
        }
    )");
    m_titleBar->setObjectName("ViewportTitle");

    // Spacer
    titleLayout->addWidget(m_titleBar);
    titleLayout->addStretch();

    // Toolbar with actions
    m_toolbar = new QToolBar();
    m_toolbar->setStyleSheet(R"(
        QToolBar {
            background-color: transparent;
            border: none;
            spacing: 4px;
        }
        QToolButton {
            background-color: transparent;
            border: none;
            padding: 2px;
            color: #a0a0a0;
            font-size: 13px;
        }
        QToolButton:hover {
            background-color: #3a3a3a;
            border-radius: 2px;
        }
    )");
    m_toolbar->setIconSize(QSize(16, 16));

    // Fit window action
    m_fitWindowAction = new QAction(tr("适应"), this);
    m_fitWindowAction->setToolTip(tr("适应窗口"));
    m_toolbar->addAction(m_fitWindowAction);
    connect(m_fitWindowAction, &QAction::triggered, this, &ViewportWidget::onFitWindow);

    // Actual size action
    m_actualSizeAction = new QAction(tr("1:1"), this);
    m_actualSizeAction->setToolTip(tr("实际像素大小"));
    m_toolbar->addAction(m_actualSizeAction);
    connect(m_actualSizeAction, &QAction::triggered, this, &ViewportWidget::onActualSize);

    m_toolbar->addSeparator();

    // Zoom in action
    m_zoomInAction = new QAction(tr("+"), this);
    m_zoomInAction->setToolTip(tr("放大"));
    m_toolbar->addAction(m_zoomInAction);
    connect(m_zoomInAction, &QAction::triggered, this, &ViewportWidget::onZoomIn);

    // Zoom out action
    m_zoomOutAction = new QAction(tr("-"), this);
    m_zoomOutAction->setToolTip(tr("缩小"));
    m_toolbar->addAction(m_zoomOutAction);
    connect(m_zoomOutAction, &QAction::triggered, this, &ViewportWidget::onZoomOut);

    m_toolbar->addSeparator();

    // Close action
    m_closeAction = new QAction(tr("×"), this);
    m_closeAction->setToolTip(tr("关闭"));
    m_toolbar->addAction(m_closeAction);
    connect(m_closeAction, &QAction::triggered, this, &ViewportWidget::onCloseClicked);

    titleLayout->addWidget(m_toolbar);

    // Add title bar to main layout
    mainLayout->addLayout(titleLayout);

    // HImageWidget for image display (2D mode)
    m_imageWidget = new HImageWidget();
    m_imageWidget->setStyleSheet(R"(
        background-color: #1a1a1a;
        border: none;
    )");
    mainLayout->addWidget(m_imageWidget);

    // Forward 2D image clicks for coordinate picking
    connect(m_imageWidget, &HImageWidget::imageClicked, this, &ViewportWidget::point2DClicked);

    // 3D toolbar (hidden by default)
    m_3dToolbar = new QToolBar();
    m_3dToolbar->setStyleSheet(R"(
        QToolBar {
            background-color: transparent;
            border: none;
            spacing: 4px;
        }
        QToolButton {
            background-color: transparent;
            border: none;
            padding: 2px;
            color: #a0a0a0;
            font-size: 13px;
        }
        QToolButton:hover {
            background-color: #3a3a3a;
            border-radius: 2px;
        }
    )");
    m_3dToolbar->setIconSize(QSize(16, 16));
    m_3dToolbar->setVisible(false);

    mainLayout->addWidget(m_3dToolbar);
}

void ViewportWidget::createActions()
{
    // Actions are created in setupUi() and connections made there
}

void ViewportWidget::setTitle(const QString& title)
{
    if (m_title != title) {
        m_title = title;
        m_titleBar->setText(title);
        emit titleChanged(m_viewportId, title);
    }
}

void ViewportWidget::displayData(const DisplayData& data)
{
    // Route based on data type
    if (data.pointCloudData() && !data.pointCloudData()->isEmpty()) {
        switchTo3D();
        if (m_3dContent) {
            m_3dContent->displayData(data);
        }
        // 如果有数据源名称，更新视口标题
        QString dsName = data.metadata().value("dataSourceName").toString();
        if (!dsName.isEmpty()) {
            setTitle(dsName);
        }
    } else if (data.imageData()) {
        // 2D image data
        switchTo2D();
        m_imageWidget->setImage(data.imageData()->toQImage());
        emit imageDisplayed();
        // 如果有数据源名称，更新视口标题
        QString dsName = data.metadata().value("dataSourceName").toString();
        if (!dsName.isEmpty()) {
            setTitle(dsName);
        }
    }
}

void ViewportWidget::displayImage(const QImage& image)
{
    switchTo2D();
    m_imageWidget->setImage(image);
    emit imageDisplayed();
}

void ViewportWidget::clearDisplay()
{
    if (m_3dContent) {
        m_3dContent->clearDisplay();
    }
    m_imageWidget->clearImage();
}

void ViewportWidget::ensure3DContent()
{
    if (m_3dContent) return;

    // Create 3D content widget — 必须指定 parent 否则 QOpenGLWidget 无法创建 context
    m_3dContent = new Viewport3DContent(this);
    m_3dContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_3dContent->setMinimumSize(100, 100);

    // Forward 3D point pick for coordinate picking
    connect(m_3dContent, &Viewport3DContent::point3DClicked, this, &ViewportWidget::point3DClicked);

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

void ViewportWidget::switchTo2D()
{
    if (m_displayMode == DisplayMode::Auto2D && m_imageWidget->isVisible()) {
        return;  // Already in 2D mode
    }

    m_displayMode = DisplayMode::Auto2D;

    if (m_3dContent) {
        m_3dContent->hide();
    }
    m_imageWidget->show();

    // Switch toolbar
    m_toolbar->setVisible(true);
    m_3dToolbar->setVisible(false);
}

void ViewportWidget::switchTo3D()
{
    if (m_displayMode == DisplayMode::Auto3D && m_3dContent && m_3dContent->isVisible()) {
        return;  // Already in 3D mode
    }

    ensure3DContent();

    m_displayMode = DisplayMode::Auto3D;

    // Switch content
    m_imageWidget->hide();
    if (m_3dContent) {
        m_3dContent->show();
    }

    // Switch toolbar
    m_toolbar->setVisible(false);
}

void ViewportWidget::zoomIn()
{
    if (m_displayMode == DisplayMode::Auto3D) return;
    double currentZoom = m_imageWidget->zoom();
    m_imageWidget->setZoom(currentZoom * 1.25);
}

void ViewportWidget::zoomOut()
{
    if (m_displayMode == DisplayMode::Auto3D) return;
    double currentZoom = m_imageWidget->zoom();
    m_imageWidget->setZoom(currentZoom / 1.25);
}

void ViewportWidget::fitToWindow()
{
    if (m_displayMode == DisplayMode::Auto3D) return;
    m_imageWidget->fitToWindow();
}

void ViewportWidget::actualSize()
{
    if (m_displayMode == DisplayMode::Auto3D) return;
    m_imageWidget->actualSize();
}

void ViewportWidget::onCloseClicked()
{
    emit viewportClosed(m_viewportId);
}

void ViewportWidget::onZoomIn()
{
    zoomIn();
}

void ViewportWidget::onZoomOut()
{
    zoomOut();
}

void ViewportWidget::onFitWindow()
{
    fitToWindow();
}

void ViewportWidget::onActualSize()
{
    actualSize();
}

void ViewportWidget::setRenderMode(int mode)
{
    if (m_3dContent) {
        m_3dContent->setRenderMode(static_cast<ColorMode>(mode));
    }
}

int ViewportWidget::renderMode() const
{
    return m_3dContent ? static_cast<int>(m_3dContent->renderMode()) : 5;
}

void ViewportWidget::applyTheme(bool isDark)
{
    QPalette imagePalette = m_imageWidget->palette();
    imagePalette.setColor(QPalette::Window, isDark ? QColor("#1a1a1a") : QColor("#ffffff"));
    m_imageWidget->setPalette(imagePalette);

    if (isDark) {
        setStyleSheet(R"(
            ViewportWidget {
                background-color: #2b2b2b;
                border: 1px solid #3a3a3a;
            }
        )");
        m_titleBar->setStyleSheet(R"(
            QLabel {
                color: #e0e0e0;
                font-size: 13px;
                font-weight: 600;
                background-color: transparent;
            }
        )");
        m_imageWidget->setStyleSheet(R"(
            background-color: #1a1a1a;
            border: none;
        )");
    } else {
        setStyleSheet(R"(
            ViewportWidget {
                background-color: #f5f5f5;
                border: 1px solid #d0d0d0;
            }
        )");
        m_titleBar->setStyleSheet(R"(
            QLabel {
                color: #212121;
                font-size: 13px;
                font-weight: 600;
                background-color: transparent;
            }
        )");
        m_imageWidget->setStyleSheet(R"(
            background-color: #ffffff;
            border: none;
        )");
    }

    // Update toolbar styles
    QString toolbarStyle = isDark ? R"(
        QToolBar {
            background-color: transparent;
            border: none;
            spacing: 4px;
        }
        QToolButton {
            background-color: transparent;
            border: none;
            padding: 2px;
            color: #f3f4f6;
            font-size: 13px;
        }
        QToolButton:hover {
            background-color: #4b5563;
            border-radius: 2px;
        }
        QToolButton:pressed {
            background-color: #6b7280;
            border-radius: 2px;
        }
    )" : R"(
        QToolBar {
            background-color: transparent;
            border: none;
            spacing: 4px;
        }
        QToolButton {
            background-color: transparent;
            border: none;
            padding: 2px;
            color: #606060;
            font-size: 13px;
        }
        QToolButton:hover {
            background-color: #e0e0e0;
            border-radius: 2px;
        }
    )";
    m_toolbar->setStyleSheet(toolbarStyle);
    m_3dToolbar->setStyleSheet(toolbarStyle);
    if (m_3dContent) {
        m_3dContent->applyTheme(isDark);
    }
}

} // namespace DeepLux
