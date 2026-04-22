#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class MeasureLinePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit MeasureLinePlugin(QObject* parent = nullptr);
    ~MeasureLinePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.measureline"; }
    QString name() const override { return tr("线条测量"); }
    QString category() const override { return "detection"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("检测并测量图像中的线条"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool detectLines(const cv::Mat& gray, std::vector<cv::Vec4i>& lines);

    double m_minLength = 20.0;
    double m_maxLength = 1000.0;
    double m_threshold = 50.0;
    double m_minAngle = 0.0;
    double m_maxAngle = 180.0;

    // 输出参数
    double m_resultRow1 = 0.0;
    double m_resultCol1 = 0.0;
    double m_resultRow2 = 0.0;
    double m_resultCol2 = 0.0;
    double m_resultLength = 0.0;
    double m_resultAngle = 0.0;
    int m_resultLineCount = 0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_gray;
#endif
};

} // namespace DeepLux