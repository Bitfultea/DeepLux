#pragma once

#include "core/base/ModuleBase.h"

#ifdef DEEPLUX_HAS_OPENCV
#include <opencv2/opencv.hpp>
#include <opencv2/calib3d.hpp>
#endif

namespace DeepLux {

class NPointCalibrationPlugin : public ModuleBase
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.deeplux.IModule" FILE "metadata.json")
    Q_INTERFACES(DeepLux::IModule)

public:
    explicit NPointCalibrationPlugin(QObject* parent = nullptr);
    ~NPointCalibrationPlugin() override;

    QString moduleId() const override { return "com.deeplux.plugin.npointcalibration"; }
    QString name() const override { return tr("N点标定"); }
    QString category() const override { return "calibration"; }
    QString version() const override { return "1.0.0"; }
    QString author() const override { return "DeepLux Team"; }
    QString description() const override { return tr("N点标定，用于相机标定和坐标转换"); }

    bool initialize() override;
    void shutdown() override;
    QWidget* createConfigWidget() override;

protected:
    bool process(const ImageData& input, ImageData& output) override;
    bool doValidateParams(const QJsonObject& params, QString& error) const override;

private:
    enum class CalibrationType {
        Perspective,
        Affine,
        Polynomial
    };

    struct CalibPoint {
        double imageX;
        double imageY;
        double worldX;
        double worldY;
    };

    struct CalibrationResult {
        cv::Mat homography;
        cv::Mat affineMatrix;
        cv::Mat mapX;
        cv::Mat mapY;
        bool isValid = false;
        double reprojectionError = 0.0;
    };

    void clearPoints();
    bool addPoint(double imgX, double imgY, double worldX, double worldY);
    bool computeCalibration();
    cv::Point2d imageToWorld(double imgX, double imgY);
    cv::Point2d worldToImage(double worldX, double worldY);

    CalibrationType m_calibType = CalibrationType::Perspective;
    std::vector<CalibPoint> m_calibPoints;
    CalibrationResult m_calibResult;
    int m_outputWidth = 0;
    int m_outputHeight = 0;
    bool m_inverseTransform = false;

#ifdef DEEPLUX_HAS_OPENCV
    cv::Mat m_inputImage;
#endif
};

} // namespace DeepLux
