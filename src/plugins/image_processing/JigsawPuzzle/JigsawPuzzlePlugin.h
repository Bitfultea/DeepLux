#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class JigsawPuzzlePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit JigsawPuzzlePlugin(QObject* parent = nullptr);
    ~JigsawPuzzlePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.jigsawsolver"; }
    QString name() const override { return tr("拼图求解"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("将碎片图像拼接成完整图像"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool solvePuzzle(const std::vector<cv::Mat>& pieces, cv::Mat& result, int rows, int cols);

    int m_rows = 2;
    int m_cols = 2;
    double m_matchThreshold = 0.3;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_resultMat;
#endif
};

} // namespace DeepLux