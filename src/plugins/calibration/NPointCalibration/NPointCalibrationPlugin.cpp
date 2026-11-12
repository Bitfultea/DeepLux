#include "NPointCalibrationPlugin.h"

#include "core/common/Logger.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>
#include <cmath>
#include <optional>

namespace DeepLux {

namespace {

std::optional<ImageCoordinateProvenance::Matrix3> inverseImageWarp(bool perspective, const cv::Mat& homography,
                                                                   const cv::Mat& affineMatrix) {
    cv::Mat forward = cv::Mat::eye(3, 3, CV_64F);
    if (perspective) {
        if (homography.empty())
            return std::nullopt;
        homography.convertTo(forward, CV_64F);
    } else {
        if (affineMatrix.rows != 2 || affineMatrix.cols != 3)
            return std::nullopt;
        cv::Mat affine64;
        affineMatrix.convertTo(affine64, CV_64F);
        for (int row = 0; row < 2; ++row)
            for (int column = 0; column < 3; ++column)
                forward.at<double>(row, column) = affine64.at<double>(row, column);
    }

    cv::Mat inverse;
    if (!cv::checkRange(forward) || std::abs(cv::determinant(forward)) <= 1e-12 ||
        cv::invert(forward, inverse, cv::DECOMP_LU) == 0.0 || !cv::checkRange(inverse))
        return std::nullopt;

    ImageCoordinateProvenance::Matrix3 result{};
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            result[row * 3 + column] = inverse.at<double>(row, column);
    return result;
}

} // namespace

NPointCalibrationPlugin::NPointCalibrationPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{
        {"calibrationType", "Perspective"}, {"outputWidth", 0},         {"outputHeight", 0},
        {"inverseTransform", false},        {"clearPointsOnRun", true}, {"calibrationPoints", QJsonArray{}}};
    m_params = m_defaultParams;
}

NPointCalibrationPlugin::~NPointCalibrationPlugin() {}

bool NPointCalibrationPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "NPointCalibrationPlugin initialized";
    return true;
}

void NPointCalibrationPlugin::shutdown() {
    clearPoints();
    ModuleBase::shutdown();
}

void NPointCalibrationPlugin::clearPoints() {
    m_calibPoints.clear();
    m_calibResult.isValid = false;
}

bool NPointCalibrationPlugin::addPoint(double imgX, double imgY, double worldX, double worldY) {
    CalibPoint point;
    point.imageX = imgX;
    point.imageY = imgY;
    point.worldX = worldX;
    point.worldY = worldY;
    m_calibPoints.push_back(point);
    return true;
}

bool NPointCalibrationPlugin::computeCalibration() {
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
            if (!m_calibResult.homography.empty() && cv::checkRange(m_calibResult.homography) &&
                std::abs(cv::determinant(m_calibResult.homography)) > 1e-12) {
                m_calibResult.isValid = true;
                Logger::instance().info("NPointCalibration: Perspective calibration computed successfully",
                                        "Calibration");
            }
        } else if (m_calibType == CalibrationType::Affine) {
            m_calibResult.affineMatrix = cv::estimateAffine2D(srcPoints, dstPoints);
            if (!m_calibResult.affineMatrix.empty() && cv::checkRange(m_calibResult.affineMatrix) &&
                std::abs(cv::determinant(m_calibResult.affineMatrix(cv::Rect(0, 0, 2, 2)))) > 1e-12) {
                m_calibResult.isValid = true;
                Logger::instance().info("NPointCalibration: Affine calibration computed successfully", "Calibration");
            }
        }

        if (m_calibResult.isValid) {
            double totalError = 0.0;
            for (size_t i = 0; i < srcPoints.size(); ++i) {
                cv::Point2f reprojected = imageToWorld(srcPoints[i].x, srcPoints[i].y);
                double error = cv::norm(reprojected - dstPoints[i]);
                totalError += error * error;
            }
            m_calibResult.reprojectionError = std::sqrt(totalError / srcPoints.size());
            m_calibResult.isValid = std::isfinite(m_calibResult.reprojectionError);
        }

    } catch (const cv::Exception& e) {
        Logger::instance().error(QString("NPointCalibration: OpenCV error - %1").arg(e.what()), "Calibration");
        return false;
    }

    return m_calibResult.isValid;
}

cv::Point2d NPointCalibrationPlugin::imageToWorld(double imgX, double imgY) {
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

cv::Point2d NPointCalibrationPlugin::worldToImage(double worldX, double worldY) {
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

bool NPointCalibrationPlugin::process(const ImageData& input, ImageData& output) {
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

    QJsonObject params = currentParams();
    QString validationError;
    if (!doValidateParams(params, validationError)) {
        emit errorOccurred(validationError);
        return false;
    }
    QString calibTypeStr = params["calibrationType"].toString("Perspective");
    if (calibTypeStr == "Affine") {
        m_calibType = CalibrationType::Affine;
    } else {
        m_calibType = CalibrationType::Perspective;
    }

    m_inverseTransform = params["inverseTransform"].toBool();
    m_outputWidth = params["outputWidth"].toInt(0);
    m_outputHeight = params["outputHeight"].toInt(0);

    bool clearOnRun = params["clearPointsOnRun"].toBool(true);
    const QJsonArray configuredPoints = params.value(QStringLiteral("calibrationPoints")).toArray();
    if (clearOnRun || !configuredPoints.isEmpty()) {
        clearPoints();
    }

    for (const QJsonValue& pointValue : configuredPoints) {
        const QJsonObject point = pointValue.toObject();
        const QJsonArray image = point.value(QStringLiteral("image")).toArray();
        const QJsonArray world = point.value(QStringLiteral("world")).toArray();
        addPoint(image[0].toDouble(), image[1].toDouble(), world[0].toDouble(), world[1].toDouble());
    }

    if (!m_calibResult.isValid && !m_calibPoints.empty()) {
        computeCalibration();
    }

    if (m_calibResult.isValid) {
        output.setData("calibration_valid", true);
        output.setData("reprojection_error", m_calibResult.reprojectionError);
        output.setData("point_count", static_cast<int>(m_calibPoints.size()));

        if (m_inverseTransform && !m_inputImage.empty()) {
            cv::Size targetSize = (m_outputWidth > 0 && m_outputHeight > 0) ? cv::Size(m_outputWidth, m_outputHeight)
                                                                            : m_inputImage.size();
            cv::Mat result;
            if (m_calibType == CalibrationType::Perspective && !m_calibResult.homography.empty()) {
                cv::warpPerspective(m_inputImage, result, m_calibResult.homography, targetSize);
            } else if (m_calibType == CalibrationType::Affine && !m_calibResult.affineMatrix.empty()) {
                cv::warpAffine(m_inputImage, result, m_calibResult.affineMatrix, targetSize);
            }
            if (!result.empty()) {
                output.setMat(result);
                if (input.hasCoordinateProvenance()) {
                    const std::optional<ImageCoordinateProvenance::Matrix3> outputToInput =
                        inverseImageWarp(m_calibType == CalibrationType::Perspective, m_calibResult.homography,
                                         m_calibResult.affineMatrix);
                    ImageCoordinateProvenance provenance = input.coordinateProvenance();
                    if (!outputToInput || !provenance.composeLocalToSource(*outputToInput))
                        output.invalidateCoordinateProvenance();
                    else
                        output.setCoordinateProvenance(provenance);
                } else {
                    output.invalidateCoordinateProvenance();
                }
            }
        }

        Logger::instance().info(QString("NPointCalibration: Applied calibration, world reprojection error: %1")
                                    .arg(m_calibResult.reprojectionError),
                                "Calibration");
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

bool NPointCalibrationPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();
    const QString type = params.value(QStringLiteral("calibrationType")).toString();
    if (type != QStringLiteral("Perspective") && type != QStringLiteral("Affine")) {
        error = tr("calibrationType 必须为 Perspective 或 Affine");
        return false;
    }
    for (const QString& key : {QStringLiteral("outputWidth"), QStringLiteral("outputHeight")}) {
        const QJsonValue value = params.value(key);
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::floor(value.toDouble()) != value.toDouble() ||
            value.toDouble() < 0 || value.toDouble() > 16384) {
            error = tr("%1 必须为 [0,16384] 整数").arg(key);
            return false;
        }
    }
    if (!params.value(QStringLiteral("inverseTransform")).isBool() ||
        !params.value(QStringLiteral("clearPointsOnRun")).isBool()) {
        error = tr("inverseTransform 和 clearPointsOnRun 必须为布尔值");
        return false;
    }
    const QJsonValue pointsValue = params.value(QStringLiteral("calibrationPoints"));
    if (!pointsValue.isArray()) {
        error = tr("calibrationPoints 必须为点对数组");
        return false;
    }
    const QJsonArray points = pointsValue.toArray();
    if (points.size() > 64 || (!points.isEmpty() && points.size() < 4)) {
        error = tr("calibrationPoints 必须为空或包含 4..64 个点对");
        return false;
    }
    for (const QJsonValue& pointValue : points) {
        if (!pointValue.isObject()) {
            error = tr("calibrationPoints 每项必须包含 image/world 点");
            return false;
        }
        const QJsonObject point = pointValue.toObject();
        for (const QString& key : {QStringLiteral("image"), QStringLiteral("world")}) {
            const QJsonValue coordinates = point.value(key);
            if (!coordinates.isArray() || coordinates.toArray().size() != 2) {
                error = tr("%1 坐标必须是 [x,y]").arg(key);
                return false;
            }
            for (const QJsonValue& value : coordinates.toArray()) {
                if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 1e7) {
                    error = tr("%1 坐标必须是有限数且绝对值不超过 1e7").arg(key);
                    return false;
                }
            }
        }
    }
    for (const QString& key : {QStringLiteral("image"), QStringLiteral("world")}) {
        bool nonCollinear = false;
        const auto separated = [&points, &key](int first, int second) {
            const QJsonArray a = points[first].toObject().value(key).toArray();
            const QJsonArray b = points[second].toObject().value(key).toArray();
            return std::hypot(a[0].toDouble() - b[0].toDouble(), a[1].toDouble() - b[1].toDouble()) > 1e-8;
        };
        const auto nonCollinearTriple = [&points, &key](int first, int second, int third) {
            const QJsonArray a = points[first].toObject().value(key).toArray();
            const QJsonArray b = points[second].toObject().value(key).toArray();
            const QJsonArray c = points[third].toObject().value(key).toArray();
            const double dx1 = b[0].toDouble() - a[0].toDouble();
            const double dy1 = b[1].toDouble() - a[1].toDouble();
            const double dx2 = c[0].toDouble() - a[0].toDouble();
            const double dy2 = c[1].toDouble() - a[1].toDouble();
            return std::abs(dx1 * dy2 - dy1 * dx2) > 1e-8 * std::hypot(dx1, dy1) * std::hypot(dx2, dy2);
        };
        for (int i = 0; i < points.size(); ++i) {
            for (int j = i + 1; j < points.size(); ++j) {
                if (!separated(i, j)) {
                    error = tr("%1 标定点存在重复坐标").arg(key);
                    return false;
                }
            }
        }
        for (int i = 1; i < points.size() && !nonCollinear; ++i) {
            for (int j = i + 1; j < points.size(); ++j) {
                if (nonCollinearTriple(0, i, j)) {
                    nonCollinear = true;
                    break;
                }
            }
        }
        if (!points.isEmpty() && !nonCollinear) {
            error = tr("%1 标定点全部共线，无法计算变换").arg(key);
            return false;
        }
        if (type == QStringLiteral("Perspective") && points.size() == 4) {
            for (int first = 0; first < 4; ++first) {
                int other[3], next = 0;
                for (int i = 0; i < 4; ++i) {
                    if (i != first)
                        other[next++] = i;
                }
                if (!nonCollinearTriple(other[0], other[1], other[2])) {
                    error = tr("%1 透视标定的任意三点不可共线").arg(key);
                    return false;
                }
            }
        }
    }
    return true;
}

QWidget* NPointCalibrationPlugin::createConfigWidget() {
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
    if (typeIndex >= 0)
        typeCombo->setCurrentIndex(typeIndex);
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
    const QJsonArray configuredPoints = m_params.value(QStringLiteral("calibrationPoints")).toArray();
    QLabel* pointCountLabel = new QLabel(tr("当前点数: %1 (需要至少4点)").arg(configuredPoints.size()));
    pointsLayout->addWidget(pointCountLabel);
    QTableWidget* pointsTable = new QTableWidget(widget);
    pointsTable->setObjectName(QStringLiteral("CalibrationPointsTable"));
    pointsTable->setColumnCount(4);
    pointsTable->setHorizontalHeaderLabels({tr("图像 X"), tr("图像 Y"), tr("世界 X"), tr("世界 Y")});
    pointsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    pointsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    {
        QSignalBlocker blocker(pointsTable);
        pointsTable->setRowCount(configuredPoints.size());
        for (int row = 0; row < configuredPoints.size(); ++row) {
            const QJsonObject pair = configuredPoints[row].toObject();
            const QJsonArray image = pair.value(QStringLiteral("image")).toArray();
            const QJsonArray world = pair.value(QStringLiteral("world")).toArray();
            for (int column = 0; column < 4; ++column) {
                const QJsonArray coordinates = column < 2 ? image : world;
                const int index = column < 2 ? column : column - 2;
                const QJsonValue value = index < coordinates.size() ? coordinates.at(index) : QJsonValue();
                pointsTable->setItem(row, column, new QTableWidgetItem(QString::number(value.toDouble(), 'g', 12)));
            }
        }
    }
    pointsLayout->addWidget(pointsTable);
    QPushButton* addPointButton = new QPushButton(tr("添加点对"), widget);
    QPushButton* removePointButton = new QPushButton(tr("删除选中点对"), widget);
    QHBoxLayout* pointButtons = new QHBoxLayout();
    pointButtons->addWidget(addPointButton);
    pointButtons->addWidget(removePointButton);
    pointsLayout->addLayout(pointButtons);
    QLabel* statusLabel = new QLabel(m_calibResult.isValid ? tr("状态: 已标定") : tr("状态: 未标定"));
    pointsLayout->addWidget(statusLabel);
    if (m_calibResult.isValid) {
        QLabel* errorLabel = new QLabel(tr("重投影误差: %1（世界坐标单位）").arg(m_calibResult.reprojectionError));
        pointsLayout->addWidget(errorLabel);
    }
    pointsGroup->setLayout(pointsLayout);
    layout->addWidget(pointsGroup);

    // 清除按钮
    QPushButton* clearBtn = new QPushButton(tr("清除所有标定点"));
    layout->addWidget(clearBtn);

    layout->addStretch();

    // 信号连接
    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [=](int index) { setParam("calibrationType", typeCombo->itemData(index).toString()); });

    connect(inverseCheck, &QCheckBox::toggled, this, [=](bool checked) { setParam("inverseTransform", checked); });

    const auto savePointRows = [=]() {
        QJsonArray pairs;
        for (int row = 0; row < pointsTable->rowCount(); ++row) {
            QJsonArray coordinates;
            for (int column = 0; column < 4; ++column) {
                const QTableWidgetItem* item = pointsTable->item(row, column);
                bool ok = false;
                const double number = item ? item->text().toDouble(&ok) : 0.0;
                coordinates.append(ok ? QJsonValue(number) : QJsonValue(QStringLiteral("")));
            }
            pairs.append(QJsonObject{{"image", QJsonArray{coordinates[0], coordinates[1]}},
                                     {"world", QJsonArray{coordinates[2], coordinates[3]}}});
        }
        setParam(QStringLiteral("calibrationPoints"), pairs);
        pointCountLabel->setText(tr("当前点数: %1 (需要至少4点)").arg(pairs.size()));
    };
    connect(pointsTable, &QTableWidget::cellChanged, this, [=](int, int) { savePointRows(); });
    connect(addPointButton, &QPushButton::clicked, this, [=]() {
        pointsTable->insertRow(pointsTable->rowCount());
        savePointRows();
    });
    connect(removePointButton, &QPushButton::clicked, this, [=]() {
        const int row = pointsTable->currentRow();
        if (row >= 0) {
            pointsTable->removeRow(row);
            savePointRows();
        }
    });

    connect(clearBtn, &QPushButton::clicked, this, [=]() {
        // 阶段 1: 不直接 clearPoints()，改为设置标记，确定后由主程序执行
        setParam("_clearPointsRequested", true);
        pointsTable->setRowCount(0);
        setParam(QStringLiteral("calibrationPoints"), QJsonArray{});
        pointCountLabel->setText(tr("当前点数: 0 (需要至少4点)"));
        statusLabel->setText(tr("状态: 未标定 (将在确定后清除)"));
        clearBtn->setEnabled(false);
        clearBtn->setText(tr("已请求清除 (确定后生效)"));
    });

    return widget;
}

IModule* NPointCalibrationPlugin::cloneImpl() const {
    return new NPointCalibrationPlugin();
}

} // namespace DeepLux
