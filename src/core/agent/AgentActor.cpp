#include "AgentActor.h"
#include "AgentRunWaiter.h"

#include "AgentController.h"
#include "AnnotationSessionExport.h"
#include "AnnotationSessionTransform.h"
#include "SamPredictor.h"
#include "ToolSchema.h"
#include "base/ModuleBase.h"
#include "common/Logger.h"
#include "common/UserManager.h"
#include "communication/CommunicationManager.h"
#include "device/CameraManager.h"
#include "device/CameraParameterWrite.h"
#include "display/DisplayData.h"
#include "engine/FlowExecutionService.h"
#include "engine/RunEngine.h"
#include "interface/ICamera.h"
#include "io/LabelMeImporter.h"
#include "io/PlyLoader.h"
#include "io/TiffLoader.h"
#include "manager/GlobalVarManager.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "model/Annotation.h"
#include "model/AnnotationMaskOps.h"
#include "model/Project.h"
#include "platform/PathUtils.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaType>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QUndoCommand>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#endif

static void ensureAgentResources() {
    static const bool initialized = []() {
        Q_INIT_RESOURCE(agent_resources);
        return true;
    }();
    Q_UNUSED(initialized)
}

namespace DeepLux {

namespace {
constexpr int kOutputResponseLimit = 65536;
constexpr int kDefaultOutputPageSize = 100;
constexpr int kMaxOutputPageSize = 1000;
constexpr int kMaxAgentCycleIterations = 10;

QJsonObject dataSourceFileSnapshot(const QString& path) {
    const QFileInfo info(path);
    QJsonObject snapshot{{QStringLiteral("path"), info.absoluteFilePath()},
                         {QStringLiteral("resolvedPath"), info.canonicalFilePath()},
                         {QStringLiteral("readable"), false}};
    if (!info.exists() || !info.isFile())
        return snapshot;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return snapshot;

    bool identityAvailable = false;
    QString identity;
#ifdef Q_OS_UNIX
    struct stat status {};
    if (fstat(file.handle(), &status) == 0) {
        identity = QStringLiteral("%1:%2").arg(QString::number(static_cast<qulonglong>(status.st_dev)),
                                               QString::number(static_cast<qulonglong>(status.st_ino)));
        identityAvailable = true;
    }
#elif defined(Q_OS_WIN)
    const intptr_t nativeHandle = _get_osfhandle(file.handle());
    BY_HANDLE_FILE_INFORMATION fileInfo{};
    if (nativeHandle != -1 && GetFileInformationByHandle(reinterpret_cast<HANDLE>(nativeHandle), &fileInfo)) {
        identity = QStringLiteral("%1:%2:%3")
                       .arg(fileInfo.dwVolumeSerialNumber)
                       .arg(fileInfo.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                       .arg(fileInfo.nFileIndexLow, 8, 16, QLatin1Char('0'));
        identityAvailable = true;
    }
#endif

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 bytes = 0;
    constexpr qint64 chunkBytes = 1024 * 1024;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(chunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return snapshot;
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
        bytes += chunk.size();
    }
    if (file.error() != QFileDevice::NoError)
        return snapshot;

    snapshot.insert(QStringLiteral("readable"), true);
    snapshot.insert(QStringLiteral("byteCount"), bytes);
    snapshot.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
    snapshot.insert(QStringLiteral("fileIdentityAvailable"), identityAvailable);
    if (identityAvailable)
        snapshot.insert(QStringLiteral("fileIdentity"), identity);
    return snapshot;
}

bool sameDataSourceFileSnapshot(const QJsonObject& approved, const QJsonObject& current) {
    return approved.value(QStringLiteral("readable")).toBool() &&
           approved.value(QStringLiteral("fileIdentityAvailable")).toBool() &&
           current.value(QStringLiteral("readable")).toBool() &&
           current.value(QStringLiteral("fileIdentityAvailable")).toBool() &&
           approved.value(QStringLiteral("path")) == current.value(QStringLiteral("path")) &&
           approved.value(QStringLiteral("resolvedPath")) == current.value(QStringLiteral("resolvedPath")) &&
           approved.value(QStringLiteral("byteCount")) == current.value(QStringLiteral("byteCount")) &&
           approved.value(QStringLiteral("sha256")) == current.value(QStringLiteral("sha256")) &&
           approved.value(QStringLiteral("fileIdentity")) == current.value(QStringLiteral("fileIdentity"));
}

QByteArray dataSourceImportProjectFingerprint(QJsonObject projectJson) {
    // Project::addDataSource calls touch() and refreshes only this timestamp.
    projectJson.remove(QStringLiteral("modified"));
    return QCryptographicHash::hash(QJsonDocument(projectJson).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1)
        .toHex();
}

bool hasSafeSourceImageId(const ImageCoordinateProvenance& provenance) {
    const QUuid sourceId(provenance.sourceImageId);
    return provenance.isValid() && !sourceId.isNull() &&
           sourceId.toString(QUuid::WithoutBraces).compare(provenance.sourceImageId, Qt::CaseInsensitive) == 0;
}

QJsonObject detectionToJson(const Detection& detection, const ImageCoordinateProvenance& provenance) {
    QJsonObject result{{"x", detection.x},
                       {"y", detection.y},
                       {"width", detection.width},
                       {"height", detection.height},
                       {"score", detection.score},
                       {"label", detection.label},
                       {"coordinateSystem", QStringLiteral("image_pixels")}};
    if (hasSafeSourceImageId(provenance)) {
        if (const auto bounds =
                provenance.mapRectToSourceBounds(QRectF(detection.x, detection.y, detection.width, detection.height))) {
            result[QStringLiteral("sourceImageCoordinates")] =
                QJsonObject{{"sourceImageId", provenance.sourceImageId},
                            {"coordinateSystem", QStringLiteral("source_image_pixels")},
                            {"bbox", QJsonArray{bounds->x(), bounds->y(), bounds->width(), bounds->height()}}};
        }
    }
    return result;
}

QJsonObject imageOutputSummary(const ImageData& image) {
    QJsonObject summary{{"width", image.width()},
                        {"height", image.height()},
                        {"channels", image.channels()},
                        {"format", image.format()},
                        {"coordinateSystem", QStringLiteral("image_pixels")}};
    const ImageCoordinateProvenance provenance = image.coordinateProvenance();
    if (hasSafeSourceImageId(provenance)) {
        QJsonArray matrix;
        for (double coefficient : provenance.localToSourceMatrix())
            matrix.append(coefficient);
        summary[QStringLiteral("sourceCoordinates")] = QJsonObject{{"sourceImageId", provenance.sourceImageId},
                                                                   {"sourceWidth", provenance.sourceSize.width()},
                                                                   {"sourceHeight", provenance.sourceSize.height()},
                                                                   {"localToSource", matrix}};
    }
    if (image.hasData(QStringLiteral("height_invalid_value")))
        summary["noData"] = QJsonValue::fromVariant(image.data(QStringLiteral("height_invalid_value")));
    return summary;
}

QJsonValue outputValueToJson(const QVariant& value, bool* supported,
                             const ImageCoordinateProvenance& imageProvenance = ImageCoordinateProvenance()) {
    *supported = true;
    if (value.userType() == QMetaType::QPointF) {
        const QPointF point = value.toPointF();
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            *supported = false;
            return QJsonValue();
        }
        return QJsonArray{point.x(), point.y()};
    }
    if (value.userType() == qMetaTypeId<QVector<QPointF>>()) {
        QJsonArray points;
        for (const QPointF& point : value.value<QVector<QPointF>>()) {
            if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
                *supported = false;
                return QJsonValue();
            }
            points.append(QJsonArray{point.x(), point.y()});
        }
        return points;
    }
    if (value.userType() == qMetaTypeId<ImageData>())
        return imageOutputSummary(value.value<ImageData>());
    if (value.userType() == qMetaTypeId<PointCloudData>()) {
        const PointCloudData& cloud = *static_cast<const PointCloudData*>(value.constData());
        return QJsonObject{{"pointCount", static_cast<qint64>(cloud.size())},
                           {"hasNormals", cloud.hasNormals()},
                           {"hasColors", cloud.hasColors()},
                           {"coordinateSystem", QStringLiteral("unspecified")}};
    }
    if (value.userType() == qMetaTypeId<Circle2D>()) {
        const Circle2D circle = value.value<Circle2D>();
        QJsonObject result{{"centerX", circle.centerX},
                           {"centerY", circle.centerY},
                           {"radius", circle.radius},
                           {"score", circle.score},
                           {"coordinateSystem", QStringLiteral("image_pixels")}};
        if (hasSafeSourceImageId(imageProvenance)) {
            if (const auto center = imageProvenance.mapLocalToSource(QPointF(circle.centerX, circle.centerY))) {
                result[QStringLiteral("sourceImageCoordinates")] =
                    QJsonObject{{"sourceImageId", imageProvenance.sourceImageId},
                                {"coordinateSystem", QStringLiteral("source_image_pixels")},
                                {"centerX", center->x()},
                                {"centerY", center->y()}};
            }
        }
        return result;
    }
    if (value.userType() == qMetaTypeId<Ellipse2D>()) {
        const Ellipse2D ellipse = value.value<Ellipse2D>();
        if (!ellipse.isValid()) {
            *supported = false;
            return QJsonValue();
        }
        QJsonObject result{
            {"centerX", ellipse.centerX},           {"centerY", ellipse.centerY},
            {"majorRadius", ellipse.majorRadius},   {"minorRadius", ellipse.minorRadius},
            {"angleDegrees", ellipse.angleDegrees}, {"coordinateSystem", QStringLiteral("image_pixels")}};
        if (hasSafeSourceImageId(imageProvenance)) {
            if (const auto center = imageProvenance.mapLocalToSource(QPointF(ellipse.centerX, ellipse.centerY))) {
                result[QStringLiteral("sourceImageCoordinates")] =
                    QJsonObject{{"sourceImageId", imageProvenance.sourceImageId},
                                {"coordinateSystem", QStringLiteral("source_image_pixels")},
                                {"centerX", center->x()},
                                {"centerY", center->y()}};
            }
        }
        return result;
    }
    if (value.userType() == qMetaTypeId<DetectionList>()) {
        QJsonArray items;
        for (const Detection& item : value.value<DetectionList>().items)
            items.append(detectionToJson(item, imageProvenance));
        return items;
    }
    if (value.userType() == qMetaTypeId<Region2D>()) {
        const Region2D& region = *static_cast<const Region2D*>(value.constData());
        QJsonObject result{
            {"label", region.label},
            {"bbox", QJsonArray{region.bbox.x(), region.bbox.y(), region.bbox.width(), region.bbox.height()}},
            {"polygonPointCount", region.polygon.size()},
            {"maskRleChars", region.maskRle.size()},
            {"score", region.score},
            {"coordinateSystem", QStringLiteral("image_pixels")}};
        if (hasSafeSourceImageId(imageProvenance)) {
            if (const auto bounds = imageProvenance.mapRectToSourceBounds(region.bbox)) {
                result[QStringLiteral("sourceImageCoordinates")] =
                    QJsonObject{{"sourceImageId", imageProvenance.sourceImageId},
                                {"coordinateSystem", QStringLiteral("source_image_pixels")},
                                {"bbox", QJsonArray{bounds->x(), bounds->y(), bounds->width(), bounds->height()}}};
            }
        }
        return result;
    }
    if (value.type() == QVariant::List || value.type() == QVariant::StringList) {
        QJsonArray array;
        for (const QVariant& item : value.toList()) {
            const QJsonValue json = outputValueToJson(item, supported, imageProvenance);
            if (!*supported)
                return QJsonValue();
            array.append(json);
        }
        return array;
    }
    if (value.type() == QVariant::Map || value.type() == QVariant::Hash) {
        QJsonObject object;
        const QVariantMap map = value.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            const QJsonValue json = outputValueToJson(it.value(), supported, imageProvenance);
            if (!*supported)
                return QJsonValue();
            object[it.key()] = json;
        }
        return object;
    }
    switch (value.type()) {
    case QVariant::Bool:
    case QVariant::Int:
    case QVariant::UInt:
    case QVariant::LongLong:
    case QVariant::ULongLong:
    case QVariant::Double:
        if (!std::isfinite(value.toDouble())) {
            *supported = false;
            return QJsonValue();
        }
        return QJsonValue::fromVariant(value);
    case QVariant::String:
        return QJsonValue::fromVariant(value);
    default:
        *supported = false;
        return QJsonValue();
    }


// 后台执行（t13 观察）：在 GUI 线程用事件循环等待 Service job 真正收尾。
// 不得用 QWaitCondition/waitForIdle 同步阻塞 GUI——worker 可能正在通过
// BlockingQueuedConnection 等 GUI 线程执行 gui 亲和模块，同步等待会与它互等至超时。
bool waitForServiceIdleViaEvents(FlowExecutionService& service, int timeoutMs) {
    if (!service.isActive()) {
        return true;
    }
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const QMetaObject::Connection finishedConn =
        QObject::connect(&service, &FlowExecutionService::jobFinished, &loop, [&loop]() { loop.quit(); });
    const QMetaObject::Connection failedConn =
        QObject::connect(&service, &FlowExecutionService::jobFailed, &loop, [&loop]() { loop.quit(); });
    timer.start(qMax(1, timeoutMs));
    // 连接建立后重新检查：收尾可能发生在“isActive() 检查”和“连接建立”之间。
    if (service.isActive()) {
        loop.exec();
    }
    QObject::disconnect(finishedConn);
    QObject::disconnect(failedConn);
    return !service.isActive();
}

QString validationError(const QString& toolName, const QJsonObject& params) {
    ToolDefinition def = ToolSchema::instance().findTool(toolName);
    if (def.name.isEmpty())
        return QString("Unknown tool: %1").arg(toolName);

    if (toolName == QLatin1String("save_viewport_snapshot_for_user")) {
        const QJsonValue viewportId = params.value(QStringLiteral("viewportId"));
        if (params.size() != 1 || !viewportId.isString() || viewportId.toString().trimmed().isEmpty())
            return QStringLiteral("Only a non-empty viewportId parameter is accepted");
    }
    if (toolName == QLatin1String("open_data_source_folder_for_user")) {
        const QJsonValue dataSourceId = params.value(QStringLiteral("dataSourceId"));
        if (params.size() != 1 || !dataSourceId.isString() || dataSourceId.toString().trimmed().isEmpty())
            return QStringLiteral("Only a non-empty dataSourceId parameter is accepted");
    }
    if (toolName == QLatin1String("open_user_knowledge_folder_for_user") && !params.isEmpty())
        return QStringLiteral("This knowledge folder action does not accept parameters");
    if (toolName == QLatin1String("open_global_variables_dialog") && !params.isEmpty())
        return QStringLiteral("This global variables action does not accept parameters");

    for (const ToolParam& param : def.params) {
        const QJsonValue value = params.value(param.name);
        if (param.name == QLatin1String("flowId") && !value.isUndefined() &&
            (!value.isString() || value.toString().trimmed().isEmpty()))
            return QStringLiteral("Invalid string parameter: flowId");
        if (toolName == QLatin1String("display_data_source_in_dedicated_viewport") &&
            param.name == QLatin1String("dataSourceId") && !value.isUndefined() &&
            (!value.isString() || value.toString().trimmed().isEmpty()))
            return QStringLiteral("Invalid string parameter: dataSourceId");
        if (param.required && value.isUndefined()) {
            return QString("Missing required parameter: %1").arg(param.name);
        }
        if (toolName == QLatin1String("set_module_position")) {
            if (param.name == QLatin1String("instanceId") &&
                (!value.isString() || value.toString().trimmed().isEmpty()))
                return QString("Invalid string parameter: %1").arg(param.name);
            if (param.name == QLatin1String("x") || param.name == QLatin1String("y")) {
                const double coordinate = value.toDouble(std::numeric_limits<double>::quiet_NaN());
                if (!value.isDouble() || !std::isfinite(coordinate) || std::floor(coordinate) != coordinate ||
                    coordinate < std::numeric_limits<int>::min() || coordinate > std::numeric_limits<int>::max())
                    return QString("Invalid integer parameter: %1").arg(param.name);
            }
        }
        if (param.required && value.isNull() && param.type != QLatin1String("json"))
            return QString("Invalid null parameter: %1").arg(param.name);
        if (param.required && param.type != QLatin1String("json") && value.isString() &&
            value.toString().trimmed().isEmpty() &&
            !(toolName == QLatin1String("set_module_note") && param.name == QLatin1String("note")))
            return QString("Missing required parameter: %1").arg(param.name);
        if (!param.enumValues.isEmpty() && !value.isUndefined() && !value.isNull() &&
            !param.enumValues.contains(value.toString())) {
            return QString("Invalid value for %1: %2").arg(param.name, value.toString());
        }
    }
    if (toolName == QLatin1String("set_camera_parameter")) {
        static const QSet<QString> allowed{QStringLiteral("deviceId"), QStringLiteral("cameraSessionGeneration"),
                                           QStringLiteral("serialNumber"), QStringLiteral("parameter"),
                                           QStringLiteral("value")};
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            if (!allowed.contains(it.key()))
                return QStringLiteral("Unexpected parameter field: %1").arg(it.key());
        const QString deviceId = params.value(QStringLiteral("deviceId")).toString();
        const QString generation = params.value(QStringLiteral("cameraSessionGeneration")).toString();
        bool generationOk = false;
        const qulonglong generationValue = generation.toULongLong(&generationOk, 10);
        if (deviceId.isEmpty() || deviceId != deviceId.trimmed() ||
            !QRegularExpression(QStringLiteral("^(0|[1-9][0-9]*)$")).match(generation).hasMatch() || !generationOk ||
            QString::number(generationValue) != generation)
            return QStringLiteral("Invalid camera identity or binding generation");
        const QString parameter = params.value(QStringLiteral("parameter")).toString();
        const QJsonValue value = params.value(QStringLiteral("value"));
        if (parameter == QLatin1String("exposureTime") || parameter == QLatin1String("gain") ||
            parameter == QLatin1String("frameRate")) {
            if (!value.isDouble() || !std::isfinite(value.toDouble()))
                return QStringLiteral("Invalid finite number parameter value");
        } else if (parameter == QLatin1String("triggerMode")) {
            if (!value.isString() ||
                (value.toString() != QLatin1String("software") && value.toString() != QLatin1String("hardware") &&
                 value.toString() != QLatin1String("continuous")))
                return QStringLiteral("Invalid trigger mode parameter value");
        } else if (parameter == QLatin1String("roi")) {
            if (!value.isObject())
                return QStringLiteral("ROI value must be an object");
            const QJsonObject roi = value.toObject();
            if (roi.size() != 4 || !roi.contains(QStringLiteral("x")) || !roi.contains(QStringLiteral("y")) ||
                !roi.contains(QStringLiteral("width")) || !roi.contains(QStringLiteral("height")))
                return QStringLiteral("ROI must contain only x, y, width, and height");
            for (const QString& key :
                 {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")}) {
                const QJsonValue coordinate = roi.value(key);
                const double number = coordinate.toDouble(std::numeric_limits<double>::quiet_NaN());
                if (!coordinate.isDouble() || !std::isfinite(number) || std::floor(number) != number ||
                    number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
                    return QStringLiteral("ROI coordinates must be finite integers");
            }
            if (roi.value(QStringLiteral("x")).toInt() < 0 || roi.value(QStringLiteral("y")).toInt() < 0 ||
                roi.value(QStringLiteral("width")).toInt() <= 0 || roi.value(QStringLiteral("height")).toInt() <= 0)
                return QStringLiteral("ROI origin must be nonnegative and size must be positive");
        } else {
            return QStringLiteral("Unsupported camera parameter");
        }
    }
    if (toolName == QLatin1String("discover_cameras")) {
        static const QSet<QString> allowed{QStringLiteral("registryGeneration"), QStringLiteral("pluginIds"),
                                           QStringLiteral("unsupportedPluginIds")};
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            if (!allowed.contains(it.key()))
                return QStringLiteral("Unexpected parameter field: %1").arg(it.key());
        const QString generation = params.value(QStringLiteral("registryGeneration")).toString();
        bool generationOk = false;
        const qulonglong generationValue = generation.toULongLong(&generationOk, 10);
        if (!QRegularExpression(QStringLiteral("^(0|[1-9][0-9]*)$")).match(generation).hasMatch() || !generationOk ||
            QString::number(generationValue) != generation)
            return QStringLiteral("registryGeneration must be a canonical unsigned decimal string");
        for (const QString& key : {QStringLiteral("pluginIds"), QStringLiteral("unsupportedPluginIds")}) {
            const QJsonValue value = params.value(key);
            if (!value.isArray())
                return QStringLiteral("%1 must be an array of sorted unique strings").arg(key);
            QStringList ids;
            for (const QJsonValue& item : value.toArray()) {
                if (!item.isString() || item.toString().trimmed().isEmpty() ||
                    item.toString() != item.toString().trimmed())
                    return QStringLiteral("%1 must contain non-empty trimmed strings").arg(key);
                ids.append(item.toString());
            }
            QStringList sorted = ids;
            sorted.removeDuplicates();
            std::sort(sorted.begin(), sorted.end());
            if (sorted != ids)
                return QStringLiteral("%1 must be sorted and unique").arg(key);
        }
    }
    if (toolName == QLatin1String("get_camera_discovery_operation")) {
        static const QSet<QString> allowed{QStringLiteral("requestId")};
        for (auto it = params.constBegin(); it != params.constEnd(); ++it)
            if (!allowed.contains(it.key()))
                return QStringLiteral("Unexpected parameter field: %1").arg(it.key());
        const QString requestId = params.value(QStringLiteral("requestId")).toString();
        bool requestIdOk = false;
        const qulonglong parsed = requestId.toULongLong(&requestIdOk, 10);
        if (!QRegularExpression(QStringLiteral("^[1-9][0-9]*$")).match(requestId).hasMatch() || !requestIdOk ||
            QString::number(parsed) != requestId)
            return QStringLiteral("requestId must be a canonical nonzero unsigned decimal string");
    }
    return QString();
}

// 阶段1 §1 统一失败返回(makeToolError,避开 toolError 信号名)：error(人类可读) + errorCode(稳定枚举) +
// retryable(能否原样重试)， details 可选。保留既有 error
// 字段,新增字段一律增量（docs/baseline/agent-phase-1-acceptance.md §1.1）
QJsonObject makeToolError(const QString& message, const QString& errorCode, bool retryable = false,
                          const QJsonObject& details = QJsonObject()) {
    QJsonObject obj{{"error", message}, {"errorCode", errorCode}, {"retryable", retryable}};
    if (!details.isEmpty())
        obj.insert(QStringLiteral("details"), details);
    return obj;
}

QJsonObject makeSamUncertainToolResult(const SamPredictOutcome& outcome) {
    return QJsonObject{{"status", QStringLiteral("uncertain")},
                       {"sideEffectStatus", QStringLiteral("uncertain")},
                       {"retryable", false},
                       {"errorCode", QStringLiteral("SIDE_EFFECT_UNCERTAIN")},
                       {"error", outcome.error},
                       {"uncertainStage", outcome.uncertainStage}};
}

// validationError 文案 → 稳定错误码
QString validationErrorCode(const QString& message) {
    if (message.startsWith(QLatin1String("Unknown tool")))
        return QStringLiteral("UNKNOWN_TOOL");
    if (message.startsWith(QLatin1String("Missing required parameter")))
        return QStringLiteral("MISSING_PARAM");
    return QStringLiteral("INVALID_PARAM");
}

QJsonObject makeToolValidationError(const QString& toolName, const QString& message) {
    if (toolName == QLatin1String("disconnect_camera"))
        return makeToolError(message, validationErrorCode(message), true,
                             QJsonObject{{"hardwareDisconnectConfirmed", false}});
    return makeToolError(message, validationErrorCode(message), true);
}

QJsonObject makeSkippedToolError(const QString& toolName, const QString& message, const QString& failedCallId) {
    QJsonObject details{{"failedCallId", failedCallId}};
    if (toolName == QLatin1String("disconnect_camera"))
        details.insert(QStringLiteral("hardwareDisconnectConfirmed"), false);
    return makeToolError(message, QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"), false, details);
}

// 显式幂等 no_op（阶段1 §1：非法状态但无害,不得谎称完成动作）
QJsonObject toolNoOp(const QString& reason) {
    return QJsonObject{{"status", QStringLiteral("no_op")}, {"reason", reason}};
}

const PortSpec* findPort(const QList<PortSpec>& ports, const QString& id) {
    for (const PortSpec& port : ports) {
        if (port.id == id)
            return &port;
    }
    return nullptr;
}

bool portTypesCompatible(const PortSpec& output, const PortSpec& input) {
    return output.control == input.control &&
           (output.type == DataType::Any || input.type == DataType::Any || output.type == input.type);
}

QJsonArray portSpecsToJson(const QList<PortSpec>& ports) {
    QJsonArray values;
    for (const PortSpec& port : ports)
        values.append(QJsonObject{{"id", port.id},
                                  {"displayName", port.displayName},
                                  {"type", dataTypeName(port.type)},
                                  {"required", port.required},
                                  {"multiple", port.multiple},
                                  {"control", port.control},
                                  {"joinPolicy", port.joinPolicy == ControlJoinPolicy::All ? "all" : "any"}});
    return values;
}

QByteArray projectFingerprint(const Project* project) {
    return project ? QCryptographicHash::hash(QJsonDocument(project->toJson()).toJson(QJsonDocument::Compact),
                                              QCryptographicHash::Sha1)
                   : QByteArray();
}

bool isAgentVariableType(VarDataType type) {
    return type == VarDataType::Int || type == VarDataType::Double || type == VarDataType::String ||
           type == VarDataType::Bool || type == VarDataType::IntArray || type == VarDataType::DoubleArray ||
           type == VarDataType::StringArray || type == VarDataType::BoolArray;
}

QJsonValue variableValueToJson(const QVariant& value) {
    if (value.userType() == QMetaType::QVariantList) {
        QJsonArray array;
        for (const QVariant& item : value.toList())
            array.append(QJsonValue::fromVariant(item));
        return array;
    }
    return QJsonValue::fromVariant(value);
}

bool jsonValueToVariableValue(VarDataType type, const QJsonValue& json, QVariant& value, QString& errorCode) {
    auto numberAsInt = [](const QJsonValue& item, int& result) {
        if (!item.isDouble())
            return false;
        const double number = item.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || number < std::numeric_limits<int>::min() ||
            number > std::numeric_limits<int>::max())
            return false;
        result = static_cast<int>(number);
        return true;
    };
    switch (type) {
    case VarDataType::Int: {
        int result = 0;
        if (!numberAsInt(json, result))
            break;
        value = result;
        return true;
    }
    case VarDataType::Double:
        if (json.isDouble() && std::isfinite(json.toDouble())) {
            value = json.toDouble();
            return true;
        }
        break;
    case VarDataType::String:
        if (json.isString()) {
            value = json.toString();
            return true;
        }
        break;
    case VarDataType::Bool:
        if (json.isBool()) {
            value = json.toBool();
            return true;
        }
        break;
    case VarDataType::IntArray:
    case VarDataType::DoubleArray:
    case VarDataType::StringArray:
    case VarDataType::BoolArray: {
        if (!json.isArray())
            break;
        QVariantList result;
        const QJsonArray array = json.toArray();
        result.reserve(array.size());
        for (const QJsonValue& item : array) {
            if (type == VarDataType::IntArray) {
                int number = 0;
                if (!numberAsInt(item, number)) {
                    errorCode = QStringLiteral("VARIABLE_TYPE_MISMATCH");
                    return false;
                }
                result.append(number);
            } else if (type == VarDataType::DoubleArray) {
                if (!item.isDouble() || !std::isfinite(item.toDouble())) {
                    errorCode = QStringLiteral("VARIABLE_TYPE_MISMATCH");
                    return false;
                }
                result.append(item.toDouble());
            } else if (type == VarDataType::StringArray) {
                if (!item.isString()) {
                    errorCode = QStringLiteral("VARIABLE_TYPE_MISMATCH");
                    return false;
                }
                result.append(item.toString());
            } else {
                if (!item.isBool()) {
                    errorCode = QStringLiteral("VARIABLE_TYPE_MISMATCH");
                    return false;
                }
                result.append(item.toBool());
            }
        }
        value = result;
        return true;
    }
    default:
        errorCode = QStringLiteral("UNSUPPORTED_TYPE");
        return false;
    }
    errorCode = QStringLiteral("VARIABLE_TYPE_MISMATCH");
    return false;
}

QJsonObject variableDto(const GlobalVariableSnapshot& snapshot, bool includeValue) {
    QJsonObject dto{{QStringLiteral("name"), snapshot.name},
                    {QStringLiteral("type"), VarModel::dataTypeToString(snapshot.type)},
                    {QStringLiteral("editable"), isAgentVariableType(snapshot.type)}};
    if (includeValue && isAgentVariableType(snapshot.type))
        dto.insert(QStringLiteral("value"), variableValueToJson(snapshot.value));
    return dto;
}


QJsonObject errorObject(const QString& error, const QString& code) {
    QJsonObject result{{"error", error}};
    if (!code.isEmpty())
        result.insert(QStringLiteral("code"), code);
    return result;
}

/// tune_node_and_verify 的参数级失败（success:false + code:invalid_parameter）
QJsonObject invalidParameter(const QString& error, const QString& parameter) {
    QJsonObject result{{"success", false}, {"error", error}, {"code", QStringLiteral("invalid_parameter")}};
    if (!parameter.isEmpty())
        result.insert(QStringLiteral("parameter"), parameter);
    return result;
}

QString scalarToString(const QJsonValue& value) {
    if (value.isString())
        return value.toString();
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) {
        const double d = value.toDouble();
        if (d == static_cast<double>(qRound64(d)) && qAbs(d) < 1.0e15)
            return QString::number(qRound64(d));
        return QString::number(d);
    }
    if (value.isNull() || value.isUndefined())
        return QString();
    return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
}

/// 运行期语义的完整参数：插件默认值 + 工程参数覆盖
QJsonObject mergedParams(IModule* mod, const ModuleInstance& inst) {
    QJsonObject merged = mod->defaultParams();
    for (auto it = inst.params.constBegin(); it != inst.params.constEnd(); ++it)
        merged.insert(it.key(), it.value());
    return merged;
}

struct ParamNormalizeResult {
    bool ok = false;
    QJsonValue value;
    QString error;
};

/**
 * @brief set_param / tune_node_and_verify 共用的参数值归一化
 *
 * 键必须存在于插件默认参数或 ui.parameters；字符串按目标类型（既有值或默认值类型）
 * 转换；类型/范围校验由调用方对完整 merged 参数调用 validateParams 完成。
 */
ParamNormalizeResult normalizeParamValue(IModule* mod, const ModuleInstance& inst, const QString& key,
                                         const QJsonValue& raw) {
    ParamNormalizeResult result;
    const QJsonObject defaults = mod->defaultParams();
    const QJsonObject uiParams =
        PluginManager::instance().pluginInfo(inst.moduleId).ui.value(QStringLiteral("parameters")).toObject();
    if (!defaults.contains(key) && !uiParams.contains(key)) {
        result.error = QString("Unknown parameter '%1' for module %2").arg(key, inst.moduleId);
        return result;
    }
    QJsonValue newVal = raw;
    if (newVal.isString()) {
        const QString s = newVal.toString().trimmed();
        const QJsonValue ref = inst.params.contains(key) ? inst.params.value(key) : defaults.value(key);
        if (ref.isDouble()) {
            bool ok = false;
            const double d = s.toDouble(&ok);
            if (!ok) {
                result.error = QString("Value '%1' is not a number for key %2").arg(s, key);
                return result;
            }
            newVal = d;
        } else if (ref.isBool()) {
            if (s.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0) {
                newVal = true;
            } else if (s.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0) {
                newVal = false;
            } else {
                result.error = QString("Value '%1' is not a boolean for key %2").arg(s, key);
                return result;
            }
        }
    }
    result.ok = true;
    result.value = newVal;
    return result;
}

struct NodeRef {
    QString runtimeName; // 引擎侧运行名（§4.0 解析规则）
    QString instanceId;  // 工程实例 id
    QString nodeName;    // 工程显示名
    QString moduleId;
};

bool resolveProjectModule(const Project* project, const QString& nodeName, ModuleInstance* out) {
    if (!project || nodeName.trimmed().isEmpty())
        return false;
    for (const ModuleInstance& inst : project->modules()) {
        if (inst.name == nodeName) {
            *out = inst;
            return true;
        }
    }
    for (const ModuleInstance& inst : project->modules()) {
        if (inst.id == nodeName) {
            *out = inst;
            return true;
        }
    }
    return false;
}

/// 解析节点（名称优先、其次实例 id），并要求引擎已加载该模块
bool resolveNodeRef(const Project* project, const QString& nodeName, NodeRef* ref) {
    ModuleInstance inst;
    if (!resolveProjectModule(project, nodeName, &inst))
        return false;
    RunEngine& engine = RunEngine::instance();
    QString runtimeName;
    if (engine.getModule(inst.name))
        runtimeName = inst.name;
    else if (engine.getModule(inst.id))
        runtimeName = inst.id;
    else
        return false;
    ref->runtimeName = runtimeName;
    ref->instanceId = inst.id;
    ref->nodeName = inst.name;
    ref->moduleId = inst.moduleId;
    return true;
}

bool serializePortValue(const QVariant& value, QJsonValue* out) {
    const int typeId = value.userType();
    if (typeId == qMetaTypeId<ImageData>()) {
        const ImageData image = value.value<ImageData>();
        *out = QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                           {QStringLiteral("width"), image.width()},
                           {QStringLiteral("height"), image.height()},
                           {QStringLiteral("present"), image.isValid()}};
        return true;
    }
    switch (typeId) {
    case QMetaType::Bool:
        *out = value.toBool();
        return true;
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        *out = static_cast<double>(value.toLongLong());
        return true;
    case QMetaType::Double:
    case QMetaType::Float:
        *out = value.toDouble();
        return true;
    case QMetaType::QString: {
        const QString s = value.toString();
        if (s.size() <= 512) {
            *out = s;
        } else {
            *out = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                               {QStringLiteral("value"), s.left(512)},
                               {QStringLiteral("truncated"), true}};
        }
        return true;
    }
    case QMetaType::QVariantList: {
        const QVariantList list = value.toList();
        QJsonObject obj{{QStringLiteral("type"), QStringLiteral("list")}, {QStringLiteral("count"), list.size()}};
        if (!list.isEmpty()) {
            QJsonValue first;
            if (serializePortValue(list.first(), &first))
                obj.insert(QStringLiteral("first"), first);
        }
        *out = obj;
        return true;
    }
    default:
        return false;
    }
}

struct PortOutputsPayload {
    QJsonObject outputs;
    QStringList unsupported;
};

PortOutputsPayload serializePortOutputs(const PortValueMap& ports) {
    PortOutputsPayload payload;
    for (auto it = ports.constBegin(); it != ports.constEnd(); ++it) {
        QJsonValue serialized;
        if (serializePortValue(it.value(), &serialized))
            payload.outputs.insert(it.key(), serialized);
        else
            payload.unsupported.append(it.key());
    }
    payload.unsupported.sort();
    return payload;
}

QString logLevelName(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("debug");
    case LogLevel::Info:
        return QStringLiteral("info");
    case LogLevel::Warning:
        return QStringLiteral("warning");
    case LogLevel::Error:
        return QStringLiteral("error");
    case LogLevel::Success:
        return QStringLiteral("success");
    }
    return QStringLiteral("info");
}
} // namespace

// --- Undo Commands ---

class AgentAddModuleCmd : public QUndoCommand {
public:
    AgentAddModuleCmd(const QString& moduleId, const QString& instanceId, const QString& displayName,
                      QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_moduleId(moduleId), m_instanceId(instanceId), m_displayName(displayName) {}

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj)
            proj->removeModule(m_instanceId, Project::ModuleOrderChangeSource::Agent);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        ModuleInstance inst;
        inst.id = m_instanceId;
        inst.moduleId = m_moduleId;
        inst.name = m_displayName;
        proj->addModule(inst, -1, Project::ModuleOrderChangeSource::Agent);
    }

    QString instanceId() const {
        return m_instanceId;
    }

private:
    QString m_moduleId;
    QString m_instanceId;
    QString m_displayName;
};

class AgentRemoveModuleCmd : public QUndoCommand {
public:
    AgentRemoveModuleCmd(const QString& instanceId, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_instanceId(instanceId) {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj) {
            const std::optional<ModuleInstance> inst = proj->moduleById(instanceId);
            if (inst)
                m_backup = *inst;
            const QList<ModuleInstance> modules = proj->modules();
            for (int i = 0; i < modules.size(); ++i) {
                if (modules[i].id == instanceId) {
                    m_index = i;
                    break;
                }
            }
            // 阶7 Agent 评审修复（P1-3）：Project::removeModule 不级联删连接，
            // 由命令备份关联连接并在 redo/undo 两侧维护连接一致性
            const QList<ModuleConnection> connections = proj->connections();
            for (int i = 0; i < connections.size(); ++i) {
                const ModuleConnection& c = connections[i];
                if (c.fromModuleId == instanceId || c.toModuleId == instanceId)
                    m_connBackup.append(qMakePair(i, c));
            }
        }
    }
    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (proj && !m_backup.id.isEmpty()) {
            proj->addModule(m_backup, m_index, Project::ModuleOrderChangeSource::Agent);
            for (const auto& entry : m_connBackup)
                proj->addConnection(entry.second, entry.first);
        }
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        for (const auto& entry : m_connBackup) {
            const ModuleConnection& c = entry.second;
            proj->removeConnectionWithPorts(c.fromModuleId, c.fromPort, c.toModuleId, c.toPort);
        }
        proj->removeModule(m_instanceId, Project::ModuleOrderChangeSource::Agent);
    }

private:
    QString m_instanceId;
    ModuleInstance m_backup;
    int m_index = -1;
    QList<QPair<int, ModuleConnection>> m_connBackup;
};

class AgentMoveModuleCmd : public QUndoCommand {
public:
    AgentMoveModuleCmd(Project* project, const QString& instanceId, int oldIndex, int newIndex,
                       const QStringList& oldOrder, std::function<void()> expired)
        : m_project(project), m_instanceId(instanceId), m_oldIndex(oldIndex), m_newIndex(newIndex),
          m_expectedOrder(oldOrder), m_externalGeneration(project ? project->moduleOrderExternalGeneration() : 0),
          m_expired(std::move(expired)) {}

    void undo() override {
        apply(m_oldIndex);
    }
    void redo() override {
        apply(m_newIndex);
    }

private:
    static QStringList orderOf(const Project* project) {
        QStringList order;
        if (project) {
            const QList<ModuleInstance> modules = project->modules();
            order.reserve(modules.size());
            for (const ModuleInstance& module : modules)
                order.append(module.id);
        }
        return order;
    }

    void apply(int index) {
        if (m_invalid) {
            if (m_expired)
                m_expired();
            return;
        }
        Project* project = ProjectManager::instance().currentProject();
        if (!m_project || project != m_project || project->moduleOrderExternalGeneration() != m_externalGeneration ||
            orderOf(project) != m_expectedOrder || !project->moduleById(m_instanceId) ||
            !project->moveModule(m_instanceId, index, Project::ModuleOrderChangeSource::Agent)) {
            m_invalid = true;
            if (m_expired)
                m_expired();
            return;
        }
        m_expectedOrder = orderOf(project);
    }

    QPointer<Project> m_project;
    QString m_instanceId;
    int m_oldIndex;
    int m_newIndex;
    QStringList m_expectedOrder;
    quint64 m_externalGeneration = 0;
    std::function<void()> m_expired;
    bool m_invalid = false;
};

class AgentSetModulePositionCmd : public QUndoCommand {
public:
    AgentSetModulePositionCmd(Project* project, const QString& instanceId, int oldX, int oldY, int newX, int newY,
                              quint64 expectedAgentGeneration, std::function<void(bool)> expired)
        : m_project(project), m_instanceId(instanceId), m_expectedX(oldX), m_expectedY(oldY), m_oldX(oldX),
          m_oldY(oldY), m_newX(newX), m_newY(newY), m_expectedAgentGeneration(expectedAgentGeneration),
          m_expired(std::move(expired)) {
        setText(QStringLiteral("Set module position %1").arg(instanceId));
    }

    void undo() override {
        apply(m_oldX, m_oldY, false);
    }

    void redo() override {
        apply(m_newX, m_newY, true);
    }

private:
    void expire(bool redo) {
        if (m_invalid)
            return;
        m_invalid = true;
        if (m_expired)
            m_expired(redo);
    }

    void apply(int x, int y, bool redo) {
        if (m_invalid) {
            if (m_expired)
                m_expired(redo);
            return;
        }
        Project* project = m_project.data();
        if (!project || ProjectManager::instance().currentProject() != project) {
            expire(redo);
            return;
        }
        const std::optional<ModuleInstance> current = project->moduleById(m_instanceId);
        const Project::PositionGenerations generations = project->positionGenerations(m_instanceId);
        if (!current || current->posX != m_expectedX || current->posY != m_expectedY ||
            generations.agent != m_expectedAgentGeneration) {
            expire(redo);
            return;
        }
        if (current->posX == x && current->posY == y)
            return;
        ModuleInstance updated = *current;
        updated.posX = x;
        updated.posY = y;
        project->updateModule(m_instanceId, updated, Project::PositionChangeSource::Agent);
        m_expectedX = updated.posX;
        m_expectedY = updated.posY;
    }

private:
    QPointer<Project> m_project;
    QString m_instanceId;
    int m_expectedX;
    int m_expectedY;
    int m_oldX = 0;
    int m_oldY = 0;
    int m_newX;
    int m_newY;
    quint64 m_expectedAgentGeneration = 0;
    std::function<void(bool)> m_expired;
    bool m_invalid = false;
};

class AgentRenameModuleCmd : public QUndoCommand {
public:
    AgentRenameModuleCmd(const QString& instanceId, const QString& oldName, const QString& newName)
        : m_instanceId(instanceId), m_oldName(oldName), m_newName(newName) {}

    void undo() override {
        apply(m_oldName);
    }
    void redo() override {
        apply(m_newName);
    }

private:
    void apply(const QString& name) {
        Project* project = ProjectManager::instance().currentProject();
        if (!project)
            return;
        const std::optional<ModuleInstance> current = project->moduleById(m_instanceId);
        if (!current)
            return;
        ModuleInstance updated = *current;
        updated.name = name;
        project->updateModule(m_instanceId, updated);
    }

    QString m_instanceId;
    QString m_oldName;
    QString m_newName;
};

class AgentRuntimeFieldCmd : public QUndoCommand {
public:
    AgentRuntimeFieldCmd(Project* project, const QString& instanceId, Project::RuntimeField field,
                         const QVariant& before, const QVariant& after, quint64 externalGeneration,
                         quint64 instanceGeneration, std::function<void()> expired)
        : m_project(project), m_instanceId(instanceId), m_field(field), m_expectedValue(before), m_before(before),
          m_after(after), m_externalGeneration(externalGeneration), m_instanceGeneration(instanceGeneration),
          m_expired(std::move(expired)) {
        setText(QStringLiteral("Set module runtime field %1").arg(instanceId));
    }

    void undo() override {
        apply(m_before);
    }

    void redo() override {
        apply(m_after);
    }

private:
    QVariant currentValue(const ModuleInstance& instance) const {
        switch (m_field) {
        case Project::RuntimeField::Note:
            return instance.note;
        case Project::RuntimeField::Enabled:
            return instance.enabled;
        case Project::RuntimeField::Breakpoint:
            return instance.breakpoint;
        }
        return {};
    }

    void writeValue(ModuleInstance& instance, const QVariant& value) const {
        switch (m_field) {
        case Project::RuntimeField::Note:
            instance.note = value.toString();
            break;
        case Project::RuntimeField::Enabled:
            instance.enabled = value.toBool();
            break;
        case Project::RuntimeField::Breakpoint:
            instance.breakpoint = value.toBool();
            break;
        }
    }

    void notifyExpired() const {
        if (m_expired)
            m_expired();
    }

    void apply(const QVariant& value) {
        if (m_invalid) {
            notifyExpired();
            return;
        }
        Project* project = m_project.data();
        if (!project || ProjectManager::instance().currentProject() != project ||
            project->parameterInstanceGeneration(m_instanceId) != m_instanceGeneration ||
            project->runtimeFieldExternalGeneration(m_instanceId, m_field) != m_externalGeneration) {
            m_invalid = true;
            notifyExpired();
            return;
        }
        const std::optional<ModuleInstance> current = project->moduleById(m_instanceId);
        if (!current || currentValue(*current) != m_expectedValue) {
            m_invalid = true;
            notifyExpired();
            return;
        }
        if (m_expectedValue == value)
            return;

        ModuleInstance updated = *current;
        writeValue(updated, value);
        project->updateModule(m_instanceId, updated, Project::PositionChangeSource::Unspecified,
                              Project::ParameterChangeSource::Unknown, Project::RuntimeFieldChangeSource::Agent);
        if (m_field == Project::RuntimeField::Enabled)
            RunEngine::instance().setModuleDisabled(m_instanceId, !value.toBool());
        else if (m_field == Project::RuntimeField::Breakpoint)
            RunEngine::instance().setBreakpoint(m_instanceId, value.toBool());
        m_expectedValue = value;
    }

    QPointer<Project> m_project;
    QString m_instanceId;
    Project::RuntimeField m_field;
    QVariant m_expectedValue;
    QVariant m_before;
    QVariant m_after;
    quint64 m_externalGeneration = 0;
    quint64 m_instanceGeneration = 0;
    std::function<void()> m_expired;
    bool m_invalid = false;
};

class AgentSetModuleParamsCmd : public QUndoCommand {
public:
    AgentSetModuleParamsCmd(Project* project, const QString& instanceId, const QJsonObject& before,
                            const QJsonObject& after, std::function<void()> expired, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_project(project), m_instanceId(instanceId), m_before(before), m_after(after),
          m_expectedInstanceGeneration(project->parameterInstanceGeneration(instanceId)),
          m_expired(std::move(expired)) {
        const QSet<QString> keys = changedKeys(before, after);
        for (const QString& key : keys)
            m_expectedGuiGenerations.insert(key, project->parameterGenerations(instanceId, key).gui);
        setText(QStringLiteral("Set module parameters %1").arg(instanceId));
    }

    void undo() override {
        apply(false);
    }
    void redo() override {
        apply(true);
    }

private:
    static QSet<QString> changedKeys(const QJsonObject& before, const QJsonObject& after) {
        QSet<QString> keys;
        for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
            if (!after.contains(it.key()) || after.value(it.key()) != it.value())
                keys.insert(it.key());
        }
        for (auto it = after.constBegin(); it != after.constEnd(); ++it) {
            if (!before.contains(it.key()))
                keys.insert(it.key());
        }
        return keys;
    }

    void expire() {
        if (m_invalid)
            return;
        m_invalid = true;
        if (m_expired)
            m_expired();
    }

    void apply(bool redo) {
        if (m_invalid) {
            if (m_expired)
                m_expired();
            return;
        }
        Project* project = m_project.data();
        if (!project || ProjectManager::instance().currentProject() != project ||
            project->parameterInstanceGeneration(m_instanceId) != m_expectedInstanceGeneration) {
            expire();
            return;
        }
        const std::optional<ModuleInstance> current = project->moduleById(m_instanceId);
        if (!current) {
            expire();
            return;
        }

        const QJsonObject& expected = redo ? m_before : m_after;
        const QJsonObject& target = redo ? m_after : m_before;
        QJsonObject candidate = current->params;
        for (const QString& key : m_expectedGuiGenerations.keys()) {
            const Project::ParameterGenerations generations = project->parameterGenerations(m_instanceId, key);
            const QJsonObject& expectedState = m_expectedParams.isEmpty() ? expected : m_expectedParams;
            const bool expectedPresent = expectedState.contains(key);
            if (generations.gui != m_expectedGuiGenerations.value(key) ||
                current->params.contains(key) != expectedPresent ||
                (expectedPresent && current->params.value(key) != expectedState.value(key))) {
                expire();
                return;
            }
            if (target.contains(key))
                candidate.insert(key, target.value(key));
            else
                candidate.remove(key);
        }

        PluginManager::ModulePtr module = PluginManager::instance().createManagedModule(current->moduleId);
        if (!module) {
            expire();
            return;
        }
        QJsonObject effective = module->defaultParams();
        for (auto it = candidate.constBegin(); it != candidate.constEnd(); ++it)
            effective.insert(it.key(), it.value());
        QString validationError;
        if (!module->validateParams(effective, validationError)) {
            expire();
            return;
        }

        ModuleInstance updated = *current;
        updated.params = candidate;
        project->updateModule(m_instanceId, updated, Project::PositionChangeSource::Unspecified,
                              Project::ParameterChangeSource::Agent);
        m_expectedParams = candidate;
    }

    QPointer<Project> m_project;
    QString m_instanceId;
    QJsonObject m_before;
    QJsonObject m_after;
    QJsonObject m_expectedParams;
    QHash<QString, quint64> m_expectedGuiGenerations;
    quint64 m_expectedInstanceGeneration = 0;
    std::function<void()> m_expired;
    bool m_invalid = false;
};

class AgentConnectCmd : public QUndoCommand {
public:
    AgentConnectCmd(const QString& fromId, const QString& fromPort, const QString& toId, const QString& toPort,
                    const QString& edgeType, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_fromId(fromId), m_toId(toId), m_fromPort(fromPort), m_toPort(toPort),
          m_edgeType(edgeType) {
        setText(QString("Connect %1 → %2").arg(fromId, toId));
    }

    void undo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        proj->removeConnectionWithPorts(m_fromId, m_fromPort, m_toId, m_toPort);
    }
    void redo() override {
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        ModuleConnection conn;
        conn.fromModuleId = m_fromId;
        conn.toModuleId = m_toId;
        conn.fromPort = m_fromPort;
        conn.toPort = m_toPort;
        conn.edgeType = m_edgeType;
        proj->addConnection(conn);
    }

private:
    QString m_fromId, m_toId, m_fromPort, m_toPort, m_edgeType;
};

class AgentDisconnectCmd : public QUndoCommand {
public:
    AgentDisconnectCmd(const ModuleConnection& connection, int originalIndex, QUndoCommand* parent = nullptr)
        : QUndoCommand(parent), m_connection(connection), m_originalIndex(originalIndex) {
        setText(QString("Disconnect %1 → %2").arg(connection.fromModuleId, connection.toModuleId));
    }

    void undo() override {
        // undo of disconnect = reconnect
        Project* proj = ProjectManager::instance().currentProject();
        if (!proj)
            return;
        proj->addConnection(m_connection, m_originalIndex);
    }
    void redo() override {
        // redo of disconnect = disconnect again
        Project* proj = ProjectManager::instance().currentProject();
        if (proj)
            proj->removeConnectionWithPorts(m_connection.fromModuleId, m_connection.fromPort, m_connection.toModuleId,
                                            m_connection.toPort);
    }

private:
    ModuleConnection m_connection;
    int m_originalIndex;
};

// --- AgentActor ---

AgentActor::AgentActor(QObject* parent)
    : QObject(parent), m_undoStack(new QUndoStack(this)), m_runWaiter(std::make_unique<AgentRunWaiter>()) {
    ensureAgentResources();
    buildHandlers();
    // 阶7 Agent 评审修复（P2-7）：schema 成员缓存在插件加载/卸载时失效
    connect(&PluginManager::instance(), &PluginManager::pluginLoaded, this,
            [this](const QString&) { m_schemaCache.clear(); });
    connect(&PluginManager::instance(), &PluginManager::pluginUnloaded, this,
            [this](const QString&) { m_schemaCache.clear(); });
    connect(&PluginManager::instance(), &PluginManager::scanCompleted, this, [this]() { m_schemaCache.clear(); });
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, m_undoStack, &QUndoStack::clear);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, m_undoStack, &QUndoStack::clear);
    connect(&ProjectManager::instance(), &ProjectManager::projectCreated, m_undoStack, &QUndoStack::clear);
    const auto clearUndoBarrier = [this]() { m_latestUndoBarrierIndex = -1; };
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this, clearUndoBarrier);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this, clearUndoBarrier);
    connect(&ProjectManager::instance(), &ProjectManager::projectCreated, this, clearUndoBarrier);
    const auto markEngineDirty = [this]() { m_engineProjectDirty = true; };
    connect(&ProjectManager::instance(), &ProjectManager::projectClosed, this, markEngineDirty);
    connect(&ProjectManager::instance(), &ProjectManager::projectOpened, this, markEngineDirty);
    connect(&ProjectManager::instance(), &ProjectManager::projectCreated, this, markEngineDirty);
}

AgentActor::~AgentActor() = default;

void AgentActor::establishUndoBarrier() {
    if (m_undoStack)
        m_latestUndoBarrierIndex = m_undoStack->index();
}

bool AgentActor::undoBlockedByBarrier() const {
    return m_undoStack && m_latestUndoBarrierIndex >= 0 && m_undoStack->index() <= m_latestUndoBarrierIndex;
}

QJsonObject AgentActor::userPermissionPreflight(const QString& toolName, const QJsonObject& params) const {
    QString permission;
    bool communicationConfiguration = false;
    if (toolName == QLatin1String("create_project"))
        permission = QStringLiteral("NewSolution");
    else if (toolName == QLatin1String("open_project"))
        permission = QStringLiteral("Open");
    else if (toolName == QLatin1String("save_project"))
        permission = QStringLiteral("Save");
    else if (toolName == QLatin1String("add_module") || toolName == QLatin1String("remove_module") ||
             toolName == QLatin1String("move_module") || toolName == QLatin1String("set_module_position") ||
             toolName == QLatin1String("rename_module") || toolName == QLatin1String("set_module_enabled") ||
             toolName == QLatin1String("set_breakpoint") || toolName == QLatin1String("set_module_note") ||
             toolName == QLatin1String("set_param") || toolName == QLatin1String("set_module_params") ||
             toolName == QLatin1String("reset_module_params") || toolName == QLatin1String("connect_modules") ||
             toolName == QLatin1String("disconnect_modules") || toolName == QLatin1String("tune_node_and_verify") ||
             toolName == QLatin1String("remove_data_source_reference") ||
             toolName == QLatin1String("import_data_source"))
        permission = QStringLiteral("Edit");
    else if (toolName == QLatin1String("list_global_variables") || toolName == QLatin1String("get_global_variable") ||
             toolName == QLatin1String("create_global_variable") || toolName == QLatin1String("set_global_variable") ||
             toolName == QLatin1String("remove_global_variable") ||
             toolName == QLatin1String("open_global_variables_dialog"))
        permission = QStringLiteral("GlobalVar");
    else if (toolName == QLatin1String("list_cameras") || toolName == QLatin1String("get_camera_discovery_targets") ||
             toolName == QLatin1String("discover_cameras") ||
             toolName == QLatin1String("get_camera_discovery_operation") ||
             toolName == QLatin1String("get_camera_details") || toolName == QLatin1String("get_camera_operation") ||
             toolName == QLatin1String("connect_camera") || toolName == QLatin1String("disconnect_camera"))
        permission = QStringLiteral("Camera");
    else if (toolName == QLatin1String("set_camera_parameter"))
        permission = QStringLiteral("CameraSet");
    else if (toolName == QLatin1String("load_module_plugin") || toolName == QLatin1String("unload_module_plugin"))
        permission = QStringLiteral("SystemConfig");
    else if (toolName == QLatin1String("open_system_settings"))
        permission = QStringLiteral("SystemParam");
    else if (toolName == QLatin1String("read_documentation") || toolName == QLatin1String("list_knowledge_documents") ||
             toolName == QLatin1String("preview_knowledge_document"))
        permission = QStringLiteral("OpenFile");
    else if (toolName == QLatin1String("add_knowledge_document") ||
             toolName == QLatin1String("remove_knowledge_document"))
        permission = QStringLiteral("SaveFile");
    else if (toolName == QLatin1String("get_annotation_session") ||
             toolName == QLatin1String("open_active_annotation_session"))
        permission = QStringLiteral("OpenFile");
    else if (toolName == QLatin1String("save_active_annotation_session") || toolName == QLatin1String("sam_annotate") ||
             toolName == QLatin1String("export_annotation_session") ||
             toolName == QLatin1String("transform_annotation_session"))
        permission = QStringLiteral("SaveFile");
    else if (toolName == QLatin1String("sam_predict") || toolName == QLatin1String("list_active_annotation_objects") ||
             toolName == QLatin1String("list_active_annotation_categories") ||
             toolName == QLatin1String("get_active_annotation_prompt_status"))
        permission = QStringLiteral("OpenFile");
    else if (toolName == QLatin1String("edit_active_annotation_masks") ||
             toolName == QLatin1String("edit_active_annotation_categories"))
        permission = QStringLiteral("Edit");
    else if (toolName == QLatin1String("run_flow"))
        permission = params.value(QStringLiteral("mode")).toString(QStringLiteral("once")) == QLatin1String("cycle")
                         ? QStringLiteral("RunCycle")
                         : QStringLiteral("RunOnce");
    else if (toolName == QLatin1String("step_once") || toolName == QLatin1String("reset_step"))
        permission = QStringLiteral("RunOnce");
    else if (toolName == QLatin1String("pause_flow") || toolName == QLatin1String("resume_flow") ||
             toolName == QLatin1String("stop_flow"))
        permission = QStringLiteral("Stop");

    if (toolName == QLatin1String("add_module")) {
        const PluginInfo info = PluginManager::instance().pluginInfo(params.value(QStringLiteral("plugin")).toString());
        communicationConfiguration = info.category.compare(QStringLiteral("communication"), Qt::CaseInsensitive) == 0;
    } else if (toolName == QLatin1String("set_param") || toolName == QLatin1String("set_module_params") ||
               toolName == QLatin1String("reset_module_params")) {
        const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
        Project* project = ProjectManager::instance().currentProject();
        const std::optional<ModuleInstance> instance = project ? project->moduleById(instanceId) : std::nullopt;
        if (instance) {
            const PluginInfo info = PluginManager::instance().pluginInfo(instance->moduleId);
            communicationConfiguration =
                info.category.compare(QStringLiteral("communication"), Qt::CaseInsensitive) == 0;
        }
    }

    UserManager& users = UserManager::instance();
    const auto permissionError = [](const QString& requiredPermission) {
        QJsonObject denied =
            makeToolError(QStringLiteral("User login or permission is required: %1").arg(requiredPermission),
                          QStringLiteral("PERMISSION_DENIED"), false,
                          QJsonObject{{QStringLiteral("requiredPermission"), requiredPermission}});
        denied.insert(QStringLiteral("status"), QStringLiteral("denied"));
        return denied;
    };
    const auto hasPermission = [&users](const QString& requiredPermission) {
        const bool guestHasExplicitPermission = requiredPermission == QLatin1String("RunOnce") ||
                                                requiredPermission == QLatin1String("RunCycle") ||
                                                requiredPermission == QLatin1String("Stop");
        const bool guestDenied = users.currentUser().type == UserType::Guest && !guestHasExplicitPermission;
        return !guestDenied && users.isLoggedIn() && users.hasPermission(requiredPermission);
    };

    if (permission.isEmpty())
        return {};
    if (!hasPermission(permission))
        return permissionError(permission);
    if (communicationConfiguration && !hasPermission(QStringLiteral("CommunicationSet")))
        return permissionError(QStringLiteral("CommunicationSet"));
    return {};
}

bool AgentActor::undoLastAction() {
    if (!m_undoStack || !m_undoStack->canUndo() || undoBlockedByBarrier() || RunEngine::instance().isBusy())
        return false;
    m_positionUndoExpired = false;
    m_moduleOrderUndoExpired = false;
    m_parameterUndoExpired = false;
    m_runtimeFieldUndoExpired = false;
    m_undoStack->undo();
    return !m_positionUndoExpired && !m_moduleOrderUndoExpired && !m_parameterUndoExpired && !m_runtimeFieldUndoExpired;
}


void AgentActor::buildHandlers() {
    m_handlers.clear();
    for (const ToolDefinition& definition : ToolSchema::instance().allTools()) {
        ToolHandler handler;
        const QString name = definition.name;
        handler.fn = [this, name](const QJsonObject& params) { return executeTool(name, params); };
        handler.mutatesProject = !definition.readOnly;
        handler.undoable = name == QLatin1String("add_module") || name == QLatin1String("remove_module") ||
                           name == QLatin1String("set_param") || name == QLatin1String("connect_modules") ||
                           name == QLatin1String("disconnect_modules");
        m_handlers.insert(name, std::move(handler));
    }
}

QStringList AgentActor::supportedTools() const {
    QStringList names;
    for (const ToolDefinition& definition : ToolSchema::instance().allTools())
        names.append(definition.name);
    names.removeDuplicates();
    names.sort();
    return names;
}

bool AgentActor::toolUndoable(const QString& toolName) const {
    const auto it = m_handlers.constFind(toolName);
    return it != m_handlers.constEnd() && it->undoable;
}

void AgentActor::setViewportImageProvider(ViewportImageProvider provider) {
    m_viewportImageProvider = std::move(provider);
}

void AgentActor::abortRunningTool() {
    if (m_runWaiter && m_runWaiter->isActive())
        m_runWaiter->abort();
    FlowExecutionService& service = FlowExecutionService::instance();
    if (service.isActive())
        service.stop();
    else if (RunEngine::instance().isBusy())
        RunEngine::instance().stop();
}

bool AgentActor::resultSucceeded(const QJsonObject& result) {
    if (result.value(QStringLiteral("success")).isBool())
        return result.value(QStringLiteral("success")).toBool();
    if (result.contains(QStringLiteral("error")))
        return false;
    const QString status = result.value(QStringLiteral("status")).toString();
    return status != QLatin1String("failed") && status != QLatin1String("uncertain") &&
           status != QLatin1String("unavailable") && status != QLatin1String("rejected");
}

QString AgentActor::resultFailureCode(const QJsonObject& result) {
    if (resultSucceeded(result))
        return {};
    const QString errorCode = result.value(QStringLiteral("errorCode")).toString();
    if (!errorCode.isEmpty())
        return errorCode;
    const QString code = result.value(QStringLiteral("code")).toString();
    return code.isEmpty() ? QStringLiteral("tool_error") : code;
}

QJsonObject AgentActor::executeTool(const QString& toolName, const QJsonObject& params) {
    return executeToolWithCameraTarget(toolName, params, {}, {}, {});
}

QJsonObject AgentActor::executeToolWithCameraTarget(const QString& toolName, const QJsonObject& params,
                                                    const QJsonObject& cameraTargetSnapshot,
                                                    const QJsonObject& folderDeleteTargetAuthorization,
                                                    const QJsonObject& variableSnapshot,
                                                    const QJsonObject& dataSourceImportSnapshot, const QString& callId,
                                                    const QJsonObject& annotationExportSnapshot,
                                                    const QJsonObject& annotationTransformSnapshot) {
    ToolSchema& schema = ToolSchema::instance();
    if (!schema.hasTool(toolName)) {
        QString err = QString("Unknown tool: %1").arg(toolName);
        emit toolError(toolName, err);
        return makeToolError(err, QStringLiteral("UNKNOWN_TOOL"));
    }

    QString invalid = validationError(toolName, params);
    if (!invalid.isEmpty()) {
        emit toolError(toolName, invalid);
        return makeToolValidationError(toolName, invalid);
    }

    const QJsonObject userPermissionError = userPermissionPreflight(toolName, params);
    if (!userPermissionError.isEmpty()) {
        emit toolError(toolName, userPermissionError.value(QStringLiteral("error")).toString());
        return userPermissionError;
    }

    if (toolName == QLatin1String("import_data_source") && QThread::currentThread() != thread()) {
        const QString error = QStringLiteral("Data source import must run on the Agent owner thread");
        emit toolError(toolName, error);
        return makeToolError(error, QStringLiteral("THREAD_AFFINITY"));
    }

    const QJsonObject selectorError = flowSelectorPreflight(toolName, params);
    if (!selectorError.isEmpty()) {
        emit toolError(toolName, selectorError.value(QStringLiteral("error")).toString());
        return selectorError;
    }

    const bool mutatesFlow =
        toolName == "create_project" || toolName == "open_project" || toolName == "add_module" ||
        toolName == "remove_module" || toolName == "move_module" || toolName == "set_module_position" ||
        toolName == "rename_module" || toolName == "set_module_enabled" || toolName == "set_breakpoint" ||
        toolName == "set_module_note" || toolName == "set_param" || toolName == "set_module_params" ||
        toolName == "reset_module_params" || toolName == "connect_modules" || toolName == "disconnect_modules" ||
        toolName == "remove_data_source_reference" || toolName == "import_data_source";
    if (mutatesFlow && RunEngine::instance().isBusy()) {
        const QString error = toolName == QLatin1String("import_data_source")
                                  ? QStringLiteral("Project is busy with flow execution or maintenance")
                                  : QStringLiteral("Flow is running; stop it before changing the project");
        emit toolError(toolName, error);
        return makeToolError(error, QStringLiteral("ENGINE_BUSY"), toolName != QLatin1String("import_data_source"));
    }

    QJsonObject result;
    if (toolName == "create_project")
        result = createProject(params);
    else if (toolName == "add_module")
        result = addModule(params);
    else if (toolName == "remove_module")
        result = removeModule(params);
    else if (toolName == "move_module")
        result = moveModule(params);
    else if (toolName == "set_module_position")
        result = setModulePosition(params);
    else if (toolName == "rename_module")
        result = renameModule(params);
    else if (toolName == "set_module_enabled")
        result = setModuleEnabled(params);
    else if (toolName == "set_breakpoint")
        result = setBreakpoint(params);
    else if (toolName == "set_module_note")
        result = setModuleNote(params);
    else if (toolName == "set_param")
        result = setParam(params);
    else if (toolName == "set_module_params")
        result = setModuleParams(params);
    else if (toolName == "reset_module_params")
        result = resetModuleParams(params);
    else if (toolName == "connect_modules")
        result = connectModules(params);
    else if (toolName == "disconnect_modules")
        result = disconnectModules(params);
    else if (toolName == "run_flow")
        result = runFlow(params, folderDeleteTargetAuthorization);
    else if (toolName == "step_once")
        result = stepOnce(params, folderDeleteTargetAuthorization);
    else if (toolName == "reset_step")
        result = resetStep(params);
    else if (toolName == "validate_flow")
        result = validateFlow(params);
    else if (toolName == "stop_flow")
        result = stopFlow(params);
    else if (toolName == "get_flow_state")
        result = getFlowState(params);
    else if (toolName == "get_available_plugins")
        result = getAvailablePlugins(params);
    else if (toolName == "list_cameras")
        result = listCameras(params);
    else if (toolName == "get_camera_discovery_targets")
        result = getCameraDiscoveryTargets(params);
    else if (toolName == "discover_cameras")
        result = discoverCameras(params);
    else if (toolName == "get_camera_discovery_operation")
        result = getCameraDiscoveryOperation(params);
    else if (toolName == "get_camera_details")
        result = getCameraDetails(params);
    else if (toolName == "get_camera_operation")
        result = getCameraOperation(params);
    else if (toolName == "set_camera_parameter")
        result = setCameraParameter(params);
    else if (toolName == "list_communications")
        result = listCommunications(params);
    else if (toolName == "load_module_plugin")
        result = loadModulePlugin(params);
    else if (toolName == "unload_module_plugin")
        result = unloadModulePlugin(params);
    else if (toolName == "connect_camera")
        result = connectCamera(params, cameraTargetSnapshot);
    else if (toolName == "disconnect_camera")
        result = disconnectCamera(params, cameraTargetSnapshot);
    else if (toolName == "save_project")
        result = saveProject(params);
    else if (toolName == "get_module_params_schema")
        result = getModuleParamsSchema(params);
    else if (toolName == "get_run_results")
        result = getRunResults(params);
    else if (toolName == "get_module_output")
        result = getModuleOutput(params);
    else if (toolName == "wait_for_run")
        result = waitForRun(params);
    else if (toolName == "open_project")
        result = openProject(params);
    else if (toolName == "pause_flow")
        result = pauseFlow(params);
    else if (toolName == "resume_flow") {
        if (m_confirmedResumeSnapshotValid) {
            const QString runId = m_confirmedResumeRunId;
            const QString sessionId = m_confirmedResumeSessionId;
            const int state = m_confirmedResumeState;
            const quint64 generation = m_confirmedResumeLifecycleGeneration;
            clearConfirmedResumeSnapshot();
            result = resumeFlowForSnapshot(runId, sessionId, state, generation, folderDeleteTargetAuthorization);
        } else {
            result = resumeFlow(params, folderDeleteTargetAuthorization);
        }
    } else if (toolName == "read_documentation")
        result = readDocumentation(params);
    else if (toolName == "list_knowledge_documents") {
        const QJsonArray documents = listKnowledgeDocuments();
        result = QJsonObject{{QStringLiteral("documents"), documents}, {QStringLiteral("total"), documents.size()}};
    } else if (toolName == "preview_knowledge_document")
        result = previewKnowledgeDocument(params.value(QStringLiteral("documentId")).toString());
    else if (toolName == "add_knowledge_document")
        result = addKnowledgeDocument(params.value(QStringLiteral("sourcePath")).toString());
    else if (toolName == "remove_knowledge_document")
        result = removeKnowledgeDocument(params.value(QStringLiteral("documentId")).toString());
    else if (toolName == QLatin1String("list_global_variables")) {
        const QString typeName = params.value(QStringLiteral("type")).toString();
        if (!typeName.isEmpty() && !isAgentVariableType(VarModel::stringToDataType(typeName))) {
            result = makeToolError(QStringLiteral("Unsupported global variable type filter"),
                                   QStringLiteral("UNSUPPORTED_TYPE"));
        } else {
            const QList<GlobalVariableSnapshot> snapshots =
                GlobalVarManager::instance().userVariableSnapshots(typeName);
            QJsonArray variables;
            for (const GlobalVariableSnapshot& snapshot : snapshots)
                variables.append(variableDto(snapshot, false));
            result = QJsonObject{{QStringLiteral("variables"), variables}, {QStringLiteral("total"), variables.size()}};
        }
    } else if (toolName == QLatin1String("list_data_sources")) {
        const QString typeFilter = params.value(QStringLiteral("type")).toString();
        Project* project = ProjectManager::instance().currentProject();
        if (!project) {
            result = makeToolError(QStringLiteral("No project is open"), QStringLiteral("NO_PROJECT"));
        } else {
            QJsonArray sources;
            for (const DataSource& source : project->dataSources()) {
                if (!typeFilter.isEmpty() && source.type != typeFilter)
                    continue;
                sources.append(QJsonObject{{QStringLiteral("id"), source.id},
                                           {QStringLiteral("name"), source.name},
                                           {QStringLiteral("type"), source.type},
                                           {QStringLiteral("filePath"), source.filePath},
                                           {QStringLiteral("fileExists"), QFileInfo::exists(source.filePath)}});
            }
            result = QJsonObject{{QStringLiteral("dataSources"), sources}, {QStringLiteral("total"), sources.size()}};
        }
    } else if (toolName == QLatin1String("remove_data_source_reference")) {
        Project* project = ProjectManager::instance().currentProject();
        if (QThread::currentThread() != thread() || (project && QThread::currentThread() != project->thread())) {
            result = makeToolError(QStringLiteral("Project mutation must run on its owner thread"),
                                   QStringLiteral("THREAD_AFFINITY"));
        } else {
            const QString dataSourceId = params.value(QStringLiteral("dataSourceId")).toString();
            QString rejectionCode;
            const auto mutation = RunEngine::instance().withProjectMutationLease([&]() {
                if (ProjectManager::instance().currentProject() != project) {
                    rejectionCode = QStringLiteral("PROJECT_CHANGED");
                    return false;
                }
                if (!project) {
                    rejectionCode = QStringLiteral("NO_PROJECT");
                    return false;
                }
                int matches = 0;
                for (const DataSource& source : project->dataSources())
                    matches += source.id == dataSourceId ? 1 : 0;
                if (matches != 1) {
                    rejectionCode = matches > 1 ? QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS")
                                                : QStringLiteral("DATA_SOURCE_NOT_FOUND");
                    return false;
                }
                project->removeDataSource(dataSourceId);
                return true;
            });
            if (mutation == RunEngine::ProjectMutationResult::Busy) {
                result = makeToolError(QStringLiteral("Project is busy with flow execution or maintenance"),
                                       QStringLiteral("ENGINE_BUSY"), true);
            } else if (mutation == RunEngine::ProjectMutationResult::Applied) {
                result = QJsonObject{{QStringLiteral("status"), QStringLiteral("removed")},
                                     {QStringLiteral("dataSourceId"), dataSourceId},
                                     {QStringLiteral("filePreserved"), true}};
            } else if (!project) {
                result = makeToolError(QStringLiteral("No project is open"), QStringLiteral("NO_PROJECT"));
            } else {
                if (rejectionCode == QStringLiteral("PROJECT_CHANGED"))
                    result = makeToolError(QStringLiteral("Project changed before the removal could be applied"),
                                           rejectionCode);
                else if (rejectionCode == QStringLiteral("DATA_SOURCE_ID_AMBIGUOUS"))
                    result = makeToolError(QStringLiteral("Data source ID is ambiguous"), rejectionCode);
                else
                    result = makeToolError(QStringLiteral("Data source was not found"),
                                           rejectionCode.isEmpty() ? QStringLiteral("DATA_SOURCE_NOT_FOUND")
                                                                   : rejectionCode);
            }
        }
    } else if (toolName == QLatin1String("import_data_source")) {
        result = importDataSource(params, dataSourceImportSnapshot, callId);
    } else if (toolName == QLatin1String("get_data_source_view_state") ||
               toolName == QLatin1String("display_data_source") ||
               toolName == QLatin1String("display_data_source_in_dedicated_viewport") ||
               toolName == QLatin1String("set_data_source_visibility") ||
               toolName == QLatin1String("set_workspace_panel_visibility") ||
               toolName == QLatin1String("set_log_level_filter") || toolName == QLatin1String("select_log_panel_tab") ||
               toolName == QLatin1String("scroll_agent_chat_to_latest") ||
               toolName == QLatin1String("clear_runtime_logs") || toolName == QLatin1String("list_recent_projects") ||
               toolName == QLatin1String("set_theme") || toolName == QLatin1String("open_system_settings") ||
               toolName == QLatin1String("open_global_variables_dialog") ||
               toolName == QLatin1String("save_viewport_snapshot_for_user") ||
               toolName == QLatin1String("open_sam_environment_setup") ||
               toolName == QLatin1String("open_sam_annotator_dialog") ||
               toolName == QLatin1String("open_sam_image_picker_for_user") ||
               toolName == QLatin1String("open_user_login_dialog") ||
               toolName == QLatin1String("open_agent_settings_dialog") ||
               toolName == QLatin1String("open_about_dialog") ||
               toolName == QLatin1String("open_knowledge_base_dialog") ||
               toolName == QLatin1String("open_user_knowledge_folder_for_user") ||
               toolName == QLatin1String("set_inspector_pinned") ||
               toolName == QLatin1String("set_inspector_collapsed") ||
               toolName == QLatin1String("set_inspector_visibility") ||
               toolName == QLatin1String("set_primary_viewport_zoom") ||
               toolName == QLatin1String("set_primary_viewport_point_size") ||
               toolName == QLatin1String("set_primary_viewport_display_mode") ||
               toolName == QLatin1String("set_point_cloud_render_mode") ||
               toolName == QLatin1String("close_viewport") ||
               toolName == QLatin1String("copy_data_source_path_to_clipboard") ||
               toolName == QLatin1String("open_data_source_folder_for_user") ||
               toolName == QLatin1String("select_flow_module") || toolName == QLatin1String("list_viewports") ||
               toolName == QLatin1String("begin_quick_measurement") ||
               toolName == QLatin1String("get_quick_measurement_status") ||
               toolName == QLatin1String("cancel_quick_measurement") ||
               toolName == QLatin1String("list_active_annotation_objects") ||
               toolName == QLatin1String("edit_active_annotation_masks") ||
               toolName == QLatin1String("list_active_annotation_categories") ||
               toolName == QLatin1String("edit_active_annotation_categories") ||
               toolName == QLatin1String("get_active_annotation_prompt_status") ||
               toolName == QLatin1String("cancel_active_annotation_prompt") ||
               toolName == QLatin1String("save_active_annotation_session") ||
               toolName == QLatin1String("open_active_annotation_session")) {
        result = AgentController::instance().executeDataSourceViewAction(toolName, params);
    } else if (toolName == QLatin1String("get_global_variable")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        const GlobalVariableSnapshot snapshot = GlobalVarManager::instance().userVariableSnapshot(name);
        if (!snapshot.exists) {
            result =
                makeToolError(QStringLiteral("Global variable was not found"), QStringLiteral("VARIABLE_NOT_FOUND"));
        } else if (!isAgentVariableType(snapshot.type)) {
            result = makeToolError(QStringLiteral("Global variable type is not supported by Agent tools"),
                                   QStringLiteral("UNSUPPORTED_TYPE"));
        } else {
            result = variableDto(snapshot, true);
        }
    } else if (toolName == QLatin1String("create_global_variable")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        const QString typeName = params.value(QStringLiteral("type")).toString();
        const VarDataType type = VarModel::stringToDataType(typeName);
        QVariant value;
        QString errorCode;
        if (name.isEmpty()) {
            result = makeToolError(QStringLiteral("Variable name cannot be empty"), QStringLiteral("INVALID_ARGUMENT"));
        } else if (!isAgentVariableType(type)) {
            result = makeToolError(QStringLiteral("Global variable type is not supported by Agent tools"),
                                   QStringLiteral("UNSUPPORTED_TYPE"));
        } else if (!jsonValueToVariableValue(type, params.value(QStringLiteral("value")), value, errorCode)) {
            result = makeToolError(QStringLiteral("Value does not match the declared global variable type"), errorCode);
        } else {
            GlobalVariableSnapshot created;
            if (!GlobalVarManager::instance().tryCreateUserVariable(name, type, value, variableSnapshot, &created,
                                                                    &errorCode)) {
                result = makeToolError(QStringLiteral("Global variable could not be created"), errorCode);
            } else {
                result = variableDto(created, true);
                result.insert(QStringLiteral("status"), QStringLiteral("created"));
            }
        }
    } else if (toolName == QLatin1String("set_global_variable")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        const GlobalVariableSnapshot current = GlobalVarManager::instance().userVariableSnapshot(name);
        const QString typeName = variableSnapshot.isEmpty() ? VarModel::dataTypeToString(current.type)
                                                            : variableSnapshot.value(QStringLiteral("type")).toString();
        const VarDataType type = VarModel::stringToDataType(typeName);
        QVariant value;
        QString errorCode;
        if (!current.exists) {
            result =
                makeToolError(QStringLiteral("Global variable was not found"), QStringLiteral("VARIABLE_NOT_FOUND"));
        } else if (!isAgentVariableType(type)) {
            result = makeToolError(QStringLiteral("Global variable type is not supported by Agent tools"),
                                   QStringLiteral("UNSUPPORTED_TYPE"));
        } else if (!jsonValueToVariableValue(type, params.value(QStringLiteral("value")), value, errorCode)) {
            result = makeToolError(QStringLiteral("Value does not match the global variable type"), errorCode);
        } else {
            GlobalVariableSnapshot updated;
            if (!GlobalVarManager::instance().trySetUserVariable(name, value, variableSnapshot, &updated, &errorCode)) {
                result = makeToolError(QStringLiteral("Global variable could not be updated"), errorCode);
            } else {
                result = variableDto(updated, true);
                result.insert(QStringLiteral("status"), QStringLiteral("updated"));
            }
        }
    } else if (toolName == QLatin1String("remove_global_variable")) {
        const QString name = params.value(QStringLiteral("name")).toString().trimmed();
        QString errorCode;
        const GlobalVariableSnapshot current = GlobalVarManager::instance().userVariableSnapshot(name);
        if (current.exists && !isAgentVariableType(current.type)) {
            result = makeToolError(QStringLiteral("Global variable type is not supported by Agent tools"),
                                   QStringLiteral("UNSUPPORTED_TYPE"));
        } else if (!GlobalVarManager::instance().tryRemoveUserVariable(name, variableSnapshot, &errorCode)) {
            result = makeToolError(QStringLiteral("Global variable could not be removed"), errorCode);
        } else {
            result = QJsonObject{{QStringLiteral("status"), QStringLiteral("removed")}, {QStringLiteral("name"), name}};
        }
    } else if (toolName == "sam_predict")
        result = samPredict(params);
    else if (toolName == "sam_annotate")
        result = samAnnotate(params);
    else if (toolName == "get_annotation_session")
        result = getAnnotationSession(params);
    else if (toolName == "export_annotation_session")
        result = AnnotationSessionExport::execute(params, annotationExportSnapshot);
    else if (toolName == "transform_annotation_session")
        result = AnnotationSessionTransform::execute(params, annotationTransformSnapshot);
    else if (toolName == QLatin1String("validate_workflow"))
        result = validateWorkflow(params);
    else if (toolName == QLatin1String("get_node_runtime_outputs"))
        result = getNodeRuntimeOutputs(params);
    else if (toolName == QLatin1String("get_execution_logs"))
        result = getExecutionLogs(params);
    else if (toolName == QLatin1String("tune_node_and_verify"))
        result = tuneNodeAndVerify(params);
    else if (toolName == QLatin1String("capture_viewport_image"))
        result = captureViewportImage(params);
    else if (toolName == QLatin1String("export_node_output"))
        result = exportNodeOutput(params);
    else {
        QString err = QString("Tool not yet implemented: %1").arg(toolName);
        emit toolError(toolName, err);
        return makeToolError(err, QStringLiteral("UNKNOWN_TOOL"));
    }

    if (toolName == QLatin1String("create_project") || toolName == QLatin1String("open_project") ||
        toolName == QLatin1String("add_module") || toolName == QLatin1String("remove_module") ||
        toolName == QLatin1String("move_module") || toolName == QLatin1String("set_module_position") ||
        toolName == QLatin1String("rename_module") || toolName == QLatin1String("set_module_enabled") ||
        toolName == QLatin1String("set_breakpoint") || toolName == QLatin1String("set_module_note") ||
        toolName == QLatin1String("set_param") || toolName == QLatin1String("set_module_params") ||
        toolName == QLatin1String("reset_module_params") || toolName == QLatin1String("connect_modules") ||
        toolName == QLatin1String("disconnect_modules"))
        m_engineProjectDirty = true;
    emit toolExecuted(toolName, result);
    return result;
}

QJsonObject AgentActor::importDataSource(const QJsonObject& params, const QJsonObject& confirmedSnapshot,
                                         const QString& callId) {
    if (QThread::currentThread() != thread())
        return makeToolError(QStringLiteral("Data source import must run on the Agent owner thread"),
                             QStringLiteral("THREAD_AFFINITY"));

    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError(QStringLiteral("No project is open"), QStringLiteral("NO_PROJECT"));
    if (QThread::currentThread() != project->thread())
        return makeToolError(QStringLiteral("Project access must run on its owner thread"),
                             QStringLiteral("THREAD_AFFINITY"));

    const QString inputPath = params.value(QStringLiteral("path")).toString();
    if (inputPath.trimmed().isEmpty() || inputPath != inputPath.trimmed() ||
        inputPath.contains(QStringLiteral("://")) || inputPath.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
        return makeToolError(QStringLiteral("path must be a local file path"), QStringLiteral("INVALID_PATH"));
    const QFileInfo inputInfo(inputPath);
    if (!inputInfo.exists())
        return makeToolError(QStringLiteral("Data source file does not exist"), QStringLiteral("FILE_NOT_FOUND"));
    if (!inputInfo.isFile())
        return makeToolError(QStringLiteral("Data source path is not a regular file"), QStringLiteral("INVALID_PATH"));
    const QString canonicalPath = inputInfo.canonicalFilePath();
    if (canonicalPath.isEmpty())
        return makeToolError(QStringLiteral("Data source path cannot be canonicalized"),
                             QStringLiteral("INVALID_PATH"));

    if (callId.isEmpty() || confirmedSnapshot.value(QStringLiteral("callId")).toString() != callId ||
        confirmedSnapshot.isEmpty())
        return makeToolError(QStringLiteral("A per-call confirmation snapshot is required"),
                             QStringLiteral("CONFIRMATION_SNAPSHOT_MISSING"));
    const QJsonObject initialFileSnapshot = dataSourceFileSnapshot(inputPath);
    if (!initialFileSnapshot.value(QStringLiteral("readable")).toBool())
        return makeToolError(QStringLiteral("Data source file is not readable"), QStringLiteral("FILE_UNREADABLE"));
    if (!initialFileSnapshot.value(QStringLiteral("fileIdentityAvailable")).toBool())
        return makeToolError(QStringLiteral("Stable data source file identity is unavailable"),
                             QStringLiteral("FILE_IDENTITY_UNAVAILABLE"));
    if (!sameDataSourceFileSnapshot(confirmedSnapshot, initialFileSnapshot))
        return makeToolError(QStringLiteral("Data source file changed after confirmation"),
                             QStringLiteral("DATA_SOURCE_FILE_CHANGED"));

    const QString extension = QFileInfo(canonicalPath).suffix().toLower();
    const bool imageType = extension == QLatin1String("png") || extension == QLatin1String("jpg") ||
                           extension == QLatin1String("jpeg") || extension == QLatin1String("bmp");
    const bool plyType = extension == QLatin1String("ply");
    const bool tiffType = extension == QLatin1String("tif") || extension == QLatin1String("tiff");
    if (!imageType && !plyType && !tiffType)
        return makeToolError(QStringLiteral("Unsupported data source file format"),
                             QStringLiteral("UNSUPPORTED_FORMAT"));

    QString type = imageType ? QStringLiteral("image") : QStringLiteral("pointcloud");
    QVariantMap metadata;
    metadata.insert(QStringLiteral("fileSize"), initialFileSnapshot.value(QStringLiteral("byteCount")).toVariant());
    if (imageType) {
        QImageReader reader(canonicalPath);
        reader.setAutoTransform(true);
        const QImage decoded = reader.read();
        if (decoded.isNull())
            return makeToolError(QStringLiteral("Image file could not be fully decoded: %1").arg(reader.errorString()),
                                 QStringLiteral("INVALID_IMAGE"));
        metadata.insert(QStringLiteral("width"), decoded.width());
        metadata.insert(QStringLiteral("height"), decoded.height());
    } else {
        PointCloudData cloud;
        QString error;
        const bool loaded =
            plyType ? PlyLoader::load(canonicalPath, cloud, error) : TiffLoader::load(canonicalPath, cloud, error);
        if (!loaded)
            return makeToolError(QStringLiteral("Point cloud file could not be fully decoded: %1").arg(error),
                                 QStringLiteral("INVALID_POINT_CLOUD"));
        metadata.insert(QStringLiteral("pointCount"), static_cast<qint64>(cloud.size()));
    }

    DataSource source;
    source.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    source.name = QFileInfo(canonicalPath).fileName();
    source.filePath = canonicalPath;
    source.type = type;
    source.metadata = metadata;
    source.importTime = QDateTime::currentMSecsSinceEpoch();

    const qint64 approvedProjectIdentity =
        confirmedSnapshot.value(QStringLiteral("projectRuntimeIdentity")).toVariant().toLongLong();
    const qint64 approvedGeneration =
        confirmedSnapshot.value(QStringLiteral("dataSourceGeneration")).toVariant().toLongLong();
    const QString approvedFingerprint = confirmedSnapshot.value(QStringLiteral("projectFingerprint")).toString();
    const QJsonObject approvedProjectBaseline = confirmedSnapshot.value(QStringLiteral("projectBaseline")).toObject();
    const bool approvedProjectDirty = confirmedSnapshot.value(QStringLiteral("projectDirty")).toBool();
    const QString currentFingerprint = QString::fromLatin1(dataSourceImportProjectFingerprint(project->toJson()));
    if (approvedProjectIdentity == 0 || approvedGeneration < 0 || approvedFingerprint.isEmpty() ||
        approvedProjectBaseline.isEmpty() ||
        QString::fromLatin1(dataSourceImportProjectFingerprint(approvedProjectBaseline)) != approvedFingerprint)
        return makeToolError(QStringLiteral("Project was not present when the operation was confirmed"),
                             QStringLiteral("CONFIRMATION_EXPIRED"));
    if (approvedProjectIdentity != static_cast<qint64>(project->runtimeIdentity()) ||
        approvedGeneration != static_cast<qint64>(project->dataSourceCollectionGeneration()) ||
        approvedFingerprint != currentFingerprint || project->isModified() != approvedProjectDirty)
        return makeToolError(QStringLiteral("Project or data source collection changed after confirmation"),
                             QStringLiteral("CONFIRMATION_EXPIRED"));

    QString rejectionCode;
    QString rejectionMessage;
    QString existingId;
    QString existingType;
    const RunEngine::ProjectMutationResult mutation = RunEngine::instance().withProjectMutationLease([&]() {
        if (QThread::currentThread() != project->thread()) {
            rejectionCode = QStringLiteral("THREAD_AFFINITY");
            rejectionMessage = QStringLiteral("Project mutation must run on its owner thread");
            return false;
        }
        if (ProjectManager::instance().currentProject() != project ||
            static_cast<qint64>(project->runtimeIdentity()) != approvedProjectIdentity ||
            static_cast<qint64>(project->dataSourceCollectionGeneration()) != approvedGeneration ||
            QString::fromLatin1(dataSourceImportProjectFingerprint(project->toJson())) != approvedFingerprint ||
            project->isModified() != approvedProjectDirty) {
            rejectionCode = QStringLiteral("CONFIRMATION_EXPIRED");
            rejectionMessage = QStringLiteral("Project or data source collection changed before commit");
            return false;
        }
        const QJsonObject commitFileSnapshot = dataSourceFileSnapshot(inputPath);
        if (!sameDataSourceFileSnapshot(confirmedSnapshot, commitFileSnapshot)) {
            rejectionCode = QStringLiteral("DATA_SOURCE_FILE_CHANGED");
            rejectionMessage = QStringLiteral("Data source file changed before commit");
            return false;
        }

        int pathMatches = 0;
        for (const DataSource& existing : project->dataSources()) {
            if (QFileInfo(existing.filePath).canonicalFilePath() == canonicalPath) {
                ++pathMatches;
                existingId = existing.id;
                existingType = existing.type;
            }
        }
        if (pathMatches > 1) {
            rejectionCode = QStringLiteral("DATA_SOURCE_PATH_AMBIGUOUS");
            rejectionMessage = QStringLiteral("More than one project reference points to this file");
            return false;
        }
        if (pathMatches == 1) {
            rejectionCode = QStringLiteral("ALREADY_IMPORTED");
            return false;
        }

        project->addDataSource(source);
        return true;
    });

    if (mutation == RunEngine::ProjectMutationResult::Busy)
        return makeToolError(QStringLiteral("Project is busy with flow execution or maintenance"),
                             QStringLiteral("ENGINE_BUSY"));
    if (mutation == RunEngine::ProjectMutationResult::Rejected) {
        if (rejectionCode == QLatin1String("ALREADY_IMPORTED"))
            return QJsonObject{{QStringLiteral("status"), QStringLiteral("already_imported")},
                               {QStringLiteral("dataSourceId"), existingId},
                               {QStringLiteral("type"), existingType},
                               {QStringLiteral("filePath"), canonicalPath}};
        return makeToolError(rejectionMessage.isEmpty() ? QStringLiteral("Data source could not be registered")
                                                        : rejectionMessage,
                             rejectionCode.isEmpty() ? QStringLiteral("DATA_SOURCE_IMPORT_FAILED") : rejectionCode);
    }
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("imported")},
                       {QStringLiteral("dataSourceId"), source.id},
                       {QStringLiteral("type"), source.type},
                       {QStringLiteral("filePath"), source.filePath}};
}

QJsonObject AgentActor::flowSelectorPreflight(const QString& toolName, const QJsonObject& params) const {
    if (toolName == QLatin1String("export_annotation_session"))
        return AnnotationSessionExport::preflight(params);
    if (toolName == QLatin1String("transform_annotation_session"))
        return AnnotationSessionTransform::preflight(params);
    if (toolName == QLatin1String("resume_flow") && RunEngine::instance().isPaused()) {
        const RunResumeSnapshot snapshot = RunEngine::instance().resumeSnapshot();
        Project* currentProject = ProjectManager::instance().currentProject();
        if (snapshot.runId.isEmpty() || snapshot.project != currentProject ||
            snapshot.projectFingerprint != projectFingerprint(currentProject))
            return makeToolError(QStringLiteral("Paused flow configuration differs from the current project"),
                                 QStringLiteral("FLOW_CONFIGURATION_CHANGED"));
    }

    if (toolName == QLatin1String("discover_cameras")) {
        const QString invalid = validationError(toolName, params);
        if (!invalid.isEmpty())
            return makeToolValidationError(toolName, invalid);
        const QJsonObject targets = CameraManager::instance().cameraDiscoveryTargets();
        if (targets.value(QStringLiteral("status")).toString() != QLatin1String("success"))
            return makeToolError(QStringLiteral("Camera discovery targets could not be read"),
                                 targets.value(QStringLiteral("errorCode")).toString());
        if (targets.value(QStringLiteral("registryGeneration")).toString() !=
            params.value(QStringLiteral("registryGeneration")).toString())
            return makeToolError(QStringLiteral("Camera plugin registry changed; query targets again"),
                                 QStringLiteral("CAMERA_TARGET_CHANGED"));
        QStringList currentAsync;
        for (const QJsonValue& value : targets.value(QStringLiteral("asyncPluginIds")).toArray())
            currentAsync.append(value.toString());
        QStringList currentUnsupported;
        for (const QJsonValue& value : targets.value(QStringLiteral("unsupportedPluginIds")).toArray())
            currentUnsupported.append(value.toString());
        QStringList requestedPlugins;
        for (const QJsonValue& value : params.value(QStringLiteral("pluginIds")).toArray())
            requestedPlugins.append(value.toString());
        if (currentAsync.isEmpty() || requestedPlugins.isEmpty())
            return makeToolError(QStringLiteral("No registered camera plugin supports asynchronous discovery"),
                                 QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
        const bool legacyOnly =
            std::all_of(requestedPlugins.cbegin(), requestedPlugins.cend(),
                        [&currentUnsupported](const QString& id) { return currentUnsupported.contains(id); });
        if (legacyOnly)
            return makeToolError(QStringLiteral("The requested camera plugins do not support asynchronous discovery"),
                                 QStringLiteral("CAMERA_DISCOVERY_ASYNC_UNSUPPORTED"));
        if (targets.value(QStringLiteral("asyncPluginIds")).toArray() !=
                params.value(QStringLiteral("pluginIds")).toArray() ||
            targets.value(QStringLiteral("unsupportedPluginIds")).toArray() !=
                params.value(QStringLiteral("unsupportedPluginIds")).toArray())
            return makeToolError(QStringLiteral("Camera plugin registry changed; query targets again"),
                                 QStringLiteral("CAMERA_TARGET_CHANGED"));
        return {};
    }
    static const QStringList selectorTools{QStringLiteral("run_flow"),       QStringLiteral("validate_flow"),
                                           QStringLiteral("get_flow_state"), QStringLiteral("step_once"),
                                           QStringLiteral("resume_flow"),    QStringLiteral("reset_step")};
    if (!selectorTools.contains(toolName))
        return {};

    if (params.contains(QStringLiteral("flowId"))) {
        const QString flowId = params.value(QStringLiteral("flowId")).toString().trimmed();
        if (flowId.isEmpty())
            return makeToolError(QStringLiteral("Invalid string parameter: flowId"), QStringLiteral("INVALID_PARAM"));
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("unavailable")},
                           {QStringLiteral("error"), QStringLiteral("Flow selection by flowId is not supported")},
                           {QStringLiteral("errorCode"), QStringLiteral("FLOW_UNAVAILABLE")},
                           {QStringLiteral("retryable"), false},
                           {QStringLiteral("flowId"), flowId},
                           {QStringLiteral("flowSwitchSupported"), false}};
    }

    if (toolName == QLatin1String("run_flow")) {
        const QString mode = params.value(QStringLiteral("mode")).toString(QStringLiteral("once"));
        const QJsonValue maxCycles = params.value(QStringLiteral("maxCycles"));
        if (mode == QLatin1String("cycle")) {
            const double value = maxCycles.toDouble(-1.0);
            if (!maxCycles.isDouble() || std::floor(value) != value || value < 1 || value > kMaxAgentCycleIterations)
                return makeToolError(QStringLiteral("cycle mode requires integer maxCycles in the range 1..10"),
                                     maxCycles.isUndefined() ? QStringLiteral("MISSING_PARAM")
                                                             : QStringLiteral("INVALID_PARAM"));
        } else if (!maxCycles.isUndefined()) {
            return makeToolError(QStringLiteral("maxCycles is only valid for cycle mode"),
                                 QStringLiteral("INVALID_PARAM"));
        }
    }

    static const QStringList executionTools{QStringLiteral("run_flow"), QStringLiteral("step_once"),
                                            QStringLiteral("resume_flow")};
    if (executionTools.contains(toolName)) {
        const Project* project = ProjectManager::instance().currentProject();
        if (project) {
            const PluginManager& plugins = PluginManager::instance();
            for (const ModuleInstance& instance : project->modules()) {
                if (plugins.pluginInfo(instance.moduleId).id != QLatin1String("com.deeplux.plugin.grabimage"))
                    continue;
                const QString source = instance.params.value(QStringLiteral("grabSource")).toString("Camera");
                const QString cameraId = instance.params.value(QStringLiteral("cameraId")).toString().trimmed();
                if ((source.isEmpty() || source == QLatin1String("Camera")) && cameraId.isEmpty())
                    return makeToolError(
                        QStringLiteral("Agent camera capture requires an explicit GrabImage cameraId; implicit camera "
                                       "discovery/connection and OpenCV index fallback are not allowed"),
                        QStringLiteral("MISSING_PARAM"));
            }
        }
    }

    return {};
}

QJsonObject AgentActor::executeTools(const QList<QPair<QString, QJsonObject>>& tools, const QString& macroName) {
    // 兼容入口：为无 callId 的旧调用方生成稳定占位 id
    QList<ToolInvocation> invocations;
    invocations.reserve(tools.size());
    for (int i = 0; i < tools.size(); ++i)
        invocations.append({QStringLiteral("auto_%1").arg(i), tools[i].first, tools[i].second});
    return executeTools(invocations, macroName);
}

QJsonObject AgentActor::executeTools(const QList<ToolInvocation>& tools, const QString& macroName) {
    if (tools.isEmpty())
        return makeToolError("No tools to execute", QStringLiteral("MISSING_PARAM"));

    // 阶段1 §7：参数校验失败也保留全部原始 callId——违规项带自身错误码，
    // 其余项补 SKIPPED_DUE_TO_PREVIOUS_ERROR；批次在执行前整体拒绝，零副作用
    for (int i = 0; i < tools.size(); ++i) {
        const QString invalid = validationError(tools[i].tool, tools[i].params);
        if (invalid.isEmpty())
            continue;
        QJsonArray results;
        for (int j = 0; j < tools.size(); ++j) {
            QJsonObject entry{{"callId", tools[j].callId}, {"tool", tools[j].tool}, {"executed", false}};
            entry["result"] =
                (j == i) ? makeToolValidationError(tools[j].tool, invalid)
                         : makeSkippedToolError(tools[j].tool,
                                                QStringLiteral("Batch rejected before execution: validation failed "
                                                               "for call %1")
                                                    .arg(tools[i].callId),
                                                tools[i].callId);
            results.append(entry);
        }
        return QJsonObject{{"status", QStringLiteral("partial_failed")},
                           {"executed", 0},
                           {"failedTool", tools[i].tool},
                           {"failedCallId", tools[i].callId},
                           {"error", invalid},
                           {"errorCode", validationErrorCode(invalid)},
                           {"retryable", false},
                           {"results", results}};
    }

    QMap<QString, int> cameraParameterWrites;
    for (int i = 0; i < tools.size(); ++i) {
        if (tools.at(i).tool != QLatin1String("set_camera_parameter"))
            continue;
        const QString deviceId = tools.at(i).params.value(QStringLiteral("deviceId")).toString();
        if (!cameraParameterWrites.contains(deviceId)) {
            cameraParameterWrites.insert(deviceId, i);
            continue;
        }
        QJsonArray rejectedResults;
        for (int j = 0; j < tools.size(); ++j) {
            QJsonObject entry{{QStringLiteral("callId"), tools.at(j).callId},
                              {QStringLiteral("tool"), tools.at(j).tool},
                              {QStringLiteral("executed"), false}};
            const QJsonObject result =
                j == i ? makeToolError(QStringLiteral("Only one camera parameter write per device is allowed in a "
                                                      "tool batch"),
                                       QStringLiteral("CAMERA_OPERATION_BATCH_CONFLICT"))
                       : makeSkippedToolError(tools.at(j).tool,
                                              QStringLiteral("Batch rejected before execution: duplicate camera "
                                                             "parameter target"),
                                              tools.at(i).callId);
            entry.insert(QStringLiteral("result"), result);
            rejectedResults.append(entry);
        }
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("partial_failed")},
                           {QStringLiteral("executed"), 0},
                           {QStringLiteral("failedTool"), tools.at(i).tool},
                           {QStringLiteral("failedCallId"), tools.at(i).callId},
                           {QStringLiteral("error"), QStringLiteral("Duplicate camera parameter target")},
                           {QStringLiteral("errorCode"), QStringLiteral("CAMERA_OPERATION_BATCH_CONFLICT")},
                           {QStringLiteral("retryable"), false},
                           {QStringLiteral("results"), rejectedResults}};
    }

    for (int i = 0; i < tools.size(); ++i) {
        const QJsonObject selectorError = flowSelectorPreflight(tools[i].tool, tools[i].params);
        if (selectorError.isEmpty())
            continue;
        QJsonArray results;
        for (int j = 0; j < tools.size(); ++j) {
            QJsonObject entry{{QStringLiteral("callId"), tools[j].callId},
                              {QStringLiteral("tool"), tools[j].tool},
                              {QStringLiteral("executed"), false}};
            entry[QStringLiteral("result")] =
                (j == i) ? selectorError
                         : makeSkippedToolError(tools[j].tool,
                                                QStringLiteral("Batch rejected before execution: flow selector is not "
                                                               "available for call %1")
                                                    .arg(tools[i].callId),
                                                tools[i].callId);
            results.append(entry);
        }
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("partial_failed")},
                           {QStringLiteral("executed"), 0},
                           {QStringLiteral("failedTool"), tools[i].tool},
                           {QStringLiteral("failedCallId"), tools[i].callId},
                           {QStringLiteral("error"), selectorError.value(QStringLiteral("error"))},
                           {QStringLiteral("errorCode"), selectorError.value(QStringLiteral("errorCode"))},
                           {QStringLiteral("retryable"), false},
                           {QStringLiteral("results"), results}};
    }
    if (firstInvalid >= 0) {
        const QString failedTool = tools.at(firstInvalid).first;
        QJsonArray results;
        for (int i = 0; i < tools.size(); ++i) {
            if (i == firstInvalid) {
                results.append(resultEntry(tools.at(i).first,
                                           QJsonObject{{QStringLiteral("success"), false},
                                                       {QStringLiteral("error"), invalid},
                                                       {QStringLiteral("code"), QStringLiteral("invalid_parameter")}}));
            } else {
                results.append(resultEntry(tools.at(i).first,
                                           skippedAfterFailure(failedTool, QStringLiteral("invalid_parameter"))));
            }
        }
        return QJsonObject{{"status", "partial_failed"},
                           {"results", results},
                           {"executed", 0},
                           {"failedTool", failedTool},
                           {"failedCode", QStringLiteral("invalid_parameter")},
                           {"error", invalid}};
    }

    QJsonArray results;
    int executed = 0;
    bool failed = false;
    QString failedTool;
    QString failedCallId;
    QString error;
    QString errorCode;
    Q_ASSERT(!m_batchUndoMacroOpen);
    m_batchUndoMacroName = macroName;
    m_batchUndoMacroEligible = false;
    QJsonObject chainedImportProjectSnapshot;
    for (int i = 0; i < tools.size(); ++i) {
        ToolInvocation invocation = tools[i];
        if (failed) {
            // 阶段1 §7：遇错停止后，为每个未执行的原始 call 补一个结果（零副作用）
            QJsonObject entry{{"callId", invocation.callId}, {"tool", invocation.tool}, {"executed", false}};
            entry["result"] = makeSkippedToolError(invocation.tool,
                                                   QStringLiteral("Batch stopped after previous error"), failedCallId);
            results.append(entry);
            continue;
        }
        const bool switchesProject =
            invocation.tool == QStringLiteral("create_project") || invocation.tool == QStringLiteral("open_project");
        const bool isUndoBarrierTool = invocation.tool == QStringLiteral("save_project") ||
                                       invocation.tool == QStringLiteral("run_flow") ||
                                       invocation.tool == QStringLiteral("step_once");
        // Position commands can expire independently across GUI/Agent edits. Keep them out
        // of mixed macros so a stale no-op cannot partially undo sibling tool calls.
        const bool isolatesPositionUndo = invocation.tool == QStringLiteral("set_module_position");
        // Parameter commands have per-key expiry and must never partially undo a mixed macro.
        const bool isolatesParameterUndo = invocation.tool == QStringLiteral("set_param") ||
                                           invocation.tool == QStringLiteral("set_module_params") ||
                                           invocation.tool == QStringLiteral("reset_module_params");
        // Runtime-field commands can expire independently after a GUI edit, so they cannot
        // share a macro with sibling calls that would otherwise be partially undone.
        const bool isolatesRuntimeFieldUndo = invocation.tool == QStringLiteral("set_module_note") ||
                                              invocation.tool == QStringLiteral("set_module_enabled") ||
                                              invocation.tool == QStringLiteral("set_breakpoint");
        // Module order commands can expire after an external reorder and must stay independent too.
        const bool isolatesModuleOrderUndo = invocation.tool == QStringLiteral("move_module");
        if ((switchesProject || isolatesPositionUndo || isolatesParameterUndo || isolatesRuntimeFieldUndo ||
             isolatesModuleOrderUndo || isUndoBarrierTool) &&
            m_batchUndoMacroOpen) {
            m_undoStack->endMacro();
            m_batchUndoMacroOpen = false;
        }
        m_batchUndoMacroEligible = !switchesProject && !isolatesPositionUndo && !isolatesParameterUndo &&
                                   !isolatesRuntimeFieldUndo && !isolatesModuleOrderUndo && !isUndoBarrierTool;
        if (invocation.tool == QLatin1String("import_data_source") && !chainedImportProjectSnapshot.isEmpty() &&
            invocation.dataSourceImportSnapshot.value(QStringLiteral("projectRuntimeIdentity")) ==
                chainedImportProjectSnapshot.value(QStringLiteral("projectRuntimeIdentity"))) {
            invocation.dataSourceImportSnapshot.insert(
                QStringLiteral("projectFingerprint"),
                chainedImportProjectSnapshot.value(QStringLiteral("projectFingerprint")));
            invocation.dataSourceImportSnapshot.insert(
                QStringLiteral("dataSourceGeneration"),
                chainedImportProjectSnapshot.value(QStringLiteral("dataSourceGeneration")));
            invocation.dataSourceImportSnapshot.insert(
                QStringLiteral("projectBaseline"),
                chainedImportProjectSnapshot.value(QStringLiteral("projectBaseline")));
            invocation.dataSourceImportSnapshot.insert(
                QStringLiteral("projectDirty"), chainedImportProjectSnapshot.value(QStringLiteral("projectDirty")));
        }
        QJsonObject result =
            executeToolWithCameraTarget(invocation.tool, invocation.params, invocation.cameraTargetSnapshot,
                                        invocation.folderDeleteTargetAuthorization, invocation.variableSnapshot,
                                        invocation.dataSourceImportSnapshot, invocation.callId,
                                        invocation.annotationExportSnapshot, invocation.annotationTransformSnapshot);
        m_batchUndoMacroEligible = false;
        if (invocation.tool == QLatin1String("import_data_source") && !result.contains(QStringLiteral("error")) &&
            (result.value(QStringLiteral("status")) == QLatin1String("imported") ||
             result.value(QStringLiteral("status")) == QLatin1String("already_imported"))) {
            Project* currentProject = ProjectManager::instance().currentProject();
            if (currentProject) {
                QJsonObject expectedProject =
                    invocation.dataSourceImportSnapshot.value(QStringLiteral("projectBaseline")).toObject();
                qint64 expectedGeneration =
                    invocation.dataSourceImportSnapshot.value(QStringLiteral("dataSourceGeneration"))
                        .toVariant()
                        .toLongLong();
                bool expectedDirty = invocation.dataSourceImportSnapshot.value(QStringLiteral("projectDirty")).toBool();
                if (result.value(QStringLiteral("status")) == QLatin1String("imported")) {
                    const QString dataSourceId = result.value(QStringLiteral("dataSourceId")).toString();
                    const std::optional<DataSource> added = currentProject->findDataSource(dataSourceId);
                    if (added) {
                        QJsonObject resources = expectedProject.value(QStringLiteral("resources")).toObject();
                        QJsonArray expectedSources = resources.value(QStringLiteral("dataSources")).toArray();
                        expectedSources.append(added->toJson());
                        resources.insert(QStringLiteral("dataSources"), expectedSources);
                        expectedProject.insert(QStringLiteral("resources"), resources);
                        ++expectedGeneration;
                        expectedDirty = true;
                    } else {
                        expectedProject = {};
                    }
                }
                if (!expectedProject.isEmpty()) {
                    chainedImportProjectSnapshot = QJsonObject{
                        {QStringLiteral("projectRuntimeIdentity"),
                         invocation.dataSourceImportSnapshot.value(QStringLiteral("projectRuntimeIdentity"))},
                        {QStringLiteral("projectFingerprint"),
                         QString::fromLatin1(dataSourceImportProjectFingerprint(expectedProject))},
                        {QStringLiteral("dataSourceGeneration"), expectedGeneration},
                        {QStringLiteral("projectBaseline"), expectedProject},
                        {QStringLiteral("projectDirty"), expectedDirty}};
                } else {
                    chainedImportProjectSnapshot = {};
                }
            }
        }
        QJsonObject entry;
        entry["callId"] = invocation.callId;
        entry["tool"] = invocation.tool;
        entry["executed"] = true;
        entry["result"] = result;
        results.append(entry);
        if (result.contains("error")) {
            failed = true;
            failedTool = invocation.tool;
            failedCallId = invocation.callId;
            error = result["error"].toString();
            errorCode = result.value("errorCode").toString();
        } else {
            ++executed;
        }
    }
    if (m_batchUndoMacroOpen) {
        m_undoStack->endMacro();
        m_batchUndoMacroOpen = false;
    }
    m_batchUndoMacroEligible = false;
    m_batchUndoMacroName.clear();

    if (failed) {
        QJsonObject out{{"status", QStringLiteral("partial_failed")},
                        {"results", results},
                        {"executed", executed},
                        {"failedTool", failedTool},
                        {"failedCallId", failedCallId},
                        {"error", error}};
        if (!errorCode.isEmpty())
            out.insert(QStringLiteral("errorCode"), errorCode);
        out.insert(QStringLiteral("retryable"), false);
        return out;
    }
    return QJsonObject{{"status", "completed"}, {"results", results}, {"executed", executed}};
}

void AgentActor::pushUndoCommand(QUndoCommand* command) {
    if (m_batchUndoMacroEligible && !m_batchUndoMacroOpen) {
        m_undoStack->beginMacro(m_batchUndoMacroName);
        m_batchUndoMacroOpen = true;
    }
    m_undoStack->push(command);
}

// --- Tool Handlers ---

// 阶段1 §3：切换前未保存检查（create/open 共用）——委托 ProjectManager::resolveUnsavedForSwitch
// 共享实现（GUI/CLI/Agent 同一逻辑源）。返回空对象=可继续；否则为已构造的失败响应
QJsonObject guardUnsavedProject(const QJsonObject& params) {
    const QString onUnsaved = params.value(QStringLiteral("onUnsaved")).toString(QStringLiteral("cancel"));
    const auto policy = onUnsaved == QLatin1String("save")      ? ProjectManager::UnsavedPolicy::Save
                        : onUnsaved == QLatin1String("discard") ? ProjectManager::UnsavedPolicy::Discard
                                                                : ProjectManager::UnsavedPolicy::Cancel;
    QString code, message;
    if (ProjectManager::instance().resolveUnsavedForSwitch(policy, &code, &message))
        return {};
    QJsonObject details;
    if (code == QLatin1String("PROJECT_UNSAVED"))
        details.insert(
            QStringLiteral("options"),
            QJsonArray::fromStringList({QStringLiteral("save"), QStringLiteral("discard"), QStringLiteral("cancel")}));
    return makeToolError(message, code, false, details);
}

QJsonObject AgentActor::createProject(const QJsonObject& params) {
    QString name = params.value("name").toString("unnamed");
    const QJsonObject guard = guardUnsavedProject(params);
    if (!guard.isEmpty())
        return guard;
    Project* project = ProjectManager::instance().newProject();
    if (!project)
        return makeToolError("Failed to create project", QStringLiteral("INVALID_STATE"));
    project->setName(name);
    return QJsonObject{{"status", "created"}, {"name", name}};
}

QJsonObject AgentActor::addModule(const QJsonObject& params) {
    QString plugin = params.value("plugin").toString();
    QString instanceName = params.value("instanceName").toString().trimmed();
    if (plugin.isEmpty())
        return makeToolError("Missing 'plugin' parameter", QStringLiteral("MISSING_PARAM"));
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));

    PluginManager& manager = PluginManager::instance();
    const PluginInfo info = manager.pluginInfo(plugin);
    if (info.name.isEmpty())
        return makeToolError(QString("Plugin not found: %1").arg(plugin), QStringLiteral("PLUGIN_NOT_FOUND"));
    PluginManager::ModulePtr module = manager.createManagedModule(info.name);
    if (!module)
        return makeToolError(QString("Plugin is not loaded: %1").arg(plugin), QStringLiteral("PLUGIN_NOT_LOADED"));

    const QString instanceId = instanceName.isEmpty()
                                   ? QString("%1_%2").arg(info.name, QUuid::createUuid().toString(QUuid::WithoutBraces))
                                   : instanceName;
    if (project->moduleById(instanceId))
        return makeToolError(QString("Module instance already exists: %1").arg(instanceId),
                             QStringLiteral("INVALID_PARAM"));

    const QString displayName = instanceName.isEmpty() ? module->name() : instanceName;
    pushUndoCommand(new AgentAddModuleCmd(info.name, instanceId, displayName));
    return QJsonObject{{"status", "added"}, {"plugin", info.name}, {"instanceId", instanceId}};
}

QJsonObject AgentActor::removeModule(const QJsonObject& params) {
    QString instanceId = params.value("instanceId").toString();
    if (instanceId.isEmpty())
        return makeToolError("Missing 'instanceId' parameter", QStringLiteral("MISSING_PARAM"));
    // 阶段1 §1（F10）：未知实例必须失败，不得以空撤销命令谎称完成删除
    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    if (!proj->moduleById(instanceId))
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));
    pushUndoCommand(new AgentRemoveModuleCmd(instanceId));
    return QJsonObject{{"status", "removed"}, {"instanceId", instanceId}};
}

QJsonObject AgentActor::moveModule(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    const QList<ModuleInstance> modules = project->modules();
    int oldIndex = -1;
    int matchingIds = 0;
    for (int i = 0; i < modules.size(); ++i) {
        if (modules[i].id == instanceId) {
            ++matchingIds;
            oldIndex = i;
        }
    }
    if (oldIndex < 0)
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));
    if (matchingIds > 1)
        return makeToolError(QString("Module instance ID is ambiguous: %1").arg(instanceId),
                             QStringLiteral("INVALID_STATE"));
    const QJsonValue indexValue = params.value(QStringLiteral("index"));
    const double index = indexValue.toDouble(-1);
    if (!indexValue.isDouble() || !std::isfinite(index) || std::floor(index) != index || index < 0 ||
        index >= modules.size())
        return makeToolError("index must be an integer within the current module list",
                             QStringLiteral("INVALID_PARAM"));
    const int newIndex = static_cast<int>(index);
    if (oldIndex == newIndex)
        return toolNoOp(QStringLiteral("module already at requested index"));
    QStringList oldOrder;
    oldOrder.reserve(modules.size());
    for (const ModuleInstance& module : modules)
        oldOrder.append(module.id);
    pushUndoCommand(new AgentMoveModuleCmd(project, instanceId, oldIndex, newIndex, oldOrder,
                                           [this]() { m_moduleOrderUndoExpired = true; }));
    return QJsonObject{{"status", "moved"}, {"instanceId", instanceId}, {"oldIndex", oldIndex}, {"index", newIndex}};
}

QJsonObject AgentActor::setModulePosition(const QJsonObject& params) {
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    const QJsonValue xValue = params.value(QStringLiteral("x"));
    const QJsonValue yValue = params.value(QStringLiteral("y"));
    auto parseCoordinate = [](const QJsonValue& value, int* coordinate) {
        if (!value.isDouble())
            return false;
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || number < std::numeric_limits<int>::min() ||
            number > std::numeric_limits<int>::max())
            return false;
        *coordinate = static_cast<int>(number);
        return true;
    };

    int x = 0;
    int y = 0;
    if (!parseCoordinate(xValue, &x) || !parseCoordinate(yValue, &y))
        return makeToolError(QStringLiteral("x and y must be signed 32-bit integers"), QStringLiteral("INVALID_PARAM"));

    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError(QStringLiteral("No project opened"), QStringLiteral("NO_PROJECT"));
    const std::optional<ModuleInstance> instance = project->moduleById(instanceId);
    if (!instance)
        return makeToolError(QStringLiteral("Module not found: %1").arg(instanceId),
                             QStringLiteral("INSTANCE_NOT_FOUND"));

    if (instance->posX == x && instance->posY == y) {
        emit modulePositionSyncRequested(instanceId);
        return toolNoOp(QStringLiteral("module already at requested position"));
    }

    m_positionUndoExpired = false;
    const Project::PositionGenerations generations = project->positionGenerations(instanceId);
    pushUndoCommand(new AgentSetModulePositionCmd(project, instanceId, instance->posX, instance->posY, x, y,
                                                  generations.agent, [this, instanceId](bool redo) {
                                                      m_positionUndoExpired = true;
                                                      emit positionUndoExpired(instanceId, redo);
                                                  }));
    return QJsonObject{{"status", QStringLiteral("updated")},
                       {"instanceId", instanceId},
                       {"oldX", instance->posX},
                       {"oldY", instance->posY},
                       {"x", x},
                       {"y", y}};
}

QJsonObject AgentActor::renameModule(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    const std::optional<ModuleInstance> instance = project->moduleById(instanceId);
    if (!instance)
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));
    const QJsonValue value = params.value(QStringLiteral("name"));
    if (!value.isString() || value.toString().trimmed().isEmpty())
        return makeToolError("name must be a non-empty string", QStringLiteral("INVALID_PARAM"));
    const QString name = value.toString().trimmed();
    if (instance->name == name)
        return toolNoOp(QStringLiteral("module already has requested name"));
    pushUndoCommand(new AgentRenameModuleCmd(instanceId, instance->name, name));
    return QJsonObject{{"status", "renamed"}, {"instanceId", instanceId}, {"oldName", instance->name}, {"name", name}};
}

QJsonObject AgentActor::setModuleEnabled(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    const std::optional<ModuleInstance> instance = project->moduleById(instanceId);
    if (!instance)
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));
    const QJsonValue enabledValue = params.value(QStringLiteral("enabled"));
    if (!enabledValue.isBool())
        return makeToolError("enabled must be a boolean", QStringLiteral("INVALID_PARAM"));
    const bool enabled = enabledValue.toBool();
    if (instance->enabled == enabled)
        return toolNoOp(QStringLiteral("module already has requested enabled state"));
    m_runtimeFieldUndoExpired = false;
    pushUndoCommand(new AgentRuntimeFieldCmd(
        project, instanceId, Project::RuntimeField::Enabled, instance->enabled, enabled,
        project->runtimeFieldExternalGeneration(instanceId, Project::RuntimeField::Enabled),
        project->parameterInstanceGeneration(instanceId), [this]() { m_runtimeFieldUndoExpired = true; }));
    return QJsonObject{
        {"status", "updated"}, {"instanceId", instanceId}, {"oldEnabled", instance->enabled}, {"enabled", enabled}};
}

QJsonObject AgentActor::setBreakpoint(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    const std::optional<ModuleInstance> instance = project->moduleById(instanceId);
    if (!instance)
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));
    const QJsonValue enabledValue = params.value(QStringLiteral("enabled"));
    if (!enabledValue.isBool())
        return makeToolError("enabled must be a boolean", QStringLiteral("INVALID_PARAM"));
    const bool enabled = enabledValue.toBool();
    if (instance->breakpoint == enabled)
        return toolNoOp(QStringLiteral("module breakpoint already has requested state"));
    m_runtimeFieldUndoExpired = false;
    pushUndoCommand(new AgentRuntimeFieldCmd(
        project, instanceId, Project::RuntimeField::Breakpoint, instance->breakpoint, enabled,
        project->runtimeFieldExternalGeneration(instanceId, Project::RuntimeField::Breakpoint),
        project->parameterInstanceGeneration(instanceId), [this]() { m_runtimeFieldUndoExpired = true; }));
    return QJsonObject{{"status", "updated"},
                       {"instanceId", instanceId},
                       {"oldBreakpoint", instance->breakpoint},
                       {"breakpoint", enabled}};
}

QJsonObject AgentActor::setModuleNote(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    const std::optional<ModuleInstance> instance = project->moduleById(instanceId);
    if (!instance)
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));
    const QJsonValue noteValue = params.value(QStringLiteral("note"));
    if (!noteValue.isString())
        return makeToolError("note must be a string", QStringLiteral("INVALID_PARAM"));
    const QString note = noteValue.toString();
    if (instance->note == note)
        return toolNoOp(QStringLiteral("module note already has requested value"));
    m_runtimeFieldUndoExpired = false;
    pushUndoCommand(new AgentRuntimeFieldCmd(
        project, instanceId, Project::RuntimeField::Note, instance->note, note,
        project->runtimeFieldExternalGeneration(instanceId, Project::RuntimeField::Note),
        project->parameterInstanceGeneration(instanceId), [this]() { m_runtimeFieldUndoExpired = true; }));
    return QJsonObject{{"status", "updated"}, {"instanceId", instanceId}, {"oldNote", instance->note}, {"note", note}};
}

QJsonObject AgentActor::setParam(const QJsonObject& params) {
    QJsonObject request{{"instanceId", params.value(QStringLiteral("instanceId"))},
                        {"values", QJsonObject{{params.value(QStringLiteral("key")).toString(),
                                                params.value(QStringLiteral("value"))}}}};
    return editModuleParams(request, false, true);
}

QJsonObject AgentActor::setModuleParams(const QJsonObject& params) {
    return editModuleParams(params, false, false);
}

QJsonObject AgentActor::resetModuleParams(const QJsonObject& params) {
    return editModuleParams(params, true, false);
}

QJsonObject AgentActor::editModuleParams(const QJsonObject& params, bool reset, bool legacySingle) {
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    const std::optional<ModuleInstance> instance = project->moduleById(instanceId);
    if (!instance)
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));

    const QJsonValue input = params.value(reset ? QStringLiteral("keys") : QStringLiteral("values"));
    if ((reset && !input.isArray()) || (!reset && !input.isObject()))
        return makeToolError(reset ? "'keys' must be an array" : "'values' must be an object",
                             QStringLiteral("INVALID_PARAM"));
    if (reset && input.toArray().isEmpty())
        return makeToolError("'keys' must not be empty", QStringLiteral("INVALID_PARAM"));
    if (!reset && QJsonDocument(input.toObject()).toJson(QJsonDocument::Compact).size() > 65536)
        return makeToolError("Parameter update exceeds 64 KiB", QStringLiteral("INVALID_PARAM"));

    PluginManager& manager = PluginManager::instance();
    PluginManager::ModulePtr module = manager.createManagedModule(instance->moduleId);
    if (!module)
        return makeToolError(QString("Cannot create module for validation: %1").arg(instance->moduleId),
                             QStringLiteral("PLUGIN_NOT_LOADED"));
    const QJsonObject defaults = module->defaultParams();
    const QJsonObject uiParams =
        manager.pluginInfo(instance->moduleId).ui.value(QStringLiteral("parameters")).toObject();
    QJsonObject next = instance->params;
    QStringList changedKeys;
    if (reset) {
        for (const QJsonValue& item : input.toArray()) {
            if (!item.isString() || item.toString().isEmpty() || changedKeys.contains(item.toString()))
                return makeToolError("'keys' must contain unique nonempty strings", QStringLiteral("INVALID_PARAM"));
            const QString key = item.toString();
            if (!defaults.contains(key) && !uiParams.contains(key))
                return makeToolError(QString("Unknown parameter '%1'").arg(key), QStringLiteral("INVALID_PARAM"));
            if (uiParams.value(key).toObject().value(QStringLiteral("readOnly")).toBool())
                return makeToolError(QString("Parameter '%1' is read-only").arg(key), QStringLiteral("INVALID_PARAM"));
            next.remove(key);
            changedKeys.append(key);
        }
    } else {
        const QJsonObject values = input.toObject();
        for (auto it = values.begin(); it != values.end(); ++it) {
            const QString key = it.key();
            if (!defaults.contains(key) && !uiParams.contains(key))
                return makeToolError(QString("Unknown parameter '%1'").arg(key), QStringLiteral("INVALID_PARAM"));
            if (uiParams.value(key).toObject().value(QStringLiteral("readOnly")).toBool())
                return makeToolError(QString("Parameter '%1' is read-only").arg(key), QStringLiteral("INVALID_PARAM"));
            const QJsonValue reference = defaults.value(key);
            QJsonValue value = it.value();
            if (legacySingle && value.isString()) {
                const QString text = value.toString().trimmed();
                if (reference.isDouble()) {
                    bool ok = false;
                    const double number = text.toDouble(&ok);
                    if (!ok || !std::isfinite(number))
                        return makeToolError(QString("Value '%1' is not a finite number for key %2").arg(text, key),
                                             QStringLiteral("INVALID_PARAM"));
                    value = number;
                } else if (reference.isBool()) {
                    if (text.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0)
                        value = true;
                    else if (text.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0)
                        value = false;
                    else
                        return makeToolError(QString("Value '%1' is not a boolean for key %2").arg(text, key),
                                             QStringLiteral("INVALID_PARAM"));
                }
            }
            const bool nullable =
                reference.isNull() || uiParams.value(key).toObject().value(QStringLiteral("nullable")).toBool();
            if (value.isNull() && !nullable)
                return makeToolError(QString("Null is not allowed for '%1'").arg(key), QStringLiteral("INVALID_PARAM"));
            const QJsonArray acceptedTypes = uiParams.value(key).toObject().value("acceptedTypes").toArray();
            const QString valueType = value.isString()   ? QStringLiteral("string")
                                      : value.isArray()  ? QStringLiteral("array")
                                      : value.isObject() ? QStringLiteral("object")
                                      : value.isBool()   ? QStringLiteral("boolean")
                                      : value.isDouble() ? QStringLiteral("number")
                                                         : QStringLiteral("null");
            if (!reference.isUndefined() && !reference.isNull() && value.type() != reference.type() &&
                !acceptedTypes.contains(valueType))
                return makeToolError(QString("Wrong JSON type for '%1'").arg(key), QStringLiteral("INVALID_PARAM"));
            if (value.isDouble() && !std::isfinite(value.toDouble()))
                return makeToolError(QString("Non-finite value for '%1'").arg(key), QStringLiteral("INVALID_PARAM"));
            next.insert(key, value);
            changedKeys.append(key);
        }
    }

    if (instance->moduleId == QLatin1String("MeasurementInput") &&
        next.contains(QStringLiteral("pick_coordinate_frame")) &&
        std::any_of(changedKeys.cbegin(), changedKeys.cend(), [](const QString& key) {
            return key == QLatin1String("mode") || key == QLatin1String("point") || key == QLatin1String("line") ||
                   key == QLatin1String("line1") || key == QLatin1String("line2");
        })) {
        next.insert(QStringLiteral("pick_coordinate_frame"), QStringLiteral("Unspecified"));
        if (!changedKeys.contains(QStringLiteral("pick_coordinate_frame")))
            changedKeys.append(QStringLiteral("pick_coordinate_frame"));
    }

    if (next == instance->params)
        return toolNoOp(QStringLiteral("Module parameters already have the requested values"));
    QJsonObject effective = defaults;
    for (auto it = next.begin(); it != next.end(); ++it)
        effective.insert(it.key(), it.value());
    QString error;
    if (!module->validateParams(effective, error)) {
        emit toolError(reset ? "reset_module_params" : legacySingle ? "set_param" : "set_module_params", error);
        return makeToolError(error, QStringLiteral("VALIDATION_FAILED"));
    }
    m_parameterUndoExpired = false;
    pushUndoCommand(new AgentSetModuleParamsCmd(project, instanceId, instance->params, next,
                                                [this]() { m_parameterUndoExpired = true; }));
    QJsonObject result{{"status", legacySingle ? "param_set"
                                  : reset      ? "params_reset"
                                               : "params_set"},
                       {"instanceId", instanceId},
                       {"keys", QJsonArray::fromStringList(changedKeys)},
                       {"effectiveParams", effective}};
    if (legacySingle)
        result.insert(QStringLiteral("key"), changedKeys.first());
    return result;
}

QJsonObject AgentActor::connectModules(const QJsonObject& params) {
    QString fromId = params.value("fromId").toString();
    QString toId = params.value("toId").toString();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    const std::optional<ModuleInstance> fromModule = proj->moduleById(fromId);
    const std::optional<ModuleInstance> toModule = proj->moduleById(toId);
    if (!fromModule)
        return makeToolError(QString("Module not found: %1").arg(fromId), QStringLiteral("INSTANCE_NOT_FOUND"));
    if (!toModule)
        return makeToolError(QString("Module not found: %1").arg(toId), QStringLiteral("INSTANCE_NOT_FOUND"));

    PluginManager& manager = PluginManager::instance();
    const PluginInfo fromInfo = manager.pluginInfo(fromModule->moduleId);
    const PluginInfo toInfo = manager.pluginInfo(toModule->moduleId);
    if (fromInfo.name.isEmpty() || toInfo.name.isEmpty())
        return makeToolError("Cannot resolve plugin metadata for connection", QStringLiteral("PLUGIN_NOT_FOUND"));

    QString fromPort = params.value("fromPort").toString().trimmed();
    if (fromPort.isEmpty())
        fromPort = params.value("port").toString().trimmed();
    if (fromPort.isEmpty())
        fromPort = QStringLiteral("image");
    QString toPort = params.value("toPort").toString().trimmed();
    const PortSpec* output = findPort(fromInfo.outputPorts, fromPort);
    const bool fromIsControl = fromPort == QLatin1String("next") || (output && output->control);
    if (toPort.isEmpty()) {
        toPort = fromIsControl ? QStringLiteral("control")
                               : (findPort(toInfo.inputPorts, fromPort) ? fromPort : QStringLiteral("image"));
    }
    const PortSpec* input = findPort(toInfo.inputPorts, toPort);
    const bool toIsControl = toPort == QLatin1String("control") || (input && input->control);
    if (!output && fromPort != QLatin1String("next"))
        return makeToolError(QString("Output port not found: %1.%2").arg(fromId, fromPort),
                             QStringLiteral("INVALID_PARAM"));
    if (!input && toPort != QLatin1String("control"))
        return makeToolError(QString("Input port not found: %1.%2").arg(toId, toPort), QStringLiteral("INVALID_PARAM"));
    if (fromIsControl != toIsControl) {
        return makeToolError(QString("Control connections require a control source and target: %1.%2 -> %3.%4")
                                 .arg(fromId, fromPort, toId, toPort),
                             QStringLiteral("VALIDATION_FAILED"));
    }
    if (!fromIsControl && !portTypesCompatible(*output, *input)) {
        return makeToolError(QString("Incompatible ports: %1.%2 -> %3.%4").arg(fromId, fromPort, toId, toPort),
                             QStringLiteral("VALIDATION_FAILED"));
    }

    for (const ModuleConnection& connection : proj->connections()) {
        if (connection.fromModuleId == fromId && connection.fromPort == fromPort && connection.toModuleId == toId &&
            connection.toPort == toPort) {
            return QJsonObject{{"status", "already_connected"}, {"from", fromId},   {"to", toId},
                               {"fromPort", fromPort},          {"toPort", toPort}, {"edgeType", connection.edgeType}};
        }
        if (!fromIsControl && !input->multiple && connection.toModuleId == toId && connection.toPort == toPort)
            return makeToolError(QString("Input port already connected: %1.%2").arg(toId, toPort),
                                 QStringLiteral("VALIDATION_FAILED"));
    }

    const QString edgeType = fromIsControl ? QStringLiteral("control") : QStringLiteral("data");
    pushUndoCommand(new AgentConnectCmd(fromId, fromPort, toId, toPort, edgeType));

    return QJsonObject{{"status", "connected"}, {"from", fromId},   {"to", toId},
                       {"fromPort", fromPort},  {"toPort", toPort}, {"edgeType", edgeType}};
}

QJsonObject AgentActor::disconnectModules(const QJsonObject& params) {
    QString fromId = params.value("fromId").toString();
    QString toId = params.value("toId").toString();
    const QString fromPort = params.value("fromPort").toString().trimmed();
    const QString toPort = params.value("toPort").toString().trimmed();

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));

    QList<ModuleConnection> matches;
    int matchedIndex = -1;
    const QList<ModuleConnection> connections = proj->connections();
    for (int i = 0; i < connections.size(); ++i) {
        const ModuleConnection& connection = connections.at(i);
        if (connection.fromModuleId == fromId && connection.toModuleId == toId &&
            (fromPort.isEmpty() || connection.fromPort == fromPort) &&
            (toPort.isEmpty() || connection.toPort == toPort)) {
            matches.append(connection);
            matchedIndex = i;
        }
    }
    if (matches.isEmpty())
        return makeToolError("Connection not found", QStringLiteral("INSTANCE_NOT_FOUND"), false,
                             QJsonObject{{"target", QStringLiteral("connection")}});
    if (matches.size() > 1)
        return makeToolError("Multiple connections match; specify fromPort and toPort",
                             QStringLiteral("INVALID_PARAM"));

    const ModuleConnection connection = matches.first();
    pushUndoCommand(new AgentDisconnectCmd(connection, matchedIndex));

    return QJsonObject{{"status", "disconnected"},
                       {"from", fromId},
                       {"to", toId},
                       {"fromPort", connection.fromPort},
                       {"toPort", connection.toPort},
                       {"edgeType", connection.edgeType}};
}

QJsonObject AgentActor::runFlow(const QJsonObject& params, const QJsonObject& folderDeleteTargetAuthorization) {
    const QString mode = params.value(QStringLiteral("mode")).toString(QStringLiteral("once"));
    if (mode != QLatin1String("once") && mode != QLatin1String("cycle"))
        return makeToolError(QStringLiteral("mode must be once or cycle"), QStringLiteral("INVALID_PARAM"));

    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError(QStringLiteral("No project opened"), QStringLiteral("NO_PROJECT"));

    FlowExecutionService& service = FlowExecutionService::instance();
    RunEngine& engine = RunEngine::instance();
    if (service.isActive() || engine.isBusy())
        return makeToolError(QStringLiteral("Flow is already running"), QStringLiteral("ENGINE_BUSY"), true);

    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return makeToolError(loadError, QStringLiteral("ENGINE_LOAD_FAILED"), true);
    if (engine.modules().isEmpty())
        return makeToolError(QStringLiteral("No modules to run"), QStringLiteral("INVALID_STATE"));

    FlowExecutionService::JobRequest request;
    request.authorization = folderDeleteTargetAuthorization;
    bool started = false;
    if (mode == QLatin1String("cycle")) {
        const QJsonValue cyclesValue = params.value(QStringLiteral("maxCycles"));
        const double cyclesNumber = cyclesValue.toDouble(-1.0);
        if (!cyclesValue.isDouble() || std::floor(cyclesNumber) != cyclesNumber || cyclesNumber < 1 ||
            cyclesNumber > kMaxAgentCycleIterations)
            return makeToolError(QStringLiteral("cycle mode requires integer maxCycles in the range 1..10"),
                                 cyclesValue.isUndefined() ? QStringLiteral("MISSING_PARAM")
                                                           : QStringLiteral("INVALID_PARAM"));
        request.maxCycles = static_cast<int>(cyclesNumber);
        started = service.startCycle(&request);
    } else {
        if (params.contains(QStringLiteral("maxCycles")))
            return makeToolError(QStringLiteral("maxCycles is only valid for cycle mode"),
                                 QStringLiteral("INVALID_PARAM"));
        started = service.startSingle(&request);
    }
    if (!started)
        return makeToolError(QStringLiteral("Flow is already running"), QStringLiteral("ENGINE_BUSY"), true);

    establishUndoBarrier();
    QJsonObject result{{QStringLiteral("status"), QStringLiteral("started")},
                       {QStringLiteral("mode"), mode},
                       {QStringLiteral("async"), true},
                       {QStringLiteral("runId"), request.runId}};
    if (mode == QLatin1String("cycle")) {
        result.insert(QStringLiteral("sessionId"), request.sessionId);
        result.insert(QStringLiteral("maxCycles"), request.maxCycles);
    }
    return result;
}

QJsonObject AgentActor::stepOnce(const QJsonObject& params, const QJsonObject& folderDeleteTargetAuthorization) {
    Q_UNUSED(params);
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy())
        return makeToolError(QStringLiteral("Flow is running; single-step is unavailable"),
                             QStringLiteral("ENGINE_BUSY"), true);

    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError(QStringLiteral("No project opened"), QStringLiteral("NO_PROJECT"));

    // 若 MainWindow 在场，让它先执行既有 GUI→Engine 同步；无 GUI 的调用方仍走下方加载回退。
    emit runEngineSyncRequested();
    const QByteArray fingerprint = QCryptographicHash::hash(
        QJsonDocument(project->toJson()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
    const QList<ModuleBase*> runtimeModules = engine.modules();
    const QList<ModuleInstance> projectModules = project->modules();
    bool runtimeMatchesProject = runtimeModules.size() == projectModules.size();
    for (int i = 0; runtimeMatchesProject && i < projectModules.size(); ++i) {
        runtimeMatchesProject = runtimeModules[i] && runtimeModules[i]->instanceName() == projectModules[i].id;
    }
    if (m_stepProjectId != project->id() || m_stepProjectFingerprint != fingerprint || !runtimeMatchesProject) {
        QString loadError;
        const QMetaObject::Connection errorConn = QObject::connect(
            &engine, &RunEngine::errorOccurred, &engine, [&loadError](const QString& error) { loadError = error; });
        const bool loaded = engine.loadProject(project);
        QObject::disconnect(errorConn);
        if (!loaded) {
            m_stepProjectId.clear();
            m_stepProjectFingerprint.clear();
            return makeToolError(loadError.isEmpty() ? QStringLiteral("Failed to load project") : loadError,
                                 QStringLiteral("INVALID_STATE"));
        }
        m_stepProjectId = project->id();
        m_stepProjectFingerprint = fingerprint;
    }

    if (engine.modules().isEmpty())
        return makeToolError(QStringLiteral("No modules to run"), QStringLiteral("INVALID_STATE"));
    const QStringList validationErrors = engine.validateFlow();
    if (!validationErrors.isEmpty())
        return makeToolError(validationErrors.join(QLatin1Char('\n')), QStringLiteral("FLOW_INVALID"));
    FlowExecutionService::JobRequest request;
    request.authorization = folderDeleteTargetAuthorization;
    if (!FlowExecutionService::instance().stepOnce(&request))
        return makeToolError(QStringLiteral("Single-step execution failed"), QStringLiteral("STEP_FAILED"), true);
    establishUndoBarrier();
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("started")},
                       {QStringLiteral("runId"), request.runId},
                       {QStringLiteral("async"), true}};
}

QJsonObject AgentActor::resetStep(const QJsonObject& params) {
    Q_UNUSED(params);
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy())
        return makeToolError(QStringLiteral("Flow is running; step position cannot be reset"),
                             QStringLiteral("ENGINE_BUSY"), true);
    engine.resetStepState();
    if (const Project* project = ProjectManager::instance().currentProject()) {
        m_stepProjectId = project->id();
        m_stepProjectFingerprint = QCryptographicHash::hash(
            QJsonDocument(project->toJson()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha1);
    } else {
        m_stepProjectId.clear();
        m_stepProjectFingerprint.clear();
    }
    return QJsonObject{{"status", "reset"}, {"nextStep", "flow_entry"}};
}

QJsonObject AgentActor::validateFlow(const QJsonObject& params) {
    Q_UNUSED(params);
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return makeToolError(QStringLiteral("No project opened"), QStringLiteral("NO_PROJECT"));

    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy())
        return makeToolError(QStringLiteral("Flow is running; validation is unavailable"),
                             QStringLiteral("ENGINE_BUSY"), true);

    // Validation is read-only: never request GUI synchronization here, because a sync can reset
    // shared outputs and single-step state. Out-of-sync runtimes fail closed below.
    const QStringList messages = engine.validateProjectFlow(project);
    if (messages.size() == 1 && messages.first().startsWith(QLatin1String("Flow is running")))
        return makeToolError(messages.first(), QStringLiteral("ENGINE_BUSY"), true);
    if (messages.size() == 1 && messages.first().startsWith(QLatin1String("Runtime modules are out of sync")))
        return makeToolError(messages.first(), QStringLiteral("ENGINE_OUT_OF_SYNC"), true);

    QJsonArray issues;
    const QRegularExpression requiredInput(QStringLiteral("^节点 (.+) 的必需输入端口 (.+) 无上游连接$"));
    const QRegularExpression disabledOutput(QStringLiteral("^禁用节点 (.+) 无法旁路输出端口 (.+)，但被下游依赖$"));
    const QRegularExpression incompatibleEdge(
        QStringLiteral("^Connection ([^\\s.]+)\\.([^\\s ]+) \\(([^)]+)\\) is incompatible with "
                       "([^\\s.]+)\\.([^\\s ]+) \\(([^)]+)\\)$"));
    const QRegularExpression endpoint(QStringLiteral("([^.\\s]+)\\.([^.\\s]+)"));
    for (const QString& message : messages) {
        QJsonObject issue{{QStringLiteral("message"), message}, {QStringLiteral("scope"), QStringLiteral("project")}};
        QRegularExpressionMatch match = requiredInput.match(message);
        if (!match.hasMatch())
            match = disabledOutput.match(message);
        if (match.hasMatch()) {
            issue.insert(QStringLiteral("moduleId"), match.captured(1));
            issue.insert(QStringLiteral("portId"), match.captured(2));
        } else if ((match = incompatibleEdge.match(message)).hasMatch()) {
            issue.insert(QStringLiteral("sourceModuleId"), match.captured(1));
            issue.insert(QStringLiteral("sourcePortId"), match.captured(2));
            issue.insert(QStringLiteral("targetModuleId"), match.captured(4));
            issue.insert(QStringLiteral("targetPortId"), match.captured(5));
        } else if (message.contains(QLatin1String(". ")) || message.contains(QLatin1Char('.'))) {
            match = endpoint.match(message);
            if (match.hasMatch()) {
                issue.insert(QStringLiteral("moduleId"), match.captured(1));
                issue.insert(QStringLiteral("portId"), match.captured(2));
            }
        }
        issues.append(issue);
    }

    return QJsonObject{
        {QStringLiteral("status"), messages.isEmpty() ? QStringLiteral("valid") : QStringLiteral("invalid")},
        {QStringLiteral("valid"), messages.isEmpty()},
        {QStringLiteral("errors"), issues}};
}

void AgentActor::noteRunEngineSynchronized(const Project* project) {
    if (!project) {
        m_stepProjectId.clear();
        m_stepProjectFingerprint.clear();
        m_engineProjectDirty = true;
        return;
    }
    m_engineProjectDirty = false;
    m_stepProjectId = project->id();
    m_stepProjectFingerprint = QCryptographicHash::hash(QJsonDocument(project->toJson()).toJson(QJsonDocument::Compact),
                                                        QCryptographicHash::Sha1);
}

QJsonObject AgentActor::stopFlow(const QJsonObject& params) {
    Q_UNUSED(params);
    FlowExecutionService& service = FlowExecutionService::instance();
    RunEngine& engine = RunEngine::instance();
    if (!service.isActive() && !engine.isRunning() && !engine.isPaused() && !engine.isExecuting())
        return toolNoOp(QStringLiteral("engine not running; nothing to stop"));
    if (service.isActive())
        service.stop();
    else
        engine.stop();
    return QJsonObject{{QStringLiteral("status"),
                        (engine.isExecuting() || service.state() == FlowExecutionService::State::Stopping)
                            ? QStringLiteral("stopping")
                            : QStringLiteral("stopped")}};
}

QJsonObject AgentActor::getFlowState(const QJsonObject& params) {
    Q_UNUSED(params);
    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));

    QJsonArray modules;
    const QList<ModuleInstance> instances = proj->modules();
    for (int index = 0; index < instances.size(); ++index) {
        const ModuleInstance& module = instances[index];
        const PluginInfo info = PluginManager::instance().pluginInfo(module.moduleId);
        QJsonObject value = module.toJson();
        value["order"] = index;
        value["ports"] =
            QJsonObject{{"inputs", portSpecsToJson(info.inputPorts)}, {"outputs", portSpecsToJson(info.outputPorts)}};
        modules.append(value);
    }

    QJsonArray connections;
    for (const ModuleConnection& c : proj->connections()) {
        QJsonObject obj = c.toJson();
        obj["from"] = c.fromModuleId;
        obj["to"] = c.toModuleId;
        obj["fromPort"] = c.fromPort;
        obj["toPort"] = c.toPort;
        obj["edgeType"] = c.edgeType;
        if (!c.edgeType.isEmpty()) {
            obj["effectiveEdgeType"] = c.edgeType;
            obj["edgeTypeResolution"] = QStringLiteral("explicit");
        } else {
            const std::optional<ModuleInstance> source = proj->moduleById(c.fromModuleId);
            const std::optional<ModuleInstance> target = proj->moduleById(c.toModuleId);
            const PluginInfo sourceInfo =
                source ? PluginManager::instance().pluginInfo(source->moduleId) : PluginInfo{};
            const PluginInfo targetInfo =
                target ? PluginManager::instance().pluginInfo(target->moduleId) : PluginInfo{};
            if (source && target && sourceInfo.portsDeclared && targetInfo.portsDeclared) {
                obj["effectiveEdgeType"] = RunEngine::inferEdgeType(c, sourceInfo.outputPorts, targetInfo.inputPorts);
                obj["edgeTypeResolution"] = QStringLiteral("inferred");
            } else {
                obj["edgeTypeResolution"] = QStringLiteral("unavailable");
            }
        }
        connections.append(obj);
    }

    QJsonArray flows;
    QList<ProjectFlow> storedFlows = proj->flows();
    if (storedFlows.isEmpty()) {
        ProjectFlow main;
        main.id = QStringLiteral("main");
        main.name = QStringLiteral("主流程");
        for (const ModuleInstance& module : instances)
            main.nodeIds.append(module.id);
        storedFlows.append(main);
    }
    for (const ProjectFlow& flow : storedFlows) {
        QJsonArray nodeIds;
        for (const QString& nodeId : flow.nodeIds)
            nodeIds.append(nodeId);
        flows.append(QJsonObject{{QStringLiteral("id"), flow.id},
                                 {QStringLiteral("name"), flow.name},
                                 {QStringLiteral("nodeIds"), nodeIds}});
    }

    return QJsonObject{{"project", QJsonObject{{"id", proj->id()},
                                               {"name", proj->name()},
                                               {"filePath", proj->filePath()},
                                               {"isModified", proj->isModified()}}},
                       {"executionScope", QStringLiteral("project_modules")},
                       {"flowSwitchSupported", false},
                       {"flows", flows},
                       {"modules", modules},
                       {"connections", connections}};
}

QJsonObject AgentActor::getAvailablePlugins(const QJsonObject& params) {
    Q_UNUSED(params);
    PluginManager& manager = PluginManager::instance();
    const QList<PluginInfo> moduleInfos = manager.moduleInfos();
    const QList<PluginInfo> cameraInfos = manager.cameraInfos();
    const bool managerThread = QThread::currentThread() == manager.thread();
    const bool engineBusy = RunEngine::instance().isBusy();
    QSet<QString> moduleNames;
    QSet<QString> cameraNames;
    for (const PluginInfo& info : moduleInfos)
        moduleNames.insert(info.name);
    for (const PluginInfo& info : cameraInfos)
        cameraNames.insert(info.name);

    auto withLifecycleActions = [&manager, managerThread, engineBusy, &moduleNames,
                                 &cameraNames](QJsonObject item, const PluginInfo& info, bool cameraPlugin) {
        QJsonArray executableActions;
        QJsonArray blockedActions;
        auto block = [&blockedActions](const QString& action, const QString& reason) {
            blockedActions.append(
                QJsonObject{{QStringLiteral("action"), action}, {QStringLiteral("reasonCode"), reason}});
        };
        const bool ambiguous = moduleNames.contains(info.name) && cameraNames.contains(info.name);
        if (ambiguous) {
            block(QStringLiteral("load"), QStringLiteral("PLUGIN_NAME_AMBIGUOUS"));
            block(QStringLiteral("unload"), QStringLiteral("PLUGIN_NAME_AMBIGUOUS"));
        } else if (cameraPlugin) {
            block(QStringLiteral("load"), QStringLiteral("PLUGIN_LIFECYCLE_UNSUPPORTED"));
            block(QStringLiteral("unload"), QStringLiteral("PLUGIN_LIFECYCLE_UNSUPPORTED"));
        } else {
            const bool loaded = item.value(QStringLiteral("loaded")).toBool();
            if (loaded) {
                block(QStringLiteral("load"), QStringLiteral("PLUGIN_ALREADY_LOADED"));
                QString unloadBlock = manager.unloadBlockReasonForPlugin(info.name);
                if (unloadBlock.isEmpty() && !managerThread)
                    unloadBlock = QStringLiteral("PLUGIN_THREAD_AFFINITY");
                if (unloadBlock.isEmpty() && engineBusy)
                    unloadBlock = QStringLiteral("ENGINE_BUSY");
                if (unloadBlock.isEmpty())
                    executableActions.append(QStringLiteral("unload"));
                else
                    block(QStringLiteral("unload"), unloadBlock);
            } else {
                block(QStringLiteral("unload"), QStringLiteral("PLUGIN_NOT_LOADED"));
                QString loadBlock;
                if (!managerThread)
                    loadBlock = QStringLiteral("PLUGIN_THREAD_AFFINITY");
                else if (engineBusy)
                    loadBlock = QStringLiteral("ENGINE_BUSY");
                else if (!item.value(QStringLiteral("installed")).toBool())
                    loadBlock = QStringLiteral("PLUGIN_BINARY_MISSING");
                if (loadBlock.isEmpty())
                    executableActions.append(QStringLiteral("load"));
                else
                    block(QStringLiteral("load"), loadBlock);
            }
        }
        item.insert(QStringLiteral("executableActions"), executableActions);
        item.insert(QStringLiteral("blockedActions"), blockedActions);
        return item;
    };

    QJsonArray plugins;
    QJsonArray items;
    for (const PluginInfo& info : moduleInfos) {
        plugins.append(info.name);
        items.append(withLifecycleActions(PluginManager::availabilityItem(info, manager.isPluginLoaded(info.name)),
                                          info, false));
    }
    QJsonArray cameras;
    for (const PluginInfo& info : cameraInfos) {
        cameras.append(
            withLifecycleActions(PluginManager::cameraAvailabilityItem(info, manager.isPluginLoaded(info.name),
                                                                       PluginManager::currentPlatformName()),
                                 info, true));
    }
    return QJsonObject{{"plugins", plugins}, {"items", items}, {"cameras", cameras}};
}

QJsonObject AgentActor::listCameras(const QJsonObject& params) {
    Q_UNUSED(params);
    auto cameraStateName = [](CameraState state) {
        switch (state) {
        case CameraState::Disconnected:
            return QStringLiteral("disconnected");
        case CameraState::Connecting:
            return QStringLiteral("connecting");
        case CameraState::Connected:
            return QStringLiteral("connected");
        case CameraState::Grabbing:
            return QStringLiteral("grabbing");
        case CameraState::Error:
            return QStringLiteral("error");
        }
        return QStringLiteral("unknown");
    };

    QJsonArray cameras;
    for (const CameraStatus& camera : CameraManager::instance().knownCameras()) {
        cameras.append(
            QJsonObject{{"deviceId", camera.deviceId},
                        {"name", camera.name},
                        {"state", cameraStateName(camera.state)},
                        {"lastGrabTime",
                         camera.lastGrabTime.isValid() ? camera.lastGrabTime.toString(Qt::ISODateWithMs) : QString()},
                        {"lastError", camera.lastError}});
    }

    return QJsonObject{
        {"discoveryPerformed", false}, {"source", QStringLiteral("CameraManager.knownCameras")}, {"cameras", cameras}};
}

QJsonObject AgentActor::listCommunications(const QJsonObject& params) {
    Q_UNUSED(params);
    CommunicationManager& manager = CommunicationManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Communication status must be queried on the manager thread"),
                             QStringLiteral("COMMUNICATION_THREAD_AFFINITY"));

    auto typeName = [](CommunicationType type) {
        switch (type) {
        case CommunicationType::TCP_Server:
            return QStringLiteral("tcp_server");
        case CommunicationType::TCP_Client:
            return QStringLiteral("tcp_client");
        case CommunicationType::SerialPort:
            return QStringLiteral("serial_port");
        case CommunicationType::PLC:
            return QStringLiteral("plc");
        }
        return QStringLiteral("unknown");
    };
    auto stateName = [](CommunicationState state) {
        switch (state) {
        case CommunicationState::Disconnected:
            return QStringLiteral("disconnected");
        case CommunicationState::Connecting:
            return QStringLiteral("connecting");
        case CommunicationState::Connected:
            return QStringLiteral("connected");
        case CommunicationState::Listening:
            return QStringLiteral("listening");
        case CommunicationState::Error:
            return QStringLiteral("error");
        }
        return QStringLiteral("unknown");
    };

    QJsonArray communications;
    for (const CommunicationConfig& config : manager.configs()) {
        communications.append(QJsonObject{{QStringLiteral("id"), config.id},
                                          {QStringLiteral("name"), config.name},
                                          {QStringLiteral("type"), typeName(config.type)},
                                          {QStringLiteral("state"), stateName(manager.state(config.id))}});
    }
    return QJsonObject{{QStringLiteral("source"), QStringLiteral("CommunicationManager.configs")},
                       {QStringLiteral("communications"), communications}};
}

QJsonObject AgentActor::getCameraDiscoveryTargets(const QJsonObject& params) {
    Q_UNUSED(params);
    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Camera discovery targets must be read on the CameraManager thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    return manager.cameraDiscoveryTargets();
}

QJsonObject AgentActor::discoverCameras(const QJsonObject& params) {
    bool generationOk = false;
    const quint64 generation =
        params.value(QStringLiteral("registryGeneration")).toString().toULongLong(&generationOk, 10);
    if (!generationOk)
        return makeToolError(QStringLiteral("registryGeneration is invalid"), QStringLiteral("INVALID_PARAM"));
    QStringList pluginIds;
    for (const QJsonValue& value : params.value(QStringLiteral("pluginIds")).toArray())
        pluginIds.append(value.toString());
    QStringList unsupportedPluginIds;
    for (const QJsonValue& value : params.value(QStringLiteral("unsupportedPluginIds")).toArray())
        unsupportedPluginIds.append(value.toString());

    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Camera discovery must start on the CameraManager thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    quint64 requestId = 0;
    QString error;
    if (!manager.requestCameraDiscoveryBatchAsync(generation, pluginIds, unsupportedPluginIds, &requestId, &error)) {
        const QString message = error == QLatin1String("ENGINE_BUSY")
                                    ? QStringLiteral("Cannot start camera discovery while the flow engine is busy")
                                    : QStringLiteral("Camera discovery batch was not accepted");
        return makeToolError(message, error.isEmpty() ? QStringLiteral("CAMERA_DISCOVERY_START_FAILED") : error,
                             error == QLatin1String("ENGINE_BUSY") || error == QLatin1String("CAMERA_DISCOVERY_BUSY"));
    }
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")},
                       {QStringLiteral("operation"), QStringLiteral("discover_cameras")},
                       {QStringLiteral("requestId"), QString::number(requestId)}};
}

QJsonObject AgentActor::getCameraDiscoveryOperation(const QJsonObject& params) {
    const QString text = params.value(QStringLiteral("requestId")).toString();
    bool requestIdOk = false;
    const quint64 requestId = text.toULongLong(&requestIdOk, 10);
    if (!QRegularExpression(QStringLiteral("^[1-9][0-9]*$")).match(text).hasMatch() || !requestIdOk ||
        QString::number(requestId) != text)
        return makeToolError(QStringLiteral("requestId must be a canonical nonzero decimal string"),
                             QStringLiteral("INVALID_PARAM"));

    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Camera discovery results must be read on the CameraManager thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    QJsonObject result;
    if (!manager.getCameraDiscoveryOperationResult(requestId, &result))
        return makeToolError(QStringLiteral("Camera discovery operation was not found or its result has expired"),
                             QStringLiteral("CAMERA_DISCOVERY_RESULT_NOT_FOUND"));
    return result;
}

QJsonObject AgentActor::getCameraDetails(const QJsonObject& params) {
    const QString deviceId = params.value(QStringLiteral("deviceId")).toString().trimmed();
    if (deviceId.isEmpty())
        return makeToolError(QStringLiteral("deviceId is required"), QStringLiteral("INVALID_PARAM"));

    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Camera details must be queried on the CameraManager thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));

    CameraLease lease = manager.acquireCamera(deviceId);
    if (!lease) {
        const QList<CameraStatus> known = manager.knownCameras();
        const auto status = std::find_if(known.cbegin(), known.cend(),
                                         [&deviceId](const CameraStatus& value) { return value.deviceId == deviceId; });
        if (status == known.cend())
            return makeToolError(QStringLiteral("Camera was not discovered"), QStringLiteral("CAMERA_NOT_FOUND"));
        if (manager.hasPendingCameraOperation(deviceId))
            return makeToolError(QStringLiteral("A camera operation is already pending for this device"),
                                 QStringLiteral("CAMERA_OPERATION_BUSY"), true);
        if (manager.supportsAsyncOperations(deviceId) &&
            (status->state == CameraState::Connected || status->state == CameraState::Grabbing)) {
            quint64 requestId = 0;
            QString error;
            if (!manager.requestCameraDetailsAsync(deviceId, &requestId, &error))
                return makeToolError(QStringLiteral("Failed to query camera details"), error);
            return QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")},
                               {QStringLiteral("deviceId"), deviceId},
                               {QStringLiteral("requestId"), QString::number(requestId)}};
        }
        return makeToolError(QStringLiteral("Camera is not connected"), QStringLiteral("CAMERA_NOT_CONNECTED"));
    }

    ICamera* camera = lease.get();
    if (QThread::currentThread() != camera->thread())
        return makeToolError(QStringLiteral("Camera details must be queried on the camera object's thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    if (!camera->isConnected())
        return makeToolError(QStringLiteral("Camera is not connected"), QStringLiteral("CAMERA_NOT_CONNECTED"));

    const CameraCapabilities capabilities = camera->capabilities();
    auto triggerModeName = [](TriggerMode mode) {
        switch (mode) {
        case TriggerMode::Software:
            return QStringLiteral("software");
        case TriggerMode::Hardware:
            return QStringLiteral("hardware");
        case TriggerMode::Continuous:
            return QStringLiteral("continuous");
        }
        return QStringLiteral("unknown");
    };
    const QRect roi = camera->roi();

    QJsonObject details{
        {"deviceId", camera->deviceId()},
        {"name", camera->name()},
        {"serialNumber", camera->serialNumber()},
        {"manufacturer", camera->manufacturer()},
        {"capabilities", QJsonObject{{"softwareTrigger", capabilities.softwareTrigger},
                                     {"hardwareTrigger", capabilities.hardwareTrigger},
                                     {"continuousMode", capabilities.continuousMode},
                                     {"exposureControl", capabilities.exposureControl},
                                     {"gainControl", capabilities.gainControl},
                                     {"frameRateControl", capabilities.frameRateControl},
                                     {"roiControl", capabilities.roiControl},
                                     {"pixelFormatControl", capabilities.pixelFormatControl},
                                     {"whiteBalance", capabilities.whiteBalance},
                                     {"depthData", capabilities.depthData},
                                     {"supportedOnWindows", capabilities.supportedOnWindows},
                                     {"supportedOnLinux", capabilities.supportedOnLinux}}},
        {"parameters",
         QJsonObject{
             {"triggerMode", triggerModeName(camera->triggerMode())},
             {"exposureTime", camera->exposureTime()},
             {"gain", camera->gain()},
             {"frameRate", camera->frameRate()},
             {"roi", QJsonObject{{"x", roi.x()}, {"y", roi.y()}, {"width", roi.width()}, {"height", roi.height()}}},
             {"imageWidth", camera->imageWidth()},
             {"imageHeight", camera->imageHeight()}}}};
    details.insert(QStringLiteral("cameraSessionGeneration"),
                   QString::number(manager.cameraParameterBindingVersion(deviceId)));
    return details;
}

QJsonObject AgentActor::setCameraParameter(const QJsonObject& params) {
    static const QSet<QString> allowedKeys{QStringLiteral("deviceId"), QStringLiteral("cameraSessionGeneration"),
                                           QStringLiteral("serialNumber"), QStringLiteral("parameter"),
                                           QStringLiteral("value")};
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) {
        if (!allowedKeys.contains(it.key()))
            return makeToolError(QStringLiteral("Unexpected parameter field: %1").arg(it.key()),
                                 QStringLiteral("INVALID_PARAM"));
    }
    const QString deviceId = params.value(QStringLiteral("deviceId")).toString();
    const QString serialNumber = params.value(QStringLiteral("serialNumber")).toString();
    const QString parameter = params.value(QStringLiteral("parameter")).toString();
    const QJsonValue value = params.value(QStringLiteral("value"));
    const QString generationText = params.value(QStringLiteral("cameraSessionGeneration")).toString();
    static const QRegularExpression canonicalGeneration(QStringLiteral("^(0|[1-9][0-9]*)$"));
    bool generationOk = false;
    const quint64 expectedGeneration = generationText.toULongLong(&generationOk, 10);
    if (deviceId.isEmpty() || deviceId != deviceId.trimmed() || !canonicalGeneration.match(generationText).hasMatch() ||
        !generationOk || QString::number(expectedGeneration) != generationText)
        return makeToolError(QStringLiteral("Camera identity and canonical binding generation are required"),
                             QStringLiteral("INVALID_PARAM"));

    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Camera parameter changes must run on the CameraManager thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    if (RunEngine::instance().isBusy())
        return makeToolError(QStringLiteral("Cannot change camera parameters while the flow engine is busy"),
                             QStringLiteral("ENGINE_BUSY"), true);
    const QList<CameraStatus> known = manager.knownCameras();
    const bool discovered = std::any_of(
        known.cbegin(), known.cend(), [&deviceId](const CameraStatus& status) { return status.deviceId == deviceId; });
    if (!discovered)
        return makeToolError(QStringLiteral("Camera was not discovered"), QStringLiteral("CAMERA_NOT_FOUND"));
    if (manager.hasPendingCameraOperation(deviceId))
        return makeToolError(QStringLiteral("A camera operation is already pending for this device"),
                             QStringLiteral("CAMERA_OPERATION_BUSY"), true);
    if (manager.cameraParameterBindingVersion(deviceId) != expectedGeneration)
        return makeToolError(QStringLiteral("Camera details changed after this write was prepared"),
                             QStringLiteral("CAMERA_TARGET_CHANGED"));

    if (manager.supportsAsyncOperations(deviceId) && !manager.getCamera(deviceId)) {
        quint64 requestId = 0;
        QString error;
        if (!manager.requestCameraParameterWriteAsync(deviceId, expectedGeneration, serialNumber, parameter, value,
                                                      &requestId, &error))
            return makeToolError(QStringLiteral("Failed to start camera parameter write"), error);
        return QJsonObject{{QStringLiteral("status"), QStringLiteral("pending")},
                           {QStringLiteral("deviceId"), deviceId},
                           {QStringLiteral("operation"), QStringLiteral("set_parameter")},
                           {QStringLiteral("requestId"), QString::number(requestId)}};
    }

    CameraLease lease = manager.acquireCamera(deviceId);
    if (!lease)
        return makeToolError(QStringLiteral("Camera is not connected"), QStringLiteral("CAMERA_NOT_CONNECTED"));
    ICamera* camera = lease.get();
    if (QThread::currentThread() != camera->thread())
        return makeToolError(QStringLiteral("Camera parameter changes must run on the camera object's thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    if (camera->deviceId() != deviceId || camera->serialNumber() != serialNumber ||
        manager.cameraParameterBindingVersion(deviceId) != expectedGeneration)
        return makeToolError(QStringLiteral("Camera target changed after confirmation"),
                             QStringLiteral("CAMERA_TARGET_CHANGED"));

    const CameraParameterWriteResult write =
        manager.writeCameraParameterIfCurrent(deviceId, expectedGeneration, camera, serialNumber, parameter, value);
    QJsonObject result{{QStringLiteral("deviceId"), deviceId},
                       {QStringLiteral("parameter"), parameter},
                       {QStringLiteral("requested"), value},
                       {QStringLiteral("sideEffectStatus"), write.sideEffectStatus},
                       {QStringLiteral("retryable"), false}};
    if (!write.before.isUndefined())
        result.insert(QStringLiteral("before"), write.before);
    if (!write.actual.isUndefined())
        result.insert(QStringLiteral("actual"), write.actual);
    const quint64 resultGeneration = manager.cameraParameterBindingVersion(deviceId);
    result.insert(QStringLiteral("cameraSessionGeneration"), QString::number(resultGeneration));
    if (write.status != QLatin1String("success")) {
        result.insert(QStringLiteral("error"), write.errorCode);
        result.insert(QStringLiteral("errorCode"), write.errorCode);
        return result;
    }
    result.insert(QStringLiteral("status"), QStringLiteral("success"));
    return result;
}

QJsonObject AgentActor::getCameraOperation(const QJsonObject& params) {
    const QString text = params.value(QStringLiteral("requestId")).toString();
    static const QRegularExpression decimal(QStringLiteral("^[0-9]+$"));
    if (!decimal.match(text).hasMatch())
        return makeToolError(QStringLiteral("requestId must be a canonical unsigned decimal string"),
                             QStringLiteral("INVALID_PARAM"));
    bool ok = false;
    const qulonglong parsed = text.toULongLong(&ok, 10);
    if (!ok || parsed == 0 || QString::number(parsed) != text)
        return makeToolError(QStringLiteral("requestId must be a canonical nonzero decimal string"),
                             QStringLiteral("INVALID_PARAM"));

    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Camera operation results must be queried on the CameraManager thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    QJsonObject result;
    if (!manager.getCameraOperationResult(static_cast<quint64>(parsed), &result))
        return makeToolError(QStringLiteral("Camera operation was not found or its result has expired"),
                             QStringLiteral("CAMERA_OPERATION_NOT_FOUND"));
    if (result.value(QStringLiteral("operation")).toString() == QLatin1String("disconnect"))
        result.insert(QStringLiteral("hardwareDisconnectConfirmed"),
                      result.value(QStringLiteral("status")).toString() == QLatin1String("success"));
    return result;
}

namespace {
QString pluginLifecycleTargetError(const PluginManager& manager, const QString& name) {
    const QList<PluginInfo> moduleInfos = manager.moduleInfos();
    const bool moduleFound = std::any_of(moduleInfos.cbegin(), moduleInfos.cend(),
                                         [&name](const PluginInfo& info) { return info.name == name; });
    const QList<PluginInfo> cameraInfos = manager.cameraInfos();
    const bool cameraFound = std::any_of(cameraInfos.cbegin(), cameraInfos.cend(),
                                         [&name](const PluginInfo& info) { return info.name == name; });
    if (moduleFound && cameraFound)
        return QStringLiteral("PLUGIN_NAME_AMBIGUOUS");
    if (cameraFound)
        return QStringLiteral("PLUGIN_LIFECYCLE_UNSUPPORTED");
    return moduleFound ? QString() : QStringLiteral("PLUGIN_NOT_FOUND");
}
} // namespace

QJsonObject AgentActor::loadModulePlugin(const QJsonObject& params) {
    const QString name = params.value(QStringLiteral("plugin")).toString();
    if (name != name.trimmed())
        return makeToolError(QStringLiteral("plugin name must not contain leading or trailing whitespace"),
                             QStringLiteral("INVALID_PARAM"));
    PluginManager& manager = PluginManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Plugin lifecycle operations must run on the PluginManager thread"),
                             QStringLiteral("PLUGIN_THREAD_AFFINITY"));
    if (RunEngine::instance().isBusy())
        return makeToolError(QStringLiteral("Cannot load a plugin while the flow engine is busy"),
                             QStringLiteral("ENGINE_BUSY"), true);

    const QString targetError = pluginLifecycleTargetError(manager, name);
    if (!targetError.isEmpty())
        return makeToolError(QStringLiteral("Plugin target is not a unique scanned module: %1").arg(name), targetError);
    if (manager.isPluginLoaded(name))
        return QJsonObject{{"status", QStringLiteral("already_loaded")}, {"plugin", name}};

    QString loadError;
    const QMetaObject::Connection failureConnection = connect(
        &manager, &PluginManager::pluginLoadFailed, &manager,
        [&name, &loadError](const QString& failedName, const QString& reason) {
            if (failedName == name)
                loadError = reason;
        },
        Qt::DirectConnection);
    const bool loaded = manager.loadPlugin(name);
    QObject::disconnect(failureConnection);
    if (!loaded) {
        QJsonObject details;
        if (!loadError.isEmpty())
            details.insert(QStringLiteral("reason"), loadError);
        return makeToolError(QStringLiteral("Failed to load module plugin: %1").arg(name),
                             QStringLiteral("PLUGIN_LOAD_FAILED"), false, details);
    }
    return QJsonObject{{"status", QStringLiteral("loaded")}, {"plugin", name}};
}

QJsonObject AgentActor::unloadModulePlugin(const QJsonObject& params) {
    const QString name = params.value(QStringLiteral("plugin")).toString();
    if (name != name.trimmed())
        return makeToolError(QStringLiteral("plugin name must not contain leading or trailing whitespace"),
                             QStringLiteral("INVALID_PARAM"));
    PluginManager& manager = PluginManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Plugin lifecycle operations must run on the PluginManager thread"),
                             QStringLiteral("PLUGIN_THREAD_AFFINITY"));
    if (RunEngine::instance().isBusy())
        return makeToolError(QStringLiteral("Cannot unload a plugin while the flow engine is busy"),
                             QStringLiteral("ENGINE_BUSY"), true);

    const QString targetError = pluginLifecycleTargetError(manager, name);
    if (!targetError.isEmpty())
        return makeToolError(QStringLiteral("Plugin target is not a unique scanned module: %1").arg(name), targetError);
    if (!manager.isPluginLoaded(name))
        return makeToolError(QStringLiteral("Module plugin is not loaded: %1").arg(name),
                             QStringLiteral("PLUGIN_NOT_LOADED"));

    QString unloadError;
    if (!manager.tryUnloadPlugin(name, &unloadError))
        return makeToolError(QStringLiteral("Failed to unload module plugin: %1").arg(name), unloadError);
    return QJsonObject{{"status", QStringLiteral("unloaded")}, {"plugin", name}};
}

QJsonObject AgentActor::connectCamera(const QJsonObject& params, const QJsonObject& cameraTargetSnapshot) {
    const QString deviceId = params.value(QStringLiteral("deviceId")).toString();
    if (deviceId != deviceId.trimmed())
        return makeToolError(QStringLiteral("deviceId must not contain leading or trailing whitespace"),
                             QStringLiteral("INVALID_PARAM"));

    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return makeToolError(QStringLiteral("Camera lifecycle operations must run on the CameraManager thread"),
                             QStringLiteral("CAMERA_THREAD_AFFINITY"));
    if (RunEngine::instance().isBusy() && !RunEngine::instance().isPaused())
        return makeToolError(QStringLiteral("Cannot connect a camera while the flow engine is running"),
                             QStringLiteral("ENGINE_BUSY"), true);

    const QList<CameraStatus> known = manager.knownCameras();
    const bool discovered = std::any_of(
        known.cbegin(), known.cend(), [&deviceId](const CameraStatus& status) { return status.deviceId == deviceId; });
    if (!discovered)
        return makeToolError(QStringLiteral("Camera was not discovered: %1").arg(deviceId),
                             QStringLiteral("CAMERA_NOT_FOUND"));
    if (manager.hasPendingCameraOperation(deviceId))
        return makeToolError(QStringLiteral("A camera operation is already pending for this device"),
                             QStringLiteral("CAMERA_OPERATION_BUSY"), true);
    if (manager.isConnected(deviceId))
        return QJsonObject{{"status", QStringLiteral("already_connected")},
                           {"deviceId", deviceId},
                           {"connectionReportedByDriver", true}};
    if (manager.getCamera(deviceId) || !manager.supportsAsyncOperations(deviceId))
        return makeToolError(
            QStringLiteral("This camera uses the legacy synchronous session path; use Camera Settings"),
            QStringLiteral("CAMERA_SESSION_REQUIRED"));

    quint64 requestId = 0;
    QString error;
    const bool accepted = cameraTargetSnapshot.isEmpty()
                              ? manager.requestConnectCameraAsync(deviceId, &requestId, &error)
                              : manager.requestConnectCameraAsync(deviceId, cameraTargetSnapshot, &requestId, &error);
    if (!accepted)
        return makeToolError(QStringLiteral("Failed to start camera connection: %1").arg(deviceId), error);
    return QJsonObject{{"status", QStringLiteral("pending")},
                       {"deviceId", deviceId},
                       {"requestId", QString::number(requestId)},
                       {"connectionReportedByDriver", false}};
}

QJsonObject AgentActor::disconnectCamera(const QJsonObject& params, const QJsonObject& cameraTargetSnapshot) {
    const auto disconnectError = [](const QString& message, const QString& errorCode, bool retryable = false) {
        return makeToolError(message, errorCode, retryable, QJsonObject{{"hardwareDisconnectConfirmed", false}});
    };
    const QString deviceId = params.value(QStringLiteral("deviceId")).toString();
    if (deviceId != deviceId.trimmed())
        return disconnectError(QStringLiteral("deviceId must not contain leading or trailing whitespace"),
                               QStringLiteral("INVALID_PARAM"));

    CameraManager& manager = CameraManager::instance();
    if (QThread::currentThread() != manager.thread())
        return disconnectError(QStringLiteral("Camera lifecycle operations must run on the CameraManager thread"),
                               QStringLiteral("CAMERA_THREAD_AFFINITY"));
    if (RunEngine::instance().isBusy())
        return disconnectError(QStringLiteral("Cannot disconnect a camera while the flow engine is busy"),
                               QStringLiteral("ENGINE_BUSY"), true);

    const QList<CameraStatus> known = manager.knownCameras();
    const bool discovered = std::any_of(
        known.cbegin(), known.cend(), [&deviceId](const CameraStatus& status) { return status.deviceId == deviceId; });
    if (!discovered)
        return disconnectError(QStringLiteral("Camera was not discovered: %1").arg(deviceId),
                               QStringLiteral("CAMERA_NOT_FOUND"));
    if (manager.hasPendingCameraOperation(deviceId))
        return disconnectError(QStringLiteral("A camera operation is already pending for this device"),
                               QStringLiteral("CAMERA_OPERATION_BUSY"), true);

    const CameraState cameraState = manager.cameraState(deviceId);
    if (!manager.isConnected(deviceId) && cameraState != CameraState::Error)
        return QJsonObject{{"status", QStringLiteral("already_disconnected")},
                           {"deviceId", deviceId},
                           {"hardwareDisconnectConfirmed", false}};
    if (manager.getCamera(deviceId) || !manager.supportsAsyncOperations(deviceId))
        return disconnectError(
            QStringLiteral("This camera uses the legacy synchronous session path; use Camera Settings"),
            QStringLiteral("CAMERA_SESSION_REQUIRED"));

    quint64 requestId = 0;
    QString error;
    const bool accepted =
        cameraTargetSnapshot.isEmpty()
            ? manager.requestDisconnectCameraAsync(deviceId, &requestId, &error)
            : manager.requestDisconnectCameraAsync(deviceId, cameraTargetSnapshot, &requestId, &error);
    if (!accepted)
        return disconnectError(QStringLiteral("Failed to start camera disconnection: %1").arg(deviceId), error);
    return QJsonObject{{"status", QStringLiteral("pending")},
                       {"deviceId", deviceId},
                       {"requestId", QString::number(requestId)},
                       {"hardwareDisconnectConfirmed", false}};
}

QJsonObject AgentActor::saveProject(const QJsonObject& params) {
    QString path = params.value("path").toString();
    // 阶段1 §1（F06）：失败必须携带 error+errorCode——控制器与审计以 error 键判定成败，
    // 仅 status:failed 会被记为假成功。保留 status 字段兼容既有调用方（Qt5 的
    // QJsonObject 无 operator|，以 insert 合并）。
    if (!ProjectManager::instance().currentProject()) {
        QJsonObject e = makeToolError(QStringLiteral("No project opened"), QStringLiteral("NO_PROJECT"));
        e.insert(QStringLiteral("status"), QStringLiteral("failed"));
        return e;
    }
    const Project* project = ProjectManager::instance().currentProject();
    if (!path.isEmpty() || (project && !project->filePath().isEmpty()))
        establishUndoBarrier();
    const bool ok = ProjectManager::instance().saveProject(path);
    if (ok)
        return QJsonObject{{"status", QStringLiteral("saved")}};
    QJsonObject e =
        makeToolError(path.isEmpty() ? QStringLiteral("Save failed: no path specified and project has never been saved")
                                     : QStringLiteral("Save failed: %1").arg(path),
                      QStringLiteral("SAVE_FAILED"));
    e.insert(QStringLiteral("status"), QStringLiteral("failed"));
    return e;
}

QJsonObject AgentActor::getModuleParamsSchema(const QJsonObject& params) {
    const QString instanceId = params.value(QStringLiteral("instanceId")).toString();
    const QString plugin = params.value(QStringLiteral("plugin")).toString();
    if (instanceId.isEmpty() == plugin.isEmpty())
        return makeToolError("Specify exactly one of 'instanceId' or 'plugin'",
                             instanceId.isEmpty() ? QStringLiteral("MISSING_PARAM") : QStringLiteral("INVALID_PARAM"));
    PluginManager& pm = PluginManager::instance();
    std::optional<ModuleInstance> instance;
    QString moduleId = plugin;
    if (!instanceId.isEmpty()) {
        Project* project = ProjectManager::instance().currentProject();
        if (!project)
            return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
        instance = project->moduleById(instanceId);
        if (!instance)
            return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));
        moduleId = instance->moduleId;
    }
    const PluginInfo info = pm.pluginInfo(moduleId);
    if (info.name.isEmpty())
        return makeToolError(QString("Plugin not found: %1").arg(moduleId), QStringLiteral("PLUGIN_NOT_FOUND"));
    const QString cacheKey = info.id.isEmpty() ? info.name : info.id;
    QJsonObject schema = m_schemaCache.value(cacheKey);
    if (schema.isEmpty()) {
        PluginManager::ModulePtr mod = pm.createManagedModule(info.name);
        if (!mod)
            return QJsonObject{
                {"moduleId", info.name},
                {"pluginId", info.id},
                {"version", info.version},
                {"category", info.category},
                {"name", info.name},
                {"description", info.description},
                {"loaded", info.loaded},
                {"available", false},
                {"availabilityReason", info.error.isEmpty() ? QStringLiteral("Plugin is not loaded") : info.error},
                {"parameters", info.ui.value(QStringLiteral("parameters")).toObject()},
                {"ports", QJsonObject{{"inputs", portSpecsToJson(info.inputPorts)},
                                      {"outputs", portSpecsToJson(info.outputPorts)}}},
                {"results", info.ui.value(QStringLiteral("results")).toObject()}};
        const QJsonObject defaults = mod->defaultParams();
        const QJsonObject uiParams = info.ui.value(QStringLiteral("parameters")).toObject();
        QJsonObject descriptors;
        QStringList keys = defaults.keys();
        for (const QString& key : uiParams.keys()) {
            if (!keys.contains(key))
                keys.append(key);
        }
        for (const QString& key : keys) {
            QJsonObject descriptor = uiParams.value(key).toObject();
            const QJsonValue value = defaults.value(key);
            const char* kind = value.isBool()     ? "boolean"
                               : value.isDouble() ? "number"
                               : value.isString() ? "string"
                               : value.isArray()  ? "array"
                               : value.isObject() ? "object"
                               : value.isNull()   ? "null"
                                                  : "unknown";
            if (descriptor.value(QStringLiteral("type")).toString().isEmpty())
                descriptor["type"] = kind;
            if (!value.isUndefined())
                descriptor["default"] = value;
            descriptor["editable"] = !descriptor.value(QStringLiteral("readOnly")).toBool();
            descriptor["nullable"] = value.isNull() || descriptor.value(QStringLiteral("nullable")).toBool();
            if (value.isArray() && !descriptor.contains(QStringLiteral("items"))) {
                const QJsonArray items = value.toArray();
                descriptor["items"] = items.isEmpty()
                                          ? QJsonObject{{"type", "unknown"}, {"note", "item type not declared"}}
                                          : QJsonObject{{"type", items.first().isDouble() ? "number" : "unknown"}};
            }
            QJsonArray options = descriptor.value(QStringLiteral("options")).toArray();
            if (options.isEmpty())
                options = descriptor.value(QStringLiteral("_options")).toArray();
            if (!options.isEmpty()) {
                QJsonArray enums;
                for (const QJsonValue& option : options)
                    enums.append(option.toObject().value(QStringLiteral("value")));
                descriptor["enum"] = enums;
                descriptor["options"] = options;
            }
            descriptors[key] = descriptor;
        }
        schema["moduleId"] = info.name;
        schema["pluginId"] = info.id;
        schema["version"] = info.version;
        schema["category"] = info.category;
        schema["loaded"] = info.loaded;
        schema["available"] = true;
        schema["name"] = mod->name();
        schema["description"] = mod->description();
        schema["defaultParams"] = defaults;
        schema["parameters"] = descriptors;
        schema["ports"] =
            QJsonObject{{"inputs", portSpecsToJson(info.inputPorts)}, {"outputs", portSpecsToJson(info.outputPorts)}};
        schema["results"] = info.ui.value(QStringLiteral("results")).toObject();
        m_schemaCache[cacheKey] = schema;
    }
    const QJsonObject current = instance ? instance->params : QJsonObject();
    QJsonObject effective = schema.value(QStringLiteral("defaultParams")).toObject();
    for (auto it = current.begin(); it != current.end(); ++it)
        effective.insert(it.key(), it.value());
    const QJsonObject staticDescriptors = schema.value(QStringLiteral("parameters")).toObject();
    QJsonObject descriptors = staticDescriptors;
    for (auto it = staticDescriptors.begin(); it != staticDescriptors.end(); ++it) {
        QJsonObject descriptor = it.value().toObject();
        const QString key = it.key();
        descriptor["hasOverride"] = current.contains(key);
        if (current.contains(key))
            descriptor["current"] = current.value(key);
        descriptor["effective"] = effective.value(key);
        const QString editor = descriptor.value(QStringLiteral("editor")).toString();
        if (editor == QStringLiteral("camera")) {
            QJsonArray cameras;
            bool selectedAvailable = false;
            const QString selected = effective.value(key).toString();
            for (const CameraStatus& camera : CameraManager::instance().knownCameras()) {
                cameras.append(QJsonObject{{"value", camera.deviceId}, {"label", camera.name}});
                selectedAvailable |= camera.deviceId == selected;
            }
            descriptor["dynamicOptions"] = cameras;
            descriptor["optionsSource"] = QStringLiteral("CameraManager.knownCameras");
            descriptor["resourceStatus"] = cameras.isEmpty()    ? QStringLiteral("unavailable")
                                           : selected.isEmpty() ? QStringLiteral("unconfigured")
                                           : selectedAvailable  ? QStringLiteral("available")
                                                                : QStringLiteral("missing");
        } else if (editor == QStringLiteral("file") || editor == QStringLiteral("folder") ||
                   editor == QStringLiteral("fileOrFolder")) {
            const QString path = effective.value(key).toString();
            descriptor["resourceStatus"] = path.isEmpty()            ? QStringLiteral("unconfigured")
                                           : QFileInfo::exists(path) ? QStringLiteral("available")
                                                                     : QStringLiteral("missing");
        }
        descriptors[key] = descriptor;
    }
    schema["parameters"] = descriptors;
    schema["currentParams"] = current;
    schema["effectiveParams"] = effective;
    const QJsonObject resultDescriptors = schema.value(QStringLiteral("results")).toObject();
    QJsonObject results;
    for (auto it = resultDescriptors.begin(); it != resultDescriptors.end(); ++it) {
        const QJsonObject result = it.value().toObject();
        const QString keyParameter = result.value(QStringLiteral("keyFromParameter")).toString();
        if (keyParameter.isEmpty()) {
            results.insert(it.key(), result);
            continue;
        }
        const QString resultKey = effective.value(keyParameter).toString();
        if (!resultKey.isEmpty())
            results.insert(resultKey, result);
    }
    schema["results"] = results;
    if (instance)
        schema["instanceId"] = instanceId;
    return schema;
}

QJsonObject AgentActor::getRunResults(const QJsonObject& params) {
    RunEngine& engine = RunEngine::instance();
    const QString requestedRunId = params.value(QStringLiteral("runId")).toString();
    if (requestedRunId.size() > 256)
        return makeToolError(QStringLiteral("runId exceeds 256 characters"), QStringLiteral("INVALID_PARAM"));
    const RunObservation run = engine.runObservation(requestedRunId);
    if (!requestedRunId.isEmpty() && run.runId.isEmpty())
        return makeToolError(QStringLiteral("Run not found: %1").arg(requestedRunId), QStringLiteral("RUN_NOT_FOUND"));
    QJsonObject result;
    result["totalRuns"] = engine.totalRuns();
    result["successRuns"] = engine.successRuns();
    result["failedRuns"] = engine.failedRuns();
    result["lastElapsedMs"] = engine.lastElapsedMs();
    result["isRunning"] = engine.isRunning();
    // 阶段 3（G3）：运行身份与状态——不再仅返回累计统计
    result["lastRunId"] = engine.lastRunId();
    const bool stale = !run.runId.isEmpty() && (run.project != ProjectManager::instance().currentProject() ||
                                                run.projectFingerprint != projectFingerprint(run.project));
    result["stale"] = stale;
    QString status = QStringLiteral("idle");
    if (!run.runId.isEmpty()) {
        result["runId"] = run.runId;
        if (!run.sessionId.isEmpty())
            result["sessionId"] = run.sessionId;
        if (run.runId == run.sessionId) {
            result["maxCycles"] = run.maxCycles;
            result["completedCycles"] = run.completedCycles;
            if (!run.lastSuccessfulRunId.isEmpty())
                result["lastSuccessfulRunId"] = run.lastSuccessfulRunId;
        }
        if (!run.lastIterationRunId.isEmpty())
            result["lastIterationRunId"] = run.lastIterationRunId;
        if (!run.lastCompletedRunId.isEmpty())
            result["lastCompletedRunId"] = run.lastCompletedRunId;
        result["isBusy"] = !run.finished;
        result["startedTime"] = run.startedTime.toString(Qt::ISODateWithMs);
        if (!run.currentModule.isEmpty())
            result["currentModule"] = run.currentModule;
        if (run.finished) {
            const bool sideEffectUncertain = run.result.errorCode == ExecError::SideEffectUncertain;
            status = sideEffectUncertain
                         ? QStringLiteral("uncertain")
                         : (run.state == RunState::Stopped
                                ? QStringLiteral("cancelled")
                                : (run.result.success ? QStringLiteral("completed") : QStringLiteral("failed")));
            if (sideEffectUncertain) {
                result["sideEffectStatus"] = QStringLiteral("uncertain");
                result["retryable"] = false;
            }
            result["finishedTime"] = run.result.finishedTime.toString(Qt::ISODateWithMs);
            result["elapsedMs"] = run.result.elapsedMs;
        } else {
            status = run.state == RunState::Paused
                         ? QStringLiteral("paused")
                         : (run.state == RunState::Stopped ? QStringLiteral("stopping") : QStringLiteral("running"));
        }
    }
    result["status"] = status;
    if (run.finished && !run.result.errorMessage.isEmpty()) {
        QJsonObject err;
        err["message"] = run.result.errorMessage.left(8192);
        err["code"] = run.result.errorCode;
        if (run.result.errorCode == ExecError::ConfirmedTargetChanged)
            err["codeName"] = QStringLiteral("CONFIRMED_TARGET_CHANGED");
        if (run.result.errorCode == ExecError::SideEffectUncertain) {
            err["sideEffectStatus"] = QStringLiteral("uncertain");
            err["retryable"] = false;
        }
        if (!run.result.failedModule.isEmpty())
            err["moduleId"] = run.result.failedModule;
        if (!run.result.failedPort.isEmpty())
            err["port"] = run.result.failedPort;
        if (!run.result.diagnostics.isEmpty())
            err["diagnostics"] = run.result.diagnostics.left(8192);
        if (run.result.errorMessage.size() > 8192 || run.result.diagnostics.size() > 8192)
            err["truncated"] = true;
        result["lastError"] = err;
    }
    return result;
}

// 阶段 3（G3）：按实例查询模块业务输出——测量值、检测列表、表格等。
// 运行中返回 running；工程已编辑返回 stale；图像/掩膜/点云仅返回摘要防上下文爆炸。
QJsonObject AgentActor::getModuleOutput(const QJsonObject& params) {
    const QString runId = params.value(QStringLiteral("runId")).toString();
    if (runId.isEmpty())
        return makeToolError("Missing 'runId' parameter", QStringLiteral("MISSING_PARAM"));
    QString instanceId = params.value("instanceId").toString();
    if (instanceId.isEmpty())
        return makeToolError("Missing 'instanceId' parameter", QStringLiteral("MISSING_PARAM"));
    const QString key = params.value("key").toString();
    const QString port = params.value("port").toString();
    if (runId.size() > 256 || instanceId.size() > 256 || key.size() > 256 || port.size() > 256)
        return makeToolError(QStringLiteral("runId, instanceId, key and port are limited to 256 characters"),
                             QStringLiteral("INVALID_PARAM"));

    RunEngine& engine = RunEngine::instance();
    const RunObservation run = engine.runObservation(runId);
    if (run.runId.isEmpty())
        return makeToolError(QStringLiteral("Run not found: %1").arg(runId), QStringLiteral("RUN_NOT_FOUND"));
    if (!run.finished)
        return QJsonObject{{"status", QStringLiteral("pending")}, {"runId", runId}};
    if (!run.result.success) {
        if (run.result.errorCode == ExecError::SideEffectUncertain) {
            return QJsonObject{{"status", QStringLiteral("uncertain")},
                               {"runId", runId},
                               {"reason", QStringLiteral("side_effect_outcome_unknown")},
                               {"sideEffectStatus", QStringLiteral("uncertain")},
                               {"retryable", false},
                               {"lastError", getRunResults(QJsonObject{{"runId", runId}}).value("lastError")}};
        }
        QJsonObject unavailable{
            {"status", QStringLiteral("unavailable")}, {"runId", runId}, {"reason", QStringLiteral("run_failed")}};
        if (run.result.failedModule == instanceId)
            unavailable["lastError"] = getRunResults(QJsonObject{{"runId", runId}}).value("lastError");
        return unavailable;
    }
    if (run.project != ProjectManager::instance().currentProject() ||
        run.projectFingerprint != projectFingerprint(run.project))
        return makeToolError(QStringLiteral("Outputs are stale — project has been modified since the last run"),
                             QStringLiteral("STALE_OUTPUT"));

    Project* proj = ProjectManager::instance().currentProject();
    if (!proj)
        return makeToolError("No project opened", QStringLiteral("NO_PROJECT"));
    if (!proj->moduleById(instanceId).has_value())
        return makeToolError(QString("Module not found: %1").arg(instanceId), QStringLiteral("INSTANCE_NOT_FOUND"));

    if (!key.isEmpty() && !port.isEmpty())
        return makeToolError("Specify either 'port' or 'key'", QStringLiteral("INVALID_PARAM"));
    const QString selected = port.isEmpty() ? key : port;
    ImageData output;
    QMap<QString, QVariant> namedValues;
    PortValueMap ports;
    if (!engine.runOutputSnapshot(runId, instanceId, output, namedValues, ports))
        return QJsonObject{{"status", QStringLiteral("unavailable")}, {"runId", runId}};
    if (output.width() == 0 && namedValues.isEmpty() && ports.isEmpty())
        return QJsonObject{{"status", QStringLiteral("unavailable")}, {"runId", runId}};
    const ImageCoordinateProvenance outputProvenance = output.coordinateProvenance();

    // 无 key：返回摘要——图像元数据 + 全部命名数据键及类型
    if (selected.isEmpty()) {
        if (params.contains(QStringLiteral("maskOffset")))
            return makeToolError(QStringLiteral("maskOffset requires a region key or port"),
                                 QStringLiteral("INVALID_PARAM"));
        if (params.contains(QStringLiteral("polygonOffset")) || params.contains(QStringLiteral("polygonLimit")))
            return makeToolError(QStringLiteral("polygon pagination requires a Region2D key or port"),
                                 QStringLiteral("INVALID_PARAM"));
        QJsonObject summary;
        summary["instanceId"] = instanceId;
        if (output.width() > 0)
            summary["image"] = imageOutputSummary(output);
        if (params.value(QStringLiteral("preview")).toBool() && output.width() > 0) {
            const QImage source = output.toQImage();
            if (!source.isNull()) {
                const QImage preview = source.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QByteArray encoded;
                QBuffer buffer(&encoded);
                if (buffer.open(QIODevice::WriteOnly) && preview.save(&buffer, "PNG")) {
                    summary["preview"] =
                        QJsonObject{{"mimeType", QStringLiteral("image/png")},
                                    {"base64", QString::fromLatin1(encoded.toBase64())},
                                    {"width", preview.width()},
                                    {"height", preview.height()},
                                    {"sourceRect", QJsonArray{0, 0, output.width(), output.height()}},
                                    {"scaleX", static_cast<double>(output.width()) / preview.width()},
                                    {"scaleY", static_cast<double>(output.height()) / preview.height()}};
                }
            }
        }
        const QMap<QString, QVariant> allData = output.allData();
        QJsonArray keys;
        for (auto it = allData.constBegin(); it != allData.constEnd(); ++it) {
            QJsonObject entry;
            entry["key"] = it.key();
            entry["type"] = QString::fromLatin1(it.value().typeName());
            keys.append(entry);
        }
        for (auto it = namedValues.cbegin(); it != namedValues.cend(); ++it) {
            if (allData.contains(it.key()))
                continue;
            keys.append(QJsonObject{{"key", it.key()}, {"type", QString::fromLatin1(it.value().typeName())}});
        }
        QJsonArray portEntries;
        for (auto it = ports.cbegin(); it != ports.cend(); ++it)
            portEntries.append(QJsonObject{{"port", it.key()}, {"type", QString::fromLatin1(it.value().typeName())}});
        summary["dataKeys"] = keys;
        summary["ports"] = portEntries;
        summary["hasOutput"] = (output.width() > 0) || !allData.isEmpty() || !namedValues.isEmpty() || !ports.isEmpty();
        if (!summary.value("hasOutput").toBool())
            return QJsonObject{{"status", QStringLiteral("unavailable")}, {"runId", runId}};
        QJsonObject response{{"status", QStringLiteral("available")}, {"runId", runId}, {"summary", summary}};
        if (QJsonDocument(response).toJson(QJsonDocument::Compact).size() > kOutputResponseLimit &&
            summary.contains(QStringLiteral("preview"))) {
            summary.remove(QStringLiteral("preview"));
            summary["previewTruncated"] = true;
            response["summary"] = summary;
        }
        if (QJsonDocument(response).toJson(QJsonDocument::Compact).size() > kOutputResponseLimit)
            return QJsonObject{{"status", QStringLiteral("truncated")},
                               {"runId", runId},
                               {"summary", QJsonObject{{"instanceId", instanceId},
                                                       {"image", summary.value(QStringLiteral("image"))},
                                                       {"dataKeyCount", keys.size()},
                                                       {"portCount", portEntries.size()}}}};
        return response;
    }

    // 有 key/port：显式支持的业务类型才可返回，表格/列表分页。
    const bool fromPort = !port.isEmpty();
    if ((fromPort && ports.contains(port)) || (!fromPort && namedValues.contains(key))) {
        const QVariant value = fromPort ? ports.value(selected) : namedValues.value(selected);
        QJsonObject result;
        result["status"] = QStringLiteral("available");
        result["runId"] = runId;
        result[fromPort ? "port" : "key"] = selected;
        result["source"] = fromPort ? QStringLiteral("port") : QStringLiteral("metadata");
        result["type"] = QString::fromLatin1(value.typeName());
        const PluginInfo info = PluginManager::instance().pluginInfo(proj->moduleById(instanceId)->moduleId);
        const bool distancePLUnknownFootPoint =
            info.id == QLatin1String("com.deeplux.plugin.distancepl") && selected == QLatin1String("foot_point") &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("Unspecified");
        if (distancePLUnknownFootPoint) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("DistancePL raw foot_point output does not accept pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.userType() != qMetaTypeId<QPointF>())
                return makeToolError(QStringLiteral("DistancePL foot_point must contain a QPointF value"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            const QPointF point = value.toPointF();
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                return makeToolError(QStringLiteral("DistancePL foot_point contains a non-finite coordinate"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            result[QStringLiteral("type")] = QStringLiteral("QVariantList");
            result[QStringLiteral("value")] = QJsonArray{point.x(), point.y()};
            return result;
        }
        const auto linePort =
            std::find_if(info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected](const PortSpec& spec) {
                return spec.id == selected && spec.type == DataType::Line2D;
            });
        const bool measurementInputLine = info.id == QStringLiteral("com.deeplux.plugin.measurementinput") &&
                                          linePort != info.outputPorts.cend() &&
                                          (selected == QStringLiteral("line") || selected == QStringLiteral("line1") ||
                                           selected == QStringLiteral("line2"));
        const bool measurementInputImageLine =
            measurementInputLine &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QStringLiteral("ImageLocalPixel");
        const bool linesDistanceNearestSegment =
            fromPort && info.id == QStringLiteral("com.deeplux.plugin.linesdistance") &&
            selected == QStringLiteral("nearest_segment") && linePort != info.outputPorts.cend();
        if (measurementInputLine &&
            (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
             params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
             params.contains(QStringLiteral("polygonLimit"))))
            return makeToolError(QStringLiteral("MeasurementInput Line2D output does not accept pagination"),
                                 QStringLiteral("INVALID_PARAM"));
        const auto measurementInputGeometryPort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.measurementinput") &&
                       spec.id == selected &&
                       ((selected == QStringLiteral("point") && spec.type == DataType::Any) ||
                        (selected == QStringLiteral("plane") && spec.type == DataType::Plane3D));
            });
        const bool measurementInputWorld3D =
            measurementInputGeometryPort != info.outputPorts.cend() &&
            output.data(QStringLiteral("measurement_input_mode")).toString() == QStringLiteral("point_plane") &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QStringLiteral("World3D");
        if (measurementInputWorld3D) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("MeasurementInput 3D geometry output does not accept pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            const int expectedSize = selected == QStringLiteral("point") ? 3 : 9;
            if (value.type() != QVariant::List || value.toList().size() != expectedSize)
                return makeToolError(QStringLiteral("MeasurementInput 3D geometry output has an invalid shape"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            for (const QVariant& coordinate : value.toList()) {
                switch (coordinate.type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default:
                    return makeToolError(
                        QStringLiteral("MeasurementInput 3D geometry output contains a non-numeric value"),
                        QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                }
                bool ok = false;
                const double number = coordinate.toDouble(&ok);
                if (!ok || !std::isfinite(number))
                    return makeToolError(
                        QStringLiteral("MeasurementInput 3D geometry output contains a non-finite value"),
                        QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            }
            bool supported = false;
            const QJsonValue jsonValue = outputValueToJson(value, &supported);
            if (!supported || !jsonValue.isArray())
                return makeToolError(QStringLiteral("MeasurementInput 3D geometry output cannot be serialized"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            result[QStringLiteral("type")] =
                selected == QStringLiteral("point") ? QStringLiteral("Point3D") : QStringLiteral("Plane3D");
            result[QStringLiteral("coordinateFrame")] = QStringLiteral("World3D");
            result[QStringLiteral("value")] = jsonValue;
            return result;
        }
        const auto ellipsePort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort](const PortSpec& spec) {
                return fromPort && spec.id == selected && spec.type == DataType::Ellipse2D;
            });
        const auto colorCenterPort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.colorrecognition") &&
                       spec.id == selected && spec.id == QStringLiteral("color_center") &&
                       spec.type == DataType::Point2D;
            });
        const auto blobCenterPort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.blob") && spec.id == selected &&
                       spec.id == QStringLiteral("largest_blob_center") && spec.type == DataType::Point2D;
            });
        const auto measurementInputPointPort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.measurementinput") &&
                       (spec.id == QStringLiteral("point1") || spec.id == QStringLiteral("point2")) &&
                       spec.id == selected && spec.type == DataType::Any;
            });
        const auto distancePLFootPointPort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.distancepl") && spec.id == selected &&
                       spec.id == QStringLiteral("foot_point") && spec.type == DataType::Point2D;
            });
        const auto pointSurfaceFootPointPort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.pointsurfacedistance") &&
                       spec.id == selected && spec.id == QStringLiteral("foot_point") && spec.type == DataType::Point3D;
            });
        bool pointSurfaceFootScalarWorld3D = false;
        if (!fromPort && info.id == QLatin1String("com.deeplux.plugin.pointsurfacedistance") &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("World3D")) {
            const int coordinateIndex = selected == QLatin1String("foot_x")   ? 0
                                        : selected == QLatin1String("foot_y") ? 1
                                        : selected == QLatin1String("foot_z") ? 2
                                                                              : -1;
            const QVariantList footPoint = output.data(QStringLiteral("foot_point")).toList();
            if (coordinateIndex >= 0 && footPoint.size() == 3) {
                const auto finiteNumeric = [](const QVariant& candidate, double& number) {
                    switch (candidate.type()) {
                    case QVariant::Int:
                    case QVariant::UInt:
                    case QVariant::LongLong:
                    case QVariant::ULongLong:
                    case QVariant::Double:
                        break;
                    default:
                        return false;
                    }
                    bool ok = false;
                    number = candidate.toDouble(&ok);
                    return ok && std::isfinite(number);
                };
                double scalar = 0.0;
                double components[3]{};
                pointSurfaceFootScalarWorld3D = finiteNumeric(value, scalar);
                for (int i = 0; pointSurfaceFootScalarWorld3D && i < 3; ++i)
                    pointSurfaceFootScalarWorld3D = finiteNumeric(footPoint.at(i), components[i]);
                pointSurfaceFootScalarWorld3D = pointSurfaceFootScalarWorld3D && scalar == components[coordinateIndex];
            }
        }
        const auto regionStatsCentroidPort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.regionstats") && spec.id == selected &&
                       spec.id == QStringLiteral("centroid") && spec.type == DataType::Point2D;
            });
        const auto fitPlanePort = std::find_if(
            info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected, fromPort, &info](const PortSpec& spec) {
                return fromPort && info.id == QStringLiteral("com.deeplux.plugin.fitplane") && spec.id == selected &&
                       spec.type == DataType::Plane3D;
            });
        const auto pointSetPort =
            std::find_if(info.outputPorts.cbegin(), info.outputPorts.cend(), [&selected](const PortSpec& spec) {
                return spec.id == selected && spec.type == DataType::PointSet2D;
            });
        const bool measurementImagePointSet =
            fromPort && proj->moduleById(instanceId)->moduleId == QStringLiteral("MeasurementInput") &&
            pointSetPort != info.outputPorts.cend() && value.userType() == qMetaTypeId<QVector<QPointF>>() &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QStringLiteral("ImageLocalPixel");
        bool edgeDefectImageLocalPointSetSource = false;
        if (info.id == QLatin1String("com.deeplux.plugin.edgedefectdetection") &&
            output.data(QStringLiteral("measurement_input_mode")).toString() == QLatin1String("point_set") &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("ImageLocalPixel")) {
            int imageConnections = 0;
            int referenceConnections = 0;
            QString imageSource;
            QString referenceSource;
            QString imageSourcePort;
            QString referenceSourcePort;
            for (const ModuleConnection& connection : proj->connections()) {
                if (connection.toModuleId != instanceId || connection.edgeType != QLatin1String("data"))
                    continue;
                if (connection.toPort == QLatin1String("image")) {
                    ++imageConnections;
                    imageSource = connection.fromModuleId;
                    imageSourcePort = connection.fromPort;
                } else if (connection.toPort == QLatin1String("reference_edge")) {
                    ++referenceConnections;
                    referenceSource = connection.fromModuleId;
                    referenceSourcePort = connection.fromPort;
                }
            }
            if (imageConnections == 1 && referenceConnections == 1 && imageSource == referenceSource &&
                imageSourcePort == QLatin1String("image") && referenceSourcePort == QLatin1String("fit_points")) {
                const auto sourceInstance = proj->moduleById(imageSource);
                const PluginInfo sourceInfo =
                    sourceInstance ? PluginManager::instance().pluginInfo(sourceInstance->moduleId) : PluginInfo{};
                edgeDefectImageLocalPointSetSource =
                    sourceInfo.id == QLatin1String("com.deeplux.plugin.measurementinput");
            }
        }
        const bool linesDistanceImageLocalLinePairSource = [&] {
            if (info.id != QLatin1String("com.deeplux.plugin.linesdistance") ||
                output.data(QStringLiteral("measurement_input_mode")).toString() != QLatin1String("line_pair") ||
                output.data(QStringLiteral("coordinate_frame")).toString() != QLatin1String("ImageLocalPixel"))
                return false;
            const auto linePort =
                std::find_if(info.outputPorts.cbegin(), info.outputPorts.cend(), [](const PortSpec& spec) {
                    return spec.id == QLatin1String("nearest_segment") && spec.type == DataType::Line2D;
                });
            if (linePort == info.outputPorts.cend())
                return false;
            QHash<QString, int> counts;
            QHash<QString, QString> sources;
            bool exactPorts = true;
            for (const ModuleConnection& connection : proj->connections()) {
                if (connection.toModuleId != instanceId || connection.edgeType != QLatin1String("data") ||
                    (connection.toPort != QLatin1String("image") && connection.toPort != QLatin1String("line1") &&
                     connection.toPort != QLatin1String("line2")))
                    continue;
                ++counts[connection.toPort];
                sources[connection.toPort] = connection.fromModuleId;
                exactPorts = exactPorts && connection.fromPort == connection.toPort;
            }
            const QString source = sources.value(QStringLiteral("image"));
            if (!exactPorts || counts.value(QStringLiteral("image")) != 1 ||
                counts.value(QStringLiteral("line1")) != 1 || counts.value(QStringLiteral("line2")) != 1 ||
                source.isEmpty() || sources.value(QStringLiteral("line1")) != source ||
                sources.value(QStringLiteral("line2")) != source)
                return false;
            const auto sourceInstance = proj->moduleById(source);
            const PluginInfo sourceInfo =
                sourceInstance ? PluginManager::instance().pluginInfo(sourceInstance->moduleId) : PluginInfo{};
            return sourceInfo.id == QLatin1String("com.deeplux.plugin.measurementinput");
        }();
        const auto finiteNumber = [](const QVariant& candidate, double& number) {
            switch (candidate.type()) {
            case QVariant::Int:
            case QVariant::UInt:
            case QVariant::LongLong:
            case QVariant::ULongLong:
            case QVariant::Double:
                break;
            default:
                return false;
            }
            bool ok = false;
            number = candidate.toDouble(&ok);
            return ok && std::isfinite(number);
        };
        QVariantList measureGapPoint1;
        QVariantList measureGapPoint2;
        const bool measureGapImageLocalPointPair = [&] {
            if (info.id != QLatin1String("com.deeplux.plugin.measuregap") ||
                output.data(QStringLiteral("measurement_dimension")).toString() != QLatin1String("2d"))
                return false;
            for (const QString& outputId : {QStringLiteral("gap"), QStringLiteral("gap_distance"),
                                            QStringLiteral("gap_delta_x"), QStringLiteral("gap_delta_y")}) {
                const auto outputPort =
                    std::find_if(info.outputPorts.cbegin(), info.outputPorts.cend(), [&outputId](const PortSpec& spec) {
                        return spec.id == outputId && spec.type == DataType::Number;
                    });
                if (outputPort == info.outputPorts.cend())
                    return false;
            }
            QHash<QString, int> counts;
            QHash<QString, QString> sources;
            bool exactPorts = true;
            for (const ModuleConnection& connection : proj->connections()) {
                if (connection.toModuleId != instanceId || connection.edgeType != QLatin1String("data") ||
                    (connection.toPort != QLatin1String("point1") && connection.toPort != QLatin1String("point2")))
                    continue;
                ++counts[connection.toPort];
                sources[connection.toPort] = connection.fromModuleId;
                exactPorts = exactPorts && connection.fromPort == connection.toPort;
            }
            const QString source = sources.value(QStringLiteral("point1"));
            if (!exactPorts || counts.value(QStringLiteral("point1")) != 1 ||
                counts.value(QStringLiteral("point2")) != 1 || source.isEmpty() ||
                sources.value(QStringLiteral("point2")) != source)
                return false;
            const auto sourceInstance = proj->moduleById(source);
            const PluginInfo sourceInfo =
                sourceInstance ? PluginManager::instance().pluginInfo(sourceInstance->moduleId) : PluginInfo{};
            if (sourceInfo.id != QLatin1String("com.deeplux.plugin.measurementinput"))
                return false;
            ImageData sourceOutput;
            QMap<QString, QVariant> sourceValues;
            PortValueMap sourcePorts;
            if (!RunEngine::instance().runOutputSnapshot(runId, source, sourceOutput, sourceValues, sourcePorts) ||
                sourceOutput.data(QStringLiteral("measurement_input_mode")).toString() != QLatin1String("point_pair") ||
                sourceOutput.data(QStringLiteral("coordinate_frame")).toString() != QLatin1String("ImageLocalPixel"))
                return false;
            measureGapPoint1 = sourceOutput.data(QStringLiteral("point1")).toList();
            measureGapPoint2 = sourceOutput.data(QStringLiteral("point2")).toList();
            if (measureGapPoint1.size() != 2 || measureGapPoint2.size() != 2)
                return false;
            double p1[2]{};
            double p2[2]{};
            for (int i = 0; i < 2; ++i) {
                if (!finiteNumber(measureGapPoint1.at(i), p1[i]) || !finiteNumber(measureGapPoint2.at(i), p2[i]))
                    return false;
            }
            const double dx = p2[0] - p1[0];
            const double dy = p2[1] - p1[1];
            const double expectedDistance = std::sqrt(dx * dx + dy * dy + 0.0 * 0.0);
            return std::isfinite(dx) && std::isfinite(dy) && std::isfinite(expectedDistance);
        }();
        QString unit = info.ui.value(QStringLiteral("results"))
                           .toObject()
                           .value(selected)
                           .toObject()
                           .value(QStringLiteral("unit"))
                           .toString();
        if (info.id == QLatin1String("com.deeplux.plugin.distancepl") && selected == QLatin1String("distance"))
            unit = output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("ImageLocalPixel")
                       ? QStringLiteral("px")
                       : QString();
        else if (info.id == QLatin1String("com.deeplux.plugin.distancepp") &&
                 (selected == QLatin1String("distance") || selected == QLatin1String("delta_x") ||
                  selected == QLatin1String("delta_y")))
            unit = output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("ImageLocalPixel")
                       ? QStringLiteral("px")
                       : QString();
        else if (info.id == QLatin1String("com.deeplux.plugin.measuregap") &&
                 (selected == QLatin1String("gap") || selected == QLatin1String("gap_distance") ||
                  selected == QLatin1String("gap_delta_x") || selected == QLatin1String("gap_delta_y")) &&
                 measureGapImageLocalPointPair) {
            double x1 = 0.0;
            double y1 = 0.0;
            double x2 = 0.0;
            double y2 = 0.0;
            double actual = 0.0;
            double reported = 0.0;
            if (measureGapPoint1.size() == 2 && measureGapPoint2.size() == 2 && finiteNumber(measureGapPoint1[0], x1) &&
                finiteNumber(measureGapPoint1[1], y1) && finiteNumber(measureGapPoint2[0], x2) &&
                finiteNumber(measureGapPoint2[1], y2) && finiteNumber(value, actual) &&
                finiteNumber(output.data(selected), reported) && actual == reported) {
                const double dx = x2 - x1;
                const double dy = y2 - y1;
                const double expected = selected == QLatin1String("gap") || selected == QLatin1String("gap_distance")
                                            ? std::sqrt(dx * dx + dy * dy + 0.0 * 0.0)
                                        : selected == QLatin1String("gap_delta_x") ? dx
                                                                                   : dy;
                if (std::isfinite(expected) && actual == expected)
                    unit = QStringLiteral("px");
            }
        } else if (info.id == QLatin1String("com.deeplux.plugin.fitline") && selected == QLatin1String("line_error"))
            unit = output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("ImageLocalPixel")
                       ? QStringLiteral("px")
                       : QString();
        else if (info.id == QLatin1String("com.deeplux.plugin.fitline") &&
                 (selected == QLatin1String("line_row1") || selected == QLatin1String("line_col1") ||
                  selected == QLatin1String("line_row2") || selected == QLatin1String("line_col2")) &&
                 output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("ImageLocalPixel")) {
            const auto linePort =
                std::find_if(info.outputPorts.cbegin(), info.outputPorts.cend(), [](const PortSpec& spec) {
                    return spec.id == QLatin1String("line") && spec.type == DataType::Line2D;
                });
            const QVariant lineValue = output.data(QStringLiteral("line"));
            const QVariantList line = lineValue.toList();
            const int component = selected == QLatin1String("line_col1")   ? 0
                                  : selected == QLatin1String("line_row1") ? 1
                                  : selected == QLatin1String("line_col2") ? 2
                                                                           : 3;
            bool validLine =
                linePort != info.outputPorts.cend() && lineValue.type() == QVariant::List && line.size() == 4;
            double lineCoordinates[4]{};
            for (int i = 0; validLine && i < line.size(); ++i) {
                switch (line.at(i).type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default:
                    validLine = false;
                    break;
                }
                bool ok = false;
                lineCoordinates[i] = line.at(i).toDouble(&ok);
                validLine = validLine && ok && std::isfinite(lineCoordinates[i]);
            }
            bool scalarIsNumeric = false;
            switch (value.type()) {
            case QVariant::Int:
            case QVariant::UInt:
            case QVariant::LongLong:
            case QVariant::ULongLong:
            case QVariant::Double:
                scalarIsNumeric = true;
                break;
            default:
                break;
            }
            bool scalarOk = false;
            const double scalar = value.toDouble(&scalarOk);
            if (validLine && scalarIsNumeric && scalarOk && std::isfinite(scalar) &&
                scalar == lineCoordinates[component])
                unit = QStringLiteral("px");
        } else if (info.id == QLatin1String("com.deeplux.plugin.linesdistance") &&
                   (selected == QLatin1String("nearest_x1") || selected == QLatin1String("nearest_y1") ||
                    selected == QLatin1String("nearest_x2") || selected == QLatin1String("nearest_y2")) &&
                   linesDistanceImageLocalLinePairSource) {
            const QVariant segmentValue = output.data(QStringLiteral("nearest_segment"));
            const QVariantList segment = segmentValue.toList();
            const int component = selected == QLatin1String("nearest_x1")   ? 0
                                  : selected == QLatin1String("nearest_y1") ? 1
                                  : selected == QLatin1String("nearest_x2") ? 2
                                                                            : 3;
            bool validSegment = segmentValue.type() == QVariant::List && segment.size() == 4;
            double coordinates[4]{};
            for (int i = 0; validSegment && i < segment.size(); ++i) {
                switch (segment.at(i).type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default:
                    validSegment = false;
                    break;
                }
                bool ok = false;
                coordinates[i] = segment.at(i).toDouble(&ok);
                validSegment = validSegment && ok && std::isfinite(coordinates[i]);
            }
            bool scalarIsNumeric = false;
            switch (value.type()) {
            case QVariant::Int:
            case QVariant::UInt:
            case QVariant::LongLong:
            case QVariant::ULongLong:
            case QVariant::Double:
                scalarIsNumeric = true;
                break;
            default:
                break;
            }
            bool scalarOk = false;
            const double scalar = value.toDouble(&scalarOk);
            if (validSegment && scalarIsNumeric && scalarOk && std::isfinite(scalar) &&
                scalar == coordinates[component])
                unit = QStringLiteral("px");
        } else if (info.id == QLatin1String("com.deeplux.plugin.fitellipse") &&
                   (selected == QLatin1String("ellipse_center_x") || selected == QLatin1String("ellipse_center_y") ||
                    selected == QLatin1String("ellipse_major_r") || selected == QLatin1String("ellipse_minor_r") ||
                    selected == QLatin1String("ellipse_phi")) &&
                   output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("ImageLocalPixel")) {
            const QVariant ellipseValue = output.data(QStringLiteral("ellipse"));
            if (ellipseValue.userType() == qMetaTypeId<Ellipse2D>() && ellipseValue.value<Ellipse2D>().isValid())
                unit = selected == QLatin1String("ellipse_phi") ? QStringLiteral("deg") : QStringLiteral("px");
        } else if (info.id == QLatin1String("com.deeplux.plugin.fitcircle") &&
                   (selected == QLatin1String("circle_center_x") || selected == QLatin1String("circle_center_y") ||
                    selected == QLatin1String("circle_radius") || selected == QLatin1String("circle_error")) &&
                   output.data(QStringLiteral("coordinate_frame")).toString() == QLatin1String("ImageLocalPixel")) {
            const QVariant circleValue = output.data(QStringLiteral("circle"));
            if (circleValue.userType() == qMetaTypeId<Circle2D>() && circleValue.value<Circle2D>().isValid())
                unit = QStringLiteral("px");
        } else if (info.id == QLatin1String("com.deeplux.plugin.edgedefectdetection") &&
                   (selected == QLatin1String("max_deviation") || selected == QLatin1String("mean_deviation") ||
                    selected == QLatin1String("deviation_stddev")) &&
                   edgeDefectImageLocalPointSetSource) {
            unit = QStringLiteral("px");
        } else if (info.id == QLatin1String("com.deeplux.plugin.regionstats") &&
                   (selected == QLatin1String("region_area") || selected == QLatin1String("bbox_x") ||
                    selected == QLatin1String("bbox_y") || selected == QLatin1String("bbox_w") ||
                    selected == QLatin1String("bbox_h") || selected == QLatin1String("centroid_x") ||
                    selected == QLatin1String("centroid_y"))) {
            int regionConnections = 0;
            int imageConnections = 0;
            QString regionSource;
            QString imageSource;
            QString regionSourcePort;
            QString imageSourcePort;
            for (const ModuleConnection& connection : proj->connections()) {
                if (connection.toModuleId != instanceId || connection.edgeType != QLatin1String("data"))
                    continue;
                if (connection.toPort == QLatin1String("region")) {
                    ++regionConnections;
                    regionSource = connection.fromModuleId;
                    regionSourcePort = connection.fromPort;
                } else if (connection.toPort == QLatin1String("image")) {
                    ++imageConnections;
                    imageSource = connection.fromModuleId;
                    imageSourcePort = connection.fromPort;
                }
            }
            const auto sourceModule = proj->moduleById(regionSource);
            const PluginInfo sourceInfo =
                sourceModule ? PluginManager::instance().pluginInfo(sourceModule->moduleId) : PluginInfo{};
            if (regionConnections == 1 && imageConnections == 1 && regionSource == imageSource &&
                regionSourcePort == QLatin1String("region") && imageSourcePort == QLatin1String("image") &&
                sourceInfo.id == QLatin1String("com.deeplux.plugin.sampredict"))
                unit = selected == QLatin1String("region_area") ? QStringLiteral("px^2") : QStringLiteral("px");
        } else if (!fromPort && info.id == QLatin1String("com.deeplux.plugin.blob") &&
                   (selected == QLatin1String("blob_center_x") || selected == QLatin1String("blob_center_y") ||
                    selected == QLatin1String("blob_area") || selected == QLatin1String("blob_perimeter"))) {
            unit = selected == QLatin1String("blob_area") ? QStringLiteral("px^2") : QStringLiteral("px");
        }
        if (!unit.isEmpty())
            result["unit"] = unit;
        if (info.id == QLatin1String("com.deeplux.plugin.edgedefectdetection") &&
            selected == QLatin1String("defect_regions") && edgeDefectImageLocalPointSetSource)
            result[QStringLiteral("columnUnits")] =
                QJsonObject{{QStringLiteral("max_deviation"), QStringLiteral("px")},
                            {QStringLiteral("mean_deviation"), QStringLiteral("px")}};
        if (linesDistanceNearestSegment) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("LinesDistance nearest_segment does not accept pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.type() != QVariant::List || value.toList().size() != 4) {
                QJsonObject error =
                    makeToolError(QStringLiteral("LinesDistance nearest_segment must contain four coordinates"),
                                  QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                error[QStringLiteral("status")] = QStringLiteral("error");
                return error;
            }
            const QVariantList coordinates = value.toList();
            double xy[4]{};
            for (int i = 0; i < 4; ++i) {
                switch (coordinates.at(i).type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default: {
                    QJsonObject error = makeToolError(QStringLiteral("LinesDistance nearest_segment contains a "
                                                                     "non-numeric coordinate"),
                                                      QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                    error[QStringLiteral("status")] = QStringLiteral("error");
                    return error;
                }
                }
                bool ok = false;
                xy[i] = coordinates.at(i).toDouble(&ok);
                if (!ok || !std::isfinite(xy[i])) {
                    QJsonObject error = makeToolError(QStringLiteral("LinesDistance nearest_segment contains a "
                                                                     "non-finite coordinate"),
                                                      QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                    error[QStringLiteral("status")] = QStringLiteral("error");
                    return error;
                }
            }

            const bool imageLocalMeasurementInput = linesDistanceImageLocalLinePairSource;

            if (!imageLocalMeasurementInput) {
                result[QStringLiteral("type")] = QStringLiteral("QVariantList");
                result[QStringLiteral("value")] = QJsonArray{xy[0], xy[1], xy[2], xy[3]};
                return result;
            }

            result[QStringLiteral("type")] = QStringLiteral("Line2D");
            QJsonObject line{{QStringLiteral("start"), QJsonArray{xy[0], xy[1]}},
                             {QStringLiteral("end"), QJsonArray{xy[2], xy[3]}},
                             {QStringLiteral("coordinateSystem"), QStringLiteral("image_pixels")}};
            if (hasSafeSourceImageId(outputProvenance)) {
                const auto& matrix = outputProvenance.localToSourceMatrix();
                const double startDenominator = matrix[6] * xy[0] + matrix[7] * xy[1] + matrix[8];
                const double endDenominator = matrix[6] * xy[2] + matrix[7] * xy[3] + matrix[8];
                const auto start = outputProvenance.mapLocalToSource(QPointF(xy[0], xy[1]));
                const auto end = outputProvenance.mapLocalToSource(QPointF(xy[2], xy[3]));
                if (start && end && std::signbit(startDenominator) == std::signbit(endDenominator)) {
                    line[QStringLiteral("sourceImageCoordinates")] =
                        QJsonObject{{QStringLiteral("sourceImageId"), outputProvenance.sourceImageId},
                                    {QStringLiteral("coordinateSystem"), QStringLiteral("source_image_pixels")},
                                    {QStringLiteral("start"), QJsonArray{start->x(), start->y()}},
                                    {QStringLiteral("end"), QJsonArray{end->x(), end->y()}}};
                }
            }
            result[QStringLiteral("value")] = line;
            return result;
        }
        if (regionStatsCentroidPort != info.outputPorts.cend()) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("RegionStats centroid output does not accept pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.userType() != QMetaType::QPointF)
                return makeToolError(QStringLiteral("RegionStats centroid must contain a QPointF value"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            const QPointF point = value.toPointF();
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                return makeToolError(QStringLiteral("RegionStats centroid contains a non-finite coordinate"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));

            result[QStringLiteral("type")] = QStringLiteral("Point2D");
            result[QStringLiteral("coordinateSystem")] = QStringLiteral("image_pixels");
            result[QStringLiteral("value")] = QJsonArray{point.x(), point.y()};
            if (hasSafeSourceImageId(outputProvenance)) {
                QString regionSource;
                QString imageSource;
                int regionConnections = 0;
                int imageConnections = 0;
                for (const ModuleConnection& connection : proj->connections()) {
                    if (connection.toModuleId != instanceId || connection.edgeType == QLatin1String("control"))
                        continue;
                    if (connection.toPort == QLatin1String("region")) {
                        regionSource = connection.fromModuleId;
                        ++regionConnections;
                    } else if (connection.toPort == QLatin1String("image")) {
                        imageSource = connection.fromModuleId;
                        ++imageConnections;
                    }
                }
                const auto sourceModule = proj->moduleById(regionSource);
                const PluginInfo sourceInfo =
                    sourceModule ? PluginManager::instance().pluginInfo(sourceModule->moduleId) : PluginInfo{};
                if (regionConnections == 1 && imageConnections == 1 && regionSource == imageSource &&
                    sourceInfo.id == QStringLiteral("com.deeplux.plugin.sampredict")) {
                    if (const auto sourcePoint = outputProvenance.mapLocalToSource(point)) {
                        result[QStringLiteral("sourceImageCoordinates")] =
                            QJsonObject{{QStringLiteral("sourceImageId"), outputProvenance.sourceImageId},
                                        {QStringLiteral("coordinateSystem"), QStringLiteral("source_image_pixels")},
                                        {QStringLiteral("value"), QJsonArray{sourcePoint->x(), sourcePoint->y()}}};
                    }
                }
            }
            return result;
        }
        if (colorCenterPort != info.outputPorts.cend() || blobCenterPort != info.outputPorts.cend()) {
            const QString source =
                blobCenterPort != info.outputPorts.cend() ? QStringLiteral("Blob") : QStringLiteral("ColorRecognition");
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")))
                return makeToolError(QStringLiteral("%1 Point2D output does not accept pagination").arg(source),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.userType() != QMetaType::QPointF)
                return makeToolError(QStringLiteral("%1 center must contain a QPointF value").arg(source),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            const QPointF point = value.toPointF();
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                return makeToolError(QStringLiteral("%1 center contains a non-finite coordinate").arg(source),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));

            result["type"] = QStringLiteral("Point2D");
            result["coordinateSystem"] = QStringLiteral("image_pixels");
            result["value"] = QJsonArray{point.x(), point.y()};
            if (hasSafeSourceImageId(outputProvenance)) {
                if (const auto sourcePoint = outputProvenance.mapLocalToSource(point)) {
                    result["sourceImageCoordinates"] =
                        QJsonObject{{"sourceImageId", outputProvenance.sourceImageId},
                                    {"coordinateSystem", QStringLiteral("source_image_pixels")},
                                    {"value", QJsonArray{sourcePoint->x(), sourcePoint->y()}}};
                }
            }
            return result;
        }
        if (measurementInputPointPort != info.outputPorts.cend() &&
            output.data(QStringLiteral("measurement_input_mode")).toString() == QStringLiteral("point_pair")) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("MeasurementInput point-pair output does not accept pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.type() != QVariant::List)
                return makeToolError(QStringLiteral("MeasurementInput point-pair output must be a numeric list"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            const QVariantList coordinates = value.toList();
            if (coordinates.size() != 2 && coordinates.size() != 3)
                return makeToolError(
                    QStringLiteral("MeasurementInput point_pair output must contain two or three coordinates"),
                    QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            double xy[3]{};
            for (int i = 0; i < coordinates.size(); ++i) {
                switch (coordinates.at(i).type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default:
                    return makeToolError(
                        QStringLiteral("MeasurementInput point_pair output contains a non-numeric coordinate"),
                        QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                }
                bool ok = false;
                xy[i] = coordinates.at(i).toDouble(&ok);
                if (!ok || !std::isfinite(xy[i]))
                    return makeToolError(
                        QStringLiteral("MeasurementInput point_pair output contains a non-finite coordinate"),
                        QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            }
            const QString coordinateFrame = output.data(QStringLiteral("coordinate_frame")).toString();
            if (coordinates.size() == 2 && coordinateFrame == QLatin1String("ImageLocalPixel")) {
                result[QStringLiteral("type")] = QStringLiteral("Point2D");
                result[QStringLiteral("coordinateSystem")] = QStringLiteral("image_pixels");
                result[QStringLiteral("value")] = QJsonArray{xy[0], xy[1]};
                if (hasSafeSourceImageId(outputProvenance)) {
                    if (const auto sourcePoint = outputProvenance.mapLocalToSource(QPointF(xy[0], xy[1]))) {
                        result[QStringLiteral("sourceImageCoordinates")] =
                            QJsonObject{{QStringLiteral("sourceImageId"), outputProvenance.sourceImageId},
                                        {QStringLiteral("coordinateSystem"), QStringLiteral("source_image_pixels")},
                                        {QStringLiteral("value"), QJsonArray{sourcePoint->x(), sourcePoint->y()}}};
                    }
                }
                return result;
            }
            if (coordinates.size() == 3 && coordinateFrame == QLatin1String("World3D")) {
                result[QStringLiteral("type")] = QStringLiteral("Point3D");
                result[QStringLiteral("coordinateFrame")] = QStringLiteral("World3D");
                result[QStringLiteral("value")] = QJsonArray{xy[0], xy[1], xy[2]};
                return result;
            }
        }
        if (distancePLFootPointPort != info.outputPorts.cend() &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QStringLiteral("ImageLocalPixel")) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("DistancePL Point2D output does not accept pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.userType() != qMetaTypeId<QPointF>())
                return makeToolError(QStringLiteral("DistancePL foot_point must contain a QPointF value"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            const QPointF point = value.toPointF();
            if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                return makeToolError(QStringLiteral("DistancePL foot_point contains a non-finite coordinate"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            result[QStringLiteral("type")] = QStringLiteral("Point2D");
            result[QStringLiteral("coordinateSystem")] = QStringLiteral("image_pixels");
            result[QStringLiteral("value")] = QJsonArray{point.x(), point.y()};
            if (hasSafeSourceImageId(outputProvenance)) {
                if (const auto sourcePoint = outputProvenance.mapLocalToSource(point)) {
                    result[QStringLiteral("sourceImageCoordinates")] =
                        QJsonObject{{QStringLiteral("sourceImageId"), outputProvenance.sourceImageId},
                                    {QStringLiteral("coordinateSystem"), QStringLiteral("source_image_pixels")},
                                    {QStringLiteral("value"), QJsonArray{sourcePoint->x(), sourcePoint->y()}}};
                }
            }
            return result;
        }
        if (pointSurfaceFootPointPort != info.outputPorts.cend() &&
            output.data(QStringLiteral("coordinate_frame")).toString() == QStringLiteral("World3D")) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("PointSurfaceDistance Point3D output does not accept pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.type() != QVariant::List || value.toList().size() != 3)
                return makeToolError(QStringLiteral("PointSurfaceDistance foot_point must contain three coordinates"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            for (const QVariant& coordinate : value.toList()) {
                switch (coordinate.type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default:
                    return makeToolError(QStringLiteral("PointSurfaceDistance foot_point contains a non-numeric value"),
                                         QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                }
                bool ok = false;
                const double number = coordinate.toDouble(&ok);
                if (!ok || !std::isfinite(number))
                    return makeToolError(QStringLiteral("PointSurfaceDistance foot_point contains a non-finite value"),
                                         QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            }
            bool supported = false;
            const QJsonValue jsonValue = outputValueToJson(value, &supported);
            if (!supported || !jsonValue.isArray())
                return makeToolError(QStringLiteral("PointSurfaceDistance foot_point cannot be serialized"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            result[QStringLiteral("type")] = QStringLiteral("Point3D");
            result[QStringLiteral("coordinateFrame")] = QStringLiteral("World3D");
            result[QStringLiteral("value")] = jsonValue;
            return result;
        }
        if (ellipsePort != info.outputPorts.cend()) {
            if (value.userType() != qMetaTypeId<Ellipse2D>() || !value.value<Ellipse2D>().isValid())
                return makeToolError(QStringLiteral("Ellipse2D output must contain a valid Ellipse2D value"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            result["type"] = QStringLiteral("Ellipse2D");
        }
        if (fitPlanePort != info.outputPorts.cend()) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")))
                return makeToolError(QStringLiteral("FitPlane Plane3D output does not accept array pagination"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.type() != QVariant::List || value.toList().size() != 9)
                return makeToolError(QStringLiteral("FitPlane Plane3D output must contain exactly nine finite numbers"),
                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
            QJsonArray plane;
            for (const QVariant& coordinate : value.toList()) {
                switch (coordinate.type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default:
                    return makeToolError(QStringLiteral("FitPlane Plane3D output contains a non-numeric coordinate"),
                                         QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                }
                bool ok = false;
                const double number = coordinate.toDouble(&ok);
                if (!ok || !std::isfinite(number))
                    return makeToolError(QStringLiteral("FitPlane Plane3D output contains a non-finite coordinate"),
                                         QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                plane.append(number);
            }
            result["type"] = QStringLiteral("Plane3D");
            result["coordinateSystem"] = QStringLiteral("height_map_local_scaled_xyz");
            result["unitBasis"] = QJsonArray{"pixelSizeX", "pixelSizeY", "zScale"};
            result["value"] = plane;
            return result;
        }
        const bool isJsonArray = value.userType() == qMetaTypeId<QJsonArray>();
        const bool isList = value.type() == QVariant::List || value.type() == QVariant::StringList || isJsonArray ||
                            value.userType() == qMetaTypeId<DetectionList>() ||
                            value.userType() == qMetaTypeId<QVector<QPointF>>();
        const bool linesDistanceNearestSegmentKey =
            !fromPort && info.id == QStringLiteral("com.deeplux.plugin.linesdistance") &&
            selected == QStringLiteral("nearest_segment") && linePort != info.outputPorts.cend();
        if (linePort != info.outputPorts.cend() && (!measurementInputLine || measurementInputImageLine) &&
            !linesDistanceNearestSegmentKey) {
            if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")) ||
                params.contains(QStringLiteral("maskOffset")) || params.contains(QStringLiteral("polygonOffset")) ||
                params.contains(QStringLiteral("polygonLimit")))
                return makeToolError(QStringLiteral("Line2D output does not accept pagination or mask parameters"),
                                     QStringLiteral("INVALID_PARAM"));
            if (value.type() != QVariant::List || value.toList().size() != 4) {
                QJsonObject error =
                    makeToolError(QStringLiteral("Line2D output must contain exactly four numeric coordinates"),
                                  QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                error[QStringLiteral("status")] = QStringLiteral("error");
                return error;
            }
            const QVariantList coordinates = value.toList();
            double xy[4]{};
            for (int i = 0; i < 4; ++i) {
                switch (coordinates.at(i).type()) {
                case QVariant::Int:
                case QVariant::UInt:
                case QVariant::LongLong:
                case QVariant::ULongLong:
                case QVariant::Double:
                    break;
                default: {
                    QJsonObject error = makeToolError(QStringLiteral("Line2D output contains a non-numeric coordinate"),
                                                      QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                    error[QStringLiteral("status")] = QStringLiteral("error");
                    return error;
                }
                }
                bool ok = false;
                xy[i] = coordinates.at(i).toDouble(&ok);
                if (!ok || !std::isfinite(xy[i])) {
                    QJsonObject error = makeToolError(QStringLiteral("Line2D output contains a non-finite coordinate"),
                                                      QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                    error[QStringLiteral("status")] = QStringLiteral("error");
                    return error;
                }
            }
            result["type"] = QStringLiteral("Line2D");
            QJsonObject line{{QStringLiteral("start"), QJsonArray{xy[0], xy[1]}},
                             {QStringLiteral("end"), QJsonArray{xy[2], xy[3]}},
                             {QStringLiteral("coordinateSystem"), QStringLiteral("image_pixels")}};
            if (hasSafeSourceImageId(outputProvenance)) {
                const auto& matrix = outputProvenance.localToSourceMatrix();
                const double startDenominator = matrix[6] * xy[0] + matrix[7] * xy[1] + matrix[8];
                const double endDenominator = matrix[6] * xy[2] + matrix[7] * xy[3] + matrix[8];
                const auto start = outputProvenance.mapLocalToSource(QPointF(xy[0], xy[1]));
                const auto end = outputProvenance.mapLocalToSource(QPointF(xy[2], xy[3]));
                if (start && end && std::signbit(startDenominator) == std::signbit(endDenominator)) {
                    line[QStringLiteral("sourceImageCoordinates")] =
                        QJsonObject{{QStringLiteral("sourceImageId"), outputProvenance.sourceImageId},
                                    {QStringLiteral("coordinateSystem"), QStringLiteral("source_image_pixels")},
                                    {QStringLiteral("start"), QJsonArray{start->x(), start->y()}},
                                    {QStringLiteral("end"), QJsonArray{end->x(), end->y()}}};
                }
            }
            result["value"] = line;
        } else {
            if (isList) {
                if (params.contains(QStringLiteral("maskOffset")))
                    return makeToolError(QStringLiteral("maskOffset is only available for region outputs"),
                                         QStringLiteral("INVALID_PARAM"));
                if (params.contains(QStringLiteral("polygonOffset")) || params.contains(QStringLiteral("polygonLimit")))
                    return makeToolError(QStringLiteral("polygon pagination is only available for Region2D outputs"),
                                         QStringLiteral("INVALID_PARAM"));
                const QJsonValue offsetParam = params.value(QStringLiteral("offset"));
                const QJsonValue limitParam = params.value(QStringLiteral("limit"));
                const int offset = offsetParam.isUndefined() ? 0 : offsetParam.toInt(-1);
                const int limit = limitParam.isUndefined() ? kDefaultOutputPageSize : limitParam.toInt(-1);
                if (offset < 0 || limit < 1 || limit > kMaxOutputPageSize ||
                    (!offsetParam.isUndefined() && !offsetParam.isDouble()) ||
                    (!limitParam.isUndefined() && !limitParam.isDouble()))
                    return makeToolError(QStringLiteral("offset must be >= 0 and limit must be 1..1000"),
                                         QStringLiteral("INVALID_PARAM"));
                QVariantList rows;
                if (isJsonArray)
                    rows = value.value<QJsonArray>().toVariantList();
                else if (value.userType() != qMetaTypeId<DetectionList>())
                    rows = value.toList();
                const DetectionList detections =
                    value.userType() == qMetaTypeId<DetectionList>() ? value.value<DetectionList>() : DetectionList{};
                const QVector<QPointF> pointSet = value.userType() == qMetaTypeId<QVector<QPointF>>()
                                                      ? value.value<QVector<QPointF>>()
                                                      : QVector<QPointF>{};
                if (value.userType() == qMetaTypeId<QVector<QPointF>>()) {
                    for (const QPointF& point : pointSet) {
                        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                            return makeToolError(QStringLiteral("Point set contains a non-finite coordinate"),
                                                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                    }
                }
                const int totalRows =
                    value.userType() == qMetaTypeId<DetectionList>()
                        ? detections.size()
                        : (value.userType() == qMetaTypeId<QVector<QPointF>>() ? pointSet.size() : rows.size());
                const bool includePointSetSource = measurementImagePointSet && hasSafeSourceImageId(outputProvenance);
                if (measurementImagePointSet) {
                    result["type"] = QStringLiteral("PointSet2D");
                    result["coordinateSystem"] = QStringLiteral("image_pixels");
                }
                QJsonArray page;
                QJsonArray sourcePointPage;
                for (int i = offset; i < totalRows && page.size() < limit; ++i) {
                    if (value.userType() == qMetaTypeId<DetectionList>()) {
                        const Detection& item = detections.items.at(i);
                        page.append(detectionToJson(item, outputProvenance));
                    } else if (value.userType() == qMetaTypeId<QVector<QPointF>>()) {
                        bool supported = false;
                        const QJsonValue json = outputValueToJson(QVariant::fromValue(pointSet.at(i)), &supported);
                        if (!supported)
                            return makeToolError(QStringLiteral("Array row %1 cannot be serialized").arg(i),
                                                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                        page.append(json);
                        if (includePointSetSource) {
                            const auto sourcePoint = outputProvenance.mapLocalToSource(pointSet.at(i));
                            sourcePointPage.append(sourcePoint
                                                       ? QJsonValue(QJsonArray{sourcePoint->x(), sourcePoint->y()})
                                                       : QJsonValue(QJsonValue::Null));
                        }
                    } else {
                        bool supported = false;
                        const QJsonValue json = outputValueToJson(rows.at(i), &supported, outputProvenance);
                        if (!supported)
                            return makeToolError(QStringLiteral("Array row %1 cannot be serialized").arg(i),
                                                 QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                        page.append(json);
                    }
                }
                result["totalRows"] = totalRows;
                result["offset"] = offset;
                result["limit"] = limit;
                result["value"] = page;
                result["truncated"] = offset + page.size() < totalRows;
                if (includePointSetSource) {
                    result["sourceImageCoordinates"] =
                        QJsonObject{{"sourceImageId", outputProvenance.sourceImageId},
                                    {"coordinateSystem", QStringLiteral("source_image_pixels")},
                                    {"points", sourcePointPage}};
                }
                while (!page.isEmpty() &&
                       QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kOutputResponseLimit) {
                    page.removeLast();
                    result["value"] = page;
                    if (includePointSetSource) {
                        sourcePointPage.removeLast();
                        QJsonObject sourceCoordinates = result["sourceImageCoordinates"].toObject();
                        sourceCoordinates["points"] = sourcePointPage;
                        result["sourceImageCoordinates"] = sourceCoordinates;
                    }
                    result["truncated"] = true;
                }
                if (page.isEmpty() && offset < totalRows)
                    return makeToolError(QStringLiteral("Array row %1 exceeds the 64 KiB response limit").arg(offset),
                                         QStringLiteral("ROW_TOO_LARGE"), false,
                                         QJsonObject{{"runId", runId}, {"instanceId", instanceId}, {"row", offset}});
            } else {
                if (params.contains(QStringLiteral("offset")) || params.contains(QStringLiteral("limit")))
                    return makeToolError(QStringLiteral("Pagination is only available for array outputs"),
                                         QStringLiteral("INVALID_PARAM"));
                const bool isRegion = value.userType() == qMetaTypeId<Region2D>();
                if ((params.contains(QStringLiteral("polygonOffset")) ||
                     params.contains(QStringLiteral("polygonLimit"))) &&
                    !isRegion)
                    return makeToolError(QStringLiteral("polygon pagination is only available for Region2D outputs"),
                                         QStringLiteral("INVALID_PARAM"));
                if (params.contains(QStringLiteral("maskOffset")) && value.userType() != qMetaTypeId<Region2D>())
                    return makeToolError(QStringLiteral("maskOffset is only available for region outputs"),
                                         QStringLiteral("INVALID_PARAM"));
                bool supported = false;
                const QJsonValue jsonValue = outputValueToJson(value, &supported, outputProvenance);
                if (!supported)
                    return makeToolError(QStringLiteral("Output type cannot be serialized: %1")
                                             .arg(QString::fromLatin1(value.typeName())),
                                         QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                result["value"] = jsonValue;
                if (params.value(QStringLiteral("preview")).toBool() &&
                    value.userType() == qMetaTypeId<PointCloudData>()) {
                    const PointCloudData& cloud = *static_cast<const PointCloudData*>(value.constData());
                    QJsonArray sample;
                    for (size_t i = 0; i < qMin<size_t>(64, cloud.points.size()); ++i) {
                        const auto& point = cloud.points[i];
                        if (!point.allFinite()) {
                            sample = QJsonArray();
                            result["previewUnavailable"] = QStringLiteral("non-finite point coordinate");
                            break;
                        }
                        sample.append(QJsonArray{point.x(), point.y(), point.z()});
                    }
                    if (!result.contains("previewUnavailable"))
                        result["preview"] = QJsonObject{{"kind", "point_sample"},
                                                        {"coordinateSystem", "unspecified"},
                                                        {"points", sample},
                                                        {"sampleCount", sample.size()},
                                                        {"totalPoints", static_cast<qint64>(cloud.size())}};
                } else if (params.value(QStringLiteral("preview")).toBool() &&
                           value.userType() == qMetaTypeId<Region2D>()) {
                    const Region2D& region = *static_cast<const Region2D*>(value.constData());
                    QJsonArray polygon;
                    for (int i = 0; i < qMin(64, region.polygon.size()); ++i)
                        polygon.append(QJsonArray{region.polygon.at(i).x(), region.polygon.at(i).y()});
                    QJsonObject preview{{"kind", "region"},
                                        {"polygon", polygon},
                                        {"totalPolygonPoints", region.polygon.size()},
                                        {"coordinateSystem", "image_pixels"}};
                    // shortcut: pixel preview is capped at 1 MP/1 MiB RLE; larger masks need a streamed export tool.
                    if (!region.maskRle.isEmpty() && output.width() > 0 && output.height() > 0 &&
                        static_cast<qint64>(output.width()) * output.height() <= 1024 * 1024 &&
                        region.maskRle.size() <= 1024 * 1024 && region.maskRle.toUtf8().size() <= 1024 * 1024) {
                        bool ok = false;
                        const QImage mask =
                            MaskOps::decodeRle(region.maskRle, QSize(output.width(), output.height()), &ok);
                        if (ok) {
                            const QImage small = mask.scaled(64, 64, Qt::KeepAspectRatio, Qt::FastTransformation);
                            QByteArray encoded;
                            QBuffer buffer(&encoded);
                            if (buffer.open(QIODevice::WriteOnly) && small.save(&buffer, "PNG")) {
                                preview["mask"] =
                                    QJsonObject{{"mimeType", "image/png"},
                                                {"base64", QString::fromLatin1(encoded.toBase64())},
                                                {"width", small.width()},
                                                {"height", small.height()},
                                                {"sourceRect", QJsonArray{0, 0, output.width(), output.height()}}};
                            }
                        }
                    }
                    if (!region.maskRle.isEmpty() && !preview.contains("mask"))
                        preview["maskUnavailable"] = QStringLiteral("mask preview unavailable or exceeds safe limits");
                    result["preview"] = preview;
                }
                if (value.userType() == qMetaTypeId<Region2D>()) {
                    const Region2D& region = *static_cast<const Region2D*>(value.constData());
                    const bool requestPolygonPage = params.contains(QStringLiteral("polygonOffset")) ||
                                                    params.contains(QStringLiteral("polygonLimit"));
                    int polygonOffset = 0;
                    int polygonLimit = kDefaultOutputPageSize;
                    if (requestPolygonPage) {
                        const auto parsePageInteger = [](const QJsonValue& value, int fallback) -> int {
                            if (value.isUndefined())
                                return fallback;
                            if (!value.isDouble())
                                return -1;
                            const double number = value.toDouble();
                            if (!std::isfinite(number) || std::floor(number) != number ||
                                number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
                                return -1;
                            return static_cast<int>(number);
                        };
                        polygonOffset = parsePageInteger(params.value(QStringLiteral("polygonOffset")), 0);
                        polygonLimit =
                            parsePageInteger(params.value(QStringLiteral("polygonLimit")), kDefaultOutputPageSize);
                        if (polygonOffset < 0 || polygonLimit < 1 || polygonLimit > kMaxOutputPageSize)
                            return makeToolError(
                                QStringLiteral("polygonOffset must be >= 0 and polygonLimit must be 1..1000"),
                                QStringLiteral("INVALID_PARAM"));

                        QJsonArray localPoints;
                        QJsonArray sourcePoints;
                        bool sourcePageAvailable = hasSafeSourceImageId(outputProvenance);
                        const int polygonEnd = static_cast<int>(
                            qMin<qint64>(region.polygon.size(), static_cast<qint64>(polygonOffset) + polygonLimit));
                        for (int i = polygonOffset; i < polygonEnd; ++i) {
                            const QPointF point = region.polygon.at(i);
                            if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
                                return makeToolError(QStringLiteral("Region polygon contains a non-finite coordinate"),
                                                     QStringLiteral("UNSUPPORTED_OUTPUT_TYPE"));
                            localPoints.append(QJsonArray{point.x(), point.y()});
                            if (sourcePageAvailable) {
                                if (const auto mapped = outputProvenance.mapLocalToSource(point))
                                    sourcePoints.append(QJsonArray{mapped->x(), mapped->y()});
                                else
                                    sourcePageAvailable = false;
                            }
                        }

                        QJsonObject polygonPage{{"totalPoints", region.polygon.size()},
                                                {"offset", polygonOffset},
                                                {"limit", polygonLimit},
                                                {"truncated", static_cast<qint64>(polygonOffset) + localPoints.size() <
                                                                  region.polygon.size()},
                                                {"coordinateSystem", QStringLiteral("image_pixels")},
                                                {"points", localPoints}};
                        if (sourcePageAvailable && !localPoints.isEmpty())
                            polygonPage[QStringLiteral("sourceImageCoordinates")] =
                                QJsonObject{{"sourceImageId", outputProvenance.sourceImageId},
                                            {"coordinateSystem", QStringLiteral("source_image_pixels")},
                                            {"points", sourcePoints}};
                        result[QStringLiteral("polygonPage")] = polygonPage;
                    }
                    if (!region.maskRle.isEmpty()) {
                        result["maskExport"] = QJsonObject{{"tool", "get_module_output"},
                                                           {"runId", runId},
                                                           {"instanceId", instanceId},
                                                           {fromPort ? "port" : "key", selected},
                                                           {"totalChars", region.maskRle.size()},
                                                           {"chunkChars", 8192},
                                                           {"encoding", "json-rle-column-major"},
                                                           {"coordinateSystem", "image_pixels"},
                                                           {"width", output.width()},
                                                           {"height", output.height()}};
                    }
                    if (params.contains(QStringLiteral("maskOffset"))) {
                        const QJsonValue offsetValue = params.value(QStringLiteral("maskOffset"));
                        const int offset = offsetValue.toInt(-1);
                        if (!offsetValue.isDouble() || offset < 0 || offset > region.maskRle.size())
                            return makeToolError(QStringLiteral("maskOffset is outside the mask RLE range"),
                                                 QStringLiteral("INVALID_PARAM"));
                        const QString chunk = region.maskRle.mid(offset, 8192);
                        result["maskRleChunk"] = chunk;
                        result["nextMaskOffset"] = offset + chunk.size();
                    }
                }
                if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kOutputResponseLimit &&
                    result.contains("preview")) {
                    result.remove("preview");
                    result["previewTruncated"] = true;
                }
                if (result.contains(QStringLiteral("polygonPage"))) {
                    QJsonObject polygonPage = result.value(QStringLiteral("polygonPage")).toObject();
                    QJsonArray localPoints = polygonPage.value(QStringLiteral("points")).toArray();
                    QJsonObject sourceCoordinates =
                        polygonPage.value(QStringLiteral("sourceImageCoordinates")).toObject();
                    QJsonArray sourcePoints = sourceCoordinates.value(QStringLiteral("points")).toArray();
                    while (!localPoints.isEmpty() &&
                           QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kOutputResponseLimit) {
                        localPoints.removeLast();
                        if (!sourcePoints.isEmpty())
                            sourcePoints.removeLast();
                        polygonPage[QStringLiteral("points")] = localPoints;
                        polygonPage[QStringLiteral("truncated")] = true;
                        if (!sourceCoordinates.isEmpty()) {
                            sourceCoordinates[QStringLiteral("points")] = sourcePoints;
                            polygonPage[QStringLiteral("sourceImageCoordinates")] = sourceCoordinates;
                        }
                        result[QStringLiteral("polygonPage")] = polygonPage;
                    }
                    if (localPoints.isEmpty() && polygonPage.contains(QStringLiteral("sourceImageCoordinates"))) {
                        polygonPage.remove(QStringLiteral("sourceImageCoordinates"));
                        result[QStringLiteral("polygonPage")] = polygonPage;
                    }
                }
                if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kOutputResponseLimit) {
                    result.remove("value");
                    result["status"] = QStringLiteral("truncated");
                    result["hint"] = QStringLiteral("Output exceeds 64 KiB; no inline value returned");
                }
            }
        }
        if (pointSurfaceFootScalarWorld3D &&
            result.value(QStringLiteral("status")).toString() == QLatin1String("available") &&
            result.contains(QStringLiteral("value")))
            result[QStringLiteral("coordinateFrame")] = QStringLiteral("World3D");
        return result;
    }

    return makeToolError(QStringLiteral("No %1 '%2' for module %3")
                             .arg(fromPort ? QStringLiteral("port") : QStringLiteral("key"), selected, instanceId),
                         QStringLiteral("OUTPUT_NOT_FOUND"));
}

// 阶段 3（G3）：有界、非阻塞等待——超时返回 pending/timeout，不把超时当失败
QJsonObject AgentActor::waitForRun(const QJsonObject& params) {
    QJsonObject result = getRunResults(params);
    if (result.contains(QStringLiteral("error")))
        return result;
    if (!result.contains(QStringLiteral("runId")))
        return makeToolError(QStringLiteral("No run to wait for"), QStringLiteral("RUN_NOT_FOUND"));
    if (result.value(QStringLiteral("isBusy")).toBool()) {
        result["status"] = QStringLiteral("pending");
        result["retryAfterMs"] = 50;
    }
    return result;
}

QJsonObject AgentActor::openProject(const QJsonObject& params) {
    QString path = params.value("path").toString();
    if (path.isEmpty())
        return makeToolError("Missing 'path' parameter", QStringLiteral("MISSING_PARAM"));

    // 阶段1 §3：未保存工程切换保护（默认 cancel，零副作用保留当前工程）
    const QJsonObject guard = guardUnsavedProject(params);
    if (!guard.isEmpty())
        return guard;

    // ProjectManager::openProject 已重构为先验证候选后替换（F07）：
    // 打开失败时当前工程、修改标记与撤销历史保持可用
    Project* opened = ProjectManager::instance().openProject(path);
    if (!opened)
        return makeToolError(QStringLiteral("Failed to open project: %1").arg(path), QStringLiteral("OPEN_FAILED"));

    Project* proj = ProjectManager::instance().currentProject();
    return QJsonObject{{"status", "opened"}, {"name", proj ? proj->name() : "unknown"}};
}

QJsonObject AgentActor::pauseFlow(const QJsonObject& params) {
    Q_UNUSED(params);
    RunEngine& engine = RunEngine::instance();
    FlowExecutionService& service = FlowExecutionService::instance();
    if (!service.isActive() && !engine.isRunning())
        return toolNoOp(QStringLiteral("engine not running; nothing to pause"));
    if (engine.isPaused() || service.state() == FlowExecutionService::State::Paused)
        return toolNoOp(QStringLiteral("already paused"));
    const bool applied = service.isActive() ? service.pauseRun() : (engine.pause(), engine.isPaused());
    if (!applied)
        return toolNoOp(QStringLiteral("flow state changed before pause was applied"));
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("paused")}};
}

QJsonObject AgentActor::resumeFlow(const QJsonObject& params, const QJsonObject& folderDeleteTargetAuthorization) {
    Q_UNUSED(params);
    RunEngine& engine = RunEngine::instance();
    const RunResumeSnapshot snapshot = engine.resumeSnapshot();
    if (snapshot.state != RunState::Paused)
        return toolNoOp(QStringLiteral("not paused"));
    return resumeFlowForSnapshot(snapshot.runId, snapshot.sessionId, static_cast<int>(snapshot.state),
                                 snapshot.lifecycleGeneration, folderDeleteTargetAuthorization);
}

void AgentActor::bindConfirmedResumeSnapshot(const QString& runId, const QString& sessionId, int state,
                                             int lifecycleGeneration) {
    m_confirmedResumeSnapshotValid = true;
    m_confirmedResumeRunId = runId;
    m_confirmedResumeSessionId = sessionId;
    m_confirmedResumeState = state;
    m_confirmedResumeLifecycleGeneration = lifecycleGeneration;
}

void AgentActor::clearConfirmedResumeSnapshot() {
    m_confirmedResumeSnapshotValid = false;
    m_confirmedResumeRunId.clear();
    m_confirmedResumeSessionId.clear();
    m_confirmedResumeState = 0;
    m_confirmedResumeLifecycleGeneration = 0;
}

QJsonObject AgentActor::resumeFlowForSnapshot(const QString& runId, const QString& sessionId, int state,
                                              int lifecycleGeneration,
                                              const QJsonObject& folderDeleteTargetAuthorization) {
    if (static_cast<RunState>(state) != RunState::Paused)
        return toolNoOp(QStringLiteral("not paused when the resume confirmation was created"));
    const RunResumeSnapshot current = RunEngine::instance().resumeSnapshot();
    if (current.runId != runId || current.sessionId != sessionId || current.state != RunState::Paused ||
        current.lifecycleGeneration != lifecycleGeneration)
        return makeToolError(QStringLiteral("Paused run changed before resume could start"),
                             QStringLiteral("CONFIRMATION_EXPIRED"));
    FlowExecutionService& service = FlowExecutionService::instance();
    if (service.isActive()) {
        if (!service.continueRun(folderDeleteTargetAuthorization))
            return makeToolError(QStringLiteral("Paused run changed before resume could start"),
                                 QStringLiteral("CONFIRMATION_EXPIRED"));
    } else if (!RunEngine::instance().resumeExpected(current, folderDeleteTargetAuthorization)) {
        return makeToolError(QStringLiteral("Paused run changed before resume could start"),
                             QStringLiteral("CONFIRMATION_EXPIRED"));
    }
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("resumed")}};
}

QString AgentActor::userKnowledgeDir() {
    const QString dir = PathUtils::appDataPath() + QStringLiteral("/knowledge");
    PathUtils::ensureDirExists(dir);
    return dir;
}

namespace {
QStringList builtInKnowledgeDocumentPaths() {
    ensureAgentResources();
    QStringList paths;
    const QDir builtin(QStringLiteral(":/agent/knowledge"));
    const QStringList builtinNames = builtin.entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
    for (const QString& name : builtinNames)
        paths.append(builtin.filePath(name));
    if (paths.isEmpty()) {
        for (const QString& path :
             {QDir::currentPath() + QStringLiteral("/src/core/agent/knowledge/DEEPLUX.md"),
              QCoreApplication::applicationDirPath() + QStringLiteral("/../src/core/agent/knowledge/DEEPLUX.md")}) {
            if (QFileInfo::exists(path))
                paths.append(QFileInfo(path).absoluteFilePath());
        }
    }
    return paths;
}
} // namespace

QStringList AgentActor::knowledgeDocumentPaths() {
    QStringList paths = builtInKnowledgeDocumentPaths();

    // 用户知识库目录（Agent 设置 → 知识库 添加的文档）
    const QDir userDir(userKnowledgeDir());
    const QStringList userNames = userDir.entryList(
        {QStringLiteral("*.md"), QStringLiteral("*.markdown"), QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
    for (const QString& name : userNames) {
        const QString path = userDir.filePath(name);
        const QFileInfo info(path);
        if (info.isFile() && !info.isSymLink())
            paths.append(path);
    }

    return paths;
}

QJsonArray AgentActor::listKnowledgeDocuments() {
    QJsonArray documents;
    const QStringList builtinPaths = builtInKnowledgeDocumentPaths();
    for (const QString& path : knowledgeDocumentPaths()) {
        const QFileInfo info(path);
        const bool builtin = builtinPaths.contains(path);
        const QString name = info.fileName();
        documents.append(QJsonObject{
            {QStringLiteral("documentId"),
             QStringLiteral("%1:%2").arg(builtin ? QStringLiteral("builtin") : QStringLiteral("user"), name)},
            {QStringLiteral("name"), name},
            {QStringLiteral("source"), builtin ? QStringLiteral("builtin") : QStringLiteral("user")},
            {QStringLiteral("sizeBytes"), info.size()},
            {QStringLiteral("removable"), !builtin}});
    }
    return documents;
}

namespace {
QString knowledgePathForId(const QString& documentId) {
    const bool builtin = documentId.startsWith(QStringLiteral("builtin:"));
    const bool user = documentId.startsWith(QStringLiteral("user:"));
    if (!builtin && !user)
        return {};
    const QString name = documentId.mid(documentId.indexOf(QLatin1Char(':')) + 1);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
        name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || QFileInfo(name).fileName() != name)
        return {};
    QString path;
    if (builtin) {
        for (const QString& candidate : builtInKnowledgeDocumentPaths()) {
            if (QFileInfo(candidate).fileName() == name) {
                path = candidate;
                break;
            }
        }
    } else {
        path = QDir(AgentActor::userKnowledgeDir()).filePath(name);
    }
    if (!AgentActor::knowledgeDocumentPaths().contains(path))
        return {};
    return path;
}

bool isWithinKnowledgeDirectory(const QString& path) {
    const QString root = QFileInfo(AgentActor::userKnowledgeDir()).canonicalFilePath();
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return !root.isEmpty() && !resolved.isEmpty() && resolved.startsWith(root + QDir::separator());
}

QMutex& knowledgeImportMutex() {
    static QMutex mutex;
    return mutex;
}
} // namespace

QJsonObject AgentActor::previewKnowledgeDocument(const QString& documentId) {
    const QString path = knowledgePathForId(documentId);
    if (path.isEmpty())
        return makeToolError(QStringLiteral("Knowledge document was not found"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_NOT_FOUND"));
    const QFileInfo info(path);
    const bool builtin = documentId.startsWith(QStringLiteral("builtin:"));
    if (!info.isFile() || info.isSymLink() || (!builtin && !isWithinKnowledgeDirectory(path)))
        return makeToolError(QStringLiteral("Knowledge document is not a regular in-library file"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_UNREADABLE"));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return makeToolError(QStringLiteral("Knowledge document could not be read"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_UNREADABLE"));
    constexpr qint64 previewLimit = 512 * 1024;
    QByteArray bytes = file.read(previewLimit + 1);
    if (file.error() != QFileDevice::NoError)
        return makeToolError(QStringLiteral("Knowledge document could not be read"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_UNREADABLE"));
    const bool truncated = bytes.size() > previewLimit;
    if (truncated)
        bytes.resize(previewLimit);
    return QJsonObject{{QStringLiteral("documentId"), documentId},
                       {QStringLiteral("name"), info.fileName()},
                       {QStringLiteral("content"), QString::fromUtf8(bytes)},
                       {QStringLiteral("truncated"), truncated},
                       {QStringLiteral("sizeBytes"), info.size()}};
}

QJsonObject AgentActor::addKnowledgeDocument(const QString& sourcePath) {
    const QFileInfo sourceInfo(sourcePath);
    const QString suffix = sourceInfo.suffix().toLower();
    if (sourcePath.trimmed().isEmpty() || !sourceInfo.isFile() || sourceInfo.isSymLink() || !sourceInfo.isReadable() ||
        (suffix != QStringLiteral("md") && suffix != QStringLiteral("markdown") && suffix != QStringLiteral("txt")))
        return makeToolError(QStringLiteral("Source must be a readable regular .md, .markdown, or .txt file"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_SOURCE_INVALID"));

    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly))
        return makeToolError(QStringLiteral("Knowledge document source could not be read"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_SOURCE_INVALID"));
    const QByteArray bytes = source.readAll();
    if (source.error() != QFileDevice::NoError)
        return makeToolError(QStringLiteral("Knowledge document source could not be read"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_SOURCE_INVALID"));

    const QString name = sourceInfo.fileName();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..") ||
        name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return makeToolError(QStringLiteral("Knowledge document filename is invalid"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_SOURCE_INVALID"));

    QMutexLocker locker(&knowledgeImportMutex());
    const QString userDir = userKnowledgeDir();
    const QString target = QDir(userDir).filePath(name);
    const QFileInfo targetInfo(target);
    if (targetInfo.exists() || targetInfo.isSymLink())
        return makeToolError(QStringLiteral("A knowledge document with this filename already exists"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_EXISTS"));

    QSaveFile output(target);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
        output.cancelWriting();
        return makeToolError(QStringLiteral("Knowledge document could not be imported"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_WRITE_FAILED"));
    }
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("added")},
                       {QStringLiteral("documentId"), QStringLiteral("user:%1").arg(name)},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("sizeBytes"), bytes.size()}};
}

QJsonObject AgentActor::removeKnowledgeDocument(const QString& documentId) {
    if (!documentId.startsWith(QStringLiteral("user:")))
        return makeToolError(QStringLiteral("Only user knowledge documents can be removed"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_READ_ONLY"));
    const QString path = knowledgePathForId(documentId);
    if (path.isEmpty())
        return makeToolError(QStringLiteral("Knowledge document was not found"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_NOT_FOUND"));
    QMutexLocker locker(&knowledgeImportMutex());
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || !isWithinKnowledgeDirectory(path))
        return makeToolError(QStringLiteral("Knowledge document is not a regular in-library file"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_NOT_FOUND"));
    if (!QFile::remove(path))
        return makeToolError(QStringLiteral("Knowledge document could not be removed"),
                             QStringLiteral("KNOWLEDGE_DOCUMENT_REMOVE_FAILED"));
    return QJsonObject{{QStringLiteral("status"), QStringLiteral("removed")},
                       {QStringLiteral("documentId"), documentId},
                       {QStringLiteral("name"), info.fileName()}};
}

QJsonObject AgentActor::readDocumentation(const QJsonObject& params) {
    QString topic = params.value("topic").toString().toLower();
    if (topic.isEmpty()) {
        return makeToolError("Missing 'topic' parameter", QStringLiteral("MISSING_PARAM"));
    }

    const auto parseInteger = [&params](const QString& key, int defaultValue, int minimum, int maximum,
                                        int* value) -> bool {
        const QJsonValue input = params.value(key);
        if (input.isUndefined()) {
            *value = defaultValue;
            return true;
        }
        if (!input.isDouble())
            return false;
        const double number = input.toDouble();
        if (!std::isfinite(number) || std::floor(number) != number || number < minimum || number > maximum)
            return false;
        *value = static_cast<int>(number);
        return true;
    };
    int offset = 0;
    int limit = 10;
    if (!parseInteger(QStringLiteral("offset"), 0, 0, std::numeric_limits<int>::max(), &offset) ||
        !parseInteger(QStringLiteral("limit"), 10, 1, 50, &limit))
        return makeToolError(
            QStringLiteral("offset must be a non-negative integer and limit must be an integer in [1,50]"),
            QStringLiteral("INVALID_PARAM"));

    const QStringList knowledgePaths = knowledgeDocumentPaths();
    if (knowledgePaths.isEmpty()) {
        // 内嵌摘要作为 fallback
        return QJsonObject{{"topic", topic},
                           {"content", "Knowledge base not found. Use get_module_params_schema(instanceId) "
                                       "to discover parameters for specific modules."},
                           {"sections", 0},
                           {"offset", offset},
                           {"limit", limit},
                           {"totalSections", 0},
                           {"hasMore", false}};
    }

    // 遍历全部知识库文档（内置 + 用户添加），按二、三级标题分节，匹配 topic 关键词。
    // 插件手册以三级标题组织具体模块；细分后可避免分类长段落截断掉靠后的模块。
    QStringList relevant;
    const QRegularExpression sectionStart(QStringLiteral("(?=^#{2,3}\\s+)"), QRegularExpression::MultilineOption);
    for (const QString& path : knowledgePaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        const QString content = QString::fromUtf8(file.readAll());
        file.close();

        const QString docName = QFileInfo(path).fileName();
        const QStringList sections = content.split(sectionStart, Qt::SkipEmptyParts);
        for (const QString& sec : sections) {
            if (sec.toLower().contains(topic) || topic == "all") {
                // 截断过长段落
                QString body =
                    sec.length() > 2000 ? sec.left(2000) + "\n... (truncated, use more specific topic)" : sec.trimmed();
                relevant.append(QStringLiteral("[来源: %1]\n%2").arg(docName, body));
            }
        }
    }

    const int totalSections = relevant.size();
    const QStringList page = relevant.mid(offset, limit);
    if (totalSections == 0) {
        return QJsonObject{{"topic", topic},
                           {"content", QString("No documentation found for '%1'. "
                                               "Try broader topics like 'workflow', 'modules', 'params', "
                                               "or a module name like 'FindCircle'. Users can add custom "
                                               "documents via Agent Settings → Knowledge Base.")
                                           .arg(topic)},
                           {"sections", 0},
                           {"offset", offset},
                           {"limit", limit},
                           {"totalSections", 0},
                           {"hasMore", false}};
    }

    return QJsonObject{{"topic", topic},
                       {"sections", page.size()},
                       {"content", page.join("\n\n")},
                       {"offset", offset},
                       {"limit", limit},
                       {"totalSections", totalSections},
                       {"hasMore", static_cast<qint64>(offset) + page.size() < totalSections}};
}

// --- SAM 批3：Agent 工具化 SAM（共享 SamPredictor 同步预测核心）---

QJsonObject AgentActor::samPredict(const QJsonObject& params) {
    const QString serverUrl = params.value("serverUrl").toString(QStringLiteral("http://127.0.0.1:8990"));
    const int timeoutMs = params.value("timeoutMs").toInt(30000);

    SamPromptItem item;
    item.label = params.value("label").toString(QStringLiteral("object"));
    QString perr;
    if (!parseSamPointsJson(params.value("points"), item.positivePoints, true, perr))
        return makeToolError(QString("points 非法: %1").arg(perr), QStringLiteral("INVALID_PARAM"));
    if (!parseSamPointsJson(params.value("negativePoints"), item.negativePoints, true, perr))
        return makeToolError(QString("negativePoints 非法: %1").arg(perr), QStringLiteral("INVALID_PARAM"));
    if (!parseSamBoxJson(params.value("box"), item.box, perr))
        return makeToolError(QString("box 非法: %1").arg(perr), QStringLiteral("INVALID_PARAM"));
    if (item.positivePoints.isEmpty() && !item.box.isValid())
        return makeToolError(QString("points 与 box 至少提供一个 prompt"), QStringLiteral("INVALID_PARAM"));
    // 批4（C）：候选选择（0=最佳；上界越界由 SamPredictor 按实际候选数失败关闭）
    const QJsonValue candIdx = params.value("candidateIndex");
    if (!candIdx.isUndefined() && !candIdx.isNull()) {
        if (!candIdx.isDouble() || candIdx.toDouble() < 0 || candIdx.toDouble() > 7 ||
            candIdx.toDouble() != double(qRound(candIdx.toDouble())))
            return makeToolError(QString("candidateIndex 必须为 [0,7] 整数"), QStringLiteral("INVALID_PARAM"));
        item.candidateIndex = qRound(candIdx.toDouble());
    }

    // imagePath 存在性/serverUrl/timeoutMs 由 runSamPredictBatch 先行校验（不发网络请求）
    const SamPredictOutcome outcome =
        runSamPredictBatch(serverUrl, params.value("imagePath").toString(), {item}, timeoutMs);
    if (!outcome.ok && !outcome.uncertainStage.isEmpty())
        return makeSamUncertainToolResult(outcome);
    if (!outcome.ok)
        return makeToolError(outcome.error, QStringLiteral("PRECONDITION_FAILED"));

    const Region2D& region = outcome.regions.first();
    QJsonArray polygon;
    for (const QPointF& p : region.polygon)
        polygon.append(QJsonArray{p.x(), p.y()});
    // 批4（C）：候选摘要回传（score 列表，降序）——LLM 可据此改选 candidateIndex
    QJsonArray candidateScores;
    for (const double s : outcome.candidateScores.value(0))
        candidateScores.append(s);
    // 不回传 maskRle 本体（可达 MB 级，会撑爆 LLM 上下文），仅给出长度
    return QJsonObject{
        {"status", QStringLiteral("predicted")},
        {"label", region.label},
        {"score", region.score},
        {"bbox", QJsonArray{region.bbox.x(), region.bbox.y(), region.bbox.width(), region.bbox.height()}},
        {"polygon", polygon},
        {"polygonPoints", region.polygon.size()},
        {"maskRleLength", region.maskRle.size()},
        {"modelName", outcome.modelName},
        {"candidateCount", candidateScores.size()},
        {"candidateScores", candidateScores},
        {"candidateIndex", item.candidateIndex}};
}

QJsonObject AgentActor::samAnnotate(const QJsonObject& params) {
    const QString imagePath = params.value("imagePath").toString();
    const QString sessionPath = params.value("sessionPath").toString();
    const QString serverUrl = params.value("serverUrl").toString(QStringLiteral("http://127.0.0.1:8990"));
    const int timeoutMs = params.value("timeoutMs").toInt(30000);

    if (sessionPath.trimmed().isEmpty())
        return makeToolError(QString("sessionPath 不能为空"), QStringLiteral("INVALID_PARAM"));
    // 批4（B）：prompt 来源二选一——items 或 labelMePath（互斥且必选其一）
    const QString labelMePath = params.value("labelMePath").toString().trimmed();
    const QJsonValue itemsValue = params.value("items");
    const bool hasItems = itemsValue.isArray();
    if (hasItems == !labelMePath.isEmpty())
        return makeToolError(QString("items 与 labelMePath 必须恰好提供一个（互斥）"), QStringLiteral("INVALID_PARAM"));

    QList<SamPromptItem> prompts;
    int importedRectangles = 0;
    int skippedShapes = 0;
    if (!labelMePath.isEmpty()) {
        // LabelMe 矩形框 → box prompt 批量：严格导入失败关闭；非矩形/退化
        // 形状跳过并计数，结果中显式报告（不静默）
        QList<LabelMeShape> shapes;
        QString importError;
        if (!LabelMeImporter::loadShapes(labelMePath, shapes, &importError))
            return makeToolError(QString("LabelMe 导入失败：%1").arg(importError), QStringLiteral("INVALID_PARAM"));
        const QList<LabelMeBox> boxes = LabelMeImporter::rectangleBoxes(shapes, &skippedShapes);
        if (boxes.isEmpty())
            return makeToolError(QString("LabelMe 无可用矩形框（跳过非矩形/退化形状 %1 个）").arg(skippedShapes),
                                 QStringLiteral("INVALID_PARAM"));
        if (boxes.size() > 16)
            return makeToolError(QString("LabelMe 矩形数 %1 超出 [1,16]").arg(boxes.size()),
                                 QStringLiteral("INVALID_PARAM"));
        for (const LabelMeBox& labelMeBox : boxes) {
            SamPromptItem item;
            item.label = labelMeBox.label;
            item.box = labelMeBox.box;
            prompts.append(item);
        }
        importedRectangles = boxes.size();
    } else {
        const QJsonArray items = itemsValue.toArray();
        if (items.isEmpty() || items.size() > 16)
            return makeToolError(QString("items 数量必须在 [1,16]"), QStringLiteral("INVALID_PARAM"));
        for (int i = 0; i < items.size(); ++i) {
            if (!items[i].isObject())
                return makeToolError(QString("items[%1] 必须为对象").arg(i), QStringLiteral("INVALID_PARAM"));
            const QJsonObject itemObj = items[i].toObject();
            SamPromptItem item;
            item.label = itemObj.value("label").toString(QStringLiteral("object"));
            QString perr;
            if (!parseSamPointsJson(itemObj.value("points"), item.positivePoints, true, perr))
                return makeToolError(QString("items[%1].points 非法: %2").arg(i).arg(perr),
                                     QStringLiteral("INVALID_PARAM"));
            if (!parseSamPointsJson(itemObj.value("negativePoints"), item.negativePoints, true, perr))
                return makeToolError(QString("items[%1].negativePoints 非法: %2").arg(i).arg(perr),
                                     QStringLiteral("INVALID_PARAM"));
            if (!parseSamBoxJson(itemObj.value("box"), item.box, perr))
                return makeToolError(QString("items[%1].box 非法: %2").arg(i).arg(perr),
                                     QStringLiteral("INVALID_PARAM"));
            if (item.positivePoints.isEmpty() && !item.box.isValid())
                return makeToolError(QString("items[%1]：points 与 box 至少提供一个 prompt").arg(i),
                                     QStringLiteral("INVALID_PARAM"));
            // 批4（C）：每项可选 candidateIndex（0=最佳；越界由 SamPredictor 失败关闭）
            const QJsonValue candIdx = itemObj.value("candidateIndex");
            if (!candIdx.isUndefined() && !candIdx.isNull()) {
                if (!candIdx.isDouble() || candIdx.toDouble() < 0 || candIdx.toDouble() > 7 ||
                    candIdx.toDouble() != double(qRound(candIdx.toDouble())))
                    return makeToolError(QString("items[%1].candidateIndex 必须为 [0,7] 整数").arg(i),
                                         QStringLiteral("INVALID_PARAM"));
                item.candidateIndex = qRound(candIdx.toDouble());
            }
            prompts.append(item);
        }
    }

    // 图像必须可读：会话需要记录真实宽高（不伪造尺寸）
    const QImage image(imagePath);
    if (imagePath.isEmpty() || image.isNull())
        return makeToolError(QString("无法读取图像：%1").arg(imagePath), QStringLiteral("PRECONDITION_FAILED"));

    // 既有会话：追加语义；加载失败、绑定异图、同路径尺寸不符或内容摘要不符
    // 均失败关闭（不覆盖用户数据，不把旧坐标系标注追加进新图）
    AnnotationSession session;
    const QString digest = AnnotationSession::computeImageDigest(image);
    if (QFileInfo::exists(sessionPath)) {
        QString loadError;
        session = AnnotationSession::load(sessionPath, &loadError);
        if (!loadError.isEmpty())
            return makeToolError(QString("标注会话加载失败：%1").arg(loadError), QStringLiteral("PRECONDITION_FAILED"));
        if (!session.imagePath.isEmpty() && session.imagePath != imagePath)
            return makeToolError(QString("会话已绑定其他图像（%1），拒绝混图追加").arg(session.imagePath),
                                 QStringLiteral("PRECONDITION_FAILED"));
        // 批3 复核二轮（P1-4）：同路径 ≠ 同图——文件被替换后旧标注属于旧坐标系，
        // 尺寸与像素内容摘要（AnnotationSession::computeImageDigest）双重校验
        if (session.imageWidth > 0 && session.imageHeight > 0 &&
            (session.imageWidth != image.width() || session.imageHeight != image.height()))
            return makeToolError(QString("同路径图像尺寸与既有会话不一致（旧 %1x%2 vs 新 %3x%4），拒绝追加")
                                     .arg(session.imageWidth)
                                     .arg(session.imageHeight)
                                     .arg(image.width())
                                     .arg(image.height()),
                                 QStringLiteral("PRECONDITION_FAILED"));
        // 批3 复核三轮（P1-1）：摘要策略收紧——有摘要必须一致；无摘要且已含
        // 标注 = 无法证明图像未被替换，拒绝追加（不静默把旧坐标系标注重绑到
        // 新图）；仅空会话允许补记摘要
        if (!session.imageDigest.isEmpty()) {
            if (session.imageDigest != digest)
                return makeToolError(QString("同路径图像内容已变化（摘要不符），拒绝把旧坐标系标注追加进新图"),
                                     QStringLiteral("PRECONDITION_FAILED"));
        } else if (!session.annotations.isEmpty()) {
            return makeToolError(QString("既有会话缺少图像摘要且已含标注，无法校验图像未被替换，拒绝追加"),
                                 QStringLiteral("PRECONDITION_FAILED"));
        }
    }
    session.imageDigest = digest; // 仅三种路径到达：新文件 / 摘要一致 / 空会话补记

    // 全有或全无：任一 prompt 预测失败则不写文件
    const SamPredictOutcome outcome = runSamPredictBatch(serverUrl, imagePath, prompts, timeoutMs);
    if (!outcome.ok) {
        if (!outcome.uncertainStage.isEmpty())
            return makeSamUncertainToolResult(outcome);
        QJsonObject result = makeToolError(outcome.error, QStringLiteral("PRECONDITION_FAILED"));
        if (outcome.failedIndex >= 0)
            result.insert(QStringLiteral("failedItem"), outcome.failedIndex);
        return result;
    }

    const QString modelName = outcome.modelName.isEmpty() ? QStringLiteral("sam") : outcome.modelName;
    QJsonArray objectIds;
    for (int i = 0; i < outcome.regions.size(); ++i) {
        const Region2D& region = outcome.regions[i];
        AnnotationObject obj;
        obj.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        obj.label = region.label;
        obj.bbox = region.bbox;
        obj.polygon = region.polygon;
        obj.maskRle = region.maskRle;
        obj.score = region.score;
        obj.prompts.pointsPos = prompts[i].positivePoints;
        obj.prompts.pointsNeg = prompts[i].negativePoints;
        if (prompts[i].box.isValid())
            obj.prompts.box = prompts[i].box;
        obj.modelName = modelName;
        session.annotations.append(obj);
        objectIds.append(obj.id);
    }
    session.imagePath = imagePath;
    session.imageWidth = image.width();
    session.imageHeight = image.height();
    if (session.modelName.isEmpty())
        session.modelName = modelName;

    QString saveError;
    if (!session.save(sessionPath, &saveError))
        return makeToolError(QString("标注会话保存失败：%1").arg(saveError), QStringLiteral("PRECONDITION_FAILED"));

    QJsonObject result{{"status", QStringLiteral("annotated")},
                       {"added", objectIds.size()},
                       {"total", session.annotations.size()},
                       {"sessionPath", sessionPath},
                       {"objectIds", objectIds}};
    if (importedRectangles > 0) {
        // 批4（B）：LabelMe 导入统计显式回传（跳过数不为 0 时一并报告，不静默）
        result.insert(QStringLiteral("importedRectangles"), importedRectangles);
        result.insert(QStringLiteral("skippedShapes"), skippedShapes);
    }
    return result;
}

QJsonObject AgentActor::getAnnotationSession(const QJsonObject& params) {
    constexpr qint64 kMaxSessionBytes = 64LL * 1024 * 1024;
    constexpr qint64 kMaxResponseBytes = 64 * 1024;
    constexpr int kMaxPageSize = 50;

    const QJsonValue pathValue = params.value(QStringLiteral("sessionPath"));
    if (!pathValue.isString() || pathValue.toString().isEmpty())
        return makeToolError(QStringLiteral("sessionPath must be a non-empty string"), QStringLiteral("INVALID_PARAM"));

    auto readInteger = [&params](const QString& key, int defaultValue, int maximum, int* out) {
        const QJsonValue value = params.value(key);
        if (value.isUndefined()) {
            *out = defaultValue;
            return true;
        }
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0 ||
            value.toDouble() > maximum || std::floor(value.toDouble()) != value.toDouble())
            return false;
        *out = int(value.toDouble());
        return true;
    };

    int offset = 0;
    int limit = 20;
    if (!readInteger(QStringLiteral("offset"), 0, std::numeric_limits<int>::max(), &offset) ||
        !readInteger(QStringLiteral("limit"), 20, kMaxPageSize, &limit) || limit < 1)
        return makeToolError(QStringLiteral("offset must be a non-negative integer and limit must be in [1, 50]"),
                             QStringLiteral("INVALID_PARAM"));

    const QJsonValue polygonValue = params.value(QStringLiteral("includePolygon"));
    if (!polygonValue.isUndefined() && !polygonValue.isBool())
        return makeToolError(QStringLiteral("includePolygon must be a boolean"), QStringLiteral("INVALID_PARAM"));
    const bool includePolygon = polygonValue.toBool(false);

    const QString sessionPath = pathValue.toString();
    const QFileInfo info(sessionPath);
    if (!info.exists())
        return makeToolError(QStringLiteral("Annotation session file was not found"), QStringLiteral("FILE_NOT_FOUND"));
    if (!info.isFile() || info.isSymLink())
        return makeToolError(QStringLiteral("Annotation session path must name a regular file"),
                             QStringLiteral("PRECONDITION_FAILED"));

    QFile file(sessionPath);
    if (!file.open(QIODevice::ReadOnly))
        return makeToolError(QStringLiteral("Annotation session file is not readable"),
                             QStringLiteral("PRECONDITION_FAILED"));

    QString loadError;
    AnnotationSession::LoadResult loadResult = AnnotationSession::LoadResult::IoError;
    const AnnotationSession session =
        AnnotationSession::loadFromDevice(file, kMaxSessionBytes, &loadResult, &loadError);
    if (loadResult == AnnotationSession::LoadResult::SizeLimitExceeded)
        return makeToolError(QStringLiteral("Annotation session exceeds the 64 MiB Agent read limit"),
                             QStringLiteral("INPUT_LIMIT_EXCEEDED"));
    if (loadResult == AnnotationSession::LoadResult::IoError)
        return makeToolError(QStringLiteral("Annotation session could not be read: %1").arg(loadError),
                             QStringLiteral("PRECONDITION_FAILED"));
    if (loadResult != AnnotationSession::LoadResult::Success)
        return makeToolError(QStringLiteral("Invalid annotation session: %1").arg(loadError),
                             QStringLiteral("INVALID_SESSION"));

    const int total = session.annotations.size();
    const int first = std::min(offset, total);
    const int last = int(std::min<qint64>(total, qint64(first) + limit));
    QJsonArray annotations;
    for (int i = first; i < last; ++i) {
        const AnnotationObject& object = session.annotations[i];
        const AnnotationPrompt& prompt = object.prompts;
        QJsonObject item{
            {QStringLiteral("id"), object.id},
            {QStringLiteral("label"), object.label},
            {QStringLiteral("bbox"),
             QJsonArray{object.bbox.x(), object.bbox.y(), object.bbox.width(), object.bbox.height()}},
            {QStringLiteral("score"), object.score},
            {QStringLiteral("modelName"), object.modelName},
            {QStringLiteral("prompt"), QJsonObject{{QStringLiteral("positivePointCount"), prompt.pointsPos.size()},
                                                   {QStringLiteral("negativePointCount"), prompt.pointsNeg.size()},
                                                   {QStringLiteral("hasBox"), prompt.box.has_value()}}}};
        if (includePolygon) {
            QJsonArray polygon;
            for (const QPointF& point : object.polygon)
                polygon.append(QJsonArray{point.x(), point.y()});
            item.insert(QStringLiteral("polygon"), polygon);
        }
        annotations.append(item);
    }

    QJsonObject result{{QStringLiteral("status"), QStringLiteral("session_loaded")},
                       {QStringLiteral("sessionPath"), sessionPath},
                       {QStringLiteral("imagePath"), session.imagePath},
                       {QStringLiteral("imageWidth"), session.imageWidth},
                       {QStringLiteral("imageHeight"), session.imageHeight},
                       {QStringLiteral("modelName"), session.modelName},
                       {QStringLiteral("hasImageDigest"), !session.imageDigest.isEmpty()},
                       {QStringLiteral("total"), total},
                       {QStringLiteral("offset"), offset},
                       {QStringLiteral("limit"), limit},
                       {QStringLiteral("hasMore"), last < total},
                       {QStringLiteral("annotations"), annotations}};
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > kMaxResponseBytes)
        return makeToolError(QStringLiteral("Annotation session page exceeds 65536 UTF-8 bytes; lower limit or omit "
                                            "includePolygon"),
                             QStringLiteral("OUTPUT_LIMIT"));
    return result;
}


// --- 第一迭代：闭环 / 观测工具 ---

bool AgentActor::engineMatchesProject(const Project* project) const {
    if (!project)
        return false;
    RunEngine& engine = RunEngine::instance();
    const QList<ModuleInstance> modules = project->modules();
    const QList<ModuleBase*> engineModules = engine.modules();
    if (modules.size() != engineModules.size())
        return false;
    QSet<QString> expected;
    for (const ModuleInstance& inst : modules)
        expected.insert(inst.id);
    QSet<QString> actual;
    for (const ModuleBase* module : engineModules) {
        const QString name = module->instanceName().isEmpty() ? module->name() : module->instanceName();
        actual.insert(name);
    }
    return expected == actual;
}

bool AgentActor::ensureEngineLoaded(QString* error, bool forceReload) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project) {
        if (error)
            *error = QStringLiteral("No project opened");
        return false;
    }
    RunEngine& engine = RunEngine::instance();
    if (!forceReload && !m_engineProjectDirty && engineMatchesProject(project))
        return true;
    if (engine.isBusy()) {
        if (error)
            *error = QStringLiteral("Flow is running");
        return false;
    }
    QString loadError;
    QMetaObject::Connection errorConn = QObject::connect(&engine, &RunEngine::errorOccurred, &engine,
                                                         [&loadError](const QString& err) { loadError = err; });
    const bool loaded = engine.loadProject(project);
    QObject::disconnect(errorConn);
    if (!loaded) {
        if (error)
            *error = loadError.isEmpty() ? QStringLiteral("Failed to load project") : loadError;
        return false;
    }
    m_engineProjectDirty = false;
    return true;
}

AgentActor::RunWaitResult AgentActor::runAndWait(const QString& mode, int timeoutMs, bool forceReload) {
    Q_UNUSED(mode)
    RunWaitResult out;
    RunEngine& engine = RunEngine::instance();
    Project* project = ProjectManager::instance().currentProject();
    if (project) {
        QString loadError;
        if (!ensureEngineLoaded(&loadError, forceReload)) {
            out.error = loadError;
            return out;
        }
    }
    // 无工程时保持 run_flow 既有语义：直接使用引擎里已手动加载的模块
    if (engine.modules().isEmpty()) {
        out.error = project ? QStringLiteral("No modules to run") : QStringLiteral("No project opened");
        return out;
    }
    // 后台执行（设计 §8.2）：run_flow(wait=true) 通过 FlowExecutionService 启动 Single。
    const AgentRunWaiter::Outcome outcome =
        m_runWaiter->waitForRun(engine, timeoutMs, []() { return FlowExecutionService::instance().startSingle(); });
    out.started = !outcome.rejectedStart && !outcome.reentrantRejected;
    out.finished = outcome.finished;
    out.timedOut = outcome.timedOut;
    out.aborted = outcome.aborted;
    out.pausedAtBreakpoint = outcome.pausedAtBreakpoint;
    out.pausedByUser = outcome.pausedByUser;
    out.startFailed = outcome.startFailed;
    out.startError = outcome.startError;
    out.reentrant = outcome.reentrantRejected;
    out.rejectedStart = outcome.rejectedStart;
    out.failedModule = outcome.failedModule;
    out.elapsedMs = outcome.result.elapsedMs;
    out.ok = outcome.finished && outcome.result.success;
    if (outcome.finished && !outcome.result.success)
        out.error = outcome.result.errorMessage;
    out.totalRuns = engine.totalRuns();
    out.successRuns = engine.successRuns();
    if (outcome.finished) {
        // 后台执行（设计 §8.2）：引擎 runFinished 早于 Service job 收尾（endExternalJob），
        // 返回前等待 job 真正结束，保持 wait=true“返回即引擎空闲”的既有语义
        // （否则紧随其后的 get_node_runtime_outputs 等工具会读到 engine_busy）。
        // t13：收尾确认同样走事件循环；此时 worker 只做 endExternalJob，等待通常瞬间完成。
        (void) waitForServiceIdleViaEvents(FlowExecutionService::instance(), 5000);
    }
    if (outcome.timedOut || outcome.aborted) {
        // 超时/中止：优先 Service::stop()（job 级取消标志；无 Service job 时它内部回退引擎停止），
        // 并等待停止收尾，避免调用方紧接着读到忙状态。
        // t13 观察：这里必须在 GUI 事件循环内等待 jobFinished——同步 waitForIdle 会阻塞 GUI，
        // 而 worker 可能正阻塞在 BlockingQueuedConnection 等待 GUI 执行 gui 亲和模块（互等死锁）。
        FlowExecutionService::instance().stop();
        (void) waitForServiceIdleViaEvents(FlowExecutionService::instance(), 5000);
    }
    return out;
}

QJsonObject AgentActor::validateWorkflow(const QJsonObject& params) {
    Q_UNUSED(params)
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorObject(QStringLiteral("No project opened"), QStringLiteral("no_project"));
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy())
        return errorObject(QStringLiteral("Flow is running; stop it before validating"), QStringLiteral("engine_busy"));
    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return errorObject(loadError, QStringLiteral("engine_load_failed"));

    const int moduleCount = project->modules().size();
    QJsonArray issues;
    if (moduleCount == 0) {
        issues.append(QJsonObject{{QStringLiteral("code"), QStringLiteral("no_modules")},
                                  {QStringLiteral("node"), QString()},
                                  {QStringLiteral("message"), QStringLiteral("Project has no modules")}});
        return QJsonObject{
            {QStringLiteral("ok"), false}, {QStringLiteral("issues"), issues}, {QStringLiteral("moduleCount"), 0}};
    }
    for (const RunEngine::FlowIssue& issue : engine.validateFlowIssues()) {
        issues.append(QJsonObject{{QStringLiteral("code"), issue.code},
                                  {QStringLiteral("node"), issue.node},
                                  {QStringLiteral("port"), issue.port},
                                  {QStringLiteral("message"), issue.message}});
    }
    return QJsonObject{{QStringLiteral("ok"), issues.isEmpty()},
                       {QStringLiteral("issues"), issues},
                       {QStringLiteral("moduleCount"), moduleCount}};
}

QJsonObject AgentActor::getNodeRuntimeOutputs(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorObject(QStringLiteral("No project opened"), QStringLiteral("no_project"));
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy()) {
        return errorObject(QStringLiteral("Flow is running; stop it before reading node outputs"),
                           QStringLiteral("engine_busy"));
    }
    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return errorObject(loadError, QStringLiteral("engine_load_failed"));

    const QString nodeName = params.value("nodeName").toString();
    NodeRef ref;
    if (!resolveNodeRef(project, nodeName, &ref))
        return errorObject(QString("Module not found: %1").arg(nodeName), QStringLiteral("node_not_found"));

    const QMap<QString, RunEngine::ModuleRuntimeInfo> snapshot = engine.moduleRuntimeSnapshot();
    const RunEngine::ModuleRuntimeInfo info = snapshot.value(ref.runtimeName);
    const PortOutputsPayload payload = serializePortOutputs(engine.nodePortOutputs(ref.runtimeName));

    QJsonObject result{{QStringLiteral("node"), ref.runtimeName},
                       {QStringLiteral("instanceId"), ref.instanceId},
                       {QStringLiteral("frameId"), static_cast<double>(engine.lastFrameId())},
                       {QStringLiteral("outputs"), payload.outputs}};
    if (!payload.unsupported.isEmpty()) {
        QJsonArray unsupported;
        for (const QString& port : payload.unsupported)
            unsupported.append(port);
        result.insert(QStringLiteral("unsupportedPorts"), unsupported);
    }
    if (!info.valid) {
        result.insert(QStringLiteral("lastStatus"), QStringLiteral("unknown"));
        result.insert(QStringLiteral("note"), QStringLiteral("node has no runtime output yet; run the flow first"));
        return result;
    }
    QString status = QStringLiteral("success");
    if (info.skipped)
        status = QStringLiteral("skipped");
    else if (!info.success)
        status = QStringLiteral("failed");
    result.insert(QStringLiteral("lastStatus"), status);
    result.insert(QStringLiteral("elapsedMs"), info.elapsedMs);
    result.insert(QStringLiteral("finishedAt"), info.finishedAt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
    if (status == QStringLiteral("failed"))
        result.insert(QStringLiteral("errorMessage"), info.errorMessage);
    return result;
}

QJsonObject AgentActor::getExecutionLogs(const QJsonObject& params) {
    const QString category = params.value("category").toString().trimmed();
    const QString levelName = params.value("level").toString(QStringLiteral("debug")).trimmed().toLower();
    const QString keyword = params.value("keyword").toString();
    const int limit = qBound(1, params.value("limit").toInt(50), 200);

    LogLevel minLevel = LogLevel::Debug;
    if (levelName == QStringLiteral("info"))
        minLevel = LogLevel::Info;
    else if (levelName == QStringLiteral("warning"))
        minLevel = LogLevel::Warning;
    else if (levelName == QStringLiteral("error"))
        minLevel = LogLevel::Error;
    else if (levelName == QStringLiteral("success"))
        minLevel = LogLevel::Success;

    QList<LogEntry> matched;
    const QList<LogEntry> all = Logger::instance().logs();
    for (const LogEntry& entry : all) {
        if (!category.isEmpty() && entry.category != category)
            continue;
        if (static_cast<int>(entry.level) < static_cast<int>(minLevel))
            continue;
        if (!keyword.isEmpty() && !entry.message.contains(keyword, Qt::CaseInsensitive))
            continue;
        matched.append(entry);
    }

    QJsonArray entries;
    const int first = qMax(0, matched.size() - limit);
    for (int i = first; i < matched.size(); ++i) {
        entries.append(QJsonObject{
            {QStringLiteral("time"), matched[i].timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))},
            {QStringLiteral("level"), logLevelName(matched[i].level)},
            {QStringLiteral("category"), matched[i].category},
            {QStringLiteral("message"), matched[i].message}});
    }
    return QJsonObject{{QStringLiteral("entries"), entries},
                       {QStringLiteral("total"), matched.size()},
                       {QStringLiteral("truncated"), matched.size() > entries.size()},
                       {QStringLiteral("source"), QStringLiteral("Logger::memory")}};
}

QJsonObject AgentActor::tuneNodeAndVerify(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return QJsonObject{{"success", false}, {"error", "No project opened"}, {"code", "no_project"}};

    const QString nodeName = params.value("nodeName").toString();
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy()) {
        return QJsonObject{
            {"success", false}, {"error", "Flow is running; stop it before tuning"}, {"code", "engine_busy"}};
    }

    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return QJsonObject{{"success", false}, {"error", loadError}, {"code", "engine_load_failed"}};

    NodeRef ref;
    if (!resolveNodeRef(project, nodeName, &ref)) {
        return QJsonObject{
            {"success", false}, {"error", QString("Module not found: %1").arg(nodeName)}, {"code", "node_not_found"}};
    }

    const QJsonValue parametersValue = params.value("parameters");
    const QJsonObject patch = parametersValue.toObject();
    if (!parametersValue.isObject() || patch.isEmpty() || patch.size() > 32)
        return invalidParameter(QStringLiteral("'parameters' must be a non-empty object with 1..32 keys"), QString());

    const QString runMode = params.value("runMode").toString(QStringLiteral("flow"));
    if (runMode != QStringLiteral("flow") && runMode != QStringLiteral("node"))
        return invalidParameter(QString("Invalid runMode: %1").arg(runMode), QString());
    const int timeoutMs = qBound(1000, params.value("timeoutMs").toInt(60000), 300000);

    const std::optional<ModuleInstance> instOpt = project->moduleById(ref.instanceId);
    if (!instOpt) {
        return QJsonObject{
            {"success", false}, {"error", QString("Module not found: %1").arg(nodeName)}, {"code", "node_not_found"}};
    }
    std::unique_ptr<IModule> mod(PluginManager::instance().createModule(instOpt->moduleId));
    if (!mod) {
        return QJsonObject{{"success", false},
                           {"error", QString("Cannot create module for validation: %1").arg(instOpt->moduleId)},
                           {"code", "invalid_parameter"}};
    }

    // 预校验全部键：任一键失败 → 工程参数零修改
    const QJsonObject currentParams = mergedParams(mod.get(), *instOpt);
    QJsonObject normalized;
    QJsonObject merged = currentParams;
    for (auto it = patch.constBegin(); it != patch.constEnd(); ++it) {
        const ParamNormalizeResult conversion = normalizeParamValue(mod.get(), *instOpt, it.key(), it.value());
        if (!conversion.ok)
            return invalidParameter(conversion.error, it.key());
        normalized.insert(it.key(), conversion.value);
        merged.insert(it.key(), conversion.value);
        QString verr;
        if (!mod->validateParams(merged, verr))
            return invalidParameter(verr, it.key());
    }

    QJsonObject beforeValues;
    QJsonObject afterValues;
    for (auto it = normalized.constBegin(); it != normalized.constEnd(); ++it) {
        beforeValues.insert(it.key(), scalarToString(currentParams.value(it.key())));
        afterValues.insert(it.key(), scalarToString(merged.value(it.key())));
    }

    // 原子写入：宏内多条 set 命令，一次撤销回滚整组
    m_undoStack->beginMacro(QStringLiteral("Agent tune %1").arg(ref.runtimeName));
    for (auto it = normalized.constBegin(); it != normalized.constEnd(); ++it) {
        const bool hadOldValue = instOpt->params.contains(it.key());
        const QJsonValue oldVal = instOpt->params.value(it.key());
        m_undoStack->push(new AgentSetParamCmd(ref.instanceId, it.key(), hadOldValue, oldVal, it.value()));
    }
    m_undoStack->endMacro();
    m_engineProjectDirty = true;

    QJsonObject result{{QStringLiteral("node"), ref.runtimeName}, {QStringLiteral("instanceId"), ref.instanceId},
                       {QStringLiteral("runMode"), runMode},      {QStringLiteral("applied"), normalized.size()},
                       {QStringLiteral("before"), beforeValues},  {QStringLiteral("after"), afterValues},
                       {QStringLiteral("undoable"), true}};

    if (runMode == QStringLiteral("node")) {
        // 零副作用改参核对：不执行节点、不触发运行，返回上一帧输出
        const PortOutputsPayload payload = serializePortOutputs(engine.nodePortOutputs(ref.runtimeName));
        const RunEngine::ModuleRuntimeInfo info = engine.moduleRuntimeSnapshot().value(ref.runtimeName);
        QString status = QStringLiteral("unknown");
        if (info.valid)
            status = info.skipped ? QStringLiteral("skipped")
                                  : (info.success ? QStringLiteral("success") : QStringLiteral("failed"));
        result.insert(QStringLiteral("success"), true);
        result.insert(QStringLiteral("executed"), false);
        result.insert(QStringLiteral("staleOutputs"), payload.outputs);
        result.insert(QStringLiteral("lastStatus"), status);
        result.insert(QStringLiteral("note"), QStringLiteral("runMode=node does not execute the flow; outputs are from "
                                                             "the previous run. Use runMode=flow to verify."));
        return result;
    }

    const auto tuneFailure = [&](const QString& code, const QString& error, const QString& failedModule) {
        QJsonObject failure{
            {"success", false},
            {"code", code},
            {"error", error},
            {"node", ref.runtimeName},
            {"runMode", runMode},
            {"applied", normalized.size()},
            {"parametersRolledBack", false},
            {"note", QStringLiteral("Parameters were applied and kept; undo one step to roll back the whole patch.")},
            {"before", beforeValues},
            {"after", afterValues}};
        if (!failedModule.isEmpty())
            failure.insert(QStringLiteral("failedModule"), failedModule);
        return failure;
    };

    const RunWaitResult run = runAndWait(QStringLiteral("once"), timeoutMs, true);
    if (run.timedOut)
        return tuneFailure(QStringLiteral("timeout"), QString("Flow did not finish within %1 ms").arg(timeoutMs),
                           QString());
    if (run.aborted)
        return tuneFailure(QStringLiteral("cancelled"), QStringLiteral("Run aborted by user"), QString());
    if (run.reentrant) {
        return tuneFailure(QStringLiteral("reentrant_wait"),
                           QStringLiteral("Another run wait is already active on this thread"), QString());
    }
    if (run.pausedAtBreakpoint) {
        return tuneFailure(QStringLiteral("paused_at_breakpoint"),
                           QStringLiteral("Flow paused at a breakpoint; continue or stop it from the UI"), QString());
    }
    if (!run.started)
        return tuneFailure(QStringLiteral("run_failed"), run.error, run.failedModule);

    const PortOutputsPayload payload = serializePortOutputs(engine.nodePortOutputs(ref.runtimeName));
    const RunEngine::ModuleRuntimeInfo info = engine.moduleRuntimeSnapshot().value(ref.runtimeName);
    QString status = QStringLiteral("unknown");
    if (info.valid)
        status = info.skipped ? QStringLiteral("skipped")
                              : (info.success ? QStringLiteral("success") : QStringLiteral("failed"));
    result.insert(QStringLiteral("success"), run.ok);
    result.insert(QStringLiteral("elapsedMs"), run.elapsedMs);
    result.insert(QStringLiteral("outputs"), payload.outputs);
    result.insert(QStringLiteral("lastStatus"), status);
    if (!run.ok) {
        result.insert(QStringLiteral("code"), QStringLiteral("run_failed"));
        result.insert(QStringLiteral("failedModule"), run.failedModule);
        result.insert(QStringLiteral("errorMessage"), run.error);
        result.insert(QStringLiteral("error"), run.error);
        // 设计取舍：参数保留（不静默回滚），一次撤销可回滚整组；用显式字段告知模型
        result.insert(QStringLiteral("parametersRolledBack"), false);
        result.insert(QStringLiteral("note"),
                      QStringLiteral("Parameters were applied and kept; undo one step to roll back the whole patch."));
    }
    return result;
}

QJsonObject AgentActor::captureViewportImage(const QJsonObject& params) {
    Q_UNUSED(params)
    QImage image;
    QString source;
    QString providerError;
    if (m_viewportImageProvider) {
        image = m_viewportImageProvider(&providerError);
        if (!image.isNull())
            source = QStringLiteral("viewport");
    }
    if (image.isNull()) {
        const ImageData last = RunEngine::instance().lastOutput();
        if (last.isValid()) {
            image = last.toQImage();
            source = QStringLiteral("last_output");
        }
    }
    if (image.isNull()) {
        return errorObject(providerError.isEmpty() ? QStringLiteral("No image available in the viewport")
                                                   : providerError,
                           QStringLiteral("no_image"));
    }
    const QString path = AgentToolImage::writeImage(image, QStringLiteral("capture_viewport_image"));
    if (path.isEmpty())
        return errorObject(QStringLiteral("Failed to write image: cannot save PNG"),
                           QStringLiteral("image_write_failed"));
    return QJsonObject{{QStringLiteral("imagePath"), path},
                       {QStringLiteral("width"), image.width()},
                       {QStringLiteral("height"), image.height()},
                       {QStringLiteral("source"), source},
                       {QStringLiteral("bytes"), static_cast<double>(QFileInfo(path).size())}};
}

QJsonObject AgentActor::exportNodeOutput(const QJsonObject& params) {
    Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return errorObject(QStringLiteral("No project opened"), QStringLiteral("no_project"));
    RunEngine& engine = RunEngine::instance();
    if (engine.isBusy()) {
        return errorObject(QStringLiteral("Flow is running; stop it before exporting node output"),
                           QStringLiteral("engine_busy"));
    }
    QString loadError;
    if (!ensureEngineLoaded(&loadError))
        return errorObject(loadError, QStringLiteral("engine_load_failed"));

    const QString nodeName = params.value("nodeName").toString();
    NodeRef ref;
    if (!resolveNodeRef(project, nodeName, &ref))
        return errorObject(QString("Module not found: %1").arg(nodeName), QStringLiteral("node_not_found"));

    const QString format = params.value("format").toString(QStringLiteral("png")).toLower();
    QImage image;
    QString source = QStringLiteral("module_output");
    const PortValueMap ports = engine.nodePortOutputs(ref.runtimeName);
    const QVariant imageValue = ports.value(QStringLiteral("image"));
    if (imageValue.userType() == qMetaTypeId<ImageData>()) {
        const ImageData data = imageValue.value<ImageData>();
        if (data.isValid())
            image = data.toQImage();
    }
    if (image.isNull()) {
        const ImageData moduleImage = engine.moduleOutput(ref.runtimeName);
        if (moduleImage.isValid())
            image = moduleImage.toQImage();
    }
    // R3（设计 §4.6）：本工具只接受节点运行输出，禁止回退到视口/上一帧——
    // 回退会把"节点无图像"变成假成功，且与 capture_viewport_image 的职责混淆。
    if (image.isNull())
        return errorObject(QString("Node '%1' produced no image output").arg(ref.runtimeName),
                           QStringLiteral("no_image_output"));

    const QString path = AgentToolImage::writeImage(image, QStringLiteral("export_node_output"), format);
    if (path.isEmpty())
        return errorObject(QStringLiteral("Failed to write image"), QStringLiteral("image_write_failed"));
    return QJsonObject{{QStringLiteral("imagePath"), path},
                       {QStringLiteral("width"), image.width()},
                       {QStringLiteral("height"), image.height()},
                       {QStringLiteral("source"), source},
                       {QStringLiteral("format"), format},
                       {QStringLiteral("bytes"), static_cast<double>(QFileInfo(path).size())}};
}

} // namespace DeepLux
