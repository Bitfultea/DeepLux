#pragma once

#include <QString>
#include <QStringList>

namespace DeepLux {

struct AnnotationSession;

/**
 * @brief 导出标注为 YOLO Seg 文本格式
 *
 * 每个图像对应一个 .txt 文件，每行格式：
 *   class_id x1 y1 x2 y2 ... xn yn
 * 坐标归一化到 [0,1]，仅导出 polygon。
 */
class YoloSegExporter {
public:
    static bool exportToFile(const AnnotationSession& session, const QString& outputPath,
                             const QStringList& classLabels, QString* error = nullptr);

    static QString toJsonString(const AnnotationSession& session, const QStringList& classLabels);
};

} // namespace DeepLux
