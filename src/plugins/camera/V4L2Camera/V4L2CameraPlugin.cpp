#include "V4L2CameraPlugin.h"

#include "V4L2Camera.h"
#include "V4L2DiscoveryJob.h"
#include "core/common/Logger.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QThread>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/videodev2.h>
#endif

namespace DeepLux {

namespace {

class ProductionV4L2DiscoverySystem final : public IV4L2DiscoverySystem {
public:
    QList<QString> listVideoDevicePaths(QString& errorCode) override {
        errorCode.clear();
        QDir devDir(QStringLiteral("/dev"));
        QList<QString> paths;
        for (const QString& entry : devDir.entryList({QStringLiteral("video*")}, QDir::System))
            paths.append(devDir.filePath(entry));
        return paths;
    }

    bool statDevice(const QString& path, bool& isCharacterDevice, QString& errorCode) override {
        errorCode.clear();
        struct stat info;
        isCharacterDevice = ::stat(path.toUtf8().constData(), &info) == 0 && S_ISCHR(info.st_mode);
        return true;
    }

    int openDevice(const QString& path, QString& errorCode) override {
        errorCode.clear();
        return ::open(path.toUtf8().constData(), O_RDONLY | O_NONBLOCK);
    }

    bool queryCapabilities(int fd, V4L2DeviceInfo& info, QString& errorCode) override {
        errorCode.clear();
        struct v4l2_capability capability;
        memset(&capability, 0, sizeof(capability));
        if (::ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0)
            return false;
        info.isV4L2 = true;
        const auto fixedString = [](const __u8* value, size_t size) {
            const auto* chars = reinterpret_cast<const char*>(value);
            return QString::fromLatin1(chars, static_cast<int>(::strnlen(chars, size)));
        };
        info.card = fixedString(capability.card, sizeof(capability.card));
        info.busInfo = fixedString(capability.bus_info, sizeof(capability.bus_info));
        info.driver = fixedString(capability.driver, sizeof(capability.driver));

        return true;
    }

    bool queryFormat(int fd, V4L2DeviceInfo& info, QString& errorCode) override {
        errorCode.clear();
        struct v4l2_format format;
        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (::ioctl(fd, VIDIOC_G_FMT, &format) >= 0) {
            info.width = static_cast<int>(format.fmt.pix.width);
            info.height = static_cast<int>(format.fmt.pix.height);
            char fourcc[5] = {0};
            memcpy(fourcc, &format.fmt.pix.pixelformat, 4);
            info.pixelFormat = QString::fromLatin1(fourcc);
        }
        return true;
    }

    void closeDevice(int fd) override {
        ::close(fd);
    }
};

} // namespace

std::shared_ptr<IV4L2DiscoverySystem> makeProductionV4L2DiscoverySystem() {
    return std::make_shared<ProductionV4L2DiscoverySystem>();
}

V4L2CameraPlugin::V4L2CameraPlugin(QObject* parent) : V4L2CameraPlugin(makeProductionV4L2DiscoverySystem(), parent) {}

V4L2CameraPlugin::V4L2CameraPlugin(std::shared_ptr<IV4L2DiscoverySystem> discoverySystem, QObject* parent)
    : ICameraPlugin(parent), m_discoverySystem(std::move(discoverySystem)) {}

V4L2CameraPlugin::~V4L2CameraPlugin() {}

bool V4L2CameraPlugin::isAvailable() const {
#ifdef __linux__
    QDir devDir("/dev");
    QStringList devices = devDir.entryList({"video*"});
    return !devices.isEmpty();
#else
    return false;
#endif
}

QString V4L2CameraPlugin::availabilityMessage() const {
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

QList<CameraInfo> V4L2CameraPlugin::discoverCameras() {
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
            QString("V4L2: 发现相机 %1 - %2 (%3x%4)").arg(devicePath).arg(info.name).arg(info.width).arg(info.height),
            "Camera");
    }
#else
    Logger::instance().warning("V4L2 仅在 Linux 上支持", "Camera");
#endif

    return cameras;
}

QObject* V4L2CameraPlugin::createCamera(const CameraInfo& info) {
    if (info.pluginId != "v4l2") {
        return nullptr;
    }

    V4L2Camera* camera = new V4L2Camera(info.deviceId);
    return qobject_cast<QObject*>(camera);
}

CameraDiscoveryJob* V4L2CameraPlugin::startDiscovery(quint64 requestId, QString& errorCode) {
    errorCode.clear();
    if (QThread::currentThread() != thread()) {
        errorCode = QStringLiteral("CAMERA_PLUGIN_THREAD_AFFINITY");
        return nullptr;
    }
#ifndef __linux__
    Q_UNUSED(requestId)
    errorCode = QStringLiteral("CAMERA_PLATFORM_UNSUPPORTED");
    return nullptr;
#else
    if (!m_discoverySystem) {
        errorCode = QStringLiteral("CAMERA_DISCOVERY_BACKEND_UNAVAILABLE");
        return nullptr;
    }
    if (m_discoveryRequests.contains(requestId)) {
        errorCode = QStringLiteral("CAMERA_DISCOVERY_BUSY");
        return nullptr;
    }
    auto cancelRequested = std::make_shared<std::atomic_bool>(false);
    m_discoveryRequests.insert(requestId, cancelRequested);
    auto* job = new V4L2DiscoveryJob(requestId, m_discoverySystem, cancelRequested, this);
    connect(job, &QObject::destroyed, this, [this, requestId]() { releaseDiscoveryRequest(requestId); });
    return job;
#endif
}

void V4L2CameraPlugin::requestCancel(quint64 requestId) {
    const auto cancelRequested = m_discoveryRequests.value(requestId);
    if (cancelRequested)
        cancelRequested->store(true, std::memory_order_relaxed);
}

void V4L2CameraPlugin::releaseDiscoveryRequest(quint64 requestId) {
    m_discoveryRequests.remove(requestId);
}

QString V4L2CameraPlugin::getDeviceName(const QString& devicePath) const {
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

bool V4L2CameraPlugin::isV4L2Device(const QString& devicePath) const {
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
