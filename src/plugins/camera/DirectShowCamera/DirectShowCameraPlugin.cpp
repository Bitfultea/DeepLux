#include "DirectShowCameraPlugin.h"
#include "DirectShowCamera.h"
#include "core/common/Logger.h"

#ifdef _WIN32
#include <dshow.h>
#include <windows.h>
#pragma comment(lib, "strmiids.lib")
#endif

namespace DeepLux {

DirectShowCameraPlugin::DirectShowCameraPlugin(QObject* parent)
    : ICameraPlugin(parent)
{
}

DirectShowCameraPlugin::~DirectShowCameraPlugin()
{
}

bool DirectShowCameraPlugin::isAvailable() const
{
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

QString DirectShowCameraPlugin::availabilityMessage() const
{
#ifdef _WIN32
    return QStringLiteral("DirectShow 相机可用");
#else
    return QStringLiteral("DirectShow 仅在 Windows 上可用");
#endif
}

QList<CameraInfo> DirectShowCameraPlugin::discoverCameras()
{
    QList<CameraInfo> cameras;

#ifdef _WIN32
    // 初始化 COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Logger::instance().warning("Failed to initialize COM", "Camera");
        return cameras;
    }

    // 创建设备枚举器
    ICreateDevEnum* pDevEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        Logger::instance().warning("Failed to create device enumerator", "Camera");
        CoUninitialize();
        return cameras;
    }

    // 获取视频输入设备枚举
    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    pDevEnum->Release();

    if (hr != S_OK) {
        Logger::instance().info("No DirectShow video devices found", "Camera");
        CoUninitialize();
        return cameras;
    }

    // 遍历所有设备
    IMoniker* pMoniker = nullptr;
    ULONG cFetched = 0;
    int deviceIndex = 0;

    while (pEnum->Next(1, &pMoniker, &cFetched) == S_OK) {
        CameraInfo info;
        info.pluginId = "directshow";
        info.deviceId = QString("DS%1").arg(deviceIndex);
        info.name = getDeviceName(deviceIndex);

        // 获取设备属性
        IPropertyBag* pPropBag = nullptr;
        hr = pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pPropBag);
        if (SUCCEEDED(hr)) {
            // 获取友好名称
            VARIANT varName;
            VariantInit(&varName);
            hr = pPropBag->Read(L"FriendlyName", &varName, nullptr);
            if (SUCCEEDED(hr)) {
                info.name = QString::fromWCharArray(varName.bstrVal);
                VariantClear(&varName);
            }

            // 获取设备路径
            VARIANT varPath;
            VariantInit(&varPath);
            hr = pPropBag->Read(L"DevicePath", &varPath, nullptr);
            if (SUCCEEDED(hr)) {
                info.serialNumber = QString::fromWCharArray(varPath.bstrVal);
                VariantClear(&varPath);
            }

            pPropBag->Release();
        }

        cameras.append(info);
        Logger::instance().info(
            QString("DirectShow: 发现相机 %1 - %2").arg(info.deviceId).arg(info.name),
            "Camera");

        pMoniker->Release();
        deviceIndex++;
    }

    pEnum->Release();
    CoUninitialize();
#else
    Logger::instance().warning("DirectShow 仅在 Windows 上支持", "Camera");
#endif

    return cameras;
}

QObject* DirectShowCameraPlugin::createCamera(const CameraInfo& info)
{
    if (info.pluginId != "directshow") {
        return nullptr;
    }

    DirectShowCamera* camera = new DirectShowCamera(info.deviceId);
    return qobject_cast<QObject*>(camera);
}

QString DirectShowCameraPlugin::getDeviceName(int index) const
{
#ifdef _WIN32
    QString name = QString("DirectShow Camera %1").arg(index);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return name;
    }

    ICreateDevEnum* pDevEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        CoUninitialize();
        return name;
    }

    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    pDevEnum->Release();

    if (hr != S_OK) {
        CoUninitialize();
        return name;
    }

    IMoniker* pMoniker = nullptr;
    ULONG cFetched = 0;
    int currentIndex = 0;

    while (pEnum->Next(1, &pMoniker, &cFetched) == S_OK) {
        if (currentIndex == index) {
            IPropertyBag* pPropBag = nullptr;
            hr = pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pPropBag);
            if (SUCCEEDED(hr)) {
                VARIANT varName;
                VariantInit(&varName);
                hr = pPropBag->Read(L"FriendlyName", &varName, nullptr);
                if (SUCCEEDED(hr)) {
                    name = QString::fromWCharArray(varName.bstrVal);
                    VariantClear(&varName);
                }
                pPropBag->Release();
            }
            pMoniker->Release();
            break;
        }
        currentIndex++;
        pMoniker->Release();
    }

    pEnum->Release();
    CoUninitialize();
    return name;
#else
    return QString("DirectShow Camera %1").arg(index);
#endif
}

bool DirectShowCameraPlugin::isDirectShowDevice(int index) const
{
#ifdef _WIN32
    return getDeviceName(index).contains("DirectShow");
#else
    return false;
#endif
}

} // namespace DeepLux
