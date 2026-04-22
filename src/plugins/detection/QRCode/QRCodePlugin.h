#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/objdetect.hpp>
#endif

namespace DeepLux {

class QRCodePlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit QRCodePlugin(QObject* parent = nullptr);
    ~QRCodePlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.qrcode"; }
    QString name() const override { return tr("二维码检测"); }
    QString category() const override { return "detection"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("检测并解码二维码/条码"); }

    enum class CodeType {
        QR_Code,
        DataMatrix,
        Code_128,
        Code_39,
        EAN_13,
        EAN_8
    };

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;
    IModule* cloneImpl() const override;

private:
    bool decodeQRCode(const ImageData& input, ImageData& output);
    bool decodeBarCode(const ImageData& input, ImageData& output, CodeType type);

    CodeType m_codeType = CodeType::QR_Code;
    QString m_decodedData;

#ifdef DEEPLUX_HAS_OPENCV
    cv::QRCodeDetector m_qrDetector;
#endif
};

} // namespace DeepLux
