#include "AgentToolImage.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <atomic>

namespace DeepLux {

namespace {
QString sanitizeToolName(const QString& toolName) {
    QString out;
    for (const QChar c : toolName) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_'))
            out.append(c.toLower());
        else
            out.append(QLatin1Char('_'));
    }
    return out.isEmpty() ? QStringLiteral("tool") : out;
}
} // namespace

QString AgentToolImage::imageDir() {
    return QDir(QDir::tempPath()).filePath(QStringLiteral("deeplux-agent"));
}

QString AgentToolImage::writeImage(const QImage& image, const QString& toolName, const QString& format) {
    if (image.isNull())
        return QString();
    const QString dir = imageDir();
    if (!QDir().mkpath(dir))
        return QString();

    const bool jpeg = format.compare(QLatin1String("jpg"), Qt::CaseInsensitive) == 0;
    const QString ext = jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
    static std::atomic<int> s_seq{0};
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString path = QDir(dir).filePath(
        QStringLiteral("agent_%1_%2_%3.%4").arg(sanitizeToolName(toolName), stamp).arg(++s_seq).arg(ext));

    const bool saved = jpeg ? image.save(path, "JPG", 90) : image.save(path, "PNG");
    if (!saved) {
        QFile::remove(path);
        return QString();
    }

    // 防御性校验：产物必须落在 imageDir() 内（禁止调用方路径注入）
    const QString canonicalDir = QFileInfo(dir).canonicalFilePath();
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    if (canonicalDir.isEmpty() || canonicalPath.isEmpty() || !canonicalPath.startsWith(canonicalDir)) {
        QFile::remove(path);
        return QString();
    }
    return path;
}

int AgentToolImage::cleanupOlderThanDays(int days) {
    if (days <= 0)
        return 0;
    QDir dir(imageDir());
    if (!dir.exists())
        return 0;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-days);
    int removed = 0;
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("agent_*")}, QDir::Files, QDir::NoSort);
    for (const QFileInfo& entry : files) {
        if (entry.lastModified() < cutoff && QFile::remove(entry.absoluteFilePath()))
            ++removed;
    }
    return removed;
}

} // namespace DeepLux
