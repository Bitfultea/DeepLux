#include "Viewport2DContent.h"
#include "widgets/HImageWidget.h"
#include <QVBoxLayout>

namespace DeepLux {

Viewport2DContent::Viewport2DContent(QWidget* parent)
    : QWidget(parent)
    , m_zoomFactor(1.0)
{
    // 创建 HImageWidget 并布局
    m_imageWidget = new HImageWidget(this);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_imageWidget);
    setLayout(layout);
}

Viewport2DContent::~Viewport2DContent() {
}

void Viewport2DContent::displayData(const DisplayData& data) {
    const auto* imgData = data.imageData();
    if (!imgData) {
        clearDisplay();
        return;
    }

    QImage image = imgData->toQImage();
    if (!image.isNull()) {
        m_imageWidget->setImage(image);
        emit imageDisplayed();
    }
}

void Viewport2DContent::clearDisplay() {
    m_imageWidget->clearImage();
}

void Viewport2DContent::setFitWindow(bool fit) {
    if (fit) {
        m_imageWidget->fitToWindow();
    } else {
        m_imageWidget->actualSize();
    }
}

void Viewport2DContent::setZoomFactor(qreal factor) {
    m_zoomFactor = factor;
    m_imageWidget->setZoom(factor);
}

} // namespace DeepLux
