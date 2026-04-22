#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class MeasureRectPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit MeasureRectPlugin(QObject* parent = nullptr);
    ~MeasureRectPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.measurerect"; }
    QString name() const override { return tr("矩形测量"); }
    QString category() const override { return "detection"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("检测并测量图像中的矩形"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool findRectangle(const cv::Mat& edges, std::vector<cv::Point>& corners);

    double m_minArea = 100.0;
    double m_maxArea = 100000.0;
    double m_threshold1 = 50.0;
    double m_threshold2 = 150.0;

    // 输出参数
    double m_resultRow1 = 0.0;
    double m_resultCol1 = 0.0;
    double m_resultRow2 = 0.0;
    double m_resultCol2 = 0.0;
    double m_resultWidth = 0.0;
    double m_resultHeight = 0.0;
    double m_resultAngle = 0.0;
    double m_resultArea = 0.0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_gray;
    cv::Mat m_edges;
#endif
};

} // namespace DeepLux