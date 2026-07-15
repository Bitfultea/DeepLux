#include "Annotation.h"

#include <QFile>
#include <QJsonDocument>
#include <QUuid>
#include <QRectF>

namespace DeepLux {

// === AnnotationPrompt ===

QJsonObject AnnotationPrompt::toJson() const {
    QJsonObject json;
    QJsonArray posArr;
    for (const QPointF& p : pointsPos) {
        QJsonArray pt;
        pt.append(p.x());
        pt.append(p.y());
        posArr.append(pt);
    }
    json["points_pos"] = posArr;

    QJsonArray negArr;
    for (const QPointF& p : pointsNeg) {
        QJsonArray pt;
        pt.append(p.x());
        pt.append(p.y());
        negArr.append(pt);
    }
    json["points_neg"] = negArr;

    if (box.has_value()) {
        QJsonArray boxArr;
        boxArr.append(box->x());
        boxArr.append(box->y());
        boxArr.append(box->width());
        boxArr.append(box->height());
        json["box"] = boxArr;
    }

    return json;
}

AnnotationPrompt AnnotationPrompt::fromJson(const QJsonObject& json) {
    AnnotationPrompt prompt;

    QJsonArray posArr = json["points_pos"].toArray();
    for (const QJsonValue& v : posArr) {
        QJsonArray pt = v.toArray();
        if (pt.size() >= 2)
            prompt.pointsPos.append(QPointF(pt[0].toDouble(), pt[1].toDouble()));
    }

    QJsonArray negArr = json["points_neg"].toArray();
    for (const QJsonValue& v : negArr) {
        QJsonArray pt = v.toArray();
        if (pt.size() >= 2)
            prompt.pointsNeg.append(QPointF(pt[0].toDouble(), pt[1].toDouble()));
    }

    QJsonValue boxVal = json["box"];
    if (boxVal.isArray()) {
        QJsonArray boxArr = boxVal.toArray();
        if (boxArr.size() >= 4)
            prompt.box = QRectF(boxArr[0].toDouble(), boxArr[1].toDouble(),
                                 boxArr[2].toDouble(), boxArr[3].toDouble());
    }

    return prompt;
}

// === AnnotationObject ===

QJsonObject AnnotationObject::toJson() const {
    QJsonObject json;
    json["id"] = id;
    json["label"] = label;

    QJsonArray bboxArr;
    bboxArr.append(bbox.x());
    bboxArr.append(bbox.y());
    bboxArr.append(bbox.width());
    bboxArr.append(bbox.height());
    json["bbox"] = bboxArr;

    QJsonArray polyArr;
    for (const QPointF& p : polygon) {
        QJsonArray pt;
        pt.append(p.x());
        pt.append(p.y());
        polyArr.append(pt);
    }
    json["polygon"] = polyArr;

    json["mask_rle"] = maskRle;
    json["prompts"] = prompts.toJson();
    json["score"] = score;
    json["model_name"] = modelName;

    return json;
}

AnnotationObject AnnotationObject::fromJson(const QJsonObject& json) {
    AnnotationObject obj;
    obj.id = json["id"].toString();
    obj.label = json["label"].toString();

    QJsonArray bboxArr = json["bbox"].toArray();
    if (bboxArr.size() >= 4) {
        obj.bbox = QRectF(bboxArr[0].toDouble(), bboxArr[1].toDouble(),
                          bboxArr[2].toDouble(), bboxArr[3].toDouble());
    }

    QJsonArray polyArr = json["polygon"].toArray();
    for (const QJsonValue& v : polyArr) {
        QJsonArray pt = v.toArray();
        if (pt.size() >= 2)
            obj.polygon.append(QPointF(pt[0].toDouble(), pt[1].toDouble()));
    }

    obj.maskRle = json["mask_rle"].toString();
    obj.prompts = AnnotationPrompt::fromJson(json["prompts"].toObject());
    obj.score = json["score"].toDouble();
    obj.modelName = json["model_name"].toString();

    return obj;
}

// === AnnotationSession ===

QJsonObject AnnotationSession::toJson() const {
    QJsonObject json;
    json["imagePath"] = imagePath;
    json["imageWidth"] = imageWidth;
    json["imageHeight"] = imageHeight;
    json["modelName"] = modelName;

    QJsonArray annArr;
    for (const AnnotationObject& obj : annotations) {
        annArr.append(obj.toJson());
    }
    json["annotations"] = annArr;

    return json;
}

AnnotationSession AnnotationSession::fromJson(const QJsonObject& json) {
    AnnotationSession session;
    session.imagePath = json["imagePath"].toString();
    session.imageWidth = json["imageWidth"].toInt();
    session.imageHeight = json["imageHeight"].toInt();
    session.modelName = json["modelName"].toString();

    QJsonArray annArr = json["annotations"].toArray();
    for (const QJsonValue& v : annArr) {
        session.annotations.append(AnnotationObject::fromJson(v.toObject()));
    }

    return session;
}

AnnotationSession AnnotationSession::load(const QString& filePath, QString* error) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Cannot open file: %1").arg(filePath);
        return AnnotationSession();
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError) {
        if (error) *error = parseError.errorString();
        return AnnotationSession();
    }

    return fromJson(doc.object());
}

bool AnnotationSession::save(const QString& filePath, QString* error) const {
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("Cannot create file: %1").arg(filePath);
        return false;
    }

    QJsonDocument doc(toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    return true;
}

AnnotationObject* AnnotationSession::findById(const QString& id) {
    for (int i = 0; i < annotations.size(); ++i) {
        if (annotations[i].id == id)
            return &annotations[i];
    }
    return nullptr;
}

void AnnotationSession::removeById(const QString& id) {
    for (int i = 0; i < annotations.size(); ++i) {
        if (annotations[i].id == id) {
            annotations.removeAt(i);
            return;
        }
    }
}

} // namespace DeepLux
