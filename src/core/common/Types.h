#pragma once

#include <QString>
#include <QPointF>
#include <QRectF>

namespace DeepLux {

/**
 * @brief 点结构
 */
struct Point {
    double x = 0.0;
    double y = 0.0;
    
    Point() = default;
    Point(double x_, double y_) : x(x_), y(y_) {}
    
    QPointF toQPointF() const { return QPointF(x, y); }
    static Point fromQPointF(const QPointF& p) { return Point(p.x(), p.y()); }
};

/**
 * @brief 矩形结构
 */
struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    
    Rect() = default;
    Rect(double x_, double y_, double w_, double h_) 
        : x(x_), y(y_), width(w_), height(h_) {}
    
    QRectF toQRectF() const { return QRectF(x, y, width, height); }
    static Rect fromQRectF(const QRectF& r) { 
        return Rect(r.x(), r.y(), r.width(), r.height()); 
    }
    
    bool contains(const Point& p) const {
        return p.x >= x && p.x <= x + width && 
               p.y >= y && p.y <= y + height;
    }
};

/**
 * @brief 圆结构
 */
struct Circle {
    double centerX = 0.0;
    double centerY = 0.0;
    double radius = 0.0;
    
    Circle() = default;
    Circle(double cx, double cy, double r) 
        : centerX(cx), centerY(cy), radius(r) {}
};

/**
 * @brief 线结构
 */
struct Line {
    Point start;
    Point end;
    
    Line() = default;
    Line(const Point& s, const Point& e) : start(s), end(e) {}
    Line(double x1, double y1, double x2, double y2) 
        : start(x1, y1), end(x2, y2) {}
    
    double length() const {
        double dx = end.x - start.x;
        double dy = end.y - start.y;
        return std::sqrt(dx * dx + dy * dy);
    }
};

} // namespace DeepLux
