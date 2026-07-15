#include "LabelMeExporter.h"
#include "model/Annotation.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

namespace DeepLux {

bool LabelMeExporter::exportToFile(const AnnotationSession& session, const QString& outputPath, QString* error) {
    QString json = toJsonString(session);

    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Cannot create file: %1").arg(outputPath);
        return false;
    }

    file.write(json.toUtf8());
    file.close();
    return true;
}

QString LabelMeExporter::toJsonString(const AnnotationSession& session) {
    QJsonObject root;
    root["version"] = QStringLiteral("5.0.0");
    root["flags"] = QJsonObject();

    QJsonArray shapes;
    for (const AnnotationObject& obj : session.annotations) {
        QJsonObject shape;
        shape["label"] = obj.label;
        shape["group_id"] = QJsonValue::Null;

        QJsonArray points;
        for (const QPointF& p : obj.polygon) {
            QJsonArray pt;
            pt.append(p.x());
            pt.append(p.y());
            points.append(pt);
        }
        shape["points"] = points;
        shape["shape_type"] = QStringLiteral("polygon");
        shape["flags"] = QJsonObject();

        shapes.append(shape);
    }
    root["shapes"] = shapes;

    // imagePath 使用文件名（LabelMe 约定使用相对路径）
    QFileInfo fi(session.imagePath);
    root["imagePath"] = fi.fileName();
    root["imageData"] = QJsonValue::Null;
    root["imageHeight"] = session.imageHeight;
    root["imageWidth"] = session.imageWidth;

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
}

} // namespace DeepLux
