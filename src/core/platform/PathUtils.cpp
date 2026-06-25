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

#ifdef DEEPLUX_PLATFORM_WINDOWS
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
#else
    QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    QString path = home + "/.deeplux";
#endif
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
