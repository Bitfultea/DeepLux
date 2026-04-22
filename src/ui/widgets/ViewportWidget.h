#pragma once

#include <QFrame>
#include <QString>
#include <QLabel>
#include <QToolBar>
#include <QAction>
#include <QPointer>

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
class ViewportWidget : public QFrame
{
    Q_OBJECT

public:
    explicit ViewportWidget(const QString& id, QWidget* parent = nullptr);
    ~ViewportWidget() override;

    // Viewport identity
    QString viewportId() const { return m_viewportId; }
    QString title() const { return m_title; }
    void setTitle(const QString& title);

    // Access the underlying widgets
    HImageWidget* imageWidget() const { return m_imageWidget; }
    Viewport3DContent* viewport3D() const { return m_3dContent; }

    // Display unified data (auto-routes to appropriate renderer)
    void displayData(const DisplayData& data);

    // Display image to this viewport (2D mode)
    void displayImage(const QImage& image);

    // Clear the display
    void clearDisplay();

    // Current display mode
    enum class DisplayMode { Auto2D, Auto3D };
    DisplayMode displayMode() const { return m_displayMode; }

    // Zoom controls (2D only)
    void zoomIn();
    void zoomOut();
    void fitToWindow();
    void actualSize();

    // Apply theme
    void applyTheme(bool isDark);

signals:
    void viewportClosed(const QString& viewportId);
    void titleChanged(const QString& viewportId, const QString& title);
    void imageDisplayed();

private slots:
    void onCloseClicked();
    void onZoomIn();
    void onZoomOut();
    void onFitWindow();
    void onActualSize();
    void onResetCamera3D();

private:
    void setupUi();
    void createActions();
    void ensure3DContent();
    void switchTo2D();
    void switchTo3D();

    QString m_viewportId;
    QString m_title;

    QLabel* m_titleBar;
    HImageWidget* m_imageWidget;  // Always created (2D default)
    Viewport3DContent* m_3dContent;  // Created on first 3D display request
    QToolBar* m_toolbar;
    QToolBar* m_3dToolbar;  // 3D-specific toolbar

    QAction* m_fitWindowAction;
    QAction* m_actualSizeAction;
    QAction* m_zoomInAction;
    QAction* m_zoomOutAction;
    QAction* m_closeAction;
    QAction* m_resetCameraAction;  // 3D reset camera

    DisplayMode m_displayMode = DisplayMode::Auto2D;
};

} // namespace DeepLux