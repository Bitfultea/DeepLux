#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class PerProcessingPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit PerProcessingPlugin(QObject* parent = nullptr);
    ~PerProcessingPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.perprocessing"; }
    QString name() const override { return tr("图像预处理"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("图像预处理操作：滤波、锐化、形态学操作等"); }

    enum class ProcessType {
        GaussianBlur,
        MedianBlur,
        BilateralFilter,
        Sobel,
        Laplacian,
        Canny,
        Dilate,
        Erode,
        Open,
        Close,
        Gradient
    };

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    cv::Mat applyMorphology(const cv::Mat& input, int operation);

    ProcessType m_processType = ProcessType::GaussianBlur;
    int m_kernelSize = 5;
    double m_sigmaX = 1.5;
    int m_iterations = 1;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_resultMat;
#endif
};

} // namespace DeepLux