#include "HikvisionCamera.h"
#include "core/common/Logger.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QJsonObject>
#include <QDateTime>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

#ifdef __linux__
#include "CameraParams.h"
#include "MvObsoleteInterfaces.h"
#endif

namespace DeepLux {

// 静态回调函数用于图像采集
static void __stdcall FrameCallback(unsigned char* pData, MV_FRAME_OUT_INFO* pFrameInfo, void* pUser)
{
    if (!pUser || !pFrameInfo) return;
    HikvisionCamera* pCamera = static_cast<HikvisionCamera*>(pUser);
    pCamera->processFrame(pData, pFrameInfo);
}

HikvisionCamera::HikvisionCamera(void* handle, const QString& deviceId, QObject* parent)
    : ICamera(parent)
    , m_hCamera(handle)
    , m_deviceId(deviceId)
{
    m_capabilities.softwareTrigger = true;
    m_capabilities.hardwareTrigger = true;
    m_capabilities.continuousMode = true;
    m_capabilities.exposureControl = true;
    m_capabilities.gainControl = true;
    m_capabilities.frameRateControl = true;
    m_capabilities.roiControl = true;
    m_capabilities.supportedOnLinux = true;
    m_capabilities.supportedOnWindows = true;
    m_capabilities.depthData = false;
}

HikvisionCamera::~HikvisionCamera()
{
    disconnect();
}

bool HikvisionCamera::isPlatformSupported() const
{
#if defined(__linux__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

QString HikvisionCamera::platformNotSupportedMessage() const
{
    return QString();
}

bool HikvisionCamera::connect()
{
#ifdef __linux__
    if (m_connected) {
        return true;
    }

    if (!m_hCamera) {
        emit errorOccurred("相机句柄无效");
        return false;
    }

    // 打开设备
    int nRet = MV_CC_OpenDevice(m_hCamera);
    if (nRet != MV_OK) {
        QString err = QString("打开设备失败: 0x%1").arg(nRet, 0, 16);
        Logger::instance().error(err, "Camera");
        emit errorOccurred(err);
        return false;
    }
    Logger::instance().debug("MV_CC_OpenDevice OK", "Camera");

    // 注册图像回调
    nRet = MV_CC_RegisterImageCallBack(m_hCamera, FrameCallback, this);
    if (nRet != MV_OK) {
        QString err = QString("注册回调失败: 0x%1").arg(nRet, 0, 16);
        Logger::instance().error(err, "Camera");
        emit errorOccurred(err);
        MV_CC_CloseDevice(m_hCamera);
        return false;
    }
    Logger::instance().debug("MV_CC_RegisterImageCallBack OK", "Camera");

    // 开始采集图像流
    nRet = MV_CC_StartGrabbing(m_hCamera);
    if (nRet != MV_OK) {
        QString err = QString("开始采集失败: 0x%1").arg(nRet, 0, 16);
        Logger::instance().error(err, "Camera");
        emit errorOccurred(err);
        MV_CC_CloseDevice(m_hCamera);
        return false;
    }
    Logger::instance().debug("MV_CC_StartGrabbing OK", "Camera");

    m_connected = true;
    emit connectedChanged(true);

    // 查询实际分辨率和像素格式
#ifdef __linux__
    MVCC_INTVALUE_EX stWidth = {0};
    MVCC_INTVALUE_EX stHeight = {0};
    MV_CC_GetIntValueEx(m_hCamera, "Width", &stWidth);
    MV_CC_GetIntValueEx(m_hCamera, "Height", &stHeight);
    m_roi = QRect(0, 0, static_cast<int>(stWidth.nCurValue), static_cast<int>(stHeight.nCurValue));

    MVCC_ENUMVALUE stPixelFormat = {0};
    int fmtRet = MV_CC_GetEnumValue(m_hCamera, "PixelFormat", &stPixelFormat);
    QString pixelFormatStr = (fmtRet == MV_OK) ? QString("0x%1").arg(stPixelFormat.nCurValue, 0, 16) : QString("Unknown");

    Logger::instance().info(QString("Hikvision 相机已连接: %1, 分辨率: %2x%3, 像素格式: %4")
        .arg(m_deviceId).arg(stWidth.nCurValue).arg(stHeight.nCurValue).arg(pixelFormatStr), "Camera");
#else
    Logger::instance().info(QString("Hikvision 相机已连接: %1").arg(m_deviceId), "Camera");
#endif
    return true;
#else
    emit errorOccurred("Hikvision 仅支持 Linux/Windows");
    return false;
#endif
}

void HikvisionCamera::disconnect()
{
#ifdef __linux__
    stopAcquisition();

    if (m_hCamera) {
        MV_CC_StopGrabbing(m_hCamera);
        MV_CC_CloseDevice(m_hCamera);
        MV_CC_DestroyHandle(m_hCamera);
        m_hCamera = nullptr;
    }

    if (m_connected) {
        m_connected = false;
        emit connectedChanged(false);
        Logger::instance().info(QString("Hikvision 相机已断开: %1").arg(m_deviceId), "Camera");
    }
#endif
}

bool HikvisionCamera::startAcquisition()
{
#ifdef __linux__
    if (!m_connected || !m_hCamera) {
        return false;
    }

    if (m_acquiring) {
        return true;
    }

    m_acquiring = true;
    emit acquiringChanged(true);
    Logger::instance().info(QString("Hikvision 开始采集: %1").arg(m_deviceId), "Camera");
    return true;
#else
    return false;
#endif
}

void HikvisionCamera::stopAcquisition()
{
#ifdef __linux__
    if (!m_acquiring) {
        return;
    }

    m_acquiring = false;
    emit acquiringChanged(false);
    Logger::instance().info(QString("Hikvision 停止采集: %1").arg(m_deviceId), "Camera");
#endif
}

bool HikvisionCamera::triggerSoftware()
{
    return grabFrame();
}

bool HikvisionCamera::grabFrame()
{
#ifdef __linux__
    if (!m_connected) {
        return false;
    }

    // 触发软件触发
    int nRet = MV_CC_SetCommandValue(m_hCamera, "TriggerSoftware");
    if (nRet != MV_OK) {
        qWarning() << "TriggerSoftware failed:" << nRet;
        return false;
    }

    return true;
#else
    return false;
#endif
}

void HikvisionCamera::setTriggerMode(TriggerMode mode)
{
    m_triggerMode = mode;
#ifdef __linux__
    if (!m_hCamera) return;

    if (mode == TriggerMode::Software) {
        MV_CC_SetEnumValue(m_hCamera, "TriggerMode", 1);  // 触发模式
        MV_CC_SetEnumValue(m_hCamera, "TriggerSource", 7);  // Software
    } else if (mode == TriggerMode::Hardware) {
        MV_CC_SetEnumValue(m_hCamera, "TriggerMode", 1);  // 触发模式
        MV_CC_SetEnumValue(m_hCamera, "TriggerSource", 0);  // Line0
    } else {
        MV_CC_SetEnumValue(m_hCamera, "TriggerMode", 0);  // 连续采集
    }
#endif
}

void HikvisionCamera::setExposureTime(double microseconds)
{
    m_exposureTime = microseconds;
#ifdef __linux__
    if (m_hCamera) {
        MV_CC_SetFloatValue(m_hCamera, "ExposureTime", microseconds);
    }
#endif
}

void HikvisionCamera::setGain(double gain)
{
    m_gain = gain;
#ifdef __linux__
    if (m_hCamera) {
        MV_CC_SetFloatValue(m_hCamera, "Gain", gain);
    }
#endif
}

void HikvisionCamera::setFrameRate(double fps)
{
    m_frameRate = fps;
#ifdef __linux__
    if (m_hCamera) {
        MV_CC_SetFloatValue(m_hCamera, "AcquisitionFrameRate", fps);
    }
#endif
}

void HikvisionCamera::setRoi(int x, int y, int width, int height)
{
    m_roi = QRect(x, y, width, height);
#ifdef __linux__
    if (m_hCamera) {
        MV_CC_SetIntValue(m_hCamera, "OffsetX", x);
        MV_CC_SetIntValue(m_hCamera, "OffsetY", y);
        MV_CC_SetIntValue(m_hCamera, "Width", width);
        MV_CC_SetIntValue(m_hCamera, "Height", height);
    }
#endif
}

bool HikvisionCamera::processFrame(unsigned char* pData, MV_FRAME_OUT_INFO* pFrameInfo)
{
    QMutexLocker locker(&m_frameMutex);

    int nWidth = pFrameInfo->nWidth;
    int nHeight = pFrameInfo->nHeight;
    unsigned int nDataLen = pFrameInfo->nFrameLen;
    int nPixelFormat = pFrameInfo->enPixelType;

    if (nDataLen == 0 || !pData || nWidth <= 0 || nHeight <= 0) {
        return false;
    }

    // 先把 SDK 回调数据深拷贝到本地缓冲区
    m_frameBuffer = QByteArray(reinterpret_cast<const char*>(pData), static_cast<int>(nDataLen));
    const uchar* localData = reinterpret_cast<const uchar*>(m_frameBuffer.constData());

    // 根据实际数据长度计算每像素字节数，优先信任 nDataLen 而不是 enPixelType
    // 因为 SDK 回调有时会报告错误的像素格式
    int expectedMono8 = nWidth * nHeight;
    int expectedRGB8 = nWidth * nHeight * 3;
    int bpp = 0;
    if (static_cast<int>(nDataLen) >= expectedRGB8 - 1024 && static_cast<int>(nDataLen) <= expectedRGB8 + 1024) {
        bpp = 3;
    } else if (static_cast<int>(nDataLen) >= expectedMono8 - 1024 && static_cast<int>(nDataLen) <= expectedMono8 + 1024) {
        bpp = 1;
    } else {
        // fallback：按像素格式估算
        if (nPixelFormat == 0x01080001) bpp = 3; // RGB8
        else if (nPixelFormat == 0x0100000B || nPixelFormat == 0x01100003) bpp = 2; // Mono16/RGB16
        else bpp = 1;
    }

    QImage image;

    // Bayer 格式判断
    bool isBayerRG = (nPixelFormat == 0x01080009); // BayerRG8
    bool isBayerGR = (nPixelFormat == 0x01080008); // BayerGR8
    bool isBayerGB = (nPixelFormat == 0x0108000A); // BayerGB8
    bool isBayerBG = (nPixelFormat == 0x0108000B); // BayerBG8
    bool isBayer = isBayerRG || isBayerGR || isBayerGB || isBayerBG;

    if (bpp == 1 && isBayer) {
#ifdef DEEPLUX_HAS_OPENCV
        cv::Mat bayer(nHeight, nWidth, CV_8UC1, const_cast<uchar*>(localData));
        cv::Mat rgb;
        int code = cv::COLOR_BayerRG2RGB;
        if (isBayerGR) code = cv::COLOR_BayerGR2RGB;
        else if (isBayerGB) code = cv::COLOR_BayerGB2RGB;
        else if (isBayerBG) code = cv::COLOR_BayerBG2RGB;
        cv::cvtColor(bayer, rgb, code);
        image = QImage(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step), QImage::Format_RGB888).copy();
#else
        image = QImage(nWidth, nHeight, QImage::Format_Grayscale8);
        if (!image.isNull()) {
            int expectedBytes = image.sizeInBytes();
            int copyBytes = qMin(expectedBytes, static_cast<int>(nDataLen));
            memcpy(image.bits(), localData, copyBytes);
            if (copyBytes < expectedBytes) {
                memset(image.bits() + copyBytes, 0, expectedBytes - copyBytes);
            }
        }
#endif
    } else if (bpp == 3) {
        image = QImage(nWidth, nHeight, QImage::Format_RGB888);
        if (!image.isNull()) {
            int expectedBytes = image.sizeInBytes();
            int copyBytes = qMin(expectedBytes, static_cast<int>(nDataLen));
            memcpy(image.bits(), localData, copyBytes);
            if (copyBytes < expectedBytes) {
                memset(image.bits() + copyBytes, 0, expectedBytes - copyBytes);
            }
        }
    } else if (bpp == 2) {
        QImage::Format fmt = (nPixelFormat == 0x01100003) ? QImage::Format_RGB16 : QImage::Format_Grayscale16;
        image = QImage(nWidth, nHeight, fmt);
        if (!image.isNull()) {
            int expectedBytes = image.sizeInBytes();
            int copyBytes = qMin(expectedBytes, static_cast<int>(nDataLen));
            memcpy(image.bits(), localData, copyBytes);
            if (copyBytes < expectedBytes) {
                memset(image.bits() + copyBytes, 0, expectedBytes - copyBytes);
            }
        }
    } else {
        image = QImage(nWidth, nHeight, QImage::Format_Grayscale8);
        if (!image.isNull()) {
            int expectedBytes = image.sizeInBytes();
            int copyBytes = qMin(expectedBytes, static_cast<int>(nDataLen));
            memcpy(image.bits(), localData, copyBytes);
            if (copyBytes < expectedBytes) {
                memset(image.bits() + copyBytes, 0, expectedBytes - copyBytes);
            }
        }
    }

    if (image.isNull()) {
        return false;
    }

    m_lastImage = image.copy();
    m_metadata["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    m_metadata["device"] = m_deviceId;
    m_metadata["width"] = nWidth;
    m_metadata["height"] = nHeight;
    m_metadata["pixelFormat"] = QString("0x%1").arg(nPixelFormat, 0, 16);
    m_metadata["frameNum"] = pFrameInfo->nFrameNum;

    // 调试：保存第一帧到文件，帮助诊断图像格式问题
    static bool s_debugSaved = false;
    if (!s_debugSaved) {
        s_debugSaved = true;
        QString debugPath = "/tmp/deeplux_debug_frame.png";
        if (m_lastImage.save(debugPath)) {
            Logger::instance().debug(QString("Saved debug frame to %1, format=0x%2, size=%3x%4")
                .arg(debugPath).arg(nPixelFormat, 0, 16).arg(nWidth).arg(nHeight), "Camera");
        }
    }

    emit imageReceived(m_lastImage, m_metadata);
    return true;
}

QWidget* HikvisionCamera::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(QString("设备: %1").arg(m_deviceId)));
    layout->addWidget(new QLabel(QString("名称: %1").arg(m_name)));
    layout->addWidget(new QLabel(QString("序列号: %1").arg(m_serialNumber)));
    layout->addWidget(new QLabel(QString("分辨率: %1x%2").arg(m_roi.width()).arg(m_roi.height())));

    layout->addStretch();
    return widget;
}

QJsonObject HikvisionCamera::toJson() const
{
    QJsonObject json;
    json["deviceId"] = m_deviceId;
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

bool HikvisionCamera::fromJson(const QJsonObject& json)
{
    m_deviceId = json["deviceId"].toString(m_deviceId);
    m_name = json["name"].toString(m_name);
    m_exposureTime = json["exposureTime"].toDouble(m_exposureTime);
    m_gain = json["gain"].toDouble(m_gain);
    m_frameRate = json["frameRate"].toDouble(m_frameRate);

    int x = json["roi_x"].toInt(0);
    int y = json["roi_y"].toInt(0);
    int w = json["roi_width"].toInt(0);
    int h = json["roi_height"].toInt(0);
    m_roi = QRect(x, y, w, h);

    return true;
}

void HikvisionCamera::setDeviceInfo(const QString& name, const QString& serial)
{
    m_name = name;
    m_serialNumber = serial;
}

} // namespace DeepLux
