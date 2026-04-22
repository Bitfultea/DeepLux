#pragma once

#include <QObject>
#include <QMap>
#include <QString>
#include <QVector>
#include <QWidget>

namespace DeepLux {

class DisplayData;
class ViewportWidget;
class IDisplayPort;

/**
 * @brief DisplayManager - Manages viewports and routes display data
 *
 * Owned by MainWindow (dependency injection - NOT a singleton).
 * Manages the lifecycle of viewports and routes DisplayData to them.
 *
 * Key responsibilities:
 * - Create/destroy viewports
 * - Route DisplayData to appropriate viewport based on:
 *   1. viewport ID stored in DisplayData
 *   2. preferredViewport() from IDisplayPort
 *   3. Default: first available viewport
 */
class DisplayManager : public QObject
{
    Q_OBJECT

public:
    explicit DisplayManager(QObject* parent = nullptr);
    ~DisplayManager() override;

    // Viewport management
    QString createViewport(const QString& title = QString());
    void destroyViewport(const QString& id);
    ViewportWidget* viewport(const QString& id) const;
    QVector<ViewportWidget*> allViewports() const;

    // Display data routing (delayMs = 0 means display immediately)
    void displayData(const DisplayData& data, int delayMs = 0);
    void displayData(IDisplayPort* source, int delayMs = 0);

    // Connect to a specific viewport
    void displayToViewport(const DisplayData& data, const QString& viewportId, int delayMs = 0);

    // Access the central display widget containing all viewports
    QWidget* centralDisplay() const { return m_centralWidget; }

    // Clear all displays
    void clearAll();

    // Get number of viewports
    int viewportCount() const { return m_viewports.size(); }

    // Apply theme to all viewports
    void applyTheme(bool isDark);

signals:
    void viewportCreated(const QString& viewportId, ViewportWidget* viewport);
    void viewportDestroyed(const QString& viewportId);
    void dataDisplayed(const QString& viewportId);

private:
    QString generateViewportId();
    ViewportWidget* findAvailableViewport(const QString& preferredId) const;

    QMap<QString, ViewportWidget*> m_viewports;
    QWidget* m_centralWidget;  // Container widget with layout
    int m_viewportCounter = 0;
};

} // namespace DeepLux