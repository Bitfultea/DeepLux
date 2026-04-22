#include "DirectShowCamera.h"
#include "core/common/Logger.h"

#include <QLabel>
#include <QVBoxLayout>
#include <QJsonObject>
#include <QDateTime>

#ifdef _WIN32
#include <dshow.h>
#include <windows.h>
#pragma comment(lib, "strmiids.lib")
#endif

namespace DeepLux {

DirectShowCamera::DirectShowCamera(const QString& devicePath, QObject* parent)
    : ICamera(parent)
    , m_devicePath(devicePath)
    , m_name(QString("DirectShow %1").arg(devicePath))
{
    m_capabilities.softwareTrigger = true;
    m_capabilities.continuousMode = true;
    m_capabilities.exposureControl = true;
    m_capabilities.gainControl = true;
    m_capabilities.frameRateControl = true;
    m_capabilities.roiControl = true;
    m_capabilities.supportedOnLinux = false;
    m_capabilities.supportedOnWindows = true;
}

DirectShowCamera::~DirectShowCamera()
{
    disconnect();
}

bool DirectShowCamera::connect()
{
#ifdef _WIN32
    if (m_connected) {
        return true;
    }

    if (!openDevice()) {
        emit errorOccurred(QString("无法打开设备: %1").arg(m_devicePath));
        return false;
    }

    m_connected = true;
    emit connectedChanged(true);
    Logger::instance().info(QString("DirectShow 相机已连接: %1").arg(m_devicePath), "Camera");
    return true;
#else
    emit errorOccurred("DirectShow 仅在 Windows 上可用");
    return false;
#endif
}

void DirectShowCamera::disconnect()
{
#ifdef _WIN32
    if (!m_connected) {
        return;
    }

    stopAcquisition();
    closeDevice();

    m_connected = false;
    emit connectedChanged(false);
    Logger::instance().info(QString("DirectShow 相机已断开: %1").arg(m_devicePath), "Camera");
#endif
}

bool DirectShowCamera::openDevice()
{
#ifdef _WIN32
    if (m_pGraph) {
        return true;
    }

    // 初始化 COM
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        qWarning() << "CoInitializeEx failed:" << hr;
        return false;
    }

    // 创建 Filter Graph
    hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                          IID_IGraphBuilder, (void**)&m_pGraph);
    if (FAILED(hr)) {
        qWarning() << "Failed to create FilterGraph:" << hr;
        return false;
    }

    // 创建 Capture Graph Builder
    hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICaptureGraphBuilder2, (void**)&m_pCapture);
    if (FAILED(hr)) {
        qWarning() << "Failed to create CaptureGraphBuilder2:" << hr;
        closeDevice();
        return false;
    }

    // 设置 Capture Graph
    hr = m_pCapture->SetFiltergraph(m_pGraph);
    if (FAILED(hr)) {
        qWarning() << "Failed to SetFiltergraph:" << hr;
        closeDevice();
        return false;
    }

    // 创建媒体控制接口
    hr = m_pGraph->QueryInterface(IID_IMediaControl, (void**)&m_pControl);
    if (FAILED(hr)) {
        qWarning() << "Failed to get IMediaControl:" << hr;
        closeDevice();
        return false;
    }

    // 创建设备枚举器
    ICreateDevEnum* pDevEnum = nullptr;
    hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                          IID_ICreateDevEnum, (void**)&pDevEnum);
    if (FAILED(hr)) {
        qWarning() << "Failed to create SystemDeviceEnum:" << hr;
        closeDevice();
        return false;
    }

    // 获取视频输入设备枚举
    IEnumMoniker* pEnum = nullptr;
    hr = pDevEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &pEnum, 0);
    pDevEnum->Release();

    if (hr != S_OK) {
        qWarning() << "No video capture devices found";
        closeDevice();
        return false;
    }

    // 遍历设备查找目标
    IMoniker* pMoniker = nullptr;
    ULONG cFetched = 0;
    int deviceIndex = m_devicePath.mid(5).toInt();  // "DS0" -> 0, "DS1" -> 1

    while (pEnum->Next(1, &pMoniker, &cFetched) == S_OK) {
        if (deviceIndex == 0) {
            // 绑定设备
            hr = pMoniker->BindToObject(nullptr, nullptr, IID_IBaseFilter, (void**)&m_pDeviceFilter);
            if (SUCCEEDED(hr)) {
                // 添加到 Graph
                hr = m_pGraph->AddFilter(m_pDeviceFilter, L"Capture Device");
                if (SUCCEEDED(hr)) {
                    // 获取设备名称
                    IPropertyBag* pPropBag = nullptr;
                    hr = pMoniker->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&pPropBag);
                    if (SUCCEEDED(hr)) {
                        VARIANT varName;
                        VariantInit(&varName);
                        hr = pPropBag->Read(L"FriendlyName", &varName, nullptr);
                        if (SUCCEEDED(hr)) {
                            m_name = QString::fromWCharArray(varName.bstrVal);
                            VariantClear(&varName);
                        }
                        pPropBag->Release();
                    }
                }
            }
            pMoniker->Release();
            break;
        }
        deviceIndex--;
        pMoniker->Release();
    }
    pEnum->Release();

    if (!m_pDeviceFilter) {
        qWarning() << "Failed to find device at index:" << m_devicePath.mid(5).toInt();
        closeDevice();
        return false;
    }

    configureFormat(640, 480);
    return true;
#else
    return false;
#endif
}

void DirectShowCamera::closeDevice()
{
#ifdef _WIN32
    if (m_pControl) {
        m_pControl->Stop();
        m_pControl->Release();
        m_pControl = nullptr;
    }

    if (m_pDeviceFilter) {
        m_pGraph->RemoveFilter(m_pDeviceFilter);
        m_pDeviceFilter->Release();
        m_pDeviceFilter = nullptr;
    }

    if (m_pCapture) {
        m_pCapture->Release();
        m_pCapture = nullptr;
    }

    if (m_pGraph) {
        m_pGraph->Release();
        m_pGraph = nullptr;
    }

    CoUninitialize();
#endif
}

bool DirectShowCamera::configureFormat(int width, int height)
{
#ifdef _WIN32
    Q_UNUSED(width)
    Q_UNUSED(height)
    // DirectShow 格式配置需要在 IAMStreamConfig 上进行
    // 这里简化处理，使用默认格式
    m_roi = QRect(0, 0, 640, 480);
    return true;
#else
    return false;
#endif
}

bool DirectShowCamera::startAcquisition()
{
#ifdef _WIN32
    if (!m_connected || !m_pControl) {
        return false;
    }

    if (m_acquiring) {
        return true;
    }

    // 启动预览/捕获
    HRESULT hr = m_pControl->Run();
    if (FAILED(hr)) {
        qWarning() << "Failed to start capture:" << hr;
        return false;
    }

    m_acquiring = true;
    emit acquiringChanged(true);
    Logger::instance().info(QString("DirectShow 开始采集: %1").arg(m_devicePath), "Camera");
    return true;
#else
    return false;
#endif
}

void DirectShowCamera::stopAcquisition()
{
#ifdef _WIN32
    if (!m_acquiring || !m_pControl) {
        return;
    }

    m_pControl->Stop();
    m_acquiring = false;
    emit acquiringChanged(false);
    Logger::instance().info(QString("DirectShow 停止采集: %1").arg(m_devicePath), "Camera");
#endif
}

bool DirectShowCamera::triggerSoftware()
{
    return grabFrame();
}

bool DirectShowCamera::grabFrame()
{
#ifdef _WIN32
    if (!m_connected) {
        return false;
    }

    // 创建测试图像（实际应用中应该使用 Sample Grabber filter 捕获帧）
    QImage testImage(m_roi.width(), m_roi.height(), QImage::Format_RGB888);
    testImage.fill(Qt::darkGray);

    m_lastImage = testImage;
    m_metadata["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    m_metadata["device"] = m_devicePath;
    m_metadata["plugin"] = "DirectShow";

    emit imageReceived(m_lastImage, m_metadata);
    return true;
#else
    return false;
#endif
}

void DirectShowCamera::setTriggerMode(TriggerMode mode)
{
    m_triggerMode = mode;
}

void DirectShowCamera::setExposureTime(double microseconds)
{
    m_exposureTime = microseconds;
    // DirectShow 使用 IAMCameraControl 设置曝光
#ifdef _WIN32
    if (m_pDeviceFilter) {
        IAMCameraControl* pCameraControl = nullptr;
        HRESULT hr = m_pDeviceFilter->QueryInterface(IID_IAMCameraControl, (void**)&pCameraControl);
        if (SUCCEEDED(hr)) {
            long minVal, maxVal, step, defVal;
            pCameraControl->GetRange(CameraControl_Exposure, &minVal, &maxVal, &step, &defVal, nullptr);
            long value = static_cast<long>(microseconds);
            pCameraControl->SetRange(CameraControl_Exposure, value, CameraControl_Flags_Auto);
            pCameraControl->Release();
        }
    }
#endif
}

void DirectShowCamera::setGain(double gain)
{
    m_gain = gain;
#ifdef _WIN32
    if (m_pDeviceFilter) {
        IAMCameraControl* pCameraControl = nullptr;
        HRESULT hr = m_pDeviceFilter->QueryInterface(IID_IAMCameraControl, (void**)&pCameraControl);
        if (SUCCEEDED(hr)) {
            long minVal, maxVal, step, defVal;
            pCameraControl->GetRange(CameraControl_Gain, &minVal, &maxVal, &step, &defVal, nullptr);
            long value = static_cast<long>(gain * 100);
            pCameraControl->SetRange(CameraControl_Gain, value, CameraControl_Flags_Manual);
            pCameraControl->Release();
        }
    }
#endif
}

void DirectShowCamera::setFrameRate(double fps)
{
    m_frameRate = fps;
    // DirectShow 帧率配置需要在 IAMStreamConfig 上进行
}

void DirectShowCamera::setRoi(int x, int y, int width, int height)
{
    m_roi = QRect(x, y, width, height);
    configureFormat(width, height);
}

QWidget* DirectShowCamera::createConfigWidget()
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

QJsonObject DirectShowCamera::toJson() const
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

bool DirectShowCamera::fromJson(const QJsonObject& json)
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
