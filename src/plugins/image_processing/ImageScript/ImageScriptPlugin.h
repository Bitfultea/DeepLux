#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class ImageScriptPlugin : public ModuleBase {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit ImageScriptPlugin(QObject* parent = nullptr);
    ~ImageScriptPlugin() override;

    QString moduleId() const override {
        return "com.deeplux.plugin.imagescript";
    }
    QString name() const override {
        return tr("图像脚本");
    }
    QString category() const override {
        return "image_processing";
    }
    QString version() const override {
        return "1.0.0";
    }
    QString author() const override {
        return "DeepLux Team";
    }
    QString description() const override {
        return tr("按类型执行内置图像操作（反转/灰度/模糊/锐化）");
    }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool executeBuiltinOperation(const cv::Mat& input, cv::Mat& output);

    int m_scriptType = 0; // 内置操作类型（0–3），持久化字段

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_resultMat;
#endif
};

} // namespace DeepLux