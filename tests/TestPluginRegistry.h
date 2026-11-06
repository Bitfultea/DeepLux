#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QVector>

namespace DeepLuxTest {

/// 一个运行时插件在构建树中的位置。
struct PluginEntry {
    QString target;       // CMake target 名，如 FitLinePlugin
    QString libraryPath;  // $<TARGET_FILE:...>：.dll/.so/.dylib 的实际路径
    QString metadataPath; // 源码 metadata.json 的绝对路径
};

/// 仓库源码根目录（由 CMake 注入，兼容 MSVC 多配置输出目录）。
inline QString testSourceDir() {
#ifdef DEEPLUX_TEST_SOURCE_DIR
    return QString::fromUtf8(DEEPLUX_TEST_SOURCE_DIR);
#else
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
#endif
}

inline QString testDataPath(const QString& relativePath) {
    return QDir(testSourceDir()).filePath(QStringLiteral("tests/acceptance/data/") + relativePath);
}

/// 全部插件 target 的构建产物清单（由 CMake 注入，格式：
/// <target>|<library>|<metadata>@@...）。
inline const QVector<PluginEntry>& pluginRegistry() {
    static const QVector<PluginEntry> entries = [] {
        QVector<PluginEntry> result;
#ifdef DEEPLUX_TEST_PLUGIN_MANIFEST
        const QString manifest = QString::fromUtf8(DEEPLUX_TEST_PLUGIN_MANIFEST);
        for (const QString& item : manifest.split(QStringLiteral("@@"), Qt::SkipEmptyParts)) {
            const QStringList parts = item.split(QLatin1Char('|'));
            if (parts.size() == 3) {
                result.append({parts.at(0), parts.at(1), parts.at(2)});
            }
        }
#endif
        return result;
    }();
    return entries;
}

/// 按 target 名或源码目录名（如 "GrabImage"）查找插件。
inline PluginEntry pluginEntry(const QString& targetOrName) {
    for (const PluginEntry& entry : pluginRegistry()) {
        if (entry.target == targetOrName) {
            return entry;
        }
    }
    for (const PluginEntry& entry : pluginRegistry()) {
        if (entry.target == targetOrName + QStringLiteral("Plugin")) {
            return entry;
        }
    }
    for (const PluginEntry& entry : pluginRegistry()) {
        const QString dirName = QFileInfo(entry.metadataPath).dir().dirName();
        if (dirName == targetOrName || dirName == targetOrName + QStringLiteral("Plugin")) {
            return entry;
        }
    }
    return {};
}

/// 插件的安装目录名（源码目录名，如 GrabImage / MathPlugin）。
inline QString pluginInstallDir(const PluginEntry& entry) {
    return QFileInfo(entry.metadataPath).dir().dirName();
}

} // namespace DeepLuxTest
