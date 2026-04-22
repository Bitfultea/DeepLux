#include "V4L2Camera.h"
#include "core/common/Logger.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QJsonObject>
#include <QDateTime>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

#ifdef __linux__
#include <linux/videodev2.h>
#endif

namespace DeepLux {

V4L2Camera::V4L2Camera(const QString& devicePath, QObject* parent)
    : ICamera(parent)
    , m_devicePath(devicePath)
    , m_name(QString("V4L2 %1").arg(devicePath))
{
    m_capabilities.softwareTrigger = true;
    m_capabilities.continuousMode = true;
    m_capabilities.exposureControl = true;
    m_capabilities.gainControl = true;
    m_capabilities.frameRateControl = true;
    m_capabilities.roiControl = true;
    m_capabilities.supportedOnLinux = true;
    m_capabilities.supportedOnWindows = false;
}

V4L2Camera::~V4L2Camera()
{
    disconnect();
}

bool V4L2Camera::connect()
{
#ifdef __linux__
    if (m_connected) {
        return true;
    }

    if (!openDevice()) {
        emit errorOccurred(QString("无法打开设备: %1").arg(m_devicePath));
        return false;
    }

    m_connected = true;
    emit connectedChanged(true);
    Logger::instance().info(QString("V4L2 相机已连接: %1").arg(m_devicePath), "Camera");
    return true;
#else
    emit errorOccurred("V4L2 仅在 Linux 上可用");
    return false;
#endif
}

void V4L2Camera::disconnect()
{
#ifdef __linux__
    if (!m_connected) {
        return;
    }

    stopAcquisition();
    closeDevice();

    m_connected = false;
    emit connectedChanged(false);
    Logger::instance().info(QString("V4L2 相机已断开: %1").arg(m_devicePath), "Camera");
#endif
}

bool V4L2Camera::openDevice()
{
#ifdef __linux__
    if (m_fd >= 0) {
        return true;
    }

    m_fd = ::open(m_devicePath.toUtf8().constData(), O_RDWR);
    if (m_fd < 0) {
        qWarning() << "Failed to open" << m_devicePath << ":" << strerror(errno);
        return false;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));

    if (ioctl(m_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        qWarning() << "VIDIOC_QUERYCAP failed for" << m_devicePath;
        close(m_fd);
        m_fd = -1;
        return false;
    }

    m_name = QString::fromLatin1(reinterpret_cast<const char*>(cap.card));
    m_serialNumber = QString::fromLatin1(reinterpret_cast<const char*>(cap.bus_info));

    if (!configureFormat(640, 480)) {
        close(m_fd);
        m_fd = -1;
        return false;
    }

    if (!initBuffers()) {
        close(m_fd);
        m_fd = -1;
        return false;
    }

    return true;
#else
    return false;
#endif
}

void V4L2Camera::closeDevice()
{
#ifdef __linux__
    uninitBuffers();
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
#endif
}

bool V4L2Camera::configureFormat(int width, int height)
{
#ifdef __linux__
    if (m_fd < 0) {
        return false;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (ioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0) {
        qWarning() << "VIDIOC_S_FMT failed:" << strerror(errno);
        // 回退：读取当前格式
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(m_fd, VIDIOC_G_FMT, &fmt) < 0) {
            qWarning() << "VIDIOC_G_FMT also failed:" << strerror(errno);
            return false;
        }
    }

    m_roi = QRect(0, 0, fmt.fmt.pix.width, fmt.fmt.pix.height);
    m_pixelFormat = fmt.fmt.pix.pixelformat;
    return true;
#else
    return false;
#endif
}

bool V4L2Camera::startAcquisition()
{
#ifdef __linux__
    if (!m_connected || m_fd < 0) {
        return false;
    }

    if (m_acquiring) {
        return true;
    }

    m_acquiring = true;
    emit acquiringChanged(true);
    Logger::instance().info(QString("V4L2 开始采集: %1").arg(m_devicePath), "Camera");
    return true;
#else
    return false;
#endif
}

void V4L2Camera::stopAcquisition()
{
#ifdef __linux__
    if (!m_acquiring) {
        return;
    }

    m_acquiring = false;
    emit acquiringChanged(false);
    Logger::instance().info(QString("V4L2 停止采集: %1").arg(m_devicePath), "Camera");
#endif
}

bool V4L2Camera::triggerSoftware()
{
    return grabFrame();
}

bool V4L2Camera::grabFrame()
{
#ifdef __linux__
    if (m_fd < 0 || m_bufferCount == 0) {
        return false;
    }

    // 将所有 buffer 入队
    for (int i = 0; i < m_bufferCount; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ioctl(m_fd, VIDIOC_QBUF, &buf); // 忽略已在队列中的错误
    }

    // 开始采集
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
        qWarning() << "VIDIOC_STREAMON failed:" << strerror(errno);
        return false;
    }

    // 出队等待帧
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0) {
        qWarning() << "VIDIOC_DQBUF failed:" << strerror(errno);
        ioctl(m_fd, VIDIOC_STREAMOFF, &type);
        return false;
    }

    if (buf.index >= static_cast<unsigned int>(m_bufferCount)) {
        qWarning() << "Invalid buffer index:" << buf.index;
        ioctl(m_fd, VIDIOC_STREAMOFF, &type);
        return false;
    }

    // 转换帧
    QImage frame = convertV4L2Frame(m_buffers[buf.index].start, buf.bytesused,
                                    m_roi.width(), m_roi.height(), m_pixelFormat);

    // 重新入队
    ioctl(m_fd, VIDIOC_QBUF, &buf);

    if (frame.isNull()) {
        return false;
    }

    m_lastImage = frame;
    m_metadata["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    m_metadata["device"] = m_devicePath;
    m_metadata["buffer_size"] = static_cast<qint64>(buf.bytesused);
    m_metadata["pixelFormat"] = QString::number(m_pixelFormat, 16);

    emit imageReceived(m_lastImage, m_metadata);
    return true;
#else
    return false;
#endif
}

#ifdef __linux__
bool V4L2Camera::initBuffers()
{
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
        qWarning() << "VIDIOC_REQBUFS failed:" << strerror(errno);
        return false;
    }

    if (req.count < 2) {
        qWarning() << "Insufficient buffer memory on" << m_devicePath;
        return false;
    }

    m_bufferCount = req.count;

    for (int i = 0; i < m_bufferCount; ++i) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(m_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            qWarning() << "VIDIOC_QUERYBUF failed:" << strerror(errno);
            return false;
        }

        m_buffers[i].length = buf.length;
        m_buffers[i].start = mmap(nullptr, buf.length,
                                  PROT_READ | PROT_WRITE, MAP_SHARED,
                                  m_fd, buf.m.offset);

        if (m_buffers[i].start == MAP_FAILED) {
            qWarning() << "mmap failed:" << strerror(errno);
            return false;
        }
    }

    return true;
}

void V4L2Camera::uninitBuffers()
{
    if (m_fd < 0) return;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(m_fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < m_bufferCount; ++i) {
        if (m_buffers[i].start && m_buffers[i].start != MAP_FAILED) {
            munmap(m_buffers[i].start, m_buffers[i].length);
            m_buffers[i].start = nullptr;
            m_buffers[i].length = 0;
        }
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 0;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(m_fd, VIDIOC_REQBUFS, &req);

    m_bufferCount = 0;
}

QImage V4L2Camera::convertV4L2Frame(const void* data, size_t bytesused, int width, int height, uint32_t pixelformat)
{
    if (pixelformat == V4L2_PIX_FMT_MJPEG) {
        QImage image;
        if (image.loadFromData(static_cast<const uchar*>(data), static_cast<int>(bytesused))) {
            if (image.format() != QImage::Format_RGB888) {
                image = image.convertToFormat(QImage::Format_RGB888);
            }
            return image;
        }
        qWarning() << "Failed to decode MJPEG frame";
        return QImage();
    } else if (pixelformat == V4L2_PIX_FMT_YUYV) {
        return yuyvToRgb(static_cast<const uchar*>(data), width, height);
    } else if (pixelformat == V4L2_PIX_FMT_RGB24) {
        return QImage(static_cast<const uchar*>(data), width, height, width * 3, QImage::Format_RGB888).copy();
    } else if (pixelformat == V4L2_PIX_FMT_GREY) {
        return QImage(static_cast<const uchar*>(data), width, height, width, QImage::Format_Grayscale8).copy();
    }

    qWarning() << "Unsupported pixel format:" << QString::number(pixelformat, 16);
    return QImage();
}

QImage V4L2Camera::yuyvToRgb(const uchar* yuyv, int width, int height)
{
    QImage image(width, height, QImage::Format_RGB888);
    uchar* rgb = image.bits();

    for (int i = 0; i < width * height * 2; i += 4) {
        int y0 = yuyv[i];
        int u = yuyv[i + 1] - 128;
        int y1 = yuyv[i + 2];
        int v = yuyv[i + 3] - 128;

        int r = 298 * (y0 - 16) + 409 * v + 128;
        int g = 298 * (y0 - 16) - 100 * u - 208 * v + 128;
        int b = 298 * (y0 - 16) + 516 * u + 128;

        *rgb++ = qBound(0, r >> 8, 255);
        *rgb++ = qBound(0, g >> 8, 255);
        *rgb++ = qBound(0, b >> 8, 255);

        r = 298 * (y1 - 16) + 409 * v + 128;
        g = 298 * (y1 - 16) - 100 * u - 208 * v + 128;
        b = 298 * (y1 - 16) + 516 * u + 128;

        *rgb++ = qBound(0, r >> 8, 255);
        *rgb++ = qBound(0, g >> 8, 255);
        *rgb++ = qBound(0, b >> 8, 255);
    }

    return image;
}
#endif

void V4L2Camera::setTriggerMode(TriggerMode mode)
{
    m_triggerMode = mode;
}

void V4L2Camera::setExposureTime(double microseconds)
{
    m_exposureTime = microseconds;
#ifdef __linux__
    if (m_fd >= 0) {
        struct v4l2_control ctrl;
        ctrl.id = V4L2_CID_EXPOSURE_ABSOLUTE;
        ctrl.value = static_cast<int>(microseconds);
        ioctl(m_fd, VIDIOC_S_CTRL, &ctrl);
    }
#endif
}

void V4L2Camera::setGain(double gain)
{
    m_gain = gain;
#ifdef __linux__
    if (m_fd >= 0) {
        struct v4l2_control ctrl;
        ctrl.id = V4L2_CID_GAIN;
        ctrl.value = static_cast<int>(gain * 255);
        ioctl(m_fd, VIDIOC_S_CTRL, &ctrl);
    }
#endif
}

void V4L2Camera::setFrameRate(double fps)
{
    m_frameRate = fps;
#ifdef __linux__
    if (m_fd >= 0) {
        struct v4l2_streamparm parm;
        memset(&parm, 0, sizeof(parm));
        parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        parm.parm.capture.timeperframe.numerator = 1;
        parm.parm.capture.timeperframe.denominator = static_cast<int>(fps);
        ioctl(m_fd, VIDIOC_S_PARM, &parm);
    }
#endif
}

void V4L2Camera::setRoi(int x, int y, int width, int height)
{
    m_roi = QRect(x, y, width, height);
    configureFormat(width, height);
}

QWidget* V4L2Camera::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(QString("设备: %1").arg(m_devicePath)));
    layout->addWidget(new QLabel(QString("名称: %1").arg(m_name)));
    layout->addWidget(new QLabel(QString("序列号: %1").arg(m_serialNumber)));
    layout->addWidget(new QLabel(QString("分辨率: %1x%2").arg(m_roi.width()).arg(m_roi.height())));

    layout->addStretch();
    return widget;
}

QJsonObject V4L2Camera::toJson() const
{
    QJsonObject json;
    json["devicePath"] = m_devicePath;
    json["name"] = m_name;
    json["exposureTime"] = m_exposureTime;
    json["gain"] = m_gain;
    json["frameRate"] = m_frameRate;
    json["roi_x"] = m_roi.x();
    json["roi_y"] = m_roi.y();
    json["roi_width"] = m_roi.width();
    json["roi_height"] = m_roi.height();
    return json;
}

bool V4L2Camera::fromJson(const QJsonObject& json)
{
    m_devicePath = json["devicePath"].toString(m_devicePath);
    m_name = json["name"].toString(m_name);
    m_exposureTime = json["exposureTime"].toDouble(m_exposureTime);
    m_gain = json["gain"].toDouble(m_gain);
    m_frameRate = json["frameRate"].toDouble(m_frameRate);

    int x = json["roi_x"].toInt(0);
    int y = json["roi_y"].toInt(0);
    int w = json["roi_width"].toInt(640);
    int h = json["roi_height"].toInt(480);
    m_roi = QRect(x, y, w, h);

    return true;
}

} // namespace DeepLux
