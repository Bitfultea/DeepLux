#include "GrabImagePlugin.h"
#include "core/device/CameraManager.h"
#include "core/interface/ICamera.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QCoreApplication>
#include <QThread>
#include <QElapsedTimer>
#include <QSpinBox>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/videoio.hpp>
#endif

namespace DeepLux {

GrabImagePlugin::GrabImagePlugin(QObject* parent)
    : ModuleBase(parent)
    , m_grabTimer(new QTimer(this))
    , m_grabTimedOut(false)
    , m_grabSuccess(false)
{
    m_defaultParams = QJsonObject{
        {"cameraId", ""},
        {"useFile", false},
        {"filePath", ""},
        {"exposureTime", 10000.0},
        {"gain", 1.0},
        {"grabSource", "Camera"},
        {"grabTimeout", 5000}  // camera grab timeout in ms
    };
    m_params = m_defaultParams;

    m_grabTimer->setSingleShot(true);
    connect(m_grabTimer, &QTimer::timeout, this, &GrabImagePlugin::onCameraGrabTimeout);
}

GrabImagePlugin::~GrabImagePlugin()
{
}

bool GrabImagePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    return true;
}

void GrabImagePlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    if (m_capture.isOpened()) {
        m_capture.release();
    }
#endif
    ModuleBase::shutdown();
}

void GrabImagePlugin::onCameraGrabTimeout()
{
    m_grabTimedOut = true;
    m_grabSuccess = false;
}

bool GrabImagePlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input)

    // 从 m_params 读取图像源
    QString sourceStr = m_params["grabSource"].toString();
    QString filePath = m_params["filePath"].toString();

    Logger::instance().debug(QString("GrabImage::process - grabSource: %1, filePath: %2").arg(sourceStr).arg(filePath), "GrabImage");

    GrabSource source = GrabSource::Demo; // 默认演示模式
    if (sourceStr == "File") {
        source = GrabSource::File;
    } else if (sourceStr == "Camera") {
        source = GrabSource::Camera;
    }

    switch (source) {
        case GrabSource::File:
            return grabFromFile(output);
        case GrabSource::Demo:
            return grabDemo(output);
        case GrabSource::Camera:
        default:
            return grabFromCamera(output);
    }
}

bool GrabImagePlugin::grabFromCamera(ImageData& output)
{
    QString cameraId = m_params["cameraId"].toString();
    int grabTimeoutMs = m_params["grabTimeout"].toInt(5000);

    // ========== 方案 A：通过 CameraManager / ICamera 取图 ==========
    CameraManager& cm = CameraManager::instance();

    // 1. 查找目标相机（先找已连接的）
    ICamera* targetCamera = nullptr;
    QList<ICamera*> connectedCameras = cm.allCameras();

    if (!cameraId.isEmpty()) {
        for (ICamera* cam : connectedCameras) {
            if (cam && cam->deviceId() == cameraId) {
                targetCamera = cam;
                break;
            }
        }
        // 未连接则尝试连接
        if (!targetCamera) {
            if (cm.connectCamera(cameraId)) {
                targetCamera = cm.getCamera(cameraId);
            }
        }
    } else {
        if (!connectedCameras.isEmpty()) {
            targetCamera = connectedCameras.first();
        } else {
            QList<CameraStatus> discovered = cm.discoverCameras();
            for (const CameraStatus& status : discovered) {
                if (cm.connectCamera(status.deviceId)) {
                    targetCamera = cm.getCamera(status.deviceId);
                    if (targetCamera) break;
                }
            }
        }
    }

    if (targetCamera) {
        // 应用曝光和增益参数
        double exposureTime = m_params["exposureTime"].toDouble(10000.0);
        double gain = m_params["gain"].toDouble(1.0);
        targetCamera->setExposureTime(exposureTime);
        targetCamera->setGain(gain);
        Logger::instance().debug(QString("GrabImage set camera params: exposure=%1, gain=%2").arg(exposureTime).arg(gain), "GrabImage");

        // 确保开始采集
        if (!targetCamera->isAcquiring()) {
            targetCamera->setTriggerMode(TriggerMode::Software);
            if (!targetCamera->startAcquisition()) {
                emit errorOccurred(tr("相机开始采集失败: %1").arg(targetCamera->deviceId()));
                return false;
            }
        }

        m_grabTimedOut = false;
        m_grabSuccess = false;
        m_grabTimer->start(grabTimeoutMs);

        // 软触发取图
        if (!targetCamera->triggerSoftware()) {
            m_grabTimer->stop();
            emit errorOccurred(tr("软触发失败: %1").arg(targetCamera->deviceId()));
            return false;
        }

        // 轮询等待图像
        QElapsedTimer pollTimer;
        pollTimer.start();
        const int pollIntervalMs = 50;
        QImage frame;

        while (!m_grabTimedOut && pollTimer.elapsed() < grabTimeoutMs) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, pollIntervalMs);
            frame = targetCamera->lastImage();
            if (!frame.isNull()) {
                m_grabTimer->stop();
                m_grabSuccess = true;
                break;
            }
            QThread::msleep(pollIntervalMs);
        }

        m_grabTimer->stop();

        if (m_grabTimedOut || frame.isNull()) {
            emit errorOccurred(tr("相机 %1 采集图像超时 (%2ms)")
                .arg(targetCamera->deviceId()).arg(grabTimeoutMs));
            return false;
        }

        output.setQImage(frame);
        Logger::instance().info(QString("从相机 %1 采集图像成功: %2x%3")
            .arg(targetCamera->deviceId()).arg(frame.width()).arg(frame.height()), "GrabImage");
        return true;
    }

    // ========== 方案 B：回退到 OpenCV VideoCapture ==========
#ifdef DEEPLUX_HAS_OPENCV
    // 尝试打开相机
    int cameraIndex = 0;
    if (!cameraId.isEmpty()) {
        bool ok;
        cameraIndex = cameraId.toInt(&ok);
        if (!ok) cameraIndex = 0;
    }

    if (!m_capture.isOpened()) {
        m_capture.open(cameraIndex);
        if (!m_capture.isOpened()) {
            emit errorOccurred(tr("无法打开相机: %1").arg(cameraIndex));
            return false;
        }
    }

    // 设置相机参数
    double exposure = m_params["exposureTime"].toDouble();
    double gain = m_params["gain"].toDouble();
    m_capture.set(cv::CAP_PROP_EXPOSURE, exposure / 1000.0); // 转换为秒
    m_capture.set(cv::CAP_PROP_GAIN, gain);

    m_grabTimedOut = false;
    m_grabSuccess = false;
    m_grabTimer->start(grabTimeoutMs);

    if (!m_capture.grab()) {
        m_grabTimer->stop();
        emit errorOccurred(tr("相机 %1 采集图像失败").arg(cameraIndex));
        return false;
    }

    QElapsedTimer pollTimer;
    pollTimer.start();
    const int pollIntervalMs = 50;

    while (!m_grabTimedOut && pollTimer.elapsed() < grabTimeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, pollIntervalMs);
        if (m_grabTimer->isActive()) {
            if (m_capture.retrieve(m_frame)) {
                m_grabTimer->stop();
                m_grabSuccess = true;
                break;
            }
        }
        QThread::msleep(pollIntervalMs);
    }

    m_grabTimer->stop();

    if (m_grabTimedOut) {
        emit errorOccurred(tr("相机 %1 采集图像超时 (%2ms)").arg(cameraIndex).arg(grabTimeoutMs));
        return false;
    }

    if (m_frame.empty()) {
        emit errorOccurred(tr("采集图像失败"));
        return false;
    }

    output.setMat(m_frame);
    Logger::instance().info(QString("从相机 %1 (OpenCV) 采集图像成功: %2x%3")
        .arg(cameraIndex).arg(m_frame.cols).arg(m_frame.rows), "GrabImage");
    return true;
#else
    Q_UNUSED(output);
    emit errorOccurred(tr("无可用相机且 OpenCV 未启用"));
    return false;
#endif
}

bool GrabImagePlugin::grabFromFile(ImageData& output)
{
#ifdef DEEPLUX_HAS_OPENCV
    QString filePath = m_params["filePath"].toString();

    Logger::instance().debug(QString("GrabImage::grabFromFile - filePath: '%1'").arg(filePath), "GrabImage");

    if (filePath.isEmpty()) {
        Logger::instance().error("GrabImage::grabFromFile - filePath is EMPTY!", "GrabImage");
        emit errorOccurred(tr("未指定图像文件路径"));
        return false;
    }

    // 检查文件是否存在
    QFileInfo fileInfo(filePath);
    Logger::instance().debug(QString("GrabImage::grabFromFile - file exists: %1, isFile: %2").arg(fileInfo.exists()).arg(fileInfo.isFile()), "GrabImage");
    if (!fileInfo.exists()) {
        emit errorOccurred(QString("文件不存在: %1").arg(filePath));
        return false;
    }

    // 使用QImage加载图像（更好地处理各种格式和编码）
    QImage image;
    Logger::instance().debug("GrabImage::grabFromFile - trying QImage::load...", "GrabImage");
    if (!image.load(filePath)) {
        Logger::instance().warning("GrabImage::grabFromFile - QImage::load FAILED, trying cv::imread...", "GrabImage");
        // Fallback to cv::imread
        // 尝试 IMREAD_UNCHANGED 以支持 32 位浮点灰度 TIFF
        Logger::instance().debug("GrabImage::grabFromFile - calling cv::imread...", "GrabImage");
        cv::Mat mat = cv::imread(filePath.toUtf8().constData(), cv::IMREAD_UNCHANGED);
        Logger::instance().debug(QString("GrabImage::grabFromFile - cv::imread returned, empty=%1").arg(mat.empty()), "GrabImage");
        if (mat.empty()) {
            emit errorOccurred(QString("无法加载图像: %1\nOpenCV 不支持此格式").arg(filePath));
            return false;
        }
        output.setMat(mat.clone());
        Logger::instance().info(QString("加载图像成功(OpenCV): %1 (%2x%3, type=%4)")
            .arg(filePath).arg(mat.cols).arg(mat.rows).arg(mat.type()), "GrabImage");
        return true;
    }

    Logger::instance().debug(QString("GrabImage::grabFromFile - QImage loaded OK, format: %1, size: %2x%3").arg(image.format()).arg(image.width()).arg(image.height()), "GrabImage");

    // 转换为cv::Mat
    cv::Mat mat;
    if (image.format() == QImage::Format_RGB32 || image.format() == QImage::Format_ARGB32) {
        mat = cv::Mat(image.height(), image.width(), CV_8UC4, const_cast<uchar*>(image.bits()), image.bytesPerLine());
        cv::cvtColor(mat, mat, cv::COLOR_BGRA2BGR);
    } else if (image.format() == QImage::Format_RGB888) {
        mat = cv::Mat(image.height(), image.width(), CV_8UC3, const_cast<uchar*>(image.bits()), image.bytesPerLine());
        cv::cvtColor(mat, mat, cv::COLOR_RGB2BGR);
    } else if (image.format() == QImage::Format_Grayscale8) {
        mat = cv::Mat(image.height(), image.width(), CV_8UC1, const_cast<uchar*>(image.bits()), image.bytesPerLine());
    } else {
        Logger::instance().debug("GrabImage::grabFromFile - converting to RGB888...", "GrabImage");
        QImage rgbImage = image.convertToFormat(QImage::Format_RGB888);
        mat = cv::Mat(rgbImage.height(), rgbImage.width(), CV_8UC3, const_cast<uchar*>(rgbImage.bits()), rgbImage.bytesPerLine());
        cv::cvtColor(mat, mat, cv::COLOR_RGB2BGR);
    }

    if (mat.empty()) {
        emit errorOccurred(QString("图像数据无效: %1").arg(filePath));
        return false;
    }

    output.setMat(mat.clone());
    Logger::instance().info(QString("加载图像成功: %1 (%2x%3)").arg(filePath).arg(mat.cols).arg(mat.rows), "GrabImage");
    return true;
#else
    Q_UNUSED(output);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool GrabImagePlugin::grabDemo(ImageData& output)
{
#ifdef DEEPLUX_HAS_OPENCV
    // 生成演示图像 - 棋盘格图案
    int width = 640;
    int height = 480;
    cv::Mat image(height, width, CV_8UC1, cv::Scalar(0));

    int blockSize = 40;
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            bool isWhite = ((row / blockSize) + (col / blockSize)) % 2;
            image.at<uchar>(row, col) = isWhite ? 255 : 0;
        }
    }

    output.setMat(image.clone());
    Logger::instance().debug("生成演示图像成功", "GrabImage");
    return true;
#else
    Q_UNUSED(output);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

void GrabImagePlugin::setParam(const QString& key, const QVariant& value)
{
    ModuleBase::setParam(key, value);
    if (key == "filePath") {
        m_filePath = value.toString();
    } else if (key == "cameraId") {
        m_cameraId = value.toString();
    }
}

bool GrabImagePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    GrabSource source = static_cast<GrabSource>(
        params["grabSource"].toString() == "File" ? 1 :
        params["grabSource"].toString() == "Demo" ? 2 : 0);

    if (source == GrabSource::File) {
        if (params["filePath"].toString().isEmpty()) {
            error = tr("使用文件模式时，必须指定文件路径");
            return false;
        }
    }
    return true;
}

QWidget* GrabImagePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    // 图像源选择
    QComboBox* sourceCombo = new QComboBox();
    sourceCombo->addItem(tr("相机"), "Camera");
    sourceCombo->addItem(tr("文件"), "File");
    sourceCombo->addItem(tr("演示"), "Demo");

    QString currentSource = m_params["grabSource"].toString();
    if (currentSource.isEmpty()) currentSource = "Demo";
    int index = sourceCombo->findData(currentSource);
    if (index < 0) index = 2;

    // 先阻塞信号避免setCurrentIndex触发currentIndexChanged
    sourceCombo->blockSignals(true);
    sourceCombo->setCurrentIndex(index);
    sourceCombo->blockSignals(false);

    layout->addWidget(new QLabel(tr("图像源:")));
    layout->addWidget(sourceCombo);

    // 文件路径控件
    QLineEdit* filePathEdit = new QLineEdit(m_params["filePath"].toString());
    QPushButton* browseBtn = new QPushButton(tr("浏览..."));
    QHBoxLayout* fileLayout = new QHBoxLayout();
    fileLayout->addWidget(filePathEdit);
    fileLayout->addWidget(browseBtn);
    layout->addWidget(new QLabel(tr("文件路径:")));
    layout->addLayout(fileLayout);

    // 相机选择下拉框（从 CameraManager 获取可用相机）
    QComboBox* cameraCombo = new QComboBox();
    cameraCombo->addItem(tr("-- 请选择相机 --"), QString());
    QList<CameraStatus> cameras = CameraManager::instance().discoverCameras();
    for (const CameraStatus& cam : cameras) {
        QString label = QString("%1 (%2)").arg(cam.name).arg(cam.deviceId);
        cameraCombo->addItem(label, cam.deviceId);
    }
    QString currentCameraId = m_params["cameraId"].toString();
    int camIndex = cameraCombo->findData(currentCameraId);
    if (camIndex >= 0) {
        cameraCombo->setCurrentIndex(camIndex);
    }

    QLineEdit* exposureEdit = new QLineEdit(QString::number(m_params["exposureTime"].toDouble()));
    QLineEdit* gainEdit = new QLineEdit(QString::number(m_params["gain"].toDouble()));
    QSpinBox* timeoutSpin = new QSpinBox();
    timeoutSpin->setRange(100, 60000);
    timeoutSpin->setValue(m_params["grabTimeout"].toInt(5000));
    timeoutSpin->setPrefix(tr("采集超时(ms): "));

    layout->addWidget(new QLabel(tr("选择相机:")));
    layout->addWidget(cameraCombo);
    layout->addWidget(new QLabel(tr("曝光时间(us):")));
    layout->addWidget(exposureEdit);
    layout->addWidget(new QLabel(tr("增益:")));
    layout->addWidget(gainEdit);
    layout->addWidget(timeoutSpin);

    layout->addStretch();

    // 信号连接 - 使用activated而不是currentIndexChanged，避免初次设置时触发
    connect(sourceCombo, QOverload<int>::of(&QComboBox::activated),
            this, [=](int index) {
        QString source = sourceCombo->itemData(index).toString();
        m_params["grabSource"] = source;
    });

    connect(browseBtn, &QPushButton::clicked, this, [=]() {
        QString path = QFileDialog::getOpenFileName(widget, tr("选择图像文件"),
            QString(), tr("图像文件 (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
        if (!path.isEmpty()) {
            filePathEdit->setText(path);
            m_params["filePath"] = path;
        }
    });

    connect(filePathEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["filePath"] = text;
    });

    connect(cameraCombo, QOverload<int>::of(&QComboBox::activated), this, [=](int index) {
        m_params["cameraId"] = cameraCombo->itemData(index).toString();
    });

    connect(exposureEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["exposureTime"] = text.toDouble();
    });

    connect(gainEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["gain"] = text.toDouble();
    });

    connect(timeoutSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["grabTimeout"] = value;
    });

    return widget;
}

IModule* GrabImagePlugin::cloneImpl() const
{
    // 创建新实例，构造函数会初始化默认参数
    GrabImagePlugin* clone = new GrabImagePlugin();
    // 拷贝参数和成员变量
    clone->m_params = this->m_params;
    clone->m_cameraId = this->m_cameraId;
    clone->m_filePath = this->m_filePath;
    clone->m_grabTimedOut = this->m_grabTimedOut;
    clone->m_grabSuccess = this->m_grabSuccess;
    return clone;
}

} // namespace DeepLux
