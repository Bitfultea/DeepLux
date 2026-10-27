#ifndef DEEPLUX_AGENT_TOOL_IMAGE_H
#define DEEPLUX_AGENT_TOOL_IMAGE_H

#include "../platform/Platform.h"

#include <QImage>
#include <QString>

namespace DeepLux {

/**
 * @brief Agent 工具图像产物的落盘工具
 *
 * 唯一允许的落点是系统临时目录下的 deeplux-agent/ 子目录，文件名由本模块生成
 * （agent_<tool>_<yyyyMMdd-HHmmss-zzz>_<seq>.<ext>），工具调用方**不得**传入路径。
 * 产物属于"工具自己命名的临时文件"，因此调用这些函数的工具可以保持 readOnly。
 */
namespace AgentToolImage {

/// 临时图像目录：QDir::tempPath() + "/deeplux-agent"
DEEPLUX_API QString imageDir();

/**
 * @brief 把图像写入临时目录，返回绝对路径；失败返回空字符串。
 * @param toolName 工具名，用于文件名前缀（仅保留 [a-z0-9_]）
 * @param format   "png"（默认，无损）或 "jpg"（质量 90）
 */
DEEPLUX_API QString writeImage(const QImage& image, const QString& toolName,
                               const QString& format = QStringLiteral("png"));

/// 删除目录内已存在的 agent_* 产物中修改时间早于 days 天的文件，返回删除数量（best-effort）
DEEPLUX_API int cleanupOlderThanDays(int days);

} // namespace AgentToolImage
} // namespace DeepLux

#endif // DEEPLUX_AGENT_TOOL_IMAGE_H
