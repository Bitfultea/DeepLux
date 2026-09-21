#include "CocoExporter.h"

#include "model/Annotation.h"
#include "model/AnnotationMaskOps.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

namespace DeepLux {

namespace {

// polygon shoelace 面积（绝对值）
double polygonArea(const QList<QPointF>& polygon) {
    if (polygon.size() < 3)
        return 0.0;
    double area2 = 0.0;
    const int n = polygon.size();
    for (int i = 0; i < n; ++i) {
        const QPointF& p = polygon[i];
        const QPointF& q = polygon[(i + 1) % n];
        area2 += p.x() * q.y() - q.x() * p.y();
    }
    return std::fabs(area2) / 2.0;
}

// 由 polygon 推导外接矩形（像素中心口径：w=maxX-minX，与 SAM bbox 约定一致）
QRectF bboxFromPolygon(const QList<QPointF>& polygon) {
    if (polygon.isEmpty())
        return QRectF();
    double minX = polygon.first().x();
    double maxX = minX;
    double minY = polygon.first().y();
    double maxY = minY;
    for (const QPointF& p : polygon) {
        minX = std::min(minX, p.x());
        maxX = std::max(maxX, p.x());
        minY = std::min(minY, p.y());
        maxY = std::max(maxY, p.y());
    }
    return QRectF(minX, minY, maxX - minX, maxY - minY);
}

} // namespace

QJsonObject CocoExporter::toJson(const AnnotationSession& session, const QStringList& categories, QString* error) {
    auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return QJsonObject();
    };
    if (session.annotations.isEmpty())
        return fail(QStringLiteral("无标注对象，无法导出 COCO"));
    if (session.imageWidth <= 0 || session.imageHeight <= 0)
        return fail(QStringLiteral("会话缺少有效图像宽高，无法导出 COCO"));

    // 类别映射：参数顺序优先（id 从 1 起），未知 label 按出现顺序追加
    QList<QString> categoryNames;
    for (const QString& name : categories) {
        if (!name.isEmpty() && !categoryNames.contains(name))
            categoryNames.append(name);
    }
    for (const AnnotationObject& obj : session.annotations) {
        if (!obj.label.isEmpty() && !categoryNames.contains(obj.label))
            categoryNames.append(obj.label);
    }

    QJsonArray categoryArr;
    for (int i = 0; i < categoryNames.size(); ++i) {
        categoryArr.append(QJsonObject{{"id", i + 1}, {"name", categoryNames[i]}, {"supercategory", QString()}});
    }

    QJsonArray annotations;
    for (int i = 0; i < session.annotations.size(); ++i) {
        const AnnotationObject& obj = session.annotations[i];
        // COCO category_id 从 1 起：空 label 无法映射类别，失败关闭不伪造
        if (obj.label.isEmpty())
            return fail(QStringLiteral("annotations[%1]（id=%2）label 为空，无法映射 COCO 类别").arg(i).arg(obj.id));
        QRectF bbox = obj.bbox;
        if (!bbox.isValid())
            bbox = bboxFromPolygon(obj.polygon); // 无效 bbox 由 polygon 推导
        if (!bbox.isValid() && obj.polygon.size() < 3)
            return fail(QStringLiteral("annotations[%1]（id=%2）缺少 polygon/bbox，无法导出 COCO").arg(i).arg(obj.id));

        // area 口径链：RLE 像素数（精确）→ polygon shoelace → bbox 面积
        double area = -1.0;
        if (!obj.maskRle.isEmpty()) {
            bool ok = false;
            const QImage mask = MaskOps::decodeRle(obj.maskRle, QSize(session.imageWidth, session.imageHeight), &ok);
            if (ok)
                area = double(MaskOps::foregroundCount(mask));
        }
        if (area < 0.0)
            area = polygonArea(obj.polygon);
        if (area <= 0.0)
            area = bbox.width() * bbox.height();

        QJsonArray segmentation;
        if (obj.polygon.size() >= 3) {
            QJsonArray ring;
            for (const QPointF& p : obj.polygon) {
                ring.append(p.x());
                ring.append(p.y());
            }
            segmentation.append(ring);
        }

        annotations.append(QJsonObject{{"id", i + 1},
                                       {"image_id", 1},
                                       {"category_id", categoryNames.indexOf(obj.label) + 1},
                                       {"segmentation", segmentation},
                                       {"bbox", QJsonArray{bbox.x(), bbox.y(), bbox.width(), bbox.height()}},
                                       {"area", area},
                                       {"iscrowd", 0}});
    }

    QJsonArray images;
    images.append(QJsonObject{{"id", 1},
                              {"file_name", QFileInfo(session.imagePath).fileName()},
                              {"width", session.imageWidth},
                              {"height", session.imageHeight}});

    QJsonObject root;
    root["info"] = QJsonObject{{"description", QStringLiteral("DeepLux annotation session export")},
                               {"version", QStringLiteral("1.0")}};
    root["licenses"] = QJsonArray();
    root["images"] = images;
    root["annotations"] = annotations;
    root["categories"] = categoryArr;
    return root;
}

bool CocoExporter::exportToFile(const AnnotationSession& session, const QString& outputPath,
                                const QStringList& categories, QString* error) {
    QString jsonError;
    const QJsonObject root = toJson(session, categories, &jsonError);
    if (root.isEmpty()) {
        if (error)
            *error = jsonError.isEmpty() ? QStringLiteral("COCO 导出失败") : jsonError;
        return false;
    }
    // QSaveFile 原子写：中途失败不损坏既有文件（与会话保存同口径）
    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot create file: %1 (%2)").arg(outputPath, file.errorString());
        return false;
    }
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("Write failed (disk full or device error): %1").arg(outputPath);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Commit failed: %1 (%2)").arg(outputPath, file.errorString());
        return false;
    }
    return true;
}

} // namespace DeepLux
