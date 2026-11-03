#include "SaveImagePlugin.h"

#include "agent/AgentFileOutputAuthorization.h"
#include "common/Logger.h"
#include "core/common/ConfigWidgetHelper.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/imgcodecs.hpp>
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

SaveImagePlugin::SaveImagePlugin(QObject* parent) : ModuleBase(parent), m_format("png"), m_quality(95) {
    m_defaultParams = QJsonObject{{"filePath", ""}, {"format", "png"}, {"quality", 95}};
    m_params = m_defaultParams;
}

SaveImagePlugin::~SaveImagePlugin() {}

bool SaveImagePlugin::initialize() {
    return ModuleBase::initialize();
}

void SaveImagePlugin::shutdown() {
    ModuleBase::shutdown();
}

ExecutionResult SaveImagePlugin::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    m_writeUnconfirmed = false;
    m_writeDiagnostics.clear();
    const QString authorizationError = agentFileOutputAuthorizationError(
        property(AgentFileOutputAuthorizationProperty).toJsonObject(), instanceName(), moduleId(), currentParams());
    if (!authorizationError.isEmpty())
        return ExecutionResult::fail(ExecError::ConfirmedTargetChanged, tr("已批准的图像输出配置或目标已变化"),
                                     authorizationError);
    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (!result.success && m_writeUnconfirmed)
        return ExecutionResult::fail(ExecError::SideEffectUncertain, result.userMessage, m_writeDiagnostics);
    return result;
}

bool SaveImagePlugin::process(const ImageData& input, ImageData& output) {
    QJsonObject params = currentParams();
    QString filePath = params["filePath"].toString();
    QString format = params["format"].toString();
    int quality = params["quality"].toInt(95);

    if (filePath.isEmpty()) {
        emit errorOccurred(tr("未指定保存路径"));
        return false;
    }

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat mat;
    if (input.hasMat()) {
        mat = input.toMat();
    } else {
        const QImage qimage = input.toQImage();
        if (qimage.isNull()) {
            emit errorOccurred(tr("输入图像无效"));
            return false;
        }
        mat = qImageToMat(qimage);
        if (mat.empty()) {
            emit errorOccurred(tr("图像转换失败"));
            return false;
        }
    }
    if (mat.empty()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }
    QByteArray encodedImage;
    QString encoderFormat = QFileInfo(filePath).suffix().toLower();
    if (encoderFormat.isEmpty())
        encoderFormat = format.toLower();
    std::vector<int> encodeParams;
    if (format == "jpg" || format == "jpeg") {
        encodeParams.push_back(cv::IMWRITE_JPEG_QUALITY);
        encodeParams.push_back(quality);
    } else if (format == "png") {
        encodeParams.push_back(cv::IMWRITE_PNG_COMPRESSION);
        encodeParams.push_back((100 - quality) * 9 / 100);
    }
    try {
        std::vector<uchar> bytes;
        if (!cv::imencode("." + encoderFormat.toStdString(), mat, bytes, encodeParams)) {
            emit errorOccurred(tr("图像编码失败"));
            return false;
        }
        encodedImage = QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
    } catch (const cv::Exception& e) {
        emit errorOccurred(QString("图像编码失败: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
#else
    const QImage qimage = input.toQImage();
    if (qimage.isNull()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }
    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists() && !dir.mkpath(".")) {
        emit errorOccurred(tr("无法创建目录: %1").arg(dir.path()));
        return false;
    }
    if (!qimage.save(filePath)) {
        emit errorOccurred(tr("保存图像失败"));
        return false;
    }
    Logger::instance().debug(QString("图像已保存: %1").arg(filePath), "SaveImage");
    output = input;
    return true;
#endif

#ifdef DEEPLUX_HAS_OPENCV
    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit errorOccurred(tr("无法创建目录: %1").arg(dir.path()));
            return false;
        }
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        emit errorOccurred(QString("无法打开图像输出文件: %1").arg(filePath));
        return false;
    }
    m_writeUnconfirmed = true;
    const qint64 bytesWritten = file.write(encodedImage);
    const bool flushOk = file.flush();
    const QFileDevice::FileError fileErrorBeforeClose = file.error();
    file.close();
    const QFileDevice::FileError fileErrorAfterClose = file.error();
    if (bytesWritten != encodedImage.size() || !flushOk || fileErrorBeforeClose != QFileDevice::NoError ||
        fileErrorAfterClose != QFileDevice::NoError) {
        m_writeDiagnostics = QStringLiteral("format=%1 bytesWritten=%2 expectedBytes=%3 flushOk=%4 "
                                            "fileErrorBeforeClose=%5 fileErrorAfterClose=%6 writeAttempted=true")
                                 .arg(encoderFormat)
                                 .arg(bytesWritten)
                                 .arg(encodedImage.size())
                                 .arg(flushOk ? QStringLiteral("true") : QStringLiteral("false"))
                                 .arg(static_cast<int>(fileErrorBeforeClose))
                                 .arg(static_cast<int>(fileErrorAfterClose));
        emit errorOccurred(QString("无法确认图像写入结果: %1").arg(filePath));
        return false;
    }
    m_writeUnconfirmed = false;

    Logger::instance().debug(QString("图像已保存: %1").arg(filePath), "SaveImage");
    output = input;
    return true;
#endif
}

#ifdef DEEPLUX_HAS_OPENCV
// qImageToMat is defined in ImageData.cpp
#endif

bool SaveImagePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    // 阶段 2: filePath 等外部资源允许暂时为空，由 process() 在运行时报告"未配置"
    Q_UNUSED(params)
    error.clear();
    return true;
}

QWidget* SaveImagePlugin::createConfigWidget() {
    ConfigWidgetHelper factory(true); // 默认深色主题

    QWidget* widget = new QWidget();
    factory.applyContainerStyle(widget);
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 保存路径
    layout->addWidget(factory.createLabel(tr("保存路径:")));
    QLineEdit* filePathEdit = factory.createLineEdit(m_params["filePath"].toString());
    QPushButton* browseBtn = new QPushButton(tr("浏览..."));
    browseBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #2d3748;
            color: #e8f4f8;
            border: 1px solid #4a5568;
            border-radius: 4px;
            padding: 6px 16px;
            min-height: 28px;
        }
        QPushButton:hover {
            background-color: #3d4758;
        }
    )");
    QHBoxLayout* pathLayout = new QHBoxLayout();
    pathLayout->setSpacing(8);
    pathLayout->addWidget(filePathEdit);
    pathLayout->addWidget(browseBtn);
    layout->addLayout(pathLayout);

    // 图像格式
    layout->addWidget(factory.createLabel(tr("图像格式:")));
    QComboBox* formatCombo = factory.createComboBox();
    formatCombo->addItem("PNG", "png");
    formatCombo->addItem("JPEG", "jpg");
    formatCombo->addItem("BMP", "bmp");
    formatCombo->addItem("TIFF", "tiff");
    int formatIndex = formatCombo->findData(m_params["format"].toString());
    if (formatIndex >= 0)
        formatCombo->setCurrentIndex(formatIndex);
    layout->addWidget(formatCombo);

    layout->addStretch();

    // 使用 QPointer 避免悬垂指针
    QPointer<SaveImagePlugin> pluginPtr(this);
    connect(browseBtn, &QPushButton::clicked, [pluginPtr, filePathEdit, formatCombo]() {
        if (!pluginPtr)
            return;
        QString format = formatCombo->currentData().toString();
        QString filter;
        if (format == "png")
            filter = "PNG (*.png)";
        else if (format == "jpg")
            filter = "JPEG (*.jpg *.jpeg)";
        else if (format == "bmp")
            filter = "BMP (*.bmp)";
        else if (format == "tiff")
            filter = "TIFF (*.tif *.tiff)";

        QString path =
            QFileDialog::getSaveFileName(nullptr, tr("保存图像"), pluginPtr->m_params["filePath"].toString(), filter);
        if (!path.isEmpty()) {
            filePathEdit->setText(path);
            pluginPtr->setParam("filePath", path);
        }
    });

    QPointer<SaveImagePlugin> pluginPtr2(this);
    connect(filePathEdit, &QLineEdit::textChanged, [pluginPtr2](const QString& text) {
        if (pluginPtr2)
            pluginPtr2->setParam("filePath", text);
    });

    QPointer<SaveImagePlugin> pluginPtr3(this);
    connect(formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), [pluginPtr3, formatCombo](int index) {
        if (pluginPtr3)
            pluginPtr3->setParam("format", formatCombo->itemData(index).toString());
    });

    return widget;
}

IModule* SaveImagePlugin::cloneImpl() const {
    // 创建新实例
    SaveImagePlugin* clone = new SaveImagePlugin();
    // 直接拷贝基本类型成员变量，避免 QJsonObject 深拷贝的开销
    clone->m_filePath = m_filePath;
    clone->m_format = m_format;
    clone->m_quality = m_quality;
    return clone;
}

} // namespace DeepLux
