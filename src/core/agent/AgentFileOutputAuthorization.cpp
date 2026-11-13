#include "AgentFileOutputAuthorization.h"

#include "platform/PathUtils.h"

#include <QJsonObject>

namespace DeepLux {

QString agentFileOutputAuthorizationError(const QJsonObject& authorization, const QString& instanceId,
                                          const QString& pluginId, const QJsonObject& currentParams) {
    if (!authorization.value(QStringLiteral("active")).toBool())
        return {};

    const QString authorizationId = authorization.value(QStringLiteral("executionAuthorizationId")).toString();
    const QString configuredPath = currentParams.value(QStringLiteral("filePath")).toString();
    const QString approvedPath = authorization.value(QStringLiteral("approvedConfiguredPath")).toString();
    const QString approvedTarget = authorization.value(QStringLiteral("resolvedTarget")).toString();
    const QString currentTarget = PathUtils::resolveFileTargetPath(configuredPath);
    const bool authorized = authorization.value(QStringLiteral("authorized")).toBool() &&
                            authorization.value(QStringLiteral("authorizedModuleId")).toString() == instanceId &&
                            authorization.value(QStringLiteral("approvedPluginId")).toString() == pluginId &&
                            !authorization.value(QStringLiteral("confirmationId")).toString().isEmpty() &&
                            !authorization.value(QStringLiteral("callId")).toString().isEmpty() &&
                            !authorizationId.isEmpty() &&
                            authorization.value(QStringLiteral("approvedParams")).toObject() == currentParams &&
                            configuredPath == approvedPath && !configuredPath.trimmed().isEmpty() &&
                            !approvedTarget.isEmpty() && !currentTarget.isEmpty() && currentTarget == approvedTarget;
    if (authorized)
        return {};

    return QStringLiteral("codeName=CONFIRMED_TARGET_CHANGED instanceId=%1 authorizationId=%2")
        .arg(instanceId, authorizationId);
}

} // namespace DeepLux
