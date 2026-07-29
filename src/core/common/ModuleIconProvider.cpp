#include "ModuleIconProvider.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>
#include <QStringList>

namespace DeepLux {
namespace {

enum class Glyph {
    Image,
    Camera,
    Save,
    Display,
    Preprocess,
    Blob,
    Color,
    Target,
    QrCode,
    Circle,
    FitCircle,
    Line,
    PointPairDistance,
    PointLineDistance,
    LinePairDistance,
    Gap,
    MeasurementInput,
    Ruler,
    Rect,
    Calibration,
    Branch,
    Loop,
    Stop,
    Delay,
    Parallel,
    QueueIn,
    QueueOut,
    Clock,
    Folder,
    Database,
    Table,
    Text,
    Variable,
    VariableDefine,
    VariableSet,
    Math,
    DataCheck,
    PointCloud,
    Communication,
    Read,
    Write,
    TcpClient,
    TcpServer,
    Serial,
    Script,
    Default
};

QPointF pt(qreal x, qreal y) {
    return QPointF(x, y);
}

QRectF rect(qreal x, qreal y, qreal w, qreal h) {
    return QRectF(x, y, w, h);
}

void line(QPainter& painter, qreal x1, qreal y1, qreal x2, qreal y2) {
    painter.drawLine(pt(x1, y1), pt(x2, y2));
}

void roundedRect(QPainter& painter, qreal x, qreal y, qreal w, qreal h, qreal radius = 2.0) {
    painter.drawRoundedRect(rect(x, y, w, h), radius, radius);
}

void chevron(QPainter& painter, qreal x1, qreal y1, qreal x2, qreal y2, qreal x3, qreal y3) {
    QPainterPath path;
    path.moveTo(pt(x1, y1));
    path.lineTo(pt(x2, y2));
    path.lineTo(pt(x3, y3));
    painter.drawPath(path);
}

void drawDot(QPainter& painter, qreal x, qreal y, qreal radius) {
    QPen oldPen = painter.pen();
    QBrush oldBrush = painter.brush();
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::white);
    painter.drawEllipse(pt(x, y), radius, radius);
    painter.setBrush(oldBrush);
    painter.setPen(oldPen);
}

void drawCircularArrow(QPainter& painter) {
    painter.drawArc(rect(8, 8, 16, 16), 35 * 16, 280 * 16);
    chevron(painter, 21, 9, 23, 9, 22, 11.5);
}

bool containsAny(const QString& text, const QStringList& needles) {
    for (const QString& needle : needles) {
        if (text.contains(needle)) {
            return true;
        }
    }
    return false;
}

QString normalizedDomainFor(const QString& moduleId, const QString& category) {
    const QString module = moduleId.toLower();
    const QString raw = moduleId;

    if (containsAny(module, {"loadpointcloud", "pointcloud", "freeformsurface", "pointsurfacedistance", "showpoint"}) ||
        raw.contains(QStringLiteral("点云"))) {
        return QStringLiteral("3d");
    }
    if (containsAny(module, {"grabimage", "saveimage", "showimage", "perprocessing", "preprocess", "colorrecognition",
                             "blob", "imagescript", "jigsawpuzzle"}) ||
        raw.contains(QStringLiteral("图像")) || raw.contains(QStringLiteral("颜色"))) {
        return QStringLiteral("image_processing");
    }
    if (containsAny(module, {"distance", "measure", "gap", "circle", "line", "surface"}) ||
        raw.contains(QStringLiteral("测量")) || raw.contains(QStringLiteral("圆")) ||
        raw.contains(QStringLiteral("线"))) {
        return QStringLiteral("geometry");
    }
    if (containsAny(module, {"displaydata", "datacheck", "vardefine", "varset", "math", "splitstring", "createstring",
                             "strformat"}) ||
        raw.contains(QStringLiteral("变量")) || raw.contains(QStringLiteral("字符串")) ||
        raw.contains(QStringLiteral("数学"))) {
        return QStringLiteral("variable");
    }
    return category;
}

Glyph glyphFor(const QString& moduleId, const QString& category) {
    const QString module = moduleId.toLower();
    const QString cat = category.toLower();
    const QString raw = moduleId;

    if (module.contains("pointcloud") || module.contains("surface") || module.contains("showpoint") ||
        cat.contains("3d") || raw.contains(QStringLiteral("点云"))) {
        return Glyph::PointCloud;
    }
    if (module.contains("grabimage")) {
        return Glyph::Camera;
    }
    if (module.contains("saveimage")) {
        return Glyph::Save;
    }
    if (module.contains("showimage")) {
        return Glyph::Display;
    }
    if (module.contains("perprocessing") || module.contains("preprocess")) {
        return Glyph::Preprocess;
    }
    if (module.contains("blob")) {
        return Glyph::Blob;
    }
    if (module.contains("colorrecognition") || raw.contains(QStringLiteral("颜色"))) {
        return Glyph::Color;
    }
    if (module.contains("matching") || module.contains("defect")) {
        return Glyph::Target;
    }
    if (module.contains("qrcode")) {
        return Glyph::QrCode;
    }
    if (module.contains("circle")) {
        if (module.contains("fit")) {
            return Glyph::FitCircle;
        }
        return Glyph::Circle;
    }
    if (module.contains("rect")) {
        return Glyph::Rect;
    }
    if (module.contains("distancepp")) {
        return Glyph::PointPairDistance;
    }
    if (module.contains("distancepl") || module.contains("pointsurfacedistance")) {
        return Glyph::PointLineDistance;
    }
    if (module.contains("linesdistance")) {
        return Glyph::LinePairDistance;
    }
    if (module.contains("measuregap")) {
        return Glyph::Gap;
    }
    if (module.contains("measurementinput")) {
        return Glyph::MeasurementInput;
    }
    if (module.contains("line") && !module.contains("distance")) {
        return Glyph::Line;
    }
    if (module.contains("distance") || module.contains("measure") || module.contains("gap")) {
        return Glyph::Ruler;
    }
    if (module.contains("calibration")) {
        return Glyph::Calibration;
    }
    if (module.contains("stopwhile") || raw.contains(QStringLiteral("停止"))) {
        return Glyph::Stop;
    }
    if (module.contains("queuein") || raw.contains(QStringLiteral("队列输入"))) {
        return Glyph::QueueIn;
    }
    if (module.contains("queueout") || raw.contains(QStringLiteral("队列输出"))) {
        return Glyph::QueueOut;
    }
    if (module.contains("parallel") || raw.contains(QStringLiteral("并行"))) {
        return Glyph::Parallel;
    }
    if (module.contains("delay") || raw.contains(QStringLiteral("延时"))) {
        return Glyph::Delay;
    }
    if (module == "if" || module.contains("condition") ||
        (raw.contains(QStringLiteral("条件")) && !raw.contains(QStringLiteral("循环")))) {
        return Glyph::Branch;
    }
    if (module.contains("loop") || module.contains("while") || raw.contains(QStringLiteral("循环"))) {
        return Glyph::Loop;
    }
    if (module.contains("systemtime") || module.contains("timeslice") || module.contains("delay")) {
        return Glyph::Clock;
    }
    if (module.contains("folder")) {
        return Glyph::Folder;
    }
    if (module.contains("savedata")) {
        return Glyph::Database;
    }
    if (module.contains("table") || module.contains("displaydata")) {
        return Glyph::Table;
    }
    if (module.contains("text") || module.contains("string") || module.contains("strformat") ||
        raw.contains(QStringLiteral("字符串"))) {
        return Glyph::Text;
    }
    if (module.contains("math") || raw.contains(QStringLiteral("数学"))) {
        return Glyph::Math;
    }
    if (module.contains("vardefine") || raw.contains(QStringLiteral("变量定义"))) {
        return Glyph::VariableDefine;
    }
    if (module.contains("varset") || raw.contains(QStringLiteral("变量赋值"))) {
        return Glyph::VariableSet;
    }
    if (module.contains("var") || raw.contains(QStringLiteral("变量"))) {
        return Glyph::Variable;
    }
    if (module.contains("datacheck")) {
        return Glyph::DataCheck;
    }
    if (module.contains("plcread") || raw.contains(QStringLiteral("读取"))) {
        return Glyph::Read;
    }
    if (module.contains("plcwrite") || raw.contains(QStringLiteral("写入"))) {
        return Glyph::Write;
    }
    if (module.contains("tcpclient") || raw.contains(QStringLiteral("客户端"))) {
        return Glyph::TcpClient;
    }
    if (module.contains("tcpserver") || raw.contains(QStringLiteral("服务器"))) {
        return Glyph::TcpServer;
    }
    if (module.contains("serial") || raw.contains(QStringLiteral("串口"))) {
        return Glyph::Serial;
    }
    if (module.contains("plc") || module.contains("tcp") || module.contains("serial") || cat.contains("commun")) {
        return Glyph::Communication;
    }
    if (module.contains("script")) {
        return Glyph::Script;
    }

    if (cat.contains("image")) {
        return Glyph::Image;
    }
    if (cat.contains("common")) {
        return Glyph::Default;
    }
    if (cat.contains("relation")) {
        return Glyph::LinePairDistance;
    }
    if (cat.contains("alignment")) {
        return Glyph::Calibration;
    }
    if (cat.contains("file")) {
        return Glyph::Folder;
    }
    if (cat.contains("string")) {
        return Glyph::Text;
    }
    if (cat.contains("detect")) {
        return Glyph::Target;
    }
    if (cat.contains("geometry")) {
        return Glyph::Ruler;
    }
    if (cat.contains("logic")) {
        return Glyph::Branch;
    }
    if (cat.contains("system")) {
        return Glyph::Database;
    }
    if (cat.contains("variable")) {
        return Glyph::Variable;
    }
    if (cat.contains("calibr")) {
        return Glyph::Calibration;
    }
    if (cat.contains("camera")) {
        return Glyph::Camera;
    }
    return Glyph::Default;
}

void drawGlyph(QPainter& painter, Glyph glyph) {
    switch (glyph) {
    case Glyph::Image:
        roundedRect(painter, 7, 9, 18, 14, 2);
        drawDot(painter, 19.5, 13, 1.4);
        line(painter, 9, 20, 14, 15);
        line(painter, 14, 15, 19, 20);
        break;
    case Glyph::Camera:
        roundedRect(painter, 7, 11, 18, 12, 2.5);
        line(painter, 11, 11, 12.5, 8);
        line(painter, 12.5, 8, 19.5, 8);
        line(painter, 19.5, 8, 21, 11);
        painter.drawEllipse(rect(13, 14, 6, 6));
        break;
    case Glyph::Save:
        roundedRect(painter, 8, 7, 16, 18, 2);
        line(painter, 12, 7, 12, 13);
        line(painter, 12, 13, 21, 13);
        roundedRect(painter, 12, 18, 8, 5, 1);
        break;
    case Glyph::Display:
        roundedRect(painter, 7, 8, 18, 13, 2);
        line(painter, 16, 21, 16, 25);
        line(painter, 12, 25, 20, 25);
        break;
    case Glyph::Preprocess:
        drawCircularArrow(painter);
        line(painter, 9, 15, 19, 15);
        line(painter, 11, 19, 21, 19);
        break;
    case Glyph::Blob:
        drawDot(painter, 11, 11, 2.2);
        drawDot(painter, 18, 12, 1.8);
        drawDot(painter, 13, 19, 2.6);
        drawDot(painter, 21, 21, 1.5);
        break;
    case Glyph::Color:
        painter.drawEllipse(rect(8, 8, 13, 13));
        drawDot(painter, 12, 12, 1.3);
        drawDot(painter, 17, 13, 1.3);
        drawDot(painter, 14, 18, 1.3);
        line(painter, 19, 19, 24, 24);
        break;
    case Glyph::Target:
        painter.drawEllipse(rect(8, 8, 16, 16));
        painter.drawEllipse(rect(12, 12, 8, 8));
        line(painter, 16, 6, 16, 10);
        line(painter, 16, 22, 16, 26);
        line(painter, 6, 16, 10, 16);
        line(painter, 22, 16, 26, 16);
        break;
    case Glyph::QrCode: {
        QPen oldPen = painter.pen();
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        for (const QRectF& r : {rect(8, 8, 5, 5), rect(19, 8, 5, 5), rect(8, 19, 5, 5), rect(16, 16, 2.5, 2.5),
                                rect(21, 18, 3, 3), rect(16, 22, 2.5, 2.5)}) {
            painter.drawRoundedRect(r, 0.8, 0.8);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(oldPen);
        break;
    }
    case Glyph::Circle:
        painter.drawEllipse(rect(8, 8, 16, 16));
        line(painter, 16, 6, 16, 10);
        line(painter, 16, 22, 16, 26);
        line(painter, 6, 16, 10, 16);
        line(painter, 22, 16, 26, 16);
        break;
    case Glyph::FitCircle:
        painter.drawEllipse(rect(8, 8, 16, 16));
        drawDot(painter, 11, 12, 1.3);
        drawDot(painter, 19, 10, 1.3);
        drawDot(painter, 22, 18, 1.3);
        drawDot(painter, 14, 23, 1.3);
        break;
    case Glyph::Line:
        line(painter, 8, 22, 24, 10);
        drawDot(painter, 8, 22, 2);
        drawDot(painter, 24, 10, 2);
        break;
    case Glyph::PointPairDistance:
        drawDot(painter, 9, 21, 2);
        drawDot(painter, 23, 11, 2);
        line(painter, 9, 21, 23, 11);
        chevron(painter, 13, 19, 9, 21, 11, 17);
        chevron(painter, 19, 13, 23, 11, 21, 15);
        break;
    case Glyph::PointLineDistance:
        line(painter, 9, 22, 24, 10);
        drawDot(painter, 11, 10, 2);
        line(painter, 11, 10, 17, 16);
        line(painter, 15.5, 14.5, 18.5, 17.5);
        break;
    case Glyph::LinePairDistance:
        line(painter, 9, 12, 24, 12);
        line(painter, 8, 21, 23, 21);
        line(painter, 16, 13, 16, 20);
        chevron(painter, 14, 16, 16, 13, 18, 16);
        chevron(painter, 14, 18, 16, 21, 18, 18);
        break;
    case Glyph::Gap:
        roundedRect(painter, 8, 9, 5, 14, 1.5);
        roundedRect(painter, 20, 9, 5, 14, 1.5);
        line(painter, 14, 16, 19, 16);
        chevron(painter, 16, 14, 14, 16, 16, 18);
        chevron(painter, 17, 14, 19, 16, 17, 18);
        break;
    case Glyph::MeasurementInput:
        painter.drawEllipse(rect(9, 9, 14, 14));
        line(painter, 16, 8, 16, 24);
        line(painter, 8, 16, 24, 16);
        line(painter, 22, 22, 26, 22);
        line(painter, 24, 20, 24, 24);
        break;
    case Glyph::Ruler:
        line(painter, 8, 23, 24, 9);
        for (int i = 0; i < 5; ++i) {
            const qreal x = 10 + i * 3.2;
            const qreal y = 21.2 - i * 2.8;
            line(painter, x, y, x + 1.8, y + 1.8);
        }
        break;
    case Glyph::Rect:
        roundedRect(painter, 8, 9, 16, 14, 2);
        line(painter, 8, 14, 24, 14);
        line(painter, 14, 9, 14, 23);
        break;
    case Glyph::Calibration:
        painter.drawEllipse(rect(8, 8, 16, 16));
        line(painter, 16, 7, 16, 25);
        line(painter, 7, 16, 25, 16);
        drawDot(painter, 16, 16, 1.4);
        break;
    case Glyph::Branch:
        drawDot(painter, 10, 10, 2.2);
        drawDot(painter, 22, 10, 2.2);
        drawDot(painter, 22, 22, 2.2);
        line(painter, 12, 10, 20, 10);
        line(painter, 12, 11, 20, 21);
        break;
    case Glyph::Loop:
        drawCircularArrow(painter);
        break;
    case Glyph::Stop:
        roundedRect(painter, 10, 10, 12, 12, 2);
        break;
    case Glyph::Delay:
        painter.drawEllipse(rect(8, 8, 16, 16));
        line(painter, 16, 16, 16, 11);
        line(painter, 16, 16, 20, 16);
        drawDot(painter, 11, 25, 0.9);
        drawDot(painter, 16, 25, 0.9);
        drawDot(painter, 21, 25, 0.9);
        break;
    case Glyph::Parallel:
        drawDot(painter, 9, 16, 2);
        drawDot(painter, 23, 10, 2);
        drawDot(painter, 23, 22, 2);
        line(painter, 11, 16, 21, 10);
        line(painter, 11, 16, 21, 22);
        break;
    case Glyph::QueueIn:
        roundedRect(painter, 9, 10, 14, 12, 2);
        line(painter, 5, 16, 15, 16);
        chevron(painter, 12, 13, 15, 16, 12, 19);
        break;
    case Glyph::QueueOut:
        roundedRect(painter, 9, 10, 14, 12, 2);
        line(painter, 17, 16, 27, 16);
        chevron(painter, 24, 13, 27, 16, 24, 19);
        break;
    case Glyph::Clock:
        painter.drawEllipse(rect(8, 8, 16, 16));
        line(painter, 16, 16, 16, 11);
        line(painter, 16, 16, 20, 18);
        break;
    case Glyph::Folder: {
        QPainterPath path;
        path.moveTo(pt(7, 12));
        path.lineTo(pt(13, 12));
        path.lineTo(pt(15, 14));
        path.lineTo(pt(25, 14));
        path.lineTo(pt(25, 23));
        path.lineTo(pt(7, 23));
        path.closeSubpath();
        painter.drawPath(path);
        break;
    }
    case Glyph::Database:
        painter.drawEllipse(rect(9, 8, 14, 5));
        line(painter, 9, 10.5, 9, 22);
        line(painter, 23, 10.5, 23, 22);
        painter.drawArc(rect(9, 19, 14, 5), 180 * 16, 180 * 16);
        line(painter, 9, 16, 23, 16);
        break;
    case Glyph::Table:
        roundedRect(painter, 8, 8, 16, 16, 1.5);
        line(painter, 8, 13, 24, 13);
        line(painter, 8, 18, 24, 18);
        line(painter, 14, 8, 14, 24);
        line(painter, 19, 8, 19, 24);
        break;
    case Glyph::Text:
        line(painter, 9, 9, 23, 9);
        line(painter, 16, 9, 16, 24);
        line(painter, 12, 24, 20, 24);
        break;
    case Glyph::Variable:
        line(painter, 9, 10, 23, 22);
        line(painter, 23, 10, 9, 22);
        break;
    case Glyph::VariableDefine:
        line(painter, 9, 10, 20, 21);
        line(painter, 20, 10, 9, 21);
        line(painter, 22, 20, 26, 20);
        line(painter, 24, 18, 24, 22);
        break;
    case Glyph::VariableSet:
        line(painter, 8, 11, 18, 21);
        line(painter, 18, 11, 8, 21);
        line(painter, 20, 16, 26, 16);
        chevron(painter, 23, 13, 26, 16, 23, 19);
        break;
    case Glyph::Math:
        line(painter, 8, 12, 16, 12);
        line(painter, 12, 8, 12, 16);
        line(painter, 18, 18, 24, 24);
        line(painter, 24, 18, 18, 24);
        break;
    case Glyph::DataCheck:
        roundedRect(painter, 8, 7, 16, 18, 2);
        line(painter, 11, 17, 15, 21);
        line(painter, 15, 21, 22, 13);
        break;
    case Glyph::PointCloud:
        for (const QPointF& p : {pt(10, 10), pt(17, 8), pt(22, 14), pt(12, 19), pt(20, 23)}) {
            drawDot(painter, p.x(), p.y(), 1.8);
        }
        line(painter, 10, 10, 17, 8);
        line(painter, 17, 8, 22, 14);
        line(painter, 12, 19, 20, 23);
        break;
    case Glyph::Communication:
        drawDot(painter, 10, 11, 2.2);
        drawDot(painter, 22, 11, 2.2);
        drawDot(painter, 16, 22, 2.2);
        line(painter, 12, 11, 20, 11);
        line(painter, 11, 13, 15, 20);
        line(painter, 21, 13, 17, 20);
        break;
    case Glyph::Read:
        roundedRect(painter, 8, 9, 16, 14, 2);
        line(painter, 22, 16, 12, 16);
        chevron(painter, 15, 13, 12, 16, 15, 19);
        break;
    case Glyph::Write:
        roundedRect(painter, 8, 9, 16, 14, 2);
        line(painter, 10, 16, 20, 16);
        chevron(painter, 17, 13, 20, 16, 17, 19);
        break;
    case Glyph::TcpClient:
        roundedRect(painter, 7, 10, 10, 10, 2);
        roundedRect(painter, 18, 12, 7, 6, 1.5);
        line(painter, 17, 15, 18, 15);
        break;
    case Glyph::TcpServer:
        roundedRect(painter, 8, 7, 16, 6, 1.5);
        roundedRect(painter, 8, 18, 16, 6, 1.5);
        line(painter, 12, 13, 12, 18);
        line(painter, 20, 13, 20, 18);
        break;
    case Glyph::Serial:
        roundedRect(painter, 8, 10, 16, 12, 2);
        line(painter, 11, 10, 11, 7);
        line(painter, 16, 10, 16, 7);
        line(painter, 21, 10, 21, 7);
        line(painter, 12, 22, 12, 25);
        line(painter, 20, 22, 20, 25);
        break;
    case Glyph::Script:
        chevron(painter, 13, 11, 9, 16, 13, 21);
        chevron(painter, 19, 11, 23, 16, 19, 21);
        line(painter, 17, 10, 15, 22);
        break;
    case Glyph::Default:
        roundedRect(painter, 9, 9, 14, 14, 3);
        drawDot(painter, 16, 16, 2);
        break;
    }
}

} // namespace

ModuleIconProvider& ModuleIconProvider::instance() {
    static ModuleIconProvider inst;
    return inst;
}

QColor ModuleIconProvider::colorForCategory(const QString& category) const {
    const QString cat = category.toLower();
    if (cat.contains("common"))
        return QColor("#0F766E");
    if (cat.contains("relation"))
        return QColor("#0D9488");
    if (cat.contains("alignment"))
        return QColor("#CA8A04");
    if (cat.contains("file"))
        return QColor("#B45309");
    if (cat.contains("string"))
        return QColor("#BE123C");
    if (cat.contains("image"))
        return QColor("#2563EB");
    if (cat.contains("detect"))
        return QColor("#EA580C");
    if (cat.contains("geometry"))
        return QColor("#059669");
    if (cat.contains("logic"))
        return QColor("#7C3AED");
    if (cat.contains("system"))
        return QColor("#64748B");
    if (cat.contains("variable"))
        return QColor("#475569");
    if (cat.contains("commun"))
        return QColor("#0891B2");
    if (cat.contains("3d"))
        return QColor("#0891B2");
    if (cat.contains("calibr"))
        return QColor("#0D9488");
    if (cat.contains("camera"))
        return QColor("#DC2626");
    if (cat.contains("deep"))
        return QColor("#9333EA");
    return QColor("#6B7280");
}

QIcon ModuleIconProvider::iconFor(const QString& moduleId, const QString& category) {
    const QString key = moduleId + "/" + category;
    if (m_cache.contains(key))
        return m_cache[key];

    const int sz = 32;
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    const QString domain = normalizedDomainFor(moduleId, category);
    const QColor bg = colorForCategory(domain);
    p.setBrush(bg);
    p.setPen(QPen(bg.darker(112), 1.0));
    p.drawRoundedRect(QRectF(1.5, 1.5, sz - 3, sz - 3), 7, 7);

    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Qt::white, 2.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    drawGlyph(p, glyphFor(moduleId, category));
    p.end();

    QIcon icon(pm);
    m_cache[key] = icon;
    return icon;
}

QIcon ModuleIconProvider::fromPngFile(const QString& filePath) {
    QPixmap pm(filePath);
    if (!pm.isNull())
        return QIcon(pm);
    return QIcon();
}

} // namespace DeepLux
