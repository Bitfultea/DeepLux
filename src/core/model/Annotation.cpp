#include "Annotation.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QRectF>
#include <QSaveFile>
#include <QUuid>

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
            prompt.box = QRectF(boxArr[0].toDouble(), boxArr[1].toDouble(), boxArr[2].toDouble(), boxArr[3].toDouble());
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
        obj.bbox = QRectF(bboxArr[0].toDouble(), bboxArr[1].toDouble(), bboxArr[2].toDouble(), bboxArr[3].toDouble());
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
    json["imageDigest"] = imageDigest; // 批3 复核二轮（P1-4）：图像内容摘要（可空=旧文件）
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
    session.imageDigest = json["imageDigest"].toString();
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
        if (error)
            *error = QStringLiteral("Cannot open file: %1").arg(filePath);
        return AnnotationSession();
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = parseError.errorString();
        return AnnotationSession();
    }

    // 批3 复核二轮（P1-5）：最小 schema 严格校验——旧实现把合法 JSON 数组/标量
    // 经 doc.object() 静默当空会话载入，追加保存后覆盖损坏原文件
    if (!doc.isObject()) {
        if (error)
            *error = QStringLiteral("Not a JSON object (invalid annotation session): %1").arg(filePath);
        return AnnotationSession();
    }
    const QJsonObject obj = doc.object();
    if (obj.contains(QLatin1String("annotations")) && !obj.value(QLatin1String("annotations")).isArray()) {
        if (error)
            *error = QStringLiteral("Field 'annotations' must be an array: %1").arg(filePath);
        return AnnotationSession();
    }
    for (const char* key : {"imageWidth", "imageHeight"}) {
        if (obj.contains(QLatin1String(key)) && !obj.value(QLatin1String(key)).isDouble()) {
            if (error)
                *error = QStringLiteral("Field '%1' must be a number: %2").arg(QLatin1String(key), filePath);
            return AnnotationSession();
        }
    }
    if (obj.contains(QLatin1String("imagePath")) && !obj.value(QLatin1String("imagePath")).isString()) {
        if (error)
            *error = QStringLiteral("Field 'imagePath' must be a string: %1").arg(filePath);
        return AnnotationSession();
    }

    return fromJson(obj);
}

bool AnnotationSession::save(const QString& filePath, QString* error) const {
    // 批3 复核二轮（P1-5）：QSaveFile 原子替换——先写临时文件再 commit，磁盘满
    // 或进程中断不会截断/损坏既有会话；写入与提交结果均显式检查
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot create file: %1 (%2)").arg(filePath, file.errorString());
        return false;
    }

    const QByteArray data = QJsonDocument(toJson()).toJson(QJsonDocument::Indented);
    if (file.write(data) != data.size()) {
        if (error)
            *error = QStringLiteral("Write failed (disk full or device error): %1").arg(filePath);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Commit failed: %1 (%2)").arg(filePath, file.errorString());
        return false;
    }
    return true;
}

QString AnnotationSession::computeImageDigest(const QImage& image) {
    if (image.isNull())
        return QString();
    // 批3 复核二轮（P1-4）：同路径 ≠ 同图——像素内容摘要用于会话追加前校验
    // 图像未被替换。统一转 ARGB32 消除编码/格式差异；逐行仅哈希 width*4 有效
    // 字节（行尾 padding 不参与，保证同图同摘要的确定性）
    const QImage canonical = image.convertToFormat(QImage::Format_ARGB32);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int y = 0; y < canonical.height(); ++y)
        hash.addData(reinterpret_cast<const char*>(canonical.constScanLine(y)), canonical.width() * 4);
    hash.addData(QByteArray::number(canonical.width()));
    hash.addData(QByteArray::number(canonical.height()));
    return QString::fromLatin1(hash.result().toHex());
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
