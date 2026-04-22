#include "HikvisionCameraPlugin.h"
#include "HikvisionCamera.h"
#include "core/common/Logger.h"

#ifdef __linux__
#include "MvCameraControl.h"
#include "CameraParams.h"
#endif

using namespace std;

namespace DeepLux {

HikvisionCameraPlugin::HikvisionCameraPlugin(QObject* parent)
    : ICameraPlugin(parent)
    , m_sdkInitialized(false)
{
    initSDK();
}

HikvisionCameraPlugin::~HikvisionCameraPlugin()
{
    cleanupSDK();
}

bool HikvisionCameraPlugin::initSDK()
{
#ifdef __linux__
    // 初始化 SDK
    int nRet = MV_CC_Initialize();
    if (nRet != MV_OK) {
        Logger::instance().warning(QString("Hikvision SDK 初始化失败: 0x%1").arg(nRet, 0, 16), "Camera");
        return false;
    }
    m_sdkInitialized = true;
    return true;
#else
    return false;
#endif
}

void HikvisionCameraPlugin::cleanupSDK()
{
#ifdef __linux__
    if (m_sdkInitialized) {
        MV_CC_Finalize();
        m_sdkInitialized = false;
    }
#endif
}

bool HikvisionCameraPlugin::isAvailable() const
{
#ifdef __linux__
    if (!m_sdkInitialized) {
        return false;
    }

    // 枚举设备
    MV_CC_DEVICE_INFO_LIST deviceList;
    memset(&deviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    int nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &deviceList);
    if (nRet != MV_OK) {
        return false;
    }

    return deviceList.nDeviceNum > 0;
#else
    return false;
#endif
}

QString HikvisionCameraPlugin::availabilityMessage() const
{
#ifdef __linux__
    if (!m_sdkInitialized) {
        return QStringLiteral("Hikvision SDK 未初始化");
    }

    MV_CC_DEVICE_INFO_LIST deviceList;
    memset(&deviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    int nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &deviceList);
    if (nRet != MV_OK) {
        return QStringLiteral("枚举设备失败");
    }

    if (deviceList.nDeviceNum == 0) {
        return QStringLiteral("未发现 Hikvision 相机");
    }

    return QStringLiteral("发现 %1 个 Hikvision 相机").arg(deviceList.nDeviceNum);
#else
    return QStringLiteral("Hikvision 仅在 Linux 上可用");
#endif
}

QList<CameraInfo> HikvisionCameraPlugin::discoverCameras()
{
    QList<CameraInfo> cameras;

#ifdef __linux__
    if (!m_sdkInitialized) {
        Logger::instance().warning("Hikvision SDK 未初始化", "Camera");
        return cameras;
    }

    // 枚举 GigE 和 USB 设备
    MV_CC_DEVICE_INFO_LIST deviceList;
    memset(&deviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    int nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &deviceList);
    if (nRet != MV_OK) {
        Logger::instance().warning(QString("Hikvision 设备枚举失败: 0x%1").arg(nRet, 0, 16), "Camera");
        return cameras;
    }

    Logger::instance().info(QString("Hikvision: 发现 %1 个设备").arg(deviceList.nDeviceNum), "Camera");

    for (unsigned int i = 0; i < deviceList.nDeviceNum; ++i) {
        CameraInfo info;
        info.pluginId = "hikvision";

        MV_CC_DEVICE_INFO* pDeviceInfo = deviceList.pDeviceInfo[i];
        if (!pDeviceInfo) {
            continue;
        }

        // GigE 设备
        if (pDeviceInfo->nTLayerType == MV_GIGE_DEVICE) {
            info.deviceId = QString("GigE:%1").arg(i);
            info.name = QString::fromUtf8(reinterpret_cast<char*>(pDeviceInfo->SpecialInfo.stGigEInfo.chModelName));
            info.serialNumber = QString::fromUtf8(reinterpret_cast<char*>(pDeviceInfo->SpecialInfo.stGigEInfo.chSerialNumber));
            info.ipAddress = QString("%1.%2.%3.%4")
                .arg(pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp & 0xFF)
                .arg((pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp >> 8) & 0xFF)
                .arg((pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp >> 16) & 0xFF)
                .arg((pDeviceInfo->SpecialInfo.stGigEInfo.nCurrentIp >> 24) & 0xFF);
        }
        // USB 设备
        else if (pDeviceInfo->nTLayerType == MV_USB_DEVICE) {
            info.deviceId = QString("USB:%1").arg(i);
            info.name = QString::fromUtf8(reinterpret_cast<char*>(pDeviceInfo->SpecialInfo.stUsb3VInfo.chModelName));
            info.serialNumber = QString::fromUtf8(reinterpret_cast<char*>(pDeviceInfo->SpecialInfo.stUsb3VInfo.chSerialNumber));
        }

        cameras.append(info);
        Logger::instance().info(
            QString("Hikvision: 发现相机 %1 - %2 (SN: %3)")
                .arg(info.deviceId).arg(info.name).arg(info.serialNumber),
            "Camera");
    }

#else
    Logger::instance().warning("Hikvision 仅在 Linux 上支持", "Camera");
#endif

    return cameras;
}

QObject* HikvisionCameraPlugin::createCamera(const CameraInfo& info)
{
    if (info.pluginId != "hikvision") {
        return nullptr;
    }

#ifdef __linux__
    if (!m_sdkInitialized) {
        Logger::instance().warning("Hikvision SDK 未初始化", "Camera");
        return nullptr;
    }

    // 重新枚举设备找到对应的句柄
    MV_CC_DEVICE_INFO_LIST deviceList;
    memset(&deviceList, 0, sizeof(MV_CC_DEVICE_INFO_LIST));
    int nRet = MV_CC_EnumDevices(MV_GIGE_DEVICE | MV_USB_DEVICE, &deviceList);
    if (nRet != MV_OK || deviceList.nDeviceNum == 0) {
        return nullptr;
    }

    // 解析设备ID
    QString cameraId = info.deviceId;
    bool isGigE = cameraId.startsWith("GigE:");
    int deviceIndex = -1;

    if (isGigE) {
        deviceIndex = cameraId.mid(5).toInt();
    } else if (cameraId.startsWith("USB:")) {
        deviceIndex = cameraId.mid(4).toInt();
    }

    if (deviceIndex < 0 || deviceIndex >= static_cast<int>(deviceList.nDeviceNum)) {
        return nullptr;
    }

    // 创建设备句柄
    void* hCamera = nullptr;
    nRet = MV_CC_CreateHandle(&hCamera, deviceList.pDeviceInfo[deviceIndex]);
    if (nRet != MV_OK) {
        Logger::instance().warning(
            QString("创建相机句柄失败: 0x%1").arg(nRet, 0, 16),
            "Camera");
        return nullptr;
    }

    HikvisionCamera* camera = new HikvisionCamera(hCamera, info.deviceId);
    camera->setDeviceInfo(info.name, info.serialNumber);

    return qobject_cast<QObject*>(camera);
#else
    return nullptr;
#endif
}

} // namespace DeepLux
