#pragma once

#include <QString>

namespace DeepLux {

struct AnnotationSession;

/**
 * @brief 导出标注为 LabelMe JSON 格式
 *
 * 第一版只导出 polygon shape，不导出 mask。
 * 坐标使用原图坐标（AnnotationSession 中已保存原图坐标）。
 */
class LabelMeExporter {
public:
    /**
     * @brief 导出 AnnotationSession 为 LabelMe JSON 文件
     * @param session 标注会话
     * @param outputPath 输出文件路径
     * @param error 错误信息
     * @return 是否成功
     */
    static bool exportToFile(const AnnotationSession& session, const QString& outputPath, QString* error = nullptr);

    /**
     * @brief 生成 LabelMe JSON 文档
     * @param session 标注会话
     * @return JSON 字符串
     */
    static QString toJsonString(const AnnotationSession& session);
};

} // namespace DeepLux
