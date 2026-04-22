#pragma once

#include <QString>

namespace DeepLux {

/**
 * @brief 路径工具类
 * 
 * 提供跨平台的路径处理功能
 */
class PathUtils
{
public:
    /**
     * @brief 获取应用数据目录
     * Windows: C:/Users/<user>/AppData/Local/DeepLux
     * Linux: ~/.deeplux
     */
    static QString appDataPath();
    
    /**
     * @brief 获取插件目录
     */
    static QString pluginPath();
    
    /**
     * @brief 获取配置目录
     */
    static QString configPath();
    
    /**
     * @brief 获取日志目录
     */
    static QString logPath();
    
    /**
     * @brief 获取项目目录
     */
    static QString projectPath();
    
    /**
     * @brief 规范化路径
     */
    static QString normalize(const QString& path);
    
    /**
     * @brief 拼接路径
     */
    static QString join(const QString& base, const QString& sub);
    
    /**
     * @brief 确保目录存在
     */
    static bool ensureDirExists(const QString& path);
    
    /**
     * @brief 获取应用可执行文件目录
     */
    static QString applicationDirPath();
};

} // namespace DeepLux
