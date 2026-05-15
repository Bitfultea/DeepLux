#pragma once

#include <QIcon>
#include <QMap>
#include <QColor>

namespace DeepLux {

/**
 * @brief 模块图标运行时生成器
 *
 * 根据模块 category 和名称生成统一风格的彩色图标。
 * 替代 metadata.json 中的 emoji 或缺失的 PNG。
 */
class ModuleIconProvider {
public:
    static ModuleIconProvider& instance();

    /**
     * @brief 为指定模块获取或生成图标
     * @param moduleId 模块 ID（如 GrabImage）
     * @param category 模块分类（如 image_processing）
     * @return 24x24 的彩色图标
     */
    QIcon iconFor(const QString& moduleId, const QString& category);

    /**
     * @brief 直接从 PNG 文件加载图标（回退方案）
     */
    static QIcon fromPngFile(const QString& filePath);

private:
    ModuleIconProvider() = default;
    QColor colorForCategory(const QString& category) const;
    QString abbreviationFor(const QString& moduleId) const;

    QMap<QString, QIcon> m_cache;
};

} // namespace DeepLux
