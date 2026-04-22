#include "DisplayManager.h"
#include "../../core/display/DisplayData.h"
#include "../../core/display/IDisplayPort.h"
#include "../widgets/ViewportWidget.h"

#include <QVBoxLayout>
#include <QTimer>
#include <QDebug>

namespace DeepLux {

DisplayManager::DisplayManager(QObject* parent)
    : QObject(parent)
    , m_centralWidget(new QWidget())
{
    // Central widget uses a vertical layout for stacked viewports
    QVBoxLayout* layout = new QVBoxLayout(m_centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_centralWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_centralWidget->setMinimumSize(0, 0);

    // Create default viewport
    createViewport(tr("主视图"));
}

DisplayManager::~DisplayManager()
{
    // Delete all viewports
    qDeleteAll(m_viewports);
    m_viewports.clear();

    // Delete central widget
    delete m_centralWidget;
}

QString DisplayManager::createViewport(const QString& title)
{
    QString id = generateViewportId();
    QString viewportTitle = title.isEmpty() ? QString("Viewport %1").arg(m_viewportCounter) : title;

    ViewportWidget* viewport = new ViewportWidget(id);
    viewport->setTitle(viewportTitle);
    viewport->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    viewport->setMinimumSize(0, 0);

    // Add to internal map
    m_viewports[id] = viewport;

    // Add to central widget layout
    if (QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(m_centralWidget->layout())) {
        layout->addWidget(viewport);
    }

    emit viewportCreated(id, viewport);
    return id;
}

void DisplayManager::destroyViewport(const QString& id)
{
    if (!m_viewports.contains(id)) {
        return;
    }

    ViewportWidget* viewport = m_viewports.take(id);

    // Remove from layout
    if (QVBoxLayout* layout = qobject_cast<QVBoxLayout*>(m_centralWidget->layout())) {
        layout->removeWidget(viewport);
    }

    // Delete viewport
    delete viewport;

    emit viewportDestroyed(id);
}

ViewportWidget* DisplayManager::viewport(const QString& id) const
{
    return m_viewports.value(id, nullptr);
}

QVector<ViewportWidget*> DisplayManager::allViewports() const
{
    return m_viewports.values().toVector();
}

void DisplayManager::displayData(const DisplayData& data, int delayMs)
{
    QString targetId = data.viewportId();

    // Find appropriate viewport
    ViewportWidget* target = findAvailableViewport(targetId);
    if (!target) {
        qWarning() << "DisplayManager: No available viewport for display data";
        return;
    }

    // Display the data (with optional delay)
    if (const ImageData* img = data.imageData()) {
        QImage image = img->toQImage();
        if (delayMs > 0) {
            QTimer::singleShot(delayMs, target, [target, image]() {
                target->displayImage(image);
            });
        } else {
            target->displayImage(image);
        }
        emit dataDisplayed(target->viewportId());
    }
}

void DisplayManager::displayData(IDisplayPort* source, int delayMs)
{
    if (!source || !source->hasDisplayOutput()) {
        return;
    }

    DisplayData data = source->getDisplayData();

    // Check if source has its own delay in metadata
    int sourceDelay = data.metadata().value("delay").toInt();
    if (sourceDelay > 0) {
        delayMs = sourceDelay;
    }

    QString preferredId = source->preferredViewport();

    if (!preferredId.isEmpty() && m_viewports.contains(preferredId)) {
        displayToViewport(data, preferredId, delayMs);
    } else {
        displayData(data, delayMs);
    }
}

void DisplayManager::displayToViewport(const DisplayData& data, const QString& viewportId, int delayMs)
{
    ViewportWidget* target = m_viewports.value(viewportId, nullptr);
    if (!target) {
        qWarning() << "DisplayManager: Viewport not found:" << viewportId;
        return;
    }

    if (const ImageData* img = data.imageData()) {
        QImage image = img->toQImage();
        if (delayMs > 0) {
            QTimer::singleShot(delayMs, target, [target, image]() {
                target->displayImage(image);
            });
        } else {
            target->displayImage(image);
        }
        emit dataDisplayed(viewportId);
    }
}

void DisplayManager::clearAll()
{
    for (ViewportWidget* viewport : m_viewports) {
        viewport->clearDisplay();
    }
}

QString DisplayManager::generateViewportId()
{
    return QString("viewport_%1").arg(++m_viewportCounter);
}

ViewportWidget* DisplayManager::findAvailableViewport(const QString& preferredId) const
{
    // If preferred ID exists, use it
    if (!preferredId.isEmpty() && m_viewports.contains(preferredId)) {
        return m_viewports[preferredId];
    }

    // Otherwise, return first available
    if (!m_viewports.isEmpty()) {
        return m_viewports.values().first();
    }

    return nullptr;
}

void DisplayManager::applyTheme(bool isDark)
{
    for (ViewportWidget* viewport : m_viewports) {
        if (viewport) {
            viewport->applyTheme(isDark);
        }
    }
}

} // namespace DeepLux