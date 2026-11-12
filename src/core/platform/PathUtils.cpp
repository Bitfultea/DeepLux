#include "PathUtils.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>

namespace DeepLux {

namespace {

QString resolveFileTargetPathImpl(const QString& path, QSet<QString>& visitedSymlinks, int depth) {
    if (depth > 64)
        return {};
    const QFileInfo pathInfo(path);
    const QString absolutePath = QDir::cleanPath(pathInfo.absoluteFilePath());
    if (pathInfo.isSymLink()) {
        if (visitedSymlinks.contains(absolutePath))
            return {};
        visitedSymlinks.insert(absolutePath);
        QString symlinkTarget = pathInfo.symLinkTarget();
        if (symlinkTarget.isEmpty()) {
            visitedSymlinks.remove(absolutePath);
            return {};
        }
        if (QFileInfo(symlinkTarget).isRelative())
            symlinkTarget = QDir(pathInfo.absolutePath()).filePath(symlinkTarget);
        const QString resolved = resolveFileTargetPathImpl(symlinkTarget, visitedSymlinks, depth + 1);
        visitedSymlinks.remove(absolutePath);
        return resolved;
    }

    const QString canonicalTarget = pathInfo.canonicalFilePath();
    if (!canonicalTarget.isEmpty())
        return QDir::cleanPath(canonicalTarget);

    QStringList unresolvedComponents{pathInfo.fileName()};
    QString existingAncestor = pathInfo.absolutePath();
    while (!existingAncestor.isEmpty()) {
        const QFileInfo ancestorInfo(existingAncestor);
        const QString canonicalAncestor = ancestorInfo.canonicalFilePath();
        if (!canonicalAncestor.isEmpty()) {
            QString resolved = canonicalAncestor;
            for (const QString& component : unresolvedComponents)
                resolved = QDir(resolved).filePath(component);
            return QDir::cleanPath(resolved);
        }

        if (ancestorInfo.isSymLink()) {
            if (visitedSymlinks.contains(existingAncestor))
                return {};
            visitedSymlinks.insert(existingAncestor);
            QString symlinkTarget = ancestorInfo.symLinkTarget();
            if (QFileInfo(symlinkTarget).isRelative())
                symlinkTarget = QDir(ancestorInfo.absolutePath()).filePath(symlinkTarget);
            QString resolved = resolveFileTargetPathImpl(symlinkTarget, visitedSymlinks, depth + 1);
            visitedSymlinks.remove(existingAncestor);
            if (resolved.isEmpty())
                return {};
            for (const QString& component : unresolvedComponents)
                resolved = QDir(resolved).filePath(component);
            return QDir::cleanPath(resolved);
        }

        const QString parent = ancestorInfo.absolutePath();
        const QString component = ancestorInfo.fileName();
        if (component.isEmpty() || parent == existingAncestor)
            break;
        unresolvedComponents.prepend(component);
        existingAncestor = parent;
    }
    return {};
}

} // namespace

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

QString PathUtils::resolveFileTargetPath(const QString& path) {
    QSet<QString> visitedSymlinks;
    return resolveFileTargetPathImpl(path, visitedSymlinks, 0);
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
