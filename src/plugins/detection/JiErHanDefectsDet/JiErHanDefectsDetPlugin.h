#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class JiErHanDefectsDetPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit JiErHanDefectsDetPlugin(QObject* parent = nullptr);
    ~JiErHanDefectsDetPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.jierhandefectsdet"; }
    QString name() const override { return tr("剑二韩缺陷检测"); }
    QString category() const override { return "detection"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("剑二韩焊接缺陷检测"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    struct DefectResult {
        QString type;
        double confidence;
        cv::Rect boundingBox;
    };

    std::vector<DefectResult> detectDefects(const cv::Mat& image);

    double m_threshold = 0.5;
    int m_defectCount = 0;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_resultMat;
#endif
};

} // namespace DeepLux