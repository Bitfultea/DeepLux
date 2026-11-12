#pragma once

#include <QJsonValue>
#include <QMutex>
#include <QString>

namespace DeepLux {

class ICamera;

struct CameraParameterWriteResult {
    QString status = QStringLiteral("error");
    QString errorCode;
    QString parameter;
    QJsonValue before = QJsonValue(QJsonValue::Undefined);
    QJsonValue requested;
    QJsonValue actual = QJsonValue(QJsonValue::Undefined);
    QString sideEffectStatus = QStringLiteral("none");
    bool setterCalled = false;
    bool bindingChanged = false;
    quint64 bindingVersion = 0;
};

// Serializes parameter writes with notifications that invalidate previously read details.
class CameraParameterBinding final {
public:
    quint64 generation() const;
    quint64 invalidate();
    CameraParameterWriteResult write(ICamera* camera, quint64 expectedGeneration, const QString& deviceId,
                                     const QString& serialNumber, const QString& parameter, const QJsonValue& value);

private:
    mutable QMutex m_mutex;
    quint64 m_generation = 0;
};

CameraParameterWriteResult writeCameraParameter(ICamera* camera, const QString& deviceId, const QString& serialNumber,
                                                const QString& parameter, const QJsonValue& value);

} // namespace DeepLux
