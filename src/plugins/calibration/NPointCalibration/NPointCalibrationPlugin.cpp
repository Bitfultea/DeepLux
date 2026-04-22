#include "NPointCalibrationPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QGroupBox>
#include <QComboBox>

namespace DeepLux {

NPointCalibrationPlugin::NPointCalibrationPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"calibrationType", "Perspective"},
        {"outputWidth", 0},
        {"outputHeight", 0},
        {"inverseTransform", false},
        {"clearPointsOnRun", true}
    };
    m_params = m_defaultParams;
}

NPointCalibrationPlugin::~NPointCalibrationPlugin()
{
}

bool NPointCalibrationPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "NPointCalibrationPlugin initialized";
    return true;
}

void NPointCalibrationPlugin::shutdown()
{
    clearPoints();
    ModuleBase::shutdown();
}

void NPointCalibrationPlugin::clearPoints()
{
    m_calibPoints.clear();
    m_calibResult.isValid = false;
}

bool NPointCalibrationPlugin::addPoint(double imgX, double imgY, double worldX, double worldY)
{
    CalibPoint point;
    point.imageX = imgX;
    point.imageY = imgY;
    point.worldX = worldX;
    point.worldY = worldY;
    m_calibPoints.push_back(point);
    return true;
}

bool NPointCalibrationPlugin::computeCalibration()
{
    if (m_calibPoints.size() < 4) {
        Logger::instance().warning("NPointCalibration: Need at least 4 points for calibration", "Calibration");
        return false;
    }

    std::vector<cv::Point2f> srcPoints, dstPoints;
    for (const auto& p : m_calibPoints) {
        srcPoints.emplace_back(p.imageX, p.imageY);
        dstPoints.emplace_back(p.worldX, p.worldY);
    }

    try {
        if (m_calibType == CalibrationType::Perspective) {
            m_calibResult.homography = cv::findHomography(srcPoints, dstPoints, cv::RANSAC);
            if (!m_calibResult.homography.empty()) {
                m_calibResult.isValid = true;
                Logger::instance().info("NPointCalibration: Perspective calibration computed successfully", "Calibration");
            }
        } else if (m_calibType == CalibrationType::Affine) {
            m_calibResult.affineMatrix = cv::estimateAffine2D(srcPoints, dstPoints);
            if (!m_calibResult.affineMatrix.empty()) {
                m_calibResult.isValid = true;
                Logger::instance().info("NPointCalibration: Affine calibration computed successfully", "Calibration");
            }
        }

        if (m_calibResult.isValid) {
            double totalError = 0.0;
            for (size_t i = 0; i < srcPoints.size(); ++i) {
                cv::Point2f reprojected = worldToImage(srcPoints[i].x, srcPoints[i].y);
                double error = cv::norm(reprojected - dstPoints[i]);
                totalError += error * error;
            }
            m_calibResult.reprojectionError = std::sqrt(totalError / srcPoints.size());
        }

    } catch (const cv::Exception& e) {
        Logger::instance().error(QString("NPointCalibration: OpenCV error - %1").arg(e.what()), "Calibration");
        return false;
    }

    return m_calibResult.isValid;
}

cv::Point2d NPointCalibrationPlugin::imageToWorld(double imgX, double imgY)
{
    if (!m_calibResult.isValid) {
        return cv::Point2d(imgX, imgY);
    }

    if (m_calibType == CalibrationType::Perspective && !m_calibResult.homography.empty()) {
        std::vector<cv::Point2f> src = {cv::Point2f(imgX, imgY)};
        std::vector<cv::Point2f> dst;
        cv::perspectiveTransform(src, dst, m_calibResult.homography);
        if (!dst.empty()) {
            return dst[0];
        }
    } else if (m_calibType == CalibrationType::Affine && !m_calibResult.affineMatrix.empty()) {
        cv::Mat src = (cv::Mat_<double>(3, 1) << imgX, imgY, 1.0);
        cv::Mat dstMat = m_calibResult.affineMatrix * src;
        return cv::Point2d(dstMat.at<double>(0), dstMat.at<double>(1));
    }

    return cv::Point2d(imgX, imgY);
}

cv::Point2d NPointCalibrationPlugin::worldToImage(double worldX, double worldY)
{
    if (!m_calibResult.isValid) {
        return cv::Point2d(worldX, worldY);
    }

    if (m_calibType == CalibrationType::Perspective && !m_calibResult.homography.empty()) {
        std::vector<cv::Point2f> src = {cv::Point2f(worldX, worldY)};
        std::vector<cv::Point2f> dst;
        cv::Mat invH = m_calibResult.homography.inv();
        cv::perspectiveTransform(src, dst, invH);
        if (!dst.empty()) {
            return dst[0];
        }
    } else if (m_calibType == CalibrationType::Affine && !m_calibResult.affineMatrix.empty()) {
        cv::Mat src = (cv::Mat_<double>(3, 1) << worldX, worldY, 1.0);
        cv::Mat invMat = m_calibResult.affineMatrix.inv();
        cv::Mat dstMat = invMat * src;
        return cv::Point2d(dstMat.at<double>(0), dstMat.at<double>(1));
    }

    return cv::Point2d(worldX, worldY);
}

bool NPointCalibrationPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

#ifdef DEEPLUX_HAS_OPENCV
    if (input.hasMat()) {
        m_inputImage = input.toMat();
    } else {
        m_inputImage = qImageToMat(input.toQImage());
    }

    if (m_inputImage.empty()) {
        emit errorOccurred(tr("输入图像无效"));
        return false;
    }

    QString calibTypeStr = m_params["calibrationType"].toString("Perspective");
    if (calibTypeStr == "Affine") {
        m_calibType = CalibrationType::Affine;
    } else {
        m_calibType = CalibrationType::Perspective;
    }

    m_inverseTransform = m_params["inverseTransform"].toBool();

    bool clearOnRun = m_params["clearPointsOnRun"].toBool(true);
    if (clearOnRun) {
        clearPoints();
    }

    int pointCount = m_params["pointCount"].toInt(0);
    for (int i = 0; i < pointCount; ++i) {
        double imgX = m_params[QString("imgX_%1").arg(i)].toDouble();
        double imgY = m_params[QString("imgY_%1").arg(i)].toDouble();
        double worldX = m_params[QString("worldX_%1").arg(i)].toDouble();
        double worldY = m_params[QString("worldY_%1").arg(i)].toDouble();
        addPoint(imgX, imgY, worldX, worldY);
    }

    if (!m_calibResult.isValid && !m_calibPoints.empty()) {
        computeCalibration();
    }

    if (m_calibResult.isValid) {
        output.setData("calibration_valid", true);
        output.setData("reprojection_error", m_calibResult.reprojectionError);
        output.setData("point_count", static_cast<int>(m_calibPoints.size()));

        if (m_inverseTransform && !m_inputImage.empty()) {
            cv::Mat result;
            if (m_calibType == CalibrationType::Perspective && !m_calibResult.homography.empty()) {
                cv::warpPerspective(m_inputImage, result, m_calibResult.homography, m_inputImage.size());
            } else if (m_calibType == CalibrationType::Affine && !m_calibResult.affineMatrix.empty()) {
                cv::warpAffine(m_inputImage, result, m_calibResult.affineMatrix, m_inputImage.size());
            }
            if (!result.empty()) {
                output.setMat(result);
            }
        }

        Logger::instance().info(QString("NPointCalibration: Applied calibration, error: %1px").arg(m_calibResult.reprojectionError), "Calibration");
        return true;
    } else {
        output.setData("calibration_valid", false);
        emit errorOccurred(tr("标定无效或未进行标定"));
        return false;
    }
#else
    Q_UNUSED(input);
    Q_UNUSED(output);
    emit errorOccurred(tr("OpenCV未启用"));
    return false;
#endif
}

bool NPointCalibrationPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* NPointCalibrationPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    // 标定类型选择
    QGroupBox* typeGroup = new QGroupBox(tr("标定类型"));
    QVBoxLayout* typeLayout = new QVBoxLayout(typeGroup);
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem(tr("透视变换"), "Perspective");
    typeCombo->addItem(tr("仿射变换"), "Affine");
    QString currentType = m_params["calibrationType"].toString("Perspective");
    int typeIndex = typeCombo->findData(currentType);
    if (typeIndex >= 0) typeCombo->setCurrentIndex(typeIndex);
    typeLayout->addWidget(new QLabel(tr("变换类型:")));
    typeLayout->addWidget(typeCombo);

    QCheckBox* inverseCheck = new QCheckBox(tr("逆变换 (图像->世界坐标)"));
    inverseCheck->setChecked(m_params["inverseTransform"].toBool());
    typeLayout->addWidget(inverseCheck);
    typeGroup->setLayout(typeLayout);
    layout->addWidget(typeGroup);

    // 标定点数显示
    QGroupBox* pointsGroup = new QGroupBox(tr("标定点信息"));
    QVBoxLayout* pointsLayout = new QVBoxLayout(pointsGroup);
    QLabel* pointCountLabel = new QLabel(tr("当前点数: %1 (需要至少4点)").arg(m_calibPoints.size()));
    pointsLayout->addWidget(pointCountLabel);
    QLabel* statusLabel = new QLabel(m_calibResult.isValid ? tr("状态: 已标定") : tr("状态: 未标定"));
    pointsLayout->addWidget(statusLabel);
    if (m_calibResult.isValid) {
        QLabel* errorLabel = new QLabel(tr("重投影误差: %1 px").arg(m_calibResult.reprojectionError));
        pointsLayout->addWidget(errorLabel);
    }
    pointsGroup->setLayout(pointsLayout);
    layout->addWidget(pointsGroup);

    // 清除按钮
    QPushButton* clearBtn = new QPushButton(tr("清除所有标定点"));
    layout->addWidget(clearBtn);

    layout->addStretch();

    // 信号连接
    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [=](int index) {
        m_params["calibrationType"] = typeCombo->itemData(index).toString();
    });

    connect(inverseCheck, &QCheckBox::toggled, this, [=](bool checked) {
        m_params["inverseTransform"] = checked;
    });

    connect(clearBtn, &QPushButton::clicked, this, [=]() {
        clearPoints();
        pointCountLabel->setText(tr("当前点数: 0 (需要至少4点)"));
        statusLabel->setText(tr("状态: 未标定"));
        Logger::instance().info("NPointCalibration: Points cleared", "Calibration");
    });

    return widget;
}

} // namespace DeepLux
