#pragma once

#include <QJsonValue>
#include <QObject>
#include <QString>

namespace DeepLux {

/** Optional camera extension that validates one requested parameter value without changing the device. */
class ICameraParameterControl {
public:
    virtual ~ICameraParameterControl() = default;
    virtual bool validateParameter(const QString& parameter, const QJsonValue& value, QString* errorCode) const = 0;
};

} // namespace DeepLux

Q_DECLARE_INTERFACE(DeepLux::ICameraParameterControl, "com.deeplux.ICameraParameterControl/1.0")
