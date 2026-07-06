#include "ModuleIconProvider.h"

#include <QFont>
#include <QLinearGradient>
#include <QPainter>
#include <QPixmap>

namespace DeepLux {

ModuleIconProvider& ModuleIconProvider::instance() {
    static ModuleIconProvider inst;
    return inst;
}

QColor ModuleIconProvider::colorForCategory(const QString& category) const {
    if (category.contains("image"))
        return QColor("#4A90D9"); // 蓝
    if (category.contains("detect"))
        return QColor("#E67E22"); // 橙
    if (category.contains("geometry"))
        return QColor("#27AE60"); // 绿
    if (category.contains("logic"))
        return QColor("#8E44AD"); // 紫
    if (category.contains("system"))
        return QColor("#7F8C8D"); // 灰
    if (category.contains("variable"))
        return QColor("#2C3E50"); // 深蓝灰
    if (category.contains("commun"))
        return QColor("#D35400"); // 深橙
    if (category.contains("calibr"))
        return QColor("#16A085"); // 青
    if (category.contains("camera"))
        return QColor("#C0392B"); // 红
    return QColor("#95A5A6");     // 默认灰
}

QString ModuleIconProvider::abbreviationFor(const QString& id) const {
    // 常见模块的缩写映射
    static const QMap<QString, QString> abbrMap = {{"GrabImage", "GI"},
                                                   {"SaveImage", "SvI"},
                                                   {"ShowImage", "ShI"},
                                                   {"PerProcessing", "PP"},
                                                   {"ColorRecognition", "CR"},
                                                   {"Blob", "BL"},
                                                   {"Matching", "MA"},
                                                   {"QRCode", "QR"},
                                                   {"FindCircle", "FC"},
                                                   {"FitCircle", "FtC"},
                                                   {"FitLine", "FL"},
                                                   {"DistancePP", "Dp"},
                                                   {"DistancePL", "Dl"},
                                                   {"LinesDistance", "LD"},
                                                   {"MeasureRect", "MR"},
                                                   {"MeasureLine", "ML"},
                                                   {"MeasureGap", "MG"},
                                                   {"NPointCalibration", "NC"},
                                                   {"FreeformSurface", "FS"},
                                                   {"PointSurfaceDistance", "PS"},
                                                   {"If", "IF"},
                                                   {"Loop", "LP"},
                                                   {"While", "WH"},
                                                   {"Delay", "DY"},
                                                   {"StopWhile", "SW"},
                                                   {"Condition", "CD"},
                                                   {"Parallel", "PL"},
                                                   {"QueueIn", "QI"},
                                                   {"QueueOut", "QO"},
                                                   {"DataCheck", "DC"},
                                                   {"SystemTime", "ST"},
                                                   {"Folder", "FD"},
                                                   {"SaveData", "SvD"},
                                                   {"ShowPoint", "Pt"},
                                                   {"TableOutPut", "TO"},
                                                   {"WriteText", "WT"},
                                                   {"TimeSlice", "TS"},
                                                   {"Math", "MX"},
                                                   {"VarDefine", "VD"},
                                                   {"VarSet", "VS"},
                                                   {"SplitString", "SS"},
                                                   {"CreateString", "CS"},
                                                   {"JiErHanDefectsDet", "JD"},
                                                   {"PLCCommunicate", "PC"},
                                                   {"PLCRead", "PR"},
                                                   {"PLCWrite", "PW"},
                                                   {"TCPClient", "TC"},
                                                   {"TCPServer", "TSv"},
                                                   {"SerialPort", "SPo"},
                                                   {"ImageScript", "IS"},
                                                   {"JigsawPuzzle", "JP"},
                                                   {"DisplayData", "DD"},
                                                   {"StrFormat", "SF"},
                                                   {"MeasurementInput", "MI"}};
    return abbrMap.value(id, id.left(2).toUpper());
}

QIcon ModuleIconProvider::iconFor(const QString& moduleId, const QString& category) {
    QString key = moduleId + "/" + category;
    if (m_cache.contains(key))
        return m_cache[key];

    const int sz = 32;
    QPixmap pm(sz, sz);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    QColor bg = colorForCategory(category);

    // 背景: 圆角矩形 + 渐变
    QLinearGradient grad(0, 0, 0, sz);
    grad.setColorAt(0, bg.lighter(120));
    grad.setColorAt(1, bg.darker(110));
    p.setBrush(grad);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(1, 1, sz - 2, sz - 2, 5, 5);

    // 文字缩写
    QString abbr = abbreviationFor(moduleId);
    QFont font("Arial", abbr.length() > 2 ? 8 : 10, QFont::Bold);
    p.setFont(font);
    p.setPen(Qt::white);
    p.drawText(QRect(0, 0, sz, sz), Qt::AlignCenter, abbr);

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
