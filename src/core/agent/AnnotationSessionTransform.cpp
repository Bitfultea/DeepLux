#include "AnnotationSessionTransform.h"

#include "AnnotationSessionExport.h"
#include "model/Annotation.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QTemporaryDir>
#include <QTemporaryFile>

namespace DeepLux {

namespace {
constexpr qint64 kMaxInputBytes = 64LL * 1024 * 1024;
constexpr qint64 kMaxOutputBytes = 64LL * 1024 * 1024;
constexpr int kMaxPathBytes = 4096;
constexpr int kMaxResponseBytes = 65536;

QJsonObject errorResult(const QString& message, const QString& code) {
    return {
        {QStringLiteral("error"), message}, {QStringLiteral("errorCode"), code}, {QStringLiteral("retryable"), false}};
}

QString operation(const QJsonObject& params) {
    return params.value(QStringLiteral("operation")).toString();
}

QStringList objectIds(const QJsonObject& params) {
    QStringList ids;
    for (const QJsonValue& value : params.value(QStringLiteral("objectIds")).toArray())
        ids.append(value.toString());
    return ids;
}

QJsonObject addOperationInputs(QJsonObject snapshot, const QJsonObject& params) {
    snapshot.insert(QStringLiteral("operation"), params.value(QStringLiteral("operation")));
    snapshot.insert(QStringLiteral("objectIds"), params.value(QStringLiteral("objectIds")));
    if (params.contains(QStringLiteral("label")))
        snapshot.insert(QStringLiteral("label"), params.value(QStringLiteral("label")).toString().trimmed());
    return snapshot;
}

QJsonObject validateSnapshotAndSession(const QJsonObject& params, const QJsonObject& snapshot,
                                       const QByteArray& sourceBytes, AnnotationSession* session) {
    const QString sourceState = snapshot.value(QStringLiteral("sourceState")).toString();
    if (sourceState == QLatin1String("missing"))
        return errorResult(QStringLiteral("Annotation session file was not found"), QStringLiteral("FILE_NOT_FOUND"));
    if (sourceState == QLatin1String("oversized"))
        return errorResult(QStringLiteral("Annotation session exceeds the 64 MiB input limit"),
                           QStringLiteral("INPUT_LIMIT_EXCEEDED"));
    if (sourceState != QLatin1String("readable") || !snapshot.value(QStringLiteral("sourceIdentity")).isString())
        return errorResult(QStringLiteral("Annotation session must be readable and have a stable file identity"),
                           QStringLiteral("PRECONDITION_FAILED"));

    AnnotationSession::LoadResult loadResult = AnnotationSession::LoadResult::InvalidSession;
    QString loadError;
    *session = AnnotationSession::loadFromBytes(sourceBytes, &loadResult, &loadError);
    if (loadResult == AnnotationSession::LoadResult::SizeLimitExceeded)
        return errorResult(QStringLiteral("Annotation session exceeds the 64 MiB input limit"),
                           QStringLiteral("INPUT_LIMIT_EXCEEDED"));
    if (loadResult != AnnotationSession::LoadResult::Success)
        return errorResult(QStringLiteral("Annotation session is invalid"), QStringLiteral("INVALID_SESSION"));

    const QStringList ids = objectIds(params);
    for (const QString& id : ids) {
        if (!session->findById(id))
            return errorResult(QStringLiteral("One or more annotation object IDs were not found"),
                               QStringLiteral("ANNOTATION_ID_NOT_FOUND"));
    }
    return {};
}

QJsonObject validateInputs(const QJsonObject& params) {
    const QString sourcePath = params.value(QStringLiteral("sessionPath")).toString();
    const QString outputPath = params.value(QStringLiteral("outputPath")).toString();
    const QString op = operation(params);
    if (sourcePath.trimmed().isEmpty() || outputPath.trimmed().isEmpty() ||
        sourcePath.toUtf8().size() > kMaxPathBytes || outputPath.toUtf8().size() > kMaxPathBytes)
        return errorResult(QStringLiteral("sessionPath and outputPath must be non-empty and at most 4096 UTF-8 bytes"),
                           QStringLiteral("INVALID_PARAM"));
    if (op != QLatin1String("set_label") && op != QLatin1String("delete_objects"))
        return errorResult(QStringLiteral("Unsupported annotation session operation"), QStringLiteral("INVALID_PARAM"));
    const QJsonValue idValue = params.value(QStringLiteral("objectIds"));
    if (!idValue.isArray() || idValue.toArray().isEmpty() || idValue.toArray().size() > 100)
        return errorResult(QStringLiteral("objectIds must contain between 1 and 100 IDs"),
                           QStringLiteral("INVALID_PARAM"));
    QSet<QString> uniqueIds;
    for (const QJsonValue& value : idValue.toArray()) {
        if (!value.isString())
            return errorResult(QStringLiteral("objectIds must contain strings"), QStringLiteral("INVALID_PARAM"));
        const QString id = value.toString();
        if (id.trimmed().isEmpty() || id.toUtf8().size() > 256 || uniqueIds.contains(id))
            return errorResult(QStringLiteral("objectIds must be unique non-empty strings of at most 256 UTF-8 bytes"),
                               QStringLiteral("INVALID_PARAM"));
        uniqueIds.insert(id);
    }
    if (op == QLatin1String("set_label")) {
        const QJsonValue labelValue = params.value(QStringLiteral("label"));
        if (!labelValue.isString())
            return errorResult(QStringLiteral("set_label requires a string label"), QStringLiteral("INVALID_PARAM"));
        const QString label = labelValue.toString().trimmed();
        if (label.isEmpty() || label.toUtf8().size() > 128)
            return errorResult(QStringLiteral("label must be non-empty and at most 128 UTF-8 bytes after trimming"),
                               QStringLiteral("INVALID_PARAM"));
    } else if (params.contains(QStringLiteral("label"))) {
        return errorResult(QStringLiteral("delete_objects does not accept label"), QStringLiteral("INVALID_PARAM"));
    }

    const QFileInfo output(outputPath);
    if (!output.fileName().endsWith(QStringLiteral(".deeplux-anno.json"), Qt::CaseInsensitive))
        return errorResult(QStringLiteral("outputPath must end in .deeplux-anno.json"),
                           QStringLiteral("INVALID_PARAM"));
    const QString parentPath = QFileInfo(output.absolutePath()).canonicalFilePath();
    if (parentPath.isEmpty() || !QFileInfo(parentPath).isDir() || !QFileInfo(parentPath).isWritable())
        return errorResult(QStringLiteral("Output parent directory must already exist and be writable"),
                           QStringLiteral("PRECONDITION_FAILED"));
    const QFileInfo source(sourcePath);
    if (source.canonicalFilePath() == QDir(parentPath).filePath(output.fileName()))
        return errorResult(QStringLiteral("Session source and output target must be different files"),
                           QStringLiteral("PRECONDITION_FAILED"));
    return {};
}

} // namespace

QJsonObject AnnotationSessionTransform::preflight(const QJsonObject& params) {
    const QJsonObject inputError = validateInputs(params);
    if (!inputError.isEmpty())
        return inputError;
    QByteArray sourceBytes;
    const QJsonObject snapshot =
        addOperationInputs(AnnotationSessionExport::confirmationSnapshot(params, &sourceBytes), params);
    const QString targetState = snapshot.value(QStringLiteral("targetState")).toString();
    if (targetState == QLatin1String("exists") || snapshot.value(QStringLiteral("outputIsSymlink")).toBool())
        return errorResult(QStringLiteral("Output target already exists"), QStringLiteral("OUTPUT_EXISTS"));
    if (targetState != QLatin1String("absent"))
        return errorResult(QStringLiteral("Output parent directory identity could not be verified"),
                           QStringLiteral("PRECONDITION_FAILED"));
    AnnotationSession session;
    return validateSnapshotAndSession(params, snapshot, sourceBytes, &session);
}

QJsonObject AnnotationSessionTransform::confirmationSnapshot(const QJsonObject& params) {
    return addOperationInputs(AnnotationSessionExport::confirmationSnapshot(params), params);
}

QJsonObject AnnotationSessionTransform::execute(const QJsonObject& params, const QJsonObject& approvedSnapshot) {
    if (approvedSnapshot.isEmpty())
        return errorResult(QStringLiteral("Session transformation requires an approved confirmation snapshot"),
                           QStringLiteral("PRECONDITION_FAILED"));
    QByteArray sourceBytes;
    const QJsonObject currentSnapshot =
        addOperationInputs(AnnotationSessionExport::confirmationSnapshot(params, &sourceBytes), params);
    if (currentSnapshot != approvedSnapshot)
        return errorResult(QStringLiteral("Annotation session confirmation is stale"),
                           QStringLiteral("CONFIRMATION_EXPIRED"));
    const QJsonObject inputError = validateInputs(params);
    if (!inputError.isEmpty())
        return inputError;
    if (currentSnapshot.value(QStringLiteral("targetState")).toString() != QLatin1String("absent"))
        return errorResult(QStringLiteral("Output target state could not be verified"),
                           QStringLiteral("PRECONDITION_FAILED"));

    AnnotationSession session;
    const QJsonObject sessionError = validateSnapshotAndSession(params, currentSnapshot, sourceBytes, &session);
    if (!sessionError.isEmpty())
        return sessionError;

    QJsonParseError parseError;
    const QJsonDocument sourceDocument = QJsonDocument::fromJson(sourceBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !sourceDocument.isObject())
        return errorResult(QStringLiteral("Annotation session is invalid"), QStringLiteral("INVALID_SESSION"));
    QJsonObject root = sourceDocument.object();
    const QString op = operation(params);
    const QStringList ids = objectIds(params);
    const QSet<QString> selectedIds(ids.begin(), ids.end());
    const QString label = params.value(QStringLiteral("label")).toString().trimmed();
    QJsonArray transformedAnnotations;
    int changedCount = 0;
    for (const QJsonValue& value : root.value(QStringLiteral("annotations")).toArray()) {
        QJsonObject annotation = value.toObject();
        const QString id = annotation.value(QStringLiteral("id")).toString();
        if (selectedIds.contains(id)) {
            if (op == QLatin1String("delete_objects")) {
                ++changedCount;
                continue;
            }
            if (annotation.value(QStringLiteral("label")).toString() != label) {
                annotation.insert(QStringLiteral("label"), label);
                ++changedCount;
            }
        }
        transformedAnnotations.append(annotation);
    }
    root.insert(QStringLiteral("annotations"), transformedAnnotations);
    const QByteArray outputBytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (outputBytes.size() > kMaxOutputBytes)
        return errorResult(QStringLiteral("Transformed session exceeds the 64 MiB output limit"),
                           QStringLiteral("OUTPUT_LIMIT_EXCEEDED"));
    AnnotationSession::LoadResult outputLoadResult = AnnotationSession::LoadResult::InvalidSession;
    QString outputLoadError;
    const AnnotationSession outputSession =
        AnnotationSession::loadFromBytes(outputBytes, &outputLoadResult, &outputLoadError);
    if (outputLoadResult != AnnotationSession::LoadResult::Success)
        return errorResult(QStringLiteral("Transformed session failed strict validation"),
                           QStringLiteral("INVALID_SESSION"));

    const QString outputPath = QDir(currentSnapshot.value(QStringLiteral("outputCanonicalParent")).toString())
                                   .filePath(currentSnapshot.value(QStringLiteral("outputFileName")).toString());
    QJsonObject result{
        {QStringLiteral("status"), QStringLiteral("transformed")},
        {QStringLiteral("operation"), op},
        {QStringLiteral("outputPath"), outputPath},
        {QStringLiteral("objectCount"), outputSession.annotations.size()},
        {QStringLiteral("changedCount"), changedCount},
        {QStringLiteral("sha256"),
         QString::fromLatin1(QCryptographicHash::hash(outputBytes, QCryptographicHash::Sha256).toHex())}};
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kMaxResponseBytes)
        return errorResult(QStringLiteral("Transformation response exceeds the response size limit"),
                           QStringLiteral("OUTPUT_LIMIT_EXCEEDED"));

    QTemporaryDir stagingDirectory(QDir(currentSnapshot.value(QStringLiteral("outputCanonicalParent")).toString())
                                       .filePath(QStringLiteral(".deeplux-transform-XXXXXX")));
    if (!stagingDirectory.isValid())
        return errorResult(QStringLiteral("Could not create a private staging directory"),
                           QStringLiteral("FILE_WRITE_FAILED"));
    QFile temporary(QDir(stagingDirectory.path())
                        .filePath(QStringLiteral("payload-") + QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!temporary.open(QIODevice::ReadWrite) || temporary.write(outputBytes) != outputBytes.size() ||
        !temporary.flush())
        return errorResult(QStringLiteral("Could not write complete transformed session"),
                           QStringLiteral("FILE_WRITE_FAILED"));
    // 同 Export：Windows 硬链接发布需要先释放 Qt 临时文件句柄，避免共享冲突。
    temporary.close();
    const QString temporaryPath = temporary.fileName();
    QString publishError;
    if (!AnnotationSessionExport::publishPreparedFileNoReplace(
            temporaryPath, outputPath, currentSnapshot.value(QStringLiteral("outputParentIdentity")).toString(),
            &publishError)) {
        if (publishError == QLatin1String("CONFIRMATION_EXPIRED"))
            return errorResult(QStringLiteral("Output parent directory changed after confirmation"), publishError);
        if (publishError == QLatin1String("OUTPUT_EXISTS"))
            return errorResult(QStringLiteral("Output target was created by another process"), publishError);
        return errorResult(QStringLiteral("Atomic no-replace publication is not available for this destination"),
                           QStringLiteral("FILE_WRITE_FAILED"));
    }
    temporary.close();
    QFile::remove(temporaryPath);
    return result;
}

} // namespace DeepLux
