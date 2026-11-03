#pragma once

#include "../platform/Platform.h"

#include <QString>
#include <QtPlugin>

namespace DeepLux {

class CameraDiscoveryJob;

class DEEPLUX_API ICameraDiscoveryAsync {
public:
    virtual ~ICameraDiscoveryAsync() = default;
    virtual CameraDiscoveryJob* startDiscovery(quint64 requestId, QString& errorCode) = 0;
    virtual void requestCancel(quint64 requestId) = 0;
};

} // namespace DeepLux

Q_DECLARE_INTERFACE(DeepLux::ICameraDiscoveryAsync, "com.deeplux.ICameraDiscoveryAsync/1.0")
