#pragma once

#include <QString>

class QJsonObject;

namespace DeepLux {

QString agentFileOutputAuthorizationError(const QJsonObject& authorization, const QString& instanceId,
                                          const QString& pluginId, const QJsonObject& currentParams);

} // namespace DeepLux
