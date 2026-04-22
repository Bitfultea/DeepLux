#pragma once

#include "IViewportContent.h"
#include <QWidget>
#include <QLabel>
#include <QImage>

namespace DeepLux {

class HImageWidget;

/**
 * @brief 2D 视口内容 Widget
 *
 * 包装现有的 HImageWidget 实现 IViewportContent 接口
 */
class Viewport2DContent : public QWidget, public IViewportContent {
    Q_OBJECT
public:
    explicit Viewport2DContent(QWidget* parent = nullptr);
    ~Viewport2DContent() override;

    // IViewportContent
    void displayData(const DisplayData& data) override;
    void clearDisplay() override;
    QWidget* toolbarExtension() override { return nullptr; }
    QWidget* widget() override { return this; }

    // 2D 特定方法
    void setFitWindow(bool fit);
    void setZoomFactor(qreal factor);
    qreal zoomFactor() const { return m_zoomFactor; }

signals:
    void imageDisplayed();

private:
    HImageWidget* m_imageWidget;  // 复用现有组件
    qreal m_zoomFactor = 1.0;
};

} // namespace DeepLux
