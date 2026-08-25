#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <optional>

namespace DeepLux {

struct AnnotationPrompt {
    QList<QPointF> pointsPos;  // 正点（原图坐标）
    QList<QPointF> pointsNeg;  // 负点（原图坐标）
    std::optional<QRectF> box; // 框选（原图坐标）

    QJsonObject toJson() const;
    static AnnotationPrompt fromJson(const QJsonObject& json);
};

struct AnnotationObject {
    QString id;
    QString label;
    QRectF bbox;              // 原图坐标
    QList<QPointF> polygon;   // 原图坐标
    QString maskRle;          // RLE 编码 mask
    AnnotationPrompt prompts; // 生成此对象的 prompt
    double score = 0.0;
    QString modelName;

    QJsonObject toJson() const;
    static AnnotationObject fromJson(const QJsonObject& json);
};

struct AnnotationSession {
    QString imagePath;   // 原图路径
    int imageWidth = 0;  // 原图宽度
    int imageHeight = 0; // 原图高度
    QString modelName;   // SAM 模型名
    QList<AnnotationObject> annotations;

    // 序列化/反序列化
    QJsonObject toJson() const;
    static AnnotationSession fromJson(const QJsonObject& json);

    // 文件读写
    static AnnotationSession load(const QString& filePath, QString* error = nullptr);
    bool save(const QString& filePath, QString* error = nullptr) const;

    // 工具
    AnnotationObject* findById(const QString& id);
    void removeById(const QString& id);
    int count() const {
        return annotations.size();
    }
};

} // namespace DeepLux
