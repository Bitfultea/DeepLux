#include "Annotation.h"

#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QRectF>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace DeepLux {

namespace {

// === 批3 复核三轮（P1-2）：会话文件严格 schema 校验 ===
// 旧实现对数组元素直接 toObject()——{"annotations":[42]} 会静默变成空标注；
// 尺寸接受负数/小数、摘要接受任意类型。畸形文件被当合法会话载入后，
// Agent 追加保存会原子地覆盖原文件（物理写安全，但逻辑数据丢失）。

bool isFiniteNumber(const QJsonValue& value) {
    return value.isDouble() && std::isfinite(value.toDouble());
}

bool isIntegralNumber(const QJsonValue& value) {
    return isFiniteNumber(value) && value.toDouble() == std::floor(value.toDouble());
}

bool validatePointArray(const QJsonArray& arr, const QString& context, QString* error) {
    for (int i = 0; i < arr.size(); ++i) {
        const QJsonValue pt = arr[i];
        if (!pt.isArray() || pt.toArray().size() < 2 || !isFiniteNumber(pt.toArray()[0]) ||
            !isFiniteNumber(pt.toArray()[1])) {
            *error = QStringLiteral("%1: 元素必须为有限数值 [x,y]（索引 %2）").arg(context).arg(i);
            return false;
        }
    }
    return true;
}

bool validatePromptJson(const QJsonValue& value, const QString& context, QString* error) {
    if (value.isUndefined() || value.isNull())
        return true;
    if (!value.isObject()) {
        *error = QStringLiteral("%1.prompts 必须为对象").arg(context);
        return false;
    }
    const QJsonObject obj = value.toObject();
    for (const char* key : {"points_pos", "points_neg"}) {
        const QJsonValue arr = obj.value(QLatin1String(key));
        if (arr.isUndefined() || arr.isNull())
            continue;
        if (!arr.isArray()) {
            *error = QStringLiteral("%1.prompts.%2 必须为数组").arg(context, QLatin1String(key));
            return false;
        }
        if (!validatePointArray(arr.toArray(), QStringLiteral("%1.prompts.%2").arg(context, QLatin1String(key)), error))
            return false;
    }
    const QJsonValue box = obj.value(QStringLiteral("box"));
    if (!box.isUndefined() && !box.isNull()) {
        if (!box.isArray() || box.toArray().size() < 4) {
            *error = QStringLiteral("%1.prompts.box 必须为 4 数值数组").arg(context);
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            if (!isFiniteNumber(box.toArray()[i])) {
                *error = QStringLiteral("%1.prompts.box 必须为有限数值").arg(context);
                return false;
            }
        }
    }
    return true;
}

bool validateObjectJson(const QJsonValue& value, int index, QString* error) {
    const QString context = QStringLiteral("annotations[%1]").arg(index);
    if (!value.isObject()) {
        *error = QStringLiteral("%1 必须为对象").arg(context);
        return false;
    }
    const QJsonObject obj = value.toObject();
    const QJsonValue id = obj.value(QStringLiteral("id"));
    if (!id.isString() || id.toString().isEmpty()) {
        *error = QStringLiteral("%1.id 必须为非空字符串").arg(context);
        return false;
    }
    for (const char* key : {"label", "mask_rle", "model_name"}) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (!v.isUndefined() && !v.isNull() && !v.isString()) {
            *error = QStringLiteral("%1.%2 必须为字符串").arg(context, QLatin1String(key));
            return false;
        }
    }
    const QJsonValue bbox = obj.value(QStringLiteral("bbox"));
    if (!bbox.isUndefined() && !bbox.isNull()) {
        if (!bbox.isArray() || bbox.toArray().size() < 4) {
            *error = QStringLiteral("%1.bbox 必须为 4 数值数组").arg(context);
            return false;
        }
        for (int i = 0; i < 4; ++i) {
            if (!isFiniteNumber(bbox.toArray()[i])) {
                *error = QStringLiteral("%1.bbox 必须为有限数值").arg(context);
                return false;
            }
        }
    }
    const QJsonValue polygon = obj.value(QStringLiteral("polygon"));
    if (!polygon.isUndefined() && !polygon.isNull()) {
        if (!polygon.isArray()) {
            *error = QStringLiteral("%1.polygon 必须为数组").arg(context);
            return false;
        }
        if (!validatePointArray(polygon.toArray(), context + QStringLiteral(".polygon"), error))
            return false;
    }
    const QJsonValue score = obj.value(QStringLiteral("score"));
    if (!score.isUndefined() && !score.isNull()) {
        if (!isFiniteNumber(score) || score.toDouble() < 0.0 || score.toDouble() > 1.0) {
            *error = QStringLiteral("%1.score 必须为 [0,1] 有限数值").arg(context);
            return false;
        }
    }
    return validatePromptJson(obj.value(QStringLiteral("prompts")), context, error);
}

} // namespace

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

    // 批3 复核二轮（P1-5）+ 三轮（P1-2）：严格 schema 校验失败关闭——
    // 旧实现把合法 JSON 数组经 doc.object() 静默当空会话载入、把 [42] 元素
    // toObject() 成空标注；畸形文件被当合法会话后，追加保存会覆盖原文件
    auto failLoad = [error](const QString& message) {
        if (error)
            *error = message;
        return AnnotationSession();
    };
    if (!doc.isObject())
        return failLoad(QStringLiteral("Not a JSON object (invalid annotation session): %1").arg(filePath));
    const QJsonObject obj = doc.object();
    // annotations 必须存在且为数组，逐元素严格校验
    if (!obj.contains(QLatin1String("annotations")) || !obj.value(QLatin1String("annotations")).isArray())
        return failLoad(QStringLiteral("Field 'annotations' must be present and be an array: %1").arg(filePath));
    const QJsonArray annArr = obj.value(QLatin1String("annotations")).toArray();
    QString fieldError;
    QSet<QString> seenIds;
    for (int i = 0; i < annArr.size(); ++i) {
        if (!validateObjectJson(annArr[i], i, &fieldError))
            return failLoad(QStringLiteral("%1: %2").arg(fieldError, filePath));
        // 批3 复核四轮（P1-2）：id 唯一性——findById/removeById/掩膜缓存均以 id
        // 为键，重复 id 会导致掩膜覆盖与编辑对象错配（只操作第一个同名对象）
        const QString id = annArr[i].toObject().value(QLatin1String("id")).toString();
        if (seenIds.contains(id))
            return failLoad(QStringLiteral("annotations[%1].id duplicated (%2): %3").arg(i).arg(id, filePath));
        seenIds.insert(id);
    }
    // 尺寸：非负整数值且不超 int 表示范围（批3 复核四轮 P2-3：2147483648 经
    // toInt() 会静默溢出为 0，反序列化不得缩零）
    for (const char* key : {"imageWidth", "imageHeight"}) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (v.isUndefined() || v.isNull())
            continue;
        if (!isIntegralNumber(v) || v.toDouble() < 0.0 || v.toDouble() > double(std::numeric_limits<int>::max()))
            return failLoad(
                QStringLiteral("Field '%1' must be an integer in [0, INT_MAX]: %2").arg(QLatin1String(key), filePath));
    }
    // 字符串字段类型受控
    for (const char* key : {"imagePath", "imageDigest", "modelName"}) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (!v.isUndefined() && !v.isNull() && !v.isString())
            return failLoad(QStringLiteral("Field '%1' must be a string: %2").arg(QLatin1String(key), filePath));
    }
    // 批3 复核四轮（P2-3）：imageDigest 为空或 64 位十六进制（SHA-256 hex，
    // computeImageDigest 产物口径），拒绝任意短串冒充摘要
    const QJsonValue digestValue = obj.value(QLatin1String("imageDigest"));
    if (digestValue.isString() && !digestValue.toString().isEmpty()) {
        const QString digest = digestValue.toString();
        const bool validHex = digest.size() == 64 && std::all_of(digest.constBegin(), digest.constEnd(), [](QChar c) {
                                  return c.isDigit() || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                              });
        if (!validHex)
            return failLoad(QStringLiteral("Field 'imageDigest' must be empty or 64 hex characters: %1").arg(filePath));
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
