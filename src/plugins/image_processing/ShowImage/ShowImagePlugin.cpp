#include "ShowImagePlugin.h"

#include "common/Logger.h"
#include "core/display/DisplayData.h"
#include "core/display/IDisplayPort.h"

#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QSpinBox>
#include <QVBoxLayout>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

ShowImagePlugin::ShowImagePlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"windowTitle", "Display"}, {"delay", 0}};
    m_params = m_defaultParams;
}

ShowImagePlugin::~ShowImagePlugin() {}

bool ShowImagePlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ShowImagePlugin initialized";
    return true;
}

void ShowImagePlugin::shutdown() {
#ifdef DEEPLUX_HAS_OPENCV
    m_displayMat.release();
#endif
    ModuleBase::shutdown();
}

bool ShowImagePlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    // Store input for display output via IDisplayPort
    m_displayData = DisplayData(input);

    // Store delay in metadata for DisplayManager to use
    QJsonObject params = currentParams();
    int delay = params["delay"].toInt();
    if (delay > 0) {
        m_displayData.metadata()["delay"] = delay;
    } else {
        m_displayData.metadata().remove("delay");
    }

#ifdef DEEPLUX_HAS_OPENCV
    if (input.hasMat()) {
        m_displayMat = input.toMat();
    } else {
        m_displayMat = qImageToMat(input.toQImage());
    }

    if (m_displayMat.empty()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }

    // 获取图像信息
    int width = m_displayMat.cols;
    int height = m_displayMat.rows;
    int channels = m_displayMat.channels();

    QString info = QString("图像: %1x%2, 通道: %3").arg(width).arg(height).arg(channels);
    output.setData("image_info", info);

    // 注意：OpenCV 窗口显示已在 shutdown() 中销毁
    // 如需显示图像，应使用 Qt 的 QLabel 或在独立线程中运行 OpenCV 窗口

    Logger::instance().debug(QString("显示图像: %1x%2").arg(width).arg(height), "ShowImage");
    return true;
#else
    QImage image = input.toQImage();
    if (image.isNull()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }

    output.setData("image_info", QString("图像: %1x%2").arg(image.width()).arg(image.height()));
    return true;
#endif
}

bool ShowImagePlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* ShowImagePlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    layout->addWidget(new QLabel(tr("窗口标题:")));
    QLineEdit* titleEdit = new QLineEdit(m_params["windowTitle"].toString());
    layout->addWidget(titleEdit);

    layout->addWidget(new QLabel(tr("显示延迟 (ms):")));
    QSpinBox* delaySpin = new QSpinBox();
    delaySpin->setRange(0, 10000);
    delaySpin->setValue(m_params["delay"].toInt());
    delaySpin->setSingleStep(100);
    layout->addWidget(delaySpin);

    layout->addStretch();

    // 使用 QPointer 避免悬垂指针
    QPointer<ShowImagePlugin> pluginPtr(this);
    connect(titleEdit, &QLineEdit::textChanged, [pluginPtr](const QString& text) {
        if (pluginPtr)
            pluginPtr->setParam("windowTitle", text);
    });

    QPointer<ShowImagePlugin> delayPlugin(this);
    connect(delaySpin, QOverload<int>::of(&QSpinBox::valueChanged), [delayPlugin](int value) {
        if (delayPlugin)
            delayPlugin->setParam("delay", value);
    });

    return widget;
}

IModule* ShowImagePlugin::cloneImpl() const {
    auto* clone = new ShowImagePlugin();
    clone->setParams(currentParams());
    return clone;
}

bool ShowImagePlugin::hasDisplayOutput() const {
    return m_displayData.isValid();
}

DisplayData ShowImagePlugin::getDisplayData() const {
    DisplayData data(m_displayData);
    data.metadata()["plugin"] = QStringLiteral("ShowImagePlugin");
    data.metadata()["windowTitle"] = m_params["windowTitle"].toString();
    return data;
}

QString ShowImagePlugin::preferredViewport() const {
    return QString(); // Empty means any available viewport
}

} // namespace DeepLux
