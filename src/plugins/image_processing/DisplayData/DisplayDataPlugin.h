#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class DisplayDataPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit DisplayDataPlugin(QObject* parent = nullptr);
    ~DisplayDataPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.displaydata"; }
    QString name() const override { return tr("数据显示"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("在图像上显示数据信息"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    QString m_displayText;
    int m_fontSize = 20;
    int m_positionX = 10;
    int m_positionY = 30;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_displayMat;
#endif
};

} // namespace DeepLux