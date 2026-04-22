#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#endif

namespace DeepLux {

class ImageScriptPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit ImageScriptPlugin(QObject* parent = nullptr);
    ~ImageScriptPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.imagescript"; }
    QString name() const override { return tr("图像脚本"); }
    QString category() const override { return "image_processing"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("使用脚本语言处理图像"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool executeScript(const QString& script, const cv::Mat& input, cv::Mat& output);

    QString m_script;
    int m_scriptType = 0;  // 0: Built-in operations

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_resultMat;
#endif
};

} // namespace DeepLux