#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <optional>

class QImage;

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
    QString imageDigest; // 原图像素内容 SHA-256（批3 复核二轮 P1-4：同路径≠同图，
                         // 追加前校验图像未被替换；旧会话文件可为空=仅尺寸校验）
    QString modelName;   // SAM 模型名
    QList<AnnotationObject> annotations;

    // 序列化/反序列化
    QJsonObject toJson() const;
    static AnnotationSession fromJson(const QJsonObject& json);

    // 文件读写（load 严格 schema 校验失败关闭；save 为 QSaveFile 原子替换，
    // 中途失败不损坏既有文件——批3 复核二轮 P1-5）
    static AnnotationSession load(const QString& filePath, QString* error = nullptr);
    bool save(const QString& filePath, QString* error = nullptr) const;

    /// 图像像素内容摘要（ARGB32 规范化后 SHA-256，逐行仅哈希有效字节，确定性）；空图返回空串
    static QString computeImageDigest(const QImage& image);

    // 工具
    AnnotationObject* findById(const QString& id);
    void removeById(const QString& id);
    int count() const {
        return annotations.size();
    }
};

} // namespace DeepLux
