#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineF>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPolygonF>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtMath>
#include <QtTest/QtTest>
#include <algorithm>
#include <cmath>
#include <core/agent/AgentActor.h>
#include <core/agent/ToolSchema.h>
#include <core/base/ModuleBase.h>
#include <core/common/UserManager.h>
#include <core/deeplux/DataContract.h>
#include <core/display/DisplayData.h>
#include <core/engine/RunEngine.h>
#include <core/manager/PluginManager.h>
#include <core/manager/ProjectManager.h>
#include <core/model/ImageData.h>
#include <core/model/Project.h>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

using namespace DeepLux;

class LineOutputFixtureModule final : public ModuleBase {
public:
    LineOutputFixtureModule(QVariantList line, ImageCoordinateProvenance provenance)
        : m_line(std::move(line)), m_provenance(std::move(provenance)) {
        m_moduleId = QStringLiteral("MeasureLine");
        m_name = QStringLiteral("MeasureLine fixture");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("measure"));
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(16, 16, QImage::Format_RGB888));
        output.setCoordinateProvenance(m_provenance);
        output.setData(QStringLiteral("line"), m_line);
        output.setData(QStringLiteral("line_compat"), m_line);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariantList m_line;
    ImageCoordinateProvenance m_provenance;
};

class UnprovenancedImageFixtureModule final : public ModuleBase {
public:
    explicit UnprovenancedImageFixtureModule(QImage image) : m_image(std::move(image)) {
        m_moduleId = QStringLiteral("GrabImage");
        m_name = QStringLiteral("Unprovenanced image fixture");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("source"));
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output.setQImage(m_image);
        output.invalidateCoordinateProvenance();
        return !m_image.isNull();
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QImage m_image;
};

class ImageWithProvenanceFixtureModule final : public ModuleBase {
public:
    ImageWithProvenanceFixtureModule(QImage image, ImageCoordinateProvenance provenance, QVariant point = {})
        : m_image(std::move(image)), m_provenance(std::move(provenance)), m_point(std::move(point)) {
        m_moduleId = QStringLiteral("GrabImage");
        m_name = QStringLiteral("Provenanced image fixture");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("grab"));
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output.setQImage(m_image);
        output.setCoordinateProvenance(m_provenance);
        if (m_point.isValid())
            output.setData(QStringLiteral("point"), m_point);
        return !m_image.isNull();
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QImage m_image;
    ImageCoordinateProvenance m_provenance;
    QVariant m_point;
};

class NonFiniteColorCenterFixtureModule final : public ModuleBase {
public:
    NonFiniteColorCenterFixtureModule() {
        m_moduleId = QStringLiteral("ColorRecognition");
        m_name = QStringLiteral("Non-finite ColorRecognition fixture");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.displayName = QStringLiteral("Image");
        imagePort.type = DataType::Image2D;
        PortSpec centerPort;
        centerPort.id = QStringLiteral("color_center");
        centerPort.displayName = QStringLiteral("Center");
        centerPort.type = DataType::Point2D;
        setPorts({imagePort}, {imagePort, centerPort});
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("color_center"), QPointF(std::numeric_limits<double>::quiet_NaN(), 50.0));
        return input.isValid();
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class MeasurementInputPointPairOutputFixtureModule final : public ModuleBase {
public:
    explicit MeasurementInputPointPairOutputFixtureModule(QVariant point1) : m_point1(std::move(point1)) {
        m_moduleId = QStringLiteral("MeasurementInput");
        m_name = QStringLiteral("MeasurementInput point-pair fixture");
        setInstanceName(QStringLiteral("input"));
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.displayName = QStringLiteral("Output");
        imagePort.type = DataType::Any;
        PortSpec point1Port;
        point1Port.id = QStringLiteral("point1");
        point1Port.displayName = QStringLiteral("Point 1");
        point1Port.type = DataType::Any;
        PortSpec point2Port = point1Port;
        point2Port.id = QStringLiteral("point2");
        point2Port.displayName = QStringLiteral("Point 2");
        point2Port.type = DataType::Point2D;
        setPorts({}, {imagePort, point1Port, point2Port});
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("measurement_input_mode"), QStringLiteral("point_pair"));
        output.setData(QStringLiteral("point1"), m_point1);
        output.setData(QStringLiteral("point2"), QVariantList{7.0, 8.0});
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariant m_point1;
};

class NonTargetPointPairWorld3DSpoofFixtureModule final : public ModuleBase {
public:
    NonTargetPointPairWorld3DSpoofFixtureModule() {
        m_moduleId = QStringLiteral("PointPairWorld3DSpoofFixture");
        m_name = QStringLiteral("Non-target point-pair World3D spoof fixture");
        setInstanceName(QStringLiteral("spoof"));
        PortSpec pointPort;
        pointPort.id = QStringLiteral("point1");
        pointPort.displayName = QStringLiteral("Point 1");
        pointPort.type = DataType::Any;
        PortSpec point2Port = pointPort;
        point2Port.id = QStringLiteral("point2");
        setPorts({}, {pointPort, point2Port});
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("measurement_input_mode"), QStringLiteral("point_pair"));
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("World3D"));
        output.setData(QStringLiteral("point1"), QVariantList{16777217.125, 12.5, -3.75});
        output.setData(QStringLiteral("point2"), QVariantList{16777220.125, 16.5, -0.75});
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class NonTargetPointPairImageLocalSpoofFixtureModule final : public ModuleBase {
public:
    NonTargetPointPairImageLocalSpoofFixtureModule() {
        m_moduleId = QStringLiteral("PointPairImageLocalSpoofFixture");
        m_name = QStringLiteral("Non-target point-pair ImageLocalPixel spoof fixture");
        PortSpec point;
        point.id = QStringLiteral("point1");
        point.type = DataType::Any;
        PortSpec point2 = point;
        point2.id = QStringLiteral("point2");
        setPorts({}, {point, point2});
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("measurement_input_mode"), QStringLiteral("point_pair"));
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("ImageLocalPixel"));
        output.setData(QStringLiteral("point1"), QVariantList{1.0, 2.0});
        output.setData(QStringLiteral("point2"), QVariantList{4.0, 6.0});
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class MeasureGapScalarMismatchFixtureModule final : public ModuleBase {
public:
    MeasureGapScalarMismatchFixtureModule() {
        m_moduleId = QStringLiteral("MeasureGap");
        m_name = QStringLiteral("MeasureGap scalar mismatch fixture");
        PortSpec point;
        point.id = QStringLiteral("point1");
        point.type = DataType::Any;
        PortSpec point2 = point;
        point2.id = QStringLiteral("point2");
        QList<PortSpec> outputs{point, point2};
        PortSpec number;
        number.type = DataType::Number;
        for (const QString& id : {QStringLiteral("gap"), QStringLiteral("gap_distance"), QStringLiteral("gap_delta_x"),
                                  QStringLiteral("gap_delta_y"), QStringLiteral("gap_delta_z")}) {
            PortSpec scalar = number;
            scalar.id = id;
            outputs.append(scalar);
        }
        PortSpec dimension;
        dimension.id = QStringLiteral("measurement_dimension");
        dimension.type = DataType::String;
        outputs.append(dimension);
        setPorts({point, point2}, outputs);
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("measurement_input_mode"), QStringLiteral("point_pair"));
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("ImageLocalPixel"));
        output.setData(QStringLiteral("measurement_dimension"), QStringLiteral("2d"));
        output.setData(QStringLiteral("gap"), 50.0);
        output.setData(QStringLiteral("gap_distance"), 51.0);
        output.setData(QStringLiteral("gap_delta_x"), 52.0);
        output.setData(QStringLiteral("gap_delta_y"), 53.0);
        output.setData(QStringLiteral("gap_delta_z"), 0.0);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class EdgeDefectMeasurementInputFixtureModule final : public ModuleBase {
public:
    EdgeDefectMeasurementInputFixtureModule(QString mode, QString coordinateFrame)
        : m_mode(std::move(mode)), m_coordinateFrame(std::move(coordinateFrame)) {
        m_moduleId = QStringLiteral("MeasurementInput");
        m_name = QStringLiteral("EdgeDefect MeasurementInput fixture");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.type = DataType::Any;
        PortSpec fitPointsPort;
        fitPointsPort.id = QStringLiteral("fit_points");
        fitPointsPort.type = DataType::PointSet2D;
        setPorts({}, {imagePort, fitPointsPort});
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("measurement_input_mode"), m_mode);
        output.setData(QStringLiteral("coordinate_frame"), m_coordinateFrame);
        QVector<QPointF> points;
        for (int i = 0; i < 72; ++i) {
            const double angle = qDegreesToRadians(i * 5.0);
            points.append(QPointF(150.0 + 95.0 * std::cos(angle), 150.0 + 95.0 * std::sin(angle)));
        }
        output.setData(QStringLiteral("fit_points"), QVariant::fromValue(points));
        return input.isValid();
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QString m_mode;
    QString m_coordinateFrame;
};

class LinePairSourceFixtureModule final : public ModuleBase {
public:
    LinePairSourceFixtureModule(QString moduleId, QVariantList line1, QVariantList line2,
                                ImageCoordinateProvenance provenance,
                                QString coordinateFrame = QStringLiteral("ImageLocalPixel"),
                                QString mode = QStringLiteral("line_pair"))
        : m_line1(std::move(line1)), m_line2(std::move(line2)), m_provenance(std::move(provenance)),
          m_coordinateFrame(std::move(coordinateFrame)), m_mode(std::move(mode)) {
        m_moduleId = std::move(moduleId);
        m_name = QStringLiteral("Line pair source fixture");
        m_category = QStringLiteral("test");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.type = DataType::Any;
        PortSpec line1Port;
        line1Port.id = QStringLiteral("line1");
        line1Port.type = DataType::Line2D;
        PortSpec line2Port = line1Port;
        line2Port.id = QStringLiteral("line2");
        setPorts({}, {imagePort, line1Port, line2Port});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(32, 24, QImage::Format_RGB32));
        output.setCoordinateProvenance(m_provenance);
        output.setData(QStringLiteral("coordinate_frame"), m_coordinateFrame);
        output.setData(QStringLiteral("measurement_input_mode"), m_mode);
        output.setData(QStringLiteral("line1"), m_line1);
        output.setData(QStringLiteral("line2"), m_line2);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariantList m_line1;
    QVariantList m_line2;
    ImageCoordinateProvenance m_provenance;
    QString m_coordinateFrame;
    QString m_mode;
};

class LinesDistanceSegmentOutputFixtureModule final : public ModuleBase {
public:
    explicit LinesDistanceSegmentOutputFixtureModule(QVariant value) : m_value(std::move(value)) {
        m_moduleId = QStringLiteral("LinesDistance");
        m_name = QStringLiteral("LinesDistance segment output fixture");
        PortSpec image;
        image.id = QStringLiteral("image");
        image.type = DataType::Any;
        PortSpec line1;
        line1.id = QStringLiteral("line1");
        line1.type = DataType::Line2D;
        PortSpec line2 = line1;
        line2.id = QStringLiteral("line2");
        PortSpec segment;
        segment.id = QStringLiteral("nearest_segment");
        // Let malformed runtime values reach AgentActor, which validates the real plugin metadata as Line2D.
        segment.type = DataType::Any;
        setPorts({image, line1, line2}, {segment});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(1, 1, QImage::Format_RGB32));
        output.setData(QStringLiteral("nearest_segment"), m_value);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariant m_value;
};

class LinesDistanceNearestScalarMismatchFixtureModule final : public ModuleBase {
public:
    LinesDistanceNearestScalarMismatchFixtureModule() {
        m_moduleId = QStringLiteral("LinesDistance");
        m_name = QStringLiteral("LinesDistance nearest scalar mismatch fixture");
        PortSpec input;
        input.id = QStringLiteral("image");
        input.type = DataType::Any;
        PortSpec line1 = input;
        line1.id = QStringLiteral("line1");
        line1.type = DataType::Line2D;
        PortSpec line2 = line1;
        line2.id = QStringLiteral("line2");
        PortSpec nearest;
        nearest.id = QStringLiteral("nearest_segment");
        nearest.type = DataType::Line2D;
        QList<PortSpec> outputs{nearest};
        PortSpec number;
        number.type = DataType::Number;
        for (const QString& id : {QStringLiteral("nearest_x1"), QStringLiteral("nearest_y1"),
                                  QStringLiteral("nearest_x2"), QStringLiteral("nearest_y2")}) {
            PortSpec scalar = number;
            scalar.id = id;
            outputs.append(scalar);
        }
        setPorts({input, line1, line2}, outputs);
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("measurement_input_mode"), QStringLiteral("line_pair"));
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("ImageLocalPixel"));
        output.setData(QStringLiteral("nearest_segment"), QVariantList{1.0, 2.0, 3.0, 4.0});
        output.setData(QStringLiteral("nearest_x1"), 10.0);
        output.setData(QStringLiteral("nearest_y1"), 20.0);
        output.setData(QStringLiteral("nearest_x2"), 30.0);
        output.setData(QStringLiteral("nearest_y2"), 40.0);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class NonFiniteDistancePLFootPointFixtureModule final : public ModuleBase {
public:
    NonFiniteDistancePLFootPointFixtureModule() {
        m_moduleId = QStringLiteral("DistancePL");
        m_name = QStringLiteral("DistancePL non-finite foot fixture");
        setInstanceName(QStringLiteral("distance"));
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.type = DataType::Any;
        PortSpec footPointPort;
        footPointPort.id = QStringLiteral("foot_point");
        footPointPort.type = DataType::Point2D;
        setPorts({}, {imagePort, footPointPort});
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("ImageLocalPixel"));
        output.setData(QStringLiteral("foot_point"), QPointF(std::numeric_limits<double>::quiet_NaN(), 30.0));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class Point2DOutputFixtureModule final : public ModuleBase {
public:
    Point2DOutputFixtureModule() {
        m_moduleId = QStringLiteral("Point2DFixture");
        m_name = QStringLiteral("Generic Point2D fixture");
        PortSpec pointPort;
        pointPort.id = QStringLiteral("point");
        pointPort.displayName = QStringLiteral("Point");
        pointPort.type = DataType::Point2D;
        PortSpec point1Port = pointPort;
        point1Port.id = QStringLiteral("point1");
        point1Port.displayName = QStringLiteral("Point 1");
        PortSpec footPointPort = pointPort;
        footPointPort.id = QStringLiteral("foot_point");
        footPointPort.displayName = QStringLiteral("Foot point");
        setPorts({}, {pointPort, point1Port, footPointPort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(32, 24, QImage::Format_RGB32));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        provenance.sourceSize = QSize(32, 24);
        provenance.localOriginInSource = QPointF(10, 20);
        output.setCoordinateProvenance(provenance);
        output.setData(QStringLiteral("point"), QPointF(3.0, 4.0));
        output.setData(QStringLiteral("point1"), QPointF(3.0, 4.0));
        output.setData(QStringLiteral("foot_point"), QPointF(3.0, 4.0));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class PointSetOutputFixtureModule final : public ModuleBase {
public:
    PointSetOutputFixtureModule(QString moduleId, QString coordinateFrame, bool nonFinitePoint = false)
        : m_coordinateFrame(std::move(coordinateFrame)), m_nonFinitePoint(nonFinitePoint) {
        m_moduleId = std::move(moduleId);
        m_name = QStringLiteral("Point set output fixture");
        m_category = QStringLiteral("test");
        setInstanceName(QStringLiteral("fixture"));
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.displayName = QStringLiteral("Output");
        imagePort.type = DataType::Any;
        PortSpec pointSetPort;
        pointSetPort.id = QStringLiteral("fit_points");
        pointSetPort.displayName = QStringLiteral("Points");
        pointSetPort.type = DataType::PointSet2D;
        setPorts({}, {imagePort, pointSetPort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(32, 24, QImage::Format_RGB32));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        provenance.sourceSize = QSize(32, 24);
        provenance.localOriginInSource = QPointF(10, 20);
        output.setCoordinateProvenance(provenance);
        output.setData(QStringLiteral("coordinate_frame"), m_coordinateFrame);
        QVector<QPointF> points{QPointF(1, 2), QPointF(3, 4)};
        if (m_nonFinitePoint)
            points[1].setX(std::numeric_limits<double>::quiet_NaN());
        output.setData(QStringLiteral("fit_points"), QVariant::fromValue(points));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QString m_coordinateFrame;
    bool m_nonFinitePoint;
};

class CircleInputFixtureModule final : public ModuleBase {
public:
    explicit CircleInputFixtureModule(QString coordinateFrame, bool setCoordinateFrame = true,
                                      bool insufficientPoints = false)
        : m_coordinateFrame(std::move(coordinateFrame)), m_setCoordinateFrame(setCoordinateFrame),
          m_insufficientPoints(insufficientPoints) {
        m_moduleId = QStringLiteral("MeasurementInput");
        m_name = QStringLiteral("Circle input fixture");
        m_category = QStringLiteral("test");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.displayName = QStringLiteral("Output");
        imagePort.type = DataType::Any;
        PortSpec pointsPort;
        pointsPort.id = QStringLiteral("fit_points");
        pointsPort.displayName = QStringLiteral("Points");
        pointsPort.type = DataType::PointSet2D;
        PortSpec circlePort;
        circlePort.id = QStringLiteral("circle");
        circlePort.displayName = QStringLiteral("Sentinel circle");
        circlePort.type = DataType::Circle2D;
        setPorts({}, {imagePort, pointsPort, circlePort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(160, 120, QImage::Format_RGB32));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        provenance.sourceSize = QSize(160, 120);
        provenance.localOriginInSource = QPointF(10, 20);
        output.setCoordinateProvenance(provenance);
        if (m_setCoordinateFrame)
            output.setData(QStringLiteral("coordinate_frame"), m_coordinateFrame);
        QVector<QPointF> points{QPointF(37, 25), QPointF(33.4852813742, 33.4852813742),
                                QPointF(25, 37), QPointF(16.5147186258, 33.4852813742),
                                QPointF(13, 25), QPointF(16.5147186258, 16.5147186258),
                                QPointF(25, 13), QPointF(33.4852813742, 16.5147186258)};
        if (m_insufficientPoints)
            points = {QPointF(1, 1), QPointF(2, 2)};
        output.setData(QStringLiteral("fit_points"), QVariant::fromValue(points));
        Circle2D sentinel;
        sentinel.centerX = 999.0;
        sentinel.centerY = -777.0;
        sentinel.radius = 555.0;
        output.setData(QStringLiteral("circle"), QVariant::fromValue(sentinel));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QString m_coordinateFrame;
    bool m_setCoordinateFrame;
    bool m_insufficientPoints;
};

class FitCircleScalarOutputWithoutCircleFixtureModule final : public ModuleBase {
public:
    FitCircleScalarOutputWithoutCircleFixtureModule() {
        m_moduleId = QStringLiteral("FitCircle");
        m_name = QStringLiteral("FitCircle scalar output without typed circle");
        const auto numberPort = [](const QString& id) {
            PortSpec port;
            port.id = id;
            port.displayName = id;
            port.type = DataType::Number;
            return port;
        };
        setPorts({}, {numberPort(QStringLiteral("circle_center_x")), numberPort(QStringLiteral("circle_center_y")),
                      numberPort(QStringLiteral("circle_radius")), numberPort(QStringLiteral("circle_error"))});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(16, 16, QImage::Format_RGB32));
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("ImageLocalPixel"));
        output.setData(QStringLiteral("circle_center_x"), 25.0);
        output.setData(QStringLiteral("circle_center_y"), 26.0);
        output.setData(QStringLiteral("circle_radius"), 12.0);
        output.setData(QStringLiteral("circle_error"), 0.125);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class FitLineInputFixtureModule final : public ModuleBase {
public:
    FitLineInputFixtureModule(QString coordinateFrame, bool setCoordinateFrame = true, bool insufficientPoints = false,
                              bool crossesHorizon = false)
        : m_coordinateFrame(std::move(coordinateFrame)), m_setCoordinateFrame(setCoordinateFrame),
          m_insufficientPoints(insufficientPoints), m_crossesHorizon(crossesHorizon) {
        m_moduleId = QStringLiteral("MeasurementInput");
        m_name = QStringLiteral("FitLine input fixture");
        m_category = QStringLiteral("test");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.displayName = QStringLiteral("Output");
        imagePort.type = DataType::Any;
        PortSpec pointsPort;
        pointsPort.id = QStringLiteral("fit_points");
        pointsPort.displayName = QStringLiteral("Points");
        pointsPort.type = DataType::PointSet2D;
        PortSpec linePort;
        linePort.id = QStringLiteral("line");
        linePort.displayName = QStringLiteral("Sentinel line");
        linePort.type = DataType::Line2D;
        setPorts({}, {imagePort, pointsPort, linePort});
    }

    void setInsufficientPoints(bool insufficient) {
        m_insufficientPoints = insufficient;
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(160, 120, QImage::Format_RGB32));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        provenance.sourceSize = QSize(400, 300);
        provenance.localOriginInSource = QPointF(10, 20);
        if (m_crossesHorizon) {
            provenance.hasLocalToSourceTransform = true;
            provenance.localToSource = ImageCoordinateProvenance::Matrix3{0, 0, 0, 0, 1, 0, 0.02, 0, 1};
        }
        output.setCoordinateProvenance(provenance);
        if (m_setCoordinateFrame)
            output.setData(QStringLiteral("coordinate_frame"), m_coordinateFrame);
        QVector<QPointF> points{QPointF(13, 25), QPointF(17, 25), QPointF(21, 25), QPointF(25, 25),
                                QPointF(29, 25), QPointF(33, 25), QPointF(37, 25)};
        if (m_insufficientPoints)
            points = {QPointF(13, 25)};
        output.setData(QStringLiteral("fit_points"), QVariant::fromValue(points));
        output.setData(QStringLiteral("line"), QVariantList{999.0, 999.0, -999.0, -999.0});
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QString m_coordinateFrame;
    bool m_setCoordinateFrame;
    bool m_insufficientPoints;
    bool m_crossesHorizon;
};

class FitLineEndpointMismatchFixtureModule final : public ModuleBase {
public:
    FitLineEndpointMismatchFixtureModule() {
        m_moduleId = QStringLiteral("FitLine");
        m_name = QStringLiteral("FitLine endpoint mismatch fixture");
        PortSpec image;
        image.id = QStringLiteral("image");
        image.type = DataType::Any;
        PortSpec points;
        points.id = QStringLiteral("fit_points");
        points.type = DataType::PointSet2D;
        PortSpec line;
        line.id = QStringLiteral("line");
        line.type = DataType::Line2D;
        PortSpec number;
        number.type = DataType::Number;
        QList<PortSpec> outputs;
        for (const QString& id : {QStringLiteral("line_row1"), QStringLiteral("line_col1"), QStringLiteral("line_row2"),
                                  QStringLiteral("line_col2")}) {
            PortSpec scalar = number;
            scalar.id = id;
            outputs.append(scalar);
        }
        outputs.append(line);
        setPorts({image, points}, outputs);
    }

protected:
    bool process(const ImageData& input, ImageData& output) override {
        output = input;
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("ImageLocalPixel"));
        output.setData(QStringLiteral("line"), QVariantList{1.0, 2.0, 3.0, 4.0});
        output.setData(QStringLiteral("line_row1"), 20.0);
        output.setData(QStringLiteral("line_col1"), 10.0);
        output.setData(QStringLiteral("line_row2"), 40.0);
        output.setData(QStringLiteral("line_col2"), 30.0);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class FitEllipseInputFixtureModule final : public ModuleBase {
public:
    FitEllipseInputFixtureModule(QString coordinateFrame, bool setCoordinateFrame = true,
                                 bool insufficientPoints = false, bool invalidCenterMapping = false)
        : m_coordinateFrame(std::move(coordinateFrame)), m_setCoordinateFrame(setCoordinateFrame),
          m_insufficientPoints(insufficientPoints), m_invalidCenterMapping(invalidCenterMapping) {
        m_moduleId = QStringLiteral("MeasurementInput");
        m_name = QStringLiteral("FitEllipse input fixture");
        m_category = QStringLiteral("test");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.displayName = QStringLiteral("Output");
        imagePort.type = DataType::Any;
        PortSpec pointsPort;
        pointsPort.id = QStringLiteral("fit_points");
        pointsPort.displayName = QStringLiteral("Points");
        pointsPort.type = DataType::PointSet2D;
        PortSpec ellipsePort;
        ellipsePort.id = QStringLiteral("ellipse");
        ellipsePort.displayName = QStringLiteral("Sentinel ellipse");
        ellipsePort.type = DataType::Ellipse2D;
        setPorts({}, {imagePort, pointsPort, ellipsePort});
    }

    void setInsufficientPoints(bool insufficient) {
        m_insufficientPoints = insufficient;
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(160, 120, QImage::Format_RGB32));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        provenance.sourceSize = QSize(400, 300);
        provenance.localOriginInSource = QPointF(10, 20);
        if (m_invalidCenterMapping) {
            provenance.hasLocalToSourceTransform = true;
            provenance.localToSource = ImageCoordinateProvenance::Matrix3{1, 0, 0, 0, 1, 0, -0.04, 0, 1};
        }
        output.setCoordinateProvenance(provenance);
        if (m_setCoordinateFrame)
            output.setData(QStringLiteral("coordinate_frame"), m_coordinateFrame);
        QVector<QPointF> points{QPointF(37, 25), QPointF(33.4852813742, 29.9497474683),
                                QPointF(25, 32), QPointF(16.5147186258, 29.9497474683),
                                QPointF(13, 25), QPointF(16.5147186258, 20.0502525317),
                                QPointF(25, 18), QPointF(33.4852813742, 20.0502525317)};
        if (m_insufficientPoints)
            points = {QPointF(13, 25), QPointF(37, 25)};
        output.setData(QStringLiteral("fit_points"), QVariant::fromValue(points));
        Ellipse2D sentinel{999.0, -777.0, 5.0, 2.0, 45.0};
        output.setData(QStringLiteral("ellipse"), QVariant::fromValue(sentinel));
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QString m_coordinateFrame;
    bool m_setCoordinateFrame;
    bool m_insufficientPoints;
    bool m_invalidCenterMapping;
};

class FitPlaneHeightMapFixtureModule final : public ModuleBase {
public:
    FitPlaneHeightMapFixtureModule() {
        m_moduleId = QStringLiteral("test.HeightMapFixture");
        m_name = QStringLiteral("FitPlane height map fixture");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.type = DataType::HeightMap2D;
        setPorts({}, {imagePort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        QImage height(8, 6, QImage::Format_Grayscale8);
        for (int y = 0; y < height.height(); ++y) {
            uchar* row = height.scanLine(y);
            for (int x = 0; x < height.width(); ++x)
                row[x] = static_cast<uchar>(3 * x + 4 * y + 10);
        }
        output.setQImage(height);
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        provenance.sourceSize = QSize(300, 200);
        provenance.hasLocalToSourceTransform = true;
        provenance.localToSource = ImageCoordinateProvenance::Matrix3{1, 0, 71, 0, 1, 43, 0, 0, 1};
        output.setCoordinateProvenance(provenance);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }
};

class FitPlaneOutputFixtureModule final : public ModuleBase {
public:
    explicit FitPlaneOutputFixtureModule(QVariantList plane, QString moduleId = QStringLiteral("FitPlane"))
        : m_plane(std::move(plane)) {
        m_moduleId = std::move(moduleId);
        m_name = QStringLiteral("Plane output fixture");
        PortSpec planePort;
        planePort.id = QStringLiteral("plane");
        planePort.type = DataType::Plane3D;
        setPorts({}, {planePort});
    }

    ExecutionResult execute(const PortValueMap&, PortValueMap& outputs, ExecutionContext&) override {
        outputs.insert(QStringLiteral("plane"), m_plane);
        return ExecutionResult::ok();
    }

protected:
    bool process(const ImageData&, ImageData&) override {
        return false;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariantList m_plane;
};

class MeasurementInput3DOutputFixtureModule final : public ModuleBase {
public:
    MeasurementInput3DOutputFixtureModule(QVariant point, QVariant plane)
        : m_point(std::move(point)), m_plane(std::move(plane)) {
        m_moduleId = QStringLiteral("MeasurementInput");
        m_name = QStringLiteral("MeasurementInput 3D output fixture");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.type = DataType::Any;
        PortSpec pointPort;
        pointPort.id = QStringLiteral("point");
        pointPort.type = DataType::Any;
        PortSpec planePort;
        planePort.id = QStringLiteral("plane");
        planePort.type = DataType::Any;
        setPorts({}, {imagePort, pointPort, planePort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(60, 40, QImage::Format_RGB32));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        provenance.sourceSize = QSize(160, 120);
        provenance.hasLocalToSourceTransform = true;
        provenance.localToSource = ImageCoordinateProvenance::Matrix3{1, 0, 50, 0, 1, 40, 0, 0, 1};
        output.setCoordinateProvenance(provenance);
        output.setData(QStringLiteral("measurement_input_mode"), QStringLiteral("point_plane"));
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("World3D"));
        output.setData(QStringLiteral("point"), m_point);
        output.setData(QStringLiteral("plane"), m_plane);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariant m_point;
    QVariant m_plane;
};

class PointSurfaceDistanceInputFixtureModule final : public ModuleBase {
public:
    PointSurfaceDistanceInputFixtureModule(QString frame, bool setFrame)
        : m_frame(std::move(frame)), m_setFrame(setFrame) {
        m_moduleId = QStringLiteral("MeasurementInput");
        m_name = QStringLiteral("PointSurfaceDistance input fixture");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.type = DataType::Any;
        PortSpec pointPort;
        pointPort.id = QStringLiteral("point");
        pointPort.type = DataType::Any;
        PortSpec planePort;
        planePort.id = QStringLiteral("plane");
        planePort.type = DataType::Any;
        setPorts({}, {imagePort, pointPort, planePort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(60, 40, QImage::Format_RGB32));
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        provenance.sourceSize = QSize(160, 120);
        provenance.hasLocalToSourceTransform = true;
        provenance.localToSource = ImageCoordinateProvenance::Matrix3{1, 0, 50, 0, 1, 40, 0, 0, 1};
        output.setCoordinateProvenance(provenance);
        output.setData(QStringLiteral("measurement_input_mode"), QStringLiteral("point_plane"));
        if (m_setFrame)
            output.setData(QStringLiteral("coordinate_frame"), m_frame);
        output.setData(QStringLiteral("point"), QVariantList{1.0, 2.0, 10.0});
        output.setData(QStringLiteral("plane"), QVariantList{0.0, 0.0, 3.0, 1.0, 0.0, 3.0, 0.0, 1.0, 3.0});
        output.setData(QStringLiteral("foot_point"), QVariantList{901.0, 902.0, 903.0});
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QString m_frame;
    bool m_setFrame;
};

class PointSurfaceDistanceOutputFixtureModule final : public ModuleBase {
public:
    explicit PointSurfaceDistanceOutputFixtureModule(QVariant footPoint, QVariant footX = QVariant(1.0))
        : m_footPoint(std::move(footPoint)), m_footX(std::move(footX)) {
        m_moduleId = QStringLiteral("PointSurfaceDistance");
        m_name = QStringLiteral("PointSurfaceDistance output fixture");
        PortSpec imagePort;
        imagePort.id = QStringLiteral("image");
        imagePort.type = DataType::Any;
        PortSpec distancePort;
        distancePort.id = QStringLiteral("distance");
        distancePort.type = DataType::Number;
        PortSpec footPort;
        footPort.id = QStringLiteral("foot_point");
        footPort.type = DataType::Any;
        setPorts({}, {imagePort, distancePort, footPort});
    }

protected:
    bool process(const ImageData&, ImageData& output) override {
        output = ImageData(QImage(60, 40, QImage::Format_RGB32));
        output.setData(QStringLiteral("coordinate_frame"), QStringLiteral("World3D"));
        output.setData(QStringLiteral("distance"), 7.0);
        output.setData(QStringLiteral("foot_x"), m_footX);
        output.setData(QStringLiteral("foot_y"), 2.0);
        output.setData(QStringLiteral("foot_z"), 3.0);
        output.setData(QStringLiteral("foot_point"), m_footPoint);
        return true;
    }

    QWidget* createConfigWidget() override {
        return nullptr;
    }

private:
    QVariant m_footPoint;
    QVariant m_footX;
};

/// 阶段 0.1 固定验收工程测试。
///
/// 设计约束：
/// - 测试数据固定落在 tests/acceptance/data/，不依赖用户目录临时文件。
/// - 验收工程固定落在 tests/acceptance/projects/，图像路径用 @ACCEPTANCE_DATA@ 占位，
///   测试运行时替换为真实数据目录，保证工程文件可移植。
/// - 预期结果与允许误差落在 tests/acceptance/expected/*.json。
class TestAcceptanceFlows : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();
    void testFindCircleFlow();
    void testAgentFindCircleMapsCircleThroughCrop();
    void testAgentEdgeDefectDeviationUnitsRequireImageLocalPointSetSource();
    void testPointToPointDistanceFlow();
    void testMeasurementInputPointPairRunsMeasureGap();
    void testAgentMeasureGapPixelUnitsFailClosed();
    void testPointCloudPointToPlaneDistanceFlow();
    void testAgentFreeformSurfaceResultsRetainUnknownUnits();
    void testFitLineFlow();
    void testAgentReadsFixedAlgorithmResults();
    void testAgentSkippedBranchIsUnavailable();
    void testAgentRunFlowPreservesIfBranchSemantics();
    void testAgentRunFlowPreservesDisabledImageBypass();
    void testAgentBlobLargestDetectionMapsThroughCropOrigin();
    void testAgentColorRecognitionCenterMapsThroughCropOrigin();
    void testAgentJiErHanDefectRegionMapsThroughCropOrigin();
    void testAgentBlobCenterWithoutProvenance();
    void testAgentMatchingScalarsDeclareLocalPixelUnits();
    void testAgentShowPointOutputsDeclareCurrentImagePixelUnits();
    void testAgentMeasureCircleMapsCenterThroughCropOrigin();
    void testAgentMeasureLineMapsEndpointsThroughCropOrigin();
    void testAgentMeasurementInputPointSetCarriesSourceCoordinates();
    void testAgentMeasurementInputPointSetFailsClosedForMissingAndHorizonSources();
    void testAgentMeasurementInputPointSetSourcePageRespectsResponseLimit();
    void testAgentMeasurementInputPointPairMapsPointsThroughCrop();
    void testAgentMeasurementInputCustomPoint1DoesNotCarryCoordinateSemantics();
    void testAgentMeasurementInputPointPairFailsClosedForMissingAndHorizonSources();
    void testAgentMeasurementInputPointPairThreeCoordinateCompatibility();
    void testAgentMeasurementInputPointPairRejectsMalformedOutputs();
    void testAgentMeasurementInputLineOutputsRespectCoordinateFrame();
    void testLegacyMarkerlessLineMeasurementsStayUnspecifiedAfterProjectRoundTrip();
    void testLegacyMarkerlessLineMeasurementsRemainUnspecifiedAfterMigration();
    void testAgentLinesDistanceNearestSegmentMapsThroughCrop();
    void testAgentLinesDistanceSourceGateAndHorizon();
    void testAgentLinesDistanceNearestScalarUnitsRequireMatchingSegment();
    void testAgentLinesDistanceRejectsMalformedNearestSegment();
    void testAgentDistancePLFootPointMapsThroughCrop();
    void testAgentDistancePLFootPointFailsClosedForMissingAndHorizonSources();
    void testAgentDistancePLFootPointRemainsRawOutsideImageLocalFrame();
    void testAgentDistancePLRejectsNonFiniteFootPoint();
    void testAgentPointSurfaceDistanceFootPointMapsThroughCrop();
    void testAgentPointSurfaceDistanceClearsFootPointOutsideWorldFrame();
    void testAgentPointSurfaceDistanceRejectsMalformedFootPoint();
    void testAgentPointSurfaceDistanceMalformedFootScalarHasNoFrameTag();
    void testAgentPointSetTypeMetadataDoesNotImplyCoordinateSemantics();
    void testFitCircleTypedOutputUsesImageFrameAndClearsPassedCircle();
    void testFitCircleRejectsPassedCircleWithoutImageFrame();
    void testAgentFitCircleScalarUnitsRequireTypedCircleOutput();
    void testFitCircleClearsPassedCircleBeforeFitFailure();
    void testFitLineTypedOutputAndSourceCoordinates();
    void testFitLineEndpointScalarUnitsRequireMatchingLineOutput();
    void testFitLineClearsPassedOutputAcrossFramesAndFailure();
    void testFitLineOmitsSourceCoordinatesAcrossHorizon();
    void testFitEllipseTypedOutputAndSourceCenter();
    void testFitEllipseClearsPassedOutputAcrossFramesAndFailure();
    void testFitEllipseKeepsLocalOutputWhenSourceCenterCannotMap();
    void testAgentFitPlaneUsesHeightMapScaledCoordinates();
    void testAgentFitPlaneRejectsMalformedPlanePortValues();
    void testAgentMeasurementInputPointPlanePortsCarry3DSemantics();
    void testAgentMeasurementInputPointPlaneRejectsMalformedOutputs();
    void testAgentLine2DSerializationBoundaries();
    void testAgentMeasureRectMapsRotatedVerticesAndDoesNotLeakFailedRunOutput();
    void testAgentMeasureRectWithoutProvenanceRetainsLocalPolygon();
    void testAgentRunFlowPreservesLoopCount();
    void testAgentRunFlowPreservesParallelAllJoin();
    void testAgentRunFlowPreservesParallelFailurePropagation();
    // 阶7: 控制流 GUI 验收工程（条件分支执行顺序）
    void testControlFlowIfBranchFlow();

    // 阶段 4: 循环/条件循环/提前退出 流程验收
    void testLoopFixedCountFlow();
    void testLoopFiftyRunsNoCrossFramePollution();
    void testWhileConditionExitFlow();
    void testStopWhileEarlyExitFlow();
    void testStopCancelsLongLoopWithinDeadline();

    // 阶段 4: Parallel all/any/失败分支/blocking 不并行 流程验收
    void testParallelAllJoinFlow();
    void testParallelAnyJoinFlow();
    void testParallelFailureCancelsSiblingFlow();
    void testParallelBlockingBranchesNotParallelizedFlow();

    // 阶段 4: 交互式拾取点集 → 圆拟合 真实工作流
    void testFitCircleFromPickSessionFlow();

private:
    QTemporaryDir m_tempDir;
    QString m_acceptanceRoot; // tests/acceptance 绝对路径

    bool installPlugin(const QString& dirName, const QString& metadataRel, const QString& libName);
    QString loadProjectWithPlaceholder(const QString& projectRel, const QString& dataDir, Project& out);
};

void TestAcceptanceFlows::initTestCase() {
    UserManager::instance().login(QStringLiteral("developer"), QString());
    QVERIFY(m_tempDir.isValid());
    qputenv("DEEPLUX_APP_DATA_DIR", m_tempDir.filePath("appdata").toLocal8Bit());

    // build/bin -> 仓库根/tests/acceptance
    m_acceptanceRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../../tests/acceptance");
    QVERIFY2(QFileInfo::exists(m_acceptanceRoot), qPrintable("missing acceptance root: " + m_acceptanceRoot));

    PluginManager::instance().shutdown();
    PluginManager::instance().addPluginPath(m_tempDir.filePath("plugins"));

    QVERIFY2(
        installPlugin("GrabImage", "src/plugins/image_processing/GrabImage/metadata.json", "libGrabImagePlugin.so"),
        "install GrabImage");
    QVERIFY2(installPlugin("ShowPoint", "src/plugins/system/ShowPoint/metadata.json", "libShowPointPlugin.so"),
             "install ShowPoint");
    QVERIFY2(installPlugin("EdgeDefectDetection", "src/plugins/detection/EdgeDefectDetection/metadata.json",
                           "libEdgeDefectDetectionPlugin.so"),
             "install EdgeDefectDetection");
    QVERIFY2(installPlugin("FindCircle", "src/plugins/detection/FindCircle/metadata.json", "libFindCirclePlugin.so"),
             "install FindCircle");
    QVERIFY2(installPlugin("MeasurementInput", "src/plugins/geometry/MeasurementInput/metadata.json",
                           "libMeasurementInputPlugin.so"),
             "install MeasurementInput");
    QVERIFY2(installPlugin("MeasureGap", "src/plugins/geometry/MeasureGap/metadata.json", "libMeasureGapPlugin.so"),
             "install MeasureGap");
    QVERIFY2(
        installPlugin("LinesDistance", "src/plugins/geometry/LinesDistance/metadata.json", "libLinesDistancePlugin.so"),
        "install LinesDistance");
    QVERIFY2(installPlugin("DistancePP", "src/plugins/geometry/DistancePP/metadata.json", "libDistancePPPlugin.so"),
             "install DistancePP");
    QVERIFY2(installPlugin("DistancePL", "src/plugins/geometry/DistancePL/metadata.json", "libDistancePLPlugin.so"),
             "install DistancePL");
    QVERIFY2(installPlugin("LoadPointCloud", "src/plugins/image_processing/LoadPointCloud/metadata.json",
                           "libLoadPointCloudPlugin.so"),
             "install LoadPointCloud");
    QVERIFY2(installPlugin("FreeformSurface", "src/plugins/geometry/FreeformSurface/metadata.json",
                           "libFreeformSurfacePlugin.so"),
             "install FreeformSurface");
    QVERIFY2(installPlugin("PointSurfaceDistance", "src/plugins/geometry/PointSurfaceDistance/metadata.json",
                           "libPointSurfaceDistancePlugin.so"),
             "install PointSurfaceDistance");
    QVERIFY2(installPlugin("FitLine", "src/plugins/geometry/FitLine/metadata.json", "libFitLinePlugin.so"),
             "install FitLine");
    QVERIFY2(installPlugin("FitEllipse", "src/plugins/geometry/FitEllipse/metadata.json", "libFitEllipsePlugin.so"),
             "install FitEllipse");
    QVERIFY2(installPlugin("FitPlane", "src/plugins/geometry/FitPlane/metadata.json", "libFitPlanePlugin.so"),
             "install FitPlane");
    QVERIFY2(installPlugin("Blob", "src/plugins/image_processing/Blob/metadata.json", "libBlobPlugin.so"),
             "install Blob");
    QVERIFY2(installPlugin("MeasureCircle", "src/plugins/detection/MeasureCircle/metadata.json",
                           "libMeasureCirclePlugin.so"),
             "install MeasureCircle");
    QVERIFY2(installPlugin("MeasureLine", "src/plugins/detection/MeasureLine/metadata.json", "libMeasureLinePlugin.so"),
             "install MeasureLine");
    QVERIFY2(installPlugin("MeasureRect", "src/plugins/detection/MeasureRect/metadata.json", "libMeasureRectPlugin.so"),
             "install MeasureRect");
    QVERIFY2(installPlugin("Matching", "src/plugins/detection/Matching/metadata.json", "libMatchingPlugin.so"),
             "install Matching");
    QVERIFY2(installPlugin("ColorRecognition", "src/plugins/detection/ColorRecognition/metadata.json",
                           "libColorRecognitionPlugin.so"),
             "install ColorRecognition");
    QVERIFY2(installPlugin("JiErHanDefectsDet", "src/plugins/detection/JiErHanDefectsDet/metadata.json",
                           "libJiErHanDefectsDetPlugin.so"),
             "install JiErHanDefectsDet");
    QVERIFY2(
        installPlugin("CropImage", "src/plugins/image_processing/CropImage/metadata.json", "libCropImagePlugin.so"),
        "install CropImage");
    QVERIFY2(installPlugin("If", "src/plugins/logic/If/metadata.json", "libIfPlugin.so"), "install If");
    QVERIFY2(installPlugin("Delay", "src/plugins/logic/Delay/metadata.json", "libDelayPlugin.so"), "install Delay");
    // 阶段 4: 循环/条件循环/停止循环/并行/数学/保存/圆拟合
    QVERIFY2(installPlugin("Loop", "src/plugins/logic/Loop/metadata.json", "libLoopPlugin.so"), "install Loop");
    QVERIFY2(installPlugin("While", "src/plugins/logic/While/metadata.json", "libWhilePlugin.so"), "install While");
    QVERIFY2(installPlugin("StopWhile", "src/plugins/logic/StopWhile/metadata.json", "libStopWhilePlugin.so"),
             "install StopWhile");
    QVERIFY2(installPlugin("Parallel", "src/plugins/logic/Parallel/metadata.json", "libParallelPlugin.so"),
             "install Parallel");
    QVERIFY2(installPlugin("MathPlugin", "src/plugins/variable/MathPlugin/metadata.json", "libMathPlugin.so"),
             "install Math");
    QVERIFY2(installPlugin("SaveData", "src/plugins/system/SaveData/metadata.json", "libSaveDataPlugin.so"),
             "install SaveData");
    QVERIFY2(installPlugin("FitCircle", "src/plugins/geometry/FitCircle/metadata.json", "libFitCirclePlugin.so"),
             "install FitCircle");

    QVERIFY(PluginManager::instance().initialize());
    QVERIFY(PluginManager::instance().loadPlugin("GrabImage"));
    QVERIFY(PluginManager::instance().loadPlugin("ShowPoint"));
    QVERIFY(PluginManager::instance().loadPlugin("EdgeDefectDetection"));
    QVERIFY(PluginManager::instance().loadPlugin("FindCircle"));
    QVERIFY(PluginManager::instance().loadPlugin("MeasurementInput"));
    QVERIFY(PluginManager::instance().loadPlugin("MeasureGap"));
    QVERIFY(PluginManager::instance().loadPlugin("LinesDistance"));
    QVERIFY(PluginManager::instance().loadPlugin("DistancePP"));
    QVERIFY(PluginManager::instance().loadPlugin("DistancePL"));
    QVERIFY(PluginManager::instance().loadPlugin("LoadPointCloud"));
    QVERIFY(PluginManager::instance().loadPlugin("FreeformSurface"));
    QVERIFY(PluginManager::instance().loadPlugin("PointSurfaceDistance"));
    QVERIFY(PluginManager::instance().loadPlugin("FitLine"));
    QVERIFY(PluginManager::instance().loadPlugin("FitPlane"));
    QVERIFY(PluginManager::instance().loadPlugin("Blob"));
    QVERIFY(PluginManager::instance().loadPlugin("MeasureCircle"));
    QVERIFY(PluginManager::instance().loadPlugin("MeasureLine"));
    QVERIFY(PluginManager::instance().loadPlugin("MeasureRect"));
    QVERIFY(PluginManager::instance().loadPlugin("Matching"));
    QVERIFY(PluginManager::instance().loadPlugin("ColorRecognition"));
    QVERIFY(PluginManager::instance().loadPlugin("JiErHanDefectsDet"));
    QVERIFY(PluginManager::instance().loadPlugin("CropImage"));
    QVERIFY(PluginManager::instance().loadPlugin("FitCircle"));
    QVERIFY(PluginManager::instance().loadPlugin("SaveData"));
    QVERIFY(PluginManager::instance().loadPlugin("Parallel"));
    // 插件索引键为 metadata name（中文）
    QVERIFY(PluginManager::instance().loadPlugin("条件分支"));
    QVERIFY(PluginManager::instance().loadPlugin("延时"));
    QVERIFY(PluginManager::instance().loadPlugin("循环"));
    QVERIFY(PluginManager::instance().loadPlugin("条件循环"));
    QVERIFY(PluginManager::instance().loadPlugin("停止循环"));
    QVERIFY(PluginManager::instance().loadPlugin("数学运算"));
}

void TestAcceptanceFlows::cleanup() {
    RunEngine::instance().stop();
    RunEngine::instance().clearModules();
    RunEngine::instance().clearOutputs();
    ProjectManager::instance().closeProject();
}

bool TestAcceptanceFlows::installPlugin(const QString& dirName, const QString& metadataRel, const QString& libName) {
    const QString repoRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    const QString pluginDir = m_tempDir.filePath("plugins/" + dirName);
    if (!QDir().mkpath(pluginDir))
        return false;

    const QString metaSrc = QDir(repoRoot).filePath(metadataRel);
    const QString libSrc = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../lib/" + libName);
    if (!QFileInfo::exists(metaSrc) || !QFileInfo::exists(libSrc))
        return false;

    QFile::remove(pluginDir + "/metadata.json");
    QFile::remove(pluginDir + "/" + libName);
    return QFile::copy(metaSrc, pluginDir + "/metadata.json") && QFile::copy(libSrc, pluginDir + "/" + libName);
}

QString TestAcceptanceFlows::loadProjectWithPlaceholder(const QString& projectRel, const QString& dataDir,
                                                        Project& out) {
    QFile f(QDir(m_acceptanceRoot).filePath(projectRel));
    if (!f.open(QIODevice::ReadOnly))
        return QString("cannot open project: %1").arg(f.fileName());
    QString text = QString::fromUtf8(f.readAll());
    f.close();
    text.replace(QStringLiteral("@ACCEPTANCE_DATA@"), dataDir);

    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return QString("invalid project json: %1").arg(err.errorString());
    out.fromJson(doc.object());
    return QString();
}

void TestAcceptanceFlows::testFindCircleFlow() {
    const QString dataDir = QDir(m_acceptanceRoot).filePath("data");

    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_findcircle.json", dataDir, project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject into RunEngine failed");

    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("findcircle"));
    QVERIFY2(out.isValid(), "FindCircle produced no output");

    // 读取预期结果与允许误差
    QFile ef(QDir(m_acceptanceRoot).filePath("expected/circle_640x480.json"));
    QVERIFY(ef.open(QIODevice::ReadOnly));
    QJsonObject expected = QJsonDocument::fromJson(ef.readAll()).object();
    ef.close();

    const double expCx = expected["circle_center_x"].toDouble();
    const double expCy = expected["circle_center_y"].toDouble();
    const double expR = expected["circle_radius"].toDouble();
    const double tolC = expected["tolerance_center_px"].toDouble();
    const double tolR = expected["tolerance_radius_px"].toDouble();

    const double gotCx = out.data("circle_center_x").toDouble();
    const double gotCy = out.data("circle_center_y").toDouble();
    const double gotR = out.data("circle_radius").toDouble();

    QVERIFY2(std::abs(gotCx - expCx) <= tolC,
             qPrintable(QString("circle_center_x off: got %1 want %2 ±%3").arg(gotCx).arg(expCx).arg(tolC)));
    QVERIFY2(std::abs(gotCy - expCy) <= tolC,
             qPrintable(QString("circle_center_y off: got %1 want %2 ±%3").arg(gotCy).arg(expCy).arg(tolC)));
    QVERIFY2(std::abs(gotR - expR) <= tolR,
             qPrintable(QString("circle_radius off: got %1 want %2 ±%3").arg(gotR).arg(expR).arg(tolR)));

    // 步4: 强类型 Circle2D 输出应存在且与标量值一致
    QVERIFY2(out.data("circle").canConvert<Circle2D>(), "FindCircle must emit typed Circle2D");
    const Circle2D circle = out.data("circle").value<Circle2D>();
    QVERIFY(circle.isValid());
    QVERIFY2(std::abs(circle.centerX - expCx) <= tolC, "Circle2D.centerX mismatch");
    QVERIFY2(std::abs(circle.radius - expR) <= tolR, "Circle2D.radius mismatch");
}

void TestAcceptanceFlows::testAgentFindCircleMapsCircleThroughCrop() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    const QString dataDir = QDir(m_acceptanceRoot).filePath(QStringLiteral("data"));
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_findcircle.json", dataDir, *project).isEmpty(),
             "load FindCircle fixed-image flow");

    ModuleInstance crop;
    crop.id = QStringLiteral("crop");
    crop.moduleId = QStringLiteral("CropImage");
    crop.name = QStringLiteral("nonzero-origin crop");
    crop.params = QJsonObject{{QStringLiteral("rectangles"), QJsonArray{QJsonArray{320, 240, 150, 150, 0}}},
                              {QStringLiteral("outputFirstAsImage"), true}};
    project->addModule(crop);
    project->removeConnection(QStringLiteral("grab"), QStringLiteral("findcircle"));
    for (const auto& edge : {qMakePair(QStringLiteral("grab"), QStringLiteral("crop")),
                             qMakePair(QStringLiteral("crop"), QStringLiteral("findcircle"))}) {
        ModuleConnection connection;
        connection.fromModuleId = edge.first;
        connection.fromPort = QStringLiteral("image");
        connection.toModuleId = edge.second;
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{QStringLiteral("grab"), crop.id, QStringLiteral("findcircle")};
    project->setFlows({flow});

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData grabOutput = engine.moduleOutput(QStringLiteral("grab"));
    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    const ImageData output = engine.moduleOutput(QStringLiteral("findcircle"));
    QCOMPARE(cropOutput.width(), 300);
    QCOMPARE(cropOutput.height(), 300);
    const ImageCoordinateProvenance provenance = cropOutput.coordinateProvenance();
    QVERIFY(provenance.isValid());
    QCOMPARE(provenance.localOriginInSource, QPointF(170, 90));
    const ImageCoordinateProvenance outputProvenance = output.coordinateProvenance();
    QVERIFY(outputProvenance.isValid());
    QCOMPARE(output.width(), cropOutput.width());
    QCOMPARE(output.height(), cropOutput.height());
    QCOMPARE(outputProvenance.sourceImageId, provenance.sourceImageId);
    QCOMPARE(outputProvenance.sourceSize, provenance.sourceSize);
    const auto cropMatrix = provenance.localToSourceMatrix();
    const auto outputMatrix = outputProvenance.localToSourceMatrix();
    for (size_t i = 0; i < cropMatrix.size(); ++i)
        QCOMPARE(outputMatrix[i], cropMatrix[i]);
    QVERIFY(output.data(QStringLiteral("circle")).canConvert<Circle2D>());
    const Circle2D circle = output.data(QStringLiteral("circle")).value<Circle2D>();
    QCOMPARE(circle.centerX, output.data(QStringLiteral("circle_center_x")).toDouble());
    QCOMPARE(circle.centerY, output.data(QStringLiteral("circle_center_y")).toDouble());
    QCOMPARE(circle.radius, output.data(QStringLiteral("circle_radius")).toDouble());

    QFile expectedFile(QDir(m_acceptanceRoot).filePath(QStringLiteral("expected/circle_640x480.json")));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    const double centerTolerance = expected.value(QStringLiteral("tolerance_center_px")).toDouble();
    const double radiusTolerance = expected.value(QStringLiteral("tolerance_radius_px")).toDouble();
    QVERIFY(std::abs(circle.centerX - 150.0) <= centerTolerance);
    QVERIFY(std::abs(circle.centerY - 150.0) <= centerTolerance);
    QVERIFY(std::abs(circle.radius - expected.value(QStringLiteral("circle_radius")).toDouble()) <= radiusTolerance);

    const QJsonObject circleResponse =
        actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{"runId", runId},
                                                                           {"instanceId", QStringLiteral("findcircle")},
                                                                           {"port", QStringLiteral("circle")}});
    QCOMPARE(circleResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(circleResponse.value(QStringLiteral("type")).toString(), QStringLiteral("DeepLux::Circle2D"));
    const QJsonObject circleValue = circleResponse.value(QStringLiteral("value")).toObject();
    QCOMPARE(circleValue.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QCOMPARE(circleValue.value(QStringLiteral("centerX")).toDouble(), circle.centerX);
    QCOMPARE(circleValue.value(QStringLiteral("centerY")).toDouble(), circle.centerY);
    QCOMPARE(circleValue.value(QStringLiteral("radius")).toDouble(), circle.radius);
    const auto expectedSourceCenter = provenance.mapLocalToSource(QPointF(circle.centerX, circle.centerY));
    QVERIFY(expectedSourceCenter);
    const QJsonObject source = circleValue.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), grabOutput.coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("centerX")).toDouble(), expectedSourceCenter->x());
    QCOMPARE(source.value(QStringLiteral("centerY")).toDouble(), expectedSourceCenter->y());
    QVERIFY(std::abs(source.value(QStringLiteral("centerX")).toDouble() - 320.0) <= centerTolerance);
    QVERIFY(std::abs(source.value(QStringLiteral("centerY")).toDouble() - 240.0) <= centerTolerance);
    QVERIFY(!source.contains(QStringLiteral("radius")));
    QVERIFY(!circleValue.contains(QStringLiteral("sourceRadius")));

    for (const QString& key :
         {QStringLiteral("circle_center_x"), QStringLiteral("circle_center_y"), QStringLiteral("circle_radius")}) {
        for (bool byPort : {true, false}) {
            QJsonObject args{{"runId", runId}, {"instanceId", QStringLiteral("findcircle")}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), output.data(key).toDouble());
            QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
}

void TestAcceptanceFlows::testAgentEdgeDefectDeviationUnitsRequireImageLocalPointSetSource() {
    ToolSchema::instance().registerDefaultTools();
    const QString imagePath = QDir(m_acceptanceRoot).filePath(QStringLiteral("data/circle_640x480.png"));
    QJsonArray points;
    for (int i = 0; i < 72; ++i) {
        const double angle = qDegreesToRadians(i * 5.0);
        points.append(QJsonArray{150.0 + 95.0 * std::cos(angle), 150.0 + 95.0 * std::sin(angle)});
    }
    const auto makeProject = [&](bool splitSources) {
        Project* project = ProjectManager::instance().newProject();
        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.params = QJsonObject{{QStringLiteral("grabSource"), QStringLiteral("File")},
                                  {QStringLiteral("filePath"), imagePath}};
        project->addModule(grab);
        ModuleInstance crop;
        crop.id = QStringLiteral("crop");
        crop.moduleId = QStringLiteral("CropImage");
        crop.params = QJsonObject{{QStringLiteral("rectangles"), QJsonArray{QJsonArray{320, 240, 150, 150, 0}}},
                                  {QStringLiteral("outputFirstAsImage"), true}};
        project->addModule(crop);
        ModuleInstance input;
        input.id = QStringLiteral("input");
        input.moduleId = QStringLiteral("MeasurementInput");
        input.params =
            QJsonObject{{QStringLiteral("mode"), QStringLiteral("point_set")}, {QStringLiteral("points"), points}};
        project->addModule(input);
        if (splitSources) {
            ModuleInstance secondInput = input;
            secondInput.id = QStringLiteral("input_points");
            project->addModule(secondInput);
        }
        ModuleInstance edge;
        edge.id = QStringLiteral("edge");
        edge.moduleId = QStringLiteral("EdgeDefectDetection");
        edge.params = QJsonObject{{QStringLiteral("threshold"), 2.0},
                                  {QStringLiteral("searchLength"), 10.0},
                                  {QStringLiteral("isConvex"), true}};
        project->addModule(edge);
        const auto connect = [project](const QString& from, const QString& fromPort, const QString& to,
                                       const QString& toPort) {
            ModuleConnection connection;
            connection.fromModuleId = from;
            connection.fromPort = fromPort;
            connection.toModuleId = to;
            connection.toPort = toPort;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        };
        connect(QStringLiteral("grab"), QStringLiteral("image"), QStringLiteral("crop"), QStringLiteral("image"));
        connect(QStringLiteral("crop"), QStringLiteral("image"), QStringLiteral("input"), QStringLiteral("image"));
        if (splitSources)
            connect(QStringLiteral("crop"), QStringLiteral("image"), QStringLiteral("input_points"),
                    QStringLiteral("image"));
        connect(QStringLiteral("input"), QStringLiteral("image"), QStringLiteral("edge"), QStringLiteral("image"));
        connect(splitSources ? QStringLiteral("input_points") : QStringLiteral("input"), QStringLiteral("fit_points"),
                QStringLiteral("edge"), QStringLiteral("reference_edge"));
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = flow.id;
        flow.nodeIds = splitSources
                           ? QStringList{QStringLiteral("grab"), QStringLiteral("crop"), QStringLiteral("input"),
                                         QStringLiteral("input_points"), QStringLiteral("edge")}
                           : QStringList{QStringLiteral("grab"), QStringLiteral("crop"), QStringLiteral("input"),
                                         QStringLiteral("edge")};
        project->setFlows({flow});
        return project;
    };
    const auto verifyOutput = [this](AgentActor& actor, RunEngine& engine, const QString& runId, bool expectUnit) {
        const ImageData output = engine.moduleOutput(QStringLiteral("edge"));
        QCOMPARE(output.data(QStringLiteral("has_defect")).toBool(), true);
        QCOMPARE(output.data(QStringLiteral("defect_count")).toDouble(), 1.0);
        QCOMPARE(output.data(QStringLiteral("convex_count")).toDouble(), 1.0);
        QCOMPARE(output.data(QStringLiteral("concave_count")).toDouble(), 0.0);
        QCOMPARE(output.data(QStringLiteral("max_deviation")).toDouble(), 7.0);
        QVERIFY(std::abs(output.data(QStringLiteral("mean_deviation")).toDouble() - 6.111111111111111) < 1e-9);
        QVERIFY(std::abs(output.data(QStringLiteral("deviation_stddev")).toDouble() - 0.3142696805273546) < 1e-9);
        QVERIFY(output.data(QStringLiteral("max_deviation")).isValid());
        QVERIFY(output.data(QStringLiteral("mean_deviation")).isValid());
        QVERIFY(output.data(QStringLiteral("deviation_stddev")).isValid());
        const QStringList keys{QStringLiteral("max_deviation"), QStringLiteral("mean_deviation"),
                               QStringLiteral("deviation_stddev")};
        for (const QString& key : keys) {
            const double expected = output.data(key).toDouble();
            QVERIFY(std::isfinite(expected));
            for (const bool byPort : {true, false}) {
                QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), "edge"}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
                const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QVERIFY(response.value(QStringLiteral("value")).isDouble());
                QCOMPARE(response.value(QStringLiteral("value")).toDouble(), expected);
                QCOMPARE(response.contains(QStringLiteral("unit")), expectUnit);
                if (expectUnit)
                    QCOMPARE(response.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
                QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
                QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
            }
        }
        ImageData snapshot;
        QMap<QString, QVariant> snapshotKeys;
        PortValueMap snapshotPorts;
        QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("edge"), snapshot, snapshotKeys, snapshotPorts));
        const QVariantList expectedRows = snapshotKeys.value(QStringLiteral("defect_regions")).toList();
        QCOMPARE(expectedRows.size(), 1);
        const QVariantMap fixedRow = expectedRows.first().toMap();
        QCOMPARE(fixedRow.value(QStringLiteral("polarity")).toString(), QStringLiteral("convex"));
        QCOMPARE(fixedRow.value(QStringLiteral("sample_count")).toInt(), 72);
        QCOMPARE(fixedRow.value(QStringLiteral("max_deviation")).toDouble(), 7.0);
        QVERIFY(std::abs(fixedRow.value(QStringLiteral("mean_deviation")).toDouble() - 6.111111111111111) < 1e-9);
        QCOMPARE(fixedRow.value(QStringLiteral("start_index")).toInt(), 37);
        QCOMPARE(fixedRow.value(QStringLiteral("end_index")).toInt(), 36);
        for (const bool byPort : {false, true}) {
            QJsonObject args{{QStringLiteral("runId"), runId},
                             {QStringLiteral("instanceId"), "edge"},
                             {QStringLiteral("limit"), 1000}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), QStringLiteral("defect_regions"));
            const QJsonObject regions = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(regions.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(!regions.contains(QStringLiteral("unit")));
            QCOMPARE(regions.value(QStringLiteral("totalRows")).toInt(), expectedRows.size());
            const QJsonArray rows = regions.value(QStringLiteral("value")).toArray();
            QCOMPARE(rows.size(), expectedRows.size());
            QCOMPARE(regions.contains(QStringLiteral("columnUnits")), expectUnit);
            if (expectUnit) {
                const QJsonObject expectedUnits{{QStringLiteral("max_deviation"), QStringLiteral("px")},
                                                {QStringLiteral("mean_deviation"), QStringLiteral("px")}};
                QCOMPARE(regions.value(QStringLiteral("columnUnits")).toObject(), expectedUnits);
            }
            for (int i = 0; i < rows.size(); ++i) {
                const QVariantMap expected = expectedRows.at(i).toMap();
                const QJsonObject actual = rows.at(i).toObject();
                QCOMPARE(actual, QJsonObject::fromVariantMap(expected));
                for (const QString& column : {QStringLiteral("max_deviation"), QStringLiteral("mean_deviation")}) {
                    QVERIFY(expected.contains(column));
                    const QVariant expectedValue = expected.value(column);
                    switch (expectedValue.type()) {
                    case QVariant::Int:
                    case QVariant::UInt:
                    case QVariant::LongLong:
                    case QVariant::ULongLong:
                    case QVariant::Double:
                        break;
                    default:
                        QFAIL(qPrintable(QStringLiteral("Snapshot column %1 is not numeric").arg(column)));
                    }
                    bool expectedOk = false;
                    const double expectedNumber = expectedValue.toDouble(&expectedOk);
                    QVERIFY(expectedOk);
                    QVERIFY(std::isfinite(expectedNumber));

                    const QJsonValue actualValue = actual.value(column);
                    QVERIFY(actualValue.isDouble());
                    const double actualNumber = actualValue.toDouble();
                    QVERIFY(std::isfinite(actualNumber));
                    QCOMPARE(actualNumber, expectedNumber);
                }
                QVERIFY(!actual.contains(QStringLiteral("coordinateSystem")));
                QVERIFY(!actual.contains(QStringLiteral("sourceImageCoordinates")));
            }
        }
    };

    AgentActor actor;
    QVERIFY(makeProject(false));
    Project* configuredProject = ProjectManager::instance().currentProject();
    QVERIFY(configuredProject);
    const auto initialEdge = configuredProject->moduleById(QStringLiteral("edge"));
    QVERIFY(initialEdge.has_value());
    const QJsonObject rejectedParams =
        actor.executeTool(QStringLiteral("set_module_params"),
                          QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("edge")},
                                      {QStringLiteral("values"), QJsonObject{{QStringLiteral("threshold"), 101.0}}}});
    QCOMPARE(rejectedParams.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
    QCOMPARE(rejectedParams.value(QStringLiteral("error")).toString(), QStringLiteral("缺陷阈值必须为[0,100]有限数"));
    const auto unchangedEdge = configuredProject->moduleById(QStringLiteral("edge"));
    QVERIFY(unchangedEdge.has_value());
    QCOMPARE(unchangedEdge->params.value(QStringLiteral("threshold")).toDouble(), 2.0);
    QCOMPARE(unchangedEdge->params.value(QStringLiteral("searchLength")).toDouble(), 10.0);
    QCOMPARE(unchangedEdge->params.value(QStringLiteral("isConvex")).toBool(), true);
    QVERIFY(!RunEngine::instance().isBusy());

    const QJsonObject acceptedParams =
        actor.executeTool(QStringLiteral("set_module_params"),
                          QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("edge")},
                                      {QStringLiteral("values"), QJsonObject{{QStringLiteral("threshold"), 4.0},
                                                                             {QStringLiteral("searchLength"), 10.0},
                                                                             {QStringLiteral("isConvex"), true}}}});
    QCOMPARE(acceptedParams.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
    const auto configuredEdge = configuredProject->moduleById(QStringLiteral("edge"));
    QVERIFY(configuredEdge.has_value());
    QCOMPARE(configuredEdge->params.value(QStringLiteral("threshold")).toDouble(), 4.0);
    QCOMPARE(configuredEdge->params.value(QStringLiteral("searchLength")).toDouble(), 10.0);
    QCOMPARE(configuredEdge->params.value(QStringLiteral("isConvex")).toBool(), true);
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString positiveRunId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!positiveRunId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(positiveRunId).finished, 10000);
    QVERIFY2(engine.runObservation(positiveRunId).result.success,
             qPrintable(engine.runObservation(positiveRunId).result.errorMessage));
    QCOMPARE(engine.moduleOutput(QStringLiteral("edge")).data(QStringLiteral("coordinate_frame")).toString(),
             QStringLiteral("ImageLocalPixel"));
    QCOMPARE(engine.moduleOutput(QStringLiteral("edge")).data(QStringLiteral("measurement_input_mode")).toString(),
             QStringLiteral("point_set"));
    QCOMPARE(engine.moduleOutput(QStringLiteral("crop")).width(), 300);
    QCOMPARE(engine.moduleOutput(QStringLiteral("crop")).height(), 300);
    verifyOutput(actor, engine, positiveRunId, true);

    for (const QString& negativeCase : {QStringLiteral("source"), QStringLiteral("mode"), QStringLiteral("frame")}) {
        const bool splitSources = negativeCase == QLatin1String("source");
        Project* project = makeProject(splitSources);
        bool loaded = false;
        if (negativeCase == QLatin1String("mode") || negativeCase == QLatin1String("frame")) {
            const QString mode =
                negativeCase == QLatin1String("mode") ? QStringLiteral("point_pair") : QStringLiteral("point_set");
            const QString frame =
                negativeCase == QLatin1String("frame") ? QStringLiteral("World3D") : QStringLiteral("ImageLocalPixel");
            loaded = engine.loadProject(project, [mode, frame](const ModuleInstance& instance) -> ModuleBase* {
                if (instance.moduleId == QLatin1String("MeasurementInput"))
                    return new EdgeDefectMeasurementInputFixtureModule(mode, frame);
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            });
        } else {
            loaded = engine.loadProject(project);
        }
        QVERIFY(loaded);
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QVERIFY(!runId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY2(engine.runObservation(runId).result.success,
                 qPrintable(engine.runObservation(runId).result.errorMessage));
        const ImageData output = engine.moduleOutput(QStringLiteral("edge"));
        if (negativeCase == QLatin1String("source")) {
            QCOMPARE(output.data(QStringLiteral("measurement_input_mode")).toString(), QStringLiteral("point_set"));
            QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
        } else {
            QCOMPARE(output.data(QStringLiteral("measurement_input_mode")).toString(),
                     negativeCase == QLatin1String("mode") ? QStringLiteral("point_pair")
                                                           : QStringLiteral("point_set"));
            QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(),
                     negativeCase == QLatin1String("frame") ? QStringLiteral("World3D")
                                                            : QStringLiteral("ImageLocalPixel"));
        }
        verifyOutput(actor, engine, runId, false);
    }

    QVERIFY(makeProject(false));
    Project* failureProject = ProjectManager::instance().currentProject();
    QVERIFY(failureProject);
    const QJsonArray insufficientPoints{points[0], points[1]};
    const QJsonObject pointsConfigured = actor.executeTool(
        QStringLiteral("set_module_params"),
        QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("input")},
                    {QStringLiteral("values"), QJsonObject{{QStringLiteral("points"), insufficientPoints}}}});
    QCOMPARE(pointsConfigured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
    const auto configuredInput = failureProject->moduleById(QStringLiteral("input"));
    QVERIFY(configuredInput.has_value());
    QCOMPARE(configuredInput->params.value(QStringLiteral("points")).toArray(), insufficientPoints);
    QCOMPARE(insufficientPoints.size(), 2);
    QVERIFY(insufficientPoints[0].toArray()[0].isDouble());
    QVERIFY(insufficientPoints[0].toArray()[1].isDouble());
    QVERIFY(insufficientPoints[1].toArray()[0].isDouble());
    QVERIFY(insufficientPoints[1].toArray()[1].isDouble());

    const QJsonObject failedStart = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString failedRunId = failedStart.value(QStringLiteral("runId")).toString();
    QVERIFY2(!failedRunId.isEmpty(), qPrintable(failedStart.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(RunEngine::instance().runObservation(failedRunId).finished, 10000);
    const RunObservation failedRun = RunEngine::instance().runObservation(failedRunId);
    QVERIFY(!failedRun.result.success);
    QCOMPARE(failedRun.result.failedModule, QStringLiteral("edge"));
    QCOMPARE(failedRun.result.errorMessage, QStringLiteral("参考边缘点不足，无法拟合基准"));
    QCOMPARE(actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{"runId", failedRunId}})
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("failed"));
    const QJsonObject unavailable = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), failedRunId},
                                                         {QStringLiteral("instanceId"), QStringLiteral("edge")},
                                                         {QStringLiteral("key"), QStringLiteral("max_deviation")}});
    QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
}

void TestAcceptanceFlows::testPointToPointDistanceFlow() {
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_distancepp.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load point-to-point project");
    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("distancepp"));
    QVERIFY2(out.data("distance").isValid(), "DistancePP produced no distance output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/two_points_640x480.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    const double actual = out.data("distance").toDouble();
    QVERIFY2(std::abs(actual - expected["distance"].toDouble()) <= expected["tolerance_distance_px"].toDouble(),
             qPrintable(QString("distance off: got %1").arg(actual)));
}

void TestAcceptanceFlows::testMeasurementInputPointPairRunsMeasureGap() {
    const PluginInfo metadata = PluginManager::instance().pluginInfo(QStringLiteral("MeasureGap"));
    QVERIFY(metadata.portsDeclared);
    const auto findPort = [](const QList<PortSpec>& ports, const QString& id) {
        return std::find_if(ports.cbegin(), ports.cend(), [&id](const PortSpec& port) { return port.id == id; });
    };
    for (const QString& id : {QStringLiteral("point1"), QStringLiteral("point2")}) {
        const auto port = findPort(metadata.inputPorts, id);
        QVERIFY(port != metadata.inputPorts.cend());
        QCOMPARE(port->type, DataType::Any);
        QVERIFY(port->required);
    }
    for (const auto& expected : {qMakePair(QStringLiteral("gap"), DataType::Number),
                                 qMakePair(QStringLiteral("gap_distance"), DataType::Number),
                                 qMakePair(QStringLiteral("gap_delta_x"), DataType::Number),
                                 qMakePair(QStringLiteral("gap_delta_y"), DataType::Number),
                                 qMakePair(QStringLiteral("gap_delta_z"), DataType::Number),
                                 qMakePair(QStringLiteral("measurement_dimension"), DataType::String)}) {
        const auto port = findPort(metadata.outputPorts, expected.first);
        QVERIFY(port != metadata.outputPorts.cend());
        QCOMPARE(port->type, expected.second);
    }

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    const QString loadErr =
        loadProjectWithPlaceholder("projects/accept_measuregap_pointpair.json", QString(), *project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    const RunObservation run = engine.runObservation(runId);
    QVERIFY2(run.result.success, qPrintable(run.result.errorMessage));

    const ImageData output = engine.moduleOutput(QStringLiteral("measuregap"));
    QVERIFY(output.data(QStringLiteral("gap_distance")).isValid());
    QCOMPARE(output.data(QStringLiteral("gap_distance")).toDouble(), 5.0);
    QCOMPARE(output.data(QStringLiteral("gap")).toDouble(), 5.0);
    QCOMPARE(output.data(QStringLiteral("gap_delta_x")).toDouble(), 3.0);
    QCOMPARE(output.data(QStringLiteral("gap_delta_y")).toDouble(), 4.0);
    QCOMPARE(output.data(QStringLiteral("gap_delta_z")).toDouble(), 0.0);
    QCOMPARE(output.data(QStringLiteral("measurement_dimension")).toString(), QStringLiteral("2d"));

    ImageData snapshot;
    QMap<QString, QVariant> snapshotKeys;
    PortValueMap snapshotPorts;
    QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("measuregap"), snapshot, snapshotKeys, snapshotPorts));
    QCOMPARE(snapshot.data(QStringLiteral("measurement_dimension")).toString(), QStringLiteral("2d"));
    ImageData inputSnapshot;
    QMap<QString, QVariant> inputKeys;
    PortValueMap inputPorts;
    QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("measurementinput"), inputSnapshot, inputKeys, inputPorts));
    QCOMPARE(inputSnapshot.data(QStringLiteral("measurement_input_mode")).toString(), QStringLiteral("point_pair"));
    QCOMPARE(inputSnapshot.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    QCOMPARE(inputKeys.value(QStringLiteral("point1")).toList(), QVariantList({1.0, 2.0}));
    QCOMPARE(inputKeys.value(QStringLiteral("point2")).toList(), QVariantList({4.0, 6.0}));
    QCOMPARE(inputPorts.value(QStringLiteral("point1")).toList(), QVariantList({1.0, 2.0}));
    QCOMPARE(inputPorts.value(QStringLiteral("point2")).toList(), QVariantList({4.0, 6.0}));

    const auto checkOutput = [&actor, &runId, &snapshotKeys, &snapshotPorts](const QString& name, double expected,
                                                                             bool expectUnit) {
        if (snapshotKeys.value(name).toDouble() != expected || snapshotPorts.value(name).toDouble() != expected)
            return false;
        for (const QString& selector : {QStringLiteral("key"), QStringLiteral("port")}) {
            const QJsonObject response =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{QStringLiteral("runId"), runId},
                                              {QStringLiteral("instanceId"), QStringLiteral("measuregap")},
                                              {selector, name}});
            if (response.value(QStringLiteral("status")).toString() != QLatin1String("available") ||
                response.value(QStringLiteral("value")).toDouble(std::numeric_limits<double>::quiet_NaN()) != expected)
                return false;
            if (response.contains(QStringLiteral("unit")) != expectUnit ||
                (expectUnit && response.value(QStringLiteral("unit")).toString() != QLatin1String("px")) ||
                response.contains(QStringLiteral("coordinateSystem")) ||
                response.contains(QStringLiteral("coordinateFrame")) ||
                response.contains(QStringLiteral("sourceImageCoordinates")))
                return false;
        }
        return true;
    };
    QVERIFY(checkOutput(QStringLiteral("gap"), 5.0, true));
    QVERIFY(checkOutput(QStringLiteral("gap_distance"), 5.0, true));
    QVERIFY(checkOutput(QStringLiteral("gap_delta_x"), 3.0, true));
    QVERIFY(checkOutput(QStringLiteral("gap_delta_y"), 4.0, true));
    QVERIFY(checkOutput(QStringLiteral("gap_delta_z"), 0.0, false));

    const QJsonObject dimensionKey =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{QStringLiteral("runId"), runId},
                                      {QStringLiteral("instanceId"), QStringLiteral("measuregap")},
                                      {QStringLiteral("key"), QStringLiteral("measurement_dimension")}});
    const QJsonObject dimensionPort =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{QStringLiteral("runId"), runId},
                                      {QStringLiteral("instanceId"), QStringLiteral("measuregap")},
                                      {QStringLiteral("port"), QStringLiteral("measurement_dimension")}});
    QCOMPARE(dimensionKey.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(dimensionPort.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(dimensionKey.value(QStringLiteral("value")).toString(), QStringLiteral("2d"));
    QCOMPARE(dimensionPort.value(QStringLiteral("value")).toString(),
             dimensionKey.value(QStringLiteral("value")).toString());
}

void TestAcceptanceFlows::testAgentMeasureGapPixelUnitsFailClosed() {
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const QStringList scalars{QStringLiteral("gap"), QStringLiteral("gap_distance"), QStringLiteral("gap_delta_x"),
                              QStringLiteral("gap_delta_y")};
    for (const QString& scenario : {QStringLiteral("fractional_positive"), QStringLiteral("crosswired"),
                                    QStringLiteral("spoofed_source"), QStringLiteral("mismatched_output")}) {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        const QString sourceId =
            scenario == QLatin1String("spoofed_source") ? QStringLiteral("spoof") : QStringLiteral("input");
        ModuleInstance source;
        source.id = sourceId;
        source.moduleId = scenario == QLatin1String("spoofed_source")
                              ? QStringLiteral("PointPairImageLocalSpoofFixture")
                              : QStringLiteral("MeasurementInput");
        source.name = source.moduleId;
        source.params = {
            {QStringLiteral("mode"), QStringLiteral("point_pair")},
            {QStringLiteral("point1"),
             scenario == QLatin1String("fractional_positive") ? QJsonArray{0.1, 0.2} : QJsonArray{1.0, 2.0}},
            {QStringLiteral("point2"),
             scenario == QLatin1String("fractional_positive") ? QJsonArray{1.3, 2.6} : QJsonArray{4.0, 6.0}}};
        project->addModule(source);
        ModuleInstance gap;
        gap.id = QStringLiteral("measuregap");
        gap.moduleId = QStringLiteral("MeasureGap");
        gap.name = gap.moduleId;
        project->addModule(gap);
        for (const QString& point : {QStringLiteral("point1"), QStringLiteral("point2")}) {
            ModuleConnection connection;
            connection.fromModuleId = sourceId;
            connection.fromPort =
                scenario == QLatin1String("crosswired")
                    ? (point == QLatin1String("point1") ? QStringLiteral("point2") : QStringLiteral("point1"))
                    : point;
            connection.toModuleId = gap.id;
            connection.toPort = point;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        }
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = flow.id;
        flow.nodeIds = QStringList{source.id, gap.id};
        project->setFlows({flow});

        const bool loaded = engine.loadProject(project, [scenario](const ModuleInstance& instance) -> ModuleBase* {
            if (instance.moduleId == QLatin1String("PointPairImageLocalSpoofFixture"))
                return new NonTargetPointPairImageLocalSpoofFixtureModule();
            if (instance.moduleId == QLatin1String("MeasureGap") && scenario == QLatin1String("mismatched_output"))
                return new MeasureGapScalarMismatchFixtureModule();
            return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
        });
        QVERIFY(loaded);
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QVERIFY(!runId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY2(engine.runObservation(runId).result.success,
                 qPrintable(engine.runObservation(runId).result.errorMessage));

        ImageData snapshot;
        QMap<QString, QVariant> named;
        PortValueMap ports;
        QVERIFY(engine.runOutputSnapshot(runId, gap.id, snapshot, named, ports));
        QCOMPARE(snapshot.data(QStringLiteral("measurement_dimension")).toString(), QStringLiteral("2d"));
        ImageData sourceSnapshot;
        QMap<QString, QVariant> sourceNamed;
        PortValueMap sourcePorts;
        QVERIFY(engine.runOutputSnapshot(runId, sourceId, sourceSnapshot, sourceNamed, sourcePorts));
        QCOMPARE(sourceSnapshot.data(QStringLiteral("measurement_input_mode")).toString(),
                 QStringLiteral("point_pair"));
        QCOMPARE(sourceSnapshot.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
        const bool expectUnit = scenario == QLatin1String("fractional_positive");
        for (const QString& scalar : scalars) {
            QVERIFY(named.value(scalar).isValid());
            QVERIFY(ports.value(scalar).isValid());
            bool ok = false;
            const double expected = named.value(scalar).toDouble(&ok);
            QVERIFY(ok);
            QVERIFY(std::isfinite(expected));
            QCOMPARE(ports.value(scalar).toDouble(), expected);
            for (const bool byPort : {false, true}) {
                QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), gap.id}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), scalar);
                const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QCOMPARE(response.value(QStringLiteral("value")).toDouble(), expected);
                QCOMPARE(response.contains(QStringLiteral("unit")), expectUnit);
                if (expectUnit)
                    QCOMPARE(response.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
                QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
                QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
                QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
            }
        }
    }
    engine.clearModules();
    ProjectManager::instance().closeProject();
}

void TestAcceptanceFlows::testPointCloudPointToPlaneDistanceFlow() {
    const QString dataDir = QDir(m_acceptanceRoot).filePath("data");
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_point_surface.json", dataDir, project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load point-cloud measurement project");
    engine.runOnce();

    const ImageData loaded = engine.moduleOutput(QStringLiteral("loadcloud"));
    QVERIFY2(loaded.data("point_count").toInt() == 273, "point cloud vertex count mismatch");
    const ImageData out = engine.moduleOutput(QStringLiteral("pointsurface"));
    QVERIFY2(out.data("distance").isValid(), "PointSurfaceDistance produced no distance output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/plane_z5.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
    const double actual = out.data("distance").toDouble();
    QVERIFY2(std::abs(actual - expected["point_surface_distance"].toDouble()) <=
                 expected["tolerance_distance"].toDouble(),
             qPrintable(QString("point-surface distance off: got %1").arg(actual)));
}

void TestAcceptanceFlows::testAgentFreeformSurfaceResultsRetainUnknownUnits() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QString plyPath = dataDir.filePath(QStringLiteral("surface.ply"));
    QFile ply(plyPath);
    QVERIFY(ply.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream stream(&ply);
    stream << "ply\nformat ascii 1.0\nelement vertex 4\n"
              "property float x\nproperty float y\nproperty float z\nend_header\n"
              "0 0 0\n1 0 0.25\n1 1 0.5\n0 1 0.125\n";
    ply.close();

    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project);
    ModuleInstance load;
    load.id = QStringLiteral("loadcloud");
    load.moduleId = QStringLiteral("LoadPointCloud");
    load.params = QJsonObject{{QStringLiteral("filePath"), plyPath},
                              {QStringLiteral("scaleX"), 2.0},
                              {QStringLiteral("scaleY"), 3.0},
                              {QStringLiteral("scaleZ"), 4.0}};
    project->addModule(load);
    ModuleInstance surface;
    surface.id = QStringLiteral("surface");
    surface.moduleId = QStringLiteral("FreeformSurface");
    project->addModule(surface);
    ModuleConnection connection;
    connection.fromModuleId = load.id;
    connection.fromPort = QStringLiteral("image");
    connection.toModuleId = surface.id;
    connection.toPort = QStringLiteral("image");
    connection.edgeType = QStringLiteral("data");
    project->addConnection(connection);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{load.id, surface.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    const ImageData output = engine.moduleOutput(surface.id);
    QCOMPARE(output.data(QStringLiteral("point_count")).toInt(), 4);

    AgentActor actor;
    const QJsonArray loadOutputPorts = actor
                                           .executeTool(QStringLiteral("get_module_params_schema"),
                                                        QJsonObject{{QStringLiteral("instanceId"), load.id}})
                                           .value(QStringLiteral("ports"))
                                           .toObject()
                                           .value(QStringLiteral("outputs"))
                                           .toArray();
    const auto pointCountPort =
        std::find_if(loadOutputPorts.cbegin(), loadOutputPorts.cend(), [](const QJsonValue& port) {
            return port.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("point_count");
        });
    QVERIFY(pointCountPort != loadOutputPorts.cend());
    QCOMPARE(pointCountPort->toObject().value(QStringLiteral("type")).toString(), QStringLiteral("Integer"));
    QVERIFY(!pointCountPort->toObject().value(QStringLiteral("required")).toBool());
    QVERIFY(std::none_of(loadOutputPorts.cbegin(), loadOutputPorts.cend(), [](const QJsonValue& port) {
        return port.toObject().value(QStringLiteral("id")).toString() == QStringLiteral("point_cloud_path");
    }));

    ImageData loaded;
    QMap<QString, QVariant> loadValues;
    PortValueMap loadSnapshotPorts;
    QVERIFY(engine.runOutputSnapshot(runId, load.id, loaded, loadValues, loadSnapshotPorts));
    QCOMPARE(loaded.data(QStringLiteral("point_count")).toInt(), 4);
    const QVariant pointCloudValue = loaded.data(QStringLiteral("point_cloud"));
    QCOMPARE(pointCloudValue.userType(), qMetaTypeId<PointCloudData>());
    const PointCloudData pointCloud = pointCloudValue.value<PointCloudData>();
    const QList<Eigen::Vector3d> expectedPoints{Eigen::Vector3d(0.0, 0.0, 0.0), Eigen::Vector3d(2.0, 0.0, 1.0),
                                                Eigen::Vector3d(2.0, 3.0, 2.0), Eigen::Vector3d(0.0, 3.0, 0.5)};
    QCOMPARE(pointCloud.points.size(), static_cast<size_t>(expectedPoints.size()));
    for (int i = 0; i < expectedPoints.size(); ++i) {
        QCOMPARE(pointCloud.points.at(static_cast<size_t>(i)).x(), expectedPoints.at(i).x());
        QCOMPARE(pointCloud.points.at(static_cast<size_t>(i)).y(), expectedPoints.at(i).y());
        QCOMPARE(pointCloud.points.at(static_cast<size_t>(i)).z(), expectedPoints.at(i).z());
    }
    const QJsonObject pointCountKey = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                         {QStringLiteral("instanceId"), load.id},
                                                         {QStringLiteral("key"), QStringLiteral("point_count")}});
    const QJsonObject pointCountByPort = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                         {QStringLiteral("instanceId"), load.id},
                                                         {QStringLiteral("port"), QStringLiteral("point_count")}});
    for (const QJsonObject& response : {pointCountKey, pointCountByPort}) {
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(response.value(QStringLiteral("value")).toInt(), loaded.data(QStringLiteral("point_count")).toInt());
        QVERIFY(!response.contains(QStringLiteral("unit")));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    }
    QCOMPARE(pointCountByPort.value(QStringLiteral("source")).toString(), QStringLiteral("port"));

    const QJsonObject pointCloudPreviewResponse = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                         {QStringLiteral("instanceId"), load.id},
                                                         {QStringLiteral("key"), QStringLiteral("point_cloud")},
                                                         {QStringLiteral("preview"), true}});
    QCOMPARE(pointCloudPreviewResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject pointCloudSummary = pointCloudPreviewResponse.value(QStringLiteral("value")).toObject();
    QCOMPARE(pointCloudSummary.value(QStringLiteral("pointCount")).toInt(), 4);
    QCOMPARE(pointCloudSummary.value(QStringLiteral("hasNormals")).toBool(), pointCloud.hasNormals());
    QCOMPARE(pointCloudSummary.value(QStringLiteral("hasColors")).toBool(), pointCloud.hasColors());
    QCOMPARE(pointCloudSummary.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("unspecified"));
    const QJsonObject pointCloudPreview = pointCloudPreviewResponse.value(QStringLiteral("preview")).toObject();
    QCOMPARE(pointCloudPreview.value(QStringLiteral("kind")).toString(), QStringLiteral("point_sample"));
    QCOMPARE(pointCloudPreview.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("unspecified"));
    QCOMPARE(pointCloudPreview.value(QStringLiteral("sampleCount")).toInt(), 4);
    QCOMPARE(pointCloudPreview.value(QStringLiteral("totalPoints")).toInt(), 4);
    const QJsonArray previewPoints = pointCloudPreview.value(QStringLiteral("points")).toArray();
    QCOMPARE(previewPoints.size(), expectedPoints.size());
    QVERIFY(!pointCloudSummary.contains(QStringLiteral("unit")));
    QVERIFY(!pointCloudSummary.contains(QStringLiteral("coordinateFrame")));
    QVERIFY(!pointCloudSummary.contains(QStringLiteral("sourceImageCoordinates")));
    QVERIFY(!pointCloudPreview.contains(QStringLiteral("unit")));
    QVERIFY(!pointCloudPreview.contains(QStringLiteral("coordinateFrame")));
    QVERIFY(!pointCloudPreview.contains(QStringLiteral("sourceImageCoordinates")));
    for (int i = 0; i < expectedPoints.size(); ++i) {
        const Eigen::Vector3d& expected = expectedPoints.at(i);
        const QJsonArray previewPoint = previewPoints.at(i).toArray();
        QCOMPARE(previewPoint, (QJsonArray{expected.x(), expected.y(), expected.z()}));
    }

    for (const QString& key : {QStringLiteral("surface_area"), QStringLiteral("surface_roughness")}) {
        const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"),
                                                       QJsonObject{{QStringLiteral("runId"), runId},
                                                                   {QStringLiteral("instanceId"), surface.id},
                                                                   {QStringLiteral("key"), key}});
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(response.value(QStringLiteral("value")).toDouble(), output.data(key).toDouble());
        QVERIFY(!response.contains(QStringLiteral("unit")));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
    }
}

void TestAcceptanceFlows::testFitLineFlow() {
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_fitline.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load fit-line project");
    engine.runOnce();

    const ImageData out = engine.moduleOutput(QStringLiteral("fitline"));
    QVERIFY2(out.data("line_error").isValid(), "FitLine produced no line_error output");

    QFile expectedFile(QDir(m_acceptanceRoot).filePath("expected/fitline_points.json"));
    QVERIFY(expectedFile.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();

    // 共线点 LS 拟合，误差应接近 0
    const double fitError = out.data("line_error").toDouble();
    QVERIFY2(fitError <= expected["tolerance_fit_error"].toDouble(),
             qPrintable(QString("fit error too large: got %1").arg(fitError)));

    // 拟合直线应经过已知线段中点 (320,240) 附近：用拟合直线方程验证
    // line_row/col 为拟合线上两点，验证中点到拟合线距离很小
    const double row1 = out.data("line_row1").toDouble();
    const double col1 = out.data("line_col1").toDouble();
    const double row2 = out.data("line_row2").toDouble();
    const double col2 = out.data("line_col2").toDouble();
    // 拟合线方向 (col2-col1, row2-row1)，中点 (320,240) 到线的距离
    const double dx = col2 - col1;
    const double dy = row2 - row1;
    const double len = std::sqrt(dx * dx + dy * dy);
    QVERIFY(len > 1e-6);
    // 点 (x=320, y=240)；线过 (col1, row1)，方向 (dx,dy)
    const double distToLine = std::abs(dy * 320.0 - dx * 240.0 + (dx * row1 - dy * col1)) / len;
    QVERIFY2(distToLine <= expected["tolerance_endpoint_px"].toDouble(),
             qPrintable(QString("fitted line far from known midpoint: %1").arg(distToLine)));
}

void TestAcceptanceFlows::testAgentReadsFixedAlgorithmResults() {
    ToolSchema::instance().registerDefaultTools();
    struct Case {
        const char* project;
        const char* expected;
        const char* instance;
        const char* key;
        const char* expectedKey;
        const char* toleranceKey;
    };
    const Case cases[] = {
        {"accept_findcircle.json", "circle_640x480.json", "findcircle", "circle_center_x", "circle_center_x",
         "tolerance_center_px"},
        {"accept_distancepp.json", "two_points_640x480.json", "distancepp", "distance", "distance",
         "tolerance_distance_px"},
        {"accept_fitline.json", "fitline_points.json", "fitline", "line_error", nullptr, "tolerance_fit_error"},
        {"accept_point_surface.json", "plane_z5.json", "pointsurface", "distance", "point_surface_distance",
         "tolerance_distance"},
        {"accept_blob.json", "circle_640x480.json", "blob", "blob_center_x", "circle_center_x", "tolerance_center_px"},
    };
    AgentActor actor;
    for (const Case& item : cases) {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        const QString loadError = loadProjectWithPlaceholder(QStringLiteral("projects/") + QLatin1String(item.project),
                                                             QDir(m_acceptanceRoot).filePath("data"), *project);
        QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
        const QJsonObject started = actor.executeTool("run_flow", {});
        const QString runId = started.value("runId").toString();
        QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
        QTRY_VERIFY_WITH_TIMEOUT(RunEngine::instance().runObservation(runId).finished, 10000);
        const RunObservation run = RunEngine::instance().runObservation(runId);
        QVERIFY2(run.result.success, qPrintable(run.result.errorMessage));
        const QJsonObject actual =
            actor.executeTool("get_module_output", QJsonObject{{"runId", runId},
                                                               {"instanceId", QLatin1String(item.instance)},
                                                               {"key", QLatin1String(item.key)}});
        QCOMPARE(actual.value("status").toString(), QStringLiteral("available"));
        const ImageData engineOutput = RunEngine::instance().moduleOutput(QLatin1String(item.instance));
        QVERIFY(engineOutput.data(QLatin1String(item.key)).isValid());
        QCOMPARE(actual.value("value").toDouble(), engineOutput.data(QLatin1String(item.key)).toDouble());
        if (QLatin1String(item.project) == QLatin1String("accept_findcircle.json")) {
            const QJsonObject previewResult = actor.executeTool(
                "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "grab"}, {"preview", true}});
            const QJsonObject preview = previewResult.value("summary").toObject().value("preview").toObject();
            QCOMPARE(previewResult.value("status").toString(), QStringLiteral("available"));
            QCOMPARE(preview.value("width").toInt(), 64);
            QCOMPARE(preview.value("height").toInt(), 48);
            QCOMPARE(preview.value("scaleX").toDouble(), 10.0);
            QCOMPARE(preview.value("scaleY").toDouble(), 10.0);
            QCOMPARE(preview.value("sourceRect").toArray(), QJsonArray({0, 0, 640, 480}));
            QVERIFY(!QByteArray::fromBase64(preview.value("base64").toString().toLatin1()).isEmpty());
            QVERIFY(QJsonDocument(previewResult).toJson(QJsonDocument::Compact).size() <= 65536);
        }
        QFile expectedFile(QDir(m_acceptanceRoot).filePath(QStringLiteral("expected/") + item.expected));
        QVERIFY(expectedFile.open(QIODevice::ReadOnly));
        const QJsonObject expected = QJsonDocument::fromJson(expectedFile.readAll()).object();
        const double target = item.expectedKey ? expected.value(QLatin1String(item.expectedKey)).toDouble() : 0.0;
        const double tolerance = expected.value(QLatin1String(item.toleranceKey)).toDouble();
        QVERIFY2(std::abs(actual.value("value").toDouble() - target) <= tolerance,
                 qPrintable(QStringLiteral("%1/%2 differs from fixed expectation")
                                .arg(QLatin1String(item.project), QLatin1String(item.key))));
        RunEngine::instance().clearModules();
        ProjectManager::instance().closeProject();
    }
}

void TestAcceptanceFlows::testAgentSkippedBranchIsUnavailable() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    const QString error = loadProjectWithPlaceholder("projects/accept_controlflow.json", QString(), *project);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    AgentActor actor;
    const QJsonObject started = actor.executeTool("run_flow", {});
    const QString runId = started.value("runId").toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
    QTRY_VERIFY(RunEngine::instance().runObservation(runId).finished);
    QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
             QStringLiteral("completed"));
    const QJsonObject skipped =
        actor.executeTool("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "falsebranch"}});
    QCOMPARE(skipped.value("status").toString(), QStringLiteral("unavailable"));
    const QJsonObject skippedKey = actor.executeTool(
        "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "falsebranch"}, {"key", "result"}});
    QCOMPARE(skippedKey.value("status").toString(), QStringLiteral("unavailable"));
}

void TestAcceptanceFlows::testControlFlowIfBranchFlow() {
    // 阶7: 控制流验收工程——If(1>0) 为真，真分支执行、假分支跳过
    Project project;
    QString loadErr = loadProjectWithPlaceholder("projects/accept_controlflow.json", QString(), project);
    QVERIFY2(loadErr.isEmpty(), qPrintable(loadErr));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "load control-flow project");

    QStringList finished;
    QStringList skipped;
    QMetaObject::Connection fConn =
        connect(&engine, &RunEngine::moduleFinished, [&](const QString& id, bool, int) { finished.append(id); });
    QMetaObject::Connection sConn =
        connect(&engine, &RunEngine::moduleSkipped, [&](const QString& id) { skipped.append(id); });
    engine.runOnce();
    disconnect(fConn);
    disconnect(sConn);

    QVERIFY2(finished.contains("cond"), "condition must run");
    QVERIFY2(finished.contains("truebranch"), "true branch must run for 1>0");
    QVERIFY2(!finished.contains("falsebranch"), "false branch must not run for 1>0");
    QVERIFY2(skipped.contains("falsebranch"), "false branch must be reported skipped");
}

// ---------------------------------------------------------------------------
// 阶段 4: Loop / While / StopWhile 流程验收
// ---------------------------------------------------------------------------

namespace {
// 并行分支会从工作线程发出信号，聚合记录需要串行化。
struct FlowTrace {
    QMutex mutex;
    QHash<QString, int> startedCount;
    QStringList finishedOrder;
    QHash<QString, int> finishedCount;
    QHash<QString, int> successfulCount;
    QHash<QString, int> failedCount;
    QStringList skipped;
    bool runSuccess = false;
    int runElapsedMs = -1;

    void connectTo(RunEngine& engine, std::vector<QMetaObject::Connection>& conns) {
        conns.push_back(QObject::connect(&engine, &RunEngine::moduleStarted, [this](const QString& id) {
            QMutexLocker locker(&mutex);
            startedCount[id]++;
        }));
        conns.push_back(
            QObject::connect(&engine, &RunEngine::moduleFinished, [this](const QString& id, bool success, int) {
                QMutexLocker locker(&mutex);
                finishedOrder.append(id);
                finishedCount[id]++;
                (success ? successfulCount : failedCount)[id]++;
            }));
        conns.push_back(QObject::connect(&engine, &RunEngine::moduleSkipped, [this](const QString& id) {
            QMutexLocker locker(&mutex);
            skipped.append(id);
        }));
        conns.push_back(QObject::connect(&engine, &RunEngine::runFinished, [this](const RunResult& r) {
            QMutexLocker locker(&mutex);
            runSuccess = r.success;
            runElapsedMs = r.elapsedMs;
        }));
    }
    void disconnectAll(std::vector<QMetaObject::Connection>& conns) {
        for (const auto& c : conns)
            QObject::disconnect(c);
        conns.clear();
    }

    int started(const QString& id) {
        QMutexLocker locker(&mutex);
        return startedCount.value(id);
    }
    int finished(const QString& id) {
        QMutexLocker locker(&mutex);
        return finishedCount.value(id);
    }
    int succeeded(const QString& id) {
        QMutexLocker locker(&mutex);
        return successfulCount.value(id);
    }
    int failed(const QString& id) {
        QMutexLocker locker(&mutex);
        return failedCount.value(id);
    }
    bool wasSkipped(const QString& id) {
        QMutexLocker locker(&mutex);
        return skipped.contains(id);
    }
    bool finishedAfter(const QString& later, const QString& earlier) {
        QMutexLocker locker(&mutex);
        return finishedOrder.indexOf(later) > finishedOrder.indexOf(earlier);
    }
    QStringList finishSequence() {
        QMutexLocker locker(&mutex);
        return finishedOrder;
    }
    bool succeededRun() {
        QMutexLocker locker(&mutex);
        return runSuccess;
    }
    int elapsedMs() {
        QMutexLocker locker(&mutex);
        return runElapsedMs;
    }
};
} // namespace

void TestAcceptanceFlows::testAgentRunFlowPreservesIfBranchSemantics() {
    ToolSchema::instance().registerDefaultTools();
    for (const bool condition : {true, false}) {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        QVERIFY2(loadProjectWithPlaceholder("projects/accept_controlflow.json", QString(), *project).isEmpty(),
                 "load control-flow project into current ProjectManager project");
        QVERIFY(project->setModuleParam("cond", "expressionString", condition ? "true" : "false"));

        for (const QString& id : {QStringLiteral("truebranch"), QStringLiteral("falsebranch")}) {
            ModuleInstance branch = project->moduleById(id).value();
            branch.moduleId = QStringLiteral("数学运算");
            branch.name = id;
            branch.params =
                QJsonObject{{"operation", "Add"},
                            {"operandA", condition ? "20" : "2"},
                            {"operandB", condition ? "22" : "3"},
                            {"outputVar", id == QStringLiteral("truebranch") ? "true_value" : "false_value"}};
            project->updateModule(id, branch);
        }

        AgentActor actor;
        RunEngine& engine = RunEngine::instance();
        FlowTrace trace;
        std::vector<QMetaObject::Connection> connections;
        trace.connectTo(engine, connections);

        const QJsonObject started = actor.executeTool("run_flow", {});
        const QString runId = started.value("runId").toString();
        QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        trace.disconnectAll(connections);

        QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
                 QStringLiteral("completed"));
        const QString selected = condition ? QStringLiteral("truebranch") : QStringLiteral("falsebranch");
        const QString skipped = condition ? QStringLiteral("falsebranch") : QStringLiteral("truebranch");
        const QString selectedKey = condition ? QStringLiteral("true_value") : QStringLiteral("false_value");
        const QString skippedKey = condition ? QStringLiteral("false_value") : QStringLiteral("true_value");
        QCOMPARE(trace.succeeded(selected), 1);
        QVERIFY(trace.wasSkipped(skipped));

        const QJsonObject selectedOutput = actor.executeTool(
            "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", selected}, {"key", selectedKey}});
        QCOMPARE(selectedOutput.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(selectedOutput.value("value").toDouble(), condition ? 42.0 : 5.0);
        const QJsonObject skippedOutput = actor.executeTool(
            "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", skipped}, {"key", skippedKey}});
        QCOMPARE(skippedOutput.value("status").toString(), QStringLiteral("unavailable"));
    }
}

void TestAcceptanceFlows::testAgentRunFlowPreservesDisabledImageBypass() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_blob.json", QDir(m_acceptanceRoot).filePath("data"), *project)
                 .isEmpty(),
             "load Blob project into current ProjectManager project");

    ModuleInstance crop;
    crop.id = QStringLiteral("disabled_crop");
    crop.moduleId = QStringLiteral("CropImage");
    crop.name = QStringLiteral("disabled crop");
    crop.params =
        QJsonObject{{"rectangles", QJsonArray{QJsonArray{320, 240, 100, 100, 0}}}, {"outputFirstAsImage", true}};
    project->addModule(crop, 1);
    project->removeConnection(QStringLiteral("grab"), QStringLiteral("blob"));
    ModuleConnection sourceToCrop;
    sourceToCrop.fromModuleId = QStringLiteral("grab");
    sourceToCrop.toModuleId = crop.id;
    sourceToCrop.fromPort = QStringLiteral("image");
    sourceToCrop.toPort = QStringLiteral("image");
    sourceToCrop.edgeType = QStringLiteral("data");
    project->addConnection(sourceToCrop);
    ModuleConnection cropToBlob = sourceToCrop;
    cropToBlob.fromModuleId = crop.id;
    cropToBlob.toModuleId = QStringLiteral("blob");
    project->addConnection(cropToBlob);
    ProjectFlow flow = project->flow(QStringLiteral("main"));
    flow.nodeIds.insert(1, crop.id);
    project->setFlows({flow});

    AgentActor actor;
    QCOMPARE(actor.executeTool("set_module_enabled", QJsonObject{{"instanceId", crop.id}, {"enabled", false}})
                 .value("status")
                 .toString(),
             QStringLiteral("updated"));
    RunEngine& engine = RunEngine::instance();
    FlowTrace trace;
    std::vector<QMetaObject::Connection> connections;
    trace.connectTo(engine, connections);

    const QJsonObject started = actor.executeTool("run_flow", {});
    const QString runId = started.value("runId").toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    trace.disconnectAll(connections);

    QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
             QStringLiteral("completed"));
    QVERIFY(trace.wasSkipped(crop.id));
    QCOMPARE(trace.started(crop.id), 0);
    QCOMPARE(trace.finished(crop.id), 0);
    QCOMPARE(trace.succeeded(QStringLiteral("blob")), 1);
    const QJsonObject blob = actor.executeTool(
        "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "blob"}, {"key", "blob_center_x"}});
    QCOMPARE(blob.value("status").toString(), QStringLiteral("available"));
    QVERIFY(std::abs(blob.value("value").toDouble() - 320.0) < 3.0);
    QCOMPARE(actor
                 .executeTool("get_module_output",
                              QJsonObject{{"runId", runId}, {"instanceId", crop.id}, {"key", "crop_count"}})
                 .value("status")
                 .toString(),
             QStringLiteral("unavailable"));
}

void TestAcceptanceFlows::testAgentBlobLargestDetectionMapsThroughCropOrigin() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(0);
    const auto fillSquare = [&image](int left, int top, int size) {
        for (int y = top; y < top + size; ++y) {
            for (int x = left; x < left + size; ++x) {
                image.setPixelColor(x, y, Qt::white);
            }
        }
    };
    fillSquare(70, 60, 41);
    fillSquare(180, 130, 21);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("circle_640x480.png"))));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_blob.json", dataDir.path(), *project).isEmpty(),
             "load Blob project with synthetic image");

    ModuleInstance crop;
    crop.id = QStringLiteral("crop_blob");
    crop.moduleId = QStringLiteral("CropImage");
    crop.name = QStringLiteral("crop before blob");
    crop.params =
        QJsonObject{{"rectangles", QJsonArray{QJsonArray{150, 110, 120, 90, 0}}}, {"outputFirstAsImage", true}};
    project->addModule(crop, 1);
    project->removeConnection(QStringLiteral("grab"), QStringLiteral("blob"));
    ModuleConnection grabToCrop;
    grabToCrop.fromModuleId = QStringLiteral("grab");
    grabToCrop.toModuleId = crop.id;
    grabToCrop.fromPort = QStringLiteral("image");
    grabToCrop.toPort = QStringLiteral("image");
    grabToCrop.edgeType = QStringLiteral("data");
    project->addConnection(grabToCrop);
    ModuleConnection cropToBlob = grabToCrop;
    cropToBlob.fromModuleId = crop.id;
    cropToBlob.toModuleId = QStringLiteral("blob");
    project->addConnection(cropToBlob);
    ProjectFlow flow = project->flow(QStringLiteral("main"));
    flow.nodeIds.insert(1, crop.id);
    project->setFlows({flow});

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData cropOutput = engine.moduleOutput(crop.id);
    QVERIFY(cropOutput.hasCoordinateProvenance());
    const ImageCoordinateProvenance provenance = cropOutput.coordinateProvenance();
    const QJsonObject response = actor.executeTool(
        QStringLiteral("get_module_output"),
        QJsonObject{{"runId", runId}, {"instanceId", "blob"}, {"key", "largest_blob"}, {"offset", 0}, {"limit", 1}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("totalRows")).toInt(), 1);
    const QJsonArray detections = response.value(QStringLiteral("value")).toArray();
    QCOMPARE(detections.size(), 1);
    const QJsonObject detection = detections.first().toObject();
    QCOMPARE(detection.value(QStringLiteral("x")).toDouble(), 40.0);
    QCOMPARE(detection.value(QStringLiteral("y")).toDouble(), 40.0);
    QCOMPARE(detection.value(QStringLiteral("width")).toDouble(), 41.0);
    QCOMPARE(detection.value(QStringLiteral("height")).toDouble(), 41.0);
    QCOMPARE(detection.value(QStringLiteral("label")).toString(), QStringLiteral("largest_blob"));
    QCOMPARE(detection.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));

    const ImageData blobOutput = engine.moduleOutput(QStringLiteral("blob"));
    QCOMPARE(blobOutput.data(QStringLiteral("blob_count")).toInt(), 2);
    QCOMPARE(blobOutput.data(QStringLiteral("blob_center_x")).toDouble(), 60.0);
    QCOMPARE(blobOutput.data(QStringLiteral("blob_center_y")).toDouble(), 60.0);
    QCOMPARE(blobOutput.data(QStringLiteral("blob_area")).toDouble(), 1600.0);
    QCOMPARE(blobOutput.data(QStringLiteral("blob_perimeter")).toDouble(), 160.0);
    QVERIFY(blobOutput.data(QStringLiteral("largest_blob_center")).canConvert<QPointF>());
    const QPointF localCenter = blobOutput.data(QStringLiteral("largest_blob_center")).toPointF();
    QCOMPARE(localCenter, QPointF(60.0, 60.0));
    QCOMPARE(localCenter.x(), blobOutput.data(QStringLiteral("blob_center_x")).toDouble());
    QCOMPARE(localCenter.y(), blobOutput.data(QStringLiteral("blob_center_y")).toDouble());
    const QJsonObject centerResponse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "blob"}, {"port", "largest_blob_center"}});
    QCOMPARE(centerResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(centerResponse.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
    QCOMPARE(centerResponse.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const QJsonArray expectedLocalCenter{60.0, 60.0};
    QCOMPARE(centerResponse.value(QStringLiteral("value")).toArray(), expectedLocalCenter);
    const auto expectedSourceCenter = provenance.mapLocalToSource(localCenter);
    QVERIFY(expectedSourceCenter.has_value());
    const QJsonObject sourceCenter = centerResponse.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(sourceCenter.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
    QCOMPARE(sourceCenter.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    const QJsonArray expectedSourceCenterValue{expectedSourceCenter->x(), expectedSourceCenter->y()};
    QCOMPARE(sourceCenter.value(QStringLiteral("value")).toArray(), expectedSourceCenterValue);
    QCOMPARE(expectedSourceCenter.value(), QPointF(90.0, 80.0));

    const QList<QPair<QString, QString>> blobPixelMetrics{{QStringLiteral("blob_center_x"), QStringLiteral("px")},
                                                          {QStringLiteral("blob_center_y"), QStringLiteral("px")},
                                                          {QStringLiteral("blob_area"), QStringLiteral("px^2")},
                                                          {QStringLiteral("blob_perimeter"), QStringLiteral("px")}};
    for (const auto& metric : blobPixelMetrics) {
        const QJsonObject scalar =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "blob"}, {"key", metric.first}});
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), metric.second);
        QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), blobOutput.data(metric.first).toDouble());
        QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        const QJsonObject port =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "blob"}, {"port", metric.first}});
        QCOMPARE(port.value(QStringLiteral("errorCode")).toString(), QStringLiteral("OUTPUT_NOT_FOUND"));
        QVERIFY(!port.contains(QStringLiteral("unit")));
    }
    for (const QString& key : {QStringLiteral("blob_count"), QStringLiteral("blob_circularity")}) {
        const QJsonObject scalar = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "blob"}, {"key", key}});
        QVERIFY(!scalar.contains(QStringLiteral("unit")));
    }

    const QJsonObject centerKeyResponse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "blob"}, {"key", "largest_blob_center"}});
    QCOMPARE(centerKeyResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(centerKeyResponse.value(QStringLiteral("type")).toString(), QStringLiteral("QPointF"));
    QCOMPARE(centerKeyResponse.value(QStringLiteral("value")).toArray(), expectedLocalCenter);
    QVERIFY(!centerKeyResponse.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!centerKeyResponse.contains(QStringLiteral("sourceImageCoordinates")));
    QVERIFY(blobOutput.data(QStringLiteral("blob_area")).toDouble() > 0.0);
    QVERIFY(blobOutput.data(QStringLiteral("blob_perimeter")).toDouble() > 0.0);
    const double circularity = blobOutput.data(QStringLiteral("blob_circularity")).toDouble();
    QCOMPARE(detection.value(QStringLiteral("score")).toDouble(), circularity);

    const QRectF localBounds(40.0, 40.0, 41.0, 41.0);
    const auto expectedSourceBounds = provenance.mapRectToSourceBounds(localBounds);
    QVERIFY(expectedSourceBounds.has_value());
    const QJsonObject source = detection.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(detection.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
    const QJsonArray sourceBbox = source.value(QStringLiteral("bbox")).toArray();
    QCOMPARE(sourceBbox.size(), 4);
    QCOMPARE(sourceBbox.at(0).toDouble(), expectedSourceBounds->x());
    QCOMPARE(sourceBbox.at(1).toDouble(), expectedSourceBounds->y());
    QCOMPARE(sourceBbox.at(2).toDouble(), expectedSourceBounds->width());
    QCOMPARE(sourceBbox.at(3).toDouble(), expectedSourceBounds->height());
    QVERIFY(sourceBbox.at(0).toDouble() != localBounds.x());
    QVERIFY(sourceBbox.at(1).toDouble() != localBounds.y());
}

void TestAcceptanceFlows::testAgentBlobCenterWithoutProvenance() {
    ToolSchema::instance().registerDefaultTools();
    QImage image(120, 100, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (int y = 20; y < 61; ++y)
        for (int x = 30; x < 71; ++x)
            image.setPixelColor(x, y, Qt::white);

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance grab;
    grab.id = QStringLiteral("grab");
    grab.moduleId = QStringLiteral("GrabImage");
    project->addModule(grab);
    ModuleInstance blob;
    blob.id = QStringLiteral("blob");
    blob.moduleId = QStringLiteral("Blob");
    project->addModule(blob);
    ModuleConnection connection;
    connection.fromModuleId = grab.id;
    connection.fromPort = QStringLiteral("image");
    connection.toModuleId = blob.id;
    connection.toPort = QStringLiteral("image");
    connection.edgeType = QStringLiteral("data");
    project->addConnection(connection);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{grab.id, blob.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [image](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("GrabImage"))
            return new UnprovenancedImageFixtureModule(image);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    const ImageData output = engine.moduleOutput(blob.id);
    QVERIFY(!output.hasCoordinateProvenance());
    QCOMPARE(output.data(QStringLiteral("largest_blob_center")).toPointF(), QPointF(50.0, 40.0));

    AgentActor actor;
    const QList<QPair<QString, QString>> blobPixelMetrics{{QStringLiteral("blob_center_x"), QStringLiteral("px")},
                                                          {QStringLiteral("blob_center_y"), QStringLiteral("px")},
                                                          {QStringLiteral("blob_area"), QStringLiteral("px^2")},
                                                          {QStringLiteral("blob_perimeter"), QStringLiteral("px")}};
    for (const auto& metric : blobPixelMetrics) {
        const QJsonObject scalar =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", blob.id}, {"key", metric.first}});
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), metric.second);
        QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), output.data(metric.first).toDouble());
        QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
    }
    for (const QString& key : {QStringLiteral("blob_count"), QStringLiteral("blob_circularity")}) {
        const QJsonObject scalar = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", blob.id}, {"key", key}});
        QVERIFY(!scalar.contains(QStringLiteral("unit")));
    }
    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", blob.id}, {"port", "largest_blob_center"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
    QCOMPARE(response.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const QJsonArray expectedLocalCenter{50.0, 40.0};
    QCOMPARE(response.value(QStringLiteral("value")).toArray(), expectedLocalCenter);
    QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testAgentMeasureCircleMapsCenterThroughCropOrigin() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 140, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const double dx = x - 80.0;
            const double dy = y - 70.0;
            if (dx * dx + dy * dy <= 30.0 * 30.0)
                image.setPixelColor(x, y, Qt::white);
        }
    }
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurecircle.png"))));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurecircle.json", dataDir.path(), *project).isEmpty(),
             "load MeasureCircle project with synthetic image");

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData grabOutput = engine.moduleOutput(QStringLiteral("grab"));
    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    const ImageData measureOutput = engine.moduleOutput(QStringLiteral("measure"));
    QVERIFY(grabOutput.hasCoordinateProvenance());
    QVERIFY(cropOutput.hasCoordinateProvenance());
    QVERIFY(measureOutput.data(QStringLiteral("circle")).canConvert<Circle2D>());
    const ImageCoordinateProvenance provenance = cropOutput.coordinateProvenance();
    const Circle2D circle = measureOutput.data(QStringLiteral("circle")).value<Circle2D>();
    QCOMPARE(circle.centerX, measureOutput.data(QStringLiteral("circle_center_x")).toDouble());
    QCOMPARE(circle.centerY, measureOutput.data(QStringLiteral("circle_center_y")).toDouble());
    QCOMPARE(circle.radius, measureOutput.data(QStringLiteral("circle_radius")).toDouble());
    QCOMPARE(circle.score, measureOutput.data(QStringLiteral("circle_roundness")).toDouble());
    QVERIFY(std::abs(circle.centerX - 60.0) < 5.0);
    QVERIFY(std::abs(circle.centerY - 60.0) < 5.0);

    const QJsonObject circleResponse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", "circle"}});
    QCOMPARE(circleResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QVERIFY(!circleResponse.contains(QStringLiteral("unit")));
    const QJsonObject circlePortResponse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"port", "circle"}});
    QVERIFY(!circlePortResponse.contains(QStringLiteral("unit")));
    const QJsonObject circleJson = circleResponse.value(QStringLiteral("value")).toObject();
    QCOMPARE(circleJson.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QCOMPARE(circleJson.value(QStringLiteral("centerX")).toDouble(), circle.centerX);
    QCOMPARE(circleJson.value(QStringLiteral("centerY")).toDouble(), circle.centerY);
    QCOMPARE(circleJson.value(QStringLiteral("radius")).toDouble(), circle.radius);
    QCOMPARE(circleJson.value(QStringLiteral("score")).toDouble(), circle.score);
    QVERIFY(!circleJson.contains(QStringLiteral("sourceRadius")));
    const auto expectedSourceCenter = provenance.mapLocalToSource(QPointF(circle.centerX, circle.centerY));
    QVERIFY(expectedSourceCenter.has_value());
    const QJsonObject source = circleJson.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), grabOutput.coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QCOMPARE(source.value(QStringLiteral("centerX")).toDouble(), expectedSourceCenter->x());
    QCOMPARE(source.value(QStringLiteral("centerY")).toDouble(), expectedSourceCenter->y());
    QVERIFY(!source.contains(QStringLiteral("radius")));

    for (const QString& key : {QStringLiteral("circle_center_x"), QStringLiteral("circle_center_y"),
                               QStringLiteral("circle_radius"), QStringLiteral("circle_diameter")}) {
        for (bool byPort : {true, false}) {
            QJsonObject args{{"runId", runId}, {"instanceId", "measure"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), measureOutput.data(key).toDouble());
            QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalar.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
    for (const QString& key : {QStringLiteral("circle_roundness"), QStringLiteral("edge_point_count")}) {
        const QJsonObject scalar =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", key}});
        QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), measureOutput.data(key).toDouble());
        QVERIFY(!scalar.contains(QStringLiteral("unit")));
    }

    QFile metadata(QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                   "/../../src/plugins/detection/MeasureCircle/metadata.json"));
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const QJsonObject metadataRoot = QJsonDocument::fromJson(metadata.readAll()).object();
    const QJsonArray outputs =
        metadataRoot.value(QStringLiteral("ports")).toObject().value(QStringLiteral("outputs")).toArray();
    bool circlePortDeclared = false;
    for (const QJsonValue& output : outputs) {
        const QJsonObject port = output.toObject();
        if (port.value(QStringLiteral("id")).toString() == QLatin1String("circle"))
            circlePortDeclared = port.value(QStringLiteral("type")).toString() == QLatin1String("Circle2D");
    }
    QVERIFY(circlePortDeclared);
}

void TestAcceptanceFlows::testAgentColorRecognitionCenterMapsThroughCropOrigin() {
    ToolSchema::instance().registerDefaultTools();
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (int y = 90; y <= 130; ++y) {
        for (int x = 110; x <= 150; ++x)
            image.setPixelColor(x, y, Qt::red);
    }
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QString imagePath = dataDir.filePath(QStringLiteral("color-center.png"));
    QVERIFY(image.save(imagePath));

    const auto makeFlow = [&]() -> Project* {
        if (!ProjectManager::instance().newProject())
            return nullptr;
        Project* project = ProjectManager::instance().currentProject();
        if (!project)
            return nullptr;

        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.name = QStringLiteral("source image");
        grab.params = QJsonObject{{"grabSource", QStringLiteral("File")}, {"filePath", imagePath}};
        project->addModule(grab);

        ModuleInstance crop;
        crop.id = QStringLiteral("crop");
        crop.moduleId = QStringLiteral("CropImage");
        crop.name = QStringLiteral("nonzero-origin crop");
        crop.params =
            QJsonObject{{"rectangles", QJsonArray{QJsonArray{180, 130, 100, 70, 0}}}, {"outputFirstAsImage", true}};
        project->addModule(crop, 1);

        ModuleInstance color;
        color.id = QStringLiteral("color");
        color.moduleId = QStringLiteral("ColorRecognition");
        color.name = QStringLiteral("red center");
        color.params = QJsonObject{{"targetColor", QStringLiteral("红色")}};
        project->addModule(color, 2);

        ModuleConnection connection;
        connection.fromModuleId = grab.id;
        connection.toModuleId = crop.id;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
        connection.fromModuleId = crop.id;
        connection.toModuleId = color.id;
        project->addConnection(connection);

        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = QStringLiteral("main");
        flow.nodeIds = QStringList{grab.id, crop.id, color.id};
        project->setFlows({flow});
        return project;
    };

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    Project* project = makeFlow();
    QVERIFY(project);
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData grabOutput = engine.moduleOutput(QStringLiteral("grab"));
    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    const ImageData colorOutput = engine.moduleOutput(QStringLiteral("color"));
    QVERIFY(grabOutput.hasCoordinateProvenance());
    QVERIFY(cropOutput.hasCoordinateProvenance());
    QCOMPARE(cropOutput.width(), 200);
    QCOMPARE(cropOutput.height(), 140);
    const ImageCoordinateProvenance cropProvenance = cropOutput.coordinateProvenance();
    const auto mappedCropOrigin = cropProvenance.mapLocalToSource(QPointF());
    QVERIFY(mappedCropOrigin.has_value());
    QCOMPARE(*mappedCropOrigin, QPointF(80, 60));
    const QPointF center = colorOutput.data(QStringLiteral("color_center")).toPointF();
    QCOMPARE(center, QPointF(50, 50));
    QCOMPARE(colorOutput.data(QStringLiteral("color_center_x")).toInt(), 50);
    QCOMPARE(colorOutput.data(QStringLiteral("color_center_y")).toInt(), 50);

    const QJsonObject centerResponse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "color"}, {"port", "color_center"}});
    QCOMPARE(centerResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(centerResponse.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
    QCOMPARE(centerResponse.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QCOMPARE(centerResponse.value(QStringLiteral("value")).toArray(), (QJsonArray{50, 50}));
    const auto expectedSourceCenter = cropProvenance.mapLocalToSource(center);
    QVERIFY(expectedSourceCenter.has_value());
    QCOMPARE(*expectedSourceCenter, QPointF(130, 110));
    const QJsonObject source = centerResponse.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), grabOutput.coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QCOMPARE(source.value(QStringLiteral("value")).toArray(), (QJsonArray{130, 110}));

    const QJsonObject imagePort = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "color"}, {"port", "image"}});
    QCOMPARE(imagePort.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QVERIFY(imagePort.value(QStringLiteral("type")).toString() != QStringLiteral("Point2D"));
    QVERIFY(!imagePort.contains(QStringLiteral("sourceImageCoordinates")));

    for (const QString& pagingKey : {QStringLiteral("offset"), QStringLiteral("limit")}) {
        QJsonObject params{{"runId", runId}, {"instanceId", "color"}, {"port", "color_center"}};
        params.insert(pagingKey, 0);
        const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), params);
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!rejected.contains(QStringLiteral("value")));
    }
    for (const QString& key : {QStringLiteral("color_center_x"), QStringLiteral("color_center_y")}) {
        const QJsonObject oldScalar = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "color"}, {"key", key}});
        QCOMPARE(oldScalar.value(QStringLiteral("value")).toDouble(), 50.0);
        QVERIFY(!oldScalar.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!oldScalar.contains(QStringLiteral("sourceImageCoordinates")));
    }

    const auto runFixture = [&](bool unprovenanced, bool horizon, bool nonFinite) -> QString {
        Project* fixtureProject = makeFlow();
        if (!fixtureProject)
            return {};
        ImageCoordinateProvenance sourceProvenance;
        sourceProvenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        sourceProvenance.sourceSize = image.size();
        if (horizon) {
            sourceProvenance.hasLocalToSourceTransform = true;
            sourceProvenance.localToSource = ImageCoordinateProvenance::Matrix3{1, 0, 0, 0, 1, 0, 1, 0, -130};
            if (!sourceProvenance.isValid())
                return {};
        }
        if (!engine.loadProject(fixtureProject, [&, sourceProvenance](const ModuleInstance& instance) -> ModuleBase* {
                if (instance.id == QLatin1String("grab")) {
                    if (unprovenanced)
                        return new UnprovenancedImageFixtureModule(image);
                    return new ImageWithProvenanceFixtureModule(image, sourceProvenance);
                }
                if (instance.id == QLatin1String("color") && nonFinite)
                    return new NonFiniteColorCenterFixtureModule();
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            }))
            return {};
        QString fixtureRunId;
        if (!engine.runOnceAsync(&fixtureRunId) || fixtureRunId.isEmpty())
            return {};
        QElapsedTimer timer;
        timer.start();
        while (!engine.runObservation(fixtureRunId).finished && timer.elapsed() < 10000)
            QTest::qWait(10);
        if (!engine.runObservation(fixtureRunId).finished || !engine.runObservation(fixtureRunId).result.success)
            return {};
        return fixtureRunId;
    };

    const QString noSourceRunId = runFixture(true, false, false);
    QVERIFY(!noSourceRunId.isEmpty());
    const QJsonObject noSource =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", noSourceRunId}, {"instanceId", "color"}, {"port", "color_center"}});
    QCOMPARE(noSource.value(QStringLiteral("value")).toArray(), (QJsonArray{50, 50}));
    QVERIFY(!noSource.contains(QStringLiteral("sourceImageCoordinates")));

    const QString horizonRunId = runFixture(false, true, false);
    QVERIFY(!horizonRunId.isEmpty());
    const ImageData horizonCrop = engine.moduleOutput(QStringLiteral("crop"));
    QVERIFY(horizonCrop.coordinateProvenance().isValid());
    QVERIFY(!horizonCrop.coordinateProvenance().mapLocalToSource(QPointF(50, 50)).has_value());
    const QJsonObject horizonResponse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", horizonRunId}, {"instanceId", "color"}, {"port", "color_center"}});
    QCOMPARE(horizonResponse.value(QStringLiteral("value")).toArray(), (QJsonArray{50, 50}));
    QVERIFY(!horizonResponse.contains(QStringLiteral("sourceImageCoordinates")));

    const QString nonFiniteRunId = runFixture(true, false, true);
    QVERIFY(!nonFiniteRunId.isEmpty());
    const QJsonObject nonFinite =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", nonFiniteRunId}, {"instanceId", "color"}, {"port", "color_center"}});
    QCOMPARE(nonFinite.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
    QVERIFY(!nonFinite.contains(QStringLiteral("value")));

    const QString fixtureDir = m_tempDir.filePath(QStringLiteral("plugins/Point2DFixture"));
    QVERIFY(QDir().mkpath(fixtureDir));
    QFile fixtureMetadata(QDir(fixtureDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(fixtureMetadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray metadata =
        R"({"id":"com.deeplux.tests.point2dfixture","name":"Point2DFixture","category":"test","version":"1.0","ports":{"inputs":[],"outputs":[{"id":"point","displayName":"Point","type":"Point2D","required":false,"multiple":false},{"id":"point1","displayName":"Point 1","type":"Point2D","required":false,"multiple":false},{"id":"foot_point","displayName":"Foot point","type":"Point2D","required":false,"multiple":false}]}})";
    QCOMPARE(fixtureMetadata.write(metadata), static_cast<qint64>(metadata.size()));
    fixtureMetadata.close();
    PluginManager::instance().scanPlugins();
    const QList<PortSpec> pointFixturePorts =
        PluginManager::instance().pluginInfo(QStringLiteral("Point2DFixture")).outputPorts;
    QCOMPARE(pointFixturePorts.size(), 3);
    QVERIFY(std::any_of(pointFixturePorts.cbegin(), pointFixturePorts.cend(), [](const PortSpec& spec) {
        return spec.id == QLatin1String("point1") && spec.type == DataType::Point2D;
    }));
    QVERIFY(std::any_of(pointFixturePorts.cbegin(), pointFixturePorts.cend(), [](const PortSpec& spec) {
        return spec.id == QLatin1String("foot_point") && spec.type == DataType::Point2D;
    }));

    QVERIFY(ProjectManager::instance().newProject());
    project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance pointFixture;
    pointFixture.id = QStringLiteral("point_fixture");
    pointFixture.moduleId = QStringLiteral("Point2DFixture");
    pointFixture.name = QStringLiteral("generic Point2D output");
    project->addModule(pointFixture);
    ProjectFlow pointFlow;
    pointFlow.id = QStringLiteral("main");
    pointFlow.name = QStringLiteral("main");
    pointFlow.nodeIds = QStringList{pointFixture.id};
    project->setFlows({pointFlow});
    QVERIFY(engine.loadProject(project,
                               [](const ModuleInstance&) -> ModuleBase* { return new Point2DOutputFixtureModule(); }));
    QString pointRunId;
    QVERIFY(engine.runOnceAsync(&pointRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(pointRunId).finished, 10000);
    QVERIFY(engine.runObservation(pointRunId).result.success);
    const QJsonObject genericPoint =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", pointRunId}, {"instanceId", "point_fixture"}, {"port", "point"}});
    QCOMPARE(genericPoint.value(QStringLiteral("value")).toArray(), (QJsonArray{3, 4}));
    QVERIFY(!genericPoint.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!genericPoint.contains(QStringLiteral("sourceImageCoordinates")));
    const QJsonObject nonTargetPoint1 =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", pointRunId}, {"instanceId", "point_fixture"}, {"port", "point1"}});
    QCOMPARE(nonTargetPoint1.value(QStringLiteral("value")).toArray(), (QJsonArray{3, 4}));
    QVERIFY(!nonTargetPoint1.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!nonTargetPoint1.contains(QStringLiteral("sourceImageCoordinates")));
    const QJsonObject nonTargetFootPoint =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", pointRunId}, {"instanceId", "point_fixture"}, {"port", "foot_point"}});
    QCOMPARE(nonTargetFootPoint.value(QStringLiteral("value")).toArray(), (QJsonArray{3, 4}));
    QVERIFY(!nonTargetFootPoint.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!nonTargetFootPoint.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testAgentJiErHanDefectRegionMapsThroughCropOrigin() {
    ToolSchema::instance().registerDefaultTools();
    QImage image(400, 300, QImage::Format_RGB32);
    image.fill(QColor(200, 200, 200));
    for (int y = 80; y <= 139; ++y) {
        for (int x = 100; x <= 159; ++x)
            image.setPixelColor(x, y, QColor(20, 20, 20));
    }
    for (int y = 180; y <= 197; ++y) {
        for (int x = 250; x <= 267; ++x)
            image.setPixelColor(x, y, QColor(20, 20, 20));
    }
    QImage weakOnlyImage(400, 300, QImage::Format_RGB32);
    weakOnlyImage.fill(QColor(200, 200, 200));
    for (int y = 180; y <= 197; ++y) {
        for (int x = 250; x <= 267; ++x)
            weakOnlyImage.setPixelColor(x, y, QColor(20, 20, 20));
    }
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QString imagePath = dataDir.filePath(QStringLiteral("jierhan-defect.png"));
    QVERIFY(image.save(imagePath));
    const QString weakOnlyImagePath = dataDir.filePath(QStringLiteral("jierhan-weak-only.png"));
    QVERIFY(weakOnlyImage.save(weakOnlyImagePath));

    const auto makeFlow = [&](const QString& inputPath, double initialThreshold) -> Project* {
        if (!ProjectManager::instance().newProject())
            return nullptr;
        Project* project = ProjectManager::instance().currentProject();
        if (!project)
            return nullptr;

        ModuleInstance grab;
        grab.id = QStringLiteral("grab");
        grab.moduleId = QStringLiteral("GrabImage");
        grab.name = QStringLiteral("source image");
        grab.params = QJsonObject{{"grabSource", QStringLiteral("File")}, {"filePath", inputPath}};
        project->addModule(grab);

        ModuleInstance crop;
        crop.id = QStringLiteral("crop");
        crop.moduleId = QStringLiteral("CropImage");
        crop.name = QStringLiteral("nonzero-origin crop");
        crop.params =
            QJsonObject{{"rectangles", QJsonArray{QJsonArray{200, 150, 160, 120, 0}}}, {"outputFirstAsImage", true}};
        project->addModule(crop, 1);

        ModuleInstance defect;
        defect.id = QStringLiteral("defect");
        defect.moduleId = QStringLiteral("JiErHanDefectsDet");
        defect.name = QStringLiteral("heuristic defect candidate");
        defect.params = QJsonObject{{"threshold", initialThreshold}};
        project->addModule(defect, 2);

        ModuleConnection connection;
        connection.fromModuleId = grab.id;
        connection.toModuleId = crop.id;
        connection.fromPort = QStringLiteral("image");
        connection.toPort = QStringLiteral("image");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
        connection.fromModuleId = crop.id;
        connection.toModuleId = defect.id;
        project->addConnection(connection);

        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = QStringLiteral("main");
        flow.nodeIds = QStringList{grab.id, crop.id, defect.id};
        project->setFlows({flow});
        return project;
    };

    const PluginInfo info = PluginManager::instance().pluginInfo(QStringLiteral("JiErHanDefectsDet"));
    bool regionPortDeclared = false;
    for (const PortSpec& port : info.outputPorts) {
        if (port.id == QLatin1String("defect_region"))
            regionPortDeclared = port.type == DataType::Region2D;
    }
    QVERIFY(regionPortDeclared);

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    Project* project = makeFlow(imagePath, 0.9);
    QVERIFY(project);
    const QJsonObject rejectedThreshold =
        actor.executeTool(QStringLiteral("set_module_params"),
                          QJsonObject{{"instanceId", "defect"}, {"values", QJsonObject{{"threshold", 1.5}}}});
    QCOMPARE(rejectedThreshold.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
    QCOMPARE(rejectedThreshold.value(QStringLiteral("error")).toString(), QStringLiteral("阈值必须在 0.1 到 1.0 之间"));
    const auto unchangedDefect = project->moduleById(QStringLiteral("defect"));
    QVERIFY(unchangedDefect.has_value());
    QCOMPARE(unchangedDefect->params.value(QStringLiteral("threshold")).toDouble(), 0.9);
    QVERIFY(!engine.isBusy());

    const QJsonObject lowThreshold =
        actor.executeTool(QStringLiteral("set_module_params"),
                          QJsonObject{{"instanceId", "defect"}, {"values", QJsonObject{{"threshold", 0.1}}}});
    QCOMPARE(lowThreshold.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
    const auto lowThresholdDefect = project->moduleById(QStringLiteral("defect"));
    QVERIFY(lowThresholdDefect.has_value());
    QCOMPARE(lowThresholdDefect->params.value(QStringLiteral("threshold")).toDouble(), 0.1);

    const QJsonObject lowStarted = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString lowRunId = lowStarted.value(QStringLiteral("runId")).toString();
    QVERIFY2(!lowRunId.isEmpty(), qPrintable(lowStarted.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(lowRunId).finished, 10000);
    QVERIFY2(engine.runObservation(lowRunId).result.success,
             qPrintable(engine.runObservation(lowRunId).result.errorMessage));
    const QJsonObject lowCount =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", lowRunId}, {"instanceId", "defect"}, {"key", "defect_count"}});
    QCOMPARE(lowCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(lowCount.value(QStringLiteral("value")).toInt(), 2);

    const QJsonObject highThreshold =
        actor.executeTool(QStringLiteral("set_module_params"),
                          QJsonObject{{"instanceId", "defect"}, {"values", QJsonObject{{"threshold", 0.9}}}});
    QCOMPARE(highThreshold.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
    const auto highThresholdDefect = project->moduleById(QStringLiteral("defect"));
    QVERIFY(highThresholdDefect.has_value());
    QCOMPARE(highThresholdDefect->params.value(QStringLiteral("threshold")).toDouble(), 0.9);

    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    const QJsonObject highCount =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "defect"}, {"key", "defect_count"}});
    QCOMPARE(highCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(highCount.value(QStringLiteral("value")).toInt(), 1);

    const ImageData grabOutput = engine.moduleOutput(QStringLiteral("grab"));
    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    const ImageData defectOutput = engine.moduleOutput(QStringLiteral("defect"));
    QVERIFY(grabOutput.hasCoordinateProvenance());
    QVERIFY(cropOutput.hasCoordinateProvenance());
    QCOMPARE(cropOutput.width(), 320);
    QCOMPARE(cropOutput.height(), 240);
    QCOMPARE(defectOutput.data(QStringLiteral("defect_count")).toInt(), 1);
    const ImageCoordinateProvenance provenance = cropOutput.coordinateProvenance();
    const auto mappedCropOrigin = provenance.mapLocalToSource(QPointF());
    QVERIFY(mappedCropOrigin.has_value());
    QCOMPARE(*mappedCropOrigin, QPointF(40, 30));

    QVERIFY(defectOutput.data(QStringLiteral("defect_region")).canConvert<Region2D>());
    const Region2D region = defectOutput.data(QStringLiteral("defect_region")).value<Region2D>();
    QCOMPARE(region.label, QStringLiteral("焊接缺陷"));
    QCOMPARE(region.bbox, QRectF(59, 49, 61, 61));
    QCOMPARE(region.score, 1.0);
    QVERIFY(region.polygon.isEmpty());
    QCOMPARE(defectOutput.data(QStringLiteral("defect_x")).toInt(), 59);
    QCOMPARE(defectOutput.data(QStringLiteral("defect_y")).toInt(), 49);
    QCOMPARE(defectOutput.data(QStringLiteral("defect_width")).toInt(), 61);
    QCOMPARE(defectOutput.data(QStringLiteral("defect_height")).toInt(), 61);
    QCOMPARE(defectOutput.data(QStringLiteral("defect_confidence")).toDouble(), region.score);

    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "defect"}, {"port", "defect_region"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("source")).toString(), QStringLiteral("port"));
    QCOMPARE(response.value(QStringLiteral("port")).toString(), QStringLiteral("defect_region"));
    const QJsonObject local = response.value(QStringLiteral("value")).toObject();
    QCOMPARE(local.value(QStringLiteral("label")).toString(), QStringLiteral("焊接缺陷"));
    QCOMPARE(local.value(QStringLiteral("bbox")).toArray(), (QJsonArray{59, 49, 61, 61}));
    QCOMPARE(local.value(QStringLiteral("score")).toDouble(), 1.0);
    QCOMPARE(local.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const QRectF localBounds(59, 49, 61, 61);
    const auto mappedBounds = provenance.mapRectToSourceBounds(localBounds);
    QVERIFY(mappedBounds.has_value());
    QCOMPARE(*mappedBounds, QRectF(99, 79, 61, 61));
    const QJsonObject source = local.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), grabOutput.coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QCOMPARE(source.value(QStringLiteral("bbox")).toArray(), (QJsonArray{99, 79, 61, 61}));

    const auto runUnprovenanced = [&]() -> QString {
        Project* fixtureProject = makeFlow(imagePath, 0.9);
        if (!fixtureProject ||
            !engine.loadProject(fixtureProject, [this, &image](const ModuleInstance& instance) -> ModuleBase* {
                if (instance.id == QLatin1String("grab"))
                    return new UnprovenancedImageFixtureModule(image);
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            }))
            return {};
        QString fixtureRunId;
        if (!engine.runOnceAsync(&fixtureRunId) || fixtureRunId.isEmpty())
            return {};
        QElapsedTimer timer;
        timer.start();
        while (!engine.runObservation(fixtureRunId).finished && timer.elapsed() < 10000)
            QTest::qWait(10);
        if (!engine.runObservation(fixtureRunId).finished || !engine.runObservation(fixtureRunId).result.success)
            return {};
        return fixtureRunId;
    };
    const QString noSourceRunId = runUnprovenanced();
    QVERIFY(!noSourceRunId.isEmpty());
    const QJsonObject noSource =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", noSourceRunId}, {"instanceId", "defect"}, {"port", "defect_region"}});
    const QJsonObject localOnly = noSource.value(QStringLiteral("value")).toObject();
    QCOMPARE(localOnly.value(QStringLiteral("bbox")).toArray(), (QJsonArray{59, 49, 61, 61}));
    QCOMPARE(localOnly.value(QStringLiteral("score")).toDouble(), 1.0);
    QCOMPARE(localOnly.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QVERIFY(!localOnly.contains(QStringLiteral("sourceImageCoordinates")));

    Project* weakOnlyProject = makeFlow(weakOnlyImagePath, 0.5);
    QVERIFY(weakOnlyProject);
    const QJsonObject weakThreshold =
        actor.executeTool(QStringLiteral("set_module_params"),
                          QJsonObject{{"instanceId", "defect"}, {"values", QJsonObject{{"threshold", 0.9}}}});
    QCOMPARE(weakThreshold.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
    const QJsonObject weakStarted = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString weakRunId = weakStarted.value(QStringLiteral("runId")).toString();
    QVERIFY2(!weakRunId.isEmpty(), qPrintable(weakStarted.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(weakRunId).finished, 10000);
    const RunObservation weakRun = engine.runObservation(weakRunId);
    QVERIFY(!weakRun.result.success);
    QCOMPARE(weakRun.result.failedModule, QStringLiteral("defect"));
    QCOMPARE(weakRun.result.errorMessage, QStringLiteral("未检测到缺陷"));
    QCOMPARE(actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{"runId", weakRunId}})
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("failed"));
    const QJsonObject unavailable =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", weakRunId}, {"instanceId", "defect"}, {"port", "defect_region"}});
    QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
}

void TestAcceptanceFlows::testAgentMatchingScalarsDeclareLocalPixelUnits() {
    ToolSchema::instance().registerDefaultTools();
    QImage image(240, 180, QImage::Format_RGB32);
    image.fill(QColor(30, 30, 30));
    {
        QPainter painter(&image);
        painter.fillRect(QRect(100, 70, 40, 40), Qt::white);
        painter.fillRect(QRect(115, 85, 10, 10), Qt::black);
    }
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
    provenance.sourceSize = QSize(640, 480);
    const ImageCoordinateProvenance::Matrix3 matrix{{1.5, 0.0, 13.0, 0.0, 2.0, 17.0, 0.0, 0.0, 1.0}};
    QVERIFY(provenance.composeLocalToSource(matrix));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance grab;
    grab.id = QStringLiteral("grab");
    grab.moduleId = QStringLiteral("GrabImage");
    grab.name = QStringLiteral("fixture image with provenance");
    project->addModule(grab);
    ModuleInstance crop;
    crop.id = QStringLiteral("crop");
    crop.moduleId = QStringLiteral("CropImage");
    crop.name = QStringLiteral("nonzero-origin crop");
    crop.params = QJsonObject{{"rectangles", QJsonArray{QJsonArray{120, 90, 80, 60, 0}}}, {"outputFirstAsImage", true}};
    project->addModule(crop, 1);
    ModuleInstance matching;
    matching.id = QStringLiteral("matching");
    matching.moduleId = QStringLiteral("Matching");
    matching.name = QStringLiteral("default center template match");
    matching.params = QJsonObject{{"matchThreshold", 0.9}, {"maxMatches", 2}};
    project->addModule(matching, 2);
    ModuleConnection connection;
    connection.fromModuleId = grab.id;
    connection.toModuleId = crop.id;
    connection.fromPort = QStringLiteral("image");
    connection.toPort = QStringLiteral("image");
    connection.edgeType = QStringLiteral("data");
    project->addConnection(connection);
    connection.fromModuleId = crop.id;
    connection.toModuleId = matching.id;
    project->addConnection(connection);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{grab.id, crop.id, matching.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    const auto loadMatchingFixture = [&engine, &image, &provenance](Project* fixtureProject) {
        return engine.loadProject(fixtureProject, [&image, &provenance](const ModuleInstance& instance) -> ModuleBase* {
            if (instance.id == QLatin1String("grab"))
                return new ImageWithProvenanceFixtureModule(image, provenance);
            return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
        });
    };
    QVERIFY(loadMatchingFixture(project));
    AgentActor actor;

    const QJsonObject rejectedParams = actor.executeTool(
        QStringLiteral("set_module_params"),
        QJsonObject{{"instanceId", QStringLiteral("matching")}, {"values", QJsonObject{{"matchThreshold", 1.1}}}});
    QCOMPARE(rejectedParams.value(QStringLiteral("errorCode")).toString(), QStringLiteral("VALIDATION_FAILED"));
    QCOMPARE(rejectedParams.value(QStringLiteral("error")).toString(), QStringLiteral("匹配阈值必须在0到1之间"));
    const auto unchangedMatching = project->moduleById(QStringLiteral("matching"));
    QVERIFY(unchangedMatching.has_value());
    QCOMPARE(unchangedMatching->params.value(QStringLiteral("matchThreshold")).toDouble(), 0.9);
    QCOMPARE(unchangedMatching->params.value(QStringLiteral("maxMatches")).toInt(), 2);
    QVERIFY(!engine.isBusy());

    const QJsonObject configured =
        actor.executeTool(QStringLiteral("set_module_params"),
                          QJsonObject{{"instanceId", QStringLiteral("matching")},
                                      {"values", QJsonObject{{"matchThreshold", 0.8}, {"maxMatches", 5}}}});
    QCOMPARE(configured.value(QStringLiteral("status")).toString(), QStringLiteral("params_set"));
    const auto configuredMatching = project->moduleById(QStringLiteral("matching"));
    QVERIFY(configuredMatching.has_value());
    QCOMPARE(configuredMatching->params.value(QStringLiteral("matchThreshold")).toDouble(), 0.8);
    QCOMPARE(configuredMatching->params.value(QStringLiteral("maxMatches")).toInt(), 5);
    QVERIFY(loadMatchingFixture(project));

    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    QCOMPARE(actor.executeTool(QStringLiteral("get_run_results"), QJsonObject{{"runId", runId}})
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("completed"));

    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    const ImageData matchingOutput = engine.moduleOutput(QStringLiteral("matching"));
    QVERIFY(cropOutput.hasCoordinateProvenance());
    QCOMPARE(cropOutput.width(), 160);
    QCOMPARE(cropOutput.height(), 120);
    QCOMPARE(cropOutput.coordinateProvenance().mapLocalToSource(QPointF()).value(),
             provenance.mapLocalToSource(QPointF(40, 30)).value());
    QVERIFY(matchingOutput.data(QStringLiteral("detections")).canConvert<DetectionList>());
    const DetectionList detections = matchingOutput.data(QStringLiteral("detections")).value<DetectionList>();
    QVERIFY(!detections.isEmpty());
    const Detection& first = detections.items.first();
    const QMap<QString, double> values{{QStringLiteral("match_x"), 65.0},
                                       {QStringLiteral("match_y"), 45.0},
                                       {QStringLiteral("match_width"), 30.0},
                                       {QStringLiteral("match_height"), 30.0}};
    QCOMPARE(matchingOutput.data(QStringLiteral("match_score")).toDouble(), 1.0);
    QCOMPARE(first.x, values.value(QStringLiteral("match_x")));
    QCOMPARE(first.y, values.value(QStringLiteral("match_y")));
    QCOMPARE(first.width, values.value(QStringLiteral("match_width")));
    QCOMPARE(first.height, values.value(QStringLiteral("match_height")));
    QCOMPARE(first.score, 1.0);

    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        for (bool byPort : {true, false}) {
            QJsonObject args{{"runId", runId}, {"instanceId", "matching"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), it.key());
            const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
            QVERIFY2(response.value(QStringLiteral("status")).toString() == QLatin1String("available"),
                     qPrintable(QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))));
            QCOMPARE(response.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QVERIFY(std::abs(response.value(QStringLiteral("value")).toDouble() - it.value()) <= 1e-9);
            QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }

    for (const QString& key :
         {QStringLiteral("match_score"), QStringLiteral("match_count"), QStringLiteral("match_threshold")}) {
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "matching"}, {"port", key}});
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QVERIFY(response.value(QStringLiteral("value")).isDouble());
        QVERIFY(std::abs(response.value(QStringLiteral("value")).toDouble() - matchingOutput.data(key).toDouble()) <=
                1e-9);
        QVERIFY(!response.contains(QStringLiteral("unit")));
    }

    const QJsonObject detectionResponse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "matching"}, {"port", "detections"}});
    QCOMPARE(detectionResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonArray detectionItems = detectionResponse.value(QStringLiteral("value")).toArray();
    QVERIFY(!detectionItems.isEmpty());
    const QJsonObject firstJson = detectionItems.first().toObject();
    QCOMPARE(firstJson.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const auto expectedSource =
        cropOutput.coordinateProvenance().mapRectToSourceBounds(QRectF(first.x, first.y, first.width, first.height));
    QVERIFY(expectedSource.has_value());
    const QJsonObject source = firstJson.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    const QJsonArray sourceBbox = source.value(QStringLiteral("bbox")).toArray();
    QCOMPARE(sourceBbox.size(), 4);
    QCOMPARE(sourceBbox, (QJsonArray{170.5, 167, 45, 60}));
    QVERIFY(std::abs(sourceBbox.at(0).toDouble() - expectedSource->x()) <= 1e-9);
    QVERIFY(std::abs(sourceBbox.at(1).toDouble() - expectedSource->y()) <= 1e-9);
    QVERIFY(std::abs(sourceBbox.at(2).toDouble() - expectedSource->width()) <= 1e-9);
    QVERIFY(std::abs(sourceBbox.at(3).toDouble() - expectedSource->height()) <= 1e-9);
}

void TestAcceptanceFlows::testAgentShowPointOutputsDeclareCurrentImagePixelUnits() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);

    const QString sourceId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
    const QSize sourceSize(220, 160);
    const QPointF localOrigin(10.0, 20.0);
    const QPointF point(13.25, 22.5);
    QImage image(32, 24, QImage::Format_RGB32);
    image.fill(Qt::white);
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = sourceId;
    provenance.sourceSize = sourceSize;
    provenance.localOriginInSource = localOrigin;
    QVERIFY(provenance.isValid());

    ModuleInstance grab;
    grab.id = QStringLiteral("grab");
    grab.moduleId = QStringLiteral("GrabImage");
    project->addModule(grab);
    ModuleInstance show;
    show.id = QStringLiteral("show");
    show.moduleId = QStringLiteral("ShowPoint");
    project->addModule(show, 1);

    ModuleConnection connection;
    connection.fromModuleId = grab.id;
    connection.fromPort = QStringLiteral("image");
    connection.toModuleId = show.id;
    connection.toPort = QStringLiteral("image");
    connection.edgeType = QStringLiteral("data");
    project->addConnection(connection);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{grab.id, show.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [this, &image, &provenance, &point](const ModuleInstance& instance) {
        if (instance.id == QLatin1String("grab"))
            return static_cast<ModuleBase*>(
                new ImageWithProvenanceFixtureModule(image, provenance, QVariant::fromValue(point)));
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData output = engine.moduleOutput(show.id);
    QCOMPARE(output.data(QStringLiteral("point_x")).toDouble(), point.x());
    QCOMPARE(output.data(QStringLiteral("point_y")).toDouble(), point.y());
    QVERIFY(output.data(QStringLiteral("point_displayed")).toBool());
    const ImageCoordinateProvenance outputProvenance = output.coordinateProvenance();
    QCOMPARE(outputProvenance.sourceImageId, sourceId);
    QCOMPARE(outputProvenance.sourceSize, sourceSize);
    QCOMPARE(outputProvenance.localOriginInSource, localOrigin);
    QCOMPARE(outputProvenance.hasLocalToSourceTransform, provenance.hasLocalToSourceTransform);
    for (size_t i = 0; i < provenance.localToSource.size(); ++i)
        QCOMPARE(outputProvenance.localToSource[i], provenance.localToSource[i]);

    AgentActor actor;
    for (const QString& key : {QStringLiteral("point_x"), QStringLiteral("point_y")}) {
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                                               {QStringLiteral("instanceId"), show.id},
                                                                               {QStringLiteral("key"), key}});
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(response.value(QStringLiteral("value")).toDouble(), output.data(key).toDouble());
        QCOMPARE(response.value(QStringLiteral("value")).toDouble(),
                 key == QLatin1String("point_x") ? point.x() : point.y());
        QCOMPARE(response.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    }
    const QJsonObject displayed = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                         {QStringLiteral("instanceId"), show.id},
                                                         {QStringLiteral("key"), QStringLiteral("point_displayed")}});
    QCOMPARE(displayed.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(displayed.value(QStringLiteral("value")).toBool(),
             output.data(QStringLiteral("point_displayed")).toBool());
    QVERIFY(!displayed.contains(QStringLiteral("unit")));
    QVERIFY(!displayed.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!displayed.contains(QStringLiteral("coordinateFrame")));
    QVERIFY(!displayed.contains(QStringLiteral("sourceImageCoordinates")));

    engine.clearModules();
    ProjectManager::instance().closeProject();
    project = ProjectManager::instance().newProject();
    QVERIFY(project);
    project->addModule(grab);
    project->addModule(show, 1);
    project->addConnection(connection);
    project->setFlows({flow});
    QVERIFY(engine.loadProject(project, [this, &image, &provenance](const ModuleInstance& instance) {
        if (instance.id == QLatin1String("grab"))
            return static_cast<ModuleBase*>(new ImageWithProvenanceFixtureModule(image, provenance));
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString failedRunId;
    QVERIFY(engine.runOnceAsync(&failedRunId));
    QVERIFY(!failedRunId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(failedRunId).finished, 10000);
    const RunObservation failedRun = engine.runObservation(failedRunId);
    QVERIFY(!failedRun.result.success);
    QCOMPARE(failedRun.result.failedModule, show.id);
    QVERIFY(failedRun.result.errorMessage.contains(QStringLiteral("未提供点坐标")));
    QCOMPARE(actor.executeTool(QStringLiteral("get_run_results"), {{"runId", failedRunId}})
                 .value(QStringLiteral("status"))
                 .toString(),
             QStringLiteral("failed"));
    for (const QString& key : {QStringLiteral("point_x"), QStringLiteral("point_y")}) {
        const QJsonObject unavailable = actor.executeTool(
            QStringLiteral("get_module_output"), {{"runId", failedRunId}, {"instanceId", show.id}, {"key", key}});
        QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(unavailable.value(QStringLiteral("reason")).toString(), QStringLiteral("run_failed"));
    }
}

void TestAcceptanceFlows::testAgentMeasureLineMapsEndpointsThroughCropOrigin() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(180, 140, QImage::Format_RGB32);
    image.fill(Qt::black);
    for (int y = 68; y <= 72; ++y)
        for (int x = 30; x <= 150; ++x)
            image.setPixelColor(x, y, Qt::white);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measureline.png"))));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measureline.json", dataDir.path(), *project).isEmpty(),
             "load MeasureLine project with synthetic image");

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData grabOutput = engine.moduleOutput(QStringLiteral("grab"));
    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    const ImageData lineOutput = engine.moduleOutput(QStringLiteral("measure"));
    QVERIFY(grabOutput.hasCoordinateProvenance());
    QVERIFY(cropOutput.hasCoordinateProvenance());
    const QVariantList coordinates = lineOutput.data(QStringLiteral("line")).toList();
    QCOMPARE(coordinates.size(), 4);
    QCOMPARE(coordinates.at(0).toDouble(), lineOutput.data(QStringLiteral("line_col1")).toDouble());
    QCOMPARE(coordinates.at(1).toDouble(), lineOutput.data(QStringLiteral("line_row1")).toDouble());
    QCOMPARE(coordinates.at(2).toDouble(), lineOutput.data(QStringLiteral("line_col2")).toDouble());
    QCOMPARE(coordinates.at(3).toDouble(), lineOutput.data(QStringLiteral("line_row2")).toDouble());
    const double dx = coordinates.at(2).toDouble() - coordinates.at(0).toDouble();
    const double dy = coordinates.at(3).toDouble() - coordinates.at(1).toDouble();
    const double expectedLength = std::hypot(dx, dy);
    double expectedAngle = std::atan2(dy, dx) * 180.0 / std::acos(-1.0);
    if (expectedAngle < 0.0)
        expectedAngle += 180.0;
    QVERIFY(expectedAngle >= 0.0 && expectedAngle < 180.0);
    QVERIFY(std::abs(lineOutput.data(QStringLiteral("line_length")).toDouble() - expectedLength) <= 1e-9);
    QVERIFY(std::abs(lineOutput.data(QStringLiteral("line_angle")).toDouble() - expectedAngle) <= 1e-9);

    const QJsonObject response = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", "line"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Line2D"));
    const QJsonObject line = response.value(QStringLiteral("value")).toObject();
    QCOMPARE(line.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const QJsonArray localStart = line.value(QStringLiteral("start")).toArray();
    const QJsonArray localEnd = line.value(QStringLiteral("end")).toArray();
    QCOMPARE(localStart, QJsonArray({coordinates.at(0).toDouble(), coordinates.at(1).toDouble()}));
    QCOMPARE(localEnd, QJsonArray({coordinates.at(2).toDouble(), coordinates.at(3).toDouble()}));
    const ImageCoordinateProvenance provenance = cropOutput.coordinateProvenance();
    const auto expectedStart =
        provenance.mapLocalToSource(QPointF(coordinates.at(0).toDouble(), coordinates.at(1).toDouble()));
    const auto expectedEnd =
        provenance.mapLocalToSource(QPointF(coordinates.at(2).toDouble(), coordinates.at(3).toDouble()));
    QVERIFY(expectedStart.has_value());
    QVERIFY(expectedEnd.has_value());
    const QJsonObject source = line.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), grabOutput.coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    const QJsonArray sourceStart = source.value(QStringLiteral("start")).toArray();
    const QJsonArray sourceEnd = source.value(QStringLiteral("end")).toArray();
    QCOMPARE(sourceStart, QJsonArray({expectedStart->x(), expectedStart->y()}));
    QCOMPARE(sourceEnd, QJsonArray({expectedEnd->x(), expectedEnd->y()}));

    for (const auto& measurement : {qMakePair(QStringLiteral("line_length"), QStringLiteral("px")),
                                    qMakePair(QStringLiteral("line_angle"), QStringLiteral("deg"))}) {
        for (bool byPort : {true, false}) {
            QJsonObject args{{"runId", runId}, {"instanceId", "measure"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), measurement.first);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), measurement.second);
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), lineOutput.data(measurement.first).toDouble());
            QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
    const QJsonObject lineCount =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", "line_count"}});
    QCOMPARE(lineCount.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(lineCount.value(QStringLiteral("value")).toDouble(),
             lineOutput.data(QStringLiteral("line_count")).toDouble());
    QVERIFY(!lineCount.contains(QStringLiteral("unit")));

    const QFileInfo rootInfo(QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                             "/../../src/plugins/detection/MeasureLine/metadata.json"));
    QFile metadata(rootInfo.absoluteFilePath());
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const QJsonObject metadataRoot = QJsonDocument::fromJson(metadata.readAll()).object();
    const QJsonArray outputs =
        metadataRoot.value(QStringLiteral("ports")).toObject().value(QStringLiteral("outputs")).toArray();
    bool linePortDeclared = false;
    for (const QJsonValue& output : outputs) {
        const QJsonObject port = output.toObject();
        if (port.value(QStringLiteral("id")).toString() == QLatin1String("line"))
            linePortDeclared = port.value(QStringLiteral("type")).toString() == QLatin1String("Line2D");
    }
    QVERIFY(linePortDeclared);
}

void TestAcceptanceFlows::testAgentMeasurementInputPointSetCarriesSourceCoordinates() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty(),
             "load MeasurementInput point-set project with synthetic image");

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData inputOutput = engine.moduleOutput(QStringLiteral("input"));
    QVERIFY(inputOutput.hasCoordinateProvenance());
    QCOMPARE(inputOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    const QVector<QPointF> localPoints = inputOutput.data(QStringLiteral("fit_points")).value<QVector<QPointF>>();
    QCOMPARE(localPoints, QVector<QPointF>({QPointF(1.25, 2.5), QPointF(7, 8), QPointF(11.5, 13.75)}));
    const QVariantList pluginSourceValues = inputOutput.data(QStringLiteral("fit_points_source")).toList();
    QCOMPARE(pluginSourceValues.size(), localPoints.size());

    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"port", "fit_points"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("PointSet2D"));
    QCOMPARE(response.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const QJsonArray localPage = response.value(QStringLiteral("value")).toArray();
    QCOMPARE(localPage.size(), localPoints.size());
    const QJsonObject source = response.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(),
             engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    const QJsonArray sourcePage = source.value(QStringLiteral("points")).toArray();
    QCOMPARE(sourcePage.size(), localPage.size());
    const ImageCoordinateProvenance provenance = inputOutput.coordinateProvenance();
    for (int i = 0; i < localPoints.size(); ++i) {
        const QJsonArray local = localPage.at(i).toArray();
        QCOMPARE(local, QJsonArray({localPoints.at(i).x(), localPoints.at(i).y()}));
        const auto mapped = provenance.mapLocalToSource(localPoints.at(i));
        QVERIFY(mapped.has_value());
        QCOMPARE(sourcePage.at(i).toArray(), QJsonArray({mapped->x(), mapped->y()}));
        const QVariantList pluginSource = pluginSourceValues.at(i).toList();
        QCOMPARE(sourcePage.at(i).toArray(),
                 QJsonArray({pluginSource.at(0).toDouble(), pluginSource.at(1).toDouble()}));
    }

    const QJsonObject paged = actor.executeTool(
        QStringLiteral("get_module_output"),
        QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"port", "fit_points"}, {"offset", 1}, {"limit", 1}});
    QCOMPARE(paged.value(QStringLiteral("totalRows")).toInt(), 3);
    QCOMPARE(paged.value(QStringLiteral("offset")).toInt(), 1);
    QCOMPARE(paged.value(QStringLiteral("limit")).toInt(), 1);
    QCOMPARE(paged.value(QStringLiteral("truncated")).toBool(), true);
    QCOMPARE(paged.value(QStringLiteral("value")).toArray(), QJsonArray{localPage.at(1)});
    QCOMPARE(paged.value(QStringLiteral("sourceImageCoordinates")).toObject().value(QStringLiteral("points")).toArray(),
             QJsonArray{sourcePage.at(1)});

    const QJsonObject namedValue =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"key", "fit_points"}});
    QVERIFY(namedValue.value(QStringLiteral("type")).toString() != QStringLiteral("PointSet2D"));
    QVERIFY(!namedValue.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!namedValue.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testAgentMeasurementInputPointSetFailsClosedForMissingAndHorizonSources() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const auto runWithProvenance = [&](const ImageCoordinateProvenance& provenance) {
        if (!ProjectManager::instance().newProject())
            return QString();
        Project* project = ProjectManager::instance().currentProject();
        if (!project ||
            !loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty())
            return QString();
        if (!engine.loadProject(project, [&image, &provenance](const ModuleInstance& instance) -> ModuleBase* {
                if (instance.id == QLatin1String("grab")) {
                    if (!provenance.isValid())
                        return new UnprovenancedImageFixtureModule(image);
                    return new ImageWithProvenanceFixtureModule(image, provenance);
                }
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            }))
            return QString();
        QString runId;
        if (!engine.runOnceAsync(&runId) || runId.isEmpty())
            return QString();
        QElapsedTimer timer;
        timer.start();
        while (!engine.runObservation(runId).finished && timer.elapsed() < 10000)
            QTest::qWait(10);
        if (!engine.runObservation(runId).finished)
            return QString();
        if (!engine.runObservation(runId).result.success)
            return QString();
        return runId;
    };

    const QString noSourceRunId = runWithProvenance(ImageCoordinateProvenance{});
    QVERIFY(!noSourceRunId.isEmpty());
    const QJsonObject noSource =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", noSourceRunId}, {"instanceId", "input"}, {"port", "fit_points"}});
    QCOMPARE(noSource.value(QStringLiteral("type")).toString(), QStringLiteral("PointSet2D"));
    QCOMPARE(noSource.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QCOMPARE(noSource.value(QStringLiteral("value")).toArray().size(), 3);
    QVERIFY(!noSource.contains(QStringLiteral("sourceImageCoordinates")));

    ImageCoordinateProvenance invalidIdProvenance;
    invalidIdProvenance.sourceImageId = QStringLiteral("not-a-uuid");
    invalidIdProvenance.sourceSize = QSize(160, 120);
    invalidIdProvenance.localOriginInSource = QPointF(10, 20);
    QVERIFY(invalidIdProvenance.isValid());
    const QString invalidIdRunId = runWithProvenance(invalidIdProvenance);
    QVERIFY(!invalidIdRunId.isEmpty());
    const QJsonObject invalidId =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", invalidIdRunId}, {"instanceId", "input"}, {"port", "fit_points"}});
    QCOMPARE(invalidId.value(QStringLiteral("value")).toArray().size(), 3);
    QVERIFY(!invalidId.contains(QStringLiteral("sourceImageCoordinates")));

    ImageCoordinateProvenance horizonProvenance;
    horizonProvenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    horizonProvenance.sourceSize = QSize(500, 400);
    horizonProvenance.localToSource = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -51.25};
    horizonProvenance.hasLocalToSourceTransform = true;
    QVERIFY(horizonProvenance.isValid());
    const QString horizonRunId = runWithProvenance(horizonProvenance);
    QVERIFY(!horizonRunId.isEmpty());
    const ImageData output = engine.moduleOutput(QStringLiteral("input"));
    const QVector<QPointF> localPoints = output.data(QStringLiteral("fit_points")).value<QVector<QPointF>>();
    const ImageCoordinateProvenance mappedProvenance = output.coordinateProvenance();
    const auto matrix = mappedProvenance.localToSourceMatrix();
    QCOMPARE(matrix[6] * localPoints.first().x() + matrix[7] * localPoints.first().y() + matrix[8], 0.0);
    const QVariantList pluginSourceValues = output.data(QStringLiteral("fit_points_source")).toList();
    QCOMPARE(pluginSourceValues.size(), 3);
    QVERIFY(!pluginSourceValues.first().isValid());

    const QJsonObject horizon =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", horizonRunId}, {"instanceId", "input"}, {"port", "fit_points"}});
    QCOMPARE(horizon.value(QStringLiteral("value")).toArray().size(), localPoints.size());
    const QJsonArray horizonPoints =
        horizon.value(QStringLiteral("sourceImageCoordinates")).toObject().value(QStringLiteral("points")).toArray();
    QCOMPARE(horizonPoints.size(), localPoints.size());
    QVERIFY(horizonPoints.first().isNull());
    for (int i = 1; i < localPoints.size(); ++i) {
        const auto mapped = output.coordinateProvenance().mapLocalToSource(localPoints.at(i));
        QVERIFY(mapped.has_value());
        QCOMPARE(horizonPoints.at(i).toArray(), QJsonArray({mapped->x(), mapped->y()}));
        const QVariantList pluginPoint = pluginSourceValues.at(i).toList();
        QCOMPARE(horizonPoints.at(i).toArray(),
                 QJsonArray({pluginPoint.at(0).toDouble(), pluginPoint.at(1).toDouble()}));
    }
}

void TestAcceptanceFlows::testAgentMeasurementInputPointSetSourcePageRespectsResponseLimit() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty(),
             "load MeasurementInput point-set project with synthetic image");

    ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
    QJsonArray largePoints;
    for (int i = 0; i < 1024; ++i)
        largePoints.append(QJsonArray{100000000000000.25 + i, -100000000000000.5 + i});
    input.params[QStringLiteral("points")] = largePoints;
    project->updateModule(input.id, input);

    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    provenance.sourceSize = QSize(160, 120);
    QVERIFY(provenance.isValid());
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [&image, &provenance](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("grab"))
            return new ImageWithProvenanceFixtureModule(image, provenance);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    AgentActor actor;
    const QJsonObject response = actor.executeTool(
        QStringLiteral("get_module_output"),
        QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"port", "fit_points"}, {"limit", 1000}});
    QCOMPARE(response.value(QStringLiteral("totalRows")).toInt(), 1024);
    QCOMPARE(response.value(QStringLiteral("limit")).toInt(), 1000);
    QVERIFY(response.value(QStringLiteral("truncated")).toBool());
    const QJsonArray localPage = response.value(QStringLiteral("value")).toArray();
    const QJsonArray sourcePage =
        response.value(QStringLiteral("sourceImageCoordinates")).toObject().value(QStringLiteral("points")).toArray();
    QVERIFY(localPage.size() < 1000);
    QCOMPARE(sourcePage.size(), localPage.size());
    QVERIFY(QJsonDocument(response).toJson(QJsonDocument::Compact).size() <= 65536);
    const ImageData output = engine.moduleOutput(QStringLiteral("input"));
    const QVector<QPointF> pluginLocalPoints = output.data(QStringLiteral("fit_points")).value<QVector<QPointF>>();
    const QVariantList pluginSource = output.data(QStringLiteral("fit_points_source")).toList();
    for (int i = 0; i < localPage.size(); ++i) {
        const QJsonArray expectedLocalPoint{pluginLocalPoints.at(i).x(), pluginLocalPoints.at(i).y()};
        QCOMPARE(localPage.at(i).toArray(), expectedLocalPoint);
        const QVariantList expectedSource = pluginSource.at(i).toList();
        const QJsonArray expectedSourcePoint{expectedSource.at(0).toDouble(), expectedSource.at(1).toDouble()};
        QCOMPARE(sourcePage.at(i).toArray(), expectedSourcePoint);
    }
}

void TestAcceptanceFlows::testAgentMeasurementInputPointPairMapsPointsThroughCrop() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty(),
             "load MeasurementInput point-pair project");
    ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
    input.params[QStringLiteral("mode")] = QStringLiteral("point_pair");
    input.params[QStringLiteral("point1")] = QJsonArray{1.25, 2.5};
    input.params[QStringLiteral("point2")] = QJsonArray{7.0, 8.0};
    project->updateModule(input.id, input);
    ModuleInstance distance;
    distance.id = QStringLiteral("distance");
    distance.moduleId = QStringLiteral("DistancePP");
    distance.name = QStringLiteral("point distance");
    project->addModule(distance, 3);
    for (const auto& ports : {qMakePair(QStringLiteral("image"), QStringLiteral("image")),
                              qMakePair(QStringLiteral("point1"), QStringLiteral("point1")),
                              qMakePair(QStringLiteral("point2"), QStringLiteral("point2"))}) {
        ModuleConnection connection;
        connection.fromModuleId = QStringLiteral("input");
        connection.toModuleId = distance.id;
        connection.fromPort = ports.first;
        connection.toPort = ports.second;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{QStringLiteral("grab"), QStringLiteral("crop"), input.id, distance.id};
    project->setFlows({flow});

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    QCOMPARE(cropOutput.width(), 60);
    QCOMPARE(cropOutput.height(), 40);
    QCOMPARE(cropOutput.coordinateProvenance().localOriginInSource, QPointF(50, 40));
    const ImageData output = engine.moduleOutput(QStringLiteral("input"));
    const ImageCoordinateProvenance provenance = output.coordinateProvenance();
    QVERIFY(provenance.hasLocalToSourceTransform);
    const ImageData distanceOutput = engine.moduleOutput(QStringLiteral("distance"));
    QCOMPARE(distanceOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    const QMap<QString, double> expectedValues{
        {QStringLiteral("distance"), distanceOutput.data(QStringLiteral("distance")).toDouble()},
        {QStringLiteral("delta_x"), distanceOutput.data(QStringLiteral("delta_x")).toDouble()},
        {QStringLiteral("delta_y"), distanceOutput.data(QStringLiteral("delta_y")).toDouble()}};
    QVERIFY(std::abs(expectedValues.value(QStringLiteral("distance")) - std::sqrt(5.75 * 5.75 + 5.5 * 5.5)) <= 1e-9);
    QCOMPARE(expectedValues.value(QStringLiteral("delta_x")), 5.75);
    QCOMPARE(expectedValues.value(QStringLiteral("delta_y")), 5.5);
    for (auto it = expectedValues.cbegin(); it != expectedValues.cend(); ++it) {
        for (bool byPort : {true, false}) {
            QJsonObject args{{"runId", runId}, {"instanceId", "distance"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), it.key());
            const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(response.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QVERIFY(std::abs(response.value(QStringLiteral("value")).toDouble() - it.value()) <= 1e-9);
            QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
    const auto assertPoint = [&](const QString& port, const QPointF& local, const QPointF& expectedSource) {
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"port", port}});
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
        QCOMPARE(response.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), QJsonArray({local.x(), local.y()}));
        const auto mapped = provenance.mapLocalToSource(local);
        QVERIFY(mapped.has_value());
        QCOMPARE(*mapped, expectedSource);
        const QJsonObject source = response.value(QStringLiteral("sourceImageCoordinates")).toObject();
        QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(),
                 engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId);
        QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
        QCOMPARE(source.value(QStringLiteral("value")).toArray(), QJsonArray({expectedSource.x(), expectedSource.y()}));
    };
    assertPoint(QStringLiteral("point1"), QPointF(1.25, 2.5), QPointF(51.25, 42.5));
    assertPoint(QStringLiteral("point2"), QPointF(7.0, 8.0), QPointF(57.0, 48.0));

    const QJsonObject namedValue = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"key", "point1"}});
    QCOMPARE(namedValue.value(QStringLiteral("value")).toArray(), QJsonArray({1.25, 2.5}));
    QVERIFY(!namedValue.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!namedValue.contains(QStringLiteral("sourceImageCoordinates")));
    for (const QString& parameter : {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("maskOffset"),
                                     QStringLiteral("polygonOffset"), QStringLiteral("polygonLimit")}) {
        QJsonObject args{{"runId", runId}, {"instanceId", "input"}, {"port", "point1"}};
        args.insert(parameter, 0);
        const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!rejected.contains(QStringLiteral("value")));
    }
}

void TestAcceptanceFlows::testAgentMeasurementInputCustomPoint1DoesNotCarryCoordinateSemantics() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty(),
             "load MeasurementInput custom-mode project");
    ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
    input.params[QStringLiteral("mode")] = QStringLiteral("custom");
    input.params[QStringLiteral("point1")] = QJsonArray{1.25, 2.5};
    input.params[QStringLiteral("point2")] = QJsonArray{7.0, 8.0};
    project->updateModule(input.id, input);
    ModuleInstance distance;
    distance.id = QStringLiteral("distance");
    distance.moduleId = QStringLiteral("DistancePP");
    distance.name = QStringLiteral("custom coordinate distance");
    project->addModule(distance, 3);
    for (const auto& ports : {qMakePair(QStringLiteral("image"), QStringLiteral("image")),
                              qMakePair(QStringLiteral("point1"), QStringLiteral("point1")),
                              qMakePair(QStringLiteral("point2"), QStringLiteral("point2"))}) {
        ModuleConnection connection;
        connection.fromModuleId = QStringLiteral("input");
        connection.toModuleId = distance.id;
        connection.fromPort = ports.first;
        connection.toPort = ports.second;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{QStringLiteral("grab"), QStringLiteral("crop"), input.id, distance.id};
    project->setFlows({flow});
    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    QCOMPARE(engine.moduleOutput(QStringLiteral("input")).data(QStringLiteral("measurement_input_mode")).toString(),
             QStringLiteral("custom"));
    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"port", "point1"}});
    QCOMPARE(response.value(QStringLiteral("value")).toArray(), QJsonArray({1.25, 2.5}));
    QVERIFY(response.value(QStringLiteral("type")).toString() != QStringLiteral("Point2D"));
    QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    const ImageData distanceOutput = RunEngine::instance().moduleOutput(QStringLiteral("distance"));
    QCOMPARE(distanceOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    const QMap<QString, double> expectedDistanceValues{
        {QStringLiteral("distance"), distanceOutput.data(QStringLiteral("distance")).toDouble()},
        {QStringLiteral("delta_x"), distanceOutput.data(QStringLiteral("delta_x")).toDouble()},
        {QStringLiteral("delta_y"), distanceOutput.data(QStringLiteral("delta_y")).toDouble()}};
    QVERIFY(std::abs(expectedDistanceValues.value(QStringLiteral("distance")) - std::sqrt(5.75 * 5.75 + 5.5 * 5.5)) <=
            1e-9);
    QCOMPARE(expectedDistanceValues.value(QStringLiteral("delta_x")), 5.75);
    QCOMPARE(expectedDistanceValues.value(QStringLiteral("delta_y")), 5.5);
    for (auto it = expectedDistanceValues.cbegin(); it != expectedDistanceValues.cend(); ++it) {
        for (bool byPort : {true, false}) {
            QJsonObject args{{"runId", runId}, {"instanceId", "distance"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), it.key());
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(std::abs(scalar.value(QStringLiteral("value")).toDouble() - it.value()) <= 1e-9);
            QVERIFY(!scalar.contains(QStringLiteral("unit")));
            QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalar.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
}

void TestAcceptanceFlows::testAgentMeasurementInputPointPairFailsClosedForMissingAndHorizonSources() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const auto runWithProvenance = [&](const ImageCoordinateProvenance& provenance) {
        if (!ProjectManager::instance().newProject())
            return QString();
        Project* project = ProjectManager::instance().currentProject();
        if (!project ||
            !loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty())
            return QString();
        ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
        input.params[QStringLiteral("mode")] = QStringLiteral("point_pair");
        input.params[QStringLiteral("point1")] = QJsonArray{1.25, 2.5};
        input.params[QStringLiteral("point2")] = QJsonArray{7.0, 8.0};
        project->updateModule(input.id, input);
        if (!engine.loadProject(project, [&image, &provenance](const ModuleInstance& instance) -> ModuleBase* {
                if (instance.id == QLatin1String("grab"))
                    return provenance.isValid()
                               ? static_cast<ModuleBase*>(new ImageWithProvenanceFixtureModule(image, provenance))
                               : new UnprovenancedImageFixtureModule(image);
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            }))
            return QString();
        QString runId;
        if (!engine.runOnceAsync(&runId) || runId.isEmpty())
            return QString();
        QElapsedTimer timer;
        timer.start();
        while (!engine.runObservation(runId).finished && timer.elapsed() < 10000)
            QTest::qWait(10);
        return engine.runObservation(runId).finished && engine.runObservation(runId).result.success ? runId : QString();
    };

    const QString noSourceRunId = runWithProvenance(ImageCoordinateProvenance{});
    QVERIFY(!noSourceRunId.isEmpty());
    const QJsonObject noSource =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", noSourceRunId}, {"instanceId", "input"}, {"port", "point1"}});
    QCOMPARE(noSource.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
    QCOMPARE(noSource.value(QStringLiteral("value")).toArray(), QJsonArray({1.25, 2.5}));
    QVERIFY(!noSource.contains(QStringLiteral("sourceImageCoordinates")));

    ImageCoordinateProvenance horizonProvenance;
    horizonProvenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    horizonProvenance.sourceSize = QSize(500, 400);
    horizonProvenance.localToSource = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -51.25};
    horizonProvenance.hasLocalToSourceTransform = true;
    QVERIFY(horizonProvenance.isValid());
    const QString horizonRunId = runWithProvenance(horizonProvenance);
    QVERIFY(!horizonRunId.isEmpty());
    const ImageCoordinateProvenance mapped = engine.moduleOutput(QStringLiteral("input")).coordinateProvenance();
    const auto matrix = mapped.localToSourceMatrix();
    QCOMPARE(matrix[6] * 1.25 + matrix[7] * 2.5 + matrix[8], 0.0);
    QVERIFY(!mapped.mapLocalToSource(QPointF(1.25, 2.5)).has_value());
    const QJsonObject horizon =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", horizonRunId}, {"instanceId", "input"}, {"port", "point1"}});
    QCOMPARE(horizon.value(QStringLiteral("value")).toArray(), QJsonArray({1.25, 2.5}));
    QVERIFY(!horizon.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testAgentMeasurementInputPointPairThreeCoordinateCompatibility() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measuregap_pointpair.json", QString(), *project).isEmpty(),
             "load MeasurementInput to MeasureGap 3D project");
    ModuleInstance input = project->moduleById(QStringLiteral("measurementinput")).value();
    input.params[QStringLiteral("mode")] = QStringLiteral("point_pair");
    input.params[QStringLiteral("point1")] = QJsonArray{16777217.125, 12.5, -3.75};
    input.params[QStringLiteral("point2")] = QJsonArray{16777220.125, 16.5, -0.75};
    project->updateModule(input.id, input);

    ModuleInstance source;
    source.id = QStringLiteral("source");
    source.moduleId = QStringLiteral("GrabImage");
    source.name = QStringLiteral("带来源图像");
    project->addModule(source, 0);
    ProjectFlow flow = project->flow(QStringLiteral("main"));
    flow.nodeIds = QStringList{source.id, input.id, QStringLiteral("measuregap")};
    project->setFlows({flow});

    const PluginInfo inputMetadata = PluginManager::instance().pluginInfo(QStringLiteral("MeasurementInput"));
    for (const QString& id : {QStringLiteral("point1"), QStringLiteral("point2")}) {
        const auto port = std::find_if(inputMetadata.outputPorts.cbegin(), inputMetadata.outputPorts.cend(),
                                       [&id](const PortSpec& spec) { return spec.id == id; });
        QVERIFY(port != inputMetadata.outputPorts.cend());
        QCOMPARE(port->type, DataType::Any);
    }

    QImage sourceImage(160, 120, QImage::Format_RGB32);
    sourceImage.fill(Qt::black);
    ImageCoordinateProvenance provenance;
    provenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    provenance.sourceSize = QSize(500, 400);
    provenance.localToSource = {1.0, 0.0, 50.0, 0.0, 1.0, 40.0, 0.0, 0.0, 1.0};
    provenance.hasLocalToSourceTransform = true;
    QVERIFY(provenance.isValid());

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [&sourceImage, &provenance](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("source"))
            return new ImageWithProvenanceFixtureModule(sourceImage, provenance);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    AgentActor actor;
    const ImageData sourceOutput = engine.moduleOutput(QStringLiteral("source"));
    QVERIFY(sourceOutput.hasCoordinateProvenance());
    QVERIFY(sourceOutput.coordinateProvenance().isValid());
    const ImageData inputOutput = engine.moduleOutput(QStringLiteral("measurementinput"));
    QVERIFY(inputOutput.hasCoordinateProvenance());
    QVERIFY(inputOutput.coordinateProvenance().isValid());
    QCOMPARE(inputOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("World3D"));
    QVERIFY(!inputOutput.data(QStringLiteral("source_coordinate_available")).toBool());
    const ImageData gapOutput = engine.moduleOutput(QStringLiteral("measuregap"));
    QVERIFY(std::abs(gapOutput.data(QStringLiteral("gap_distance")).toDouble() - std::sqrt(34.0)) < 1e-12);
    QCOMPARE(gapOutput.data(QStringLiteral("gap_delta_x")).toDouble(), 3.0);
    QCOMPARE(gapOutput.data(QStringLiteral("gap_delta_y")).toDouble(), 4.0);
    QCOMPARE(gapOutput.data(QStringLiteral("gap_delta_z")).toDouble(), 3.0);

    const auto verifyMeasureGapScalarsUnlabelled = [&actor](const QString& scalarRunId) {
        for (const QString& scalar :
             {QStringLiteral("gap"), QStringLiteral("gap_distance"), QStringLiteral("gap_delta_x"),
              QStringLiteral("gap_delta_y"), QStringLiteral("gap_delta_z")}) {
            for (const bool byPort : {false, true}) {
                QJsonObject args{{QStringLiteral("runId"), scalarRunId},
                                 {QStringLiteral("instanceId"), QStringLiteral("measuregap")}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), scalar);
                const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
                if (response.value(QStringLiteral("status")).toString() != QLatin1String("available") ||
                    !response.value(QStringLiteral("value")).isDouble() ||
                    !std::isfinite(response.value(QStringLiteral("value")).toDouble()) ||
                    response.contains(QStringLiteral("unit")) ||
                    response.contains(QStringLiteral("coordinateSystem")) ||
                    response.contains(QStringLiteral("coordinateFrame")) ||
                    response.contains(QStringLiteral("sourceImageCoordinates")))
                    return false;
            }
        }
        return true;
    };
    QVERIFY(verifyMeasureGapScalarsUnlabelled(runId));

    const QMap<QString, QJsonArray> expectedPoints{{QStringLiteral("point1"), QJsonArray{16777217.125, 12.5, -3.75}},
                                                   {QStringLiteral("point2"), QJsonArray{16777220.125, 16.5, -0.75}}};
    for (auto it = expectedPoints.cbegin(); it != expectedPoints.cend(); ++it) {
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "measurementinput"}, {"port", it.key()}});
        QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Point3D"));
        QCOMPARE(response.value(QStringLiteral("coordinateFrame")).toString(), QStringLiteral("World3D"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), it.value());
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        QVERIFY(!response.contains(QStringLiteral("unit")));
    }

    const QJsonObject namedValue =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "measurementinput"}, {"key", "point1"}});
    QCOMPARE(namedValue.value(QStringLiteral("value")).toArray(), expectedPoints.value(QStringLiteral("point1")));
    QVERIFY(namedValue.value(QStringLiteral("type")).toString() != QStringLiteral("Point3D"));
    QVERIFY(!namedValue.contains(QStringLiteral("coordinateFrame")));

    ModuleInstance zeroZInput = project->moduleById(QStringLiteral("measurementinput")).value();
    zeroZInput.params[QStringLiteral("point1")] = QJsonArray{16777217.125, 12.5, 0.0};
    zeroZInput.params[QStringLiteral("point2")] = QJsonArray{16777220.125, 16.5, 0.0};
    project->updateModule(zeroZInput.id, zeroZInput);
    QVERIFY(engine.loadProject(project, [&sourceImage, &provenance](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("source"))
            return new ImageWithProvenanceFixtureModule(sourceImage, provenance);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString zeroZRunId;
    QVERIFY(engine.runOnceAsync(&zeroZRunId));
    QVERIFY(!zeroZRunId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(zeroZRunId).finished, 10000);
    QVERIFY2(engine.runObservation(zeroZRunId).result.success,
             qPrintable(engine.runObservation(zeroZRunId).result.errorMessage));
    QCOMPARE(
        engine.moduleOutput(QStringLiteral("measurementinput")).data(QStringLiteral("coordinate_frame")).toString(),
        QStringLiteral("World3D"));
    QCOMPARE(engine.moduleOutput(QStringLiteral("measuregap")).data(QStringLiteral("measurement_dimension")).toString(),
             QStringLiteral("2d"));
    QVERIFY(verifyMeasureGapScalarsUnlabelled(zeroZRunId));

    QVERIFY(ProjectManager::instance().newProject());
    project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance typedInput;
    typedInput.id = QStringLiteral("typed_input");
    typedInput.moduleId = QStringLiteral("MeasurementInput");
    typedInput.name = QStringLiteral("三维点源");
    typedInput.params = {{QStringLiteral("mode"), QStringLiteral("point_pair")},
                         {QStringLiteral("point1"), QJsonArray{16777217.125, 12.5, -3.75}},
                         {QStringLiteral("point2"), QJsonArray{16777220.125, 16.5, -0.75}}};
    project->addModule(typedInput);
    ModuleInstance fitCircle;
    fitCircle.id = QStringLiteral("fit");
    fitCircle.moduleId = QStringLiteral("FitCircle");
    fitCircle.name = QStringLiteral("二维拟合消费者");
    project->addModule(fitCircle);
    ModuleConnection threeDimensionalToTwoDimensional;
    threeDimensionalToTwoDimensional.fromModuleId = typedInput.id;
    threeDimensionalToTwoDimensional.toModuleId = fitCircle.id;
    threeDimensionalToTwoDimensional.fromPort = QStringLiteral("point1");
    threeDimensionalToTwoDimensional.toPort = QStringLiteral("fit_points");
    threeDimensionalToTwoDimensional.edgeType = QStringLiteral("data");
    project->addConnection(threeDimensionalToTwoDimensional);
    ProjectFlow typedFlow;
    typedFlow.id = QStringLiteral("main");
    typedFlow.name = QStringLiteral("main");
    typedFlow.nodeIds = QStringList{typedInput.id, fitCircle.id};
    project->setFlows({typedFlow});
    QVERIFY(engine.loadProject(project));
    QString typedRunId;
    QVERIFY(engine.runOnceAsync(&typedRunId));
    QVERIFY(!typedRunId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(typedRunId).finished, 10000);
    QVERIFY(!engine.runObservation(typedRunId).result.success);
    QCOMPARE(engine.runObservation(typedRunId).result.failedModule, fitCircle.id);
    QCOMPARE(engine.runObservation(typedRunId).result.failedPort, QStringLiteral("fit_points"));

    QVERIFY(ProjectManager::instance().newProject());
    project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance spoof;
    spoof.id = QStringLiteral("spoof");
    spoof.moduleId = QStringLiteral("PointPairWorld3DSpoofFixture");
    spoof.name = QStringLiteral("Non-target spoof");
    project->addModule(spoof);
    ProjectFlow spoofFlow;
    spoofFlow.id = QStringLiteral("main");
    spoofFlow.name = QStringLiteral("main");
    spoofFlow.nodeIds = QStringList{spoof.id};
    project->setFlows({spoofFlow});
    QVERIFY(engine.loadProject(
        project, [](const ModuleInstance&) { return new NonTargetPointPairWorld3DSpoofFixtureModule(); }));
    QString spoofRunId;
    QVERIFY(engine.runOnceAsync(&spoofRunId));
    QVERIFY(!spoofRunId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(spoofRunId).finished, 10000);
    QVERIFY(engine.runObservation(spoofRunId).result.success);
    const QJsonObject spoofOutput =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", spoofRunId}, {"instanceId", "spoof"}, {"port", "point1"}});
    QCOMPARE(spoofOutput.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(spoofOutput.value(QStringLiteral("value")).toArray(), expectedPoints.value(QStringLiteral("point1")));
    QVERIFY(spoofOutput.value(QStringLiteral("type")).toString() != QStringLiteral("Point3D"));
    QVERIFY(!spoofOutput.contains(QStringLiteral("coordinateFrame")));
    QVERIFY(!spoofOutput.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!spoofOutput.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testAgentMeasurementInputPointPairRejectsMalformedOutputs() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const QList<QVariant> invalidValues{QVariantList{1.0},
                                        QVariantList{1.0, QStringLiteral("2")},
                                        QVariantList{1.0, std::numeric_limits<double>::quiet_NaN()},
                                        QVariantList{1.0, std::numeric_limits<double>::infinity()},
                                        QVariantList{1.0, 2.0, 3.0, 4.0},
                                        QVariant::fromValue(QPointF(1.0, 2.0))};
    for (const QVariant& invalid : invalidValues) {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                     .isEmpty(),
                 "load malformed point-pair fixture project");
        QVERIFY(engine.loadProject(project, [&invalid](const ModuleInstance& instance) -> ModuleBase* {
            if (instance.id == QLatin1String("input"))
                return new MeasurementInputPointPairOutputFixtureModule(invalid);
            return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
        }));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject rejected =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"port", "point1"}});
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!rejected.contains(QStringLiteral("value")));
        const QJsonObject validSecondPoint =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "input"}, {"port", "point2"}});
        QCOMPARE(validSecondPoint.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(validSecondPoint.value(QStringLiteral("value")).toArray(), QJsonArray({7.0, 8.0}));
    }
}

void TestAcceptanceFlows::testLegacyMarkerlessLineMeasurementsStayUnspecifiedAfterProjectRoundTrip() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));

    struct Scenario {
        QString mode;
        QJsonObject values;
        QList<QPair<QString, QJsonArray>> outputs;
    };
    const QList<Scenario> scenarios{{QStringLiteral("point_line"),
                                     QJsonObject{{QStringLiteral("point"), QJsonArray{12.25, 13.5, 14.75}},
                                                 {QStringLiteral("line"), QJsonArray{5.5, 30.25, 45.75, 30.5}}},
                                     {{QStringLiteral("point"), QJsonArray{12.25, 13.5, 14.75}},
                                      {QStringLiteral("line"), QJsonArray{5.5, 30.25, 45.75, 30.5}}}},
                                    {QStringLiteral("line_pair"),
                                     QJsonObject{{QStringLiteral("line1"), QJsonArray{5.5, 30.25, 45.75, 30.5}},
                                                 {QStringLiteral("line2"), QJsonArray{10.125, 5.25, 10.75, 35.5}}},
                                     {{QStringLiteral("line1"), QJsonArray{5.5, 30.25, 45.75, 30.5}},
                                      {QStringLiteral("line2"), QJsonArray{10.125, 5.25, 10.75, 35.5}}}}};

    ProjectManager& projectManager = ProjectManager::instance();
    RunEngine& engine = RunEngine::instance();
    AgentActor actor;
    for (const Scenario& scenario : scenarios) {
        Project* initial = projectManager.newProject();
        QVERIFY(initial != nullptr);
        const QString loadError = loadProjectWithPlaceholder(
            QStringLiteral("projects/accept_measurementinput_pointset.json"), dataDir.path(), *initial);
        QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
        QCOMPARE(initial->toJson().value(QStringLiteral("version")).toString(), QStringLiteral("3.0"));

        ModuleInstance input = initial->moduleById(QStringLiteral("input")).value();
        QVERIFY(!input.params.contains(QStringLiteral("pick_coordinate_frame")));
        input.params.insert(QStringLiteral("mode"), scenario.mode);
        for (auto it = scenario.values.constBegin(); it != scenario.values.constEnd(); ++it)
            input.params.insert(it.key(), it.value());
        initial->updateModule(input.id, input);
        const QJsonObject beforeSave = initial->toJson();
        const QJsonObject beforeInput = initial->moduleById(QStringLiteral("input"))->toJson();
        QVERIFY(
            !beforeInput.value(QStringLiteral("params")).toObject().contains(QStringLiteral("pick_coordinate_frame")));

        const QString projectPath = dataDir.filePath(scenario.mode + QStringLiteral("-markerless.dlx"));
        QVERIFY(initial->save(projectPath));
        QFile savedFile(projectPath);
        QVERIFY(savedFile.open(QIODevice::ReadOnly));
        const QJsonObject savedJson = QJsonDocument::fromJson(savedFile.readAll()).object();
        QCOMPARE(savedJson.value(QStringLiteral("version")).toString(), QStringLiteral("3.0"));
        const QJsonObject savedInput = savedJson.value(QStringLiteral("modules")).toArray().at(2).toObject();
        QVERIFY(
            !savedInput.value(QStringLiteral("params")).toObject().contains(QStringLiteral("pick_coordinate_frame")));

        Project* reopened = projectManager.openProject(projectPath);
        QVERIFY(reopened != nullptr);
        const QJsonObject reopenedJson = reopened->toJson();
        QCOMPARE(reopenedJson.value(QStringLiteral("version")).toString(), QStringLiteral("3.0"));
        QVERIFY(
            !reopened->moduleById(QStringLiteral("input"))->params.contains(QStringLiteral("pick_coordinate_frame")));
        QCOMPARE(reopenedJson.value(QStringLiteral("connections")), beforeSave.value(QStringLiteral("connections")));
        QCOMPARE(reopenedJson.value(QStringLiteral("flows")), beforeSave.value(QStringLiteral("flows")));
        QCOMPARE(reopened->moduleById(QStringLiteral("input"))->toJson(), beforeInput);

        QVERIFY(engine.loadProject(reopened));
        const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), QJsonObject{});
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("errorMessage")).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY2(engine.runObservation(runId).result.success,
                 qPrintable(engine.runObservation(runId).result.errorMessage));
        QCOMPARE(reopened->toJson().value(QStringLiteral("connections")),
                 beforeSave.value(QStringLiteral("connections")));
        QCOMPARE(reopened->toJson().value(QStringLiteral("flows")), beforeSave.value(QStringLiteral("flows")));
        QCOMPARE(reopened->moduleById(QStringLiteral("input"))->toJson(), beforeInput);

        const ImageData inputOutput = engine.moduleOutput(QStringLiteral("input"));
        QCOMPARE(inputOutput.data(QStringLiteral("measurement_input_mode")).toString(), scenario.mode);
        QCOMPARE(inputOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
        QVERIFY(inputOutput.hasData(QStringLiteral("source_coordinate_available")));
        QVERIFY(!inputOutput.data(QStringLiteral("source_coordinate_available")).toBool());
        QVERIFY(!inputOutput.hasData(QStringLiteral("source_image_id")));
        QVERIFY(!inputOutput.hasData(QStringLiteral("source_image_size")));
        for (const auto& expected : scenario.outputs)
            QVERIFY(!inputOutput.hasData(expected.first + QStringLiteral("_source")));
        QVERIFY(inputOutput.hasCoordinateProvenance());
        const ImageCoordinateProvenance provenance = inputOutput.coordinateProvenance();
        QCOMPARE(provenance.sourceSize, QSize(160, 120));
        const ImageCoordinateProvenance::Matrix3 expectedSource{{1.0, 0.0, 50.0, 0.0, 1.0, 40.0, 0.0, 0.0, 1.0}};
        for (size_t index = 0; index < expectedSource.size(); ++index)
            QCOMPARE(provenance.localToSourceMatrix()[index], expectedSource[index]);

        for (const auto& expected : scenario.outputs) {
            for (bool byPort : {false, true}) {
                QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), "input"}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), expected.first);
                const QJsonObject result = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QCOMPARE(result.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
                QCOMPARE(result.value(QStringLiteral("value")).toArray(), expected.second);
                QVERIFY(!result.contains(QStringLiteral("coordinateSystem")));
                QVERIFY(!result.contains(QStringLiteral("coordinateFrame")));
                QVERIFY(!result.contains(QStringLiteral("sourceImageCoordinates")));
            }
        }
    }
}

void TestAcceptanceFlows::testLegacyMarkerlessLineMeasurementsRemainUnspecifiedAfterMigration() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));

    const QJsonObject pointLineParams{{QStringLiteral("mode"), QStringLiteral("point_line")},
                                      {QStringLiteral("point"), QJsonArray{12.25, 13.5, 14.75}},
                                      {QStringLiteral("line"), QJsonArray{5.5, 30.25, 45.75, 30.5}}};
    const QJsonObject linePairParams{{QStringLiteral("mode"), QStringLiteral("line_pair")},
                                     {QStringLiteral("line1"), QJsonArray{5.5, 30.25, 45.75, 30.5}},
                                     {QStringLiteral("line2"), QJsonArray{10.125, 5.25, 10.75, 35.5}}};
    const QList<QPair<QString, bool>> formats{{QStringLiteral("v2"), true}, {QStringLiteral("missing-version"), false}};
    ProjectManager& projectManager = ProjectManager::instance();
    RunEngine& engine = RunEngine::instance();
    AgentActor actor;

    for (const auto& format : formats) {
        Project source;
        const QString loadError = loadProjectWithPlaceholder(
            QStringLiteral("projects/accept_measurementinput_pointset.json"), dataDir.path(), source);
        QVERIFY2(loadError.isEmpty(), qPrintable(loadError));
        QJsonObject legacy = source.toJson();
        QJsonArray modules = legacy.value(QStringLiteral("modules")).toArray();
        QJsonArray migratedModules;
        for (const QJsonValue& value : modules) {
            const QJsonObject module = value.toObject();
            if (module.value(QStringLiteral("id")).toString() != QLatin1String("input"))
                migratedModules.append(module);
        }
        migratedModules.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("input_point_line")},
                                           {QStringLiteral("moduleId"), QStringLiteral("MeasurementInput")},
                                           {QStringLiteral("name"), QStringLiteral("Legacy point line")},
                                           {QStringLiteral("params"), pointLineParams}});
        migratedModules.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("input_line_pair")},
                                           {QStringLiteral("moduleId"), QStringLiteral("MeasurementInput")},
                                           {QStringLiteral("name"), QStringLiteral("Legacy line pair")},
                                           {QStringLiteral("params"), linePairParams}});
        legacy.insert(QStringLiteral("modules"), migratedModules);

        QJsonArray connections;
        for (const QJsonValue& value : legacy.value(QStringLiteral("connections")).toArray()) {
            const QJsonObject connection = value.toObject();
            if (connection.value(QStringLiteral("toModuleId")).toString() != QLatin1String("input"))
                connections.append(connection);
        }
        for (const QString& target : {QStringLiteral("input_point_line"), QStringLiteral("input_line_pair")}) {
            connections.append(QJsonObject{{QStringLiteral("fromModuleId"), QStringLiteral("crop")},
                                           {QStringLiteral("toModuleId"), target},
                                           {QStringLiteral("fromOutput"), 0},
                                           {QStringLiteral("toInput"), 0},
                                           {QStringLiteral("fromPort"), QStringLiteral("image")},
                                           {QStringLiteral("toPort"), QStringLiteral("image")},
                                           {QStringLiteral("edgeType"), QStringLiteral("data")}});
        }
        legacy.insert(QStringLiteral("connections"), connections);
        const QJsonArray expectedConnections = connections;
        legacy.remove(QStringLiteral("flows"));
        legacy.remove(QStringLiteral("resources"));
        if (format.second)
            legacy.insert(QStringLiteral("version"), QStringLiteral("2.0"));
        else
            legacy.remove(QStringLiteral("version"));

        const QString projectPath = dataDir.filePath(format.first + QStringLiteral("-markerless-lines.dlx"));
        QFile file(projectPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = QJsonDocument(legacy).toJson(QJsonDocument::Indented);
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        const QJsonObject rawJson = QJsonDocument::fromJson(bytes).object();
        if (format.second)
            QCOMPARE(rawJson.value(QStringLiteral("version")).toString(), QStringLiteral("2.0"));
        else
            QVERIFY(!rawJson.contains(QStringLiteral("version")));
        const QJsonArray rawModules = rawJson.value(QStringLiteral("modules")).toArray();
        for (const QString& id : {QStringLiteral("input_point_line"), QStringLiteral("input_line_pair")}) {
            const auto it = std::find_if(rawModules.cbegin(), rawModules.cend(), [&id](const QJsonValue& item) {
                return item.toObject().value(QStringLiteral("id")).toString() == id;
            });
            QVERIFY(it != rawModules.cend());
            QVERIFY(!it->toObject()
                         .value(QStringLiteral("params"))
                         .toObject()
                         .contains(QStringLiteral("pick_coordinate_frame")));
        }

        Project* reopened = projectManager.openProject(projectPath);
        QVERIFY(reopened != nullptr);
        QCOMPARE(reopened->formatVersion(), QStringLiteral("3.0"));
        QCOMPARE(reopened->migration().originalVersion, QStringLiteral("2.0"));
        QCOMPARE(reopened->toJson().value(QStringLiteral("connections")).toArray(), expectedConnections);
        for (const auto& item : {qMakePair(QStringLiteral("input_point_line"), pointLineParams),
                                 qMakePair(QStringLiteral("input_line_pair"), linePairParams)}) {
            const ModuleInstance module = reopened->moduleById(item.first).value();
            QCOMPARE(module.params, item.second);
            QVERIFY(!module.params.contains(QStringLiteral("pick_coordinate_frame")));
        }

        QVERIFY(engine.loadProject(reopened));
        const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), QJsonObject{});
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("errorMessage")).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY2(engine.runObservation(runId).result.success,
                 qPrintable(engine.runObservation(runId).result.errorMessage));

        for (const auto& scenario : {qMakePair(QStringLiteral("input_point_line"), pointLineParams),
                                     qMakePair(QStringLiteral("input_line_pair"), linePairParams)}) {
            const QString instanceId = scenario.first;
            const QString mode = scenario.second.value(QStringLiteral("mode")).toString();
            const ImageData output = engine.moduleOutput(instanceId);
            QCOMPARE(output.data(QStringLiteral("measurement_input_mode")).toString(), mode);
            QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
            QCOMPARE(output.data(QStringLiteral("source_coordinate_available")).toBool(), false);
            QVERIFY(output.hasCoordinateProvenance());
            QCOMPARE(output.coordinateProvenance().localToSourceMatrix()[2], 50.0);
            QCOMPARE(output.coordinateProvenance().localToSourceMatrix()[5], 40.0);

            for (const QString& key : scenario.second.keys()) {
                if (key == QLatin1String("mode"))
                    continue;
                const QJsonArray expected = scenario.second.value(key).toArray();
                for (bool byPort : {false, true}) {
                    QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), instanceId}};
                    args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
                    const QJsonObject result = actor.executeTool(QStringLiteral("get_module_output"), args);
                    QCOMPARE(result.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                    QCOMPARE(result.value(QStringLiteral("value")).toArray(), expected);
                    QVERIFY(!result.contains(QStringLiteral("coordinateSystem")));
                    QVERIFY(!result.contains(QStringLiteral("coordinateFrame")));
                    QVERIFY(!result.contains(QStringLiteral("sourceImageCoordinates")));
                }
            }
        }
    }
}

void TestAcceptanceFlows::testAgentMeasurementInputLineOutputsRespectCoordinateFrame() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();

    const auto runMode = [&](const QString& mode, const QString& markerBeforeAgent = QString(),
                             const QString& markerAfterAgent = QString()) {
        if (!ProjectManager::instance().newProject())
            return QString();
        Project* project = ProjectManager::instance().currentProject();
        if (!project ||
            !loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty())
            return QString();
        ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
        if (!markerBeforeAgent.isEmpty() &&
            !project->setModuleParam(input.id, QStringLiteral("pick_coordinate_frame"), markerBeforeAgent))
            return QString();
        const QJsonObject configured =
            actor.executeTool(QStringLiteral("set_module_params"),
                              QJsonObject{{"instanceId", input.id},
                                          {"values", QJsonObject{{"mode", mode},
                                                                 {"line", QJsonArray{5, 30, 45, 30}},
                                                                 {"line1", QJsonArray{5, 30, 45, 30}},
                                                                 {"line2", QJsonArray{10, 5, 10, 35}}}}});
        if (configured.value(QStringLiteral("status")).toString() != QLatin1String("params_set"))
            return QString();
        if (!markerAfterAgent.isEmpty() &&
            !project->setModuleParam(input.id, QStringLiteral("pick_coordinate_frame"), markerAfterAgent))
            return QString();
        const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
        const QString runId = started.value(QStringLiteral("runId")).toString();
        if (runId.isEmpty())
            return QString();
        QElapsedTimer elapsed;
        elapsed.start();
        while (!engine.runObservation(runId).finished && elapsed.elapsed() < 10000)
            QTest::qWait(10);
        const RunObservation observation = engine.runObservation(runId);
        return observation.finished && observation.result.success ? runId : QString();
    };

    const auto query = [&actor](const QString& runId, const QString& name, bool byPort) {
        QJsonObject args{{"runId", runId}, {"instanceId", "input"}};
        args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), name);
        return actor.executeTool(QStringLiteral("get_module_output"), args);
    };

    QString runId = runMode(QStringLiteral("point_line"), QString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(!runId.isEmpty());
    ImageData output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    const ImageCoordinateProvenance provenance = output.coordinateProvenance();
    QVERIFY(provenance.hasLocalToSourceTransform);
    QCOMPARE(provenance.localOriginInSource, QPointF(50, 40));
    QCOMPARE(output.width(), 60);
    QCOMPARE(output.height(), 40);
    const auto assertTypedLine = [&](const QString& name, const QPointF& localStart, const QPointF& localEnd,
                                     const QPointF& sourceStart, const QPointF& sourceEnd,
                                     const ImageCoordinateProvenance& lineProvenance, const QString& sourceImageId) {
        for (bool byPort : {true, false}) {
            const QJsonObject response = query(runId, name, byPort);
            QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Line2D"));
            const QJsonObject value = response.value(QStringLiteral("value")).toObject();
            QCOMPARE(value.value(QStringLiteral("start")).toArray(), QJsonArray({localStart.x(), localStart.y()}));
            QCOMPARE(value.value(QStringLiteral("end")).toArray(), QJsonArray({localEnd.x(), localEnd.y()}));
            QCOMPARE(value.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
            const QJsonObject source = value.value(QStringLiteral("sourceImageCoordinates")).toObject();
            QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), sourceImageId);
            QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(),
                     QStringLiteral("source_image_pixels"));
            QCOMPARE(source.value(QStringLiteral("start")).toArray(), QJsonArray({sourceStart.x(), sourceStart.y()}));
            QCOMPARE(source.value(QStringLiteral("end")).toArray(), QJsonArray({sourceEnd.x(), sourceEnd.y()}));
            const auto mappedStart = lineProvenance.mapLocalToSource(localStart);
            const auto mappedEnd = lineProvenance.mapLocalToSource(localEnd);
            QVERIFY(mappedStart.has_value());
            QVERIFY(mappedEnd.has_value());
            QCOMPARE(*mappedStart, sourceStart);
            QCOMPARE(*mappedEnd, sourceEnd);
        }
    };
    assertTypedLine(QStringLiteral("line"), QPointF(5, 30), QPointF(45, 30), QPointF(55, 70), QPointF(95, 70),
                    provenance, engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId);

    runId = runMode(QStringLiteral("point_line"));
    QVERIFY(!runId.isEmpty());
    output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QVERIFY(output.hasData(QStringLiteral("source_coordinate_available")));
    QVERIFY(!output.data(QStringLiteral("source_coordinate_available")).toBool());
    QVERIFY(!output.hasData(QStringLiteral("source_image_id")));
    QVERIFY(!output.hasData(QStringLiteral("source_image_size")));
    QVERIFY(!output.hasData(QStringLiteral("source_coordinate_origin")));
    QVERIFY(!output.hasData(QStringLiteral("point_source")));
    QVERIFY(!output.hasData(QStringLiteral("line_source")));
    for (bool byPort : {true, false}) {
        const QJsonObject response = query(runId, QStringLiteral("line"), byPort);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), QJsonArray({5.0, 30.0, 45.0, 30.0}));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    }
    const QJsonObject markerSpoof = actor.executeTool(
        QStringLiteral("set_param"), QJsonObject{{QStringLiteral("instanceId"), QStringLiteral("input")},
                                                 {QStringLiteral("key"), QStringLiteral("pick_coordinate_frame")},
                                                 {QStringLiteral("value"), QStringLiteral("ImageLocalPixel")}});
    QCOMPARE(markerSpoof.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    const QJsonObject batchMarkerSpoof = actor.executeTool(
        QStringLiteral("set_module_params"),
        QJsonObject{{"instanceId", "input"}, {"values", QJsonObject{{"pick_coordinate_frame", "ImageLocalPixel"}}}});
    QCOMPARE(batchMarkerSpoof.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    ModuleInstance guiMarkedInput =
        ProjectManager::instance().currentProject()->moduleById(QStringLiteral("input")).value();
    guiMarkedInput.params[QStringLiteral("pick_coordinate_frame")] = QStringLiteral("ImageLocalPixel");
    ProjectManager::instance().currentProject()->updateModule(guiMarkedInput.id, guiMarkedInput);
    QCOMPARE(ProjectManager::instance()
                 .currentProject()
                 ->moduleById(QStringLiteral("input"))
                 ->params.value(QStringLiteral("pick_coordinate_frame"))
                 .toString(),
             QStringLiteral("ImageLocalPixel"));
    const QJsonObject resetMarker =
        actor.executeTool(QStringLiteral("reset_module_params"),
                          QJsonObject{{"instanceId", "input"}, {"keys", QJsonArray{"pick_coordinate_frame"}}});
    QCOMPARE(resetMarker.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
    QCOMPARE(ProjectManager::instance()
                 .currentProject()
                 ->moduleById(QStringLiteral("input"))
                 ->params.value(QStringLiteral("pick_coordinate_frame"))
                 .toString(),
             QStringLiteral("ImageLocalPixel"));

    runId = runMode(QStringLiteral("point_line"), QStringLiteral("ImageLocalPixel"));
    QVERIFY(!runId.isEmpty());
    output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QVERIFY(output.hasData(QStringLiteral("source_coordinate_available")));
    QVERIFY(!output.data(QStringLiteral("source_coordinate_available")).toBool());
    QVERIFY(!output.hasData(QStringLiteral("source_image_id")));
    QVERIFY(!output.hasData(QStringLiteral("source_image_size")));
    QVERIFY(!output.hasData(QStringLiteral("source_coordinate_origin")));
    QVERIFY(!output.hasData(QStringLiteral("point_source")));
    QVERIFY(!output.hasData(QStringLiteral("line_source")));
    for (bool byPort : {true, false}) {
        const QJsonObject response = query(runId, QStringLiteral("line"), byPort);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), QJsonArray({5.0, 30.0, 45.0, 30.0}));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    }

    runId = runMode(QStringLiteral("point_line"), QString(), QStringLiteral("not-a-frame"));
    QVERIFY(!runId.isEmpty());
    output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QVERIFY(output.hasData(QStringLiteral("source_coordinate_available")));
    QVERIFY(!output.data(QStringLiteral("source_coordinate_available")).toBool());
    QVERIFY(!output.hasData(QStringLiteral("source_image_id")));
    QVERIFY(!output.hasData(QStringLiteral("source_image_size")));
    QVERIFY(!output.hasData(QStringLiteral("source_coordinate_origin")));
    QVERIFY(!output.hasData(QStringLiteral("point_source")));
    QVERIFY(!output.hasData(QStringLiteral("line_source")));
    for (bool byPort : {true, false}) {
        const QJsonObject response = query(runId, QStringLiteral("line"), byPort);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), QJsonArray({5.0, 30.0, 45.0, 30.0}));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    }

    runId = runMode(QStringLiteral("line_pair"), QString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(!runId.isEmpty());
    output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    const ImageCoordinateProvenance linePairProvenance = output.coordinateProvenance();
    const QString linePairSourceId = engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId;
    assertTypedLine(QStringLiteral("line1"), QPointF(5, 30), QPointF(45, 30), QPointF(55, 70), QPointF(95, 70),
                    linePairProvenance, linePairSourceId);
    assertTypedLine(QStringLiteral("line2"), QPointF(10, 5), QPointF(10, 35), QPointF(60, 45), QPointF(60, 75),
                    linePairProvenance, linePairSourceId);

    runId = runMode(QStringLiteral("line_pair"));
    QVERIFY(!runId.isEmpty());
    output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QVERIFY(output.hasData(QStringLiteral("source_coordinate_available")));
    QVERIFY(!output.data(QStringLiteral("source_coordinate_available")).toBool());
    QVERIFY(!output.hasData(QStringLiteral("source_image_id")));
    QVERIFY(!output.hasData(QStringLiteral("source_image_size")));
    QVERIFY(!output.hasData(QStringLiteral("source_coordinate_origin")));
    QVERIFY(!output.hasData(QStringLiteral("line1_source")));
    QVERIFY(!output.hasData(QStringLiteral("line2_source")));
    for (const auto& line : {qMakePair(QStringLiteral("line1"), QJsonArray{5.0, 30.0, 45.0, 30.0}),
                             qMakePair(QStringLiteral("line2"), QJsonArray{10.0, 5.0, 10.0, 35.0})}) {
        for (bool byPort : {true, false}) {
            const QJsonObject response = query(runId, line.first, byPort);
            QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
            QCOMPARE(response.value(QStringLiteral("value")).toArray(), line.second);
            QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }

    runId = runMode(QStringLiteral("custom"));
    QVERIFY(!runId.isEmpty());
    output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QVERIFY(output.coordinateProvenance().hasLocalToSourceTransform);
    const QMap<QString, QJsonArray> customLines{{QStringLiteral("line"), QJsonArray{5, 30, 45, 30}},
                                                {QStringLiteral("line1"), QJsonArray{5, 30, 45, 30}},
                                                {QStringLiteral("line2"), QJsonArray{10, 5, 10, 35}}};
    for (auto it = customLines.cbegin(); it != customLines.cend(); ++it) {
        for (bool byPort : {true, false}) {
            const QJsonObject response = query(runId, it.key(), byPort);
            QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
            QCOMPARE(response.value(QStringLiteral("value")).toArray(), it.value());
            QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        }
        for (const QString& parameter :
             {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("maskOffset"),
              QStringLiteral("polygonOffset"), QStringLiteral("polygonLimit")}) {
            for (bool byPort : {true, false}) {
                QJsonObject args{{"runId", runId}, {"instanceId", "input"}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), it.key());
                args.insert(parameter, 0);
                const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
                QVERIFY(!rejected.contains(QStringLiteral("value")));
            }
        }
    }
}

void TestAcceptanceFlows::testAgentLinesDistanceNearestSegmentMapsThroughCrop() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty(),
             "load LinesDistance source flow");

    ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
    input.params[QStringLiteral("mode")] = QStringLiteral("line_pair");
    input.params[QStringLiteral("line1")] = QJsonArray{0, 0, 5, 0};
    input.params[QStringLiteral("line2")] = QJsonArray{10, 5, 10, 10};
    input.params[QStringLiteral("pick_coordinate_frame")] = QStringLiteral("ImageLocalPixel");
    project->updateModule(input.id, input);

    ModuleInstance distance;
    distance.id = QStringLiteral("distance");
    distance.moduleId = QStringLiteral("LinesDistance");
    project->addModule(distance);
    for (const auto& ports : {qMakePair(QStringLiteral("image"), QStringLiteral("image")),
                              qMakePair(QStringLiteral("line1"), QStringLiteral("line1")),
                              qMakePair(QStringLiteral("line2"), QStringLiteral("line2"))}) {
        ModuleConnection connection;
        connection.fromModuleId = input.id;
        connection.fromPort = ports.first;
        connection.toModuleId = distance.id;
        connection.toPort = ports.second;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow = project->flow(QStringLiteral("main"));
    flow.nodeIds.append(distance.id);
    project->setFlows({flow});

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData crop = engine.moduleOutput(QStringLiteral("crop"));
    const ImageCoordinateProvenance provenance = crop.coordinateProvenance();
    QVERIFY(provenance.isValid());
    QCOMPARE(provenance.localOriginInSource, QPointF(50, 40));
    const ImageData output = engine.moduleOutput(distance.id);
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    QCOMPARE(output.data(QStringLiteral("distance")).toDouble(), std::sqrt(50.0));
    const QPointF localStart(output.data(QStringLiteral("nearest_x1")).toDouble(),
                             output.data(QStringLiteral("nearest_y1")).toDouble());
    const QPointF localEnd(output.data(QStringLiteral("nearest_x2")).toDouble(),
                           output.data(QStringLiteral("nearest_y2")).toDouble());
    QCOMPARE(localStart, QPointF(5, 0));
    QCOMPARE(localEnd, QPointF(10, 5));
    QCOMPARE(output.data(QStringLiteral("nearest_segment")).toList(), (QVariantList{5.0, 0.0, 10.0, 5.0}));

    const QJsonObject nearest = actor.executeTool(
        QStringLiteral("get_module_output"),
        QJsonObject{{"runId", runId}, {"instanceId", distance.id}, {"port", QStringLiteral("nearest_segment")}});
    QCOMPARE(nearest.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(nearest.value(QStringLiteral("type")).toString(), QStringLiteral("Line2D"));
    const QJsonObject line = nearest.value(QStringLiteral("value")).toObject();
    QCOMPARE(line.value(QStringLiteral("start")).toArray(), (QJsonArray{5, 0}));
    QCOMPARE(line.value(QStringLiteral("end")).toArray(), (QJsonArray{10, 5}));
    QCOMPARE(line.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const QPointF expectedSourceStart(55, 40);
    const QPointF expectedSourceEnd(60, 45);
    const auto mappedStart = provenance.mapLocalToSource(localStart);
    const auto mappedEnd = provenance.mapLocalToSource(localEnd);
    QVERIFY(mappedStart);
    QVERIFY(mappedEnd);
    QCOMPARE(*mappedStart, expectedSourceStart);
    QCOMPARE(*mappedEnd, expectedSourceEnd);
    const QJsonObject source = line.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), provenance.sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QCOMPARE(source.value(QStringLiteral("start")).toArray(), (QJsonArray{55, 40}));
    QCOMPARE(source.value(QStringLiteral("end")).toArray(), (QJsonArray{60, 45}));

    const QJsonObject compatibility = actor.executeTool(
        QStringLiteral("get_module_output"),
        QJsonObject{{"runId", runId}, {"instanceId", distance.id}, {"key", QStringLiteral("nearest_segment")}});
    QCOMPARE(compatibility.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
    QCOMPARE(compatibility.value(QStringLiteral("value")).toArray(), (QJsonArray{5, 0, 10, 5}));
    QVERIFY(!compatibility.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!compatibility.contains(QStringLiteral("sourceImageCoordinates")));
    ImageData snapshot;
    QMap<QString, QVariant> snapshotKeys;
    PortValueMap snapshotPorts;
    QVERIFY(engine.runOutputSnapshot(runId, distance.id, snapshot, snapshotKeys, snapshotPorts));
    const QVariantList snapshotSegment = snapshotPorts.value(QStringLiteral("nearest_segment")).toList();
    QCOMPARE(snapshotSegment.size(), 4);
    const QList<QPair<QString, int>> nearestFields{{QStringLiteral("nearest_x1"), 0},
                                                   {QStringLiteral("nearest_y1"), 1},
                                                   {QStringLiteral("nearest_x2"), 2},
                                                   {QStringLiteral("nearest_y2"), 3}};
    for (const auto& [key, component] : nearestFields) {
        const QVariant expectedValue = snapshotKeys.value(key);
        QVERIFY(expectedValue.isValid());
        bool expectedOk = false;
        const double expected = expectedValue.toDouble(&expectedOk);
        QVERIFY(expectedOk);
        QVERIFY(std::isfinite(expected));
        QCOMPARE(expected, snapshotSegment.at(component).toDouble());
        for (const bool byPort : {false, true}) {
            QJsonObject args{{"runId", runId}, {"instanceId", distance.id}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(scalar.value(QStringLiteral("value")).isDouble());
            QVERIFY(std::isfinite(scalar.value(QStringLiteral("value")).toDouble()));
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), expected);
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
    for (const QString& parameter : {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("maskOffset"),
                                     QStringLiteral("polygonOffset"), QStringLiteral("polygonLimit")}) {
        QJsonObject args{{"runId", runId}, {"instanceId", distance.id}, {"port", QStringLiteral("nearest_segment")}};
        args.insert(parameter, 0);
        const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!rejected.contains(QStringLiteral("value")));
    }
}

void TestAcceptanceFlows::testAgentLinesDistanceSourceGateAndHorizon() {
    ToolSchema::instance().registerDefaultTools();
    const QString fixtureDir = m_tempDir.filePath(QStringLiteral("plugins/LinePairFixture"));
    QVERIFY(QDir().mkpath(fixtureDir));
    QFile metadataFile(QDir(fixtureDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(metadataFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray metadata =
        R"({"id":"com.deeplux.tests.linepairfixture","name":"LinePairFixture","category":"test","version":"1.0","ports":{"inputs":[],"outputs":[{"id":"image","displayName":"Image","type":"Any","required":false,"multiple":false},{"id":"line1","displayName":"Line 1","type":"Line2D","required":false,"multiple":false},{"id":"line2","displayName":"Line 2","type":"Line2D","required":false,"multiple":false}]}})";
    QCOMPARE(metadataFile.write(metadata), static_cast<qint64>(metadata.size()));
    metadataFile.close();
    PluginManager::instance().scanPlugins();
    QCOMPARE(PluginManager::instance().pluginInfo(QStringLiteral("LinePairFixture")).id,
             QStringLiteral("com.deeplux.tests.linepairfixture"));

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const auto makeProvenance = [](const ImageCoordinateProvenance::Matrix3& matrix, bool hasTransform) {
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        provenance.sourceSize = QSize(200, 200);
        provenance.localOriginInSource = QPointF(50, 40);
        provenance.localToSource = matrix;
        provenance.hasLocalToSourceTransform = hasTransform;
        return provenance;
    };
    const auto runFixture = [&engine](const QString& firstModuleId, bool splitSources,
                                      const ImageCoordinateProvenance& provenance, const QString& frame,
                                      const QString& mode, QString& runId) {
        engine.clearModules();
        ProjectManager::instance().closeProject();
        Project* project = ProjectManager::instance().newProject();
        if (!project)
            return false;
        ModuleInstance sourceA;
        sourceA.id = QStringLiteral("source_a");
        sourceA.moduleId = firstModuleId;
        project->addModule(sourceA);
        if (splitSources) {
            ModuleInstance sourceB;
            sourceB.id = QStringLiteral("source_b");
            sourceB.moduleId = QStringLiteral("MeasurementInput");
            project->addModule(sourceB);
        }
        ModuleInstance distance;
        distance.id = QStringLiteral("distance");
        distance.moduleId = QStringLiteral("LinesDistance");
        project->addModule(distance);
        const auto connectPort = [project, &distance](const QString& source, const QString& fromPort,
                                                      const QString& toPort) {
            ModuleConnection connection;
            connection.fromModuleId = source;
            connection.fromPort = fromPort;
            connection.toModuleId = distance.id;
            connection.toPort = toPort;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        };
        connectPort(sourceA.id, QStringLiteral("image"), QStringLiteral("image"));
        connectPort(sourceA.id, QStringLiteral("line1"), QStringLiteral("line1"));
        connectPort(splitSources ? QStringLiteral("source_b") : sourceA.id, QStringLiteral("line2"),
                    QStringLiteral("line2"));
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = QStringLiteral("main");
        flow.nodeIds = splitSources ? QStringList{QStringLiteral("source_a"), QStringLiteral("source_b"), distance.id}
                                    : QStringList{QStringLiteral("source_a"), distance.id};
        project->setFlows({flow});
        return engine.loadProject(
                   project,
                   [provenance, frame, mode, splitSources](const ModuleInstance& instance) -> ModuleBase* {
                       if (instance.id == QLatin1String("source_a") || instance.id == QLatin1String("source_b"))
                           return new LinePairSourceFixtureModule(instance.moduleId, QVariantList{0.0, 0.0, 5.0, 0.0},
                                                                  QVariantList{10.0, 5.0, 10.0, 10.0}, provenance,
                                                                  frame, mode);
                       PluginManager& plugins = PluginManager::instance();
                       if (!plugins.isPluginLoaded(instance.moduleId) && !plugins.loadPlugin(instance.moduleId))
                           return nullptr;
                       return qobject_cast<ModuleBase*>(plugins.createModule(instance.moduleId));
                   }) &&
               engine.runOnceAsync(&runId);
    };
    const auto query = [&actor](const QString& runId) {
        return actor.executeTool(QStringLiteral("get_module_output"),
                                 QJsonObject{{"runId", runId},
                                             {"instanceId", QStringLiteral("distance")},
                                             {"port", QStringLiteral("nearest_segment")}});
    };
    const auto verifyNoNearestPointUnits = [&actor](const QString& runId) {
        for (const QString& field : {QStringLiteral("nearest_x1"), QStringLiteral("nearest_y1"),
                                     QStringLiteral("nearest_x2"), QStringLiteral("nearest_y2")}) {
            for (const bool byPort : {false, true}) {
                QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), "distance"}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), field);
                const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QVERIFY(scalar.value(QStringLiteral("value")).isDouble());
                QVERIFY(!scalar.contains(QStringLiteral("unit")));
                QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
                QVERIFY(!scalar.contains(QStringLiteral("coordinateFrame")));
                QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
            }
        }
    };
    const ImageCoordinateProvenance translated = makeProvenance({1, 0, 50, 0, 1, 40, 0, 0, 1}, true);

    for (const auto& scenario :
         {qMakePair(QStringLiteral("LinePairFixture"), false), qMakePair(QStringLiteral("MeasurementInput"), true)}) {
        QString runId;
        QVERIFY(runFixture(scenario.first, scenario.second, translated, QStringLiteral("ImageLocalPixel"),
                           QStringLiteral("line_pair"), runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY2(engine.runObservation(runId).result.success,
                 qPrintable(engine.runObservation(runId).result.errorMessage));
        const QJsonObject response = query(runId);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), (QJsonArray{5, 0, 10, 5}));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        verifyNoNearestPointUnits(runId);
    }

    QString runId;
    QVERIFY(runFixture(QStringLiteral("MeasurementInput"), false, translated, QStringLiteral("Unspecified"),
                       QStringLiteral("custom"), runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    const QJsonObject custom = query(runId);
    QCOMPARE(custom.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
    QVERIFY(!custom.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!custom.contains(QStringLiteral("sourceImageCoordinates")));
    verifyNoNearestPointUnits(runId);

    QVERIFY(runFixture(QStringLiteral("MeasurementInput"), false, translated, QStringLiteral("World3D"),
                       QStringLiteral("line_pair"), runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    verifyNoNearestPointUnits(runId);

    QVERIFY(runFixture(QStringLiteral("MeasurementInput"), false, ImageCoordinateProvenance(),
                       QStringLiteral("ImageLocalPixel"), QStringLiteral("line_pair"), runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    const QJsonObject noProvenance = query(runId);
    QCOMPARE(noProvenance.value(QStringLiteral("type")).toString(), QStringLiteral("Line2D"));
    QVERIFY(!noProvenance.value(QStringLiteral("value")).toObject().contains(QStringLiteral("sourceImageCoordinates")));

    const auto horizon = makeProvenance({1, 0, 0, 0, 1, 0, 1, 0, -7.5}, true);
    QVERIFY(runFixture(QStringLiteral("MeasurementInput"), false, horizon, QStringLiteral("ImageLocalPixel"),
                       QStringLiteral("line_pair"), runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    const auto startSource = horizon.mapLocalToSource(QPointF(5, 0));
    const auto endSource = horizon.mapLocalToSource(QPointF(10, 5));
    QVERIFY(startSource);
    QVERIFY(endSource);
    QCOMPARE(startSource->x(), -2.0);
    QCOMPARE(endSource->x(), 4.0);
    const QJsonObject crossing = query(runId);
    QCOMPARE(crossing.value(QStringLiteral("type")).toString(), QStringLiteral("Line2D"));
    QCOMPARE(crossing.value(QStringLiteral("value")).toObject().value(QStringLiteral("start")).toArray(),
             (QJsonArray{5, 0}));
    QVERIFY(!crossing.value(QStringLiteral("value")).toObject().contains(QStringLiteral("sourceImageCoordinates")));
    engine.clearModules();
    ProjectManager::instance().closeProject();
}

void TestAcceptanceFlows::testAgentLinesDistanceNearestScalarUnitsRequireMatchingSegment() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance input;
    input.id = QStringLiteral("input");
    input.moduleId = QStringLiteral("MeasurementInput");
    project->addModule(input);
    ModuleInstance distance;
    distance.id = QStringLiteral("distance");
    distance.moduleId = QStringLiteral("LinesDistance");
    project->addModule(distance);
    for (const QString& port : {QStringLiteral("image"), QStringLiteral("line1"), QStringLiteral("line2")}) {
        ModuleConnection connection;
        connection.fromModuleId = input.id;
        connection.fromPort = port;
        connection.toModuleId = distance.id;
        connection.toPort = port;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{input.id, distance.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("MeasurementInput"))
            return new LinePairSourceFixtureModule(QStringLiteral("MeasurementInput"), QVariantList{0.0, 0.0, 5.0, 0.0},
                                                   QVariantList{10.0, 5.0, 10.0, 10.0}, ImageCoordinateProvenance());
        if (instance.moduleId == QLatin1String("LinesDistance"))
            return new LinesDistanceNearestScalarMismatchFixtureModule();
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    ImageData snapshot;
    QMap<QString, QVariant> snapshotKeys;
    PortValueMap snapshotPorts;
    QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("distance"), snapshot, snapshotKeys, snapshotPorts));
    QCOMPARE(snapshot.data(QStringLiteral("measurement_input_mode")).toString(), QStringLiteral("line_pair"));
    QCOMPARE(snapshot.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    const QVariantList segment = snapshotPorts.value(QStringLiteral("nearest_segment")).toList();
    QCOMPARE(segment, QVariantList({1.0, 2.0, 3.0, 4.0}));
    const PluginInfo info = PluginManager::instance().pluginInfo(QStringLiteral("LinesDistance"));
    const auto typedPort = std::find_if(info.outputPorts.cbegin(), info.outputPorts.cend(), [](const PortSpec& port) {
        return port.id == QLatin1String("nearest_segment");
    });
    QVERIFY(typedPort != info.outputPorts.cend());
    QCOMPARE(typedPort->type, DataType::Line2D);

    AgentActor actor;
    const QList<QPair<QString, int>> fields{{QStringLiteral("nearest_x1"), 0},
                                            {QStringLiteral("nearest_y1"), 1},
                                            {QStringLiteral("nearest_x2"), 2},
                                            {QStringLiteral("nearest_y2"), 3}};
    for (const auto& [field, component] : fields) {
        const QVariant scalarValue = snapshotKeys.value(field);
        QVERIFY(scalarValue.isValid());
        bool scalarOk = false;
        const double scalarExpected = scalarValue.toDouble(&scalarOk);
        QVERIFY(scalarOk);
        QVERIFY(std::isfinite(scalarExpected));
        QVERIFY(scalarExpected != segment.at(component).toDouble());
        for (const bool byPort : {false, true}) {
            QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), "distance"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), field);
            const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(response.value(QStringLiteral("value")).isDouble());
            QVERIFY(std::isfinite(response.value(QStringLiteral("value")).toDouble()));
            QCOMPARE(response.value(QStringLiteral("value")).toDouble(), scalarExpected);
            QVERIFY(!response.contains(QStringLiteral("unit")));
            QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
}

void TestAcceptanceFlows::testAgentLinesDistanceRejectsMalformedNearestSegment() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    const QList<QVariant> malformed{
        QVariant::fromValue(QVariantList{1.0, 2.0, 3.0}),
        QVariant::fromValue(QVariantList{1.0, 2.0, QStringLiteral("3"), 4.0}),
        QVariant::fromValue(QVariantList{1.0, 2.0, true, 4.0}),
        QVariant::fromValue(QVariantList{1.0, 2.0, std::numeric_limits<double>::quiet_NaN(), 4.0}),
        QVariant::fromValue(QVariantList{1.0, 2.0, std::numeric_limits<double>::infinity(), 4.0}),
    };
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    for (const QVariant& segment : malformed) {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                     .isEmpty(),
                 "load malformed LinesDistance output fixture flow");
        ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
        input.params[QStringLiteral("mode")] = QStringLiteral("line_pair");
        input.params[QStringLiteral("line1")] = QJsonArray{0, 0, 5, 0};
        input.params[QStringLiteral("line2")] = QJsonArray{10, 5, 10, 10};
        project->updateModule(input.id, input);
        ModuleInstance distance;
        distance.id = QStringLiteral("distance");
        distance.moduleId = QStringLiteral("LinesDistance");
        project->addModule(distance);
        for (const auto& ports : {qMakePair(QStringLiteral("image"), QStringLiteral("image")),
                                  qMakePair(QStringLiteral("line1"), QStringLiteral("line1")),
                                  qMakePair(QStringLiteral("line2"), QStringLiteral("line2"))}) {
            ModuleConnection connection;
            connection.fromModuleId = QStringLiteral("input");
            connection.fromPort = ports.first;
            connection.toModuleId = distance.id;
            connection.toPort = ports.second;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        }
        ProjectFlow flow = project->flow(QStringLiteral("main"));
        flow.nodeIds.append(distance.id);
        project->setFlows({flow});
        QVERIFY(engine.loadProject(project, [&segment](const ModuleInstance& instance) -> ModuleBase* {
            if (instance.id != QLatin1String("distance"))
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            return new LinesDistanceSegmentOutputFixtureModule(segment);
        }));

        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        const RunObservation observation = engine.runObservation(runId);
        QVERIFY2(observation.result.success,
                 qPrintable(QStringLiteral("code=%1 module=%2 port=%3 message=%4 diagnostics=%5")
                                .arg(observation.result.errorCode)
                                .arg(observation.result.failedModule)
                                .arg(observation.result.failedPort)
                                .arg(observation.result.errorMessage)
                                .arg(observation.result.diagnostics)));
        const QJsonObject rejected = actor.executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{"runId", runId}, {"instanceId", distance.id}, {"port", QStringLiteral("nearest_segment")}});
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!rejected.contains(QStringLiteral("value")));
    }
    engine.clearModules();
    ProjectManager::instance().closeProject();
}

void TestAcceptanceFlows::testAgentDistancePLFootPointMapsThroughCrop() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("distancepl.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(
        loadProjectWithPlaceholder("projects/accept_distancepl_footpoint.json", dataDir.path(), *project).isEmpty(),
        "load DistancePL foot point project");

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData crop = engine.moduleOutput(QStringLiteral("crop"));
    QCOMPARE(crop.width(), 60);
    QCOMPARE(crop.height(), 40);
    QCOMPARE(crop.coordinateProvenance().localOriginInSource, QPointF(50, 40));
    const ImageData inputOutput = engine.moduleOutput(QStringLiteral("input"));
    const ImageData output = engine.moduleOutput(QStringLiteral("distance"));
    QCOMPARE(inputOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    QCOMPARE(output.coordinateProvenance().sourceImageId, inputOutput.coordinateProvenance().sourceImageId);
    const QPointF local = output.data(QStringLiteral("foot_point")).toPointF();
    QCOMPARE(local, QPointF(20, 30));
    QCOMPARE(output.data(QStringLiteral("foot_x")).toDouble(), local.x());
    QCOMPARE(output.data(QStringLiteral("foot_y")).toDouble(), local.y());
    QCOMPARE(output.data(QStringLiteral("distance")).toDouble(), 20.0);

    for (bool byPort : {true, false}) {
        QJsonObject args{{"runId", runId}, {"instanceId", "distance"}};
        args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), QStringLiteral("distance"));
        const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
        QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), output.data(QStringLiteral("distance")).toDouble());
        QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
    }

    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}});
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Point2D"));
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QCOMPARE(response.value(QStringLiteral("value")).toArray(), QJsonArray({20, 30}));
    const auto mapped = output.coordinateProvenance().mapLocalToSource(local);
    QVERIFY(mapped.has_value());
    QCOMPARE(*mapped, QPointF(70, 70));
    const QJsonObject source = response.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(),
             engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QCOMPARE(source.value(QStringLiteral("value")).toArray(), QJsonArray({70, 70}));

    const QJsonObject namedValue =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"key", "foot_point"}});
    QCOMPARE(namedValue.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(namedValue.value(QStringLiteral("type")).toString(), QStringLiteral("QPointF"));
    QCOMPARE(namedValue.value(QStringLiteral("value")).toArray(), QJsonArray({20, 30}));
    QVERIFY(!namedValue.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!namedValue.contains(QStringLiteral("sourceImageCoordinates")));
    for (const QString& parameter : {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("maskOffset"),
                                     QStringLiteral("polygonOffset"), QStringLiteral("polygonLimit")}) {
        QJsonObject args{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}};
        args.insert(parameter, 0);
        const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!rejected.contains(QStringLiteral("value")));
    }
}

void TestAcceptanceFlows::testAgentDistancePLFootPointFailsClosedForMissingAndHorizonSources() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("distancepl.png"))));
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const auto runWithProvenance = [&](const ImageCoordinateProvenance& provenance, bool markerless = false) {
        if (!ProjectManager::instance().newProject())
            return QString();
        Project* project = ProjectManager::instance().currentProject();
        if (!project ||
            !loadProjectWithPlaceholder("projects/accept_distancepl_footpoint.json", dataDir.path(), *project)
                 .isEmpty())
            return QString();
        if (markerless) {
            ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
            input.params.remove(QStringLiteral("pick_coordinate_frame"));
            project->updateModule(input.id, input);
        }
        if (!engine.loadProject(project, [&image, &provenance](const ModuleInstance& instance) -> ModuleBase* {
                if (instance.id == QLatin1String("grab"))
                    return provenance.isValid()
                               ? static_cast<ModuleBase*>(new ImageWithProvenanceFixtureModule(image, provenance))
                               : new UnprovenancedImageFixtureModule(image);
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            }))
            return QString();
        QString runId;
        if (!engine.runOnceAsync(&runId) || runId.isEmpty())
            return QString();
        QElapsedTimer timer;
        timer.start();
        while (!engine.runObservation(runId).finished && timer.elapsed() < 10000)
            QTest::qWait(10);
        return engine.runObservation(runId).finished && engine.runObservation(runId).result.success ? runId : QString();
    };

    const QString noSourceRunId = runWithProvenance(ImageCoordinateProvenance{});
    QVERIFY(!noSourceRunId.isEmpty());
    const QJsonObject noSource =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", noSourceRunId}, {"instanceId", "distance"}, {"port", "foot_point"}});
    QCOMPARE(noSource.value(QStringLiteral("value")).toArray(), QJsonArray({20, 30}));
    QVERIFY(!noSource.contains(QStringLiteral("sourceImageCoordinates")));

    ImageCoordinateProvenance horizonProvenance;
    horizonProvenance.sourceImageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    horizonProvenance.sourceSize = QSize(160, 120);
    horizonProvenance.localToSource = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -70.0};
    horizonProvenance.hasLocalToSourceTransform = true;
    QVERIFY(horizonProvenance.isValid());
    const QString horizonRunId = runWithProvenance(horizonProvenance);
    QVERIFY(!horizonRunId.isEmpty());
    const ImageData output = engine.moduleOutput(QStringLiteral("distance"));
    const auto matrix = output.coordinateProvenance().localToSourceMatrix();
    QCOMPARE(matrix[6] * 20.0 + matrix[7] * 30.0 + matrix[8], 0.0);
    QVERIFY(!output.coordinateProvenance().mapLocalToSource(QPointF(20, 30)).has_value());
    const QJsonObject horizon =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", horizonRunId}, {"instanceId", "distance"}, {"port", "foot_point"}});
    QCOMPARE(horizon.value(QStringLiteral("value")).toArray(), QJsonArray({20, 30}));
    QVERIFY(!horizon.contains(QStringLiteral("sourceImageCoordinates")));

    const QString markerlessRunId = runWithProvenance(horizonProvenance, true);
    QVERIFY(!markerlessRunId.isEmpty());
    const ImageData markerlessInput = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(markerlessInput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QVERIFY(markerlessInput.coordinateProvenance().isValid());
    QVERIFY(markerlessInput.coordinateProvenance().hasLocalToSourceTransform);
    const ImageData markerlessOutput = engine.moduleOutput(QStringLiteral("distance"));
    QCOMPARE(markerlessOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QCOMPARE(markerlessOutput.data(QStringLiteral("foot_point")).toPointF(), QPointF(20, 30));
    for (bool byPort : {true, false}) {
        QJsonObject args{{"runId", markerlessRunId}, {"instanceId", "distance"}};
        args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), QStringLiteral("foot_point"));
        const QJsonObject raw = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(raw.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(raw.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(raw.value(QStringLiteral("value")).toArray(), QJsonArray({20, 30}));
        QVERIFY(!raw.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!raw.contains(QStringLiteral("coordinateFrame")));
        QVERIFY(!raw.contains(QStringLiteral("sourceImageCoordinates")));
    }
}

void TestAcceptanceFlows::testAgentDistancePLFootPointRemainsRawOutsideImageLocalFrame() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("distancepl.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(
        loadProjectWithPlaceholder("projects/accept_distancepl_footpoint.json", dataDir.path(), *project).isEmpty(),
        "load DistancePL unknown-coordinate-frame project");
    ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
    input.params[QStringLiteral("mode")] = QStringLiteral("custom");
    project->updateModule(input.id, input);
    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    QCOMPARE(engine.moduleOutput(QStringLiteral("input")).data(QStringLiteral("coordinate_frame")).toString(),
             QStringLiteral("Unspecified"));
    const ImageData output = engine.moduleOutput(QStringLiteral("distance"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("Unspecified"));
    QCOMPARE(output.data(QStringLiteral("foot_point")).toPointF(), QPointF(20, 30));
    QCOMPARE(output.data(QStringLiteral("distance")).toDouble(), 20.0);
    for (bool byPort : {true, false}) {
        QJsonObject args{{"runId", runId}, {"instanceId", "distance"}};
        args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), QStringLiteral("distance"));
        const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), output.data(QStringLiteral("distance")).toDouble());
        QVERIFY(!scalar.contains(QStringLiteral("unit")));
    }
    const QJsonObject unavailable =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}});
    QCOMPARE(unavailable.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
    QCOMPARE(unavailable.value(QStringLiteral("value")).toArray(), QJsonArray({20, 30}));
    QVERIFY(!unavailable.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!unavailable.contains(QStringLiteral("coordinateFrame")));
}

void TestAcceptanceFlows::testAgentDistancePLRejectsNonFiniteFootPoint() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("distancepl.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(
        loadProjectWithPlaceholder("projects/accept_distancepl_footpoint.json", dataDir.path(), *project).isEmpty(),
        "load non-finite DistancePL output project");
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("distance"))
            return new NonFiniteDistancePLFootPointFixtureModule();
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    AgentActor actor;
    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}});
    QCOMPARE(response.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
    QVERIFY(!response.contains(QStringLiteral("value")));
}

void TestAcceptanceFlows::testAgentPointSurfaceDistanceFootPointMapsThroughCrop() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_pointsurfacedistance_footpoint.json", dataDir.path(), *project)
                 .isEmpty(),
             "load PointSurfaceDistance foot point project");

    AgentActor actor;
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    RunEngine& engine = RunEngine::instance();
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData crop = engine.moduleOutput(QStringLiteral("crop"));
    QVERIFY(crop.coordinateProvenance().hasLocalToSourceTransform);
    QVERIFY(crop.coordinateProvenance().localOriginInSource != QPointF(0, 0));
    ImageData output;
    QMap<QString, QVariant> named;
    PortValueMap ports;
    QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("distance"), output, named, ports));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("World3D"));
    QCOMPARE(output.data(QStringLiteral("distance")).toDouble(), 7.0);
    QCOMPARE(output.data(QStringLiteral("foot_x")).toDouble(), 1.0);
    QCOMPARE(output.data(QStringLiteral("foot_y")).toDouble(), 2.0);
    QCOMPARE(output.data(QStringLiteral("foot_z")).toDouble(), 3.0);
    QCOMPARE(output.data(QStringLiteral("foot_point")).toList(), QVariantList({1.0, 2.0, 3.0}));
    for (const auto& [key, index] : {std::pair<QString, int>{QStringLiteral("foot_x"), 0},
                                     {QStringLiteral("foot_y"), 1},
                                     {QStringLiteral("foot_z"), 2}}) {
        const QJsonObject coordinate =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"key", key}});
        QCOMPARE(coordinate.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(coordinate.value(QStringLiteral("value")).toDouble(), output.data(key).toDouble());
        QCOMPARE(output.data(key).toDouble(), output.data(QStringLiteral("foot_point")).toList().at(index).toDouble());
        QCOMPARE(coordinate.value(QStringLiteral("coordinateFrame")).toString(), QStringLiteral("World3D"));
        QVERIFY(!coordinate.contains(QStringLiteral("unit")));
        QVERIFY(!coordinate.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!coordinate.contains(QStringLiteral("sourceImageCoordinates")));
    }

    for (const QString& selector : {QStringLiteral("key"), QStringLiteral("port")}) {
        const QJsonObject distance =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {selector, "distance"}});
        QCOMPARE(distance.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(distance.value(QStringLiteral("value")).toDouble(),
                 output.data(QStringLiteral("distance")).toDouble());
        QVERIFY(!distance.contains(QStringLiteral("unit")));
        QVERIFY(!distance.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!distance.contains(QStringLiteral("coordinateFrame")));
        QVERIFY(!distance.contains(QStringLiteral("sourceImageCoordinates")));
    }

    const QJsonObject typed =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}});
    QCOMPARE(typed.value(QStringLiteral("type")).toString(), QStringLiteral("Point3D"));
    QCOMPARE(typed.value(QStringLiteral("coordinateFrame")).toString(), QStringLiteral("World3D"));
    QCOMPARE(typed.value(QStringLiteral("value")).toArray(), QJsonArray({1, 2, 3}));
    QVERIFY(!typed.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!typed.contains(QStringLiteral("sourceImageCoordinates")));
    QVERIFY(!typed.contains(QStringLiteral("unit")));
    QVERIFY(!typed.contains(QStringLiteral("unitBasis")));
    const QJsonObject keyValue =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"key", "foot_point"}});
    QCOMPARE(keyValue.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
    QCOMPARE(keyValue.value(QStringLiteral("value")).toArray(), QJsonArray({1, 2, 3}));
    QVERIFY(!keyValue.contains(QStringLiteral("coordinateFrame")));
    for (const QString& parameter : {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("maskOffset"),
                                     QStringLiteral("polygonOffset"), QStringLiteral("polygonLimit")}) {
        QJsonObject args{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}};
        args.insert(parameter, 0);
        const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!rejected.contains(QStringLiteral("value")));
    }
}

void TestAcceptanceFlows::testAgentPointSurfaceDistanceClearsFootPointOutsideWorldFrame() {
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const auto runWithFrame = [&engine](const QString& frame, bool setFrame) {
        Project* project = ProjectManager::instance().newProject();
        if (!project)
            return QString();
        ModuleInstance source;
        source.id = QStringLiteral("input");
        source.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(source);
        ModuleInstance distance;
        distance.id = QStringLiteral("distance");
        distance.moduleId = QStringLiteral("PointSurfaceDistance");
        project->addModule(distance);
        for (const auto& ports : {qMakePair(QStringLiteral("image"), QStringLiteral("image")),
                                  qMakePair(QStringLiteral("point"), QStringLiteral("point")),
                                  qMakePair(QStringLiteral("plane"), QStringLiteral("plane"))}) {
            ModuleConnection connection;
            connection.fromModuleId = source.id;
            connection.toModuleId = distance.id;
            connection.fromPort = ports.first;
            connection.toPort = ports.second;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        }
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = flow.id;
        flow.nodeIds = QStringList{source.id, distance.id};
        project->setFlows({flow});
        if (!engine.loadProject(project, [frame, setFrame](const ModuleInstance& instance) -> ModuleBase* {
                if (instance.id == QLatin1String("input"))
                    return new PointSurfaceDistanceInputFixtureModule(frame, setFrame);
                return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
            }))
            return QString();
        QString runId;
        if (!engine.runOnceAsync(&runId) || runId.isEmpty())
            return QString();
        QElapsedTimer elapsed;
        elapsed.start();
        while (!engine.runObservation(runId).finished && elapsed.elapsed() < 10000)
            QTest::qWait(10);
        return engine.runObservation(runId).finished && engine.runObservation(runId).result.success ? runId : QString();
    };
    for (const auto& frame : {qMakePair(QString(), false), qMakePair(QStringLiteral("Unspecified"), true),
                              qMakePair(QStringLiteral("ImageLocalPixel"), true)}) {
        const QString runId = runWithFrame(frame.first, frame.second);
        QVERIFY(!runId.isEmpty());
        ImageData output;
        QMap<QString, QVariant> named;
        PortValueMap ports;
        QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("distance"), output, named, ports));
        QVERIFY(!output.hasData(QStringLiteral("foot_point")));
        const QJsonObject unavailable =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}});
        QCOMPARE(unavailable.value(QStringLiteral("errorCode")).toString(), QStringLiteral("OUTPUT_NOT_FOUND"));
        QVERIFY(!unavailable.contains(QStringLiteral("value")));
        for (const QString& key : {QStringLiteral("foot_x"), QStringLiteral("foot_y"), QStringLiteral("foot_z")}) {
            const QJsonObject coordinate =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"key", key}});
            QCOMPARE(coordinate.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(coordinate.value(QStringLiteral("value")).toDouble(), output.data(key).toDouble());
            QVERIFY(!coordinate.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!coordinate.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!coordinate.contains(QStringLiteral("sourceImageCoordinates")));
            QVERIFY(!coordinate.contains(QStringLiteral("unit")));
        }
    }
}

void TestAcceptanceFlows::testAgentPointSurfaceDistanceRejectsMalformedFootPoint() {
    ToolSchema::instance().registerDefaultTools();
    QList<QVariant> malformed{QVariant(QVariantList{1.0, 2.0}), QVariant(QVariantList{1.0, QStringLiteral("2"), 3.0}),
                              QVariant(QVariantList{1.0, true, 3.0}),
                              QVariant(QVariantList{1.0, 2.0, std::numeric_limits<double>::quiet_NaN()}),
                              QVariant(QVariantList{1.0, 2.0, std::numeric_limits<double>::infinity()})};
    RunEngine& engine = RunEngine::instance();
    AgentActor actor;
    for (const QVariant& footPoint : malformed) {
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        ModuleInstance distance;
        distance.id = QStringLiteral("distance");
        distance.moduleId = QStringLiteral("PointSurfaceDistance");
        project->addModule(distance);
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = flow.id;
        flow.nodeIds = QStringList{distance.id};
        project->setFlows({flow});
        QVERIFY(engine.loadProject(project, [footPoint](const ModuleInstance&) {
            return new PointSurfaceDistanceOutputFixtureModule(footPoint);
        }));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QVERIFY(!runId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"port", "foot_point"}});
        QCOMPARE(response.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!response.contains(QStringLiteral("value")));
    }
}

void TestAcceptanceFlows::testAgentPointSurfaceDistanceMalformedFootScalarHasNoFrameTag() {
    ToolSchema::instance().registerDefaultTools();
    Project* project = ProjectManager::instance().newProject();
    QVERIFY(project);
    ModuleInstance distance;
    distance.id = QStringLiteral("distance");
    distance.moduleId = QStringLiteral("PointSurfaceDistance");
    project->addModule(distance);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{distance.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance&) {
        return new PointSurfaceDistanceOutputFixtureModule(QVariantList{1.0, 2.0, 3.0}, QStringLiteral("1"));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);

    AgentActor actor;
    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "distance"}, {"key", "foot_x"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("value")).toString(), QStringLiteral("1"));
    QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
    QVERIFY(!response.contains(QStringLiteral("unit")));
}

void TestAcceptanceFlows::testAgentPointSetTypeMetadataDoesNotImplyCoordinateSemantics() {
    ToolSchema::instance().registerDefaultTools();
    const QString repoRoot = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    const QString fixtureDir = m_tempDir.filePath(QStringLiteral("plugins/PointSetFixture"));
    QVERIFY(QDir().mkpath(fixtureDir));
    QFile::remove(QDir(fixtureDir).filePath(QStringLiteral("metadata.json")));
    QVERIFY(QFile::copy(QDir(repoRoot).filePath(QStringLiteral("tests/fixtures/PointSetFixture/metadata.json")),
                        QDir(fixtureDir).filePath(QStringLiteral("metadata.json"))));
    PluginManager::instance().scanPlugins();
    QCOMPARE(PluginManager::instance().pluginInfo(QStringLiteral("PointSetFixture")).outputPorts.size(), 2);

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance fixture;
    fixture.id = QStringLiteral("fixture");
    fixture.moduleId = QStringLiteral("PointSetFixture");
    fixture.name = QStringLiteral("Point set fixture");
    project->addModule(fixture);
    ModuleInstance worldFixture;
    worldFixture.id = QStringLiteral("world");
    worldFixture.moduleId = QStringLiteral("MeasurementInput");
    worldFixture.name = QStringLiteral("World point set fixture");
    project->addModule(worldFixture);
    ModuleInstance nonFiniteFixture;
    nonFiniteFixture.id = QStringLiteral("nonfinite");
    nonFiniteFixture.moduleId = QStringLiteral("MeasurementInput");
    nonFiniteFixture.name = QStringLiteral("Nonfinite point set fixture");
    project->addModule(nonFiniteFixture);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{fixture.id, worldFixture.id, nonFiniteFixture.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        const QString frame =
            instance.id == QLatin1String("world") ? QStringLiteral("World3D") : QStringLiteral("ImageLocalPixel");
        return new PointSetOutputFixtureModule(instance.moduleId, frame, instance.id == QLatin1String("nonfinite"));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    AgentActor actor;
    for (const QString& instanceId : {QStringLiteral("fixture"), QStringLiteral("world")}) {
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", instanceId}, {"port", "fit_points"}});
        QVERIFY2(response.value(QStringLiteral("status")).toString() == QStringLiteral("available"),
                 qPrintable(QJsonDocument(response).toJson(QJsonDocument::Compact)));
        const QJsonArray expectedPoints{QJsonArray{1.0, 2.0}, QJsonArray{3.0, 4.0}};
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), expectedPoints);
        QVERIFY(response.value(QStringLiteral("type")).toString() != QStringLiteral("PointSet2D"));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    }
    const QJsonObject nonFinite =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "nonfinite"}, {"port", "fit_points"}});
    QCOMPARE(nonFinite.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
    QVERIFY(!nonFinite.contains(QStringLiteral("value")));
    QVERIFY(!nonFinite.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testFitCircleTypedOutputUsesImageFrameAndClearsPassedCircle() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("fitcircle.png"))));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitcircle_pointset.json", dataDir.path(), *project).isEmpty(),
             "load FitCircle point-set project");
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const QJsonObject circleResponse = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"port", "circle"}});
    QCOMPARE(circleResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject circle = circleResponse.value(QStringLiteral("value")).toObject();
    QCOMPARE(circle.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QVERIFY(std::abs(circle.value(QStringLiteral("centerX")).toDouble() - 25.0) < 0.1);
    QVERIFY(std::abs(circle.value(QStringLiteral("centerY")).toDouble() - 25.0) < 0.1);
    QVERIFY(std::abs(circle.value(QStringLiteral("radius")).toDouble() - 12.0) < 0.1);
    QCOMPARE(circle.value(QStringLiteral("score")).toDouble(), 0.0);
    const QJsonObject source = circle.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(),
             engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QVERIFY(std::abs(source.value(QStringLiteral("centerX")).toDouble() - 75.0) < 0.1);
    QVERIFY(std::abs(source.value(QStringLiteral("centerY")).toDouble() - 65.0) < 0.1);
    QVERIFY(!source.contains(QStringLiteral("radius")));

    const ImageData fitOutput = engine.moduleOutput(QStringLiteral("fit"));
    QCOMPARE(fitOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(fitOutput.data(QStringLiteral("circle")).value<Circle2D>().isValid());
    for (const QString& key : {QStringLiteral("circle_center_x"), QStringLiteral("circle_center_y"),
                               QStringLiteral("circle_radius"), QStringLiteral("circle_error")}) {
        for (const bool byPort : {false, true}) {
            QJsonObject args{{"runId", runId}, {"instanceId", "fit"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), fitOutput.data(key).toDouble());
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }

    ModuleBase* fitInstance = engine.getModule(QStringLiteral("fit"));
    ModuleBase* inputInstance = engine.getModule(QStringLiteral("input"));
    QVERIFY(fitInstance);
    QVERIFY(inputInstance);
    inputInstance->setParams(
        QJsonObject{{"mode", "point_set"}, {"points", QJsonArray{QJsonArray{1.0, 1.0}, QJsonArray{2.0, 2.0}}}});
    QString failedRunId;
    QVERIFY(engine.runOnceAsync(&failedRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(failedRunId).finished, 10000);
    QVERIFY(!engine.runObservation(failedRunId).result.success);
    QCOMPARE(engine.getModule(QStringLiteral("fit")), fitInstance);
    const QJsonObject failedCircle =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", failedRunId}, {"instanceId", "fit"}, {"port", "circle"}});
    QCOMPARE(failedCircle.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QVERIFY(!failedCircle.contains(QStringLiteral("value")));

    QVERIFY(ProjectManager::instance().newProject());
    project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitcircle_pointset.json", dataDir.path(), *project).isEmpty(),
             "reload FitCircle point-set project without provenance");
    QVERIFY(engine.loadProject(project, [&image](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("grab"))
            return new UnprovenancedImageFixtureModule(image);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString unprovenancedRunId;
    QVERIFY(engine.runOnceAsync(&unprovenancedRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(unprovenancedRunId).finished, 10000);
    QVERIFY(engine.runObservation(unprovenancedRunId).result.success);
    const QJsonObject unprovenancedCircle =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", unprovenancedRunId}, {"instanceId", "fit"}, {"port", "circle"}});
    QCOMPARE(unprovenancedCircle.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(unprovenancedCircle.value(QStringLiteral("value"))
                 .toObject()
                 .value(QStringLiteral("coordinateSystem"))
                 .toString(),
             QStringLiteral("image_pixels"));
    QVERIFY(!unprovenancedCircle.value(QStringLiteral("value"))
                 .toObject()
                 .contains(QStringLiteral("sourceImageCoordinates")));
    const ImageData unprovenancedOutput = engine.moduleOutput(QStringLiteral("fit"));
    QVERIFY(unprovenancedOutput.data(QStringLiteral("circle")).value<Circle2D>().isValid());
    for (const QString& key : {QStringLiteral("circle_center_x"), QStringLiteral("circle_center_y"),
                               QStringLiteral("circle_radius"), QStringLiteral("circle_error")}) {
        for (const bool byPort : {false, true}) {
            QJsonObject args{{"runId", unprovenancedRunId}, {"instanceId", "fit"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), unprovenancedOutput.data(key).toDouble());
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
}

void TestAcceptanceFlows::testFitCircleRejectsPassedCircleWithoutImageFrame() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    for (const QString& id : {QStringLiteral("unspecified"), QStringLiteral("missing"), QStringLiteral("world")}) {
        ModuleInstance source;
        source.id = id;
        source.moduleId = QStringLiteral("MeasurementInput");
        source.name = id;
        project->addModule(source);
        ModuleInstance fit;
        fit.id = QStringLiteral("fit_") + id;
        fit.moduleId = QStringLiteral("FitCircle");
        fit.name = QStringLiteral("FitCircle ") + id;
        project->addModule(fit);
        ModuleConnection imageConnection;
        imageConnection.fromModuleId = source.id;
        imageConnection.toModuleId = fit.id;
        imageConnection.fromPort = QStringLiteral("image");
        imageConnection.toPort = QStringLiteral("image");
        imageConnection.edgeType = QStringLiteral("data");
        project->addConnection(imageConnection);
        ModuleConnection connection;
        connection.fromModuleId = source.id;
        connection.toModuleId = fit.id;
        connection.fromPort = QStringLiteral("fit_points");
        connection.toPort = QStringLiteral("fit_points");
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds =
        QStringList{QStringLiteral("unspecified"), QStringLiteral("fit_unspecified"), QStringLiteral("missing"),
                    QStringLiteral("fit_missing"), QStringLiteral("world"),           QStringLiteral("fit_world")};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("MeasurementInput"))
            return new CircleInputFixtureModule(instance.id == QLatin1String("world") ? QStringLiteral("World3D")
                                                                                      : QString(),
                                                instance.id != QLatin1String("missing"));
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    AgentActor actor;
    for (const QString& id : {QStringLiteral("unspecified"), QStringLiteral("missing"), QStringLiteral("world")}) {
        const QString fitId = QStringLiteral("fit_") + id;
        const QJsonObject circle =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", fitId}, {"port", "circle"}});
        QVERIFY(!circle.contains(QStringLiteral("value")));
        QVERIFY(!circle.contains(QStringLiteral("sourceImageCoordinates")));
        const QJsonObject scalar =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", fitId}, {"key", "circle_center_x"}});
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        const ImageData output = engine.moduleOutput(fitId);
        QVERIFY(!output.hasData(QStringLiteral("circle")));
        for (const QString& key : {QStringLiteral("circle_center_x"), QStringLiteral("circle_center_y"),
                                   QStringLiteral("circle_radius"), QStringLiteral("circle_error")}) {
            for (const bool byPort : {false, true}) {
                QJsonObject args{{"runId", runId}, {"instanceId", fitId}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
                const QJsonObject value = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QCOMPARE(value.value(QStringLiteral("value")).toDouble(), output.data(key).toDouble());
                QVERIFY(!value.contains(QStringLiteral("unit")));
                QVERIFY(!value.contains(QStringLiteral("coordinateSystem")));
                QVERIFY(!value.contains(QStringLiteral("sourceImageCoordinates")));
            }
        }
    }
}

void TestAcceptanceFlows::testAgentFitCircleScalarUnitsRequireTypedCircleOutput() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance fit;
    fit.id = QStringLiteral("fit");
    fit.moduleId = QStringLiteral("FitCircle");
    project->addModule(fit);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{fit.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance&) -> ModuleBase* {
        return new FitCircleScalarOutputWithoutCircleFixtureModule();
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    const ImageData output = engine.moduleOutput(fit.id);
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    QVERIFY(!output.hasData(QStringLiteral("circle")));

    AgentActor actor;
    for (const QString& key : {QStringLiteral("circle_center_x"), QStringLiteral("circle_center_y"),
                               QStringLiteral("circle_radius"), QStringLiteral("circle_error")}) {
        for (const bool byPort : {false, true}) {
            QJsonObject args{{"runId", runId}, {"instanceId", fit.id}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
            const QJsonObject value = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(value.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(value.value(QStringLiteral("value")).toDouble(), output.data(key).toDouble());
            QVERIFY(!value.contains(QStringLiteral("unit")));
            QVERIFY(!value.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!value.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!value.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
}

void TestAcceptanceFlows::testFitCircleClearsPassedCircleBeforeFitFailure() {
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance source;
    source.id = QStringLiteral("source");
    source.moduleId = QStringLiteral("MeasurementInput");
    source.name = QStringLiteral("World point fixture");
    project->addModule(source);
    ModuleInstance fit;
    fit.id = QStringLiteral("fit");
    fit.moduleId = QStringLiteral("FitCircle");
    fit.name = QStringLiteral("Failing FitCircle");
    project->addModule(fit);
    ModuleConnection imageConnection;
    imageConnection.fromModuleId = source.id;
    imageConnection.toModuleId = fit.id;
    imageConnection.fromPort = QStringLiteral("image");
    imageConnection.toPort = QStringLiteral("image");
    imageConnection.edgeType = QStringLiteral("data");
    project->addConnection(imageConnection);
    ModuleConnection pointsConnection;
    pointsConnection.fromModuleId = source.id;
    pointsConnection.toModuleId = fit.id;
    pointsConnection.fromPort = QStringLiteral("fit_points");
    pointsConnection.toPort = QStringLiteral("fit_points");
    pointsConnection.edgeType = QStringLiteral("data");
    project->addConnection(pointsConnection);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{source.id, fit.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("MeasurementInput"))
            return new CircleInputFixtureModule(QStringLiteral("World3D"), true, true);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(!engine.runObservation(runId).result.success);
    QVERIFY(!engine.moduleOutput(QStringLiteral("fit")).hasData(QStringLiteral("circle")));
    const QJsonObject circle = AgentActor().executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"port", "circle"}});
    QCOMPARE(circle.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QVERIFY(!circle.contains(QStringLiteral("value")));
}

void TestAcceptanceFlows::testFitLineTypedOutputAndSourceCoordinates() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("fitline.png"))));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitline_pointset.json", dataDir.path(), *project).isEmpty(),
             "load FitLine point-set project");
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const ImageData fitOutput = engine.moduleOutput(QStringLiteral("fit"));
    QCOMPARE(fitOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    const double expectedFitError = fitOutput.data(QStringLiteral("line_error")).toDouble();
    for (bool byPort : {true, false}) {
        QJsonObject args{{"runId", runId}, {"instanceId", "fit"}};
        args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), QStringLiteral("line_error"));
        const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
        QVERIFY(std::abs(scalar.value(QStringLiteral("value")).toDouble() - expectedFitError) <= 1e-9);
        QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
    }

    const PluginInfo fitInfo = PluginManager::instance().pluginInfo(QStringLiteral("FitLine"));
    const auto typedPort = std::find_if(fitInfo.outputPorts.cbegin(), fitInfo.outputPorts.cend(),
                                        [](const PortSpec& port) { return port.id == QLatin1String("line"); });
    QVERIFY(typedPort != fitInfo.outputPorts.cend());
    QCOMPARE(typedPort->type, DataType::Line2D);
    ImageData fitSnapshot;
    QMap<QString, QVariant> fitSnapshotKeys;
    PortValueMap fitSnapshotPorts;
    QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("fit"), fitSnapshot, fitSnapshotKeys, fitSnapshotPorts));
    const QVariantList fitLine = fitSnapshotPorts.value(QStringLiteral("line")).toList();
    QCOMPARE(fitLine.size(), 4);
    const QList<QPair<QString, int>> endpointFields{{QStringLiteral("line_col1"), 0},
                                                    {QStringLiteral("line_row1"), 1},
                                                    {QStringLiteral("line_col2"), 2},
                                                    {QStringLiteral("line_row2"), 3}};
    for (const auto& [field, component] : endpointFields) {
        const QVariant snapshotValue = fitSnapshotKeys.value(field);
        QVERIFY(snapshotValue.isValid());
        bool expectedOk = false;
        const double expected = snapshotValue.toDouble(&expectedOk);
        QVERIFY(expectedOk);
        QVERIFY(std::isfinite(expected));
        QCOMPARE(expected, fitLine.at(component).toDouble());
        for (const bool byPort : {false, true}) {
            QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), "fit"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), field);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(scalar.value(QStringLiteral("value")).isDouble());
            QVERIFY(std::isfinite(scalar.value(QStringLiteral("value")).toDouble()));
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), expected);
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
    const QJsonObject response = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"port", "line"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject line = response.value(QStringLiteral("value")).toObject();
    QCOMPARE(line.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    const QJsonArray start = line.value(QStringLiteral("start")).toArray();
    const QJsonArray end = line.value(QStringLiteral("end")).toArray();
    QCOMPARE(start.size(), 2);
    QCOMPARE(end.size(), 2);
    const auto matchesPoint = [](const QJsonArray& point, double x, double y) {
        return std::abs(point[0].toDouble() - x) < 1e-3 && std::abs(point[1].toDouble() - y) < 1e-3;
    };
    QVERIFY((matchesPoint(start, -75, 25) && matchesPoint(end, 125, 25)) ||
            (matchesPoint(start, 125, 25) && matchesPoint(end, -75, 25)));
    for (const auto& [key, value] : {std::pair<QString, double>{QStringLiteral("line_col1"), start[0].toDouble()},
                                     {QStringLiteral("line_row1"), start[1].toDouble()},
                                     {QStringLiteral("line_col2"), end[0].toDouble()},
                                     {QStringLiteral("line_row2"), end[1].toDouble()}}) {
        const QJsonObject scalar = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"key", key}});
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), value);
    }
    const ImageCoordinateProvenance provenance = engine.moduleOutput(QStringLiteral("crop")).coordinateProvenance();
    const QString grabSourceId = engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId;
    QCOMPARE(provenance.sourceImageId, grabSourceId);
    const auto expectedStart = provenance.mapLocalToSource(QPointF(start[0].toDouble(), start[1].toDouble()));
    const auto expectedEnd = provenance.mapLocalToSource(QPointF(end[0].toDouble(), end[1].toDouble()));
    QVERIFY(expectedStart);
    QVERIFY(expectedEnd);
    const QJsonObject source = line.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), grabSourceId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    const QJsonArray sourceStart = source.value(QStringLiteral("start")).toArray();
    const QJsonArray sourceEnd = source.value(QStringLiteral("end")).toArray();
    QVERIFY(std::abs(sourceStart[0].toDouble() - expectedStart->x()) < 1e-8);
    QVERIFY(std::abs(sourceStart[1].toDouble() - expectedStart->y()) < 1e-8);
    QVERIFY(std::abs(sourceEnd[0].toDouble() - expectedEnd->x()) < 1e-8);
    QVERIFY(std::abs(sourceEnd[1].toDouble() - expectedEnd->y()) < 1e-8);
    QVERIFY((matchesPoint(sourceStart, -25, 65) && matchesPoint(sourceEnd, 175, 65)) ||
            (matchesPoint(sourceStart, 175, 65) && matchesPoint(sourceEnd, -25, 65)));

    QVERIFY(ProjectManager::instance().newProject());
    project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitline_pointset.json", dataDir.path(), *project).isEmpty(),
             "reload FitLine point-set project without provenance");
    QVERIFY(engine.loadProject(project, [&image](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("grab"))
            return new UnprovenancedImageFixtureModule(image);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString unprovenancedRunId;
    QVERIFY(engine.runOnceAsync(&unprovenancedRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(unprovenancedRunId).finished, 10000);
    QVERIFY(engine.runObservation(unprovenancedRunId).result.success);
    const QJsonObject localOnly =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", unprovenancedRunId}, {"instanceId", "fit"}, {"port", "line"}});
    QCOMPARE(localOnly.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject localLine = localOnly.value(QStringLiteral("value")).toObject();
    QCOMPARE(localLine.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QVERIFY(!localLine.contains(QStringLiteral("sourceImageCoordinates")));
    const QJsonArray localStart = localLine.value(QStringLiteral("start")).toArray();
    const QJsonArray localEnd = localLine.value(QStringLiteral("end")).toArray();
    const QJsonObject localRow1 =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", unprovenancedRunId}, {"instanceId", "fit"}, {"key", "line_row1"}});
    const QJsonObject localCol1 =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", unprovenancedRunId}, {"instanceId", "fit"}, {"key", "line_col1"}});
    QCOMPARE(localStart[0].toDouble(), localCol1.value(QStringLiteral("value")).toDouble());
    QCOMPARE(localStart[1].toDouble(), localRow1.value(QStringLiteral("value")).toDouble());
    const QJsonObject localRow2 =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", unprovenancedRunId}, {"instanceId", "fit"}, {"key", "line_row2"}});
    const QJsonObject localCol2 =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", unprovenancedRunId}, {"instanceId", "fit"}, {"key", "line_col2"}});
    QCOMPARE(localEnd[0].toDouble(), localCol2.value(QStringLiteral("value")).toDouble());
    QCOMPARE(localEnd[1].toDouble(), localRow2.value(QStringLiteral("value")).toDouble());
}

void TestAcceptanceFlows::testFitLineEndpointScalarUnitsRequireMatchingLineOutput() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance input;
    input.id = QStringLiteral("input");
    input.moduleId = QStringLiteral("MeasurementInput");
    project->addModule(input);
    ModuleInstance fit;
    fit.id = QStringLiteral("fit");
    fit.moduleId = QStringLiteral("FitLine");
    project->addModule(fit);
    for (const QString& port : {QStringLiteral("image"), QStringLiteral("fit_points")}) {
        ModuleConnection connection;
        connection.fromModuleId = QStringLiteral("input");
        connection.fromPort = port;
        connection.toModuleId = QStringLiteral("fit");
        connection.toPort = port;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{QStringLiteral("input"), QStringLiteral("fit")};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("MeasurementInput"))
            return new FitLineInputFixtureModule(QStringLiteral("ImageLocalPixel"));
        if (instance.moduleId == QLatin1String("FitLine"))
            return new FitLineEndpointMismatchFixtureModule();
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    ImageData snapshot;
    QMap<QString, QVariant> snapshotKeys;
    PortValueMap snapshotPorts;
    QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("fit"), snapshot, snapshotKeys, snapshotPorts));
    QCOMPARE(snapshot.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    const QVariantList line = snapshotPorts.value(QStringLiteral("line")).toList();
    QCOMPARE(line, QVariantList({1.0, 2.0, 3.0, 4.0}));
    const PluginInfo fitInfo = PluginManager::instance().pluginInfo(QStringLiteral("FitLine"));
    const auto typedLine = std::find_if(fitInfo.outputPorts.cbegin(), fitInfo.outputPorts.cend(),
                                        [](const PortSpec& port) { return port.id == QLatin1String("line"); });
    QVERIFY(typedLine != fitInfo.outputPorts.cend());
    QCOMPARE(typedLine->type, DataType::Line2D);

    AgentActor actor;
    const QList<QPair<QString, int>> endpointFields{{QStringLiteral("line_col1"), 0},
                                                    {QStringLiteral("line_row1"), 1},
                                                    {QStringLiteral("line_col2"), 2},
                                                    {QStringLiteral("line_row2"), 3}};
    for (const auto& [field, component] : endpointFields) {
        const QVariant expectedValue = snapshotKeys.value(field);
        QVERIFY(expectedValue.isValid());
        bool expectedOk = false;
        const double expected = expectedValue.toDouble(&expectedOk);
        QVERIFY(expectedOk);
        QVERIFY(std::isfinite(expected));
        QVERIFY(expected != line.at(component).toDouble());
        for (const bool byPort : {false, true}) {
            QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), "fit"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), field);
            const QJsonObject response = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(response.value(QStringLiteral("value")).isDouble());
            QVERIFY(std::isfinite(response.value(QStringLiteral("value")).toDouble()));
            QCOMPARE(response.value(QStringLiteral("value")).toDouble(), expected);
            QVERIFY(!response.contains(QStringLiteral("unit")));
            QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
}

void TestAcceptanceFlows::testFitLineClearsPassedOutputAcrossFramesAndFailure() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    for (const QString& id : {QStringLiteral("unspecified"), QStringLiteral("world"), QStringLiteral("image")}) {
        ModuleInstance source;
        source.id = id;
        source.moduleId = QStringLiteral("MeasurementInput");
        source.name = id;
        project->addModule(source);
        ModuleInstance fit;
        fit.id = QStringLiteral("fit_") + id;
        fit.moduleId = QStringLiteral("FitLine");
        fit.name = fit.id;
        project->addModule(fit);
        for (const QString& port : {QStringLiteral("image"), QStringLiteral("fit_points")}) {
            ModuleConnection connection;
            connection.fromModuleId = id;
            connection.toModuleId = fit.id;
            connection.fromPort = port;
            connection.toPort = port;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        }
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds =
        QStringList{QStringLiteral("unspecified"), QStringLiteral("fit_unspecified"), QStringLiteral("world"),
                    QStringLiteral("fit_world"),   QStringLiteral("image"),           QStringLiteral("fit_image")};
    project->setFlows({flow});

    QHash<QString, FitLineInputFixtureModule*> inputs;
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [&inputs](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("MeasurementInput")) {
            const bool local = instance.id == QLatin1String("image");
            auto* input =
                new FitLineInputFixtureModule(local ? QStringLiteral("ImageLocalPixel")
                                              : instance.id == QLatin1String("world") ? QStringLiteral("World3D")
                                                                                      : QString(),
                                              instance.id != QLatin1String("unspecified"));
            inputs.insert(instance.id, input);
            return input;
        }
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    AgentActor actor;
    for (const QString& id : {QStringLiteral("unspecified"), QStringLiteral("world")}) {
        const QString fitId = QStringLiteral("fit_") + id;
        const ImageData fitOutput = engine.moduleOutput(fitId);
        if (id == QLatin1String("world"))
            QCOMPARE(fitOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("World3D"));
        else
            QVERIFY(!fitOutput.hasData(QStringLiteral("coordinate_frame")));
        const QJsonObject line =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", fitId}, {"port", "line"}});
        QVERIFY(!line.contains(QStringLiteral("value")));
        ImageData snapshot;
        QMap<QString, QVariant> snapshotKeys;
        PortValueMap snapshotPorts;
        QVERIFY(engine.runOutputSnapshot(runId, fitId, snapshot, snapshotKeys, snapshotPorts));
        for (const QString& field : {QStringLiteral("line_row1"), QStringLiteral("line_col1"),
                                     QStringLiteral("line_row2"), QStringLiteral("line_col2")}) {
            const QVariant expectedValue = snapshotKeys.value(field);
            QVERIFY(expectedValue.isValid());
            bool expectedOk = false;
            const double expected = expectedValue.toDouble(&expectedOk);
            QVERIFY(expectedOk);
            QVERIFY(std::isfinite(expected));
            for (const bool byPort : {false, true}) {
                QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), fitId}};
                args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), field);
                const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
                QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QVERIFY(scalar.value(QStringLiteral("value")).isDouble());
                QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), expected);
                QVERIFY(!scalar.contains(QStringLiteral("unit")));
                QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
                QVERIFY(!scalar.contains(QStringLiteral("coordinateFrame")));
                QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
            }
        }
        const double expectedFitError = fitOutput.data(QStringLiteral("line_error")).toDouble();
        for (bool byPort : {true, false}) {
            QJsonObject args{{"runId", runId}, {"instanceId", fitId}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), QStringLiteral("line_error"));
            const QJsonObject fitError = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(fitError.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(std::abs(fitError.value(QStringLiteral("value")).toDouble() - expectedFitError) <= 1e-9);
            QVERIFY(!fitError.contains(QStringLiteral("unit")));
        }
    }
    const QJsonObject localLine =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "fit_image"}, {"port", "line"}});
    QCOMPARE(localLine.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QVERIFY(
        std::abs(
            localLine.value(QStringLiteral("value")).toObject().value(QStringLiteral("start")).toArray()[0].toDouble() -
            999.0) > 1.0);
    const ImageData localFitOutput = engine.moduleOutput(QStringLiteral("fit_image"));
    QCOMPARE(localFitOutput.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("ImageLocalPixel"));
    ImageData localSnapshot;
    QMap<QString, QVariant> localSnapshotKeys;
    PortValueMap localSnapshotPorts;
    QVERIFY(engine.runOutputSnapshot(runId, QStringLiteral("fit_image"), localSnapshot, localSnapshotKeys,
                                     localSnapshotPorts));
    const QVariantList localTypedLine = localSnapshotPorts.value(QStringLiteral("line")).toList();
    QCOMPARE(localTypedLine.size(), 4);
    const QList<QPair<QString, int>> endpointFields{{QStringLiteral("line_col1"), 0},
                                                    {QStringLiteral("line_row1"), 1},
                                                    {QStringLiteral("line_col2"), 2},
                                                    {QStringLiteral("line_row2"), 3}};
    for (const auto& [field, component] : endpointFields) {
        const double expected = localSnapshotKeys.value(field).toDouble();
        QCOMPARE(expected, localTypedLine.at(component).toDouble());
        for (const bool byPort : {false, true}) {
            QJsonObject args{{QStringLiteral("runId"), runId}, {QStringLiteral("instanceId"), "fit_image"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), field);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), expected);
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
    const double expectedLocalFitError = localFitOutput.data(QStringLiteral("line_error")).toDouble();
    for (bool byPort : {true, false}) {
        QJsonObject args{{"runId", runId}, {"instanceId", "fit_image"}};
        args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), QStringLiteral("line_error"));
        const QJsonObject fitError = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(fitError.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(fitError.value(QStringLiteral("unit")).toString(), QStringLiteral("px"));
        QVERIFY(std::abs(fitError.value(QStringLiteral("value")).toDouble() - expectedLocalFitError) <= 1e-9);
    }

    ModuleBase* fitImage = engine.getModule(QStringLiteral("fit_image"));
    inputs.value(QStringLiteral("image"))->setInsufficientPoints(true);
    QString failedRunId;
    QVERIFY(engine.runOnceAsync(&failedRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(failedRunId).finished, 10000);
    QVERIFY(!engine.runObservation(failedRunId).result.success);
    QCOMPARE(engine.getModule(QStringLiteral("fit_image")), fitImage);
    QVERIFY(!engine.moduleOutput(QStringLiteral("fit_image")).hasData(QStringLiteral("line")));
    const QJsonObject failedLine =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", failedRunId}, {"instanceId", "fit_image"}, {"port", "line"}});
    QCOMPARE(failedLine.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QVERIFY(!failedLine.contains(QStringLiteral("value")));
}

void TestAcceptanceFlows::testFitLineOmitsSourceCoordinatesAcrossHorizon() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance source;
    source.id = QStringLiteral("source");
    source.moduleId = QStringLiteral("MeasurementInput");
    project->addModule(source);
    ModuleInstance fit;
    fit.id = QStringLiteral("fit");
    fit.moduleId = QStringLiteral("FitLine");
    project->addModule(fit);
    for (const QString& port : {QStringLiteral("image"), QStringLiteral("fit_points")}) {
        ModuleConnection connection;
        connection.fromModuleId = source.id;
        connection.toModuleId = fit.id;
        connection.fromPort = port;
        connection.toPort = port;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{source.id, fit.id};
    project->setFlows({flow});
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("source"))
            return new FitLineInputFixtureModule(QStringLiteral("ImageLocalPixel"), true, false, true);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    const QJsonObject response = AgentActor().executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"port", "line"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject line = response.value(QStringLiteral("value")).toObject();
    QVERIFY(!line.contains(QStringLiteral("sourceImageCoordinates")));
    const QJsonArray start = line.value(QStringLiteral("start")).toArray();
    const QJsonArray end = line.value(QStringLiteral("end")).toArray();
    QVERIFY(std::abs(start[0].toDouble() - 999.0) > 1.0);
    const QJsonObject row =
        AgentActor().executeTool(QStringLiteral("get_module_output"),
                                 QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"key", "line_row1"}});
    const QJsonObject col =
        AgentActor().executeTool(QStringLiteral("get_module_output"),
                                 QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"key", "line_col1"}});
    const bool startMatches = start[0].toDouble() == col.value(QStringLiteral("value")).toDouble() &&
                              start[1].toDouble() == row.value(QStringLiteral("value")).toDouble();
    const QJsonObject row2 =
        AgentActor().executeTool(QStringLiteral("get_module_output"),
                                 QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"key", "line_row2"}});
    const QJsonObject col2 =
        AgentActor().executeTool(QStringLiteral("get_module_output"),
                                 QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"key", "line_col2"}});
    const bool endMatches = end[0].toDouble() == col2.value(QStringLiteral("value")).toDouble() &&
                            end[1].toDouble() == row2.value(QStringLiteral("value")).toDouble();
    QVERIFY(startMatches && endMatches);
}

void TestAcceptanceFlows::testFitEllipseTypedOutputAndSourceCenter() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("fitellipse.png"))));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitellipse_pointset.json", dataDir.path(), *project).isEmpty(),
             "load FitEllipse point-set project");
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString runId = started.value(QStringLiteral("runId")).toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    const PluginInfo fitInfo = PluginManager::instance().pluginInfo(QStringLiteral("FitEllipse"));
    const auto typedPort = std::find_if(fitInfo.outputPorts.cbegin(), fitInfo.outputPorts.cend(),
                                        [](const PortSpec& port) { return port.id == QLatin1String("ellipse"); });
    QVERIFY(typedPort != fitInfo.outputPorts.cend());
    QCOMPARE(typedPort->type, DataType::Ellipse2D);
    const QJsonObject response = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"port", "ellipse"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Ellipse2D"));
    const QJsonObject ellipse = response.value(QStringLiteral("value")).toObject();
    QCOMPARE(ellipse.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("image_pixels"));
    QVERIFY(std::abs(ellipse.value(QStringLiteral("centerX")).toDouble() - 25.0) < 0.1);
    QVERIFY(std::abs(ellipse.value(QStringLiteral("centerY")).toDouble() - 25.0) < 0.1);
    QVERIFY(std::abs(ellipse.value(QStringLiteral("majorRadius")).toDouble() - 12.0) < 0.2);
    QVERIFY(std::abs(ellipse.value(QStringLiteral("minorRadius")).toDouble() - 7.0) < 0.2);
    const double angle = ellipse.value(QStringLiteral("angleDegrees")).toDouble();
    QVERIFY(std::min(angle, 180.0 - angle) < 2.0);
    for (const auto& [key, field] :
         {std::pair<QString, QString>{QStringLiteral("ellipse_center_x"), QStringLiteral("centerX")},
          {QStringLiteral("ellipse_center_y"), QStringLiteral("centerY")},
          {QStringLiteral("ellipse_major_r"), QStringLiteral("majorRadius")},
          {QStringLiteral("ellipse_minor_r"), QStringLiteral("minorRadius")},
          {QStringLiteral("ellipse_phi"), QStringLiteral("angleDegrees")}}) {
        const QJsonObject scalar = actor.executeTool(
            QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"key", key}});
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(scalar.value(QStringLiteral("value")).toDouble(), ellipse.value(field).toDouble());
        const QString expectedUnit = key == QLatin1String("ellipse_phi") ? QStringLiteral("deg") : QStringLiteral("px");
        QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), expectedUnit);
        const QJsonObject port = actor.executeTool(QStringLiteral("get_module_output"),
                                                   QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"port", key}});
        QCOMPARE(port.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(port.value(QStringLiteral("value")).toDouble(), scalar.value(QStringLiteral("value")).toDouble());
        QCOMPARE(port.value(QStringLiteral("unit")).toString(), expectedUnit);
    }
    for (const QString& key : {QStringLiteral("ellipse_ellipticity"), QStringLiteral("ellipse_error")}) {
        for (const QString& selector : {QStringLiteral("key"), QStringLiteral("port")}) {
            const QJsonObject scalar =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {selector, key}});
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QVERIFY(!scalar.contains(QStringLiteral("unit")));
        }
    }
    const QString grabSourceId = engine.moduleOutput(QStringLiteral("grab")).coordinateProvenance().sourceImageId;
    const QJsonObject source = ellipse.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(source.value(QStringLiteral("sourceImageId")).toString(), grabSourceId);
    QCOMPARE(source.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    QVERIFY(std::abs(source.value(QStringLiteral("centerX")).toDouble() - 75.0) < 0.1);
    QVERIFY(std::abs(source.value(QStringLiteral("centerY")).toDouble() - 65.0) < 0.1);
    QVERIFY(!source.contains(QStringLiteral("majorRadius")));
    QVERIFY(!source.contains(QStringLiteral("minorRadius")));
    QVERIFY(!source.contains(QStringLiteral("angleDegrees")));

    ModuleBase* fitInstance = engine.getModule(QStringLiteral("fit"));
    ModuleBase* inputInstance = engine.getModule(QStringLiteral("input"));
    QVERIFY(fitInstance);
    QVERIFY(inputInstance);
    inputInstance->setParams(QJsonObject{{"mode", "point_set"}, {"points", QJsonArray{QJsonArray{13.0, 25.0}}}});
    QString failedRunId;
    QVERIFY(engine.runOnceAsync(&failedRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(failedRunId).finished, 10000);
    QVERIFY(!engine.runObservation(failedRunId).result.success);
    QCOMPARE(engine.getModule(QStringLiteral("fit")), fitInstance);
    QVERIFY(!engine.moduleOutput(QStringLiteral("fit")).hasData(QStringLiteral("ellipse")));
    const QJsonObject failed =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", failedRunId}, {"instanceId", "fit"}, {"port", "ellipse"}});
    QCOMPARE(failed.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QVERIFY(!failed.contains(QStringLiteral("value")));

    QVERIFY(ProjectManager::instance().newProject());
    project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitellipse_pointset.json", dataDir.path(), *project).isEmpty(),
             "reload FitEllipse point-set project without provenance");
    QVERIFY(engine.loadProject(project, [&image](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("grab"))
            return new UnprovenancedImageFixtureModule(image);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString unprovenancedRunId;
    QVERIFY(engine.runOnceAsync(&unprovenancedRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(unprovenancedRunId).finished, 10000);
    QVERIFY(engine.runObservation(unprovenancedRunId).result.success);
    const QJsonObject localOnly =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", unprovenancedRunId}, {"instanceId", "fit"}, {"port", "ellipse"}});
    QCOMPARE(localOnly.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QVERIFY(!localOnly.value(QStringLiteral("value")).toObject().contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testFitEllipseClearsPassedOutputAcrossFramesAndFailure() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    for (const QString& id : {QStringLiteral("unspecified"), QStringLiteral("world"), QStringLiteral("image")}) {
        ModuleInstance source;
        source.id = id;
        source.moduleId = QStringLiteral("MeasurementInput");
        source.name = id;
        project->addModule(source);
        ModuleInstance fit;
        fit.id = QStringLiteral("fit_") + id;
        fit.moduleId = QStringLiteral("FitEllipse");
        fit.name = fit.id;
        project->addModule(fit);
        for (const QString& port : {QStringLiteral("image"), QStringLiteral("fit_points")}) {
            ModuleConnection connection;
            connection.fromModuleId = id;
            connection.toModuleId = fit.id;
            connection.fromPort = port;
            connection.toPort = port;
            connection.edgeType = QStringLiteral("data");
            project->addConnection(connection);
        }
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds =
        QStringList{QStringLiteral("unspecified"), QStringLiteral("fit_unspecified"), QStringLiteral("world"),
                    QStringLiteral("fit_world"),   QStringLiteral("image"),           QStringLiteral("fit_image")};
    project->setFlows({flow});
    QHash<QString, FitEllipseInputFixtureModule*> inputs;
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [&inputs](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("MeasurementInput")) {
            const bool local = instance.id == QLatin1String("image");
            auto* input =
                new FitEllipseInputFixtureModule(local ? QStringLiteral("ImageLocalPixel")
                                                 : instance.id == QLatin1String("world") ? QStringLiteral("World3D")
                                                                                         : QString(),
                                                 instance.id != QLatin1String("unspecified"));
            inputs.insert(instance.id, input);
            return input;
        }
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    AgentActor actor;
    for (const QString& id : {QStringLiteral("unspecified"), QStringLiteral("world")}) {
        const QString fitId = QStringLiteral("fit_") + id;
        const QJsonObject ellipse =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", fitId}, {"port", "ellipse"}});
        QVERIFY(!ellipse.contains(QStringLiteral("value")));
        const QJsonObject scalar =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", fitId}, {"key", "ellipse_center_x"}});
        QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        for (const QString& key :
             {QStringLiteral("ellipse_center_x"), QStringLiteral("ellipse_center_y"), QStringLiteral("ellipse_major_r"),
              QStringLiteral("ellipse_minor_r"), QStringLiteral("ellipse_phi")}) {
            for (const QString& selector : {QStringLiteral("key"), QStringLiteral("port")}) {
                const QJsonObject output =
                    actor.executeTool(QStringLiteral("get_module_output"),
                                      QJsonObject{{"runId", runId}, {"instanceId", fitId}, {selector, key}});
                QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
                QVERIFY(!output.contains(QStringLiteral("unit")));
            }
        }
    }
    const QJsonObject localEllipse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", "fit_image"}, {"port", "ellipse"}});
    QCOMPARE(localEllipse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QVERIFY(
        std::abs(localEllipse.value(QStringLiteral("value")).toObject().value(QStringLiteral("centerX")).toDouble() -
                 999.0) > 1.0);
    for (const auto& [key, expectedUnit] :
         {std::pair<QString, QString>{QStringLiteral("ellipse_center_x"), QStringLiteral("px")},
          {QStringLiteral("ellipse_center_y"), QStringLiteral("px")},
          {QStringLiteral("ellipse_major_r"), QStringLiteral("px")},
          {QStringLiteral("ellipse_minor_r"), QStringLiteral("px")},
          {QStringLiteral("ellipse_phi"), QStringLiteral("deg")}}) {
        for (const QString& selector : {QStringLiteral("key"), QStringLiteral("port")}) {
            const QJsonObject output =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{"runId", runId}, {"instanceId", "fit_image"}, {selector, key}});
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(output.value(QStringLiteral("unit")).toString(), expectedUnit);
        }
    }

    ModuleBase* fitImage = engine.getModule(QStringLiteral("fit_image"));
    inputs.value(QStringLiteral("image"))->setInsufficientPoints(true);
    QString failedRunId;
    QVERIFY(engine.runOnceAsync(&failedRunId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(failedRunId).finished, 10000);
    QVERIFY(!engine.runObservation(failedRunId).result.success);
    QCOMPARE(engine.getModule(QStringLiteral("fit_image")), fitImage);
    QVERIFY(!engine.moduleOutput(QStringLiteral("fit_image")).hasData(QStringLiteral("ellipse")));
    const QJsonObject failedEllipse =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", failedRunId}, {"instanceId", "fit_image"}, {"port", "ellipse"}});
    QCOMPARE(failedEllipse.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QVERIFY(!failedEllipse.contains(QStringLiteral("value")));
}

void TestAcceptanceFlows::testFitEllipseKeepsLocalOutputWhenSourceCenterCannotMap() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance source;
    source.id = QStringLiteral("source");
    source.moduleId = QStringLiteral("MeasurementInput");
    project->addModule(source);
    ModuleInstance fit;
    fit.id = QStringLiteral("fit");
    fit.moduleId = QStringLiteral("FitEllipse");
    project->addModule(fit);
    for (const QString& port : {QStringLiteral("image"), QStringLiteral("fit_points")}) {
        ModuleConnection connection;
        connection.fromModuleId = source.id;
        connection.toModuleId = fit.id;
        connection.fromPort = port;
        connection.toPort = port;
        connection.edgeType = QStringLiteral("data");
        project->addConnection(connection);
    }
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = flow.id;
    flow.nodeIds = QStringList{source.id, fit.id};
    project->setFlows({flow});
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("source"))
            return new FitEllipseInputFixtureModule(QStringLiteral("ImageLocalPixel"), true, false, true);
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY(engine.runObservation(runId).result.success);
    const QJsonObject response = AgentActor().executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", "fit"}, {"port", "ellipse"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject ellipse = response.value(QStringLiteral("value")).toObject();
    QVERIFY(!ellipse.contains(QStringLiteral("sourceImageCoordinates")));
    QVERIFY(std::abs(ellipse.value(QStringLiteral("centerX")).toDouble() - 25.0) < 0.1);
}

void TestAcceptanceFlows::testAgentFitPlaneUsesHeightMapScaledCoordinates() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);

    ModuleInstance height;
    height.id = QStringLiteral("height");
    height.moduleId = QStringLiteral("test.HeightMapFixture");
    height.name = QStringLiteral("Height fixture");
    project->addModule(height);
    ModuleInstance fit;
    fit.id = QStringLiteral("fit");
    fit.moduleId = QStringLiteral("FitPlane");
    fit.name = QStringLiteral("Fit plane");
    fit.params = QJsonObject{{"pixelSizeX", 0.5}, {"pixelSizeY", 2.0}, {"zScale", 0.25}, {"autoNoData", false}};
    project->addModule(fit);
    ModuleInstance other;
    other.id = QStringLiteral("other_plane");
    other.moduleId = QStringLiteral("MeasurementInput");
    other.name = QStringLiteral("Other Plane3D");
    project->addModule(other);

    ModuleConnection connection;
    connection.fromModuleId = height.id;
    connection.toModuleId = fit.id;
    connection.fromPort = QStringLiteral("image");
    connection.toPort = QStringLiteral("image");
    connection.edgeType = QStringLiteral("data");
    project->addConnection(connection);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{height.id, fit.id, other.id};
    project->setFlows({flow});

    const QVariantList otherPlane{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [otherPlane](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.moduleId == QLatin1String("test.HeightMapFixture"))
            return new FitPlaneHeightMapFixtureModule();
        if (instance.id == QLatin1String("other_plane"))
            return new FitPlaneOutputFixtureModule(otherPlane, QStringLiteral("MeasurementInput"));
        return qobject_cast<ModuleBase*>(PluginManager::instance().createModule(instance.moduleId));
    }));
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));

    ImageData output;
    QMap<QString, QVariant> named;
    PortValueMap ports;
    QVERIFY(engine.runOutputSnapshot(runId, fit.id, output, named, ports));
    const QVariantList actual = ports.value(QStringLiteral("plane")).toList();
    QCOMPARE(actual.size(), 9);
    for (int i = 0; i < actual.size(); i += 3) {
        const double x = actual.at(i).toDouble();
        const double y = actual.at(i + 1).toDouble();
        const double z = actual.at(i + 2).toDouble();
        QVERIFY(std::abs(z - (1.5 * x + 0.5 * y + 2.5)) < 1e-8);
    }

    AgentActor actor;
    const QJsonObject response = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", fit.id}, {"port", "plane"}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("Plane3D"));
    QCOMPARE(response.value(QStringLiteral("coordinateSystem")).toString(),
             QStringLiteral("height_map_local_scaled_xyz"));
    QCOMPARE(response.value(QStringLiteral("unitBasis")).toArray(), QJsonArray({"pixelSizeX", "pixelSizeY", "zScale"}));
    const QJsonArray jsonPlane = response.value(QStringLiteral("value")).toArray();
    QCOMPARE(jsonPlane.size(), 9);
    for (int i = 0; i < actual.size(); ++i)
        QCOMPARE(jsonPlane.at(i).toDouble(), actual.at(i).toDouble());
    QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    const double normalLength = std::sqrt(3.5);
    const QStringList normalScalars{QStringLiteral("plane_nx"), QStringLiteral("plane_ny"), QStringLiteral("plane_nz")};
    const QList<double> expectedNormal{-1.5 / normalLength, -0.5 / normalLength, 1.0 / normalLength};
    for (int i = 0; i < normalScalars.size(); ++i) {
        const QString& scalar = normalScalars.at(i);
        QCOMPARE(named.value(scalar).toDouble(), output.data(scalar).toDouble());
        QVERIFY(std::abs(output.data(scalar).toDouble() - expectedNormal.at(i)) < 1e-8);
        for (const QString& selector : {QStringLiteral("key"), QStringLiteral("port")}) {
            const QJsonObject scalarResponse =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{"runId", runId}, {"instanceId", fit.id}, {selector, scalar}});
            QCOMPARE(scalarResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            const double value = scalarResponse.value(QStringLiteral("value")).toDouble();
            QCOMPARE(value, named.value(scalar).toDouble());
            QVERIFY(std::abs(value - expectedNormal.at(i)) < 1e-8);
            QVERIFY(!scalarResponse.contains(QStringLiteral("unit")));
            QVERIFY(!scalarResponse.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalarResponse.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!scalarResponse.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }
    for (const QString& scalar :
         {QStringLiteral("plane_d"), QStringLiteral("flatness"), QStringLiteral("max_deviation"),
          QStringLiteral("min_deviation"), QStringLiteral("rms")}) {
        QCOMPARE(output.data(scalar).toDouble(), named.value(scalar).toDouble());
        for (const QString& selector : {QStringLiteral("key"), QStringLiteral("port")}) {
            const QJsonObject scalarResponse =
                actor.executeTool(QStringLiteral("get_module_output"),
                                  QJsonObject{{"runId", runId}, {"instanceId", fit.id}, {selector, scalar}});
            QCOMPARE(scalarResponse.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalarResponse.value(QStringLiteral("value")).toDouble(), output.data(scalar).toDouble());
            QVERIFY(!scalarResponse.contains(QStringLiteral("unit")));
            QVERIFY(!scalarResponse.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalarResponse.contains(QStringLiteral("coordinateFrame")));
            QVERIFY(!scalarResponse.contains(QStringLiteral("sourceImageCoordinates")));
        }
    }

    const QJsonObject keyResponse = actor.executeTool(
        QStringLiteral("get_module_output"), QJsonObject{{"runId", runId}, {"instanceId", fit.id}, {"key", "plane"}});
    QCOMPARE(keyResponse.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
    QVERIFY(!keyResponse.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!keyResponse.contains(QStringLiteral("unitBasis")));

    for (const QString& paging : {QStringLiteral("offset"), QStringLiteral("limit")}) {
        QJsonObject args{{"runId", runId}, {"instanceId", fit.id}, {"port", "plane"}};
        args.insert(paging, 1);
        const QJsonObject paged = actor.executeTool(QStringLiteral("get_module_output"), args);
        QCOMPARE(paged.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
        QVERIFY(!paged.contains(QStringLiteral("value")));
    }
    const QJsonObject genericPlane =
        actor.executeTool(QStringLiteral("get_module_output"),
                          QJsonObject{{"runId", runId}, {"instanceId", other.id}, {"port", "plane"}});
    QCOMPARE(genericPlane.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
    QVERIFY(!genericPlane.contains(QStringLiteral("coordinateSystem")));
    QVERIFY(!genericPlane.contains(QStringLiteral("unitBasis")));
}

void TestAcceptanceFlows::testAgentFitPlaneRejectsMalformedPlanePortValues() {
    ToolSchema::instance().registerDefaultTools();
    const QList<QVariantList> malformed{
        {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0},
        {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0},
        {0.0, 1.0, std::numeric_limits<double>::quiet_NaN(), 3.0, 4.0, 5.0, 6.0, 7.0, 8.0},
        {0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, std::numeric_limits<double>::infinity(), 8.0}};

    RunEngine& engine = RunEngine::instance();
    AgentActor actor;
    for (const QVariantList& plane : malformed) {
        QVERIFY(ProjectManager::instance().newProject());
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance fit;
        fit.id = QStringLiteral("fit");
        fit.moduleId = QStringLiteral("FitPlane");
        fit.name = QStringLiteral("Malformed FitPlane fixture");
        project->addModule(fit);
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = QStringLiteral("main");
        flow.nodeIds = QStringList{fit.id};
        project->setFlows({flow});
        QVERIFY(engine.loadProject(project,
                                   [plane](const ModuleInstance&) { return new FitPlaneOutputFixtureModule(plane); }));
        QString runId;
        QVERIFY(engine.runOnceAsync(&runId));
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", fit.id}, {"port", "plane"}});
        QCOMPARE(response.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!response.contains(QStringLiteral("type")) ||
                response.value(QStringLiteral("type")).toString() != QStringLiteral("Plane3D"));
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("unitBasis")));
    }
}

void TestAcceptanceFlows::testAgentMeasurementInputPointPlanePortsCarry3DSemantics() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    QImage image(160, 120, QImage::Format_RGB32);
    image.fill(Qt::black);
    QVERIFY(image.save(dataDir.filePath(QStringLiteral("measurementinput.png"))));
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const QJsonArray point{1.25, -2.0, 30.5};
    const QJsonArray plane{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    const auto runRealMode = [&](const QString& mode) {
        if (!ProjectManager::instance().newProject())
            return QString();
        Project* project = ProjectManager::instance().currentProject();
        if (!project ||
            !loadProjectWithPlaceholder("projects/accept_measurementinput_pointset.json", dataDir.path(), *project)
                 .isEmpty())
            return QString();
        ModuleInstance input = project->moduleById(QStringLiteral("input")).value();
        input.params[QStringLiteral("mode")] = mode;
        input.params[QStringLiteral("point")] = point;
        input.params[QStringLiteral("plane")] = plane;
        project->updateModule(input.id, input);
        const QJsonObject started = actor.executeTool(QStringLiteral("run_flow"), {});
        const QString runId = started.value(QStringLiteral("runId")).toString();
        if (runId.isEmpty())
            return QString();
        QElapsedTimer elapsed;
        elapsed.start();
        while (!engine.runObservation(runId).finished && elapsed.elapsed() < 10000)
            QTest::qWait(10);
        const RunObservation observation = engine.runObservation(runId);
        return observation.finished && observation.result.success ? runId : QString();
    };
    const auto query = [&actor](const QString& runId, const QString& name, bool byPort) {
        QJsonObject args{{"runId", runId}, {"instanceId", "input"}};
        args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), name);
        return actor.executeTool(QStringLiteral("get_module_output"), args);
    };

    QString runId = runRealMode(QStringLiteral("point_plane"));
    QVERIFY(!runId.isEmpty());
    const ImageData output = engine.moduleOutput(QStringLiteral("input"));
    QCOMPARE(output.data(QStringLiteral("measurement_input_mode")).toString(), QStringLiteral("point_plane"));
    QCOMPARE(output.data(QStringLiteral("coordinate_frame")).toString(), QStringLiteral("World3D"));
    QVERIFY(output.coordinateProvenance().isValid());
    QCOMPARE(output.coordinateProvenance().localOriginInSource, QPointF(50, 40));
    for (const auto& expected :
         {qMakePair(QStringLiteral("point"), point), qMakePair(QStringLiteral("plane"), plane)}) {
        const QJsonObject response = query(runId, expected.first, true);
        QCOMPARE(response.value(QStringLiteral("type")).toString(),
                 expected.first == QLatin1String("point") ? QStringLiteral("Point3D") : QStringLiteral("Plane3D"));
        QCOMPARE(response.value(QStringLiteral("coordinateFrame")).toString(), QStringLiteral("World3D"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), expected.second);
        QVERIFY(!response.contains(QStringLiteral("coordinateSystem")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
        const QJsonObject keyResponse = query(runId, expected.first, false);
        QCOMPARE(keyResponse.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(keyResponse.value(QStringLiteral("value")).toArray(), expected.second);
        QVERIFY(!keyResponse.contains(QStringLiteral("coordinateFrame")));
        QVERIFY(!keyResponse.contains(QStringLiteral("sourceImageCoordinates")));
        for (const QString& parameter :
             {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("maskOffset"),
              QStringLiteral("polygonOffset"), QStringLiteral("polygonLimit")}) {
            QJsonObject args{{"runId", runId}, {"instanceId", "input"}, {"port", expected.first}};
            args.insert(parameter, 0);
            const QJsonObject rejected = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("INVALID_PARAM"));
            QVERIFY(!rejected.contains(QStringLiteral("value")));
        }
    }

    runId = runRealMode(QStringLiteral("custom"));
    QVERIFY(!runId.isEmpty());
    QCOMPARE(engine.moduleOutput(QStringLiteral("input")).data(QStringLiteral("coordinate_frame")).toString(),
             QStringLiteral("Unspecified"));
    for (const QString& name : {QStringLiteral("point"), QStringLiteral("plane")}) {
        const QJsonObject response = query(runId, name, true);
        QCOMPARE(response.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
        QCOMPARE(response.value(QStringLiteral("value")).toArray(), name == QLatin1String("point") ? point : plane);
        QVERIFY(!response.contains(QStringLiteral("coordinateFrame")));
        QVERIFY(!response.contains(QStringLiteral("sourceImageCoordinates")));
    }
}

void TestAcceptanceFlows::testAgentMeasurementInputPointPlaneRejectsMalformedOutputs() {
    ToolSchema::instance().registerDefaultTools();
    const QVariantList validPoint{1.0, 2.0, 3.0};
    const QVariantList validPlane{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0};
    QList<QPair<QString, QVariant>> malformed;
    malformed.append(qMakePair(QStringLiteral("point"), QVariant(QVariantList{1.0, 2.0})));
    malformed.append(
        qMakePair(QStringLiteral("point"), QVariant(QVariantList{1.0, 2.0, std::numeric_limits<double>::quiet_NaN()})));
    malformed.append(
        qMakePair(QStringLiteral("point"), QVariant(QVariantList{1.0, 2.0, std::numeric_limits<double>::infinity()})));
    malformed.append(qMakePair(QStringLiteral("point"), QVariant(QVariantList{1.0, QStringLiteral("3"), 3.0})));
    malformed.append(qMakePair(QStringLiteral("point"), QVariant(QVariantList{1.0, true, 3.0})));
    malformed.append(
        qMakePair(QStringLiteral("plane"), QVariant(QVariantList{0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0})));
    for (const double nonFinite : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
        QVariantList values = validPlane;
        values[3] = nonFinite;
        malformed.append(qMakePair(QStringLiteral("plane"), QVariant(values)));
    }
    for (const QVariant& invalid : {QVariant(QStringLiteral("4")), QVariant(true)}) {
        QVariantList values = validPlane;
        values[4] = invalid;
        malformed.append(qMakePair(QStringLiteral("plane"), QVariant(values)));
    }

    RunEngine& engine = RunEngine::instance();
    AgentActor actor;
    for (const auto& testCase : malformed) {
        QString runId;
        if (!ProjectManager::instance().newProject())
            QFAIL("create fixture project");
        Project* project = ProjectManager::instance().currentProject();
        QVERIFY(project);
        ModuleInstance input;
        input.id = QStringLiteral("input");
        input.moduleId = QStringLiteral("MeasurementInput");
        project->addModule(input);
        ProjectFlow flow;
        flow.id = QStringLiteral("main");
        flow.name = flow.id;
        flow.nodeIds = QStringList{input.id};
        project->setFlows({flow});
        const QVariant point = testCase.first == QLatin1String("point") ? testCase.second : QVariant(validPoint);
        const QVariant plane = testCase.first == QLatin1String("plane") ? testCase.second : QVariant(validPlane);
        QVERIFY(engine.loadProject(project, [point, plane](const ModuleInstance&) {
            return new MeasurementInput3DOutputFixtureModule(point, plane);
        }));
        QVERIFY(engine.runOnceAsync(&runId));
        QVERIFY(!runId.isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
        QVERIFY(engine.runObservation(runId).result.success);
        const QJsonObject response =
            actor.executeTool(QStringLiteral("get_module_output"),
                              QJsonObject{{"runId", runId}, {"instanceId", input.id}, {"port", testCase.first}});
        QCOMPARE(response.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
        QVERIFY(!response.contains(QStringLiteral("value")));
    }
}

void TestAcceptanceFlows::testAgentLine2DSerializationBoundaries() {
    ToolSchema::instance().registerDefaultTools();
    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const auto runFixture = [&engine](const QVariantList& line, const ImageCoordinateProvenance& provenance,
                                      QString& runId) {
        Project* project = ProjectManager::instance().newProject();
        if (!project)
            return false;
        ModuleInstance instance;
        instance.id = QStringLiteral("measure");
        instance.moduleId = QStringLiteral("MeasureLine");
        project->addModule(instance);
        if (!engine.loadProject(project, [line, provenance](const ModuleInstance&) {
                return new LineOutputFixtureModule(line, provenance);
            }))
            return false;
        return engine.runOnceAsync(&runId);
    };
    const auto query = [&actor](const QString& runId, const QString& key) {
        return actor.executeTool(QStringLiteral("get_module_output"),
                                 QJsonObject{{"runId", runId}, {"instanceId", "measure"}, {"key", key}});
    };
    const auto provenanceFor = [](const ImageCoordinateProvenance::Matrix3& matrix) {
        ImageCoordinateProvenance provenance;
        provenance.sourceImageId = QStringLiteral("12345678-1234-4234-9234-123456789abc");
        provenance.sourceSize = QSize(300, 200);
        provenance.hasLocalToSourceTransform = true;
        provenance.localToSource = matrix;
        return provenance;
    };
    const auto runAndWait = [&runFixture, &engine](const QVariantList& values,
                                                   const ImageCoordinateProvenance& provenance, QString& runId) {
        if (!runFixture(values, provenance, runId))
            return false;
        QElapsedTimer elapsed;
        elapsed.start();
        while (!engine.runObservation(runId).finished && elapsed.elapsed() < 3000)
            QTest::qWait(10);
        const RunObservation observation = engine.runObservation(runId);
        return observation.finished && observation.result.success;
    };

    const auto identity = provenanceFor({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0});
    QString runId;
    QVERIFY(runAndWait({10.0, 20.0, 30.0, 40.0}, identity, runId));
    const QJsonObject typed = query(runId, QStringLiteral("line"));
    QCOMPARE(typed.value(QStringLiteral("type")).toString(), QStringLiteral("Line2D"));
    QCOMPARE(typed.value(QStringLiteral("value")).toObject().value(QStringLiteral("start")).toArray(),
             QJsonArray({10.0, 20.0}));
    const QJsonObject compatibility = query(runId, QStringLiteral("line_compat"));
    QCOMPARE(compatibility.value(QStringLiteral("type")).toString(), QStringLiteral("QVariantList"));
    QCOMPARE(compatibility.value(QStringLiteral("value")).toArray(), QJsonArray({10.0, 20.0, 30.0, 40.0}));

    for (int index = 0; index < 4; ++index) {
        for (const double nonFinite :
             {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            QVariantList invalid{10.0, 20.0, 30.0, 40.0};
            invalid[index] = nonFinite;
            QVERIFY(runAndWait(invalid, identity, runId));
            const QJsonObject rejected = query(runId, QStringLiteral("line"));
            QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
            QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            QVERIFY(!rejected.contains(QStringLiteral("value")));
        }
        for (const QVariant& nonNumeric : {QVariant(QStringLiteral("12.5")), QVariant(true)}) {
            QVariantList invalid{10.0, 20.0, 30.0, 40.0};
            invalid[index] = nonNumeric;
            QVERIFY(runAndWait(invalid, identity, runId));
            const QJsonObject rejected = query(runId, QStringLiteral("line"));
            QCOMPARE(rejected.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
            QCOMPARE(rejected.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            QVERIFY(!rejected.contains(QStringLiteral("value")));
        }
    }
    QVERIFY(runAndWait({10.0, 20.0, 30.0}, identity, runId));
    const QJsonObject wrongSize = query(runId, QStringLiteral("line"));
    QCOMPARE(wrongSize.value(QStringLiteral("errorCode")).toString(), QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
    QVERIFY(!wrongSize.contains(QStringLiteral("value")));

    const auto crossingHorizon = provenanceFor({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.02, 0.0, -1.0});
    QVERIFY(runAndWait({10.0, 20.0, 100.0, 20.0}, crossingHorizon, runId));
    const QJsonObject crossingLine = query(runId, QStringLiteral("line"));
    QCOMPARE(crossingLine.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject crossingValue = crossingLine.value(QStringLiteral("value")).toObject();
    QCOMPARE(crossingValue.value(QStringLiteral("start")).toArray(), QJsonArray({10.0, 20.0}));
    QCOMPARE(crossingValue.value(QStringLiteral("end")).toArray(), QJsonArray({100.0, 20.0}));
    QVERIFY(!crossingValue.contains(QStringLiteral("sourceImageCoordinates")));

    const auto nearHorizon = provenanceFor({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, -10.00000000000001});
    QVERIFY(runAndWait({10.0, 20.0, 30.0, 20.0}, nearHorizon, runId));
    const QJsonObject nearLine = query(runId, QStringLiteral("line"));
    QCOMPARE(nearLine.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    const QJsonObject nearValue = nearLine.value(QStringLiteral("value")).toObject();
    QCOMPARE(nearValue.value(QStringLiteral("start")).toArray(), QJsonArray({10.0, 20.0}));
    QCOMPARE(nearValue.value(QStringLiteral("end")).toArray(), QJsonArray({30.0, 20.0}));
    QVERIFY(!nearValue.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testAgentMeasureRectMapsRotatedVerticesAndDoesNotLeakFailedRunOutput() {
    ToolSchema::instance().registerDefaultTools();
    QTemporaryDir dataDir;
    QVERIFY(dataDir.isValid());
    const QString imagePath = dataDir.filePath(QStringLiteral("measurerect.png"));
    constexpr double centerX = 140.0;
    constexpr double centerY = 100.0;
    constexpr double width = 110.0;
    constexpr double height = 60.0;
    constexpr double angleDegrees = 27.0;
    constexpr double cropX = 20.0;
    constexpr double cropY = 20.0;
    const double angle = qDegreesToRadians(angleDegrees);
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    const QVector<QPointF> parentExpected{QPointF(centerX - width / 2.0 * cosine + height / 2.0 * sine,
                                                  centerY - width / 2.0 * sine - height / 2.0 * cosine),
                                          QPointF(centerX + width / 2.0 * cosine + height / 2.0 * sine,
                                                  centerY + width / 2.0 * sine - height / 2.0 * cosine),
                                          QPointF(centerX + width / 2.0 * cosine - height / 2.0 * sine,
                                                  centerY + width / 2.0 * sine + height / 2.0 * cosine),
                                          QPointF(centerX - width / 2.0 * cosine - height / 2.0 * sine,
                                                  centerY - width / 2.0 * sine + height / 2.0 * cosine)};
    QImage image(280, 200, QImage::Format_RGB32);
    image.fill(Qt::black);
    {
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        QPolygonF polygon;
        for (const QPointF& point : parentExpected)
            polygon.append(point);
        painter.drawPolygon(polygon);
    }
    QVERIFY(image.save(imagePath));

    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_measurerect.json", dataDir.path(), *project).isEmpty(),
             "load MeasureRect project with synthetic rotated rectangle");

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    const QJsonObject firstStarted = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString firstRunId = firstStarted.value(QStringLiteral("runId")).toString();
    QVERIFY2(!firstRunId.isEmpty(), qPrintable(firstStarted.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(firstRunId).finished, 10000);
    QVERIFY2(engine.runObservation(firstRunId).result.success,
             qPrintable(engine.runObservation(firstRunId).result.errorMessage));

    const ImageData grabOutput = engine.moduleOutput(QStringLiteral("grab"));
    const ImageData cropOutput = engine.moduleOutput(QStringLiteral("crop"));
    const ImageData rectangleOutput = engine.moduleOutput(QStringLiteral("measure"));
    QVERIFY(grabOutput.hasCoordinateProvenance());
    QVERIFY(cropOutput.hasCoordinateProvenance());
    const Region2D rectangle = rectangleOutput.data(QStringLiteral("rectangle")).value<Region2D>();
    QCOMPARE(rectangle.polygon.size(), 4);
    const QVector<QPointF> localExpected{QPointF(parentExpected.at(0).x() - cropX, parentExpected.at(0).y() - cropY),
                                         QPointF(parentExpected.at(1).x() - cropX, parentExpected.at(1).y() - cropY),
                                         QPointF(parentExpected.at(2).x() - cropX, parentExpected.at(2).y() - cropY),
                                         QPointF(parentExpected.at(3).x() - cropX, parentExpected.at(3).y() - cropY)};

    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{"runId", firstRunId},
                                                                           {"instanceId", "measure"},
                                                                           {"key", "rectangle"},
                                                                           {"polygonOffset", 0},
                                                                           {"polygonLimit", 4}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QVERIFY(!response.contains(QStringLiteral("unit")));
    const QJsonObject value = response.value(QStringLiteral("value")).toObject();
    QCOMPARE(value.value(QStringLiteral("label")).toString(), QStringLiteral("rectangle"));
    QCOMPARE(value.value(QStringLiteral("score")).toDouble(), 0.0);
    QCOMPARE(value.value(QStringLiteral("maskRleChars")).toInt(), 0);
    const QJsonArray bbox = value.value(QStringLiteral("bbox")).toArray();
    QCOMPARE(bbox.size(), 4);
    const QJsonObject polygonPage = response.value(QStringLiteral("polygonPage")).toObject();
    QCOMPARE(polygonPage.value(QStringLiteral("totalPoints")).toInt(), 4);
    QCOMPARE(polygonPage.value(QStringLiteral("points")).toArray().size(), 4);
    const QJsonArray localPoints = polygonPage.value(QStringLiteral("points")).toArray();

    QVector<QPointF> actualPoints;
    double minX = std::numeric_limits<double>::infinity();
    double minY = std::numeric_limits<double>::infinity();
    double maxX = -std::numeric_limits<double>::infinity();
    double maxY = -std::numeric_limits<double>::infinity();
    for (const QJsonValue& pointValue : localPoints) {
        const QJsonArray point = pointValue.toArray();
        QCOMPARE(point.size(), 2);
        const QPointF actual(point.at(0).toDouble(), point.at(1).toDouble());
        QVERIFY(std::isfinite(actual.x()));
        QVERIFY(std::isfinite(actual.y()));
        for (const QPointF& previous : actualPoints)
            QVERIFY(QLineF(actual, previous).length() > 1.0);
        actualPoints.append(actual);
        minX = qMin(minX, actual.x());
        minY = qMin(minY, actual.y());
        maxX = qMax(maxX, actual.x());
        maxY = qMax(maxY, actual.y());
    }
    QCOMPARE(bbox.at(0).toDouble(), minX);
    QCOMPARE(bbox.at(1).toDouble(), minY);
    QCOMPARE(bbox.at(2).toDouble(), maxX - minX);
    QCOMPARE(bbox.at(3).toDouble(), maxY - minY);

    bool matchesLocalTruth = false;
    int expectedForActual[4] = {-1, -1, -1, -1};
    for (int start = 0; start < 4 && !matchesLocalTruth; ++start) {
        for (const int direction : {-1, 1}) {
            bool allVerticesMatch = true;
            int candidateExpected[4] = {-1, -1, -1, -1};
            for (int i = 0; i < 4; ++i) {
                const int expectedIndex = (start + direction * i + 8) % 4;
                if (QLineF(actualPoints.at(i), localExpected.at(expectedIndex)).length() > 6.0) {
                    allVerticesMatch = false;
                    break;
                }
                candidateExpected[i] = expectedIndex;
            }
            matchesLocalTruth = allVerticesMatch;
            if (matchesLocalTruth) {
                for (int i = 0; i < 4; ++i)
                    expectedForActual[i] = candidateExpected[i];
                break;
            }
        }
    }
    QVERIFY(matchesLocalTruth);
    bool hasNonAxisAlignedEdge = false;
    for (int i = 0; i < actualPoints.size(); ++i) {
        const QPointF edge = actualPoints.at((i + 1) % 4) - actualPoints.at(i);
        if (std::abs(edge.x()) > 1.0 && std::abs(edge.y()) > 1.0)
            hasNonAxisAlignedEdge = true;
    }
    QVERIFY(hasNonAxisAlignedEdge);

    const QJsonObject sourcePage = polygonPage.value(QStringLiteral("sourceImageCoordinates")).toObject();
    QCOMPARE(sourcePage.value(QStringLiteral("sourceImageId")).toString(),
             grabOutput.coordinateProvenance().sourceImageId);
    QCOMPARE(sourcePage.value(QStringLiteral("coordinateSystem")).toString(), QStringLiteral("source_image_pixels"));
    const QJsonArray sourcePoints = sourcePage.value(QStringLiteral("points")).toArray();
    QCOMPARE(sourcePoints.size(), 4);
    const ImageCoordinateProvenance provenance = cropOutput.coordinateProvenance();
    for (int i = 0; i < 4; ++i) {
        const QJsonArray actualLocal = localPoints.at(i).toArray();
        const auto mapped =
            provenance.mapLocalToSource(QPointF(actualLocal.at(0).toDouble(), actualLocal.at(1).toDouble()));
        QVERIFY(mapped.has_value());
        const QJsonArray actualSource = sourcePoints.at(i).toArray();
        QVERIFY(QLineF(QPointF(actualSource.at(0).toDouble(), actualSource.at(1).toDouble()), *mapped).length() < 1e-6);
        const QPointF expectedSource = parentExpected.at(expectedForActual[i]);
        QVERIFY(
            QLineF(QPointF(actualSource.at(0).toDouble(), actualSource.at(1).toDouble()), expectedSource).length() <=
            6.0);
    }

    QMap<QString, double> scalarValues;
    for (const QString& key : {QStringLiteral("rect_width"), QStringLiteral("rect_height"),
                               QStringLiteral("rect_angle"), QStringLiteral("rect_area")}) {
        const QString unit = key == QLatin1String("rect_width") || key == QLatin1String("rect_height")
                                 ? QStringLiteral("px")
                                 : (key == QLatin1String("rect_area") ? QStringLiteral("px^2") : QStringLiteral("deg"));
        const QList<bool> queryPorts{true, false};
        for (bool byPort : queryPorts) {
            QJsonObject args{{"runId", firstRunId}, {"instanceId", "measure"}};
            args.insert(byPort ? QStringLiteral("port") : QStringLiteral("key"), key);
            const QJsonObject scalar = actor.executeTool(QStringLiteral("get_module_output"), args);
            QCOMPARE(scalar.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
            QCOMPARE(scalar.value(QStringLiteral("unit")).toString(), unit);
            QVERIFY(scalar.value(QStringLiteral("value")).isDouble());
            QVERIFY(std::abs(scalar.value(QStringLiteral("value")).toDouble() - rectangleOutput.data(key).toDouble()) <=
                    1e-9);
            QVERIFY(!scalar.contains(QStringLiteral("coordinateSystem")));
            QVERIFY(!scalar.contains(QStringLiteral("sourceImageCoordinates")));
            if (!byPort)
                scalarValues.insert(key, scalar.value(QStringLiteral("value")).toDouble());
        }
    }
    const double measuredWidth = scalarValues.value(QStringLiteral("rect_width"));
    const double measuredHeight = scalarValues.value(QStringLiteral("rect_height"));
    const double firstSide = QLineF(localExpected.at(0), localExpected.at(1)).length();
    const double secondSide = QLineF(localExpected.at(1), localExpected.at(2)).length();
    const double expectedWidth = qMax(firstSide, secondSide);
    const double expectedHeight = qMin(firstSide, secondSide);
    QVERIFY(std::abs(measuredWidth - expectedWidth) <= 6.0);
    QVERIFY(std::abs(measuredHeight - expectedHeight) <= 6.0);
    QVERIFY(std::abs(scalarValues.value(QStringLiteral("rect_area")) - 6599.0) <= 100.0);
    QVERIFY(std::abs(scalarValues.value(QStringLiteral("rect_angle")) - 26.9958) <= 2.0);

    QFile metadata(QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                   "/../../src/plugins/detection/MeasureRect/metadata.json"));
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    const QJsonArray outputs = QJsonDocument::fromJson(metadata.readAll())
                                   .object()
                                   .value(QStringLiteral("ports"))
                                   .toObject()
                                   .value(QStringLiteral("outputs"))
                                   .toArray();
    bool rectanglePortDeclared = false;
    bool rectangleAnglePortDeclared = false;
    for (const QJsonValue& output : outputs) {
        const QJsonObject port = output.toObject();
        if (port.value(QStringLiteral("id")).toString() == QLatin1String("rectangle"))
            rectanglePortDeclared = port.value(QStringLiteral("type")).toString() == QLatin1String("Region2D");
        if (port.value(QStringLiteral("id")).toString() == QLatin1String("rect_angle"))
            rectangleAnglePortDeclared = port.value(QStringLiteral("type")).toString() == QLatin1String("Number") &&
                                         !port.value(QStringLiteral("required")).toBool(true);
    }
    QVERIFY(rectanglePortDeclared);
    QVERIFY(rectangleAnglePortDeclared);

    QImage blank(280, 200, QImage::Format_RGB32);
    blank.fill(Qt::black);
    QVERIFY(blank.save(imagePath));
    const QJsonObject secondStarted = actor.executeTool(QStringLiteral("run_flow"), {});
    const QString secondRunId = secondStarted.value(QStringLiteral("runId")).toString();
    QVERIFY2(!secondRunId.isEmpty(), qPrintable(secondStarted.value(QStringLiteral("error")).toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(secondRunId).finished, 10000);
    QVERIFY(!engine.runObservation(secondRunId).result.success);
    const QJsonObject unavailable =
        actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{"runId", secondRunId},
                                                                           {"instanceId", "measure"},
                                                                           {"key", "rectangle"},
                                                                           {"polygonOffset", 0},
                                                                           {"polygonLimit", 4}});
    QCOMPARE(unavailable.value(QStringLiteral("status")).toString(), QStringLiteral("unavailable"));
    QVERIFY(!unavailable.contains(QStringLiteral("value")));
    QVERIFY(!unavailable.contains(QStringLiteral("polygonPage")));
}

void TestAcceptanceFlows::testAgentMeasureRectWithoutProvenanceRetainsLocalPolygon() {
    ToolSchema::instance().registerDefaultTools();
    QImage image(180, 140, QImage::Format_RGB32);
    image.fill(Qt::black);
    {
        QPainter painter(&image);
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawRect(QRect(35, 25, 90, 65));
    }
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    ModuleInstance grab;
    grab.id = QStringLiteral("grab");
    grab.moduleId = QStringLiteral("GrabImage");
    grab.name = QStringLiteral("unprovenanced fixture image");
    project->addModule(grab);
    ModuleInstance measure;
    measure.id = QStringLiteral("measure");
    measure.moduleId = QStringLiteral("MeasureRect");
    measure.name = QStringLiteral("MeasureRect");
    project->addModule(measure);
    ModuleConnection connection;
    connection.fromModuleId = grab.id;
    connection.toModuleId = measure.id;
    connection.fromPort = QStringLiteral("image");
    connection.toPort = QStringLiteral("image");
    connection.edgeType = QStringLiteral("data");
    project->addConnection(connection);
    ProjectFlow flow;
    flow.id = QStringLiteral("main");
    flow.name = QStringLiteral("main");
    flow.nodeIds = QStringList{grab.id, measure.id};
    project->setFlows({flow});

    RunEngine& engine = RunEngine::instance();
    QVERIFY(engine.loadProject(project, [&image](const ModuleInstance& instance) -> ModuleBase* {
        if (instance.id == QLatin1String("grab"))
            return new UnprovenancedImageFixtureModule(image);
        IModule* module = PluginManager::instance().createModule(instance.moduleId);
        return qobject_cast<ModuleBase*>(module);
    }));

    AgentActor actor;
    QString runId;
    QVERIFY(engine.runOnceAsync(&runId));
    QVERIFY(!runId.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    QVERIFY2(engine.runObservation(runId).result.success, qPrintable(engine.runObservation(runId).result.errorMessage));
    QVERIFY(!engine.moduleOutput(QStringLiteral("grab")).hasCoordinateProvenance());

    const QJsonObject response =
        actor.executeTool(QStringLiteral("get_module_output"), QJsonObject{{"runId", runId},
                                                                           {"instanceId", "measure"},
                                                                           {"key", "rectangle"},
                                                                           {"polygonOffset", 0},
                                                                           {"polygonLimit", 4}});
    QCOMPARE(response.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
    QCOMPARE(response.value(QStringLiteral("value")).toObject().value(QStringLiteral("coordinateSystem")).toString(),
             QStringLiteral("image_pixels"));
    QVERIFY(!response.value(QStringLiteral("value")).toObject().contains(QStringLiteral("sourceImageCoordinates")));
    const QJsonObject polygonPage = response.value(QStringLiteral("polygonPage")).toObject();
    QCOMPARE(polygonPage.value(QStringLiteral("points")).toArray().size(), 4);
    QVERIFY(!polygonPage.contains(QStringLiteral("sourceImageCoordinates")));
}

void TestAcceptanceFlows::testAgentRunFlowPreservesLoopCount() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_loop.json", QString(), *project).isEmpty(),
             "load Loop project into current ProjectManager project");

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    FlowTrace trace;
    std::vector<QMetaObject::Connection> connections;
    trace.connectTo(engine, connections);
    const QJsonObject started = actor.executeTool("run_flow", {});
    const QString runId = started.value("runId").toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    trace.disconnectAll(connections);

    QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
             QStringLiteral("completed"));
    QCOMPARE(trace.succeeded(QStringLiteral("body")), 3);
    QCOMPARE(trace.succeeded(QStringLiteral("after")), 1);
    QCOMPARE(
        trace.finishSequence(),
        (QStringList{QStringLiteral("loop"), QStringLiteral("body"), QStringLiteral("loop"), QStringLiteral("body"),
                     QStringLiteral("loop"), QStringLiteral("body"), QStringLiteral("loop"), QStringLiteral("after")}));

    const QJsonObject rejectedParams = actor.executeTool(
        "set_module_params", QJsonObject{{"instanceId", "loop"}, {"values", QJsonObject{{"loopCount", -1}}}});
    QCOMPARE(rejectedParams.value("errorCode").toString(), QStringLiteral("VALIDATION_FAILED"));
    QCOMPARE(rejectedParams.value("error").toString(), QStringLiteral("Loop count must not be negative"));
    const auto loopAfterRejection = project->moduleById(QStringLiteral("loop"));
    QVERIFY(loopAfterRejection.has_value());
    QCOMPARE(loopAfterRejection->params.value(QStringLiteral("loopCount")).toInt(), 3);
    QVERIFY(!engine.isBusy());
    QVERIFY(engine.runObservation(runId).result.success);
    QCOMPARE(trace.succeeded(QStringLiteral("body")), 3);
    QCOMPARE(trace.succeeded(QStringLiteral("after")), 1);
    QCOMPARE(
        trace.finishSequence(),
        (QStringList{QStringLiteral("loop"), QStringLiteral("body"), QStringLiteral("loop"), QStringLiteral("body"),
                     QStringLiteral("loop"), QStringLiteral("body"), QStringLiteral("loop"), QStringLiteral("after")}));
}

void TestAcceptanceFlows::testAgentRunFlowPreservesParallelAllJoin() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_all.json", QString(), *project).isEmpty(),
             "load Parallel project into current ProjectManager project");

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    for (const QString& instanceId : {QStringLiteral("par"), QStringLiteral("merge")}) {
        const QJsonObject configured =
            actor.executeTool("set_module_params",
                              QJsonObject{{"instanceId", instanceId}, {"values", QJsonObject{{"parallelCount", 3}}}});
        QCOMPARE(configured.value("status").toString(), QStringLiteral("params_set"));
        const auto module = project->moduleById(instanceId);
        QVERIFY(module.has_value());
        QCOMPARE(module->params.value("parallelCount").toInt(), 3);
    }

    FlowTrace trace;
    std::vector<QMetaObject::Connection> connections;
    trace.connectTo(engine, connections);
    const QJsonObject started = actor.executeTool("run_flow", {});
    const QString runId = started.value("runId").toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    trace.disconnectAll(connections);

    QCOMPARE(actor.executeTool("get_run_results", QJsonObject{{"runId", runId}}).value("status").toString(),
             QStringLiteral("completed"));
    QCOMPARE(trace.succeeded(QStringLiteral("b1")), 1);
    QCOMPARE(trace.succeeded(QStringLiteral("b2")), 1);
    QCOMPARE(trace.succeeded(QStringLiteral("merge")), 1);
    QCOMPARE(trace.succeeded(QStringLiteral("after")), 1);
    QVERIFY(engine.lastParallelMaxConcurrency() >= 2);
    QVERIFY(trace.finishedAfter(QStringLiteral("merge"), QStringLiteral("b1")));
    QVERIFY(trace.finishedAfter(QStringLiteral("merge"), QStringLiteral("b2")));
    for (const QString& instanceId : {QStringLiteral("par"), QStringLiteral("merge")}) {
        const QJsonObject output = actor.executeTool(
            "get_module_output", QJsonObject{{"runId", runId}, {"instanceId", instanceId}, {"key", "parallel_count"}});
        QCOMPARE(output.value("status").toString(), QStringLiteral("available"));
        QCOMPARE(output.value("value").toInt(), 3);
    }
}

void TestAcceptanceFlows::testAgentRunFlowPreservesParallelFailurePropagation() {
    ToolSchema::instance().registerDefaultTools();
    QVERIFY(ProjectManager::instance().newProject());
    Project* project = ProjectManager::instance().currentProject();
    QVERIFY(project);
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_failure.json", QString(), *project).isEmpty(),
             "load Parallel failure project into current ProjectManager project");

    AgentActor actor;
    RunEngine& engine = RunEngine::instance();
    FlowTrace trace;
    std::vector<QMetaObject::Connection> connections;
    trace.connectTo(engine, connections);
    const QJsonObject started = actor.executeTool("run_flow", {});
    const QString runId = started.value("runId").toString();
    QVERIFY2(!runId.isEmpty(), qPrintable(started.value("error").toString()));
    QTRY_VERIFY_WITH_TIMEOUT(engine.runObservation(runId).finished, 10000);
    trace.disconnectAll(connections);

    const QJsonObject results = actor.executeTool("get_run_results", QJsonObject{{"runId", runId}});
    QCOMPARE(results.value("status").toString(), QStringLiteral("failed"));
    QVERIFY(trace.started(QStringLiteral("bad")) > 0);
    QCOMPARE(trace.failed(QStringLiteral("bad")), 1);
    QVERIFY(trace.started(QStringLiteral("slow")) > 0); // 已派发到并行执行器
    QCOMPARE(trace.failed(QStringLiteral("slow")), 1);
    QCOMPARE(trace.succeeded(QStringLiteral("slow")), 0);
    QCOMPARE(trace.started(QStringLiteral("merge")), 0);
    QCOMPARE(trace.finished(QStringLiteral("merge")), 0);
    QCOMPARE(trace.started(QStringLiteral("after")), 0);
    QCOMPARE(trace.finished(QStringLiteral("after")), 0);
    QCOMPARE(trace.succeeded(QStringLiteral("merge")), 0);
    QCOMPARE(trace.succeeded(QStringLiteral("after")), 0);
    QVERIFY(trace.elapsedMs() < 500); // 500ms Delay 被失败分支协作取消，而非正常跑满
    QCOMPARE(
        actor.executeTool("get_module_output", QJsonObject{{"runId", runId}, {"instanceId", "slow"}, {"key", "image"}})
            .value("status")
            .toString(),
        QStringLiteral("unavailable"));
}

void TestAcceptanceFlows::testLoopFixedCountFlow() {
    // Loop(3) → body 恰执行 3 次 → done → after；执行顺序完全确定
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_loop.json", QString(), project).isEmpty(),
             "load accept_loop.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject loop flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "loop flow must succeed");
    QCOMPARE(trace.finishedCount.value("body"), 3);
    QCOMPARE(trace.finishedCount.value("after"), 1);
    QCOMPARE(trace.finishedOrder, QStringList({"loop", "body", "loop", "body", "loop", "body", "loop", "after"}));
}

void TestAcceptanceFlows::testLoopFiftyRunsNoCrossFramePollution() {
    // 同一工程重复运行 50 次：每次循环体都恰执行 3 次，无跨帧污染
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_loop.json", QString(), project).isEmpty(),
             "load accept_loop.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject loop flow");

    for (int run = 0; run < 50; ++run) {
        FlowTrace trace;
        std::vector<QMetaObject::Connection> conns;
        trace.connectTo(engine, conns);
        engine.runOnce();
        trace.disconnectAll(conns);
        QVERIFY2(trace.runSuccess, qPrintable(QString("run %1 must succeed").arg(run)));
        QCOMPARE(trace.finishedCount.value("body"), 3);
        QCOMPARE(trace.finishedCount.value("after"), 1);
    }
}

void TestAcceptanceFlows::testWhileConditionExitFlow() {
    // While(counter < 3)：种子写 counter=0，循环体每次 +1，
    // 条件在第 4 次求值时为假退出 —— 由数据驱动而非固定次数
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_while.json", QString(), project).isEmpty(),
             "load accept_while.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject while flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "while flow must succeed");
    QCOMPARE(trace.finishedCount.value("seed"), 1);
    QCOMPARE(trace.finishedCount.value("body"), 3); // 条件退出，而非 maxIterations
    QCOMPARE(trace.finishedCount.value("after"), 1);

    // 循环体最后一次输出 counter=3（数据载体无图像，按键存在性断言）
    const ImageData bodyOut = engine.moduleOutput(QStringLiteral("body"));
    QVERIFY2(bodyOut.hasData("counter"), "while body must produce counter output");
    QCOMPARE(bodyOut.data("counter").toDouble(), 3.0);
}

void TestAcceptanceFlows::testStopWhileEarlyExitFlow() {
    // While(恒真, maxIterations=1000) 内 StopWhile 首次迭代即提前退出
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_stopwhile.json", QString(), project).isEmpty(),
             "load accept_stopwhile.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject stopwhile flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "stopwhile flow must succeed");
    QCOMPARE(trace.finishedCount.value("body"), 1); // 提前退出：仅 1 次而非 1000 次
    QCOMPARE(trace.finishedCount.value("stopMod"), 1);
    QCOMPARE(trace.finishedCount.value("after"), 1); // stop 边触发一次，不重复
}

void TestAcceptanceFlows::testStopCancelsLongLoopWithinDeadline() {
    // 停止/取消时限可断言：恒真长循环（100 次 × 30ms）运行中请求取消，
    // 必须在 500ms 内停下，且远未跑满全部迭代
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_stop_cancel.json", QString(), project).isEmpty(),
             "load accept_stop_cancel.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject stop-cancel flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);

    std::thread runner([&engine]() { engine.runOnce(); });

    QTest::qWait(250); // 让循环跑几轮
    QElapsedTimer stopWatch;
    stopWatch.start();
    engine.requestCancellation();

    // 工程自身最多运行 3s，即使取消失效，join 也有确定上限。
    while (engine.isBusy() && stopWatch.elapsed() < 3000)
        QTest::qWait(10);
    const qint64 stopLatencyMs = stopWatch.elapsed();

    runner.join();
    trace.disconnectAll(conns);

    QVERIFY2(!engine.isBusy(), "engine must stop after cancellation");
    QVERIFY2(!trace.runSuccess, "cancelled run must not report success");
    QVERIFY2(stopLatencyMs < 500, qPrintable(QString("stop latency %1ms exceeds 500ms deadline").arg(stopLatencyMs)));
    QVERIFY2(trace.finishedCount.value("body") < 100,
             qPrintable(QString("body ran %1 times; cancellation ineffective").arg(trace.finishedCount.value("body"))));
    QCOMPARE(trace.finishedCount.value("after"), 0); // 取消后不应走到 done 分支
}

// ---------------------------------------------------------------------------
// 阶段 4: Parallel all / any / 失败分支 / blocking 不并行 流程验收
// ---------------------------------------------------------------------------

void TestAcceptanceFlows::testParallelAllJoinFlow() {
    // Parallel 分叉两个 60ms 线程安全分支 → all 汇合 → after。
    // 断言：真实并发（最大并发度≥2）、汇合在两个分支都完成后、各节点恰一次。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_all.json", QString(), project).isEmpty(),
             "load accept_parallel_all.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-all flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);

    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "parallel-all flow must succeed");
    QCOMPARE(trace.finishedCount.value("b1"), 1);
    QCOMPARE(trace.finishedCount.value("b2"), 1);
    QCOMPARE(trace.finishedCount.value("merge"), 1);
    QCOMPARE(trace.finishedCount.value("after"), 1);
    QVERIFY2(engine.lastParallelMaxConcurrency() >= 2,
             qPrintable(QString("expected real concurrency >=2, got %1").arg(engine.lastParallelMaxConcurrency())));
    // all 汇合：merge 必须在两个分支完成信号之后。
    QVERIFY2(trace.finishedOrder.indexOf("merge") > trace.finishedOrder.indexOf("b1"), "merge finished before b1");
    QVERIFY2(trace.finishedOrder.indexOf("merge") > trace.finishedOrder.indexOf("b2"), "merge finished before b2");
}

void TestAcceptanceFlows::testParallelAnyJoinFlow() {
    // any 汇合：真分支触发一条控制边即可激活汇合点；假分支被跳过、
    // 其控制边永不触发，但 any 策略下汇合点仍执行且仅执行一次。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_any.json", QString(), project).isEmpty(),
             "load accept_parallel_any.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-any flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "any-join flow must succeed");
    QCOMPARE(trace.finishedCount.value("branchA"), 1);
    QCOMPARE(trace.finishedCount.value("branchB"), 0);
    QVERIFY2(trace.skipped.contains("branchB"), "false branch must be skipped");
    // any 策略：只有一条边触发也应激活汇合点，且仅一次
    QCOMPARE(trace.finishedCount.value("merge"), 1);
}

void TestAcceptanceFlows::testParallelFailureCancelsSiblingFlow() {
    // 失败分支：快速失败分支取消同组 500ms 慢分支，
    // 总耗时必须远小于慢分支时长（取消时限可断言），汇合点不执行。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_failure.json", QString(), project).isEmpty(),
             "load accept_parallel_failure.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-failure flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(!trace.runSuccess, "run with failed branch must fail");
    QVERIFY2(trace.finishedCount.value("bad") == 1, "failed branch must have executed");
    QVERIFY2(trace.runElapsedMs < 400,
             qPrintable(QString("sibling cancellation too slow: %1ms").arg(trace.runElapsedMs)));
    QCOMPARE(trace.finishedCount.value("merge"), 0); // 失败分支不触发后继
    QCOMPARE(trace.finishedCount.value("after"), 0);
}

void TestAcceptanceFlows::testParallelBlockingBranchesNotParallelizedFlow() {
    // blocking 模块（SaveData，文件 I/O）即使声明并发数 2 也不得并行：
    // 两个分支都必须输出文件，且最大并发度 ≤1。
    const QString file1 = m_tempDir.filePath(QStringLiteral("accept_parallel_blocking_1.json"));
    const QString file2 = m_tempDir.filePath(QStringLiteral("accept_parallel_blocking_2.json"));

    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_parallel_blocking.json", QString(), project).isEmpty(),
             "load accept_parallel_blocking.json");
    QVERIFY(project.setModuleParam("s1", "filePath", file1));
    QVERIFY(project.setModuleParam("s2", "filePath", file2));

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject parallel-blocking flow");

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);

    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "blocking-branch flow must succeed");
    QCOMPARE(trace.finishedCount.value("s1"), 1);
    QCOMPARE(trace.finishedCount.value("s2"), 1);
    QCOMPARE(trace.finishedCount.value("merge"), 1);
    QVERIFY2(QFile::exists(file1), "blocking branch s1 must have written its file");
    QVERIFY2(QFile::exists(file2), "blocking branch s2 must have written its file");

    QVERIFY2(engine.lastParallelMaxConcurrency() <= 1,
             qPrintable(QString("blocking modules must not be parallelized, concurrency=%1")
                            .arg(engine.lastParallelMaxConcurrency())));
}

// ---------------------------------------------------------------------------
// 阶段 4: 交互式拾取点集 → 圆拟合 真实工作流
// ---------------------------------------------------------------------------

void TestAcceptanceFlows::testFitCircleFromPickSessionFlow() {
    // 工程内 MeasurementInput 点集为空（拾取前状态）：
    // 1) 未完成拾取时运行必须失败（拾取门控）；
    // 2) 按 MeasurementInput 的 points 持久化格式提交圆周点；
    //    之后流程成功，拟合结果与已知圆一致。
    Project project;
    QVERIFY2(loadProjectWithPlaceholder("projects/accept_fitcircle_pick.json", QString(), project).isEmpty(),
             "load accept_fitcircle_pick.json");

    RunEngine& engine = RunEngine::instance();
    QVERIFY2(engine.loadProject(&project), "loadProject fitcircle-pick flow");

    // 1) 拾取门控：空点集运行必须失败
    {
        FlowTrace trace;
        std::vector<QMetaObject::Connection> conns;
        trace.connectTo(engine, conns);
        engine.runOnce();
        trace.disconnectAll(conns);
        QVERIFY2(!trace.runSuccess, "run before picking must fail (pick gate)");
    }

    // 2) 模拟一次拾取会话：逐点累积写入 points（与 MainWindow 拾取写参同路径）
    ModuleBase* pick = engine.getModule(QStringLiteral("pickinput"));
    QVERIFY2(pick != nullptr, "pick input module must exist");

    QFile ef(QDir(m_acceptanceRoot).filePath("expected/fitcircle_pick.json"));
    QVERIFY(ef.open(QIODevice::ReadOnly));
    const QJsonObject expected = QJsonDocument::fromJson(ef.readAll()).object();
    ef.close();
    const double cx = expected["circle_center_x"].toDouble();
    const double cy = expected["circle_center_y"].toDouble();
    const double r = expected["circle_radius"].toDouble();
    const int pickCount = expected["pick_count"].toInt();

    QJsonArray picked;
    for (int k = 0; k < pickCount; ++k) {
        const double theta = 2.0 * M_PI * k / pickCount;
        picked.append(QJsonArray{cx + r * std::cos(theta), cy + r * std::sin(theta)});
        pick->setParam("points", picked);
    }
    QCOMPARE(picked.size(), pickCount);

    FlowTrace trace;
    std::vector<QMetaObject::Connection> conns;
    trace.connectTo(engine, conns);
    engine.runOnce();
    trace.disconnectAll(conns);

    QVERIFY2(trace.runSuccess, "fit flow must succeed after pick session");
    const ImageData out = engine.moduleOutput(QStringLiteral("fitcircle"));
    QVERIFY2(out.hasData("circle_center_x") && out.hasData("circle_radius"), "FitCircle produced no fit output");

    const double tolC = expected["tolerance_center_px"].toDouble();
    const double tolR = expected["tolerance_radius_px"].toDouble();
    const double tolE = expected["tolerance_fit_error"].toDouble();
    const double gotCx = out.data("circle_center_x").toDouble();
    const double gotCy = out.data("circle_center_y").toDouble();
    const double gotR = out.data("circle_radius").toDouble();
    const double gotE = out.data("circle_error").toDouble();
    QVERIFY2(std::abs(gotCx - cx) <= tolC, qPrintable(QString("center_x off: %1").arg(gotCx)));
    QVERIFY2(std::abs(gotCy - cy) <= tolC, qPrintable(QString("center_y off: %1").arg(gotCy)));
    QVERIFY2(std::abs(gotR - r) <= tolR, qPrintable(QString("radius off: %1").arg(gotR)));
    QVERIFY2(gotE <= tolE, qPrintable(QString("fit error too large: %1").arg(gotE)));
}

QTEST_MAIN(TestAcceptanceFlows)
#include "test_acceptance_flows.moc"
