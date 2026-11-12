#include "CameraParameterWrite.h"

#include "core/interface/ICamera.h"
#include "core/interface/ICameraParameterControl.h"

#include <QJsonObject>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <limits>

namespace DeepLux {

namespace {

bool readValue(ICamera* camera, const QString& parameter, QJsonValue* value) {
    if (parameter == QLatin1String("exposureTime"))
        *value = camera->exposureTime();
    else if (parameter == QLatin1String("gain"))
        *value = camera->gain();
    else if (parameter == QLatin1String("frameRate"))
        *value = camera->frameRate();
    else if (parameter == QLatin1String("triggerMode")) {
        switch (camera->triggerMode()) {
        case TriggerMode::Software:
            *value = QStringLiteral("software");
            break;
        case TriggerMode::Hardware:
            *value = QStringLiteral("hardware");
            break;
        case TriggerMode::Continuous:
            *value = QStringLiteral("continuous");
            break;
        default:
            return false;
        }
    } else if (parameter == QLatin1String("roi")) {
        const QRect roi = camera->roi();
        if (roi.x() < 0 || roi.y() < 0 || roi.width() <= 0 || roi.height() <= 0)
            return false;
        *value = QJsonObject{{QStringLiteral("x"), roi.x()},
                             {QStringLiteral("y"), roi.y()},
                             {QStringLiteral("width"), roi.width()},
                             {QStringLiteral("height"), roi.height()}};
    } else {
        return false;
    }
    if (value->isDouble() && !std::isfinite(value->toDouble()))
        return false;
    return true;
}

bool validRoi(const QJsonValue& value) {
    if (!value.isObject())
        return false;
    const QJsonObject roi = value.toObject();
    if (roi.size() != 4 || !roi.contains(QStringLiteral("x")) || !roi.contains(QStringLiteral("y")) ||
        !roi.contains(QStringLiteral("width")) || !roi.contains(QStringLiteral("height")))
        return false;
    for (const QString& key :
         {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")}) {
        const QJsonValue item = roi.value(key);
        if (!item.isDouble() || !std::isfinite(item.toDouble()) || std::floor(item.toDouble()) != item.toDouble() ||
            item.toDouble() > std::numeric_limits<int>::max() || item.toDouble() < std::numeric_limits<int>::min())
            return false;
    }
    return roi.value(QStringLiteral("x")).toInt() >= 0 && roi.value(QStringLiteral("y")).toInt() >= 0 &&
           roi.value(QStringLiteral("width")).toInt() > 0 && roi.value(QStringLiteral("height")).toInt() > 0;
}

bool validRequested(const QString& parameter, const QJsonValue& value) {
    if (parameter == QLatin1String("exposureTime") || parameter == QLatin1String("gain") ||
        parameter == QLatin1String("frameRate"))
        return value.isDouble() && std::isfinite(value.toDouble());
    if (parameter == QLatin1String("triggerMode"))
        return value.isString() &&
               (value.toString() == QLatin1String("software") || value.toString() == QLatin1String("hardware") ||
                value.toString() == QLatin1String("continuous"));
    return parameter == QLatin1String("roi") && validRoi(value);
}

bool supports(const CameraCapabilities& capabilities, const QString& parameter, const QJsonValue& value) {
    if (parameter == QLatin1String("exposureTime"))
        return capabilities.exposureControl;
    if (parameter == QLatin1String("gain"))
        return capabilities.gainControl;
    if (parameter == QLatin1String("frameRate"))
        return capabilities.frameRateControl;
    if (parameter == QLatin1String("roi"))
        return capabilities.roiControl;
    if (parameter == QLatin1String("triggerMode")) {
        if (value.toString() == QLatin1String("software"))
            return capabilities.softwareTrigger;
        if (value.toString() == QLatin1String("hardware"))
            return capabilities.hardwareTrigger;
        return capabilities.continuousMode;
    }
    return false;
}

bool equalValue(const QString& parameter, const QJsonValue& expected, const QJsonValue& actual) {
    if (parameter == QLatin1String("exposureTime") || parameter == QLatin1String("gain") ||
        parameter == QLatin1String("frameRate")) {
        const double requested = expected.toDouble();
        const double observed = actual.toDouble(std::numeric_limits<double>::quiet_NaN());
        return std::isfinite(observed) && std::abs(observed - requested) <= std::max(1e-6, std::abs(requested) * 1e-6);
    }
    return expected == actual;
}

void apply(ICamera* camera, const QString& parameter, const QJsonValue& value) {
    if (parameter == QLatin1String("exposureTime"))
        camera->setExposureTime(value.toDouble());
    else if (parameter == QLatin1String("gain"))
        camera->setGain(value.toDouble());
    else if (parameter == QLatin1String("frameRate"))
        camera->setFrameRate(value.toDouble());
    else if (parameter == QLatin1String("triggerMode")) {
        const QString mode = value.toString();
        camera->setTriggerMode(mode == QLatin1String("software")   ? TriggerMode::Software
                               : mode == QLatin1String("hardware") ? TriggerMode::Hardware
                                                                   : TriggerMode::Continuous);
    } else {
        const QJsonObject roi = value.toObject();
        camera->setRoi(roi.value(QStringLiteral("x")).toInt(), roi.value(QStringLiteral("y")).toInt(),
                       roi.value(QStringLiteral("width")).toInt(), roi.value(QStringLiteral("height")).toInt());
    }
}

} // namespace

CameraParameterWriteResult writeCameraParameter(ICamera* camera, const QString& deviceId, const QString& serialNumber,
                                                const QString& parameter, const QJsonValue& value) {
    CameraParameterWriteResult result;
    result.parameter = parameter;
    result.requested = value;
    if (!camera) {
        result.errorCode = QStringLiteral("CAMERA_NOT_FOUND");
        return result;
    }
    try {
        if (!camera->isConnected()) {
            result.errorCode = QStringLiteral("CAMERA_NOT_CONNECTED");
            return result;
        }
        if (!validRequested(parameter, value)) {
            result.errorCode = QStringLiteral("INVALID_PARAM");
            return result;
        }
        if (!supports(camera->capabilities(), parameter, value)) {
            result.errorCode = QStringLiteral("CAMERA_PARAMETER_UNSUPPORTED");
            return result;
        }
        auto* control = qobject_cast<ICameraParameterControl*>(camera);
        if (!control) {
            result.errorCode = QStringLiteral("CAMERA_PARAMETER_RANGE_UNAVAILABLE");
            return result;
        }
        QString validationError;
        if (!control->validateParameter(parameter, value, &validationError)) {
            result.errorCode =
                validationError.isEmpty() ? QStringLiteral("CAMERA_PARAMETER_OUT_OF_RANGE") : validationError;
            return result;
        }
    } catch (...) {
        result.errorCode = QStringLiteral("CAMERA_PARAMETER_VALIDATE_FAILED");
        return result;
    }

    try {
        if (!readValue(camera, parameter, &result.before)) {
            result.errorCode = QStringLiteral("CAMERA_PARAMETER_READ_FAILED");
            return result;
        }
    } catch (...) {
        result.errorCode = QStringLiteral("CAMERA_PARAMETER_READ_FAILED");
        return result;
    }

    try {
        if (!camera->isConnected()) {
            result.errorCode = QStringLiteral("CAMERA_NOT_CONNECTED");
            return result;
        }
        if (camera->deviceId() != deviceId || camera->serialNumber() != serialNumber) {
            result.errorCode = QStringLiteral("CAMERA_TARGET_CHANGED");
            return result;
        }
    } catch (...) {
        result.errorCode = QStringLiteral("CAMERA_PARAMETER_VALIDATE_FAILED");
        return result;
    }

    result.setterCalled = true;
    result.bindingChanged = true;
    try {
        apply(camera, parameter, value);
    } catch (...) {
        result.errorCode = QStringLiteral("CAMERA_PARAMETER_WRITE_UNCONFIRMED");
        result.sideEffectStatus = QStringLiteral("uncertain");
        try {
            readValue(camera, parameter, &result.actual);
        } catch (...) {
            result.actual = QJsonValue();
        }
        return result;
    }
    try {
        if (!readValue(camera, parameter, &result.actual)) {
            result.errorCode = QStringLiteral("CAMERA_PARAMETER_WRITE_UNCONFIRMED");
            result.sideEffectStatus = QStringLiteral("uncertain");
            return result;
        }
    } catch (...) {
        result.errorCode = QStringLiteral("CAMERA_PARAMETER_WRITE_UNCONFIRMED");
        result.sideEffectStatus = QStringLiteral("uncertain");
        return result;
    }

    result.sideEffectStatus = QStringLiteral("changed_or_clamped");
    if (!equalValue(parameter, value, result.actual)) {
        result.errorCode = QStringLiteral("CAMERA_PARAMETER_READBACK_MISMATCH");
        return result;
    }
    result.status = QStringLiteral("success");
    result.errorCode.clear();
    result.sideEffectStatus = QStringLiteral("confirmed");
    return result;
}

quint64 CameraParameterBinding::generation() const {
    QMutexLocker locker(&m_mutex);
    return m_generation;
}

quint64 CameraParameterBinding::invalidate() {
    QMutexLocker locker(&m_mutex);
    return ++m_generation;
}

CameraParameterWriteResult CameraParameterBinding::write(ICamera* camera, quint64 expectedGeneration,
                                                         const QString& deviceId, const QString& serialNumber,
                                                         const QString& parameter, const QJsonValue& value) {
    QMutexLocker locker(&m_mutex);
    if (m_generation != expectedGeneration) {
        CameraParameterWriteResult result;
        result.parameter = parameter;
        result.requested = value;
        result.errorCode = QStringLiteral("CAMERA_TARGET_CHANGED");
        result.bindingVersion = m_generation;
        return result;
    }
    CameraParameterWriteResult result = writeCameraParameter(camera, deviceId, serialNumber, parameter, value);
    if (result.bindingChanged)
        ++m_generation;
    result.bindingVersion = m_generation;
    return result;
}

} // namespace DeepLux
