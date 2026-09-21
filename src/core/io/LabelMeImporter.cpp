#include "LabelMeImporter.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <cmath>

namespace DeepLux {

bool LabelMeImporter::loadShapes(const QString& filePath, QList<LabelMeShape>& outShapes, QString* error) {
    outShapes.clear();
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return false;
    };

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开 LabelMe 文件：%1").arg(filePath));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("LabelMe JSON 解析失败：%1").arg(parseError.errorString()));
    if (!doc.isObject())
        return fail(QStringLiteral("LabelMe 文件不是 JSON 对象"));

    const QJsonObject root = doc.object();
    if (!root.contains(QLatin1String("shapes")) || !root.value(QLatin1String("shapes")).isArray())
        return fail(QStringLiteral("缺少 shapes 数组"));
    const QJsonArray shapes = root.value(QLatin1String("shapes")).toArray();

    for (int i = 0; i < shapes.size(); ++i) {
        const QString context = QStringLiteral("shapes[%1]").arg(i);
        if (!shapes[i].isObject())
            return fail(QStringLiteral("%1 必须为对象").arg(context));
        const QJsonObject obj = shapes[i].toObject();

        LabelMeShape shape;
        const QJsonValue label = obj.value(QLatin1String("label"));
        if (!label.isString() || label.toString().isEmpty())
            return fail(QStringLiteral("%1.label 必须为非空字符串").arg(context));
        shape.label = label.toString();

        const QJsonValue pointsValue = obj.value(QLatin1String("points"));
        if (!pointsValue.isArray() || pointsValue.toArray().size() < 2)
            return fail(QStringLiteral("%1.points 必须为至少 2 个点的数组").arg(context));
        const QJsonArray points = pointsValue.toArray();
        for (int k = 0; k < points.size(); ++k) {
            const QJsonValue pv = points[k];
            if (!pv.isArray() || pv.toArray().size() != 2 || !pv.toArray()[0].isDouble() ||
                !pv.toArray()[1].isDouble() || !std::isfinite(pv.toArray()[0].toDouble()) ||
                !std::isfinite(pv.toArray()[1].toDouble()))
                return fail(QStringLiteral("%1.points[%2] 必须为有限数值 [x,y]").arg(context).arg(k));
            shape.points.append(QPointF(pv.toArray()[0].toDouble(), pv.toArray()[1].toDouble()));
        }

        const QJsonValue typeValue = obj.value(QLatin1String("shape_type"));
        if (!typeValue.isUndefined() && !typeValue.isNull() && !typeValue.isString())
            return fail(QStringLiteral("%1.shape_type 必须为字符串或缺省").arg(context));
        shape.shapeType = typeValue.toString();
        if (shape.shapeType.isEmpty())
            shape.shapeType = (shape.points.size() == 2) ? QStringLiteral("rectangle") : QStringLiteral("polygon");

        outShapes.append(shape);
    }
    if (error)
        error->clear();
    return true;
}

QList<LabelMeBox> LabelMeImporter::rectangleBoxes(const QList<LabelMeShape>& shapes, int* skippedCount) {
    QList<LabelMeBox> boxes;
    int skipped = 0;
    for (const LabelMeShape& shape : shapes) {
        if (shape.shapeType != QLatin1String("rectangle") || shape.points.size() != 2) {
            ++skipped; // 非矩形（polygon/linestrip 等）不在 box prompt 范围
            continue;
        }
        const QRectF box = QRectF(shape.points[0], shape.points[1]).normalized();
        if (box.width() <= 0.0 || box.height() <= 0.0) {
            ++skipped; // 退化矩形（零宽/高）无法作为 prompt
            continue;
        }
        boxes.append(LabelMeBox{shape.label, box});
    }
    if (skippedCount)
        *skippedCount = skipped;
    return boxes;
}

} // namespace DeepLux
