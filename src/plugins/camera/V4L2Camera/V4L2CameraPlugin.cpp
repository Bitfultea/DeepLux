#include "V4L2CameraPlugin.h"
#include "V4L2Camera.h"
#include "core/common/Logger.h"

#include <QDir>
#include <QFile>
#include <QDebug>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <linux/videodev2.h>
#endif

namespace DeepLux {

V4L2CameraPlugin::V4L2CameraPlugin(QObject* parent)
    : ICameraPlugin(parent)
{
}

V4L2CameraPlugin::~V4L2CameraPlugin()
{
}

bool V4L2CameraPlugin::isAvailable() const
{
#ifdef __linux__
    QDir devDir("/dev");
    QStringList devices = devDir.entryList({"video*"});
    return !devices.isEmpty();
#else
    return false;
#endif
}

QString V4L2CameraPlugin::availabilityMessage() const
{
#ifdef __linux__
    QDir devDir("/dev");
    QStringList devices = devDir.entryList({"video*"});
    if (devices.isEmpty()) {
        return QStringLiteral("未找到 V4L2 相机设备 (/dev/video*)");
    }
    return QStringLiteral("发现 %1 个 V4L2 设备").arg(devices.size());
#else
    return QStringLiteral("V4L2 仅在 Linux 上可用");
#endif
}

QList<CameraInfo> V4L2CameraPlugin::discoverCameras()
{
    QList<CameraInfo> cameras;

#ifdef __linux__
    QDir devDir("/dev");
    QStringList devices = devDir.entryList({"video*"}, QDir::System);

    for (const QString& device : devices) {
        QString devicePath = "/dev/" + device;

        if (!isV4L2Device(devicePath)) {
            continue;
        }

        CameraInfo info;
        info.pluginId = "v4l2";
        info.deviceId = devicePath;
        info.name = getDeviceName(devicePath);

        // 尝试获取分辨率和格式
        int fd = open(devicePath.toUtf8().constData(), O_RDONLY);
        if (fd >= 0) {
            struct v4l2_format fmt;
            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

            if (ioctl(fd, VIDIOC_G_FMT, &fmt) >= 0) {
                info.width = fmt.fmt.pix.width;
                info.height = fmt.fmt.pix.height;

                char fourcc[5] = {0};
                memcpy(fourcc, &fmt.fmt.pix.pixelformat, 4);
                info.pixelFormat = QString(fourcc);
            }

            // 尝试获取设备信息
            struct v4l2_capability cap;
            memset(&cap, 0, sizeof(cap));
            if (ioctl(fd, VIDIOC_QUERYCAP, &cap) >= 0) {
                info.serialNumber = QString::fromLatin1(reinterpret_cast<const char*>(cap.bus_info));
                info.rawInfo["driver"] = QString::fromLatin1(reinterpret_cast<const char*>(cap.driver));
            }

            close(fd);
        }

        cameras.append(info);
        Logger::instance().info(
            QString("V4L2: 发现相机 %1 - %2 (%3x%4)")
                .arg(devicePath).arg(info.name).arg(info.width).arg(info.height),
            "Camera");
    }
#else
    Logger::instance().warning("V4L2 仅在 Linux 上支持", "Camera");
#endif

    return cameras;
}

QObject* V4L2CameraPlugin::createCamera(const CameraInfo& info)
{
    if (info.pluginId != "v4l2") {
        return nullptr;
    }

    V4L2Camera* camera = new V4L2Camera(info.deviceId);
    return qobject_cast<QObject*>(camera);
}

QString V4L2CameraPlugin::getDeviceName(const QString& devicePath) const
{
#ifdef __linux__
    int fd = open(devicePath.toUtf8().constData(), O_RDONLY);
    if (fd < 0) {
        return QString("V4L2 Device %1").arg(devicePath);
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) >= 0) {
        close(fd);
        return QString::fromLatin1(reinterpret_cast<const char*>(cap.card));
    }

    close(fd);
#endif
    return QString("V4L2 Device %1").arg(devicePath);
}

bool V4L2CameraPlugin::isV4L2Device(const QString& devicePath) const
{
#ifdef __linux__
    struct stat st;
    if (stat(devicePath.toUtf8().constData(), &st) < 0) {
        return false;
    }

    if (!S_ISCHR(st.st_mode)) {
        return false;
    }

    int fd = open(devicePath.toUtf8().constData(), O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    bool isV4L2 = (ioctl(fd, VIDIOC_QUERYCAP, &cap) >= 0);
    close(fd);

    return isV4L2;
#else
    return false;
#endif
}

} // namespace DeepLux
