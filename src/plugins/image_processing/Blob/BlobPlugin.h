#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class BlobPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit BlobPlugin(QObject* parent = nullptr);
    ~BlobPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.blob"; }
    QString name() const override { return tr("Blob分析"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("检测并分析图像中的Blob区域"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    struct BlobResult {
        double centerX;
        double centerY;
        double area;
        double perimeter;
        double circularity;
    };

    enum class ThresholdType {
        Fixed,
        Otsu,
        Adaptive
    };

    std::vector<BlobResult> detectBlobs(const cv::Mat& gray, const cv::Mat& mask);
    void applyThreshold(const cv::Mat& gray, cv::Mat& mask);

    double m_minArea = 10.0;
    double m_maxArea = 10000.0;
    double m_minCircularity = 0.5;
    int m_blobCount = 0;
    ThresholdType m_thresholdType = ThresholdType::Otsu;
    int m_fixedThreshold = 127;
    int m_adaptiveBlockSize = 11;
    int m_adaptiveC = 2;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_gray;
    cv::Mat m_mask;
#endif
};

} // namespace DeepLux