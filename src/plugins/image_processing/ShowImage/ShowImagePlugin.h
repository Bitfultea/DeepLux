#pragma once

#include "core/base/ModuleBase.h"
#include "core/display/IDisplayPort.h"
#include "core/display/DisplayData.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class ShowImagePlugin : public ModuleBase, public IDisplayPort
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit ShowImagePlugin(QObject* parent = nullptr);
    ~ShowImagePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.showimage"; }
    QString name() const override { return tr("显示图像"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("在窗口显示输入图像"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

    // IDisplayPort interface
    bool hasDisplayOutput() const override;
    DisplayData getDisplayData() const override;
    QString preferredViewport() const override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool m_displayHistogram = false;
    bool m_windowCreated = false;
    int m_displayWidth = 0;
    int m_displayHeight = 0;
    QString m_windowTitle;

    // Cached DisplayData for display output (includes ImageData + metadata)
    DisplayData m_displayData;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_displayMat;
#endif
};

} // namespace DeepLux