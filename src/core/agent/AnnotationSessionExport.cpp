#if defined(_WIN32)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0602
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "AnnotationSessionExport.h"

#include "io/CocoExporter.h"
#include "io/LabelMeExporter.h"
#include "io/YoloSegExporter.h"
#include "model/Annotation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <new>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace DeepLux {

namespace {
constexpr qint64 kMaxInputBytes = 64LL * 1024 * 1024;
constexpr qint64 kMaxOutputBytes = 64LL * 1024 * 1024;
constexpr int kMaxPathBytes = 4096;
constexpr int kMaxResponseBytes = 65536;

QJsonObject toolError(const QString& message, const QString& code) {
    return QJsonObject{
        {QStringLiteral("error"), message}, {QStringLiteral("errorCode"), code}, {QStringLiteral("retryable"), false}};
}

bool pathIdentity(int handle, QString* identity) {
#ifdef Q_OS_UNIX
    struct stat status {};
    if (fstat(handle, &status) != 0)
        return false;
    *identity = QStringLiteral("%1:%2").arg(QString::number(static_cast<qulonglong>(status.st_dev)),
                                            QString::number(static_cast<qulonglong>(status.st_ino)));
    return true;
#elif defined(Q_OS_WIN)
    const intptr_t nativeHandle = _get_osfhandle(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (nativeHandle == -1 || !GetFileInformationByHandle(reinterpret_cast<HANDLE>(nativeHandle), &info))
        return false;
    *identity = QStringLiteral("%1:%2:%3")
                    .arg(info.dwVolumeSerialNumber)
                    .arg(info.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                    .arg(info.nFileIndexLow, 8, 16, QLatin1Char('0'));
    return true;
#else
    Q_UNUSED(handle)
    Q_UNUSED(identity)
    return false;
#endif
}

bool directoryIdentity(const QString& path, QString* identity) {
#ifdef Q_OS_UNIX
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = pathIdentity(fd, identity);
    ::close(fd);
    return ok;
#elif defined(Q_OS_WIN)
    const HANDLE handle =
        CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(handle, &info) != 0;
    CloseHandle(handle);
    if (!ok)
        return false;
    *identity = QStringLiteral("%1:%2:%3")
                    .arg(info.dwVolumeSerialNumber)
                    .arg(info.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                    .arg(info.nFileIndexLow, 8, 16, QLatin1Char('0'));
    return true;
#else
    Q_UNUSED(path)
    Q_UNUSED(identity)
    return false;
#endif
}

bool openRegularFileNoFollow(const QString& path, QFile* file, QString* identity, bool* wasSymlink) {
#ifdef Q_OS_UNIX
    const QByteArray encoded = QFile::encodeName(path);
    const int fd = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        struct stat linkStatus {};
        if (::lstat(encoded.constData(), &linkStatus) == 0 && S_ISLNK(linkStatus.st_mode))
            *wasSymlink = true;
        return false;
    }
    struct stat status {};
    const bool regular = fstat(fd, &status) == 0 && S_ISREG(status.st_mode);
    if (!regular || !pathIdentity(fd, identity)) {
        ::close(fd);
        return false;
    }
    if (!file->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    return true;
#elif defined(Q_OS_WIN)
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo, sizeof(tagInfo))) {
        CloseHandle(handle);
        return false;
    }
    if (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        *wasSymlink = true;
        CloseHandle(handle);
        return false;
    }
    if (!GetFileInformationByHandle(handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        CloseHandle(handle);
        return false;
    }
    *identity = QStringLiteral("%1:%2:%3")
                    .arg(info.dwVolumeSerialNumber)
                    .arg(info.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                    .arg(info.nFileIndexLow, 8, 16, QLatin1Char('0'));
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return false;
    }
    if (!file->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        _close(fd);
        return false;
    }
    return true;
#else
    QFileInfo info(path);
    if (info.isSymLink()) {
        *wasSymlink = true;
        return false;
    }
    if (!info.isFile() || !file->open(path, QIODevice::ReadOnly) || !pathIdentity(file->handle(), identity))
        return false;
    return true;
#endif
}

QJsonObject captureSnapshot(const QJsonObject& params, QByteArray* sourceBytes = nullptr) {
    const QString sourcePath = params.value(QStringLiteral("sessionPath")).toString();
    const QString requestedOutputPath = params.value(QStringLiteral("outputPath")).toString();
    const QString format = params.value(QStringLiteral("format")).toString();
    const QStringList categories = [&params]() {
        QStringList values;
        for (const QJsonValue& value : params.value(QStringLiteral("categories")).toArray())
            values.append(value.toString());
        return values;
    }();

    QJsonObject snapshot{{QStringLiteral("sessionPath"), sourcePath},
                         {QStringLiteral("format"), format},
                         {QStringLiteral("categories"), QJsonArray::fromStringList(categories)},
                         {QStringLiteral("outputPath"), requestedOutputPath},
                         {QStringLiteral("sourceState"), QStringLiteral("invalid")},
                         {QStringLiteral("targetState"), QStringLiteral("invalid")}};
    for (const QString& key : {QStringLiteral("operation"), QStringLiteral("objectIds"), QStringLiteral("label")}) {
        if (params.contains(key))
            snapshot.insert(key, params.value(key));
    }
    if (sourcePath.toUtf8().size() > kMaxPathBytes || requestedOutputPath.toUtf8().size() > kMaxPathBytes ||
        sourcePath.trimmed().isEmpty() || requestedOutputPath.trimmed().isEmpty())
        return snapshot;

    const QFileInfo sourceInfo(sourcePath);
    snapshot.insert(QStringLiteral("sourceCanonicalPath"), sourceInfo.canonicalFilePath());
    snapshot.insert(QStringLiteral("sourceIsSymlink"), sourceInfo.isSymLink());
    if (!sourceInfo.exists())
        snapshot.insert(QStringLiteral("sourceState"), QStringLiteral("missing"));
    else if (sourceInfo.isSymLink() || !sourceInfo.isFile())
        snapshot.insert(QStringLiteral("sourceState"), QStringLiteral("not_regular"));
    if (sourceInfo.isFile() && !sourceInfo.isSymLink()) {
        QFile source;
        QString identity;
        bool wasSymlink = false;
        if (openRegularFileNoFollow(sourcePath, &source, &identity, &wasSymlink)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            QByteArray data;
            constexpr qint64 chunkSize = 64 * 1024;
            bool oversized = false;
            while (data.size() <= kMaxInputBytes) {
                const qint64 remaining = kMaxInputBytes + 1 - data.size();
                const QByteArray chunk = source.read(std::min(chunkSize, remaining));
                if (chunk.isEmpty()) {
                    if (!source.atEnd())
                        break;
                    break;
                }
                data.append(chunk);
                hash.addData(chunk);
                if (data.size() > kMaxInputBytes) {
                    oversized = true;
                    break;
                }
            }
            if (source.error() == QFileDevice::NoError && !oversized) {
                snapshot.insert(QStringLiteral("sourceState"), QStringLiteral("readable"));
                snapshot.insert(QStringLiteral("sourceBytes"), data.size());
                snapshot.insert(QStringLiteral("sourceSha256"), QString::fromLatin1(hash.result().toHex()));
                snapshot.insert(QStringLiteral("sourceIdentity"), identity);
                if (sourceBytes)
                    *sourceBytes = data;
            } else if (oversized) {
                snapshot.insert(QStringLiteral("sourceState"), QStringLiteral("oversized"));
            } else if (source.error() != QFileDevice::NoError) {
                snapshot.insert(QStringLiteral("sourceState"), QStringLiteral("read_error"));
            }
        } else if (wasSymlink) {
            snapshot.insert(QStringLiteral("sourceIsSymlink"), true);
            snapshot.insert(QStringLiteral("sourceState"), QStringLiteral("not_regular"));
        }
    }

    const QFileInfo outputInfo(requestedOutputPath);
    const QString parentPath = QFileInfo(outputInfo.absolutePath()).canonicalFilePath();
    snapshot.insert(QStringLiteral("outputCanonicalParent"), parentPath);
    snapshot.insert(QStringLiteral("outputFileName"), outputInfo.fileName());
    snapshot.insert(QStringLiteral("outputIsSymlink"), outputInfo.isSymLink());
    snapshot.insert(QStringLiteral("outputExists"), outputInfo.exists());
    if (!parentPath.isEmpty() && QFileInfo(parentPath).isDir()) {
        QString parentIdentity;
        if (directoryIdentity(parentPath, &parentIdentity)) {
            snapshot.insert(QStringLiteral("outputParentIdentity"), parentIdentity);
            snapshot.insert(QStringLiteral("targetState"), outputInfo.exists() || outputInfo.isSymLink()
                                                               ? QStringLiteral("exists")
                                                               : QStringLiteral("absent"));
        }
    }
    return snapshot;
}

QString expectedSuffix(const QString& format) {
    return format == QLatin1String("yolo_seg") ? QStringLiteral("txt") : QStringLiteral("json");
}

bool publishNoReplace(const QString& temporaryPath, const QString& outputPath, const QString& expectedParentIdentity,
                      QString* errorCode) {
#ifdef Q_OS_UNIX
    const QFileInfo outputInfo(outputPath);
    const QByteArray parent = QFile::encodeName(outputInfo.absolutePath());
    const QFileInfo temporaryInfo(temporaryPath);
    const QByteArray stagingDirectory = QFile::encodeName(temporaryInfo.absolutePath());
    const QByteArray sourceName = QFile::encodeName(temporaryInfo.fileName());
    const QByteArray targetName = QFile::encodeName(outputInfo.fileName());
    const int parentFd = ::open(parent.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parentFd < 0) {
        *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    QString currentParentIdentity;
    if (!pathIdentity(parentFd, &currentParentIdentity) || currentParentIdentity != expectedParentIdentity) {
        ::close(parentFd);
        *errorCode = QStringLiteral("CONFIRMATION_EXPIRED");
        return false;
    }
    const int stagingFd = ::open(stagingDirectory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (stagingFd < 0) {
        ::close(parentFd);
        *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    struct stat sourceStatus {};
    if (::fstatat(stagingFd, sourceName.constData(), &sourceStatus, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(sourceStatus.st_mode)) {
        ::close(stagingFd);
        ::close(parentFd);
        *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    const int result = ::linkat(stagingFd, sourceName.constData(), parentFd, targetName.constData(), 0);
    const int linkError = errno;
    ::close(stagingFd);
    ::close(parentFd);
    if (result == 0)
        return true;
    *errorCode = (linkError == EEXIST) ? QStringLiteral("OUTPUT_EXISTS") : QStringLiteral("FILE_WRITE_FAILED");
    return false;
#elif defined(Q_OS_WIN)
    const HANDLE parentHandle =
        CreateFileW(reinterpret_cast<LPCWSTR>(QFileInfo(outputPath).absolutePath().utf16()),
                    FILE_ADD_FILE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (parentHandle == INVALID_HANDLE_VALUE) {
        *errorCode = QStringLiteral("CONFIRMATION_EXPIRED");
        return false;
    }
    BY_HANDLE_FILE_INFORMATION parentInfo{};
    QString currentParentIdentity;
    const bool parentValid = GetFileInformationByHandle(parentHandle, &parentInfo) != 0;
    if (parentValid) {
        currentParentIdentity = QStringLiteral("%1:%2:%3")
                                    .arg(parentInfo.dwVolumeSerialNumber)
                                    .arg(parentInfo.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                                    .arg(parentInfo.nFileIndexLow, 8, 16, QLatin1Char('0'));
    }
    if (!parentValid || currentParentIdentity != expectedParentIdentity) {
        CloseHandle(parentHandle);
        *errorCode = QStringLiteral("CONFIRMATION_EXPIRED");
        return false;
    }

    HANDLE sourceHandle = INVALID_HANDLE_VALUE;
    DWORD sourceOpenError = ERROR_SUCCESS;
    // Windows 上刚关闭的临时文件可能短暂被索引/杀毒等进程以“不共享删除”方式打开，
    // 对共享冲突做有限重试，避免把可成功发布误判为 FILE_WRITE_FAILED。
    for (int attempt = 0; attempt < 200; ++attempt) {
        sourceHandle =
            CreateFileW(reinterpret_cast<LPCWSTR>(temporaryPath.utf16()), DELETE | GENERIC_READ | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (sourceHandle != INVALID_HANDLE_VALUE)
            break;
        sourceOpenError = GetLastError();
        if (sourceOpenError != ERROR_SHARING_VIOLATION && sourceOpenError != ERROR_ACCESS_DENIED)
            break;
        Sleep(25);
    }
    if (sourceHandle == INVALID_HANDLE_VALUE) {
        // 若源文件仍被其他进程以“不共享删除”方式占用，退化为同一卷上的原子重命名：
        // 不指定 REPLACE_EXISTING，目标已存在时 MoveFileExW 以原子方式失败。
        if (sourceOpenError == ERROR_SHARING_VIOLATION || sourceOpenError == ERROR_ACCESS_DENIED) {
            if (MoveFileExW(reinterpret_cast<LPCWSTR>(temporaryPath.utf16()),
                            reinterpret_cast<LPCWSTR>(outputPath.utf16()), MOVEFILE_WRITE_THROUGH)) {
                CloseHandle(parentHandle);
                return true;
            }
            const DWORD moveError = GetLastError();
            CloseHandle(parentHandle);
            *errorCode = (moveError == ERROR_ALREADY_EXISTS || moveError == ERROR_FILE_EXISTS)
                             ? QStringLiteral("OUTPUT_EXISTS")
                             : QStringLiteral("FILE_WRITE_FAILED:move:%1").arg(moveError);
            return false;
        }
        CloseHandle(parentHandle);
        *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    FILE_ATTRIBUTE_TAG_INFO sourceTag{};
    BY_HANDLE_FILE_INFORMATION sourceInfo{};
    const bool sourceValid =
        GetFileInformationByHandleEx(sourceHandle, FileAttributeTagInfo, &sourceTag, sizeof(sourceTag)) &&
        GetFileInformationByHandle(sourceHandle, &sourceInfo) &&
        !(sourceTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !(sourceInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (!sourceValid) {
        CloseHandle(sourceHandle);
        CloseHandle(parentHandle);
        *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }

    const QString fileName = QFileInfo(outputPath).fileName();
    struct FileLinkInformationBuffer {
        BOOLEAN replaceIfExists;
        HANDLE rootDirectory;
        ULONG fileNameLength;
        WCHAR fileName[1];
    };
    struct NativeIoStatusBlock {
        union {
            LONG status;
            void* pointer;
        };
        ULONG_PTR information;
    };
    using NtSetInformationFileFunction = LONG(NTAPI*)(HANDLE, NativeIoStatusBlock*, void*, ULONG, ULONG);

    const ULONG fileNameBytes = static_cast<ULONG>(fileName.size() * sizeof(wchar_t));
    const size_t linkInfoSize = offsetof(FileLinkInformationBuffer, fileName) + fileNameBytes;
    auto* linkInfo = static_cast<FileLinkInformationBuffer*>(::operator new(linkInfoSize, std::nothrow));
    if (!linkInfo) {
        CloseHandle(sourceHandle);
        CloseHandle(parentHandle);
        *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    linkInfo->replaceIfExists = FALSE;
    linkInfo->rootDirectory = parentHandle;
    linkInfo->fileNameLength = fileNameBytes;
    std::memcpy(linkInfo->fileName, fileName.utf16(), fileNameBytes);

    // shortcut: NtSetInformationFile is undocumented for apps; retain the directory handle until Win32 exposes rooted
    // hard-link creation.
    const auto setInformationFile = reinterpret_cast<NtSetInformationFileFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
    if (!setInformationFile) {
        ::operator delete(linkInfo);
        CloseHandle(sourceHandle);
        CloseHandle(parentHandle);
        *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    NativeIoStatusBlock ioStatus{};
    constexpr ULONG fileLinkInformationClass = 11;
    const LONG linkStatus = setInformationFile(sourceHandle, &ioStatus, linkInfo, static_cast<ULONG>(linkInfoSize),
                                               fileLinkInformationClass);
    ::operator delete(linkInfo);
    CloseHandle(sourceHandle);
    CloseHandle(parentHandle);
    if (linkStatus >= 0)
        return true;
    *errorCode = (static_cast<ULONG>(linkStatus) == 0xC0000035u) ? QStringLiteral("OUTPUT_EXISTS")
                                                                 : QStringLiteral("FILE_WRITE_FAILED");
    return false;
#else
    Q_UNUSED(temporaryPath)
    Q_UNUSED(outputPath)
    Q_UNUSED(expectedParentIdentity)
    *errorCode = QStringLiteral("FILE_WRITE_FAILED");
    return false;
#endif
}

QJsonObject generateOutput(const AnnotationSession& session, const QString& format, const QStringList& categories,
                           QByteArray* bytes) {
    if (format == QLatin1String("labelme")) {
        *bytes = LabelMeExporter::toJsonString(session).toUtf8();
    } else if (format == QLatin1String("yolo_seg")) {
        if (session.imageWidth <= 0 || session.imageHeight <= 0)
            return toolError(QStringLiteral("Session image dimensions are invalid"),
                             QStringLiteral("PRECONDITION_FAILED"));
        for (const AnnotationObject& object : session.annotations) {
            if (!categories.contains(object.label))
                return toolError(QStringLiteral("YOLO categories must include every annotation label"),
                                 QStringLiteral("INVALID_PARAM"));
        }
        *bytes = YoloSegExporter::toJsonString(session, categories).toUtf8();
    } else if (format == QLatin1String("coco")) {
        QString error;
        const QJsonObject document = CocoExporter::toJson(session, categories, &error);
        if (document.isEmpty())
            return toolError(QStringLiteral("COCO export precondition failed"), QStringLiteral("PRECONDITION_FAILED"));
        *bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
    } else {
        return toolError(QStringLiteral("Unsupported annotation export format"), QStringLiteral("INVALID_PARAM"));
    }
    if (bytes->size() > kMaxOutputBytes)
        return toolError(QStringLiteral("Generated annotation export exceeds the 64 MiB output limit"),
                         QStringLiteral("OUTPUT_LIMIT_EXCEEDED"));
    return {};
}
} // namespace

QJsonObject AnnotationSessionExport::newOutputTargetSnapshot(const QString& outputPath, const QString& requiredSuffix) {
    const QFileInfo output(outputPath);
    const QString canonicalParent = QFileInfo(output.absolutePath()).canonicalFilePath();
    QString parentIdentity;
    const bool parentIdentityAvailable =
        !canonicalParent.isEmpty() && directoryIdentity(canonicalParent, &parentIdentity);
    const bool targetSymlink = output.isSymLink();
    const bool targetExists = output.exists();
    return {{QStringLiteral("requestedOutputPath"), outputPath},
            {QStringLiteral("absolute"), output.isAbsolute()},
            {QStringLiteral("pathBytes"), outputPath.toUtf8().size()},
            {QStringLiteral("suffixValid"), output.fileName().endsWith(requiredSuffix, Qt::CaseInsensitive)},
            {QStringLiteral("targetState"),
             targetExists || targetSymlink ? QStringLiteral("exists") : QStringLiteral("absent")},
            {QStringLiteral("outputIsSymlink"), targetSymlink},
            {QStringLiteral("outputCanonicalParent"), canonicalParent},
            {QStringLiteral("outputFileName"), output.fileName()},
            {QStringLiteral("outputParentIdentity"), parentIdentity},
            {QStringLiteral("parentIdentityAvailable"), parentIdentityAvailable},
            {QStringLiteral("parentWritable"), !canonicalParent.isEmpty() && QFileInfo(canonicalParent).isDir() &&
                                                   QFileInfo(canonicalParent).isWritable()}};
}

bool AnnotationSessionExport::publishSessionBytesNoReplace(const QString& outputPath, const QString& requiredSuffix,
                                                           const QJsonObject& approvedTargetSnapshot,
                                                           const QByteArray& bytes, QString* errorCode) {
    if (errorCode)
        errorCode->clear();
    if (bytes.isEmpty() || bytes.size() > kMaxOutputBytes) {
        if (errorCode)
            *errorCode = QStringLiteral("OUTPUT_LIMIT_EXCEEDED");
        return false;
    }
    const QJsonObject current = newOutputTargetSnapshot(outputPath, requiredSuffix);
    if (current != approvedTargetSnapshot) {
        if (errorCode)
            *errorCode = QStringLiteral("CONFIRMATION_EXPIRED");
        return false;
    }
    if (!current.value(QStringLiteral("absolute")).toBool() ||
        current.value(QStringLiteral("pathBytes")).toInt() > kMaxPathBytes ||
        !current.value(QStringLiteral("suffixValid")).toBool() ||
        !current.value(QStringLiteral("parentIdentityAvailable")).toBool() ||
        !current.value(QStringLiteral("parentWritable")).toBool()) {
        if (errorCode)
            *errorCode = QStringLiteral("PRECONDITION_FAILED");
        return false;
    }
    if (current.value(QStringLiteral("targetState")).toString() != QLatin1String("absent") ||
        current.value(QStringLiteral("outputIsSymlink")).toBool()) {
        if (errorCode)
            *errorCode = QStringLiteral("OUTPUT_EXISTS");
        return false;
    }

    const QString canonicalParent = current.value(QStringLiteral("outputCanonicalParent")).toString();
    QTemporaryDir stagingDirectory(QDir(canonicalParent).filePath(QStringLiteral(".deeplux-annotation-XXXXXX")));
    if (!stagingDirectory.isValid()) {
        if (errorCode)
            *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    QFile temporary(QDir(stagingDirectory.path()).filePath(QStringLiteral("session-") +
                                                          QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!temporary.open(QIODevice::ReadWrite) || temporary.write(bytes) != bytes.size() || !temporary.flush()) {
        if (errorCode)
            *errorCode = QStringLiteral("FILE_WRITE_FAILED");
        return false;
    }
    // Windows 上 QTemporaryFile 未释放的共享句柄会让发布路径的 delete 访问失败；先关闭再发布。
    temporary.close();
    const QString temporaryPath = temporary.fileName();
    const QString targetPath =
        QDir(canonicalParent).filePath(current.value(QStringLiteral("outputFileName")).toString());
    if (!publishPreparedFileNoReplace(temporaryPath, targetPath,
                                      current.value(QStringLiteral("outputParentIdentity")).toString(), errorCode))
        return false;
    QFile::remove(temporaryPath);
    return true;
}

QJsonObject AnnotationSessionExport::preflight(const QJsonObject& params) {
    const QString sourcePath = params.value(QStringLiteral("sessionPath")).toString();
    const QString outputPath = params.value(QStringLiteral("outputPath")).toString();
    const QString format = params.value(QStringLiteral("format")).toString();
    if (sourcePath.trimmed().isEmpty() || outputPath.trimmed().isEmpty() ||
        sourcePath.toUtf8().size() > kMaxPathBytes || outputPath.toUtf8().size() > kMaxPathBytes)
        return toolError(QStringLiteral("sessionPath and outputPath must be non-empty and at most 4096 UTF-8 bytes"),
                         QStringLiteral("INVALID_PARAM"));
    if (format != QLatin1String("labelme") && format != QLatin1String("yolo_seg") && format != QLatin1String("coco"))
        return toolError(QStringLiteral("Unsupported annotation export format"), QStringLiteral("INVALID_PARAM"));

    const QFileInfo source(sourcePath);
    if (!source.exists())
        return toolError(QStringLiteral("Annotation session file was not found"), QStringLiteral("FILE_NOT_FOUND"));
    if (!source.isFile() || source.isSymLink() || !source.isReadable())
        return toolError(QStringLiteral("Annotation session must be a readable regular non-symlink file"),
                         QStringLiteral("PRECONDITION_FAILED"));
    if (source.size() > kMaxInputBytes)
        return toolError(QStringLiteral("Annotation session exceeds the 64 MiB input limit"),
                         QStringLiteral("INPUT_LIMIT_EXCEEDED"));

    const QFileInfo output(outputPath);
    const QString parentPath = QFileInfo(output.absolutePath()).canonicalFilePath();
    if (parentPath.isEmpty() || !QFileInfo(parentPath).isDir() || !QFileInfo(parentPath).isWritable())
        return toolError(QStringLiteral("Output parent directory must already exist and be writable"),
                         QStringLiteral("PRECONDITION_FAILED"));
    if (output.exists() || output.isSymLink())
        return toolError(QStringLiteral("Output target already exists"), QStringLiteral("OUTPUT_EXISTS"));
    if (output.suffix().compare(expectedSuffix(format), Qt::CaseInsensitive) != 0)
        return toolError(QStringLiteral("Output file extension does not match the selected format"),
                         QStringLiteral("INVALID_PARAM"));
    const QString canonicalOutput = QDir(parentPath).filePath(output.fileName());
    if (source.canonicalFilePath() == canonicalOutput)
        return toolError(QStringLiteral("Session source and export target must be different files"),
                         QStringLiteral("PRECONDITION_FAILED"));
    const QJsonArray categories = params.value(QStringLiteral("categories")).toArray();
    if (categories.size() > 100)
        return toolError(QStringLiteral("At most 100 categories may be specified"), QStringLiteral("INVALID_PARAM"));
    QSet<QString> seenCategories;
    for (const QJsonValue& value : categories) {
        const QString category = value.toString();
        if (category.trimmed().isEmpty() || category.toUtf8().size() > 128 || seenCategories.contains(category))
            return toolError(QStringLiteral("Categories must be unique non-empty strings of at most 128 UTF-8 bytes"),
                             QStringLiteral("INVALID_PARAM"));
        seenCategories.insert(category);
    }
    if (format == QLatin1String("labelme") && !categories.isEmpty())
        return toolError(QStringLiteral("LabelMe export does not accept categories"), QStringLiteral("INVALID_PARAM"));
    if (format == QLatin1String("yolo_seg") && categories.isEmpty())
        return toolError(QStringLiteral("YOLO Seg requires an explicit category list"),
                         QStringLiteral("INVALID_PARAM"));
    return {};
}

bool AnnotationSessionExport::publishPreparedFileNoReplace(const QString& temporaryPath, const QString& outputPath,
                                                           const QString& expectedParentIdentity, QString* errorCode) {
    return publishNoReplace(temporaryPath, outputPath, expectedParentIdentity, errorCode);
}

QJsonObject AnnotationSessionExport::confirmationSnapshot(const QJsonObject& params) {
    return captureSnapshot(params);
}

QJsonObject AnnotationSessionExport::confirmationSnapshot(const QJsonObject& params, QByteArray* sourceBytes) {
    return captureSnapshot(params, sourceBytes);
}

QJsonObject AnnotationSessionExport::execute(const QJsonObject& params, const QJsonObject& approvedSnapshot) {
    if (approvedSnapshot.isEmpty())
        return toolError(QStringLiteral("Annotation export requires an approved confirmation snapshot"),
                         QStringLiteral("PRECONDITION_FAILED"));
    QByteArray sourceBytes;
    const QJsonObject currentSnapshot = captureSnapshot(params, &sourceBytes);
    if (currentSnapshot != approvedSnapshot)
        return toolError(QStringLiteral("Annotation export confirmation is stale"),
                         QStringLiteral("CONFIRMATION_EXPIRED"));
    if (currentSnapshot.value(QStringLiteral("sourceState")).toString() == QLatin1String("oversized"))
        return toolError(QStringLiteral("Annotation session exceeds the 64 MiB input limit"),
                         QStringLiteral("INPUT_LIMIT_EXCEEDED"));
    if (currentSnapshot.value(QStringLiteral("sourceState")).toString() == QLatin1String("missing"))
        return toolError(QStringLiteral("Annotation session file was not found"), QStringLiteral("FILE_NOT_FOUND"));
    if (currentSnapshot.value(QStringLiteral("sourceState")).toString() != QLatin1String("readable"))
        return toolError(QStringLiteral("Annotation session is not a readable regular file"),
                         QStringLiteral("PRECONDITION_FAILED"));
    if (currentSnapshot.value(QStringLiteral("targetState")).toString() == QLatin1String("exists") ||
        currentSnapshot.value(QStringLiteral("outputIsSymlink")).toBool())
        return toolError(QStringLiteral("Output target already exists"), QStringLiteral("OUTPUT_EXISTS"));
    if (currentSnapshot.value(QStringLiteral("targetState")).toString() != QLatin1String("absent"))
        return toolError(QStringLiteral("Output parent directory identity could not be verified"),
                         QStringLiteral("PRECONDITION_FAILED"));

    const QString format = params.value(QStringLiteral("format")).toString();
    const QString outputPath = QDir(currentSnapshot.value(QStringLiteral("outputCanonicalParent")).toString())
                                   .filePath(currentSnapshot.value(QStringLiteral("outputFileName")).toString());
    if (QFileInfo(outputPath).suffix().compare(expectedSuffix(format), Qt::CaseInsensitive) != 0)
        return toolError(QStringLiteral("Output file extension does not match the selected format"),
                         QStringLiteral("INVALID_PARAM"));
    if (QFileInfo(currentSnapshot.value(QStringLiteral("sourceCanonicalPath")).toString()).canonicalFilePath() ==
        QFileInfo(outputPath).absoluteFilePath())
        return toolError(QStringLiteral("Session source and export target must be different files"),
                         QStringLiteral("PRECONDITION_FAILED"));

    AnnotationSession::LoadResult loadResult = AnnotationSession::LoadResult::InvalidSession;
    QString loadError;
    const AnnotationSession session = AnnotationSession::loadFromBytes(sourceBytes, &loadResult, &loadError);
    if (loadResult != AnnotationSession::LoadResult::Success)
        return toolError(QStringLiteral("Annotation session is invalid"), QStringLiteral("INVALID_SESSION"));

    QStringList categories;
    for (const QJsonValue& value : params.value(QStringLiteral("categories")).toArray()) {
        const QString category = value.toString();
        if (category.trimmed().isEmpty() || category.toUtf8().size() > 128 || categories.contains(category))
            return toolError(QStringLiteral("Categories must be unique non-empty strings of at most 128 UTF-8 bytes"),
                             QStringLiteral("INVALID_PARAM"));
        categories.append(category);
    }
    if (categories.size() > 100)
        return toolError(QStringLiteral("At most 100 categories may be specified"), QStringLiteral("INVALID_PARAM"));
    if (format == QLatin1String("labelme") && !categories.isEmpty())
        return toolError(QStringLiteral("LabelMe export does not accept categories"), QStringLiteral("INVALID_PARAM"));
    if (format == QLatin1String("yolo_seg") && categories.isEmpty())
        return toolError(QStringLiteral("YOLO Seg requires an explicit category list"),
                         QStringLiteral("INVALID_PARAM"));

    QByteArray outputBytes;
    const QJsonObject generationError = generateOutput(session, format, categories, &outputBytes);
    if (!generationError.isEmpty())
        return generationError;

    QJsonObject result{{QStringLiteral("status"), QStringLiteral("exported")},
                       {QStringLiteral("format"), format},
                       {QStringLiteral("outputPath"), outputPath},
                       {QStringLiteral("bytes"), outputBytes.size()},
                       {QStringLiteral("sha256"),
                        QString::fromLatin1(QCryptographicHash::hash(outputBytes, QCryptographicHash::Sha256).toHex())},
                       {QStringLiteral("objectCount"), session.annotations.size()}};
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kMaxResponseBytes)
        return toolError(QStringLiteral("Export response exceeds the response size limit"),
                         QStringLiteral("OUTPUT_LIMIT_EXCEEDED"));

    QTemporaryDir stagingDirectory(QDir(currentSnapshot.value(QStringLiteral("outputCanonicalParent")).toString())
                                       .filePath(QStringLiteral(".deeplux-export-XXXXXX")));
    if (!stagingDirectory.isValid())
        return toolError(QStringLiteral("Could not create a private staging directory"),
                         QStringLiteral("FILE_WRITE_FAILED"));
    QFile temporary(QDir(stagingDirectory.path()).filePath(QStringLiteral("payload-") + QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!temporary.open(QIODevice::ReadWrite) || temporary.write(outputBytes) != outputBytes.size() || !temporary.flush())
        return toolError(QStringLiteral("Could not write complete annotation export"),
                         QStringLiteral("FILE_WRITE_FAILED"));
    // Windows 上硬链接发布需要对源文件持有 DELETE 权限的句柄；先关闭 Qt 临时文件句柄，
    // 否则 Qt 的共享模式会让发布路径的 CreateFileW(DELETE) 因共享冲突失败。
    temporary.close();
    const QString temporaryPath = temporary.fileName();
    QString publishError;
    if (!publishPreparedFileNoReplace(temporaryPath, outputPath,
                                      currentSnapshot.value(QStringLiteral("outputParentIdentity")).toString(),
                                      &publishError)) {
        if (publishError == QLatin1String("CONFIRMATION_EXPIRED"))
            return toolError(QStringLiteral("Output parent directory changed after confirmation"), publishError);
        if (publishError == QLatin1String("OUTPUT_EXISTS"))
            return toolError(QStringLiteral("Output target was created by another process"), publishError);
        return toolError(QStringLiteral("Atomic no-replace publication is not available for this destination"),
                         QStringLiteral("FILE_WRITE_FAILED"));
    }
    temporary.close();
    QFile::remove(temporaryPath);
    return result;
}

} // namespace DeepLux
