#include "ShowImagePlugin.h"
#include "common/Logger.h"
#include "core/display/DisplayData.h"
#include "core/display/IDisplayPort.h"
#include "core/common/ConfigWidgetHelper.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPointer>

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

ShowImagePlugin::ShowImagePlugin(QObject* parent)
    : ModuleBase(parent)
    , m_displayHistogram(false)
    , m_windowCreated(false)
    , m_displayWidth(0)
    , m_displayHeight(0)
    , m_windowTitle("Display")
{
    m_defaultParams = QJsonObject{
        {"windowTitle", "Display"},
        {"displayWidth", 0},
        {"displayHeight", 0},
        {"displayHistogram", false},
        {"delay", 0}
    };
    m_params = m_defaultParams;
}

ShowImagePlugin::~ShowImagePlugin()
{
}

bool ShowImagePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "ShowImagePlugin initialized";
    return true;
}

void ShowImagePlugin::shutdown()
{
#ifdef DEEPLUX_HAS_OPENCV
    if (m_windowCreated) {
        QString windowTitle = m_params["windowTitle"].toString();
        if (windowTitle.isEmpty()) {
            windowTitle = "Display";
        }
        cv::destroyWindow(windowTitle.toStdString());
        m_windowCreated = false;
    }
    m_displayMat.release();
#endif
    ModuleBase::shutdown();
}

bool ShowImagePlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // Store input for display output via IDisplayPort
    m_displayData = DisplayData(input);

    // Store delay in metadata for DisplayManager to use
    int delay = m_params["delay"].toInt();
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

    QString info = QString("图像: %1x%2, 通道: %3")
                      .arg(width)
                      .arg(height)
                      .arg(channels);
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

bool ShowImagePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* ShowImagePlugin::createConfigWidget()
{
    ConfigWidgetHelper factory(true);  // 默认深色主题

    QWidget* widget = new QWidget();
    factory.applyContainerStyle(widget);
    QVBoxLayout* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // 窗口标题
    layout->addWidget(factory.createLabel(tr("窗口标题:")));
    QLineEdit* titleEdit = factory.createLineEdit(m_params["windowTitle"].toString());
    layout->addWidget(titleEdit);

    // 显示尺寸
    layout->addWidget(factory.createLabel(tr("显示尺寸 (0=原始):")));
    QHBoxLayout* sizeLayout = new QHBoxLayout();
    sizeLayout->setSpacing(8);
    QSpinBox* widthSpin = factory.createSpinBox(0, 4096, m_params["displayWidth"].toInt());
    QSpinBox* heightSpin = factory.createSpinBox(0, 4096, m_params["displayHeight"].toInt());
    sizeLayout->addWidget(factory.createLabel(tr("宽:")));
    sizeLayout->addWidget(widthSpin);
    sizeLayout->addWidget(factory.createLabel(tr("高:")));
    sizeLayout->addWidget(heightSpin);
    sizeLayout->addStretch();
    layout->addLayout(sizeLayout);

    // 显示直方图
    QCheckBox* histogramCheck = factory.createCheckBox(tr("显示直方图"), m_params["displayHistogram"].toBool());
    layout->addWidget(histogramCheck);

    // 显示延迟
    layout->addWidget(factory.createLabel(tr("显示延迟 (ms):")));
    QSpinBox* delaySpin = factory.createSpinBox(0, 10000, m_params["delay"].toInt());
    delaySpin->setSingleStep(100);
    layout->addWidget(delaySpin);

    layout->addStretch();

    // 使用 QPointer 避免悬垂指针
    QPointer<ShowImagePlugin> pluginPtr(this);
    connect(titleEdit, &QLineEdit::textChanged, [pluginPtr](const QString& text) {
        if (pluginPtr) pluginPtr->m_params["windowTitle"] = text;
    });

    QPointer<ShowImagePlugin> pluginPtr2(this);
    connect(widthSpin, QOverload<int>::of(&QSpinBox::valueChanged), [pluginPtr2](int value) {
        if (pluginPtr2) pluginPtr2->m_params["displayWidth"] = value;
    });

    QPointer<ShowImagePlugin> pluginPtr3(this);
    connect(heightSpin, QOverload<int>::of(&QSpinBox::valueChanged), [pluginPtr3](int value) {
        if (pluginPtr3) pluginPtr3->m_params["displayHeight"] = value;
    });

    QPointer<ShowImagePlugin> pluginPtr4(this);
    connect(histogramCheck, &QCheckBox::toggled, [pluginPtr4](bool checked) {
        if (pluginPtr4) pluginPtr4->m_params["displayHistogram"] = checked;
    });

    QPointer<ShowImagePlugin> pluginPtr5(this);
    connect(delaySpin, QOverload<int>::of(&QSpinBox::valueChanged), [pluginPtr5](int value) {
        if (pluginPtr5) pluginPtr5->m_params["delay"] = value;
    });

    return widget;
}

IModule* ShowImagePlugin::cloneImpl() const
{
    // 创建新实例
    ShowImagePlugin* clone = new ShowImagePlugin();

    // 拷贝参数和成员变量
    clone->m_params = m_params;
    clone->m_displayHistogram = m_displayHistogram;
    clone->m_displayWidth = m_displayWidth;
    clone->m_displayHeight = m_displayHeight;
    // m_windowTitle 使用默认值，m_params 由构造函数初始化
    // m_displayData 不需要拷贝，它是运行时数据

    return clone;
}

bool ShowImagePlugin::hasDisplayOutput() const
{
    return m_displayData.isValid();
}

DisplayData ShowImagePlugin::getDisplayData() const
{
    DisplayData data(m_displayData);
    data.metadata()["plugin"] = QStringLiteral("ShowImagePlugin");
    data.metadata()["windowTitle"] = m_params["windowTitle"].toString();
    return data;
}

QString ShowImagePlugin::preferredViewport() const
{
    return QString();  // Empty means any available viewport
}

} // namespace DeepLux
