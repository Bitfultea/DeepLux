#pragma once

#include <QByteArray>
#include <QJsonObject>

namespace DeepLux {

class AnnotationSessionExport {
public:
    static QJsonObject newOutputTargetSnapshot(const QString& outputPath, const QString& requiredSuffix);
    static bool publishSessionBytesNoReplace(const QString& outputPath, const QString& requiredSuffix,
                                             const QJsonObject& approvedTargetSnapshot, const QByteArray& bytes,
                                             QString* errorCode);
    static QJsonObject preflight(const QJsonObject& params);
    static QJsonObject confirmationSnapshot(const QJsonObject& params);
    static QJsonObject confirmationSnapshot(const QJsonObject& params, QByteArray* sourceBytes);
    static QJsonObject execute(const QJsonObject& params, const QJsonObject& approvedSnapshot);
    static bool publishPreparedFileNoReplace(const QString& temporaryPath, const QString& outputPath,
                                             const QString& expectedParentIdentity, QString* errorCode);
};

} // namespace DeepLux
