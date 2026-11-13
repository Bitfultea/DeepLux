#pragma once

#include <QList>
#include <QString>
#include <memory>

namespace DeepLux {

struct V4L2DeviceInfo {
    bool isV4L2 = false;
    QString card;
    QString busInfo;
    QString driver;
    int width = 0;
    int height = 0;
    QString pixelFormat;
};

class IV4L2DiscoverySystem {
public:
    virtual ~IV4L2DiscoverySystem() = default;
    virtual QList<QString> listVideoDevicePaths(QString& errorCode) = 0;
    virtual bool statDevice(const QString& path, bool& isCharacterDevice, QString& errorCode) = 0;
    virtual int openDevice(const QString& path, QString& errorCode) = 0;
    virtual bool queryCapabilities(int fd, V4L2DeviceInfo& info, QString& errorCode) = 0;
    virtual bool queryFormat(int fd, V4L2DeviceInfo& info, QString& errorCode) = 0;
    virtual void closeDevice(int fd) = 0;
};

std::shared_ptr<IV4L2DiscoverySystem> makeProductionV4L2DiscoverySystem();

} // namespace DeepLux
