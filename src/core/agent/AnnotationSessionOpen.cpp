#include "AnnotationSessionOpen.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#endif

namespace DeepLux {

namespace {

struct FileIdentity {
    QString canonicalPath;
    QString identity;
    qint64 size = -1;
    QString modified;

    QJsonObject toJson(const QByteArray& bytes) const {
        return {{QStringLiteral("canonicalPath"), canonicalPath},
                {QStringLiteral("identity"), identity},
                {QStringLiteral("size"), static_cast<double>(size)},
                {QStringLiteral("lastModified"), modified},
                {QStringLiteral("sha256"),
                 QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}};
    }
};

QString identityFromDescriptor(qintptr descriptor) {
#ifdef Q_OS_UNIX
    struct stat info {};
    if (descriptor >= 0 && ::fstat(static_cast<int>(descriptor), &info) == 0)
        return QStringLiteral("%1:%2")
            .arg(static_cast<qulonglong>(info.st_dev))
            .arg(static_cast<qulonglong>(info.st_ino));
#elif defined(Q_OS_WIN)
    // QFile::handle() 在 Windows 上是 HANDLE，不是 C 运行时 fd；直接用 Win32 文件信息取得卷序列号 + 文件索引。
    const int fd = static_cast<int>(descriptor);
    const intptr_t nativeHandle = (fd >= 0) ? ::_get_osfhandle(fd) : -1;
    const HANDLE handle = (nativeHandle == -1) ? INVALID_HANDLE_VALUE : reinterpret_cast<HANDLE>(nativeHandle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (handle != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(handle, &info) != 0)
        return QStringLiteral("%1:%2")
            .arg(static_cast<qulonglong>(info.dwVolumeSerialNumber))
            .arg((static_cast<qulonglong>(info.nFileIndexHigh) << 32) | info.nFileIndexLow);
#else
    Q_UNUSED(descriptor)
#endif
    return {};
}

QString identityAtPath(const QString& path) {
#ifdef Q_OS_UNIX
    struct stat info {};
    const QByteArray encoded = QFile::encodeName(path);
    if (::lstat(encoded.constData(), &info) == 0)
        return QStringLiteral("%1:%2:%3")
            .arg(static_cast<qulonglong>(info.st_dev))
            .arg(static_cast<qulonglong>(info.st_ino))
            .arg(S_ISLNK(info.st_mode) ? QStringLiteral("symlink") : QStringLiteral("file"));
#elif defined(Q_OS_WIN)
    const HANDLE handle = ::CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), 0,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};
        const bool ok = ::GetFileInformationByHandle(handle, &info) != 0;
        const DWORD attributes = info.dwFileAttributes;
        ::CloseHandle(handle);
        if (ok) {
            return QStringLiteral("%1:%2:%3")
                .arg(static_cast<qulonglong>(info.dwVolumeSerialNumber))
                .arg((static_cast<qulonglong>(info.nFileIndexHigh) << 32) | info.nFileIndexLow)
                .arg((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? QStringLiteral("symlink") : QStringLiteral("file"));
        }
    }
#else
    Q_UNUSED(path)
#endif
    return {};
}

bool readRegularFileOnce(const QString& path, qint64 limit, const QString& missingCode, const QString& readCode,
                         const QString& invalidCode, const QString& limitCode, QByteArray* bytes,
                         FileIdentity* identity, QString* errorCode, QString* errorMessage) {
    const QFileInfo before(path);
    if (before.isSymLink()) {
        *errorCode = invalidCode;
        *errorMessage = QStringLiteral("Source must be a regular non-symlink file");
        return false;
    }
    if (!before.exists()) {
        *errorCode = missingCode;
        *errorMessage = QStringLiteral("Source file does not exist");
        return false;
    }
    if (!before.isFile()) {
        *errorCode = invalidCode;
        *errorMessage = QStringLiteral("Source must be a regular non-symlink file");
        return false;
    }
    if (before.size() < 0 || before.size() > limit) {
        *errorCode = limitCode;
        *errorMessage = QStringLiteral("Source file exceeds its size limit");
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *errorCode = readCode;
        *errorMessage = QStringLiteral("Source file could not be opened");
        return false;
    }
    const QString descriptorIdentity = identityFromDescriptor(file.handle());
    const QString pathIdentityBefore = identityAtPath(path);
    if (descriptorIdentity.isEmpty() || pathIdentityBefore.isEmpty() ||
        pathIdentityBefore.endsWith(QStringLiteral(":symlink")) ||
        descriptorIdentity != pathIdentityBefore.left(pathIdentityBefore.lastIndexOf(QLatin1Char(':')))) {
        *errorCode = readCode;
        *errorMessage = QStringLiteral("Source file identity changed while opening");
        return false;
    }
    const QByteArray content = file.read(limit + 1);
    if (file.error() != QFileDevice::NoError) {
        *errorCode = readCode;
        *errorMessage = QStringLiteral("Source file could not be read");
        return false;
    }
    if (content.size() > limit) {
        *errorCode = limitCode;
        *errorMessage = QStringLiteral("Source file exceeds its size limit");
        return false;
    }

    const QFileInfo after(path);
    const QString pathIdentityAfter = identityAtPath(path);
    if (!after.exists() || after.isSymLink() || !after.isFile() ||
        after.canonicalFilePath() != before.canonicalFilePath() || after.size() != before.size() ||
        after.lastModified() != before.lastModified() || pathIdentityAfter != pathIdentityBefore ||
        content.size() != before.size()) {
        *errorCode = readCode;
        *errorMessage = QStringLiteral("Source file changed while being read");
        return false;
    }

    identity->canonicalPath = before.canonicalFilePath();
    identity->identity = descriptorIdentity;
    identity->size = content.size();
    identity->modified = before.lastModified().toUTC().toString(Qt::ISODateWithMs);
    *bytes = content;
    return true;
}

bool validAbsolutePath(const QString& path, const QString& suffix) {
    return !path.trimmed().isEmpty() && path.toUtf8().size() <= 4096 && QFileInfo(path).isAbsolute() &&
           path.endsWith(suffix, Qt::CaseInsensitive);
}

} // namespace

bool AnnotationSessionOpen::capture(const QString& sessionPath, AnnotationSessionOpenCandidate* candidate,
                                    QString* errorCode, QString* errorMessage) {
    if (!candidate || !errorCode || !errorMessage)
        return false;
    *errorCode = QStringLiteral("INVALID_PARAM");
    *errorMessage = QStringLiteral("Invalid session path");
    if (!validAbsolutePath(sessionPath, QStringLiteral(".deeplux-anno.json")))
        return false;

    QByteArray sessionBytes;
    FileIdentity sessionIdentity;
    if (!readRegularFileOnce(sessionPath, kMaxSessionBytes, QStringLiteral("FILE_NOT_FOUND"),
                             QStringLiteral("FILE_READ_FAILED"), QStringLiteral("INVALID_SESSION"),
                             QStringLiteral("INPUT_LIMIT_EXCEEDED"), &sessionBytes, &sessionIdentity, errorCode,
                             errorMessage))
        return false;

    AnnotationSession loaded;
    AnnotationSession::LoadResult loadResult = AnnotationSession::LoadResult::InvalidSession;
    QString loadError;
    loaded =
        AnnotationSession::loadFromBytes(sessionBytes, &loadResult, &loadError, QStringLiteral("Agent SAM session"));
    if (loadResult != AnnotationSession::LoadResult::Success) {
        *errorCode = loadResult == AnnotationSession::LoadResult::SizeLimitExceeded
                         ? QStringLiteral("INPUT_LIMIT_EXCEEDED")
                         : QStringLiteral("INVALID_SESSION");
        *errorMessage = QStringLiteral("The annotation session is invalid");
        return false;
    }

    QImage image;
    FileIdentity imageIdentity;
    QByteArray imageBytes;
    bool legacyUnverified = false;
    if (!loaded.imagePath.isEmpty()) {
        if (!QFileInfo(loaded.imagePath).isAbsolute() || QFileInfo(loaded.imagePath).isSymLink()) {
            *errorCode = QStringLiteral("INVALID_SESSION");
            *errorMessage = QStringLiteral("The bound image path must be absolute and not a symlink");
            return false;
        }
        if (!readRegularFileOnce(loaded.imagePath, kMaxImageBytes, QStringLiteral("IMAGE_NOT_FOUND"),
                                 QStringLiteral("IMAGE_READ_FAILED"), QStringLiteral("INVALID_SESSION"),
                                 QStringLiteral("IMAGE_READ_FAILED"), &imageBytes, &imageIdentity, errorCode,
                                 errorMessage))
            return false;
        image = QImage::fromData(imageBytes);
        if (image.isNull()) {
            *errorCode = QStringLiteral("IMAGE_READ_FAILED");
            *errorMessage = QStringLiteral("The bound image could not be decoded");
            return false;
        }
        if (loaded.imageWidth > 0 && loaded.imageHeight > 0 &&
            (loaded.imageWidth != image.width() || loaded.imageHeight != image.height())) {
            *errorCode = QStringLiteral("IMAGE_BINDING_MISMATCH");
            *errorMessage = QStringLiteral("The bound image dimensions do not match the session");
            return false;
        }
        if (!loaded.imageDigest.isEmpty() && loaded.imageDigest != AnnotationSession::computeImageDigest(image)) {
            *errorCode = QStringLiteral("IMAGE_BINDING_MISMATCH");
            *errorMessage = QStringLiteral("The bound image digest does not match the session");
            return false;
        }
        if (loaded.imageDigest.isEmpty() && !loaded.annotations.isEmpty()) {
            if (loaded.imageWidth <= 0 || loaded.imageHeight <= 0) {
                *errorCode = QStringLiteral("IMAGE_BINDING_MISMATCH");
                *errorMessage = QStringLiteral("Legacy session has no verifiable image dimensions");
                return false;
            }
            legacyUnverified = true;
        }
    } else if (!loaded.annotations.isEmpty()) {
        *errorCode = QStringLiteral("INVALID_SESSION");
        *errorMessage = QStringLiteral("A non-empty session must have an image binding");
        return false;
    }

    const QByteArray normalizedSession = QJsonDocument(loaded.toJson()).toJson(QJsonDocument::Compact);
    QJsonObject snapshot{
        {QStringLiteral("sessionPath"), sessionIdentity.canonicalPath},
        {QStringLiteral("sessionFile"), sessionIdentity.toJson(sessionBytes)},
        {QStringLiteral("sessionContentSha256"),
         QString::fromLatin1(QCryptographicHash::hash(sessionBytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("sessionDigest"),
         QString::fromLatin1(QCryptographicHash::hash(normalizedSession, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("objectCount"), loaded.annotations.size()},
        {QStringLiteral("imageBound"), !image.isNull()},
        {QStringLiteral("imageWidth"), loaded.imageWidth},
        {QStringLiteral("imageHeight"), loaded.imageHeight},
        {QStringLiteral("legacyImageBindingUnverified"), legacyUnverified}};
    if (!loaded.imagePath.isEmpty()) {
        snapshot.insert(QStringLiteral("imagePath"), imageIdentity.canonicalPath);
        snapshot.insert(QStringLiteral("imageFile"), imageIdentity.toJson(imageBytes));
        snapshot.insert(QStringLiteral("imagePixelDigest"), AnnotationSession::computeImageDigest(image));
    }

    candidate->session = std::move(loaded);
    candidate->boundImage = std::move(image);
    candidate->sessionPath = sessionIdentity.canonicalPath;
    candidate->imagePath = imageIdentity.canonicalPath;
    candidate->imageBound = !candidate->boundImage.isNull();
    candidate->legacyImageBindingUnverified = legacyUnverified;
    candidate->snapshot = snapshot;
    errorCode->clear();
    errorMessage->clear();
    return true;
}

} // namespace DeepLux
