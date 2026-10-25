#include "PathUtils.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace DeepLux {

QString PathUtils::appDataPath() {
    const QByteArray overridePath = qgetenv("DEEPLUX_APP_DATA_DIR");
    if (!overridePath.isEmpty()) {
        const QString path = QString::fromLocal8Bit(overridePath);
        ensureDirExists(path);
        return path;
    }

    // 跨平台统一使用用户主目录下的 .deeplux，与 scripts/sync-plugins.* 保持一致。
    // Windows 上不再使用 QStandardPaths::AppLocalDataLocation：该路径会随
    // applicationName/organizationName 变化（CLI 与 GUI 因此曾落到不同目录），
    // 且仓库脚本与文档约定用户插件位于 ~/.deeplux/plugins。
    const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString path = home + QStringLiteral("/.deeplux");
    ensureDirExists(path);
    return path;
}

QString PathUtils::pluginPath() {
    QString path = appDataPath() + "/plugins";
    ensureDirExists(path);
    return path;
}

QString PathUtils::configPath() {
    QString path = appDataPath() + "/config";
    ensureDirExists(path);
    return path;
}

QString PathUtils::logPath() {
    QString path = appDataPath() + "/logs";
    ensureDirExists(path);
    return path;
}

QString PathUtils::projectPath() {
    QString path = appDataPath() + "/projects";
    ensureDirExists(path);
    return path;
}

QString PathUtils::normalize(const QString& path) {
    return QDir::cleanPath(path);
}

QString PathUtils::join(const QString& base, const QString& sub) {
    return QDir(base).filePath(sub);
}

bool PathUtils::ensureDirExists(const QString& path) {
    QDir dir(path);
    if (!dir.exists()) {
        return dir.mkpath(".");
    }
    return true;
}

QString PathUtils::applicationDirPath() {
    return QCoreApplication::applicationDirPath();
}

} // namespace DeepLux
