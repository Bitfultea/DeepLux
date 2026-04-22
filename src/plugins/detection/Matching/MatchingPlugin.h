#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class MatchingPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit MatchingPlugin(QObject* parent = nullptr);
    ~MatchingPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.matching"; }
    QString name() const override { return tr("模板匹配"); }
    QString category() const override { return "detection"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("在图像中查找与模板匹配的区域"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    std::vector<cv::Rect> matchTemplate(const cv::Mat& image, const cv::Mat& templ, double threshold);

    double m_matchThreshold = 0.8;
    int m_maxMatches = 10;
    int m_matchCount = 0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_template;
    cv::Mat m_resultMat;
#endif
};

} // namespace DeepLux