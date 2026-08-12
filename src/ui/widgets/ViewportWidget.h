#pragma once

#include "core/display/DisplayData.h"

#include <QAction>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QPointer>
#include <QString>
#include <QToolBar>
#include <QVector3D>

namespace DeepLux {

class HImageWidget;
class Viewport3DContent;
class DisplayData;

/**
 * @brief ViewportWidget - Container for display viewport
 *
 * A QFrame-based container that supports both 2D and 3D content:
 * - Title bar with viewport name and close button
 * - HImageWidget for 2D display
 * - Viewport3DContent for 3D display (created on demand)
 * - Basic controls (zoom, pan mode, ROI mode for 2D)
 *
 * Display mode is automatically determined by the data type:
 * - ImageData → 2D mode (HImageWidget)
 * - PointCloudData → 3D mode (Viewport3DContent)
 */
class ViewportWidget : public QFrame {
    Q_OBJECT

public:
    explicit ViewportWidget(const QString& id, QWidget* parent = nullptr);
    ~ViewportWidget() override;

    // Viewport identity
    QString viewportId() const {
        return m_viewportId;
    }
    QString title() const {
        return m_title;
    }
    void setTitle(const QString& title);

    // Access the underlying widgets
    HImageWidget* imageWidget() const {
        return m_imageWidget;
    }
    Viewport3DContent* viewport3D() const {
        return m_3dContent;
    }
    QImage currentImage() const;

    // Display unified data (auto-routes to appropriate renderer)
    void displayData(const DisplayData& data);

    // Display image to this viewport (2D mode)
    void displayImage(const QImage& image);

    // Clear the display
    void clearDisplay();

    // Current display mode
    enum class DisplayMode { Auto2D, Auto3D };
    DisplayMode displayMode() const {
        return m_displayMode;
    }

    // Zoom controls (2D only)
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void actualSize();

    // Apply theme
    void applyTheme(bool isDark);

    // 3D 渲染模式
    void setRenderMode(int mode);
    int renderMode() const;

    // 3D 拾取模式（左键直接拾取，不需要 Ctrl）
    void setPickMode(bool enabled);

    // 设置 2D/3D 双数据（用于 TIFF 等 depth-map 数据）
    void setDualData(const QImage& image, const DisplayData& cloudData, DisplayMode initialMode = DisplayMode::Auto3D);

signals:
    void viewportClosed(const QString& viewportId);
    void titleChanged(const QString& viewportId, const QString& title);
    void imageDisplayed();
    void point2DClicked(const QPointF& point);
    void point3DClicked(const QVector3D& point);
    void contentWidgetCreated(QWidget* widget); // 高: 延迟创建的子控件通知

private slots:
    void onCloseClicked();
    void onZoomIn();
    void onZoomOut();
    void onFitWindow();
    void onActualSize();
    void onToggleView();
    void onSaveSnapshot();
    void updateZoomLabel(double factor);

private:
    void setupUi();
    void createActions();
    void ensure3DContent();
    void switchTo2D();
    void switchTo3D();
    void updateToggleAction();
    void updateToolbarState();

    QString m_viewportId;
    QString m_title;

    QLabel* m_titleBar;
    QLabel* m_contentInfoLabel = nullptr;
    QLabel* m_zoomLabel = nullptr;
    QAction* m_zoomLabelAction = nullptr;
    HImageWidget* m_imageWidget;
    Viewport3DContent* m_3dContent;
    QToolBar* m_toolbar;

    QAction* m_fitWindowAction;
    QAction* m_actualSizeAction;
    QAction* m_zoomInAction;
    QAction* m_zoomOutAction;
    QDoubleSpinBox* m_pointSizeSpinBox = nullptr;
    QAction* m_pointSizeAction = nullptr;
    QAction* m_contentSeparatorAction = nullptr;
    QAction* m_snapshotAction = nullptr;
    QAction* m_closeAction;
    QAction* m_toggleViewAction = nullptr;

    DisplayMode m_displayMode = DisplayMode::Auto2D;
    bool m_isDarkTheme = true;

    // 2D/3D 双数据缓存（用于切换）
    QImage m_cachedImage;
    bool m_hasCachedImage = false;
    DisplayData m_cachedCloudData;
    bool m_hasCachedCloud = false;
};

} // namespace DeepLux
