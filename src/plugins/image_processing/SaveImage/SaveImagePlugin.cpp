#include "SaveImagePlugin.h"
#include "common/Logger.h"
#include "core/common/ConfigWidgetHelper.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFileDialog>
#include <QDir>
#include <QPointer>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#endif

namespace DeepLux {

SaveImagePlugin::SaveImagePlugin(QObject* parent)
    : ModuleBase(parent)
    , m_format("png")
    , m_quality(95)
{
    m_defaultParams = QJsonObject{
        {"filePath", ""},
        {"format", "png"},
        {"quality", 95}
    };
    m_params = m_defaultParams;
}

SaveImagePlugin::~SaveImagePlugin()
{
}

bool SaveImagePlugin::initialize()
{
    return ModuleBase::initialize();
}

void SaveImagePlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    m_mat.release();
#endif
    ModuleBase::shutdown();
}

bool SaveImagePlugin::process(const ImageData& input, ImageData& output)
{
    QString filePath = m_params["filePath"].toString();
    QString format = m_params["format"].toString();
    int quality = m_params["quality"].toInt(95);

    if (filePath.isEmpty()) {
        emit errorOccurred(tr("未指定保存路径"));
        return false;
    }

    QFileInfo fileInfo(filePath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        if (!dir.mkpath(".")) {
            emit errorOccurred(tr("无法创建目录: %1").arg(dir.path()));
            return false;
        }
    }

#ifdef DEEPLUX_HAS_OPENCV
    // Try to get cv::Mat first
    if (input.hasMat()) {
        return saveImage(filePath, format, quality);
    }

    // Fallback to QImage conversion
    QImage qimage = input.toQImage();
    if (!qimage.isNull()) {
        cv::Mat mat = qImageToMat(qimage);
        if (mat.empty()) {
            emit errorOccurred(tr("图像转换失败"));
            return false;
        }

        std::vector<int> params;
        if (format == "jpg" || format == "jpeg") {
            params.push_back(cv::IMWRITE_JPEG_QUALITY);
            params.push_back(quality);
        } else if (format == "png") {
            params.push_back(cv::IMWRITE_PNG_COMPRESSION);
            params.push_back((100 - quality) * 9 / 100); // PNG compression: 0-9
        }

        bool success = cv::imwrite(filePath.toUtf8().constData(), mat, params);
        if (!success) {
            emit errorOccurred(tr("保存图像失败"));
            return false;
        }

        Logger::instance().debug(QString("图像已保存: %1").arg(filePath), "SaveImage");
        output = input;
        return true;
    }
#else
    QImage image = input.toQImage();
    if (image.isNull()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }

    if (!image.save(filePath)) {
        emit errorOccurred(tr("保存图像失败"));
        return false;
    }
#endif

    Logger::instance().debug(QString("图像已保存: %1").arg(filePath), "SaveImage");
    output = input;
    return true;
}

bool SaveImagePlugin::saveImage(const QString& filePath, const QString& format, int quality)
{
#ifdef DEEPLUX_HAS_OPENCV
    try {
        cv::Mat image = m_mat;
        if (image.empty()) {
            emit errorOccurred(tr("图像数据无效"));
            return false;
        }

        std::vector<int> params;
        if (format == "jpg" || format == "jpeg") {
            params.push_back(cv::IMWRITE_JPEG_QUALITY);
            params.push_back(quality);
        } else if (format == "png") {
            params.push_back(cv::IMWRITE_PNG_COMPRESSION);
            params.push_back((100 - quality) * 9 / 100);
        } else if (format == "tif" || format == "tiff") {
            params.push_back(cv::IMWRITE_TIFF_COMPRESSION);
            params.push_back(quality);
        }

        bool success = cv::imwrite(filePath.toUtf8().constData(), image, params);
        if (!success) {
            emit errorOccurred(tr("保存图像失败"));
            return false;
        }

        Logger::instance().debug(QString("OpenCV图像已保存: %1").arg(filePath), "SaveImage");
        return true;

    } catch (const cv::Exception& e) {
        emit errorOccurred(QString("保存图像失败: %1").arg(QString::fromUtf8(e.what())));
        return false;
    }
#else
    Q_UNUSED(filePath);
    Q_UNUSED(format);
    Q_UNUSED(quality);
    return false;
#endif
}

#ifdef DEEPLUX_HAS_OPENCV
// qImageToMat is defined in ImageData.cpp
#endif

bool SaveImagePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["filePath"].toString().isEmpty()) {
        error = tr("必须指定保存路径");
        return false;
    }
    return true;
}

QWidget* SaveImagePlugin::createConfigWidget()
{
    ConfigWidgetHelper factory(true);  // 默认深色主题

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
    if (formatIndex >= 0) formatCombo->setCurrentIndex(formatIndex);
    layout->addWidget(formatCombo);

    layout->addStretch();

    // 使用 QPointer 避免悬垂指针
    QPointer<SaveImagePlugin> pluginPtr(this);
    connect(browseBtn, &QPushButton::clicked, [pluginPtr, filePathEdit, formatCombo]() {
        if (!pluginPtr) return;
        QString format = formatCombo->currentData().toString();
        QString filter;
        if (format == "png") filter = "PNG (*.png)";
        else if (format == "jpg") filter = "JPEG (*.jpg *.jpeg)";
        else if (format == "bmp") filter = "BMP (*.bmp)";
        else if (format == "tiff") filter = "TIFF (*.tif *.tiff)";

        QString path = QFileDialog::getSaveFileName(nullptr, tr("保存图像"),
            pluginPtr->m_params["filePath"].toString(), filter);
        if (!path.isEmpty()) {
            filePathEdit->setText(path);
            pluginPtr->m_params["filePath"] = path;
        }
    });

    QPointer<SaveImagePlugin> pluginPtr2(this);
    connect(filePathEdit, &QLineEdit::textChanged, [pluginPtr2](const QString& text) {
        if (pluginPtr2) pluginPtr2->m_params["filePath"] = text;
    });

    QPointer<SaveImagePlugin> pluginPtr3(this);
    connect(formatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [pluginPtr3, formatCombo](int index) {
        if (pluginPtr3) pluginPtr3->m_params["format"] = formatCombo->itemData(index).toString();
    });

    return widget;
}

IModule* SaveImagePlugin::cloneImpl() const
{
    // 创建新实例
    SaveImagePlugin* clone = new SaveImagePlugin();
    // 直接拷贝基本类型成员变量，避免 QJsonObject 深拷贝的开销
    clone->m_filePath = m_filePath;
    clone->m_format = m_format;
    clone->m_quality = m_quality;
    return clone;
}

} // namespace DeepLux
