#pragma once

#include <QJsonObject>

namespace DeepLux {

class AnnotationSessionTransform {
public:
    static QJsonObject preflight(const QJsonObject& params);
    static QJsonObject confirmationSnapshot(const QJsonObject& params);
    static QJsonObject execute(const QJsonObject& params, const QJsonObject& approvedSnapshot);
};

} // namespace DeepLux
