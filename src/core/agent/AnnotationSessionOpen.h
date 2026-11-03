#pragma once

#include "core/model/Annotation.h"

#include <QImage>
#include <QJsonObject>
#include <QString>

namespace DeepLux {

struct AnnotationSessionOpenCandidate {
    AnnotationSession session;
    QImage boundImage;
    QString sessionPath;
    QString imagePath;
    bool imageBound = false;
    bool legacyImageBindingUnverified = false;
    QJsonObject snapshot;
};

class AnnotationSessionOpen {
public:
    static constexpr qint64 kMaxSessionBytes = 64LL * 1024 * 1024;
    static constexpr qint64 kMaxImageBytes = 512LL * 1024 * 1024;

    // Reads each source once, then parses/decodes from those exact byte buffers.
    static bool capture(const QString& sessionPath, AnnotationSessionOpenCandidate* candidate, QString* errorCode,
                        QString* errorMessage = nullptr);
};

} // namespace DeepLux
