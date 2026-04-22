#pragma once

#include <QWidget>
#include <QImage>
#include <QPointF>
#include <QTransform>

namespace DeepLux {

enum class RoiType {
    None,
    Rectangle1,
    Rectangle2,
    Circle,
    Line,
    Point
};

struct RoiData {
    RoiType type = RoiType::None;
    QString name;
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
};

class HImageWidget : public QWidget
{
    Q_OBJECT

public:
    explicit HImageWidget(QWidget* parent = nullptr);
    ~HImageWidget() override;

    void setImage(const QImage& image);
    void clearImage();
    bool hasImage() const { return m_hasImage; }
    
    int imageWidth() const { return m_imageWidth; }
    int imageHeight() const { return m_imageHeight; }

    void setZoom(double factor);
    double zoom() const { return m_zoom; }
    void fitToWindow();
    void actualSize();
    
    void setRoiMode(RoiType type);
    RoiType roiMode() const { return m_roiMode; }
    void clearRois();
    QList<RoiData> rois() const { return m_rois; }

    QPointF widgetToImage(const QPointF& widgetPoint) const;
    QPointF imageToWidget(const QPointF& imagePoint) const;

signals:
    void imageLoaded(const QImage& image);
    void zoomChanged(double factor);
    void roiCreated(const RoiData& roi);
    void mouseMoved(const QPointF& imagePoint);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void updateTransform();

    QImage m_image;
    bool m_hasImage = false;
    int m_imageWidth = 0;
    int m_imageHeight = 0;
    
    double m_zoom = 1.0;
    QPointF m_offset;
    QTransform m_transform;
    QTransform m_inverseTransform;
    
    RoiType m_roiMode = RoiType::None;
    QList<RoiData> m_rois;
    
    bool m_isDrawing = false;
    QPointF m_drawStart;
    QPointF m_drawEnd;
    
    bool m_isPanning = false;
    QPointF m_panStart;
};

} // namespace DeepLux
