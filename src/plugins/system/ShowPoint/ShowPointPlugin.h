#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class ShowPointPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit ShowPointPlugin(QObject* parent = nullptr);
    ~ShowPointPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.showpoint"; }
    QString name() const override { return tr("显示点"); }
    QString category() const override { return "system"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("在图像上显示点坐标"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    int m_markerSize = 5;
    int m_markerColorR = 0;
    int m_markerColorG = 255;
    int m_markerColorB = 0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_resultMat;
#endif
};

} // namespace DeepLux