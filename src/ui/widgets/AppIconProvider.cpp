#include "AppIconProvider.h"

#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <cmath>

namespace DeepLux {
namespace {

QPointF pt(qreal x, qreal y, qreal scale) {
    return QPointF(x * scale, y * scale);
}

QRectF rect(qreal x, qreal y, qreal w, qreal h, qreal scale) {
    return QRectF(x * scale, y * scale, w * scale, h * scale);
}

void line(QPainter& painter, qreal scale, qreal x1, qreal y1, qreal x2, qreal y2) {
    painter.drawLine(pt(x1, y1, scale), pt(x2, y2, scale));
}

void chevron(QPainter& painter, qreal scale, qreal x1, qreal y1, qreal x2, qreal y2, qreal x3, qreal y3) {
    QPainterPath path;
    path.moveTo(pt(x1, y1, scale));
    path.lineTo(pt(x2, y2, scale));
    path.lineTo(pt(x3, y3, scale));
    painter.drawPath(path);
}

void roundedRect(QPainter& painter, qreal scale, qreal x, qreal y, qreal w, qreal h, qreal radius) {
    painter.drawRoundedRect(rect(x, y, w, h, scale), radius * scale, radius * scale);
}

void document(QPainter& painter, qreal scale) {
    QPainterPath path;
    path.moveTo(pt(6, 4, scale));
    path.lineTo(pt(14, 4, scale));
    path.lineTo(pt(18, 8, scale));
    path.lineTo(pt(18, 20, scale));
    path.lineTo(pt(6, 20, scale));
    path.closeSubpath();
    painter.drawPath(path);
    line(painter, scale, 14, 4, 14, 8);
    line(painter, scale, 14, 8, 18, 8);
}

void folder(QPainter& painter, qreal scale) {
    QPainterPath path;
    path.moveTo(pt(4, 8, scale));
    path.lineTo(pt(9, 8, scale));
    path.lineTo(pt(11, 10, scale));
    path.lineTo(pt(20, 10, scale));
    path.lineTo(pt(20, 19, scale));
    path.lineTo(pt(4, 19, scale));
    path.closeSubpath();
    painter.drawPath(path);
}

void camera(QPainter& painter, qreal scale) {
    roundedRect(painter, scale, 4, 8, 16, 10, 2.5);
    line(painter, scale, 8, 8, 9.5, 5.5);
    line(painter, scale, 9.5, 5.5, 14.5, 5.5);
    line(painter, scale, 14.5, 5.5, 16, 8);
    painter.drawEllipse(rect(9, 10, 6, 6, scale));
}

void chip(QPainter& painter, qreal scale) {
    roundedRect(painter, scale, 7, 7, 10, 10, 2);
    for (int y : {8, 12, 16}) {
        line(painter, scale, 4, y, 7, y);
        line(painter, scale, 17, y, 20, y);
    }
    for (int x : {8, 12, 16}) {
        line(painter, scale, x, 4, x, 7);
        line(painter, scale, x, 17, x, 20);
    }
}

void nodes(QPainter& painter, qreal scale) {
    painter.drawEllipse(rect(5, 6, 5, 5, scale));
    painter.drawEllipse(rect(14, 6, 5, 5, scale));
    painter.drawEllipse(rect(9.5, 15, 5, 5, scale));
    line(painter, scale, 10, 9, 14, 9);
    line(painter, scale, 8.5, 11, 11, 15);
    line(painter, scale, 15.5, 11, 13, 15);
}

void circularArrow(QPainter& painter, qreal scale, bool reverse = false) {
    const QRectF arcRect = rect(5, 5, 14, 14, scale);
    painter.drawArc(arcRect, reverse ? 35 * 16 : 205 * 16, 270 * 16);
    if (reverse) {
        chevron(painter, scale, 7.5, 7, 5, 6.5, 5.5, 9);
    } else {
        chevron(painter, scale, 17, 17, 19, 17.5, 18.5, 15);
    }
}

void cross(QPainter& painter, qreal scale, qreal cx, qreal cy) {
    line(painter, scale, cx - 4, cy, cx + 4, cy);
    line(painter, scale, cx, cy - 4, cx, cy + 4);
}

void drawIcon(QPainter& painter, AppIconProvider::Icon icon, qreal scale) {
    switch (icon) {
    case AppIconProvider::Icon::NewFile:
        document(painter, scale);
        cross(painter, scale, 11, 14);
        break;
    case AppIconProvider::Icon::OpenFolder:
    case AppIconProvider::Icon::Folder:
        folder(painter, scale);
        break;
    case AppIconProvider::Icon::Save:
        roundedRect(painter, scale, 5, 5, 14, 14, 2);
        line(painter, scale, 8, 5, 8, 10);
        line(painter, scale, 8, 10, 16, 10);
        roundedRect(painter, scale, 8, 14, 8, 5, 1);
        break;
    case AppIconProvider::Icon::List:
        for (qreal y : {7.0, 12.0, 17.0}) {
            line(painter, scale, 5, y, 6, y);
            line(painter, scale, 9, y, 19, y);
        }
        break;
    case AppIconProvider::Icon::Play:
    case AppIconProvider::Icon::Execute: {
        QPolygonF triangle;
        triangle << pt(8, 6, scale) << pt(8, 18, scale) << pt(18, 12, scale);
        painter.drawPolygon(triangle);
        break;
    }
    case AppIconProvider::Icon::Pause:
        roundedRect(painter, scale, 7, 6, 3, 12, 1);
        roundedRect(painter, scale, 14, 6, 3, 12, 1);
        break;
    case AppIconProvider::Icon::Stop:
        roundedRect(painter, scale, 7, 7, 10, 10, 2);
        break;
    case AppIconProvider::Icon::Cycle:
    case AppIconProvider::Icon::Refresh:
        circularArrow(painter, scale);
        break;
    case AppIconProvider::Icon::QuickMode: {
        QPainterPath path;
        path.moveTo(pt(13, 3.5, scale));
        path.lineTo(pt(7, 13, scale));
        path.lineTo(pt(12, 13, scale));
        path.lineTo(pt(10, 20.5, scale));
        path.lineTo(pt(17, 10, scale));
        path.lineTo(pt(12, 10, scale));
        painter.drawPath(path);
        break;
    }
    case AppIconProvider::Icon::User:
        painter.drawEllipse(rect(9, 5, 6, 6, scale));
        painter.drawArc(rect(5, 12, 14, 9, scale), 20 * 16, 140 * 16);
        break;
    case AppIconProvider::Icon::Variable:
        line(painter, scale, 6, 7, 18, 17);
        line(painter, scale, 18, 7, 6, 17);
        break;
    case AppIconProvider::Icon::Camera:
    case AppIconProvider::Icon::LoadImage:
    case AppIconProvider::Icon::Image:
        camera(painter, scale);
        break;
    case AppIconProvider::Icon::Communication:
        nodes(painter, scale);
        break;
    case AppIconProvider::Icon::Hardware:
        chip(painter, scale);
        break;
    case AppIconProvider::Icon::Report:
        document(painter, scale);
        line(painter, scale, 8, 11, 16, 11);
        line(painter, scale, 8, 15, 16, 15);
        break;
    case AppIconProvider::Icon::Home:
        chevron(painter, scale, 4, 11, 12, 5, 20, 11);
        roundedRect(painter, scale, 7, 11, 10, 8, 1.5);
        line(painter, scale, 11, 19, 11, 15);
        line(painter, scale, 13, 19, 13, 15);
        break;
    case AppIconProvider::Icon::Design:
        roundedRect(painter, scale, 5, 5, 14, 14, 2.5);
        line(painter, scale, 5, 10, 19, 10);
        line(painter, scale, 10, 5, 10, 19);
        break;
    case AppIconProvider::Icon::Laser:
        line(painter, scale, 5, 12, 19, 12);
        line(painter, scale, 4, 9, 7, 12);
        line(painter, scale, 4, 15, 7, 12);
        cross(painter, scale, 17, 12);
        break;
    case AppIconProvider::Icon::Theme:
        painter.drawEllipse(rect(6, 6, 10, 10, scale));
        painter.drawArc(rect(9, 4, 10, 12, scale), 80 * 16, 210 * 16);
        break;
    case AppIconProvider::Icon::Add:
        cross(painter, scale, 12, 12);
        break;
    case AppIconProvider::Icon::Delete:
        line(painter, scale, 8, 8, 16, 8);
        line(painter, scale, 10, 8, 10, 5.5);
        line(painter, scale, 14, 8, 14, 5.5);
        roundedRect(painter, scale, 8, 10, 8, 9, 1);
        line(painter, scale, 10.5, 12, 10.5, 17);
        line(painter, scale, 13.5, 12, 13.5, 17);
        break;
    case AppIconProvider::Icon::Connect:
    case AppIconProvider::Icon::TestConnection:
        nodes(painter, scale);
        line(painter, scale, 17, 17, 21, 17);
        line(painter, scale, 19, 15, 19, 19);
        break;
    case AppIconProvider::Icon::Disconnect:
        nodes(painter, scale);
        line(painter, scale, 16, 16, 21, 21);
        line(painter, scale, 21, 16, 16, 21);
        break;
    case AppIconProvider::Icon::MoveUp:
        chevron(painter, scale, 6, 14, 12, 8, 18, 14);
        line(painter, scale, 12, 8, 12, 19);
        break;
    case AppIconProvider::Icon::MoveDown:
        chevron(painter, scale, 6, 10, 12, 16, 18, 10);
        line(painter, scale, 12, 5, 12, 16);
        break;
    case AppIconProvider::Icon::Clear:
        roundedRect(painter, scale, 5, 8, 14, 9, 2);
        line(painter, scale, 8, 8, 12, 4);
        line(painter, scale, 12, 4, 19, 11);
        break;
    case AppIconProvider::Icon::Undo:
        circularArrow(painter, scale, true);
        break;
    case AppIconProvider::Icon::Confirm:
        line(painter, scale, 5, 12, 10, 17);
        line(painter, scale, 10, 17, 19, 7);
        break;
    case AppIconProvider::Icon::Cancel:
    case AppIconProvider::Icon::Close:
        line(painter, scale, 7, 7, 17, 17);
        line(painter, scale, 17, 7, 7, 17);
        break;
    case AppIconProvider::Icon::FitWindow:
        roundedRect(painter, scale, 5, 5, 14, 14, 2);
        chevron(painter, scale, 8, 10, 8, 8, 10, 8);
        chevron(painter, scale, 16, 10, 16, 8, 14, 8);
        chevron(painter, scale, 8, 14, 8, 16, 10, 16);
        chevron(painter, scale, 16, 14, 16, 16, 14, 16);
        break;
    case AppIconProvider::Icon::ActualSize:
        roundedRect(painter, scale, 5, 5, 14, 14, 2);
        line(painter, scale, 8, 12, 16, 12);
        line(painter, scale, 12, 8, 12, 16);
        break;
    case AppIconProvider::Icon::ZoomIn:
    case AppIconProvider::Icon::ZoomOut:
        painter.drawEllipse(rect(5, 5, 10, 10, scale));
        line(painter, scale, 13, 13, 19, 19);
        line(painter, scale, 8, 10, 12, 10);
        if (icon == AppIconProvider::Icon::ZoomIn) {
            line(painter, scale, 10, 8, 10, 12);
        }
        break;
    case AppIconProvider::Icon::PointCloud:
        for (QPointF p : {pt(7, 8, scale), pt(13, 6, scale), pt(17, 11, scale), pt(9, 15, scale), pt(15, 17, scale)}) {
            painter.drawEllipse(p, 1.2 * scale, 1.2 * scale);
        }
        line(painter, scale, 7, 8, 13, 6);
        line(painter, scale, 13, 6, 17, 11);
        line(painter, scale, 9, 15, 15, 17);
        break;
    case AppIconProvider::Icon::Copy:
        roundedRect(painter, scale, 8, 6, 10, 12, 1.5);
        roundedRect(painter, scale, 5, 9, 10, 12, 1.5);
        break;
    case AppIconProvider::Icon::Settings:
        painter.drawEllipse(rect(8, 8, 8, 8, scale));
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * 45.0 * 3.14159265358979323846 / 180.0;
            const qreal x1 = 12 + 5.5 * std::cos(a);
            const qreal y1 = 12 + 5.5 * std::sin(a);
            const qreal x2 = 12 + 8.0 * std::cos(a);
            const qreal y2 = 12 + 8.0 * std::sin(a);
            line(painter, scale, x1, y1, x2, y2);
        }
        break;
    case AppIconProvider::Icon::Agent:
        roundedRect(painter, scale, 5, 7, 14, 10, 3);
        line(painter, scale, 12, 7, 12, 4);
        painter.drawEllipse(rect(7.5, 10, 1.8, 1.8, scale));
        painter.drawEllipse(rect(14.7, 10, 1.8, 1.8, scale));
        line(painter, scale, 9, 15, 15, 15);
        break;
    }
}

} // namespace

QIcon AppIconProvider::icon(Icon icon, int size, const QColor& color) {
    static QHash<QString, QIcon> cache;
    const QString key = QString("%1/%2/%3").arg(static_cast<int>(icon)).arg(size).arg(color.rgba());
    if (cache.contains(key)) {
        return cache.value(key);
    }

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, qMax<qreal>(1.6, size / 12.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    drawIcon(painter, icon, size / 24.0);
    painter.end();

    QIcon result(pixmap);
    cache.insert(key, result);
    return result;
}

} // namespace DeepLux
