#include "TestPluginRegistry.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QVariant>
#include <QVector>
#include <QtTest/QtTest>
#include <cmath>
#include <core/base/ModuleBase.h>
#include <core/deeplux/DataContract.h>
#include <core/deeplux/PayloadTypes.h>
#include <core/display/DisplayData.h>
#include <core/interface/IModule.h>
#include <core/manager/PluginManager.h>
#include <core/model/ImageData.h>
#include <memory>
#include <thread>

using namespace DeepLux;
using DeepLuxTest::PluginEntry;

// B9 审计规则（t21 F1 复盘，防止同类漏审）：
// 1) process()（及其直接调用链）不得创建 QObject（如 VarModel），也不得把 QObject 移交/删除
//    到 GUI 线程；跨线程 QObject 亲和性违反 Qt 规则，输出一致性冒烟不能替代该检查。
// 2) 审计必须扫描 process() 及直接调用链中的单例/全局状态（GlobalVarManager/VarModel/
//    PluginManager/RunEngine 等）与未加锁的可变全局/静态状态，不能只查文件级 static 变量。
namespace {

/// B9 审计候选：只登记 plan B9 明确列出的插件（外加 design §6.3（审计与标记，第二处 6.3 编号）点名的 Matching）。
struct Candidate {
    const char* dir;                 // 源码目录/CMake target 基名（TestPluginRegistry 查找键）
    const char* name;                // metadata.json 的 "name"
    const char* category;            // 所属批次（报告分组）
    bool auditedWorker = false;      // 审计通过并已标记 execution.affinity=worker
    bool deterministicOutput = true; // 输出是否必须逐值一致（时间类插件只比较端口/类型）
};

const QVector<Candidate>& candidates() {
    static const QVector<Candidate> list = {
        // B9A：图像处理（auditedWorker=true 表示静态审计 + 一致性冒烟均通过并已标记 worker）
        {"Blob", "Blob", "image", true},
        {"CropImage", "CropImage", "image", true},
        {"PerProcessing", "PerProcessing", "image", true},
        {"JigsawPuzzle", "JigsawPuzzle", "image", true},
        {"SaveImage", "SaveImage", "image", false},
        {"LoadPointCloud", "LoadPointCloud", "image", true},
        {"PreProcessing3D", "3DPreProcessing", "image", false},
        // B9B：检测/几何/标定（含 design §6.3 点名的 Matching）
        {"FindCircle", "FindCircle", "detection", true},
        {"Matching", "Matching", "detection", true},
        {"ColorRecognition", "ColorRecognition", "detection", false},
        {"QRCode", "QRCode", "detection", false},
        {"RegionStats", "RegionStats", "geometry", true},
        {"EdgeDefectDetection", "EdgeDefectDetection", "detection", true},
        {"JiErHanDefectsDet", "JiErHanDefectsDet", "detection", true},
        // hymson3d/DefectDetection 在本构建 ENABLE_HYMSON3D=OFF 下不产出插件 target，
        // 无法执行一致性冒烟，保持 gui（交付报告记录原因）。
        {"MeasureCircle", "MeasureCircle", "detection", false},
        {"MeasureRect", "MeasureRect", "detection", true},
        {"MeasureLine", "MeasureLine", "detection", true},
        {"MeasureGap", "MeasureGap", "geometry", false},
        {"FitCircle", "FitCircle", "geometry", true},
        {"FitLine", "FitLine", "geometry", true},
        {"FitEllipse", "FitEllipse", "geometry", true},
        {"FitPlane", "FitPlane", "geometry", false},
        {"DistancePL", "DistancePL", "geometry", true},
        {"DistancePP", "DistancePP", "geometry", true},
        {"LinesDistance", "LinesDistance", "geometry", true},
        {"PointSurfaceDistance", "PointSurfaceDistance", "geometry", false},
        {"NPointCalibration", "N点标定", "calibration", false},
        {"GapMeasure3D", "GapMeasure3D", "geometry", false},
        {"FreeformSurface", "FreeformSurface", "geometry", false},
        // B9C：变量/逻辑/系统（Delay/Parallel/QueueIn/QueueOut/TableOutPut 按 plan 排除，保持 gui）
        {"CreateString", "CreateString", "variable", true},
        {"SplitString", "SplitString", "variable", false},
        // t21 F1：process() 在 worker 线程写 GlobalVarManager 并 new VarModel(QObject)，
        // GUI 侧 GlobalVarView 读写/delete 这些 QObject 且 count/isEmpty 无锁读者；
        // UI 绑定未拆分 → 保持 gui（与 QueueIn/QueueOut 同类延后）。
        {"StrFormatPlugin", "StrFormatPlugin", "variable", false},
        {"VarDefinePlugin", "VarDefinePlugin", "variable", false},
        {"VarSetPlugin", "VarSetPlugin", "variable", false},
        {"MathPlugin", "MathPlugin", "variable", true},
        {"Condition", "Condition", "logic", false},
        {"If", "If", "logic", true},
        {"Loop", "Loop", "logic", true},
        {"While", "While", "logic", true},
        {"StopWhile", "StopWhile", "logic", true},
        {"TimeSlice", "TimeSlice", "system", true},
        {"DataCheck", "DataCheck", "system", true},
        {"SystemTime", "SystemTime", "system", true, false}, // 输出含时间戳：只比较端口/类型
        {"WriteText", "WriteText", "system", true},
        {"Folder", "Folder", "system", true},
        {"SaveData", "SaveData", "system", true},
    };
    return list;
}

struct Snapshot {
    bool success = false;
    int errorCode = 0;
    QString userMessage;
    PortValueMap outputs;
};

/// 一次一致性比较的结果：equal=false 时 reason 给出首个差异。
struct SnapshotComparisonResult {
    bool equal = false;
    QString label;
    QString reason;
};

ImageData syntheticImage() {
    QImage image(320, 240, QImage::Format_RGB32);
    image.fill(Qt::black);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(Qt::white, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QPointF(160, 120), 60, 60);
    painter.drawRect(QRectF(20, 20, 60, 40));
    painter.drawLine(QPointF(20, 200), QPointF(300, 210));
    painter.end();
    return ImageData(image);
}

ImageData syntheticPointCloud() {
    PointCloudData cloud;
    for (int y = 0; y < 20; ++y) {
        for (int x = 0; x < 20; ++x) {
            cloud.points.emplace_back(static_cast<double>(x), static_cast<double>(y), 0.0);
        }
    }
    cloud.normals.assign(cloud.points.size(), Eigen::Vector3d(0.0, 0.0, 1.0));
    ImageData image;
    image.setData(QStringLiteral("point_cloud"), QVariant::fromValue(cloud));
    return image;
}

QVector<QPointF> circlePoints() {
    constexpr double kPi = 3.14159265358979323846;
    QVector<QPointF> points;
    for (int i = 0; i < 72; ++i) {
        const double angle = 2.0 * kPi * i / 72.0;
        points.append(QPointF(160.0 + 60.0 * std::cos(angle), 120.0 + 60.0 * std::sin(angle)));
    }
    return points;
}

QVariant valueForType(DataType type) {
    switch (type) {
    case DataType::Image2D:
    case DataType::HeightMap2D:
    case DataType::Any:
        return QVariant::fromValue(syntheticImage());
    case DataType::PointCloud3D:
        return QVariant::fromValue(syntheticPointCloud());
    case DataType::Point2D:
        return QVariant::fromValue(QPointF(100.0, 100.0));
    case DataType::Point3D:
        return QVariantList{1.0, 2.0, 3.0};
    case DataType::PointSet2D:
        return QVariant::fromValue(circlePoints());
    case DataType::Line2D:
        return QVariantList{20.0, 20.0, 300.0, 210.0};
    case DataType::Circle2D:
        return QVariantList{160.0, 120.0, 60.0};
    case DataType::Plane3D:
        return QVariantList{0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0};
    case DataType::Number:
        return QVariant(1.0);
    case DataType::Integer:
        return QVariant(1);
    case DataType::Boolean:
        return QVariant(true);
    case DataType::String:
        return QVariant(QStringLiteral("test"));
    case DataType::Binary:
        return QVariant(QByteArray("test"));
    case DataType::Table:
        return QVariant(QVariantMap{{QStringLiteral("value"), 1}});
    case DataType::DetectionList:
        return QVariant(QVariantList{QVariantMap{{QStringLiteral("x"), 10.0},
                                                 {QStringLiteral("y"), 10.0},
                                                 {QStringLiteral("width"), 20.0},
                                                 {QStringLiteral("height"), 20.0},
                                                 {QStringLiteral("score"), 0.9},
                                                 {QStringLiteral("label"), QStringLiteral("defect")}}});
    case DataType::Region2D: {
        // 契约层接受 polygon+bbox 的 map 形态（Region2D 强类型亦可）。
        const QVariantList polygon{QPointF(100.0, 100.0), QPointF(180.0, 100.0), QPointF(180.0, 160.0),
                                   QPointF(100.0, 160.0)};
        return QVariant(QVariantMap{{QStringLiteral("polygon"), polygon},
                                    {QStringLiteral("bbox"), QVariantList{100.0, 100.0, 80.0, 60.0}},
                                    {QStringLiteral("score"), 0.9}});
    }
    default:
        return QVariant();
    }
}

PortValueMap buildInputs(IModule* module) {
    PortValueMap inputs;
    const QList<PortSpec> ports = module->inputPorts();
    for (const PortSpec& spec : ports) {
        if (spec.control) {
            continue;
        }
        // 必需输入必须提供；图像/点云/点集（以及 Any 载体端口）即使可选也提供，
        // 便于真正跑到计算路径（旧式插件从 ImageData 载体读隐藏数据键）。
        const bool alwaysProvide = spec.type == DataType::Image2D || spec.type == DataType::HeightMap2D ||
                                   spec.type == DataType::PointCloud3D || spec.type == DataType::PointSet2D ||
                                   spec.type == DataType::Any;
        if (!spec.required && !alwaysProvide) {
            continue;
        }
        const QVariant value = valueForType(spec.type);
        if (value.isValid()) {
            inputs.insert(spec.id, value);
        }
    }
    return inputs;
}

ExecutionResult executeOnce(IModule* module, const PortValueMap& inputs, PortValueMap& outputs, qint64 frameId) {
    ExecutionContext context;
    context.runId = QStringLiteral("worker-consistency");
    context.frameId = frameId;
    outputs.clear();
    return module->execute(inputs, outputs, context);
}

Snapshot snapshotOf(IModule* module, const PortValueMap& inputs, qint64 frameId) {
    Snapshot snapshot;
    PortValueMap isolatedInputs = inputs;
    for (auto it = isolatedInputs.begin(); it != isolatedInputs.end(); ++it) {
        if (!it.value().canConvert<ImageData>())
            continue;
        ImageData image = it.value().value<ImageData>();
        if (image.hasMat())
            image.setMat(image.toMat().clone());
        it.value() = QVariant::fromValue(std::move(image));
    }
    PortValueMap outputs;
    const ExecutionResult result = executeOnce(module, isolatedInputs, outputs, frameId);
    snapshot.success = result.success;
    snapshot.errorCode = result.errorCode;
    snapshot.userMessage = result.userMessage;
    for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        if (!it.value().canConvert<ImageData>())
            continue;
        ImageData image = it.value().value<ImageData>();
        image.setQImage(image.toQImage().copy());
        it.value() = QVariant::fromValue(std::move(image));
    }
    snapshot.outputs = outputs;
    return snapshot;
}

QByteArray imageSignature(const ImageData& image) {
    const QImage converted = image.toQImage().convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(converted.width()) + "x" + QByteArray::number(converted.height()));
    for (int y = 0; y < converted.height(); ++y) {
        hash.addData(reinterpret_cast<const char*>(converted.constScanLine(y)), converted.width() * 4);
    }
    return hash.result();
}

bool fuzzyEqual(double a, double b) {
    return qFuzzyCompare(a + 1.0, b + 1.0) || qAbs(a - b) <= 1e-9 * qMax(1.0, qMax(qAbs(a), qAbs(b)));
}

bool valuesEqual(const QVariant& a, const QVariant& b, QString& reason);

bool listsEqual(const QVariantList& a, const QVariantList& b, QString& reason) {
    if (a.size() != b.size()) {
        reason = QStringLiteral("list size %1 != %2").arg(a.size()).arg(b.size());
        return false;
    }
    for (int i = 0; i < a.size(); ++i) {
        if (!valuesEqual(a.at(i), b.at(i), reason)) {
            reason = QStringLiteral("list[%1]: %2").arg(i).arg(reason);
            return false;
        }
    }
    return true;
}

bool mapsEqual(const QVariantMap& a, const QVariantMap& b, QString& reason) {
    if (a.keys() != b.keys()) {
        reason = QStringLiteral("map keys differ");
        return false;
    }
    for (auto it = a.constBegin(); it != a.constEnd(); ++it) {
        if (!valuesEqual(it.value(), b.value(it.key()), reason)) {
            reason = QStringLiteral("map[%1]: %2").arg(it.key(), reason);
            return false;
        }
    }
    return true;
}

bool valuesEqual(const QVariant& a, const QVariant& b, QString& reason) {
    if (a.canConvert<ImageData>() && b.canConvert<ImageData>()) {
        if (imageSignature(a.value<ImageData>()) != imageSignature(b.value<ImageData>())) {
            reason = QStringLiteral("image payload differs");
            return false;
        }
        return true;
    }
    if (a.canConvert<PointCloudData>() && b.canConvert<PointCloudData>()) {
        const PointCloudData ca = a.value<PointCloudData>();
        const PointCloudData cb = b.value<PointCloudData>();
        if (ca.size() != cb.size()) {
            reason = QStringLiteral("point cloud size %1 != %2").arg(ca.size()).arg(cb.size());
            return false;
        }
        for (size_t i = 0; i < ca.points.size() && i < 32; ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                if (!fuzzyEqual(ca.points[i][axis], cb.points[i][axis])) {
                    reason = QStringLiteral("point cloud point[%1] differs").arg(i);
                    return false;
                }
            }
        }
        return true;
    }
    // 强类型结果载荷：按字段逐一比较（QVariant == 对自定义类型不可靠）。
    if (a.canConvert<Circle2D>() && b.canConvert<Circle2D>()) {
        const Circle2D ca = a.value<Circle2D>();
        const Circle2D cb = b.value<Circle2D>();
        if (!fuzzyEqual(ca.centerX, cb.centerX) || !fuzzyEqual(ca.centerY, cb.centerY) ||
            !fuzzyEqual(ca.radius, cb.radius) || !fuzzyEqual(ca.score, cb.score)) {
            reason = QStringLiteral("Circle2D payload differs");
            return false;
        }
        return true;
    }
    if (a.canConvert<DetectionList>() && b.canConvert<DetectionList>()) {
        const DetectionList da = a.value<DetectionList>();
        const DetectionList db = b.value<DetectionList>();
        if (da.size() != db.size()) {
            reason = QStringLiteral("detection count %1 != %2").arg(da.size()).arg(db.size());
            return false;
        }
        for (int i = 0; i < da.size(); ++i) {
            const Detection& x = da.items.at(i);
            const Detection& y = db.items.at(i);
            if (!fuzzyEqual(x.x, y.x) || !fuzzyEqual(x.y, y.y) || !fuzzyEqual(x.width, y.width) ||
                !fuzzyEqual(x.height, y.height) || !fuzzyEqual(x.score, y.score) || x.label != y.label) {
                reason = QStringLiteral("detection[%1] differs").arg(i);
                return false;
            }
        }
        return true;
    }
    if (a.canConvert<Region2D>() && b.canConvert<Region2D>()) {
        const Region2D ra = a.value<Region2D>();
        const Region2D rb = b.value<Region2D>();
        if (ra.label != rb.label || ra.polygon.size() != rb.polygon.size() || ra.maskRle != rb.maskRle ||
            !fuzzyEqual(ra.score, rb.score) || ra.bbox != rb.bbox) {
            reason = QStringLiteral("Region2D payload differs");
            return false;
        }
        for (int i = 0; i < ra.polygon.size(); ++i) {
            if (!fuzzyEqual(ra.polygon.at(i).x(), rb.polygon.at(i).x()) ||
                !fuzzyEqual(ra.polygon.at(i).y(), rb.polygon.at(i).y())) {
                reason = QStringLiteral("Region2D polygon[%1] differs").arg(i);
                return false;
            }
        }
        return true;
    }
    if (a.canConvert<QVector<QPointF>>() && b.canConvert<QVector<QPointF>>()) {
        const QVector<QPointF> pa = a.value<QVector<QPointF>>();
        const QVector<QPointF> pb = b.value<QVector<QPointF>>();
        if (pa.size() != pb.size()) {
            reason = QStringLiteral("point set size %1 != %2").arg(pa.size()).arg(pb.size());
            return false;
        }
        for (int i = 0; i < pa.size(); ++i) {
            if (!fuzzyEqual(pa.at(i).x(), pb.at(i).x()) || !fuzzyEqual(pa.at(i).y(), pb.at(i).y())) {
                reason = QStringLiteral("point set[%1] differs").arg(i);
                return false;
            }
        }
        return true;
    }
    if (a.type() == QVariant::List && b.type() == QVariant::List) {
        return listsEqual(a.toList(), b.toList(), reason);
    }
    if (a.type() == QVariant::Map && b.type() == QVariant::Map) {
        return mapsEqual(a.toMap(), b.toMap(), reason);
    }
    if ((a.type() == QVariant::Double || a.type() == QVariant::Int || a.type() == QVariant::UInt ||
         a.type() == QVariant::LongLong || a.type() == QVariant::ULongLong) &&
        (b.type() == QVariant::Double || b.type() == QVariant::Int || b.type() == QVariant::UInt ||
         b.type() == QVariant::LongLong || b.type() == QVariant::ULongLong)) {
        if (!fuzzyEqual(a.toDouble(), b.toDouble())) {
            reason = QStringLiteral("numeric %1 != %2").arg(a.toDouble()).arg(b.toDouble());
            return false;
        }
        return true;
    }
    if (a.type() == QVariant::PointF && b.type() == QVariant::PointF) {
        return fuzzyEqual(a.toPointF().x(), b.toPointF().x()) && fuzzyEqual(a.toPointF().y(), b.toPointF().y());
    }
    if (a == b) {
        return true;
    }
    // 自定义强类型：退化到 QString 表示比较，保证“同一输入下关键结果一致”。
    if (a.canConvert<QString>() && b.canConvert<QString>()) {
        if (a.toString() != b.toString()) {
            reason = QStringLiteral("value differs: %1 != %2").arg(a.toString(), b.toString());
            return false;
        }
        return true;
    }
    reason = QStringLiteral("value differs");
    return false;
}

SnapshotComparisonResult compareSnapshots(const Snapshot& reference, const Snapshot& actual, const Candidate& candidate,
                                          const char* phase) {
    QString reason;
    if (reference.success != actual.success) {
        reason = QStringLiteral("success %1 != %2").arg(reference.success).arg(actual.success);
    } else if (reference.errorCode != actual.errorCode) {
        reason = QStringLiteral("errorCode %1 != %2").arg(reference.errorCode).arg(actual.errorCode);
    } else if (reference.success && reference.userMessage != actual.userMessage) {
        reason = QStringLiteral("userMessage differs: %1 != %2").arg(reference.userMessage, actual.userMessage);
    } else if (!reference.success) {
        // 失败分支：只要求结构化错误一致（同一输入下不得出现跨线程差异）。
    }
    if (reason.isEmpty()) {
        if (reference.outputs.keys() != actual.outputs.keys()) {
            reason =
                QStringLiteral("output ports differ: %1 vs %2")
                    .arg(reference.outputs.keys().join(QLatin1Char(',')), actual.outputs.keys().join(QLatin1Char(',')));
        } else if (candidate.deterministicOutput) {
            for (auto it = reference.outputs.constBegin(); it != reference.outputs.constEnd(); ++it) {
                if (!valuesEqual(it.value(), actual.outputs.value(it.key()), reason)) {
                    reason = QStringLiteral("port '%1': %2").arg(it.key(), reason);
                    break;
                }
            }
        } else {
            // 非确定性输出（如系统时间）：只要求端口集合与载荷类型一致。
            for (auto it = reference.outputs.constBegin(); it != reference.outputs.constEnd(); ++it) {
                const QVariant other = actual.outputs.value(it.key());
                const QString refType = QString::fromLatin1(it.value().typeName());
                const QString otherType = QString::fromLatin1(other.typeName());
                if (refType != otherType) {
                    reason =
                        QStringLiteral("port '%1' payload type differs: %2 vs %3").arg(it.key(), refType, otherType);
                    break;
                }
            }
        }
    }
    return {reason.isEmpty(),
            QStringLiteral("%1/%2/%3").arg(candidate.name, candidate.category, QString::fromLatin1(phase)), reason};
}

} // namespace

class TestWorkerPluginConsistency : public QObject {
    Q_OBJECT

private:
    struct InstalledPlugin {
        QString dir;
        QString name;
        QString metadataPath;
    };

    QList<InstalledPlugin> m_installed;
    QTemporaryDir m_tempDir;
    QString m_cloudPath;

    /// 每插件运行期参数：IO/变量类候选需要真实文件路径或非空参数；其余保持 metadata 默认值。
    void applyTestParams(IModule* module, const QString& name, const QString& tag) const {
        QJsonObject params = module->defaultParams();
        if (name == QStringLiteral("SaveImage")) {
            params.insert(QStringLiteral("filePath"),
                          QDir(m_tempDir.path()).filePath(QStringLiteral("saved_%1.png").arg(tag)));
            params.insert(QStringLiteral("format"), QStringLiteral("png"));
        } else if (name == QStringLiteral("LoadPointCloud")) {
            params.insert(QStringLiteral("filePath"), m_cloudPath);
        } else if (name == QStringLiteral("SplitString")) {
            params.insert(QStringLiteral("inputString"), QStringLiteral("alpha,beta,gamma"));
        } else if (name == QStringLiteral("VarDefinePlugin")) {
            params.insert(QStringLiteral("variableName"), QStringLiteral("worker_var"));
            params.insert(QStringLiteral("variableType"), QStringLiteral("double"));
            params.insert(QStringLiteral("initialValue"), 1);
        } else if (name == QStringLiteral("VarSetPlugin")) {
            params.insert(QStringLiteral("variableName"), QStringLiteral("worker_var"));
            params.insert(QStringLiteral("value"), QStringLiteral("1"));
        } else if (name == QStringLiteral("Condition")) {
            params.insert(QStringLiteral("variableName"), QStringLiteral("worker_var"));
            params.insert(QStringLiteral("operator"), QStringLiteral("NotEmpty"));
        } else if (name == QStringLiteral("WriteText")) {
            params.insert(QStringLiteral("filePath"),
                          QDir(m_tempDir.path()).filePath(QStringLiteral("text_%1.txt").arg(tag)));
            params.insert(QStringLiteral("textContent"), QStringLiteral("worker consistency"));
        } else if (name == QStringLiteral("Folder")) {
            params.insert(QStringLiteral("folderPath"), m_tempDir.path());
            params.insert(QStringLiteral("operation"), QStringLiteral("Exists"));
        } else if (name == QStringLiteral("SaveData")) {
            params.insert(QStringLiteral("filePath"),
                          QDir(m_tempDir.path()).filePath(QStringLiteral("data_%1.json").arg(tag)));
            params.insert(QStringLiteral("fileFormat"), QStringLiteral("json"));
        }
        module->setParams(params);
    }

    /// 旧式 ImageData 数据键（非端口）由插件直接读取；为这些候选补充确定性载荷。
    void decorateInputs(const QString& name, PortValueMap& inputs) const {
        if (name != QStringLiteral("DataCheck") && name != QStringLiteral("Condition"))
            return;
        auto it = inputs.find(QStringLiteral("image"));
        if (it == inputs.end() || !it->canConvert<ImageData>())
            return;
        ImageData image = it->value<ImageData>();
        if (name == QStringLiteral("DataCheck"))
            image.setData(QStringLiteral("check_data"), 50.0);
        else
            image.setData(QStringLiteral("worker_var"), QStringLiteral("value"));
        it->setValue(image);
    }

    bool writePointCloudFile() {
        m_cloudPath = QDir(m_tempDir.path()).filePath(QStringLiteral("cloud.ply"));
        QFile file(m_cloudPath);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write("ply\nformat ascii 1.0\ncomment consistency fixture\n"
                   "element vertex 4\nproperty float x\nproperty float y\nproperty float z\nend_header\n"
                   "0 0 0\n1 0 0\n0 1 0\n1 1 0\n");
        file.close();
        return true;
    }

    const Candidate* candidateFor(const QString& dir) const {
        for (const Candidate& candidate : candidates()) {
            if (dir == QLatin1String(candidate.dir)) {
                return &candidate;
            }
        }
        return nullptr;
    }

    bool installPlugin(const PluginEntry& entry, InstalledPlugin& installed) {
        QFile metaFile(entry.metadataPath);
        if (!metaFile.open(QIODevice::ReadOnly)) {
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(metaFile.readAll());
        metaFile.close();
        if (!doc.isObject()) {
            return false;
        }
        const QJsonObject json = doc.object();
        const QString name = json.value(QStringLiteral("name")).toString();
        if (name.isEmpty()) {
            return false;
        }
        const QString dirName = DeepLuxTest::pluginInstallDir(entry);
        const QString targetDir = QDir(m_tempDir.filePath(QStringLiteral("plugins"))).filePath(dirName);
        if (!QDir().mkpath(targetDir)) {
            return false;
        }
        const QString libName = QFileInfo(entry.libraryPath).fileName();
        QFile::remove(QDir(targetDir).filePath(QStringLiteral("metadata.json")));
        QFile::remove(QDir(targetDir).filePath(libName));
        if (!QFile::copy(entry.metadataPath, QDir(targetDir).filePath(QStringLiteral("metadata.json"))) ||
            !QFile::copy(entry.libraryPath, QDir(targetDir).filePath(libName))) {
            return false;
        }
        installed.dir = dirName;
        installed.name = name;
        installed.metadataPath = entry.metadataPath;
        return true;
    }

private slots:
    void initTestCase() {
        QVERIFY2(m_tempDir.isValid(), "temporary plugin directory must be valid");
        qputenv("DEEPLUX_APP_DATA_DIR", m_tempDir.filePath(QStringLiteral("appdata")).toLocal8Bit());
        QVERIFY2(writePointCloudFile(), "must be able to write the PLY fixture");

        QStringList missing;
        for (const Candidate& candidate : candidates()) {
            const QString registryKey = QString::fromLatin1(candidate.dir);
            const PluginEntry entry = DeepLuxTest::pluginEntry(registryKey);
            if (entry.target.isEmpty()) {
                missing.append(registryKey);
                continue;
            }
            InstalledPlugin installed;
            if (!installPlugin(entry, installed)) {
                missing.append(registryKey);
                continue;
            }
            m_installed.append(installed);
        }
        QVERIFY2(missing.isEmpty(),
                 qPrintable(QStringLiteral("未构建/未安装的候选插件: ") + missing.join(QLatin1Char(','))));

        PluginManager::instance().shutdown();
        PluginManager::instance().addPluginPath(m_tempDir.filePath(QStringLiteral("plugins")));
        QVERIFY2(PluginManager::instance().initialize(), "PluginManager initialize failed");
        for (const InstalledPlugin& plugin : m_installed) {
            QString error;
            QVERIFY2(PluginManager::instance().loadPlugin(plugin.name), qPrintable(plugin.name));
        }
    }

    void cleanupTestCase() {
        PluginManager::instance().shutdown();
        qunsetenv("DEEPLUX_APP_DATA_DIR");
    }

    void testCandidateConsistency_data() {
        QTest::addColumn<QString>("dir");
        for (const Candidate& candidate : candidates()) {
            // 行名用 ASCII 源码目录名，避免控制台编码影响；插件显示名从安装的 metadata 解析。
            QTest::newRow(candidate.dir) << QString::fromLatin1(candidate.dir);
        }
    }

    void testCandidateConsistency() {
        QFETCH(QString, dir);
        const Candidate* candidate = candidateFor(dir);
        QVERIFY2(candidate != nullptr, "candidate must be registered");

        QString pluginName;
        for (const InstalledPlugin& plugin : m_installed) {
            if (plugin.dir == dir) {
                pluginName = plugin.name;
                break;
            }
        }
        QVERIFY2(!pluginName.isEmpty(), qPrintable(QStringLiteral("plugin not installed: %1").arg(dir)));

        std::unique_ptr<IModule> first(PluginManager::instance().createModule(pluginName));
        QVERIFY2(first != nullptr, qPrintable(QStringLiteral("createModule failed: %1").arg(pluginName)));
        applyTestParams(first.get(), pluginName, QStringLiteral("first"));
        QVERIFY2(first->initialize(), qPrintable(QStringLiteral("initialize failed: %1").arg(pluginName)));
        std::unique_ptr<IModule> second(PluginManager::instance().createModule(pluginName));
        QVERIFY2(second != nullptr, qPrintable(QStringLiteral("createModule(second) failed: %1").arg(pluginName)));
        applyTestParams(second.get(), pluginName, QStringLiteral("second"));
        QVERIFY2(second->initialize(), qPrintable(QStringLiteral("second initialize failed: %1").arg(pluginName)));

        PortValueMap inputs = buildInputs(first.get());
        decorateInputs(pluginName, inputs);
        const Snapshot guiSnapshot = snapshotOf(first.get(), inputs, 1);

        Snapshot sameInstanceWorker;
        std::thread sameInstanceThread([&]() { sameInstanceWorker = snapshotOf(first.get(), inputs, 1); });
        sameInstanceThread.join();

        Snapshot secondInstanceWorker;
        std::thread secondInstanceThread([&]() { secondInstanceWorker = snapshotOf(second.get(), inputs, 1); });
        secondInstanceThread.join();

        QStringList failures;
        const SnapshotComparisonResult threadResult =
            compareSnapshots(guiSnapshot, sameInstanceWorker, *candidate, "same-instance");
        if (!threadResult.equal) {
            failures.append(QStringLiteral("%1: %2").arg(threadResult.label, threadResult.reason));
        }
        const SnapshotComparisonResult instanceResult =
            compareSnapshots(sameInstanceWorker, secondInstanceWorker, *candidate, "two-instances");
        if (!instanceResult.equal) {
            failures.append(QStringLiteral("%1: %2").arg(instanceResult.label, instanceResult.reason));
        }

        // 已标记 worker 的插件必须真正跑到计算路径并产出端口（一致性失败分支不足以通过审计）。
        if (candidate->auditedWorker) {
            QVERIFY2(guiSnapshot.success, qPrintable(QStringLiteral("%1 audited as worker but run failed: %2")
                                                         .arg(pluginName, guiSnapshot.userMessage)));
            QVERIFY2(!guiSnapshot.outputs.isEmpty(),
                     qPrintable(QStringLiteral("%1 audited as worker but produced no output port").arg(pluginName)));
        }

        qInfo().noquote() << QStringLiteral("[audit] %1 (%2): success=%3 errorCode=%4 outputs=%5 message=%6")
                                 .arg(pluginName, QString::fromLatin1(candidate->category))
                                 .arg(guiSnapshot.success ? QStringLiteral("true") : QStringLiteral("false"))
                                 .arg(guiSnapshot.errorCode)
                                 .arg(QStringList(guiSnapshot.outputs.keys()).join(QLatin1Char(',')))
                                 .arg(guiSnapshot.userMessage);
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral(" | "))));
    }
};

QTEST_MAIN(TestWorkerPluginConsistency)
#include "test_workerpluginconsistency.moc"
