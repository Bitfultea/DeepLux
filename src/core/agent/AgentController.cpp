#include "AgentController.h"

#include "AgentActor.h"
#include "AgentAuditLog.h"
#include "AgentBridge.h"
#include "AgentObserver.h"
#include "AgentToolImage.h"
#include "AnnotationSessionExport.h"
#include "AnnotationSessionTransform.h"
#include "GuiEvent.h"
#include "ILLMClient.h"
#include "ToolSchema.h"
#include "common/Logger.h"
#include "device/CameraManager.h"
#include "engine/RunEngine.h"
#include "interface/IModule.h"
#include "manager/ConfigManager.h"
#include "manager/GlobalVarManager.h"
#include "manager/PluginManager.h"
#include "manager/ProjectManager.h"
#include "model/ImageData.h"
#include "model/Project.h"
#include "platform/PathUtils.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPixmap>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#include <unistd.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#endif

namespace DeepLux {

namespace {
constexpr int kMaxAgentToolTurns = 20;
constexpr int kMaxToolCallsPerTurn = 10;
constexpr int kMaxParameterCorrectionRetries = 3;
constexpr int kMaxImageAttachments = 4;
constexpr qint64 kMaxImageAttachmentBytes = 8LL * 1024 * 1024;
constexpr int kMaxToolImagesPerTurn = 2;
constexpr int kMaxToolImageLongEdge = 1280;
constexpr qint64 kMaxToolImageBytes = 4LL * 1024 * 1024;

int takeElapsedMs(QJsonObject& result) {
    const QJsonValue value = result.value(QStringLiteral("_elapsedMs"));
    if (!value.isDouble())
        return -1;
    result.remove(QStringLiteral("_elapsedMs"));
    return value.toInt(-1);
}

bool isImageProducingTool(const QString& toolName) {
    static const QSet<QString> tools{QStringLiteral("capture_viewport_image"), QStringLiteral("export_node_output")};
    return tools.contains(toolName);
}

struct ToolOutcome {
    QString callId;
    QString toolName;
    QJsonObject result;
    bool executed = false;
    bool denied = false;
};

QString safeToolErrorText(const QString& errorCode) {
    static const QMap<QString, QString> descriptions = {
        {QStringLiteral("INVALID_PARAM"), QStringLiteral("参数无效")},
        {QStringLiteral("MISSING_PARAM"), QStringLiteral("缺少必需参数")},
        {QStringLiteral("INSTANCE_NOT_FOUND"), QStringLiteral("模块实例不存在")},
        {QStringLiteral("PLUGIN_NOT_FOUND"), QStringLiteral("插件未找到")},
        {QStringLiteral("PLUGIN_NOT_LOADED"), QStringLiteral("插件未加载")},
        {QStringLiteral("PERMISSION_DENIED"), QStringLiteral("当前权限不允许此操作")},
        {QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR"), QStringLiteral("因前序调用失败而未执行")},
        {QStringLiteral("UNKNOWN_TOOL"), QStringLiteral("工具不可用")},
        {QStringLiteral("RUN_NOT_FOUND"), QStringLiteral("运行记录不存在")},
        {QStringLiteral("SAVE_FAILED"), QStringLiteral("保存失败")},
        {QStringLiteral("OPEN_FAILED"), QStringLiteral("打开工程失败")},
        {QStringLiteral("NO_PROJECT"), QStringLiteral("当前没有打开工程")},
        {QStringLiteral("FLOW_UNAVAILABLE"), QStringLiteral("当前不支持按 flowId 选择流程")},
    };
    return descriptions.value(errorCode, QStringLiteral("工具执行失败"));
}

QString toolOutcomeLabel(const ToolOutcome& outcome) {
    const QString errorCode = outcome.result.value(QStringLiteral("errorCode")).toString();
    if (outcome.denied || errorCode == QLatin1String("PERMISSION_DENIED") || errorCode == QLatin1String("UNKNOWN_TOOL"))
        return QStringLiteral("未执行（权限拒绝或工具不可用，%1）")
            .arg(errorCode.isEmpty() ? QStringLiteral("UNKNOWN_TOOL") : errorCode);
    if (!outcome.executed)
        return QStringLiteral("未执行（%1，%2）")
            .arg(safeToolErrorText(errorCode), errorCode.isEmpty() ? QStringLiteral("UNKNOWN_ERROR") : errorCode);
    if (outcome.result.value(QStringLiteral("status")) == QLatin1String("uncertain"))
        return QStringLiteral("结果不确定（禁止自动重试）");
    if (outcome.result.contains(QStringLiteral("error")))
        return QStringLiteral("执行失败（%1：%2）")
            .arg(errorCode.isEmpty() ? QStringLiteral("UNKNOWN_ERROR") : errorCode, safeToolErrorText(errorCode));

    const QString status = outcome.result.value(QStringLiteral("status")).toString();
    if (status == QLatin1String("no_op"))
        return QStringLiteral("无操作");
    if (status == QLatin1String("started") || status == QLatin1String("pending") ||
        status == QLatin1String("stopping") || status == QLatin1String("resumed"))
        return QStringLiteral("已受理/进行中（%1）").arg(status);

    static const QStringList synchronousStatuses = {
        QStringLiteral("added"),     QStringLiteral("created"),      QStringLiteral("updated"),
        QStringLiteral("renamed"),   QStringLiteral("removed"),      QStringLiteral("moved"),
        QStringLiteral("connected"), QStringLiteral("disconnected"), QStringLiteral("saved"),
        QStringLiteral("deleted"),   QStringLiteral("reset"),        QStringLiteral("completed"),
        QStringLiteral("success"),   QStringLiteral("valid"),        QStringLiteral("valid_with_warnings"),
        QStringLiteral("paused"),    QStringLiteral("cancelled"),    QStringLiteral("rejected"),
    };
    if (status.isEmpty())
        return QStringLiteral("调用已返回");
    if (synchronousStatuses.contains(status))
        return QStringLiteral("已完成");
    return QStringLiteral("调用已返回（状态未确认：%1）").arg(status);
}

QString batchFailureSummary(const QString& heading, const QList<ToolOutcome>& outcomes, bool sideEffectsMayRemain) {
    QStringList lines;
    lines.reserve(outcomes.size() + 1);
    lines.append(heading);
    if (sideEffectsMayRemain)
        lines.append(QStringLiteral("已发生的副作用未自动回滚。"));
    for (int i = 0; i < outcomes.size(); ++i) {
        const ToolOutcome& outcome = outcomes.at(i);
        const QString callId = outcome.callId.isEmpty() ? QStringLiteral("(无 callId)") : outcome.callId;
        const QString tool = outcome.toolName.isEmpty() ? QStringLiteral("(未知工具)") : outcome.toolName;
        lines.append(QStringLiteral("%1. %2 · %3 · %4").arg(i + 1).arg(callId, tool, toolOutcomeLabel(outcome)));
    }
    return lines.join(QLatin1Char('\n'));
}

bool isReadOnlyToolName(const QString& toolName) {
    static const QStringList readOnlyTools = {
        "get_flow_state",
        "get_available_plugins",
        "get_module_params_schema",
        "get_run_results",
        "get_module_output",
        "wait_for_run",
        "read_documentation",
        "list_knowledge_documents",
        "preview_knowledge_document",
        "open_system_settings",
        "open_global_variables_dialog",
        "open_sam_environment_setup",
        "open_sam_annotator_dialog",
        "open_sam_image_picker_for_user",
        "open_user_login_dialog",
        "open_agent_settings_dialog",
        "open_about_dialog",
        "open_knowledge_base_dialog",
        "get_annotation_session",
        "list_global_variables",
        "get_global_variable",
        "list_data_sources",
        "list_recent_projects",
        "list_active_annotation_objects",
        "list_active_annotation_categories",
        "get_active_annotation_prompt_status",
        "get_data_source_view_state",
        "set_log_level_filter",
        "select_log_panel_tab",
        "scroll_agent_chat_to_latest",
        "set_workspace_panel_visibility",
        "set_inspector_pinned",
        "set_inspector_collapsed",
        "set_inspector_visibility",
        "set_primary_viewport_zoom",
        "set_primary_viewport_point_size",
        "set_primary_viewport_display_mode",
        "set_point_cloud_render_mode",
        "close_viewport",
        "list_viewports",
        "get_quick_measurement_status",
        "select_flow_module",
        "validate_flow",
        "validate_workflow",
        "get_node_runtime_outputs",
        "get_execution_logs",
        "capture_viewport_image",
        "export_node_output",
        "list_cameras",
        "get_camera_discovery_targets",
        "get_camera_discovery_operation",
        "get_camera_details",
        "get_camera_operation",
        "list_communications",
    };
    return readOnlyTools.contains(toolName);
}

QString toolCallName(const QJsonObject& tc) {
    QString name = tc["name"].toString();
    if (name.isEmpty())
        name = tc["function"].toObject()["name"].toString();
    return name;
}

QString permissionLevelName(AgentController::PermissionLevel level) {
    switch (level) {
    case AgentController::PermissionLevel::Observer:
        return QStringLiteral("Observer");
    case AgentController::PermissionLevel::Advisor:
        return QStringLiteral("Advisor");
    case AgentController::PermissionLevel::Autopilot:
        return QStringLiteral("Autopilot");
    }
    return QStringLiteral("Unknown");
}

QJsonObject toolCallParams(const QJsonObject& tc, bool* valid = nullptr) {
    if (valid)
        *valid = true;
    QJsonValue arguments = tc.value(QStringLiteral("arguments"));
    if (arguments.isUndefined())
        arguments = tc.value(QStringLiteral("function")).toObject().value(QStringLiteral("arguments"));
    if (arguments.isUndefined())
        return {};
    if (arguments.isObject())
        return arguments.toObject();
    if (arguments.isString()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(arguments.toString().toUtf8(), &error);
        if (error.error == QJsonParseError::NoError && document.isObject())
            return document.object();
    }
    if (valid)
        *valid = false;
    return {};
}

QJsonArray pendingToolCallsForUiAndBridge(const QJsonArray& calls) {
    QJsonArray safeCalls;
    for (const QJsonValue& value : calls) {
        QJsonObject call = value.toObject();
        if (toolCallName(call) == QLatin1String("edit_active_annotation_masks")) {
            const QJsonObject params = toolCallParams(call);
            QJsonObject safeParams{
                {QStringLiteral("operation"), params.value(QStringLiteral("operation"))},
                {QStringLiteral("objectIds"), params.value(QStringLiteral("objectIds"))},
                {QStringLiteral("strokePointCount"), params.value(QStringLiteral("strokePoints")).toArray().size()}};
            if (params.contains(QStringLiteral("radiusPx")))
                safeParams.insert(QStringLiteral("radiusPx"), params.value(QStringLiteral("radiusPx")));
            if (call.contains(QStringLiteral("name"))) {
                call.insert(QStringLiteral("arguments"), safeParams);
            } else {
                QJsonObject function = call.value(QStringLiteral("function")).toObject();
                function.insert(QStringLiteral("arguments"), safeParams);
                call.insert(QStringLiteral("function"), function);
            }
        } else if (toolCallName(call) == QLatin1String("edit_active_annotation_categories")) {
            const QJsonObject params = toolCallParams(call);
            QJsonObject safeParams{{QStringLiteral("operation"), params.value(QStringLiteral("operation"))}};
            for (const QString& key :
                 {QStringLiteral("category"), QStringLiteral("objectIds"), QStringLiteral("label")}) {
                if (params.contains(key))
                    safeParams.insert(key, params.value(key));
            }
            if (call.contains(QStringLiteral("name"))) {
                call.insert(QStringLiteral("arguments"), safeParams);
            } else {
                QJsonObject function = call.value(QStringLiteral("function")).toObject();
                function.insert(QStringLiteral("arguments"), safeParams);
                call.insert(QStringLiteral("function"), function);
            }
        }
        safeCalls.append(call);
    }
    return safeCalls;
}

QJsonObject auditParamsForTool(const QString& toolName, const QJsonObject& params) {
    if (toolName == QLatin1String("save_viewport_snapshot_for_user"))
        return {{QStringLiteral("viewportId"), params.value(QStringLiteral("viewportId"))}};
    if (toolName == QLatin1String("open_data_source_folder_for_user"))
        return {{QStringLiteral("dataSourceId"), params.value(QStringLiteral("dataSourceId"))}};
    if (toolName == QLatin1String("open_user_knowledge_folder_for_user"))
        return {};
    if (toolName == QLatin1String("open_global_variables_dialog"))
        return {};
    if (toolName == QLatin1String("list_active_annotation_categories")) {
        QJsonObject safe;
        if (params.contains(QStringLiteral("offset")))
            safe.insert(QStringLiteral("offset"), params.value(QStringLiteral("offset")));
        if (params.contains(QStringLiteral("limit")))
            safe.insert(QStringLiteral("limit"), params.value(QStringLiteral("limit")));
        return safe;
    }
    if (toolName == QLatin1String("edit_active_annotation_categories")) {
        QJsonObject safe{{QStringLiteral("operation"), params.value(QStringLiteral("operation"))}};
        for (const QString& key : {QStringLiteral("category"), QStringLiteral("objectIds"), QStringLiteral("label")}) {
            if (params.contains(key))
                safe.insert(key, params.value(key));
        }
        return safe;
    }
    if (toolName == QLatin1String("get_active_annotation_prompt_status") ||
        toolName == QLatin1String("cancel_active_annotation_prompt"))
        return {};
    if (toolName == QLatin1String("save_active_annotation_session"))
        return {{QStringLiteral("outputPath"), params.value(QStringLiteral("outputPath"))}};
    if (toolName == QLatin1String("open_active_annotation_session"))
        return {{QStringLiteral("sessionPath"), params.value(QStringLiteral("sessionPath"))}};
    if (toolName == QLatin1String("list_active_annotation_objects")) {
        QJsonObject safe;
        if (params.contains(QStringLiteral("offset")))
            safe.insert(QStringLiteral("offset"), params.value(QStringLiteral("offset")));
        if (params.contains(QStringLiteral("limit")))
            safe.insert(QStringLiteral("limit"), params.value(QStringLiteral("limit")));
        return safe;
    }
    if (toolName == QLatin1String("edit_active_annotation_masks")) {
        QJsonObject safe{
            {QStringLiteral("operation"), params.value(QStringLiteral("operation"))},
            {QStringLiteral("objectIds"), params.value(QStringLiteral("objectIds"))},
            {QStringLiteral("strokePointCount"), params.value(QStringLiteral("strokePoints")).toArray().size()}};
        if (params.contains(QStringLiteral("radiusPx")))
            safe.insert(QStringLiteral("radiusPx"), params.value(QStringLiteral("radiusPx")));
        return safe;
    }
    if (toolName == QLatin1String("select_flow_module")) {
        return QJsonObject{{QStringLiteral("instanceId"), params.value(QStringLiteral("instanceId"))}};
    }
    if (toolName == QLatin1String("display_data_source_in_dedicated_viewport"))
        return QJsonObject{{QStringLiteral("dataSourceId"), params.value(QStringLiteral("dataSourceId"))}};
    if (toolName == QLatin1String("set_workspace_panel_visibility")) {
        return QJsonObject{{QStringLiteral("panel"), params.value(QStringLiteral("panel"))},
                           {QStringLiteral("visible"), params.value(QStringLiteral("visible"))}};
    }
    if (toolName == QLatin1String("set_log_level_filter")) {
        return QJsonObject{{QStringLiteral("minimumLevel"), params.value(QStringLiteral("minimumLevel"))}};
    }
    if (toolName == QLatin1String("set_theme"))
        return QJsonObject{{QStringLiteral("theme"), params.value(QStringLiteral("theme"))}};
    if (toolName == QLatin1String("open_system_settings"))
        return {};
    if (toolName == QLatin1String("set_inspector_pinned")) {
        const QJsonValue pinned = params.value(QStringLiteral("pinned"));
        return pinned.isBool() ? QJsonObject{{QStringLiteral("pinned"), pinned}} : QJsonObject();
    }
    if (toolName == QLatin1String("set_inspector_collapsed")) {
        const QJsonValue collapsed = params.value(QStringLiteral("collapsed"));
        return collapsed.isBool() ? QJsonObject{{QStringLiteral("collapsed"), collapsed}} : QJsonObject();
    }
    if (toolName == QLatin1String("set_inspector_visibility")) {
        const QJsonValue visible = params.value(QStringLiteral("visible"));
        return visible.isBool() ? QJsonObject{{QStringLiteral("visible"), visible}} : QJsonObject();
    }
    if (toolName == QLatin1String("close_viewport")) {
        const QJsonValue viewportId = params.value(QStringLiteral("viewportId"));
        return viewportId.isString() && !viewportId.toString().trimmed().isEmpty()
                   ? QJsonObject{{QStringLiteral("viewportId"), viewportId}}
                   : QJsonObject();
    }
    if (toolName == QLatin1String("copy_data_source_path_to_clipboard")) {
        const QJsonValue dataSourceId = params.value(QStringLiteral("dataSourceId"));
        return dataSourceId.isString() && !dataSourceId.toString().trimmed().isEmpty()
                   ? QJsonObject{{QStringLiteral("dataSourceId"), dataSourceId}}
                   : QJsonObject();
    }
    if (toolName == QLatin1String("set_primary_viewport_zoom"))
        return QJsonObject{{QStringLiteral("action"), params.value(QStringLiteral("action"))}};
    if (toolName == QLatin1String("set_primary_viewport_point_size")) {
        const QJsonValue sizeValue = params.value(QStringLiteral("size"));
        if (!sizeValue.isDouble())
            return {};
        const double size = sizeValue.toDouble();
        if (!std::isfinite(size) || size < 1.0 || size > 8.0 || std::abs(size * 10.0 - std::round(size * 10.0)) > 1e-8)
            return {};
        return QJsonObject{{QStringLiteral("size"), size}};
    }
    if (toolName == QLatin1String("set_primary_viewport_display_mode")) {
        const QString mode = params.value(QStringLiteral("mode")).toString();
        return mode == QLatin1String("2d") || mode == QLatin1String("3d") ? QJsonObject{{QStringLiteral("mode"), mode}}
                                                                          : QJsonObject();
    }
    if (toolName == QLatin1String("set_point_cloud_render_mode")) {
        const QString mode = params.value(QStringLiteral("mode")).toString();
        static const QStringList modes{QStringLiteral("uniform"), QStringLiteral("rgb"),
                                       QStringLiteral("height"),  QStringLiteral("intensity"),
                                       QStringLiteral("normal"),  QStringLiteral("blinn_phong")};
        return modes.contains(mode) ? QJsonObject{{QStringLiteral("mode"), mode}} : QJsonObject();
    }
    if (toolName == QLatin1String("select_log_panel_tab")) {
        return QJsonObject{{QStringLiteral("tab"), params.value(QStringLiteral("tab"))}};
    }
    if (toolName == QLatin1String("scroll_agent_chat_to_latest"))
        return {};
    if (toolName == QLatin1String("list_recent_projects"))
        return {};
    if (toolName == QLatin1String("clear_runtime_logs"))
        return {};
    if (toolName == QLatin1String("export_annotation_session")) {
        return QJsonObject{
            {QStringLiteral("sessionPath"), params.value(QStringLiteral("sessionPath"))},
            {QStringLiteral("format"), params.value(QStringLiteral("format"))},
            {QStringLiteral("outputPath"), params.value(QStringLiteral("outputPath"))},
            {QStringLiteral("categoryCount"), params.value(QStringLiteral("categories")).toArray().size()}};
    }
    if (toolName == QLatin1String("transform_annotation_session")) {
        return QJsonObject{{QStringLiteral("sessionPath"), params.value(QStringLiteral("sessionPath"))},
                           {QStringLiteral("outputPath"), params.value(QStringLiteral("outputPath"))},
                           {QStringLiteral("operation"), params.value(QStringLiteral("operation"))},
                           {QStringLiteral("objectCount"), params.value(QStringLiteral("objectIds")).toArray().size()}};
    }
    if (toolName == QLatin1String("list_global_variables")) {
        QJsonObject safe;
        if (params.contains(QStringLiteral("type")))
            safe.insert(QStringLiteral("type"), params.value(QStringLiteral("type")));
        return safe;
    }
    if (toolName == QLatin1String("get_global_variable") || toolName == QLatin1String("create_global_variable") ||
        toolName == QLatin1String("set_global_variable") || toolName == QLatin1String("remove_global_variable")) {
        QJsonObject safe;
        if (params.contains(QStringLiteral("name")))
            safe.insert(QStringLiteral("name"), params.value(QStringLiteral("name")));
        if (params.contains(QStringLiteral("type")))
            safe.insert(QStringLiteral("type"), params.value(QStringLiteral("type")));
        return safe;
    }
    return params;
}

QJsonObject preConfirmationParameterError(const QString& toolName, const QJsonObject& params) {
    if (toolName == QLatin1String("save_viewport_snapshot_for_user")) {
        const QJsonValue viewportId = params.value(QStringLiteral("viewportId"));
        if (params.size() != 1 || !viewportId.isString() || viewportId.toString().trimmed().isEmpty())
            return {{QStringLiteral("error"), QStringLiteral("Only a non-empty viewportId parameter is accepted")},
                    {QStringLiteral("errorCode"), QStringLiteral("INVALID_PARAM")},
                    {QStringLiteral("retryable"), false}};
    }
    if (toolName == QLatin1String("open_data_source_folder_for_user")) {
        const QJsonValue dataSourceId = params.value(QStringLiteral("dataSourceId"));
        if (params.size() != 1 || !dataSourceId.isString() || dataSourceId.toString().trimmed().isEmpty())
            return {{QStringLiteral("error"), QStringLiteral("Only a non-empty dataSourceId parameter is accepted")},
                    {QStringLiteral("errorCode"), QStringLiteral("INVALID_PARAM")},
                    {QStringLiteral("retryable"), false}};
    }
    if (toolName == QLatin1String("open_user_knowledge_folder_for_user") && !params.isEmpty())
        return {{QStringLiteral("error"), QStringLiteral("This knowledge folder action does not accept parameters")},
                {QStringLiteral("errorCode"), QStringLiteral("INVALID_PARAM")},
                {QStringLiteral("retryable"), false}};
    if (toolName == QLatin1String("open_global_variables_dialog") && !params.isEmpty())
        return {{QStringLiteral("error"), QStringLiteral("This global variables action does not accept parameters")},
                {QStringLiteral("errorCode"), QStringLiteral("INVALID_PARAM")},
                {QStringLiteral("retryable"), false}};
    return {};
}

QString compactAuditParams(const QString& toolName, const QJsonObject& params) {
    return QString(QJsonDocument(auditParamsForTool(toolName, params)).toJson(QJsonDocument::Compact));
}

QString compactAuditResultParams(const QString& toolName, const QJsonObject& params, const QJsonObject& result) {
    QJsonObject auditParams = auditParamsForTool(toolName, params);
    if (toolName == QLatin1String("save_active_annotation_session") &&
        result.value(QStringLiteral("status")) == QLatin1String("saved")) {
        for (const QString& key : {QStringLiteral("objectCount"), QStringLiteral("bytes"), QStringLiteral("sha256")})
            auditParams.insert(key, result.value(key));
    }
    if (toolName == QLatin1String("open_active_annotation_session") &&
        result.value(QStringLiteral("status")) == QLatin1String("opened")) {
        for (const QString& key :
             {QStringLiteral("objectCount"), QStringLiteral("imageWidth"), QStringLiteral("imageHeight"),
              QStringLiteral("imageBound"), QStringLiteral("legacyImageBindingUnverified")})
            auditParams.insert(key, result.value(key));
    }
    return QString(QJsonDocument(auditParams).toJson(QJsonDocument::Compact));
}

QString variableSnapshotKey(int callIndex) {
    return QStringLiteral("variable-call-index:") + QString::number(callIndex);
}

QJsonArray uniqueToolCallIds(const QJsonArray& calls) {
    QSet<QString> seen;
    QJsonArray uniqueCalls;
    for (const QJsonValue& value : calls) {
        QJsonObject call = value.toObject();
        QString id = call.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty() || seen.contains(id)) {
            do {
                id = QStringLiteral("deeplux-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            } while (seen.contains(id));
            call.insert(QStringLiteral("id"), id);
        }
        seen.insert(id);
        uniqueCalls.append(call);
    }
    return uniqueCalls;
}

bool isDangerousToolName(const QString& toolName) {
    return ToolSchema::instance().findTool(toolName).dangerous;
}

// 阶段1 §5（F13）保守口径：流程执行入口可间接触发文件删除/写入、PLC/网络写，
// Autopilot 也必须确认；阶段 5 再按流程实际副作用精细判定
bool isExecutionEntryPoint(const QString& toolName, const QJsonObject& params = {}) {
    return toolName == QLatin1String("run_flow") || toolName == QLatin1String("step_once") ||
           toolName == QLatin1String("resume_flow") ||
           (toolName == QLatin1String("tune_node_and_verify") &&
            params.value(QStringLiteral("runMode")).toString(QStringLiteral("flow")) == QLatin1String("flow"));
}

// 阶段1 §6：undoable 由实际命令能力决定——仅压入 QUndoStack 的工程编辑命令可撤销；
// 保存/网络/推理/设备/执行/工程切换类动作一律不可撤销，不因"非只读"就显示可撤销
bool isUndoCapableToolName(const QString& toolName) {
    static const QStringList undoCapable = {
        "add_module",          "remove_module",   "move_module",        "set_module_position", "rename_module",
        "set_module_enabled",  "set_breakpoint",  "set_module_note",    "set_param",           "set_module_params",
        "reset_module_params", "connect_modules", "disconnect_modules",
    };
    return undoCapable.contains(toolName);
}

bool isUndoableChange(const QString& toolName, const QJsonObject& result) {
    const QString status = result.value(QStringLiteral("status")).toString();
    return !result.contains(QStringLiteral("error")) && status != QStringLiteral("no_op") &&
           status != QStringLiteral("already_connected") && isUndoCapableToolName(toolName);
}

QString confirmedToolAuditOutcome(const QJsonObject& result) {
    if (result.value(QStringLiteral("errorCode")) == QLatin1String("CONFIRMATION_EXPIRED"))
        return QStringLiteral("expired");
    if (result.value(QStringLiteral("status")) == QLatin1String("uncertain"))
        return QStringLiteral("uncertain");
    if (result.value(QStringLiteral("status")) == QLatin1String("cancelled"))
        return QStringLiteral("cancelled");
    if (result.value(QStringLiteral("status")) == QLatin1String("open_requested"))
        return QStringLiteral("open_requested");
    if (result.contains(QStringLiteral("error")))
        return QStringLiteral("error");
    if (result.value(QStringLiteral("status")) == QLatin1String("pending"))
        return QStringLiteral("pending");
    return QStringLiteral("success");
}

// 阶段1 §4：按工具与参数声明副作用（确认内容具体化；设备目标细化属阶段 5）
QString safeSamServerOrigin(const QString& serverUrl) {
    const QUrl url(serverUrl);
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
        return QStringLiteral("<invalid>");

    const QString scheme = url.scheme().toLower();
    int port = url.port();
    if (port < 0) {
        if (scheme == QLatin1String("http"))
            port = 80;
        else if (scheme == QLatin1String("https"))
            port = 443;
    }
    QString host = url.host();
    if (host.contains(QLatin1Char(':')) && !host.startsWith(QLatin1Char('[')))
        host = QLatin1Char('[') + host + QLatin1Char(']');
    const QString effectivePort = port >= 0 ? QString::number(port) : QStringLiteral("<unknown>");
    return QStringLiteral("%1://%2:%3").arg(scheme, host, effectivePort);
}

QString resolvedFileTargetPath(const QString& configuredPath) {
    return PathUtils::resolveFileTargetPath(configuredPath);
}

QJsonObject currentProjectSaveTargetSnapshot(const Project* project) {
    if (!project)
        return {};
    const QString configuredPath = project->filePath();
    const QString resolvedTarget =
        configuredPath.trimmed().isEmpty() ? QString() : resolvedFileTargetPath(configuredPath);
    return QJsonObject{{QStringLiteral("configuredPath"), configuredPath},
                       {QStringLiteral("resolvedTarget"), resolvedTarget}};
}

bool hasImplicitProjectSaveTarget(const QString& toolName, const QJsonObject& params, const Project* project) {
    if ((toolName != QLatin1String("create_project") && toolName != QLatin1String("open_project")) ||
        params.value(QStringLiteral("onUnsaved")).toString(QStringLiteral("cancel")) != QLatin1String("save") ||
        !project || !project->isModified())
        return false;
    const QJsonObject target = currentProjectSaveTargetSnapshot(project);
    return !target.value(QStringLiteral("configuredPath")).toString().trimmed().isEmpty() &&
           !target.value(QStringLiteral("resolvedTarget")).toString().isEmpty();
}

QString directFileTargetPath(const QString& toolName, const QJsonObject& params, const Project* project) {
    QString configuredPath;
    if (toolName == QLatin1String("save_project")) {
        configuredPath = params.value(QStringLiteral("path")).toString();
        if (configuredPath.isEmpty() && project)
            configuredPath = project->filePath();
    } else if (toolName == QLatin1String("sam_annotate")) {
        configuredPath = params.value(QStringLiteral("sessionPath")).toString();
    }
    if (configuredPath.isEmpty())
        return {};
    return resolvedFileTargetPath(configuredPath);
}

QJsonObject fileHashSnapshot(const QString& path) {
    const QFileInfo info(path);
    QJsonObject snapshot{{QStringLiteral("path"), info.absoluteFilePath()},
                         {QStringLiteral("resolvedPath"), info.canonicalFilePath()},
                         {QStringLiteral("readable"), false}};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return snapshot;

    bool fileIdentityAvailable = false;
    QString fileIdentity;
#ifdef Q_OS_UNIX
    struct stat fileStatus {};
    if (fstat(file.handle(), &fileStatus) == 0) {
        fileIdentity = QStringLiteral("%1:%2").arg(QString::number(static_cast<qulonglong>(fileStatus.st_dev)),
                                                   QString::number(static_cast<qulonglong>(fileStatus.st_ino)));
        fileIdentityAvailable = true;
    }
#elif defined(Q_OS_WIN)
    const intptr_t nativeHandle = _get_osfhandle(file.handle());
    BY_HANDLE_FILE_INFORMATION fileInfo{};
    if (nativeHandle != -1 && GetFileInformationByHandle(reinterpret_cast<HANDLE>(nativeHandle), &fileInfo)) {
        fileIdentity = QStringLiteral("%1:%2:%3")
                           .arg(fileInfo.dwVolumeSerialNumber)
                           .arg(fileInfo.nFileIndexHigh, 8, 16, QLatin1Char('0'))
                           .arg(fileInfo.nFileIndexLow, 8, 16, QLatin1Char('0'));
        fileIdentityAvailable = true;
    }
#endif

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 byteCount = 0;
    constexpr qint64 kChunkBytes = 1024 * 1024;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return snapshot;
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
        byteCount += chunk.size();
    }
    if (file.error() != QFileDevice::NoError)
        return snapshot;

    snapshot.insert(QStringLiteral("readable"), true);
    snapshot.insert(QStringLiteral("byteCount"), byteCount);
    snapshot.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
    snapshot.insert(QStringLiteral("fileIdentityAvailable"), fileIdentityAvailable);
    if (fileIdentityAvailable)
        snapshot.insert(QStringLiteral("fileIdentity"), fileIdentity);
    return snapshot;
}

QString projectInputSideEffect(const QString& configuredPath, const QJsonObject& snapshot) {
    QString effect = QStringLiteral("project_input:%1").arg(configuredPath);
    const QString resolvedPath = snapshot.value(QStringLiteral("resolvedPath")).toString();
    if (!resolvedPath.isEmpty() && QDir::cleanPath(QFileInfo(configuredPath).absoluteFilePath()) != resolvedPath)
        effect += QStringLiteral(" resolved_target=%1").arg(resolvedPath);
    if (snapshot.value(QStringLiteral("readable")).toBool()) {
        effect += QStringLiteral(" bytes=%1 sha256=%2")
                      .arg(snapshot.value(QStringLiteral("byteCount")).toVariant().toLongLong())
                      .arg(snapshot.value(QStringLiteral("sha256")).toString().left(12));
    } else {
        effect += QStringLiteral(" snapshot=unavailable");
    }
    return effect;
}

QJsonObject pluginLifecycleSnapshot(const PluginManager& manager, const QString& name) {
    const PluginInfo info = manager.pluginInfo(name);
    const bool found = !info.name.isEmpty();
    QJsonObject snapshot{{QStringLiteral("found"), found},
                         {QStringLiteral("name"), info.name},
                         {QStringLiteral("id"), info.id},
                         {QStringLiteral("category"), info.category},
                         {QStringLiteral("loaded"), manager.isPluginLoaded(name)}};
    if (!found)
        return snapshot;

    const QFileInfo metadataInfo(info.path);
    const QString metadataPath = metadataInfo.absoluteFilePath();
    snapshot.insert(QStringLiteral("metadataPath"), metadataPath);
    snapshot.insert(QStringLiteral("metadata"), fileHashSnapshot(metadataPath));

    const QDir pluginDir(metadataInfo.absolutePath());
    QStringList filters;
#ifdef Q_OS_WIN
    filters << QStringLiteral("*.dll");
#else
    filters << QStringLiteral("*.so");
#endif
    QJsonArray binaries;
    const QStringList libraryNames = pluginDir.entryList(filters, QDir::Files, QDir::Name);
    for (const QString& libraryName : libraryNames) {
        const QString libraryPath = pluginDir.filePath(libraryName);
        QJsonObject binary = fileHashSnapshot(libraryPath);
        binary.insert(QStringLiteral("name"), libraryName);
        binaries.append(binary);
    }
    snapshot.insert(QStringLiteral("binaries"), binaries);
    return snapshot;
}

bool pluginLifecycleSnapshotIsHashable(const QJsonObject& snapshot) {
    if (!snapshot.value(QStringLiteral("found")).toBool())
        return true;
    const auto hasSha256 = [](const QJsonObject& file) {
        return file.value(QStringLiteral("readable")).toBool() &&
               file.value(QStringLiteral("sha256")).toString().size() == 64;
    };
    if (!hasSha256(snapshot.value(QStringLiteral("metadata")).toObject()))
        return false;
    for (const QJsonValue& value : snapshot.value(QStringLiteral("binaries")).toArray()) {
        if (!hasSha256(value.toObject()))
            return false;
    }
    return true;
}

bool pluginLifecycleTargetsAreHashable(const QJsonObject& targets) {
    for (auto it = targets.constBegin(); it != targets.constEnd(); ++it) {
        if (it.key().startsWith(QLatin1String("plugin:")) && !pluginLifecycleSnapshotIsHashable(it.value().toObject()))
            return false;
    }
    return true;
}

bool containsInvalidatedExecutionImageSnapshot(const QJsonObject& targets) {
    for (auto it = targets.constBegin(); it != targets.constEnd(); ++it) {
        if (it.key().startsWith(QLatin1String("module-input-image:")) &&
            it.value().toObject().value(QStringLiteral("source")) == QLatin1String("execution_context_invalidated"))
            return true;
    }
    return false;
}

QString pluginLifecycleSideEffect(const QString& toolName, const QString& name, const QJsonObject& snapshot) {
    QString effect =
        QStringLiteral("%1:%2").arg(toolName == QLatin1String("load_module_plugin") ? QStringLiteral("plugin_load")
                                                                                    : QStringLiteral("plugin_unload"),
                                    name);
    if (!snapshot.value(QStringLiteral("found")).toBool())
        return effect;

    effect +=
        QStringLiteral(" target=%1 metadataSha256=%2 loaded=%3")
            .arg(snapshot.value(QStringLiteral("metadataPath")).toString(),
                 snapshot.value(QStringLiteral("metadata"))
                         .toObject()
                         .value(QStringLiteral("sha256"))
                         .toString()
                         .isEmpty()
                     ? QStringLiteral("unavailable")
                     : snapshot.value(QStringLiteral("metadata"))
                           .toObject()
                           .value(QStringLiteral("sha256"))
                           .toString()
                           .left(12),
                 snapshot.value(QStringLiteral("loaded")).toBool() ? QStringLiteral("true") : QStringLiteral("false"));
    const QJsonArray binaries = snapshot.value(QStringLiteral("binaries")).toArray();
    for (const QJsonValue& value : binaries) {
        const QJsonObject binary = value.toObject();
        const QString digest = binary.value(QStringLiteral("sha256")).toString();
        effect += QStringLiteral(" binary=%1 sha256=%2")
                      .arg(binary.value(QStringLiteral("path")).toString(),
                           digest.isEmpty() ? QStringLiteral("unavailable") : digest.left(12));
    }
    return effect;
}

QJsonObject samImageSnapshotForPath(const QString& configuredPath) {
    QJsonObject snapshot{{QStringLiteral("configuredPath"), configuredPath}, {QStringLiteral("readable"), false}};
    if (configuredPath.isEmpty())
        return snapshot;

    const QString targetPath = resolvedFileTargetPath(configuredPath);
    snapshot.insert(QStringLiteral("targetPath"), targetPath);
    QFile file(configuredPath);
    if (targetPath.isEmpty() || !file.open(QIODevice::ReadOnly))
        return snapshot;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 byteCount = 0;
    constexpr qint64 kChunkBytes = 1024 * 1024;
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(kChunkBytes);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
            return snapshot;
        if (chunk.isEmpty())
            break;
        hash.addData(chunk);
        byteCount += chunk.size();
    }
    if (file.error() != QFileDevice::NoError || resolvedFileTargetPath(configuredPath) != targetPath)
        return snapshot;

    snapshot.insert(QStringLiteral("readable"), true);
    snapshot.insert(QStringLiteral("byteCount"), byteCount);
    snapshot.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
    return snapshot;
}

QJsonObject samImageSnapshotForCachedInput(const QString& configuredPath, const QString& targetModuleId,
                                           const ImageData& image) {
    QJsonObject snapshot{{QStringLiteral("configuredPath"), configuredPath},
                         {QStringLiteral("source"), QStringLiteral("cached_input")},
                         {QStringLiteral("targetModuleId"), targetModuleId},
                         {QStringLiteral("readable"), false}};
    QImage pixels = image.toQImage().convertToFormat(QImage::Format_RGBA8888);
    if (pixels.isNull())
        return snapshot;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(pixels.width()));
    hash.addData("x", 1);
    hash.addData(QByteArray::number(pixels.height()));
    for (int row = 0; row < pixels.height(); ++row)
        hash.addData(reinterpret_cast<const char*>(pixels.constScanLine(row)), pixels.bytesPerLine());
    snapshot.insert(QStringLiteral("readable"), true);
    snapshot.insert(QStringLiteral("width"), pixels.width());
    snapshot.insert(QStringLiteral("height"), pixels.height());
    snapshot.insert(QStringLiteral("byteCount"), pixels.sizeInBytes());
    snapshot.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
    return snapshot;
}

QJsonObject samImageSnapshotForPendingPipelineInput(const QString& configuredPath, const QString& targetModuleId) {
    return {{QStringLiteral("configuredPath"), configuredPath},
            {QStringLiteral("source"), QStringLiteral("pipeline_input_not_materialized")},
            {QStringLiteral("targetModuleId"), targetModuleId},
            {QStringLiteral("readable"), false}};
}

QJsonObject samImageSnapshotForInvalidatedContext(const QString& configuredPath, const QString& targetModuleId) {
    return {{QStringLiteral("configuredPath"), configuredPath},
            {QStringLiteral("source"), QStringLiteral("execution_context_invalidated")},
            {QStringLiteral("targetModuleId"), targetModuleId},
            {QStringLiteral("readable"), false}};
}

QSet<QString> grabImageFileSourcesFeedingSamPredict(const Project* project,
                                                    const QString& targetSamPredictId = QString()) {
    QSet<QString> sourceIds;
    if (!project)
        return sourceIds;

    PluginManager& plugins = PluginManager::instance();
    const QList<ModuleInstance> projectModules = project->modules();
    QHash<QString, QString> pluginIds;
    QHash<QString, bool> enabled;
    QList<QString> samPredictIds;
    QHash<QString, ModuleInstance> instanceById;
    QHash<QString, PluginInfo> pluginInfoByInstance;
    for (const ModuleInstance& instance : projectModules) {
        const PluginInfo info = plugins.pluginInfo(instance.moduleId);
        const QString pluginId = info.id;
        instanceById.insert(instance.id, instance);
        pluginInfoByInstance.insert(instance.id, info);
        pluginIds.insert(instance.id, pluginId);
        enabled.insert(instance.id, instance.enabled);
        if (instance.enabled && pluginId == QLatin1String("com.deeplux.plugin.sampredict") &&
            (targetSamPredictId.isEmpty() || targetSamPredictId == instance.id))
            samPredictIds.append(instance.id);
    }

    QHash<QString, QStringList> parentsByChild;
    const auto imagePortMatches = [](const QList<PortSpec>& ports, const QString& configuredPort, int legacyIndex) {
        int imageIndex = -1;
        for (int i = 0; i < ports.size(); ++i) {
            if (ports[i].id == QLatin1String("image")) {
                imageIndex = i;
                break;
            }
        }
        if (!configuredPort.isEmpty())
            return configuredPort == QLatin1String("image") && imageIndex >= 0;
        return imageIndex >= 0 && legacyIndex == imageIndex;
    };
    for (const ModuleConnection& connection : project->connections()) {
        if (connection.edgeType != QLatin1String("data"))
            continue;
        if (!instanceById.contains(connection.fromModuleId) || !instanceById.contains(connection.toModuleId))
            continue;
        const ModuleInstance sourceInstance = instanceById.value(connection.fromModuleId);
        const ModuleInstance targetInstance = instanceById.value(connection.toModuleId);
        const PluginInfo sourceInfo = pluginInfoByInstance.value(sourceInstance.id);
        const PluginInfo targetInfo = pluginInfoByInstance.value(targetInstance.id);
        const bool disabledSourcePassesImage = !sourceInstance.enabled && connection.fromPort == QLatin1String("image");
        const bool disabledTargetPassesImage = !targetInstance.enabled && connection.toPort == QLatin1String("image");
        const bool imageOutput = imagePortMatches(sourceInfo.outputPorts, connection.fromPort, connection.fromOutput) ||
                                 disabledSourcePassesImage;
        const bool imageInput =
            imagePortMatches(targetInfo.inputPorts, connection.toPort, connection.toInput) || disabledTargetPassesImage;
        if (!imageOutput || !imageInput)
            continue;
        parentsByChild[connection.toModuleId].append(connection.fromModuleId);
    }

    QSet<QString> visited;
    while (!samPredictIds.isEmpty()) {
        const QString currentId = samPredictIds.takeFirst();
        if (visited.contains(currentId))
            continue;
        visited.insert(currentId);
        for (const QString& parentId : parentsByChild.value(currentId)) {
            if (enabled.value(parentId) && pluginIds.value(parentId) == QLatin1String("com.deeplux.plugin.grabimage"))
                sourceIds.insert(parentId);
            if (!visited.contains(parentId))
                samPredictIds.append(parentId);
        }
    }
    return sourceIds;
}

QHash<QString, QStringList> samPredictConsumersByGrabImageSource(const Project* project) {
    QHash<QString, QStringList> consumers;
    if (!project)
        return consumers;

    PluginManager& plugins = PluginManager::instance();
    const QList<ModuleInstance> modules = project->modules();
    for (const ModuleInstance& instance : modules) {
        if (!instance.enabled ||
            plugins.pluginInfo(instance.moduleId).id != QLatin1String("com.deeplux.plugin.sampredict"))
            continue;
        for (const QString& sourceId : grabImageFileSourcesFeedingSamPredict(project, instance.id))
            consumers[sourceId].append(instance.id);
    }
    return consumers;
}

QString samImageSideEffect(const QString& configuredPath, const QJsonObject& snapshot) {
    QString effect = QStringLiteral("sam_input_image:%1")
                         .arg(configuredPath.isEmpty() ? QStringLiteral("<未配置>") : configuredPath);
    const QString source = snapshot.value(QStringLiteral("source")).toString();
    if (source == QLatin1String("execution_context_invalidated")) {
        effect += QStringLiteral(" source=execution_context_invalidated:%1 image_snapshot=unavailable")
                      .arg(snapshot.value(QStringLiteral("targetModuleId")).toString());
        return effect;
    }
    if (source == QLatin1String("pipeline_input_not_materialized")) {
        effect += QStringLiteral(" source=pipeline_input_pending:%1 image_snapshot=not_materialized")
                      .arg(snapshot.value(QStringLiteral("targetModuleId")).toString());
        return effect;
    }
    if (source == QLatin1String("cached_input")) {
        effect += QStringLiteral(" source=cached_input_for:%1")
                      .arg(snapshot.value(QStringLiteral("targetModuleId")).toString());
        if (snapshot.value(QStringLiteral("readable")).toBool()) {
            effect += QStringLiteral(" dimensions=%1x%2 pixel_bytes=%3 image_sha256=%4")
                          .arg(snapshot.value(QStringLiteral("width")).toInt())
                          .arg(snapshot.value(QStringLiteral("height")).toInt())
                          .arg(snapshot.value(QStringLiteral("byteCount")).toVariant().toLongLong())
                          .arg(snapshot.value(QStringLiteral("sha256")).toString().left(12));
        } else {
            effect += QStringLiteral(" image_snapshot=unavailable");
        }
        return effect;
    }
    const QString targetPath = snapshot.value(QStringLiteral("targetPath")).toString();
    if (!targetPath.isEmpty() && QDir::cleanPath(QFileInfo(configuredPath).absoluteFilePath()) != targetPath)
        effect += QStringLiteral(" resolved_image_target=%1").arg(targetPath);
    if (snapshot.value(QStringLiteral("readable")).toBool()) {
        effect += QStringLiteral(" bytes=%1 image_sha256=%2")
                      .arg(snapshot.value(QStringLiteral("byteCount")).toVariant().toLongLong())
                      .arg(snapshot.value(QStringLiteral("sha256")).toString().left(12));
    } else {
        effect += QStringLiteral(" image_snapshot=unavailable");
    }
    return effect;
}

QByteArray projectFingerprint(const Project* proj);

QJsonObject cameraCycleCapabilitySnapshot(const Project* project) {
    QJsonObject snapshots;
    if (!project)
        return snapshots;

    CameraManager& cameras = CameraManager::instance();
    const bool queryAvailable = QThread::currentThread() == cameras.thread();
    const int pendingDiscoveries = queryAvailable ? cameras.pendingDiscoveryCount() : -1;
    PluginManager& plugins = PluginManager::instance();
    for (const ModuleInstance& instance : project->modules()) {
        if (!instance.enabled ||
            plugins.pluginInfo(instance.moduleId).id != QLatin1String("com.deeplux.plugin.grabimage"))
            continue;

        PluginManager::ModulePtr module = plugins.createManagedModule(instance.moduleId);
        QJsonObject effective = module ? module->defaultParams() : QJsonObject();
        for (auto it = instance.params.begin(); it != instance.params.end(); ++it)
            effective.insert(it.key(), it.value());
        const bool sourceResolved = effective.contains(QStringLiteral("grabSource"));
        const QString source = effective.value(QStringLiteral("grabSource")).toString();
        if (sourceResolved && source != QLatin1String("Camera"))
            continue;

        const QString configuredDeviceId = effective.value(QStringLiteral("cameraId")).toString().trimmed();
        QString resolvedDeviceId = configuredDeviceId;
        bool asyncCapable = false;
        QJsonObject targetSnapshot;
        if (queryAvailable && sourceResolved) {
            if (!configuredDeviceId.isEmpty()) {
                asyncCapable = cameras.supportsAsyncOperations(configuredDeviceId);
                targetSnapshot = cameras.cameraLifecycleSnapshot(configuredDeviceId);
            } else {
                for (const CameraStatus& status : cameras.knownCameras()) {
                    if ((status.state == CameraState::Connected || status.state == CameraState::Grabbing) &&
                        cameras.hasAsyncCameraSession(status.deviceId)) {
                        resolvedDeviceId = status.deviceId;
                        asyncCapable = true;
                        targetSnapshot = cameras.cameraLifecycleSnapshot(status.deviceId);
                        break;
                    }
                }
            }
        }
        snapshots.insert(QStringLiteral("camera-cycle:") + instance.id,
                         QJsonObject{{QStringLiteral("instanceId"), instance.id},
                                     {QStringLiteral("source"), source},
                                     {QStringLiteral("sourceResolved"), sourceResolved},
                                     {QStringLiteral("configuredDeviceId"), configuredDeviceId},
                                     {QStringLiteral("resolvedDeviceId"), resolvedDeviceId},
                                     {QStringLiteral("queryAvailable"), queryAvailable},
                                     {QStringLiteral("asyncCapable"), asyncCapable},
                                     {QStringLiteral("targetSnapshot"), targetSnapshot},
                                     {QStringLiteral("pendingDiscoveries"), pendingDiscoveries}});
    }
    return snapshots;
}

QJsonObject cameraExecutionSnapshot(const QString& toolName, const Project* project) {
    QJsonObject result{{QStringLiteral("complete"), false},
                       {QStringLiteral("mayRepeat"), false},
                       {QStringLiteral("candidates"), QJsonArray{}}};
    if (!project)
        return result;

    RunExecutionCandidates execution;
    if (toolName == QLatin1String("run_flow"))
        execution = RunEngine::instance().runExecutionCandidates();
    else if (toolName == QLatin1String("step_once"))
        execution = RunEngine::instance().stepExecutionCandidates();
    else if (toolName == QLatin1String("resume_flow"))
        execution = RunEngine::instance().resumeExecutionCandidates();
    else
        return result;

    bool loadedPlanMatches =
        execution.project == project && execution.projectFingerprint.toHex() == projectFingerprint(project);
    if (!loadedPlanMatches && toolName == QLatin1String("resume_flow") && execution.project == project) {
        loadedPlanMatches = RunEngine::instance().isPaused();
    }
    if (!loadedPlanMatches) {
        execution.moduleIds.clear();
        execution.complete = false;
        execution.mayRepeat = false;
    }

    // A fresh, unloaded project can be analyzed exactly only when it has no
    // graph edges and every enabled module is sequential. Never reuse a stale
    // runtime plan or guess a connected graph's next node.
    if (!execution.complete && !loadedPlanMatches && toolName != QLatin1String("resume_flow") &&
        project->connections().isEmpty()) {
        const QList<ModuleInstance> modules = project->modules();
        bool sequentialPlan = true;
        for (const ModuleInstance& instance : modules) {
            if (!instance.enabled)
                continue;
            PluginManager::ModulePtr module = PluginManager::instance().createManagedModule(instance.moduleId);
            if (!module || module->flowControlType() != ControlFlowType::Sequential) {
                sequentialPlan = false;
                break;
            }
            execution.moduleIds.append(instance.id);
        }
        if (sequentialPlan) {
            execution.complete = true;
        } else {
            execution.moduleIds.clear();
        }
        if (toolName == QLatin1String("step_once") && sequentialPlan) {
            const auto firstEnabled = std::find_if(modules.cbegin(), modules.cend(),
                                                   [](const ModuleInstance& instance) { return instance.enabled; });
            execution.moduleIds.clear();
            if (firstEnabled != modules.cend())
                execution.moduleIds.append(firstEnabled->id);
        }
    }

    result.insert(QStringLiteral("complete"), execution.complete);
    result.insert(QStringLiteral("mayRepeat"), execution.mayRepeat);
    QJsonArray moduleIds;
    for (const QString& moduleId : execution.moduleIds)
        moduleIds.append(moduleId);
    result.insert(QStringLiteral("moduleIds"), moduleIds);
    PluginManager& plugins = PluginManager::instance();
    CameraManager& cameras = CameraManager::instance();
    const bool queryAvailable = QThread::currentThread() == cameras.thread();
    const QList<ModuleInstance> projectModules = project->modules();
    QJsonArray candidates;
    for (const QString& moduleId : execution.moduleIds) {
        const auto instanceIt =
            std::find_if(projectModules.cbegin(), projectModules.cend(), [&moduleId](const ModuleInstance& instance) {
                return instance.id == moduleId && instance.enabled;
            });
        if (instanceIt == projectModules.cend() ||
            plugins.pluginInfo(instanceIt->moduleId).id != QLatin1String("com.deeplux.plugin.grabimage"))
            continue;

        PluginManager::ModulePtr module = plugins.createManagedModule(instanceIt->moduleId);
        QJsonObject effective = module ? module->defaultParams() : QJsonObject();
        for (auto it = instanceIt->params.begin(); it != instanceIt->params.end(); ++it)
            effective.insert(it.key(), it.value());
        const QString source = effective.value(QStringLiteral("grabSource")).toString();
        if (effective.contains(QStringLiteral("grabSource")) && source != QLatin1String("Camera"))
            continue;

        const QString deviceId = effective.value(QStringLiteral("cameraId")).toString().trimmed();
        QJsonObject lifecycle;
        bool asyncCapable = false;
        bool hasAsyncSession = false;
        if (queryAvailable && !deviceId.isEmpty()) {
            asyncCapable = cameras.supportsAsyncOperations(deviceId);
            lifecycle = cameras.cameraLifecycleSnapshot(deviceId);
            hasAsyncSession = cameras.hasAsyncCameraSession(deviceId);
        } else if (queryAvailable && deviceId.isEmpty()) {
            for (const CameraStatus& status : cameras.knownCameras()) {
                if ((status.state == CameraState::Connected || status.state == CameraState::Grabbing) &&
                    cameras.hasAsyncCameraSession(status.deviceId)) {
                    asyncCapable = true;
                    hasAsyncSession = true;
                    lifecycle = cameras.cameraLifecycleSnapshot(status.deviceId);
                    break;
                }
            }
        }
        candidates.append(
            QJsonObject{{QStringLiteral("instanceId"), instanceIt->id},
                        {QStringLiteral("deviceId"), deviceId},
                        {QStringLiteral("resolvedDeviceId"), lifecycle.value(QStringLiteral("deviceId"))},
                        {QStringLiteral("sourceResolved"), effective.contains(QStringLiteral("grabSource"))},
                        {QStringLiteral("queryAvailable"), queryAvailable},
                        {QStringLiteral("asyncCapable"), asyncCapable},
                        {QStringLiteral("hasAsyncSession"), hasAsyncSession},
                        {QStringLiteral("lifecycle"), lifecycle}});
    }
    result.insert(QStringLiteral("candidates"), candidates);
    return result;
}

QJsonObject fileTargetPathsForCalls(const QJsonArray& calls, const Project* project) {
    QJsonObject targets;
    if (project) {
        PluginManager& plugins = PluginManager::instance();
        int executionCallIndex = 0;
        for (const QJsonValue& value : calls) {
            const QJsonObject call = value.toObject();
            const QString toolName = toolCallName(call);
            if (hasImplicitProjectSaveTarget(toolName, toolCallParams(call), project))
                targets.insert(QStringLiteral("project-save-current"), currentProjectSaveTargetSnapshot(project));
            if (toolName != QLatin1String("run_flow") && toolName != QLatin1String("step_once") &&
                toolName != QLatin1String("resume_flow"))
                continue;
            const QJsonObject executionSnapshot = cameraExecutionSnapshot(toolName, project);
            targets.insert(QStringLiteral("camera-execution:%1").arg(executionCallIndex++), executionSnapshot);
            QSet<QString> executionModuleIds;
            for (const QJsonValue& value : executionSnapshot.value(QStringLiteral("moduleIds")).toArray())
                executionModuleIds.insert(value.toString());
            const bool executionScopeKnown = executionSnapshot.value(QStringLiteral("complete")).toBool();
            const bool isRunFlowOnce =
                toolName == QLatin1String("run_flow") &&
                toolCallParams(call).value(QStringLiteral("mode")).toString(QStringLiteral("once")) ==
                    QLatin1String("once");
            const QJsonObject runFlowParams = toolCallParams(call);
            const bool isBoundedRunFlowCycle =
                toolName == QLatin1String("run_flow") &&
                runFlowParams.value(QStringLiteral("mode")).toString() == QLatin1String("cycle") &&
                runFlowParams.value(QStringLiteral("maxCycles")).toInt() > 0;
            const bool isMultiCycleRunFlow =
                isBoundedRunFlowCycle && runFlowParams.value(QStringLiteral("maxCycles")).toInt() > 1;
            if (isMultiCycleRunFlow) {
                const QJsonObject cameraSnapshots = cameraCycleCapabilitySnapshot(project);
                for (auto it = cameraSnapshots.begin(); it != cameraSnapshots.end(); ++it)
                    targets.insert(it.key(), it.value());
            }
            const bool capturesSamInput = isRunFlowOnce || isBoundedRunFlowCycle ||
                                          toolName == QLatin1String("step_once") ||
                                          toolName == QLatin1String("resume_flow");
            const QHash<QString, QStringList> samImageConsumers =
                capturesSamInput ? samPredictConsumersByGrabImageSource(project) : QHash<QString, QStringList>();
            for (const ModuleInstance& instance : project->modules()) {
                const QString pluginId = plugins.pluginInfo(instance.moduleId).id;
                if (pluginId == QLatin1String("com.deeplux.plugin.folder")) {
                    PluginManager::ModulePtr module = plugins.createManagedModule(instance.moduleId);
                    QJsonObject effective = module ? module->defaultParams() : QJsonObject();
                    for (auto it = instance.params.begin(); it != instance.params.end(); ++it)
                        effective.insert(it.key(), it.value());
                    const QString folderOperation = effective.value(QStringLiteral("operation")).toString();
                    const QString configuredPath = effective.value(QStringLiteral("folderPath")).toString();
                    if (folderOperation == QLatin1String("Delete") || folderOperation == QLatin1String("Create")) {
                        targets.insert(
                            QStringLiteral("folder-config:") + instance.id,
                            QJsonObject{{QStringLiteral("operation"), folderOperation},
                                        {QStringLiteral("configuredPath"), configuredPath},
                                        {QStringLiteral("targetExistsAtApproval"), QFileInfo(configuredPath).isDir()},
                                        {QStringLiteral("resolvedTarget"),
                                         configuredPath.trimmed().isEmpty() ? QString()
                                                                            : resolvedFileTargetPath(configuredPath)}});
                        if (!configuredPath.trimmed().isEmpty())
                            targets.insert(QStringLiteral("module:") + instance.id,
                                           resolvedFileTargetPath(configuredPath));
                    }
                } else if (pluginId == QLatin1String("com.deeplux.plugin.writetext") ||
                           pluginId == QLatin1String("com.deeplux.plugin.savedata") ||
                           pluginId == QLatin1String("com.deeplux.plugin.saveimage")) {
                    PluginManager::ModulePtr module = plugins.createManagedModule(instance.moduleId);
                    QJsonObject effective = module ? module->defaultParams() : QJsonObject();
                    for (auto it = instance.params.begin(); it != instance.params.end(); ++it)
                        effective.insert(it.key(), it.value());
                    const QString configuredPath = effective.value(QStringLiteral("filePath")).toString();
                    if (!configuredPath.trimmed().isEmpty()) {
                        const QString resolvedTarget = resolvedFileTargetPath(configuredPath);
                        targets.insert(QStringLiteral("module:") + instance.id, resolvedTarget);
                        targets.insert(QStringLiteral("file-output-config:") + instance.id,
                                       QJsonObject{{QStringLiteral("instanceId"), instance.id},
                                                   {QStringLiteral("pluginId"), pluginId},
                                                   {QStringLiteral("params"), effective},
                                                   {QStringLiteral("configuredPath"), configuredPath},
                                                   {QStringLiteral("resolvedTarget"), resolvedTarget}});
                    }
                }

                if (samImageConsumers.contains(instance.id) &&
                    pluginId == QLatin1String("com.deeplux.plugin.grabimage")) {
                    PluginManager::ModulePtr module = plugins.createManagedModule(instance.moduleId);
                    QJsonObject effective = module ? module->defaultParams() : QJsonObject();
                    for (auto it = instance.params.begin(); it != instance.params.end(); ++it)
                        effective.insert(it.key(), it.value());
                    if (effective.value(QStringLiteral("grabSource")).toString() == QLatin1String("Path")) {
                        const QString configuredPath = effective.value(QStringLiteral("filePath")).toString();
                        const bool mayUseCurrentOutput =
                            toolName == QLatin1String("step_once") || toolName == QLatin1String("resume_flow");
                        for (const QString& targetModuleId : samImageConsumers.value(instance.id)) {
                            QJsonObject snapshot;
                            ImageData cachedInput;
                            const RunEngine::CurrentImageInputSnapshot inputSnapshot =
                                mayUseCurrentOutput ? RunEngine::instance().currentExecutionModuleImageInput(
                                                          targetModuleId, instance.id, cachedInput)
                                                    : RunEngine::CurrentImageInputSnapshot::NoContext;
                            if (inputSnapshot == RunEngine::CurrentImageInputSnapshot::ContextChanged) {
                                snapshot = samImageSnapshotForInvalidatedContext(configuredPath, targetModuleId);
                            } else if (inputSnapshot == RunEngine::CurrentImageInputSnapshot::NoContext ||
                                       inputSnapshot == RunEngine::CurrentImageInputSnapshot::UpstreamNotExecuted) {
                                snapshot = samImageSnapshotForPath(configuredPath);
                            } else if (inputSnapshot == RunEngine::CurrentImageInputSnapshot::Available) {
                                snapshot = samImageSnapshotForCachedInput(configuredPath, targetModuleId, cachedInput);
                            } else {
                                snapshot = samImageSnapshotForPendingPipelineInput(configuredPath, targetModuleId);
                            }
                            const bool inputStillPending = snapshot.value(QStringLiteral("source")) ==
                                                           QLatin1String("pipeline_input_not_materialized");
                            if (executionScopeKnown && !executionModuleIds.contains(instance.id) &&
                                !executionModuleIds.contains(targetModuleId) && !inputStillPending)
                                continue;
                            targets.insert(QStringLiteral("module-input-image:") + instance.id + QLatin1Char(':') +
                                               targetModuleId,
                                           snapshot);
                        }
                    }
                }
            }
        }
    }
    for (int callIndex = 0; callIndex < calls.size(); ++callIndex) {
        const QJsonObject call = calls.at(callIndex).toObject();
        const QString toolName = toolCallName(call);
        if (toolName == QLatin1String("import_data_source")) {
            const QString callId = call.value(QStringLiteral("id")).toString();
            QJsonObject snapshot = fileHashSnapshot(toolCallParams(call).value(QStringLiteral("path")).toString());
            snapshot.insert(QStringLiteral("callId"), callId);
            snapshot.insert(QStringLiteral("projectRuntimeIdentity"),
                            project ? static_cast<qint64>(project->runtimeIdentity()) : 0);
            QJsonObject projectBaseline = project ? project->toJson() : QJsonObject();
            QJsonObject fingerprintInput = projectBaseline;
            fingerprintInput.remove(QStringLiteral("modified"));
            const QByteArray projectHash =
                project ? QCryptographicHash::hash(QJsonDocument(fingerprintInput).toJson(QJsonDocument::Compact),
                                                   QCryptographicHash::Sha1)
                        : QByteArray();
            snapshot.insert(QStringLiteral("projectFingerprint"), QString::fromLatin1(projectHash.toHex()));
            snapshot.insert(QStringLiteral("projectBaseline"), projectBaseline);
            snapshot.insert(QStringLiteral("projectDirty"), project ? project->isModified() : false);
            snapshot.insert(QStringLiteral("dataSourceGeneration"),
                            project ? static_cast<qint64>(project->dataSourceCollectionGeneration()) : 0);
            targets.insert(QStringLiteral("data-source-import:") + QString::number(callIndex), snapshot);
        }
        if (toolName == QLatin1String("open_project")) {
            const QString path = toolCallParams(call).value(QStringLiteral("path")).toString();
            if (!path.isEmpty())
                targets.insert(QStringLiteral("project-input:") + QString::number(callIndex), fileHashSnapshot(path));
        }
        const QString callId = call.value(QStringLiteral("id")).toString();
        const QJsonObject params = toolCallParams(call);
        if (toolName == QLatin1String("export_annotation_session") && !callId.isEmpty())
            targets.insert(QStringLiteral("annotation-export:") + callId,
                           AnnotationSessionExport::confirmationSnapshot(params));
        if (toolName == QLatin1String("transform_annotation_session") && !callId.isEmpty())
            targets.insert(QStringLiteral("annotation-transform:") + callId,
                           AnnotationSessionTransform::confirmationSnapshot(params));
        if (toolName == QLatin1String("add_knowledge_document") && !callId.isEmpty()) {
            const QString sourcePath = params.value(QStringLiteral("sourcePath")).toString();
            QJsonObject snapshot = fileHashSnapshot(sourcePath);
            snapshot.insert(QStringLiteral("isSymlink"), QFileInfo(sourcePath).isSymLink());
            snapshot.insert(QStringLiteral("targetName"), QFileInfo(sourcePath).fileName());
            snapshot.insert(QStringLiteral("targetPath"),
                            QDir(AgentActor::userKnowledgeDir()).filePath(QFileInfo(sourcePath).fileName()));
            targets.insert(QStringLiteral("knowledge-call:") + callId, snapshot);
        } else if (toolName == QLatin1String("remove_knowledge_document") && !callId.isEmpty()) {
            const QString documentId = params.value(QStringLiteral("documentId")).toString();
            const QString name = documentId.startsWith(QStringLiteral("user:")) ? documentId.mid(5) : QString();
            const bool safeName = !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
                                  !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
                                  QFileInfo(name).fileName() == name;
            const QString path = safeName ? QDir(AgentActor::userKnowledgeDir()).filePath(name) : QString();
            QJsonObject snapshot = safeName ? fileHashSnapshot(path) : QJsonObject();
            snapshot.insert(QStringLiteral("documentId"), documentId);
            snapshot.insert(QStringLiteral("isSymlink"), !path.isEmpty() && QFileInfo(path).isSymLink());
            targets.insert(QStringLiteral("knowledge-call:") + callId, snapshot);
        }
        if (toolName == QLatin1String("connect_camera") || toolName == QLatin1String("disconnect_camera")) {
            if (!callId.isEmpty()) {
                const QString deviceId = toolCallParams(call).value(QStringLiteral("deviceId")).toString();
                targets.insert(QStringLiteral("camera-call:") + callId,
                               CameraManager::instance().cameraLifecycleSnapshot(deviceId));
            }
            continue;
        }
        if (toolName == QLatin1String("load_module_plugin") || toolName == QLatin1String("unload_module_plugin")) {
            const QString name = toolCallParams(call).value(QStringLiteral("plugin")).toString();
            targets.insert(QStringLiteral("plugin:") + name, pluginLifecycleSnapshot(PluginManager::instance(), name));
            continue;
        }
        if (toolName != QLatin1String("save_project") && toolName != QLatin1String("sam_annotate") &&
            toolName != QLatin1String("sam_predict"))
            continue;
        const QString target = directFileTargetPath(toolName, toolCallParams(call), project);
        if (!target.isEmpty() && !callId.isEmpty())
            targets.insert(QStringLiteral("call:") + callId, target);
        if ((toolName == QLatin1String("sam_predict") || toolName == QLatin1String("sam_annotate")) &&
            !callId.isEmpty()) {
            const QString imagePath = toolCallParams(call).value(QStringLiteral("imagePath")).toString();
            targets.insert(QStringLiteral("sam-image:") + callId, samImageSnapshotForPath(imagePath));
        }
    }
    return targets;
}

QJsonObject executionSideEffectAuthorization(const QJsonObject& call, const QString& confirmationId,
                                             const QJsonObject& frozenTargets) {
    const QString toolName = toolCallName(call);
    if (toolName != QLatin1String("run_flow") && toolName != QLatin1String("step_once") &&
        toolName != QLatin1String("resume_flow"))
        return {};

    QString callId = call.value(QStringLiteral("id")).toString();
    if (callId.isEmpty())
        callId = QStringLiteral("confirmed-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QJsonObject folders;
    QJsonObject fileOutputs;
    if (const Project* project = ProjectManager::instance().currentProject()) {
        PluginManager& plugins = PluginManager::instance();
        for (const ModuleInstance& instance : project->modules()) {
            if (plugins.pluginInfo(instance.moduleId).id != QLatin1String("com.deeplux.plugin.folder"))
                continue;
            const QJsonObject approved = frozenTargets.value(QStringLiteral("folder-config:") + instance.id).toObject();
            if (approved.isEmpty())
                continue;
            const QString operation = approved.value(QStringLiteral("operation")).toString();
            const QString configuredPath = approved.value(QStringLiteral("configuredPath")).toString();
            QString executionTarget;
            if (operation == QLatin1String("Delete") && QFileInfo(configuredPath).isDir())
                executionTarget = frozenTargets.value(QStringLiteral("module:") + instance.id).toString();
            else if (operation == QLatin1String("Create"))
                executionTarget = frozenTargets.value(QStringLiteral("module:") + instance.id).toString();
            folders.insert(instance.id, QJsonObject{{QStringLiteral("operation"), operation},
                                                    {QStringLiteral("configuredPath"), configuredPath},
                                                    {QStringLiteral("targetExistsAtApproval"),
                                                     approved.value(QStringLiteral("targetExistsAtApproval")).toBool()},
                                                    {QStringLiteral("resolvedTarget"), executionTarget}});
        }
        for (const ModuleInstance& instance : project->modules()) {
            const QJsonObject approved =
                frozenTargets.value(QStringLiteral("file-output-config:") + instance.id).toObject();
            if (approved.isEmpty())
                continue;
            fileOutputs.insert(instance.id, approved);
        }
    }
    return QJsonObject{{QStringLiteral("executionAuthorizationId"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                       {QStringLiteral("confirmationId"), confirmationId},
                       {QStringLiteral("callId"), callId},
                       {QStringLiteral("folders"), folders},
                       {QStringLiteral("fileOutputs"), fileOutputs}};
}

QStringList sideEffectsForToolCall(
    const QString& toolName, const QJsonObject& params, const QJsonObject& frozenFileTargets = QJsonObject(),
    const QString& directFileTarget = QString(), const QJsonObject& samImageSnapshot = QJsonObject(),
    const QJsonObject& cameraSnapshot = QJsonObject(), const QJsonObject& projectInputSnapshot = QJsonObject(),
    const QJsonObject& knowledgeSnapshot = QJsonObject(), const QJsonObject& dataSourceImportSnapshot = QJsonObject()) {
    QStringList effects;
    if (toolName == QLatin1String("load_module_plugin") || toolName == QLatin1String("unload_module_plugin")) {
        const QString name = params.value(QStringLiteral("plugin")).toString();
        effects << pluginLifecycleSideEffect(toolName, name,
                                             frozenFileTargets.value(QStringLiteral("plugin:") + name).toObject());
    } else if (toolName == QLatin1String("connect_camera") || toolName == QLatin1String("disconnect_camera")) {
        const QString action = toolName == QLatin1String("connect_camera") ? QStringLiteral("camera_connect")
                                                                           : QStringLiteral("camera_disconnect");
        QString effect = QStringLiteral("%1:%2").arg(action, params.value(QStringLiteral("deviceId")).toString());
        if (cameraSnapshot.value(QStringLiteral("found")).toBool()) {
            effect += QStringLiteral(" owner=%1 name=%2 serial=%3 ip=%4 state=%5 generation=%6 sessionGeneration=%7")
                          .arg(cameraSnapshot.value(QStringLiteral("ownerPluginId")).toString(),
                               cameraSnapshot.value(QStringLiteral("name")).toString(),
                               cameraSnapshot.value(QStringLiteral("serialNumber")).toString(),
                               cameraSnapshot.value(QStringLiteral("ipAddress")).toString(),
                               cameraSnapshot.value(QStringLiteral("state")).toString(),
                               cameraSnapshot.value(QStringLiteral("deviceGeneration")).toString(),
                               cameraSnapshot.value(QStringLiteral("sessionGeneration")).toString());
        } else {
            effect += QStringLiteral(" target=unavailable");
        }
        effects << effect;
    } else if (toolName == QLatin1String("set_camera_parameter")) {
        const QByteArray compactValue =
            QJsonDocument(QJsonArray{params.value(QStringLiteral("value"))}).toJson(QJsonDocument::Compact);
        effects << QStringLiteral("camera_parameter:%1:%2=%3")
                       .arg(params.value(QStringLiteral("deviceId")).toString(),
                            params.value(QStringLiteral("parameter")).toString(),
                            QString::fromUtf8(compactValue.mid(1, compactValue.size() - 2)));
    } else if (toolName == QLatin1String("discover_cameras")) {
        for (const QJsonValue& value : params.value(QStringLiteral("pluginIds")).toArray()) {
            if (value.isString())
                effects << QStringLiteral("camera_discovery:%1").arg(value.toString());
        }
    } else if (toolName == QLatin1String("save_project")) {
        QString path = params.value(QStringLiteral("path")).toString();
        if (path.isEmpty()) {
            const Project* project = ProjectManager::instance().currentProject();
            if (project)
                path = project->filePath();
        }
        if (path.isEmpty())
            path = QStringLiteral("<当前路径>");
        QString effect = QStringLiteral("file_write:%1").arg(path);
        if (!directFileTarget.isEmpty() && QDir::cleanPath(QFileInfo(path).absoluteFilePath()) != directFileTarget)
            effect += QStringLiteral(" resolved_target=%1").arg(directFileTarget);
        effects << effect;
    } else if (toolName == QLatin1String("add_knowledge_document")) {
        effects << QStringLiteral("knowledge_import:%1 -> %2")
                       .arg(knowledgeSnapshot.value(QStringLiteral("path")).toString(),
                            knowledgeSnapshot.value(QStringLiteral("targetPath")).toString());
    } else if (toolName == QLatin1String("remove_knowledge_document")) {
        effects
            << QStringLiteral("knowledge_remove:%1").arg(knowledgeSnapshot.value(QStringLiteral("path")).toString());
    } else if (toolName == QLatin1String("create_global_variable")) {
        effects << QStringLiteral("global_variable_create:%1 (%2)")
                       .arg(params.value(QStringLiteral("name")).toString(),
                            params.value(QStringLiteral("type")).toString());
    } else if (toolName == QLatin1String("set_global_variable")) {
        effects << QStringLiteral("global_variable_set:%1").arg(params.value(QStringLiteral("name")).toString());
    } else if (toolName == QLatin1String("remove_global_variable")) {
        effects << QStringLiteral("global_variable_remove:%1").arg(params.value(QStringLiteral("name")).toString());
    } else if (toolName == QLatin1String("remove_data_source_reference")) {
        effects << QStringLiteral("project_change:data_source_reference:%1; source_file=preserved")
                       .arg(params.value(QStringLiteral("dataSourceId")).toString());
    } else if (toolName == QLatin1String("import_data_source")) {
        effects << QStringLiteral("project_add_data_source:%1 (register reference only; source file unchanged)")
                       .arg(dataSourceImportSnapshot.value(QStringLiteral("resolvedPath"))
                                .toString(params.value(QStringLiteral("path")).toString()));
    } else if (toolName == QLatin1String("display_data_source")) {
        effects << QStringLiteral("gui_display_data_source:%1")
                       .arg(params.value(QStringLiteral("dataSourceId")).toString());
    } else if (toolName == QLatin1String("display_data_source_in_dedicated_viewport")) {
        effects << QStringLiteral("gui_display_data_source_in_dedicated_viewport:%1 (read-only source)")
                       .arg(params.value(QStringLiteral("dataSourceId")).toString());
    } else if (toolName == QLatin1String("open_data_source_folder_for_user")) {
        effects << QStringLiteral("desktop_file_manager_request_for_data_source:%1 (parent path hidden)")
                       .arg(params.value(QStringLiteral("dataSourceId")).toString());
    } else if (toolName == QLatin1String("open_user_knowledge_folder_for_user")) {
        effects << QStringLiteral("desktop_file_manager_request_for_user_knowledge_folder (path hidden)");
    } else if (toolName == QLatin1String("copy_data_source_path_to_clipboard")) {
        QString path = QStringLiteral("<路径不可用>");
        if (const Project* project = ProjectManager::instance().currentProject()) {
            int matches = 0;
            for (const DataSource& source : project->dataSources()) {
                if (source.id == params.value(QStringLiteral("dataSourceId")).toString()) {
                    ++matches;
                    path = source.filePath;
                }
            }
            if (matches > 1)
                path = QStringLiteral("<数据源 ID 不唯一>");
            else if (matches == 0 || path.isEmpty())
                path = QStringLiteral("<路径不可用>");
        }
        effects << QStringLiteral("clipboard_replace:%1 (replaces the current system clipboard)").arg(path);
    } else if (toolName == QLatin1String("set_data_source_visibility")) {
        effects << QStringLiteral("gui_set_data_source_visibility:%1:%2")
                       .arg(params.value(QStringLiteral("dataSourceId")).toString(),
                            params.value(QStringLiteral("visible")).toBool() ? QStringLiteral("true")
                                                                             : QStringLiteral("false"));
    } else if (toolName == QLatin1String("begin_quick_measurement")) {
        effects << QStringLiteral("gui_human_measurement:%1:%2:%3 (user picks only; project and files unchanged)")
                       .arg(params.value(QStringLiteral("viewportId")).toString(),
                            params.value(QStringLiteral("dimension")).toString(),
                            params.value(QStringLiteral("measurementType")).toString());
    } else if (toolName == QLatin1String("edit_active_annotation_masks")) {
        effects << QStringLiteral("gui_sam_mask_edit:%1 objects=%2 strokePointCount=%3; in-memory active session only; "
                                  "GUI undoable; project and files unchanged")
                       .arg(params.value(QStringLiteral("operation")).toString())
                       .arg(params.value(QStringLiteral("objectIds")).toArray().size())
                       .arg(params.value(QStringLiteral("strokePoints")).toArray().size());
    } else if (toolName == QLatin1String("edit_active_annotation_categories")) {
        const QString operation = params.value(QStringLiteral("operation")).toString();
        effects
            << (operation == QLatin1String("set_object_label")
                    ? QStringLiteral("gui_sam_object_labels:%1 objects=%2; in-memory session only; GUI undoable; "
                                     "project and files unchanged")
                          .arg(operation)
                          .arg(params.value(QStringLiteral("objectIds")).toArray().size())
                    : QStringLiteral("gui_sam_category_list:%1; active dialog only; not saved to disk").arg(operation));
    } else if (toolName == QLatin1String("cancel_active_annotation_prompt")) {
        effects << QStringLiteral("gui_sam_cancel_transient_prompt_prediction_and_candidates; preserves confirmed "
                                  "annotations, object masks, selection, undo/redo, image binding, project, and files");
    } else if (toolName == QLatin1String("save_active_annotation_session")) {
        const QString pathKey = QStringLiteral("sam-session-save-path:") +
                                QDir::cleanPath(params.value(QStringLiteral("outputPath")).toString());
        const QJsonObject snapshot = frozenFileTargets.value(pathKey).toObject();
        const QJsonObject target = snapshot.value(QStringLiteral("outputTarget")).toObject();
        const QString resolvedPath = QDir(target.value(QStringLiteral("outputCanonicalParent")).toString())
                                         .filePath(target.value(QStringLiteral("outputFileName")).toString());
        effects << QStringLiteral("file_create:%1 native SAM session objectCount=%2 geometry+masks+image-path-metadata "
                                  "(atomic no-replace)")
                       .arg(QDir::cleanPath(resolvedPath))
                       .arg(snapshot.value(QStringLiteral("objectCount")).toInt());
    } else if (toolName == QLatin1String("open_active_annotation_session")) {
        const QString pathKey = QStringLiteral("sam-session-open-path:") +
                                QDir::cleanPath(params.value(QStringLiteral("sessionPath")).toString());
        const QJsonObject snapshot = frozenFileTargets.value(pathKey).toObject();
        effects << QStringLiteral("gui_sam_session_replace:%1 objectCount=%2; clears prompts/candidates and SAM "
                                  "undo/redo; may create application-managed temporary backend image; source files "
                                  "unchanged")
                       .arg(snapshot.value(QStringLiteral("sessionPath")).toString())
                       .arg(snapshot.value(QStringLiteral("objectCount")).toInt());
    } else if (toolName == QLatin1String("run_flow") || toolName == QLatin1String("step_once") ||
               toolName == QLatin1String("resume_flow")) {
        if (toolName == QLatin1String("run_flow") &&
            params.value(QStringLiteral("mode")).toString(QStringLiteral("once")) == QLatin1String("cycle")) {
            effects << QStringLiteral("cycle_session:maxCycles=%1 interval=100ms")
                           .arg(params.value(QStringLiteral("maxCycles")).toInt());
        }
        effects << (toolName == QLatin1String("resume_flow")
                        ? QStringLiteral("execution:resume_paused_session_may_touch_files_devices_network")
                        : QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"));
        const Project* project = ProjectManager::instance().currentProject();
        const bool isBoundedRunFlowCycle = toolName == QLatin1String("run_flow") &&
                                           params.value(QStringLiteral("mode")).toString() == QLatin1String("cycle") &&
                                           params.value(QStringLiteral("maxCycles")).toInt() > 0;
        const bool capturesSamInput =
            (toolName == QLatin1String("run_flow") &&
             params.value(QStringLiteral("mode")).toString(QStringLiteral("once")) == QLatin1String("once")) ||
            isBoundedRunFlowCycle || toolName == QLatin1String("step_once") || toolName == QLatin1String("resume_flow");
        const QHash<QString, QStringList> samImageConsumers =
            capturesSamInput ? samPredictConsumersByGrabImageSource(project) : QHash<QString, QStringList>();
        if (project) {
            PluginManager& plugins = PluginManager::instance();
            const QJsonObject executionSnapshot = cameraExecutionSnapshot(toolName, project);
            QSet<QString> executionModuleIds;
            for (const QJsonValue& value : executionSnapshot.value(QStringLiteral("moduleIds")).toArray())
                executionModuleIds.insert(value.toString());
            const bool executionScopeKnown = executionSnapshot.value(QStringLiteral("complete")).toBool();
            for (const ModuleInstance& instance : project->modules()) {
                const QString pluginId = plugins.pluginInfo(instance.moduleId).id;
                const QStringList samConsumers = samImageConsumers.value(instance.id);
                bool hasBoundSamInput = false;
                for (const QString& consumerId : samConsumers) {
                    const QString targetKey =
                        QStringLiteral("module-input-image:") + instance.id + QLatin1Char(':') + consumerId;
                    hasBoundSamInput = hasBoundSamInput || frozenFileTargets.contains(targetKey);
                }
                const bool isSamInputSourceForExecution =
                    pluginId == QLatin1String("com.deeplux.plugin.grabimage") && hasBoundSamInput;
                if (executionScopeKnown && !executionModuleIds.contains(instance.id) && !isSamInputSourceForExecution)
                    continue;
                if (pluginId != QLatin1String("com.deeplux.plugin.plcwrite") &&
                    pluginId != QLatin1String("com.deeplux.plugin.folder") &&
                    pluginId != QLatin1String("com.deeplux.plugin.grabimage") &&
                    pluginId != QLatin1String("com.deeplux.plugin.writetext") &&
                    pluginId != QLatin1String("com.deeplux.plugin.savedata") &&
                    pluginId != QLatin1String("com.deeplux.plugin.saveimage") &&
                    pluginId != QLatin1String("com.deeplux.plugin.plcread") &&
                    pluginId != QLatin1String("com.deeplux.plugin.tcpclient") &&
                    pluginId != QLatin1String("com.deeplux.plugin.tcpserver") &&
                    pluginId != QLatin1String("com.deeplux.plugin.serialport") &&
                    pluginId != QLatin1String("com.deeplux.plugin.plccommunicate") &&
                    pluginId != QLatin1String("com.deeplux.plugin.sampredict"))
                    continue;
                PluginManager::ModulePtr module = plugins.createManagedModule(instance.moduleId);
                QJsonObject effective = module ? module->defaultParams() : QJsonObject();
                for (auto it = instance.params.begin(); it != instance.params.end(); ++it)
                    effective.insert(it.key(), it.value());
                if (pluginId == QLatin1String("com.deeplux.plugin.plcwrite")) {
                    const QString host =
                        effective.value(QStringLiteral("ipAddress")).toString(QStringLiteral("<未知>"));
                    const QJsonValue port = effective.value(QStringLiteral("port"));
                    const QString portText = port.isDouble() ? QString::number(port.toInt()) : QStringLiteral("<未知>");
                    const QJsonValue configuredStart = effective.value(QStringLiteral("startAddress"));
                    const int startAddress = configuredStart.toInt(-1);
                    const QString startText = startAddress >= 0 && startAddress <= 65535 ? QString::number(startAddress)
                                                                                         : QStringLiteral("<invalid>");
                    const QString valueSource = effective.value(QStringLiteral("valueSource")).toString();
                    QString rangeDetails;
                    if (valueSource != QLatin1String("immediate")) {
                        const QString sourceLabel =
                            valueSource == QLatin1String("variable")
                                ? QStringLiteral("variable:%1")
                                      .arg(effective.value(QStringLiteral("values")).toString())
                                : QStringLiteral("unknown:%1")
                                      .arg(valueSource.isEmpty() ? QStringLiteral("<unset>") : valueSource);
                        rangeDetails = QStringLiteral("quantity=<runtime> end=<runtime> "
                                                      "range=unresolved-at-confirmation valueSource=%1")
                                           .arg(sourceLabel);
                    } else {
                        const int quantity = effective.value(QStringLiteral("values")).toArray().size();
                        const qint64 endAddress = static_cast<qint64>(startAddress) + quantity - 1;
                        const bool rangeValid = startAddress >= 0 && startAddress <= 65535 && quantity > 0 &&
                                                quantity <= 123 && endAddress <= 65535;
                        const QString endText =
                            quantity > 0 ? QString::number(endAddress) : QStringLiteral("<invalid>");
                        rangeDetails = QStringLiteral("quantity=%1 end=%2").arg(quantity).arg(endText);
                        if (!rangeValid)
                            rangeDetails += QStringLiteral(" range=invalid");
                        rangeDetails += QStringLiteral(" valueSource=immediate");
                    }
                    QString effect = QStringLiteral("plc_write:%1@%2:%3 slave=%4 address=%5 %6")
                                         .arg(instance.id, host, portText,
                                              QString::number(effective.value(QStringLiteral("slaveId")).toInt()),
                                              startText, rangeDetails);
                    effects << effect;
                } else if (pluginId == QLatin1String("com.deeplux.plugin.folder")) {
                    const QString operation = effective.value(QStringLiteral("operation")).toString();
                    const QString folderPath = effective.value(QStringLiteral("folderPath")).toString();
                    if (operation == QLatin1String("Delete")) {
                        QString effect = QStringLiteral("folder_delete:%1@%2").arg(instance.id, folderPath);
                        const QString targetKey = QStringLiteral("module:") + instance.id;
                        if (frozenFileTargets.contains(targetKey)) {
                            const QString resolvedTarget = frozenFileTargets.value(targetKey).toString();
                            if (resolvedTarget.isEmpty()) {
                                effect += QStringLiteral(" resolved_target=<unavailable>");
                            } else if (QDir::cleanPath(QFileInfo(folderPath).absoluteFilePath()) != resolvedTarget) {
                                effect += QStringLiteral(" resolved_target=%1").arg(resolvedTarget);
                            }
                        }
                        effects << effect;
                    } else if (operation == QLatin1String("Create")) {
                        QString effect = QStringLiteral("folder_create:%1@%2").arg(instance.id, folderPath);
                        const QString targetKey = QStringLiteral("module:") + instance.id;
                        if (frozenFileTargets.contains(targetKey)) {
                            const QString resolvedTarget = frozenFileTargets.value(targetKey).toString();
                            if (resolvedTarget.isEmpty()) {
                                effect += QStringLiteral(" resolved_target=<unavailable>");
                            } else if (QDir::cleanPath(QFileInfo(folderPath).absoluteFilePath()) != resolvedTarget) {
                                effect += QStringLiteral(" resolved_target=%1").arg(resolvedTarget);
                            }
                        }
                        effects << effect;
                    }
                } else if (pluginId == QLatin1String("com.deeplux.plugin.grabimage")) {
                    const QString grabSource = effective.value(QStringLiteral("grabSource")).toString();
                    if (grabSource == QLatin1String("Camera")) {
                        const QString cameraId = effective.value(QStringLiteral("cameraId")).toString();
                        const QString target =
                            cameraId.trimmed().isEmpty() ? QStringLiteral("<explicit-camera-id-required>") : cameraId;
                        const double exposure = effective.value(QStringLiteral("exposureTime")).toDouble(10000.0);
                        const double gain = effective.value(QStringLiteral("gain")).toDouble(1.0);
                        const int timeout = effective.value(QStringLiteral("grabTimeout")).toInt(5000);
                        effects << QStringLiteral("camera_capture:%1@%2 exposure=%3 gain=%4 timeout=%5ms")
                                       .arg(instance.id, target, QString::number(exposure), QString::number(gain),
                                            QString::number(timeout));
                    } else if (grabSource == QLatin1String("Path") && samImageConsumers.contains(instance.id)) {
                        const QString configuredPath = effective.value(QStringLiteral("filePath")).toString();
                        for (const QString& targetModuleId : samImageConsumers.value(instance.id)) {
                            const QString targetKey =
                                QStringLiteral("module-input-image:") + instance.id + QLatin1Char(':') + targetModuleId;
                            if (!frozenFileTargets.contains(targetKey))
                                continue;
                            effects << samImageSideEffect(configuredPath,
                                                          frozenFileTargets.value(targetKey).toObject());
                        }
                    }
                } else if (pluginId == QLatin1String("com.deeplux.plugin.writetext") ||
                           pluginId == QLatin1String("com.deeplux.plugin.savedata") ||
                           pluginId == QLatin1String("com.deeplux.plugin.saveimage")) {
                    const QString configuredPath = effective.value(QStringLiteral("filePath")).toString();
                    const QString outputPath =
                        configuredPath.isEmpty() && pluginId != QLatin1String("com.deeplux.plugin.saveimage")
                            ? QStringLiteral("<运行时输入路径或未配置>")
                        : configuredPath.isEmpty() ? QStringLiteral("<未配置>")
                                                   : configuredPath;
                    QString effect = QStringLiteral("file_write:%1").arg(outputPath);
                    const QString resolvedTarget =
                        frozenFileTargets.value(QStringLiteral("module:") + instance.id).toString();
                    if (!resolvedTarget.isEmpty() && !configuredPath.isEmpty() &&
                        QDir::cleanPath(QFileInfo(configuredPath).absoluteFilePath()) != resolvedTarget)
                        effect += QStringLiteral(" resolved_target=%1").arg(resolvedTarget);
                    if (pluginId == QLatin1String("com.deeplux.plugin.writetext")) {
                        effect += QStringLiteral(" format=text mode=%1")
                                      .arg(effective.value(QStringLiteral("appendMode")).toBool()
                                               ? QStringLiteral("append")
                                               : QStringLiteral("overwrite"));
                    } else if (pluginId == QLatin1String("com.deeplux.plugin.savedata")) {
                        const QString format =
                            effective.value(QStringLiteral("fileFormat")).toString(QStringLiteral("json"));
                        const bool appendMode = effective.value(QStringLiteral("appendMode")).toBool();
                        const QString mode = !appendMode ? QStringLiteral("overwrite")
                                             : format == QLatin1String("json")
                                                 ? QStringLiteral("merge_json_keys_if_existing_object_else_overwrite")
                                                 : QStringLiteral("append");
                        effect += QStringLiteral(" format=%1 mode=%2").arg(format, mode);
                    }
                    effects << effect;
                } else if (pluginId == QLatin1String("com.deeplux.plugin.tcpclient")) {
                    const QString host = effective.value(QStringLiteral("host")).toString(QStringLiteral("<未配置>"));
                    const QJsonValue configuredPort = effective.value(QStringLiteral("port"));
                    const QString port = configuredPort.isDouble() ? QString::number(configuredPort.toInt())
                                                                   : QStringLiteral("<未配置>");
                    const QString operation =
                        effective.value(QStringLiteral("operation")).toString(QStringLiteral("<未配置>"));
                    const int timeout = effective.value(QStringLiteral("timeout")).toInt(3000);
                    effects << QStringLiteral("tcp_client:%1@%2:%3 operation=%4 timeout=%5ms")
                                   .arg(instance.id, host, port, operation, QString::number(timeout));
                } else if (pluginId == QLatin1String("com.deeplux.plugin.tcpserver")) {
                    const QString configuredAddress =
                        effective.value(QStringLiteral("bindAddress")).toString(QStringLiteral("Any"));
                    const QString address =
                        configuredAddress == QLatin1String("LocalHost") ? QStringLiteral("127.0.0.1/loopback")
                        : configuredAddress == QLatin1String("Any")     ? QStringLiteral("0.0.0.0/all_interfaces")
                                                                        : QStringLiteral("<invalid>");
                    const QJsonValue configuredPort = effective.value(QStringLiteral("port"));
                    const QString port = configuredPort.isDouble() ? QString::number(configuredPort.toInt())
                                                                   : QStringLiteral("<invalid>");
                    const int timeout = effective.value(QStringLiteral("timeout")).toInt(5000);
                    const QString operation =
                        effective.value(QStringLiteral("operation")).toString(QStringLiteral("WriteRead"));
                    effects << QStringLiteral("tcp_server:%1@%2:%3 timeout=%4ms operation=%5")
                                   .arg(instance.id, address, port, QString::number(timeout), operation);
                } else if (pluginId == QLatin1String("com.deeplux.plugin.plcread")) {
                    const QString host =
                        effective.value(QStringLiteral("ipAddress")).toString(QStringLiteral("<未知>"));
                    const QJsonValue configuredPort = effective.value(QStringLiteral("port"));
                    const QString port =
                        configuredPort.isDouble() ? QString::number(configuredPort.toInt()) : QStringLiteral("<未知>");
                    const QString slave = QString::number(effective.value(QStringLiteral("slaveId")).toInt(1));
                    const QString startAddress =
                        QString::number(effective.value(QStringLiteral("startAddress")).toInt());
                    const QString readCount = QString::number(effective.value(QStringLiteral("readCount")).toInt(10));
                    const QString timeout = QString::number(effective.value(QStringLiteral("timeout")).toInt(3000));
                    effects << QStringLiteral("plc_read:%1@%2:%3 slave=%4 start=%5 count=%6 timeout=%7ms")
                                   .arg(instance.id, host, port, slave, startAddress, readCount, timeout);
                } else if (pluginId == QLatin1String("com.deeplux.plugin.serialport")) {
                    const QString portName =
                        effective.value(QStringLiteral("portName")).toString(QStringLiteral("<未配置>"));
                    const QString baudRate = QString::number(effective.value(QStringLiteral("baudRate")).toInt(9600));
                    const QString dataBits = QString::number(effective.value(QStringLiteral("dataBits")).toInt(8));
                    const QString parity = effective.value(QStringLiteral("parity")).toString(QStringLiteral("None"));
                    const QString stopBits =
                        effective.value(QStringLiteral("stopBits")).toString(QStringLiteral("One"));
                    const QString flowControl =
                        effective.value(QStringLiteral("flowControl")).toString(QStringLiteral("None"));
                    const QString operation =
                        effective.value(QStringLiteral("operation")).toString(QStringLiteral("WriteRead"));
                    const QString timeout = QString::number(effective.value(QStringLiteral("timeout")).toInt(100));
                    effects
                        << QStringLiteral(
                               "serial_port:%1@%2 baud=%3 data=%4 parity=%5 stop=%6 flow=%7 operation=%8 timeout=%9ms")
                               .arg(instance.id, portName, baudRate, dataBits, parity, stopBits, flowControl, operation,
                                    timeout);
                } else if (pluginId == QLatin1String("com.deeplux.plugin.plccommunicate")) {
                    const QString host =
                        effective.value(QStringLiteral("ipAddress")).toString(QStringLiteral("<未配置>"));
                    const QJsonValue configuredPort = effective.value(QStringLiteral("port"));
                    const QString port = configuredPort.isDouble() ? QString::number(configuredPort.toInt())
                                                                   : QStringLiteral("<未配置>");
                    const QString timeout = QString::number(effective.value(QStringLiteral("timeout")).toInt(3000));
                    effects << QStringLiteral("plc_communicate:%1@%2:%3 function=03 address=0 count=1 timeout=%4ms")
                                   .arg(instance.id, host, port, timeout);
                } else if (pluginId == QLatin1String("com.deeplux.plugin.sampredict")) {
                    const QString serverUrl =
                        effective.value(QStringLiteral("serverUrl")).toString(QStringLiteral("http://127.0.0.1:8990"));
                    effects << QStringLiteral("sam_predict_plugin:%1@%2 upstream_image=sent")
                                   .arg(instance.id, safeSamServerOrigin(serverUrl));
                }
            }
        }
    } else if (toolName == QLatin1String("export_annotation_session")) {
        effects << QStringLiteral("file_create:%1 format=%2 (atomic no-replace)")
                       .arg(params.value(QStringLiteral("outputPath")).toString(),
                            params.value(QStringLiteral("format")).toString());
    } else if (toolName == QLatin1String("transform_annotation_session")) {
        effects << QStringLiteral("file_create:%1 operation=%2 objects=%3 (atomic no-replace; source preserved)")
                       .arg(params.value(QStringLiteral("outputPath")).toString(),
                            params.value(QStringLiteral("operation")).toString())
                       .arg(params.value(QStringLiteral("objectIds")).toArray().size());
    } else if (toolName == QLatin1String("sam_annotate")) {
        const QString path = params.value(QStringLiteral("sessionPath")).toString();
        QString effect = QStringLiteral("file_write:%1").arg(path);
        if (!directFileTarget.isEmpty() && QDir::cleanPath(QFileInfo(path).absoluteFilePath()) != directFileTarget)
            effect += QStringLiteral(" resolved_target=%1").arg(directFileTarget);
        effects << effect << samImageSideEffect(params.value(QStringLiteral("imagePath")).toString(), samImageSnapshot)
                << QStringLiteral("inference:sam_server");
    } else if (toolName == QLatin1String("sam_predict")) {
        const QString imagePath = params.value(QStringLiteral("imagePath")).toString(QStringLiteral("<未配置>"));
        const QString serverUrl =
            params.value(QStringLiteral("serverUrl")).toString(QStringLiteral("http://127.0.0.1:8990"));
        effects << QStringLiteral("sam_predict:image=%1 server=%2").arg(imagePath, safeSamServerOrigin(serverUrl))
                << samImageSideEffect(params.value(QStringLiteral("imagePath")).toString(), samImageSnapshot);
    } else if (toolName == QLatin1String("open_project")) {
        if (hasImplicitProjectSaveTarget(toolName, params, ProjectManager::instance().currentProject())) {
            const QJsonObject snapshot = frozenFileTargets.value(QStringLiteral("project-save-current")).toObject();
            const QString configuredPath = snapshot.value(QStringLiteral("configuredPath")).toString();
            const QString targetPath = snapshot.value(QStringLiteral("resolvedTarget")).toString();
            QString effect =
                QStringLiteral("file_write:%1 (implicit current project save before switch)").arg(configuredPath);
            if (!targetPath.isEmpty() && QDir::cleanPath(QFileInfo(configuredPath).absoluteFilePath()) != targetPath)
                effect += QStringLiteral(" resolved_target=%1").arg(targetPath);
            effects << effect;
        }
        const QString path = params.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            effects << projectInputSideEffect(path, projectInputSnapshot);
        effects << QStringLiteral("project_switch:%1").arg(params.value("path").toString());
    } else if (toolName == QLatin1String("create_project")) {
        if (hasImplicitProjectSaveTarget(toolName, params, ProjectManager::instance().currentProject())) {
            const QJsonObject snapshot = frozenFileTargets.value(QStringLiteral("project-save-current")).toObject();
            const QString configuredPath = snapshot.value(QStringLiteral("configuredPath")).toString();
            const QString targetPath = snapshot.value(QStringLiteral("resolvedTarget")).toString();
            QString effect =
                QStringLiteral("file_write:%1 (implicit current project save before switch)").arg(configuredPath);
            if (!targetPath.isEmpty() && QDir::cleanPath(QFileInfo(configuredPath).absoluteFilePath()) != targetPath)
                effect += QStringLiteral(" resolved_target=%1").arg(targetPath);
            effects << effect;
        }
        effects << QStringLiteral("project_switch:new");
    }
    return effects;
}

// 阶段1 §2：被拒绝调用的结构化结果（进入对话与审计，保证每个 call 恰有一个结果）
QJsonObject deniedToolResult(const QString& toolName, const QString& levelName, const AgentActor* actor,
                             const QJsonObject& params) {
    if (toolName.isEmpty())
        return QJsonObject{{"error", QStringLiteral("Tool name missing in tool call")},
                           {"errorCode", QStringLiteral("UNKNOWN_TOOL")},
                           {"retryable", false}};
    const QJsonObject userPermissionError = actor ? actor->userPermissionPreflight(toolName, params) : QJsonObject();
    if (!userPermissionError.isEmpty())
        return userPermissionError;
    return QJsonObject{
        {"error", QStringLiteral("Permission denied: %1 mode does not allow '%2'").arg(levelName, toolName)},
        {"errorCode", QStringLiteral("PERMISSION_DENIED")},
        {"retryable", false}};
}

QJsonObject makeToolCall(const QString& toolName, const QJsonObject& params) {
    return QJsonObject{
        {"id", QString("manual_%1").arg(toolName)},
        {"type", "function"},
        {"name", toolName},
        {"arguments", params},
    };
}
} // namespace

AgentController::AgentController(QObject* parent)
    : QObject(parent), m_observer(new AgentObserver(this)), m_actor(new AgentActor(this)) {}

AgentController::~AgentController() = default;

AgentController& AgentController::instance() {
    static AgentController instance;
    return instance;
}

bool AgentController::initialize() {
    if (m_initialized)
        return true;
    qRegisterMetaType<AgentResponse>("AgentResponse");
    qRegisterMetaType<AgentResponse>("DeepLux::AgentResponse");
    qRegisterMetaType<AgentActionLogEntry>("AgentActionLogEntry");
    qRegisterMetaType<AgentActionLogEntry>("DeepLux::AgentActionLogEntry");
    ToolSchema::instance().registerDefaultTools();
    if (!m_observer->initialize()) {
        qWarning() << "AgentController: Failed to initialize AgentObserver";
        return false;
    }
    connect(m_observer, &AgentObserver::guiEventOccurred, this, &AgentController::onGuiEvent);
    AgentBridge::instance().setToolCallCallback(
        [this](const QString& n, const QJsonObject& p) { return handleToolCall(n, p); });
    connect(m_actor, &AgentActor::toolExecuted, this,
            [this](const QString& t, const QJsonObject& r) { emit agentActionCompleted(t, r); });
    connect(m_actor, &AgentActor::toolError, this, [this](const QString& t, const QString& e) {
        emit agentActionCompleted(t, {{"error", e}});
    });
    m_systemPrompt = ConfigManager::instance().groupString("agent", "systemPrompt", defaultSystemPrompt());
    m_initialized = true;
    AgentAuditLog::instance().initialize(this);
    AgentToolImage::cleanupOlderThanDays(7);
    return true;
}

void AgentController::registerDataSourceViewHandler(QObject* owner, DataSourceViewHandler handler) {
    if (!owner || !handler || owner->thread() != thread()) {
        m_dataSourceViewOwner.clear();
        m_dataSourceViewHandler = {};
        return;
    }
    if (m_dataSourceViewOwner && m_dataSourceViewOwner != owner && m_dataSourceViewHandler) {
        const QJsonObject terminal = m_dataSourceViewHandler(QStringLiteral("__quick_measurement_owner_destroyed"), {});
        if (!terminal.value(QStringLiteral("requestId")).toString().isEmpty())
            m_lastQuickMeasurementStatus = terminal;
    }
    m_dataSourceViewOwner = owner;
    m_dataSourceViewHandler = std::move(handler);
}

void AgentController::unregisterDataSourceViewHandler(QObject* owner) {
    if (owner && m_dataSourceViewOwner == owner) {
        if (m_dataSourceViewHandler) {
            const QJsonObject terminal =
                m_dataSourceViewHandler(QStringLiteral("__quick_measurement_owner_destroyed"), {});
            if (!terminal.value(QStringLiteral("requestId")).toString().isEmpty())
                m_lastQuickMeasurementStatus = terminal;
        }
        m_dataSourceViewOwner.clear();
        m_dataSourceViewHandler = {};
    }
}

QJsonObject AgentController::executeDataSourceViewAction(const QString& toolName, const QJsonObject& params) const {
    if (QThread::currentThread() != thread())
        return {{QStringLiteral("error"), QStringLiteral("GUI display action must run on the owner thread")},
                {QStringLiteral("errorCode"), QStringLiteral("THREAD_AFFINITY")},
                {QStringLiteral("retryable"), false}};
    if (!m_dataSourceViewOwner || !m_dataSourceViewHandler) {
        if (toolName == QLatin1String("list_active_annotation_objects") ||
            toolName == QLatin1String("edit_active_annotation_masks") ||
            toolName == QLatin1String("list_active_annotation_categories") ||
            toolName == QLatin1String("edit_active_annotation_categories") ||
            toolName == QLatin1String("get_active_annotation_prompt_status") ||
            toolName == QLatin1String("cancel_active_annotation_prompt") ||
            toolName == QLatin1String("save_active_annotation_session") ||
            toolName == QLatin1String("open_active_annotation_session") ||
            toolName == QLatin1String("__sam_mask_edit_preflight") ||
            toolName == QLatin1String("__sam_prompt_cancel_confirmation_snapshot") ||
            toolName == QLatin1String("__sam_prompt_cancel_preflight") ||
            toolName.startsWith(QLatin1String("__sam_session_open_")))
            return {{QStringLiteral("error"), QStringLiteral("The SAM annotation dialog is not open")},
                    {QStringLiteral("errorCode"), QStringLiteral("SAM_DIALOG_NOT_OPEN")},
                    {QStringLiteral("retryable"), false}};
        if (toolName == QLatin1String("get_quick_measurement_status")) {
            const QString requestId = params.value(QStringLiteral("requestId")).toString();
            if (m_lastQuickMeasurementStatus.value(QStringLiteral("requestId")).toString() == requestId &&
                !requestId.isEmpty())
                return m_lastQuickMeasurementStatus;
            if (!m_lastQuickMeasurementStatus.isEmpty())
                return {{QStringLiteral("error"), QStringLiteral("Unknown or replaced quick measurement request")},
                        {QStringLiteral("errorCode"), QStringLiteral("QUICK_MEASURE_REQUEST_NOT_FOUND")},
                        {QStringLiteral("retryable"), false}};
        }
        return {{QStringLiteral("error"), QStringLiteral("Data source display requires an open GUI window")},
                {QStringLiteral("errorCode"), QStringLiteral("REQUIRES_GUI")},
                {QStringLiteral("retryable"), false}};
    }
    if (m_dataSourceViewOwner->thread() != QThread::currentThread())
        return {{QStringLiteral("error"), QStringLiteral("GUI display action must run on the owner thread")},
                {QStringLiteral("errorCode"), QStringLiteral("THREAD_AFFINITY")},
                {QStringLiteral("retryable"), false}};
    const QJsonObject result = m_dataSourceViewHandler(toolName, params);
    if (toolName == QLatin1String("begin_quick_measurement") &&
        result.value(QStringLiteral("status")) == QLatin1String("awaiting_user_input")) {
        m_lastQuickMeasurementStatus = result;
    } else if (toolName == QLatin1String("get_quick_measurement_status") ||
               toolName == QLatin1String("cancel_quick_measurement")) {
        const QString requestId = params.value(QStringLiteral("requestId")).toString();
        if (result.value(QStringLiteral("requestId")).toString() == requestId &&
            !result.value(QStringLiteral("errorCode")).toString().size())
            m_lastQuickMeasurementStatus = result;
        else if (result.value(QStringLiteral("errorCode")) == QLatin1String("QUICK_MEASURE_REQUEST_NOT_FOUND") &&
                 m_lastQuickMeasurementStatus.value(QStringLiteral("requestId")).toString() == requestId)
            return m_lastQuickMeasurementStatus;
    } else if (toolName == QLatin1String("__quick_measurement_owner_destroyed") &&
               !result.value(QStringLiteral("requestId")).toString().isEmpty()) {
        m_lastQuickMeasurementStatus = result;
    }
    return result;
}

QJsonObject AgentController::confirmedActiveAnnotationSessionSaveSnapshot(const QString& outputPath) const {
    if (state() != AgentState::Executing || outputPath.isEmpty())
        return {};
    const QString snapshotKey = QStringLiteral("sam-session-save-path:") + QDir::cleanPath(outputPath);
    const QJsonObject snapshot = m_pendingFileTargetPaths.value(snapshotKey).toObject();
    if (!snapshot.value(QStringLiteral("available")).toBool())
        return {};
    return snapshot;
}

QJsonObject AgentController::confirmedActiveAnnotationSessionOpenSnapshot(const QString& sessionPath) const {
    if (state() != AgentState::Executing || sessionPath.isEmpty())
        return {};
    const QString snapshotKey = QStringLiteral("sam-session-open-path:") + QDir::cleanPath(sessionPath);
    const QJsonObject snapshot = m_pendingFileTargetPaths.value(snapshotKey).toObject();
    if (!snapshot.value(QStringLiteral("available")).toBool())
        return {};
    return snapshot;
}

void AgentController::shutdown() {
    if (!m_initialized)
        return;
    if (m_llmClient && !m_activeLLMRequestId.isEmpty())
        m_llmClient->cancelRequest();
    m_activeLLMRequestId.clear();
    AgentBridge::instance().setToolCallCallback(nullptr);
    if (m_observer)
        m_observer->shutdown();
    if (m_llmClient)
        disconnect(m_llmClient, nullptr, this, nullptr);
    // 阶7 Agent 评审修复（P3-10）：清空客户端与待确认工具，避免 shutdown 后
    // sendUserMessage 仍向已断开客户端发请求或残留待确认状态
    m_llmClient = nullptr;
    m_pendingToolCalls = QJsonArray();
    m_pendingConfirmationId.clear();
    m_dispatchQueued = false;
    m_pendingFileTargetPaths = QJsonObject();
    m_pendingFileTargetsCaptured = false;
    cancelPendingQueries(QStringLiteral("controller_shutdown"));
    ++m_waitGeneration;
    m_initialized = false;
    AgentAuditLog::instance().shutdown();
    transitionTo(AgentState::Idle);
}

void AgentController::setPermissionLevel(PermissionLevel level) {
    if (m_permissionLevel == level)
        return;
    m_permissionLevel = level;
    emit permissionLevelChanged(level);
}

void AgentController::setLLMClient(ILLMClient* client) {
    if (m_llmClient && m_llmClient != client && !m_activeLLMRequestId.isEmpty())
        m_llmClient->cancelRequest();
    if (m_llmClient != client) {
        m_activeLLMRequestId.clear();
        if (state() == AgentState::Thinking)
            transitionTo(AgentState::Idle);
    }
    if (m_llmClient)
        disconnect(m_llmClient, nullptr, this, nullptr);
    m_llmClient = client;
    if (m_llmClient) {
        connect(m_llmClient, &ILLMClient::responseReceived, this, &AgentController::onLLMResponse,
                Qt::QueuedConnection);
        if (m_llmClient->supportsRequestCorrelation()) {
            connect(m_llmClient, &ILLMClient::requestFailed, this, &AgentController::onLLMRequestFailed,
                    Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::errorOccurredClassifiedForRequest, this,
                    &AgentController::onLLMErrorClassifiedForRequest, Qt::QueuedConnection);
            connect(
                m_llmClient, &ILLMClient::streamStartedForRequest, this,
                [this](const QString& id) {
                    if (id == m_activeLLMRequestId)
                        onStreamStarted();
                },
                Qt::QueuedConnection);
            connect(
                m_llmClient, &ILLMClient::streamContentDeltaForRequest, this,
                [this](const QString& id, const QString& value) {
                    if (id == m_activeLLMRequestId)
                        onStreamContentDelta(value);
                },
                Qt::QueuedConnection);
            connect(
                m_llmClient, &ILLMClient::streamReasoningDeltaForRequest, this,
                [this](const QString& id, const QString& value) {
                    if (id == m_activeLLMRequestId)
                        onStreamReasoningDelta(value);
                },
                Qt::QueuedConnection);
            connect(
                m_llmClient, &ILLMClient::streamFinishedForRequest, this,
                [this](const QString& id) {
                    if (id == m_activeLLMRequestId)
                        onStreamFinished();
                },
                Qt::QueuedConnection);
            connect(
                m_llmClient, &ILLMClient::streamFailedForRequest, this,
                [this](const QString& id) {
                    if (id == m_activeLLMRequestId)
                        onStreamFailed();
                },
                Qt::QueuedConnection);
            connect(
                m_llmClient, &ILLMClient::streamFallbackForRequest, this,
                [this](const QString& id, const QString& why) {
                    if (id == m_activeLLMRequestId)
                        onStreamFallback(why);
                },
                Qt::QueuedConnection);
        } else {
            connect(m_llmClient, &ILLMClient::errorOccurred, this, &AgentController::onLLMError, Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::errorOccurredClassified, this, &AgentController::onLLMErrorClassified,
                    Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::streamStarted, this, &AgentController::onStreamStarted,
                    Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::streamContentDelta, this, &AgentController::onStreamContentDelta,
                    Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::streamReasoningDelta, this, &AgentController::onStreamReasoningDelta,
                    Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::streamFinished, this, &AgentController::onStreamFinished,
                    Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::streamFailed, this, &AgentController::onStreamFailed,
                    Qt::QueuedConnection);
            connect(m_llmClient, &ILLMClient::streamFallback, this, &AgentController::onStreamFallback,
                    Qt::QueuedConnection);
        }
    }
}

void AgentController::setSystemPrompt(const QString& prompt) {
    m_systemPrompt = prompt.trimmed();
    if (m_systemPrompt.isEmpty())
        m_systemPrompt = defaultSystemPrompt();
}

void AgentController::logAction(const AgentActionLogEntry& entry) {
    emit actionLogEntryAdded(entry);
}

void AgentController::clearConversation() {
    if (!m_activeLLMRequestId.isEmpty() && m_llmClient)
        m_llmClient->cancelRequest();
    m_activeLLMRequestId.clear();
    cancelPendingQueries(QStringLiteral("conversation_cleared"));
    m_conversationHistory.clear();
    ++m_waitGeneration;
    m_pendingToolCalls = QJsonArray();
    m_dispatchQueued = false;
    m_pendingFileTargetPaths = QJsonObject();
    m_pendingFileTargetsCaptured = false;
    m_continueAfterPendingTools = true;
    // 阶段1 §4：清理对话即丢弃待确认批次（零副作用）,快照与确认 id 一并复位
    m_pendingConfirmationId.clear();
    m_pendingHasProject = false;
    m_pendingProjectPath.clear();
    m_pendingProjectName.clear();
    m_pendingProjectFingerprint.clear();
    m_pendingDataSourceEpochCaptured = false;
    m_pendingDataSourceProjectIdentity = 0;
    m_pendingDataSourceGeneration = 0;
    m_pendingDataSourceViewOwnerCaptured = false;
    m_pendingDataSourceViewOwner.clear();
    m_pendingDataSourceViewOwnerIdentity = 0;
    m_pendingUndoIndex = -1;
    m_pendingUndoCount = -1;
    m_pendingResumeSnapshotValid = false;
    m_pendingResumeRunId.clear();
    m_pendingResumeSessionId.clear();
    m_pendingResumeState = 0;
    m_pendingResumeLifecycleGeneration = 0;
    m_agentTurnCount = 0;
    m_parameterCorrectionRetries = 0;
    m_parameterFailureSignatures.clear();
    transitionTo(AgentState::Idle);
}

bool AgentController::undoLastAgentAction() {
    if (!m_actor || !m_actor->undoStack() || !m_actor->undoStack()->canUndo())
        return false;
    if (RunEngine::instance().isBusy())
        return false;
    if (m_actor->undoBlockedByBarrier()) {
        AgentActionLogEntry e;
        e.timestamp = QDateTime::currentDateTime();
        e.actor = "User";
        e.action = "undo";
        e.result = QStringLiteral("blocked_by_undo_barrier");
        e.undoable = false;
        emit actionLogEntryAdded(e);
        emit undoRejected(QStringLiteral("最近一次不可撤销操作之前的 Agent 编辑不能撤销"));
        return false;
    }
    const bool applied = m_actor->undoLastAction();
    if (!applied && RunEngine::instance().isBusy())
        return false;
    AgentActionLogEntry e;
    e.timestamp = QDateTime::currentDateTime();
    e.actor = "User";
    e.action = "undo";
    e.result = applied ? QStringLiteral("success") : QStringLiteral("expired");
    e.undoable = false;
    emit actionLogEntryAdded(e);
    return applied;
}

// ========== State Machine ==========

void AgentController::transitionTo(AgentState newState) {
    QMutexLocker locker(&m_stateMutex);
    if (m_state == newState)
        return;
    qDebug() << "[AgentState]" << stateName(m_state) << "->" << stateName(newState);
    m_state = newState;
}

bool AgentController::tryTransitionTo(AgentState expected, AgentState newState) {
    QMutexLocker locker(&m_stateMutex);
    if (m_state != expected)
        return false;
    if (m_state == newState)
        return true;
    qDebug() << "[AgentState]" << stateName(m_state) << "->" << stateName(newState);
    m_state = newState;
    return true;
}

QString AgentController::stateName(AgentState state) {
    switch (state) {
    case AgentState::Idle:
        return "Idle";
    case AgentState::Thinking:
        return "Thinking";
    case AgentState::Confirming:
        return "Confirming";
    case AgentState::Executing:
        return "Executing";
    }
    return "Unknown";
}

// ========== Context ==========

QString AgentController::buildContext() {
    QString ctx = m_systemPrompt;
    Project* proj = ProjectManager::instance().currentProject();
    if (proj) {
        ctx += QString("\n\n## Current State\n- Project: %1\n- Modules (%2): ")
                   .arg(proj->name())
                   .arg(proj->modules().size());
        const QList<ModuleInstance> modules = proj->modules();
        QStringList modNames;
        const int previewCount =
            modules.size() >= flowSummaryThreshold() ? qMin(flowSummaryPreviewCount(), modules.size()) : modules.size();
        for (int i = 0; i < previewCount; ++i) {
            const ModuleInstance& m = modules.at(i);
            modNames.append(QString("%1(id=%2)").arg(m.moduleId).arg(m.id));
        }
        ctx += modNames.join(", ");
        if (previewCount < modules.size())
            ctx += QString(" (+%1 more; call get_flow_state for the full list)").arg(modules.size() - previewCount);
        ctx += QString("\n- Connections: %1\n- RunEngine: %2\n")
                   .arg(proj->connections().size())
                   .arg(RunEngine::instance().isRunning() ? "Running" : "Idle");
    } else {
        ctx += "\n\n## Current State\n- No project opened.\n";
    }
    if (m_llmClient && !m_llmClient->supportsToolCalling()) {
        ctx += "\n## Tool Availability\nTool calling is disabled for this request. Do not claim to inspect or change "
               "the project, run a workflow, or access external devices. Answer from the supplied conversation and "
               "context only, and state clearly when the request requires enabling tools.\n";
    }
    QList<GuiEvent> recent = m_observer->recentEvents(5);
    if (!recent.isEmpty()) {
        ctx += "\n## Recent Events\n";
        for (const GuiEvent& e : recent) {
            ctx += QString("[%1] %2").arg(e.timestamp.toString("hh:mm:ss")).arg(e.typeString());
            if (e.type == GuiEventType::RunFinished && !e.details.value(QStringLiteral("success")).toBool()) {
                for (const QString& key :
                     {QStringLiteral("moduleId"), QStringLiteral("port"), QStringLiteral("errorCode"),
                      QStringLiteral("error"), QStringLiteral("diagnostics")}) {
                    const QString value = e.details.value(key).toVariant().toString().left(256);
                    if (!value.isEmpty())
                        ctx += QStringLiteral(" %1=%2").arg(key, value);
                }
            }
            ctx += QLatin1Char('\n');
        }
    }
    constexpr int kMaxContextChars = 16384;
    const QString truncationMarker = QStringLiteral("…[context truncated]");
    if (ctx.size() > kMaxContextChars) {
        ctx.truncate(kMaxContextChars - truncationMarker.size());
        ctx += truncationMarker;
    }
    return ctx;
}

// ========== User Input ==========

void AgentController::sendUserMessage(const QString& message) {
    if (!m_initialized) {
        emit llmErrorOccurred("Agent not initialized");
        return;
    }
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }
    if (!tryTransitionTo(AgentState::Idle, AgentState::Thinking)) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }
    AgentMessage m;
    m.role = "user";
    m.content = message;
    m_conversationHistory.append(m);
    trimHistoryIfNeeded();
    m_agentTurnCount = 0;
    m_parameterCorrectionRetries = 0;
    m_parameterFailureSignatures.clear();
    m_turnTimer.start();
    ++m_turnGeneration;
    m_visionRetryDone = false;
    m_suppressNextLlmError = false;
    m_lastErrorCategory.clear();
    sendPreparedContext();
}

void AgentController::sendUserMessageWithImages(const QString& message, const QList<QPixmap>& images) {
    // 批3 复核五轮（P2-4）：附件上限（与 AgentChatPanel::kMaxImageAttachments
    // 同一契约，测试断言两者相等）——参数校验先于状态/客户端校验，超限在
    // 任何控制器状态下都得到明确反馈；UI 侧另有源头限额
    if (images.size() > kMaxImageAttachments) {
        emit llmErrorOccurred(QString("Too many image attachments (maximum %1)").arg(kMaxImageAttachments));
        return;
    }
    if (!m_initialized) {
        emit llmErrorOccurred("Agent not initialized");
        return;
    }
    if (!m_llmClient) {
        emit llmErrorOccurred("LLM client not configured");
        return;
    }
    if (!m_llmClient->supportsImageInput()) {
        emit llmErrorOccurred("The configured model does not support image input");
        return;
    }
    if (images.size() > kMaxImageAttachments) {
        emit llmErrorOccurred(QString("Too many image attachments (maximum %1)").arg(kMaxImageAttachments));
        return;
    }
    if (!tryTransitionTo(AgentState::Idle, AgentState::Thinking)) {
        emit llmResponseReceived("Agent is busy processing a previous request. Please wait.", {});
        return;
    }

    AgentMessage msg;
    msg.role = "user";
    msg.content = message.isEmpty() ? QStringLiteral("Analyze the attached image.") : message;
    qint64 totalBytes = 0;
    for (const QPixmap& image : images) {
        if (image.isNull())
            continue;

        QByteArray data;
        QBuffer buffer(&data);
        if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG")) {
            totalBytes += data.size();
            if (totalBytes > kMaxImageAttachmentBytes) {
                transitionTo(AgentState::Idle);
                emit llmErrorOccurred("Image attachments exceed the 8 MB request limit");
                return;
            }
            msg.images.append({data, QStringLiteral("image/png"), QString()});
        }
    }
    if (msg.images.isEmpty()) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("No valid image attachment");
        return;
    }

    m_conversationHistory.append(msg);
    trimHistoryIfNeeded();
    m_agentTurnCount = 0;
    m_parameterCorrectionRetries = 0;
    m_parameterFailureSignatures.clear();
    m_turnTimer.start();
    ++m_turnGeneration;
    m_visionRetryDone = false;
    m_suppressNextLlmError = false;
    m_lastErrorCategory.clear();
    sendPreparedContext();
}

bool AgentController::cancelCurrentRequest() {
    const AgentState current = state();
    if (current == AgentState::Thinking && m_llmClient) {
        m_llmClient->cancelRequest();
        m_activeLLMRequestId.clear();
        ++m_waitGeneration;
        m_parameterCorrectionRetries = 0;
        m_parameterFailureSignatures.clear();
        transitionTo(AgentState::Idle);
        emit llmResponseReceived(QStringLiteral("已取消当前 Agent 请求。"), {});
        return true;
    }
    if (current == AgentState::Executing && !m_pendingQueries.isEmpty()) {
        ++m_waitGeneration;
        cancelPendingQueries(QStringLiteral("agent_request_cancelled"));
        m_activeLLMRequestId.clear();
        m_parameterCorrectionRetries = 0;
        m_parameterFailureSignatures.clear();
        transitionTo(AgentState::Idle);
        emit llmResponseReceived(QStringLiteral("已取消 Agent 的等待。已启动的工作流会继续运行。"), {});
        return true;
    }
    return false;
}

void AgentController::endTurnWithReason(const QString& responseText, const QString& errorText,
                                        bool suppressClientError) {
    if (state() == AgentState::Idle)
        return;
    const bool chatConfirmation = state() == AgentState::Confirming &&
                                  (m_dispatchQueued ? m_queuedContinueAfterPending : m_continueAfterPendingTools);
    if (chatConfirmation) {
        QJsonArray calls = m_pendingToolCalls;
        if (calls.isEmpty() && !m_conversationHistory.isEmpty() &&
            m_conversationHistory.constLast().role == QLatin1String("assistant"))
            calls = m_conversationHistory.constLast().toolCalls;
        appendToolResultsForCalls(calls, QJsonObject{{QStringLiteral("error"), QStringLiteral("User cancelled")},
                                                     {QStringLiteral("errorCode"), QStringLiteral("USER_CANCELLED")},
                                                     {QStringLiteral("retryable"), false}});
    }
    ++m_turnGeneration;
    ++m_waitGeneration;
    m_pendingToolCalls = QJsonArray();
    m_continueAfterPendingTools = true;
    m_dispatchQueued = false;
    m_activeLLMRequestId.clear();
    m_suppressNextLlmError = suppressClientError;
    cancelPendingQueries(QStringLiteral("turn_ended"));
    flushStreamPending();
    if (m_llmClient)
        m_llmClient->abortRequest();
    if (m_actor)
        m_actor->abortRunningTool();
    transitionTo(AgentState::Idle);
    if (m_streamActive) {
        m_streamActive = false;
        emit agentStreamAborted();
    }
    emit llmResponseReceived(responseText, {});
    if (!errorText.isEmpty()) {
        m_lastErrorCategory = llmErrorCategoryName(LLMErrorCategory::Timeout);
        emit llmErrorOccurred(errorText);
    }
    m_turnTimer.invalidate();
    emit agentTurnAborted();
}

int AgentController::turnDeadlineMs() const {
    int configured = ConfigManager::instance().isInitialized()
                         ? ConfigManager::instance().groupInt("agent", "turnDeadlineMs", 600000)
                         : 600000;
#ifdef DEEPLUX_BUILD_TESTS
    if (m_turnDeadlineConfigOverride >= 0)
        configured = m_turnDeadlineConfigOverride;
#endif
    return qBound(10000, configured, 3600000);
}

#ifdef DEEPLUX_BUILD_TESTS
void AgentController::setTurnDeadlineMsForTest(int ms) {
    m_turnDeadlineOverride = ms;
}

void AgentController::setTurnDeadlineConfigForTest(int ms) {
    m_turnDeadlineConfigOverride = ms;
}
#endif

bool AgentController::turnDeadlineExceeded() const {
    if (!m_turnTimer.isValid())
        return false;
    int deadline = turnDeadlineMs();
#ifdef DEEPLUX_BUILD_TESTS
    if (m_turnDeadlineOverride >= 0)
        deadline = m_turnDeadlineOverride;
#endif
    return deadline > 0 && m_turnTimer.elapsed() >= deadline;
}

// ========== LLM Response ==========

void AgentController::onLLMResponse(const AgentResponse& resp) {
    const bool correlationRequired = m_llmClient && m_llmClient->supportsRequestCorrelation();
    if ((correlationRequired && resp.requestId.isEmpty()) ||
        (!resp.requestId.isEmpty() && resp.requestId != m_activeLLMRequestId)) {
        qWarning() << "[AgentController] Ignoring stale or uncorrelated LLM response" << resp.requestId;
        return;
    }
    // 阶7 Agent 评审修复（P3-12）：统一经 state() 访问器读状态（带锁），
    // 避免未来跨线程引入后直读成员造成回归
    const AgentState currentState = state();
    if (currentState != AgentState::Thinking) {
        qWarning() << "[AgentController] Unexpected onLLMResponse in state" << stateName(currentState);
        return;
    }
    m_activeLLMRequestId.clear();

    const QJsonArray toolCalls = uniqueToolCallIds(resp.toolCalls);

    m_lastResponseReasoning = resp.reasoningContent;

    AgentMessage am;
    am.role = "assistant";
    am.content = resp.content;
    am.toolCalls = toolCalls;
    am.reasoningContent = resp.reasoningContent;
    m_conversationHistory.append(am);
    trimHistoryIfNeeded();

    // A length-truncated response may contain an incomplete tool call. Never execute
    // it; close the turn with an explicit audit record so the user can retry safely.
    if (resp.truncatedByMaxTokens) {
        AgentActionLogEntry entry;
        entry.timestamp = QDateTime::currentDateTime();
        entry.actor = QStringLiteral("Agent");
        entry.action = QStringLiteral("LLM response truncated by token limit");
        entry.result = QStringLiteral("truncated");
        emit actionLogEntryAdded(entry);
        emit agentStreamTruncated();
        if (!toolCalls.isEmpty()) {
            appendToolResultsForCalls(
                toolCalls, QJsonObject{{QStringLiteral("error"),
                                        QStringLiteral("LLM response was truncated; tool call was not executed")},
                                       {QStringLiteral("errorCode"), QStringLiteral("RESPONSE_TRUNCATED")},
                                       {QStringLiteral("retryable"), false}});
        }
        transitionTo(AgentState::Idle);
        emit llmResponseReceived(resp.content, {});
        return;
    }

    if (!toolCalls.isEmpty() && m_llmClient && !m_llmClient->supportsToolCalling()) {
        transitionTo(AgentState::Executing);
        for (const QJsonValue& value : toolCalls) {
            const QJsonObject call = value.toObject();
            const QString name = toolCallName(call);
            const QJsonObject params = toolCallParams(call);
            const QJsonObject result{
                {QStringLiteral("error"), QStringLiteral("Tool calling is disabled for the configured provider")},
                {QStringLiteral("errorCode"), QStringLiteral("TOOLS_DISABLED")},
                {QStringLiteral("retryable"), false}};

            AgentActionLogEntry entry;
            entry.timestamp = QDateTime::currentDateTime();
            entry.actor = QStringLiteral("Agent");
            entry.action = name;
            entry.params = compactAuditParams(name, params);
            entry.result = QStringLiteral("denied");
            entry.undoable = false;
            emit actionLogEntryAdded(entry);

            AgentMessage toolResult;
            toolResult.role = QStringLiteral("tool");
            toolResult.toolCallId = call.value(QStringLiteral("id")).toString();
            toolResult.content = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
            m_conversationHistory.append(toolResult);
        }
        trimHistoryIfNeeded();
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(
            QStringLiteral("Provider returned tool calls while tool calling is disabled; no tools were executed."));
        return;
    }

    if (!toolCalls.isEmpty()) {
        // 在确认前识别非法参数；整批交给统一失败回填，不能把坏 JSON 当作 {} 执行。
        for (const QJsonValue& value : toolCalls) {
            bool valid = false;
            toolCallParams(value.toObject(), &valid);
            if (!valid) {
                transitionTo(AgentState::Executing);
                extendAgentLoop(toolCalls);
                return;
            }
        }
        // 阶段1 §2（F08）：三入口共用 evaluateToolGate——Observer 的只读工具不再被
        // 整轮丢弃，而是正常执行并继续问答；写操作在 extendAgentLoop 内逐项拒绝
        // （PERMISSION_DENIED 结果进入对话与审计，保证每个 call 恰有一个结果）。
        // Advisor/Autopilot 的确认语义不变，但确认批次绑定工程与配置版本（§4）。
        bool requiresConfirmation = false;
        int selectorFailureIndex = -1;
        QJsonObject selectorFailure;
        // Selector schema errors take precedence over semantic unavailability across the batch.
        for (int i = 0; i < toolCalls.size(); ++i) {
            const QJsonObject call = toolCalls.at(i).toObject();
            const QString name = toolCallName(call);
            const QJsonObject strictParamError = preConfirmationParameterError(name, toolCallParams(call));
            if (!strictParamError.isEmpty()) {
                selectorFailureIndex = i;
                selectorFailure = strictParamError;
                break;
            }
            if (evaluateToolGate(name, toolCallParams(call)) == ToolGate::Deny)
                continue;
            const QJsonObject preflight = m_actor->flowSelectorPreflight(name, toolCallParams(call));
            if (preflight.value(QStringLiteral("errorCode")).toString() == QLatin1String("INVALID_PARAM")) {
                selectorFailureIndex = i;
                selectorFailure = preflight;
                break;
            }
        }
        if (selectorFailureIndex < 0) {
            for (int i = 0; i < toolCalls.size(); ++i) {
                const QJsonObject call = toolCalls.at(i).toObject();
                const QString name = toolCallName(call);
                if (evaluateToolGate(name, toolCallParams(call)) == ToolGate::Deny)
                    continue;
                QJsonObject preflight;
                if (name == QLatin1String("run_flow"))
                    preflight = cameraCycleBatchPreflight(toolCalls);
                if (preflight.isEmpty())
                    preflight = cameraCyclePreflight(name, toolCallParams(call));
                if (preflight.isEmpty())
                    preflight = cameraSessionPreflight(name);
                if (preflight.isEmpty())
                    preflight = m_actor->flowSelectorPreflight(name, toolCallParams(call));
                if (preflight.isEmpty())
                    preflight = unresolvedFileTargetPreflight(name);
                if (preflight.isEmpty() && name == QLatin1String("edit_active_annotation_masks"))
                    preflight =
                        executeDataSourceViewAction(QStringLiteral("__sam_mask_edit_preflight"), toolCallParams(call));
                if (preflight.isEmpty() && name == QLatin1String("edit_active_annotation_categories"))
                    preflight = executeDataSourceViewAction(QStringLiteral("__sam_category_edit_preflight"),
                                                            toolCallParams(call));
                if (preflight.isEmpty() && name == QLatin1String("cancel_active_annotation_prompt"))
                    preflight = executeDataSourceViewAction(QStringLiteral("__sam_prompt_cancel_preflight"),
                                                            toolCallParams(call));
                if (preflight.isEmpty() && name == QLatin1String("save_active_annotation_session"))
                    preflight = executeDataSourceViewAction(QStringLiteral("__sam_session_save_preflight"),
                                                            toolCallParams(call));
                if (preflight.isEmpty() && name == QLatin1String("open_active_annotation_session"))
                    preflight = executeDataSourceViewAction(QStringLiteral("__sam_session_open_preflight"),
                                                            toolCallParams(call));
                if (!preflight.isEmpty()) {
                    selectorFailureIndex = i;
                    selectorFailure = preflight;
                    break;
                }
            }
        }
        if (selectorFailureIndex >= 0) {
            transitionTo(AgentState::Executing);
            QList<ToolOutcome> outcomes;
            const QString failedCallId = toolCalls.at(selectorFailureIndex).toObject().value("id").toString();
            for (int i = 0; i < toolCalls.size(); ++i) {
                const QJsonObject call = toolCalls.at(i).toObject();
                const QString name = toolCallName(call);
                const QString callId = call.value("id").toString();
                const QJsonObject params = toolCallParams(call);
                const bool denied = evaluateToolGate(name, params) == ToolGate::Deny;
                QJsonObject result;
                if (denied) {
                    result = deniedToolResult(name, permissionLevelName(m_permissionLevel), m_actor, params);
                } else if (i == selectorFailureIndex) {
                    result = selectorFailure;
                } else {
                    result = QJsonObject{{"error", QStringLiteral("Batch rejected before execution")},
                                         {"errorCode", QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR")},
                                         {"retryable", false},
                                         {"details", QJsonObject{{"failedCallId", failedCallId}}}};
                }
                AgentMessage message;
                message.role = "tool";
                message.toolCallId = callId;
                message.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
                m_conversationHistory.append(message);
                AgentActionLogEntry entry;
                entry.timestamp = QDateTime::currentDateTime();
                entry.actor = QStringLiteral("Agent");
                entry.action = name;
                entry.params = compactAuditParams(name, params);
                const QString errorCode = result.value(QStringLiteral("errorCode")).toString();
                entry.result =
                    denied ? QStringLiteral("denied")
                           : (errorCode == QLatin1String("SKIPPED_DUE_TO_PREVIOUS_ERROR") ? QStringLiteral("skipped")
                                                                                          : QStringLiteral("error"));
                emit actionLogEntryAdded(entry);
                outcomes.append({callId, name, result, false, denied});
            }
            trimHistoryIfNeeded();
            transitionTo(AgentState::Idle);
            const QString failureReason =
                selectorFailure.value(QStringLiteral("errorCode")) == QLatin1String("INVALID_PARAM")
                    ? QStringLiteral("批次未执行（工具参数无效）")
                    : QStringLiteral("批次未执行（flow selector 不可用）");
            emit llmErrorOccurred(batchFailureSummary(failureReason, outcomes, false));
            return;
        }

        for (const QJsonValue& v : toolCalls) {
            if (evaluateToolGate(toolCallName(v.toObject()), toolCallParams(v.toObject())) ==
                ToolGate::RequireConfirmation) {
                requiresConfirmation = true;
                break;
            }
        }
        if (requiresConfirmation) {
            beginPendingConfirmation(toolCalls, resp.content, true);
            return;
        }
        // 允许项直接执行 + 闭环
        transitionTo(AgentState::Executing);
        extendAgentLoop(toolCalls);
        return;
    }

    // 纯文本回复 — 终态。涉及运行结果的结论必须带本轮真实工具依据；未确认状态由控制器给出。
    const QString finalContent = finalizeTextReply(resp.content);
    if (!m_conversationHistory.isEmpty() && m_conversationHistory.last().role == QLatin1String("assistant"))
        m_conversationHistory.last().content = finalContent;
    transitionTo(AgentState::Idle);
    emit llmResponseReceived(finalContent, {});
}

QString AgentController::finalizeTextReply(const QString& content) const {
    struct Evidence {
        QString callId;
        QString toolName;
        QString targetInstanceId;
        QJsonObject result;
    };
    const QSet<QString> relevantTools{QStringLiteral("run_flow"), QStringLiteral("wait_for_run"),
                                      QStringLiteral("get_run_results"), QStringLiteral("get_module_output")};
    int currentUserIndex = -1;
    for (int i = m_conversationHistory.size() - 1; i >= 0; --i) {
        if (m_conversationHistory.at(i).role == QLatin1String("user")) {
            currentUserIndex = i;
            break;
        }
    }
    if (currentUserIndex < 0)
        return content;

    QHash<QString, QString> toolNamesByCallId;
    QHash<QString, QString> targetInstancesByCallId;
    QStringList relevantCallIds;
    QList<Evidence> evidence;
    bool moduleOutputRequested = false;
    QString latestModuleOutputCallId;
    for (int i = currentUserIndex + 1; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role == QLatin1String("assistant")) {
            for (const QJsonValue& callValue : message.toolCalls) {
                const QJsonObject call = callValue.toObject();
                const QString name = toolCallName(call);
                const QString callId = call.value(QStringLiteral("id")).toString();
                if (!callId.isEmpty() && relevantTools.contains(name)) {
                    toolNamesByCallId.insert(callId, name);
                    relevantCallIds.append(callId);
                    if (name == QLatin1String("get_module_output")) {
                        const QJsonObject params = toolCallParams(call);
                        targetInstancesByCallId.insert(callId, params.value(QStringLiteral("instanceId")).toString());
                    }
                    if (name == QLatin1String("get_module_output")) {
                        moduleOutputRequested = true;
                        latestModuleOutputCallId = callId;
                    }
                }
            }
        } else if (message.role == QLatin1String("tool")) {
            const QString toolName = toolNamesByCallId.value(message.toolCallId);
            if (toolName.isEmpty())
                continue;
            evidence.append({message.toolCallId, toolName, targetInstancesByCallId.value(message.toolCallId),
                             QJsonDocument::fromJson(message.content.toUtf8()).object()});
        }
    }
    if (relevantCallIds.isEmpty())
        return content;
    QSet<QString> resultCallIds;
    for (const Evidence& item : evidence)
        resultCallIds.insert(item.callId);
    for (const QString& callId : relevantCallIds) {
        if (!resultCallIds.contains(callId)) {
            evidence.append({callId,
                             toolNamesByCallId.value(callId),
                             targetInstancesByCallId.value(callId),
                             {{QStringLiteral("status"), QStringLiteral("missing")},
                              {QStringLiteral("errorCode"), QStringLiteral("MISSING_TOOL_RESULT")}}});
        }
    }

    const Evidence& latest = evidence.last();
    const QString status = latest.result.value(QStringLiteral("status")).toString();
    const QSet<QString> unresolvedStatuses{
        QStringLiteral("started"),     QStringLiteral("pending"),   QStringLiteral("running"),
        QStringLiteral("paused"),      QStringLiteral("stopping"),  QStringLiteral("failed"),
        QStringLiteral("timeout"),     QStringLiteral("uncertain"), QStringLiteral("cancelled"),
        QStringLiteral("unavailable"), QStringLiteral("truncated"), QStringLiteral("idle"),
        QStringLiteral("missing")};
    const bool unresolved = latest.result.isEmpty() || latest.result.contains(QStringLiteral("error")) ||
                            latest.result.contains(QStringLiteral("errorCode")) ||
                            latest.result.value(QStringLiteral("stale")).toBool() ||
                            unresolvedStatuses.contains(status);
    bool hasMeasuredValue = false;
    for (const Evidence& item : evidence) {
        if (item.toolName == QLatin1String("get_module_output") && item.callId == latestModuleOutputCallId &&
            item.result.value(QStringLiteral("status")) == QLatin1String("available") &&
            item.result.contains(QStringLiteral("value")) &&
            (!item.result.value(QStringLiteral("key")).toString().isEmpty() ||
             !item.result.value(QStringLiteral("port")).toString().isEmpty())) {
            hasMeasuredValue = true;
        }
    }

    QStringList evidenceLines;
    const int firstEvidence = qMax(0, evidence.size() - 4);
    for (int i = firstEvidence; i < evidence.size(); ++i) {
        const Evidence& item = evidence.at(i);
        QJsonObject selected;
        for (const QString& key :
             {QStringLiteral("status"), QStringLiteral("runId"), QStringLiteral("sessionId"),
              QStringLiteral("instanceId"), QStringLiteral("key"), QStringLiteral("port"), QStringLiteral("value"),
              QStringLiteral("type"), QStringLiteral("source"), QStringLiteral("successRuns"),
              QStringLiteral("failedRuns"), QStringLiteral("sideEffectStatus"), QStringLiteral("error"),
              QStringLiteral("errorCode"), QStringLiteral("stale")}) {
            const QJsonValue value = item.result.value(key);
            if (value.isUndefined())
                continue;
            if (key == QLatin1String("value") &&
                QJsonDocument(QJsonObject{{QStringLiteral("value"), value}}).toJson(QJsonDocument::Compact).size() >
                    512) {
                selected.insert(key, QStringLiteral("<实际返回值过长，未展开>"));
            } else {
                selected.insert(key, value);
            }
        }
        if (!item.targetInstanceId.isEmpty() && !selected.contains(QStringLiteral("instanceId")))
            selected.insert(QStringLiteral("instanceId"), item.targetInstanceId);
        const QString json = QString::fromUtf8(QJsonDocument(selected).toJson(QJsonDocument::Compact)).left(700);
        evidenceLines.append(QStringLiteral("- %1（callId=%2）：%3").arg(item.toolName, item.callId, json));
    }
    const QString evidenceText = QStringLiteral("工具依据：\n%1").arg(evidenceLines.join(QLatin1Char('\n')));

    if (unresolved) {
        return QStringLiteral("本轮工具结果未能确认成功，不能报告为成功。\n%1").arg(evidenceText);
    }
    if ((moduleOutputRequested && !hasMeasuredValue) ||
        (!moduleOutputRequested && evidence.last().toolName != QLatin1String("get_module_output"))) {
        return QStringLiteral("工具返回了运行状态，但本轮没有取得可引用的模块测量值；不能据此断言测量成功。\n%1")
            .arg(evidenceText);
    }

    const QString reply = content.trimmed();
    return (reply.isEmpty() ? QStringLiteral("本轮实际测量结果如下。") : reply) + QStringLiteral("\n\n") + evidenceText;
}

// ========== Tool Confirm (通过 QueuedConnection 入队，避免阻塞按钮信号链) ==========

bool AgentController::confirmPendingTools(const QString& confirmationId) {
    const AgentState currentState = state();
    if (currentState != AgentState::Confirming) {
        qWarning() << "[AgentController] confirmPendingTools called in state" << stateName(currentState);
        return false;
    }
    // 阶段1 审核整改 R2 + 复审2 R1：批准必须绑定到用户所看到的批次——
    // 空 id 与错配（旧批次迟到事件）一律拒绝,零副作用,当前批次保持待确认
    if (confirmationId.isEmpty() || confirmationId != m_pendingConfirmationId) {
        Logger::instance().warning(QStringLiteral("[AgentController] 确认被拒绝（id 为空或错配）: 提交=%1 当前=%2")
                                       .arg(confirmationId, m_pendingConfirmationId),
                                   QStringLiteral("Agent"));
        return false;
    }
    if (m_pendingToolCalls.isEmpty()) {
        transitionTo(AgentState::Idle);
        return false;
    }
    if (m_dispatchQueued)
        return false;

    // 执行放到事件队列中，避免确认按钮槽同步执行插件或阻塞 GUI；排队窗口内
    // 仍保留批次快照，以便撤销、工程切换和权限变更先行取消/拒绝该确认。
    m_dispatchQueued = true;
    m_queuedContinueAfterPending = m_continueAfterPendingTools;
    QMetaObject::invokeMethod(
        this,
        [this, confirmationId]() {
            if (!m_dispatchQueued || state() != AgentState::Confirming || confirmationId != m_pendingConfirmationId)
                return;

            const QJsonArray calls = m_pendingToolCalls;
            const bool continueAfter = m_queuedContinueAfterPending;
            m_dispatchQueued = false;
            m_pendingToolCalls = QJsonArray();
            m_pendingConfirmationId.clear();
            m_continueAfterPendingTools = continueAfter;
            doConfirmPendingTools(calls, confirmationId);
            m_pendingFileTargetPaths = QJsonObject();
            m_pendingFileTargetsCaptured = false;
            m_continueAfterPendingTools = true;
        },
        Qt::QueuedConnection);
    return true;
}

void AgentController::doConfirmPendingTools(QJsonArray calls, const QString& confirmationId) {
    const AgentState currentState = state();
    if (currentState != AgentState::Confirming) {
        qWarning() << "[AgentController] doConfirmPendingTools: state changed to" << stateName(currentState);
        return;
    }

    auto auditBatch = [this](const QJsonArray& batch, const QString& outcome) {
        for (const QJsonValue& v : batch) {
            const QJsonObject tc = v.toObject();
            AgentActionLogEntry e;
            e.timestamp = QDateTime::currentDateTime();
            e.actor = "Agent";
            e.action = toolCallName(tc);
            e.params = compactAuditParams(e.action, toolCallParams(tc));
            e.result = outcome;
            e.undoable = false;
            emit actionLogEntryAdded(e);
        }
    };
    auto clearResumeSnapshot = [this]() {
        if (m_actor)
            m_actor->clearConfirmedResumeSnapshot();
        m_pendingResumeSnapshotValid = false;
        m_pendingResumeRunId.clear();
        m_pendingResumeSessionId.clear();
        m_pendingResumeState = 0;
        m_pendingResumeLifecycleGeneration = 0;
    };

    // 阶段1 §4：确认绑定工程与配置版本——窗口期内目标变化则整批拒绝，零副作用，
    // 不凭陈旧确认执行
    QString staleReason;
    if (!confirmationContextStillValid(calls, &staleReason)) {
        m_pendingToolCalls = QJsonArray();
        m_pendingFileTargetPaths = QJsonObject();
        m_pendingFileTargetsCaptured = false;
        clearResumeSnapshot();
        auditBatch(calls, QStringLiteral("expired"));
        // 仅聊天路径的历史中存在 assistant tool_calls；Bridge 路径无此消息，
        // 补 tool 结果反而会制造孤立 tool 消息
        if (m_continueAfterPendingTools) {
            appendToolResultsForCalls(
                calls, QJsonObject{{"status", QStringLiteral("rejected")},
                                   {"errorCode", QStringLiteral("CONFIRMATION_EXPIRED")},
                                   {"error", QStringLiteral("Confirmation expired: %1").arg(staleReason)},
                                   {"retryable", false}});
        }
        transitionTo(AgentState::Idle);
        emit llmResponseReceived(QStringLiteral("确认已失效（%1），未执行任何操作").arg(staleReason), {});
        return;
    }
    // 阶段1 §2：执行前复验权限——确认窗口内权限降级不产生未批准操作
    QString deniedToolName;
    QJsonObject deniedToolParams;
    for (const QJsonValue& v : calls) {
        if (evaluateToolGate(toolCallName(v.toObject()), toolCallParams(v.toObject())) == ToolGate::Deny) {
            deniedToolName = toolCallName(v.toObject());
            deniedToolParams = toolCallParams(v.toObject());
            break;
        }
    }
    if (!deniedToolName.isEmpty()) {
        m_pendingToolCalls = QJsonArray();
        m_pendingFileTargetPaths = QJsonObject();
        m_pendingFileTargetsCaptured = false;
        clearResumeSnapshot();
        auditBatch(calls, QStringLiteral("denied"));
        if (m_continueAfterPendingTools) {
            const QJsonObject userPermissionError =
                m_actor ? m_actor->userPermissionPreflight(deniedToolName, deniedToolParams) : QJsonObject();
            QJsonObject details = userPermissionError.value(QStringLiteral("details")).toObject();
            appendToolResultsForCalls(
                calls, QJsonObject{{"status", QStringLiteral("rejected")},
                                   {"errorCode", QStringLiteral("PERMISSION_DENIED")},
                                   {"error", QStringLiteral("User or Agent permission changed before execution")},
                                   {"retryable", false},
                                   {"details", details}});
        }
        transitionTo(AgentState::Idle);
        emit llmResponseReceived(QStringLiteral("用户或 Agent 权限已变化，确认批次被拒绝，未执行任何操作"), {});
        return;
    }

    transitionTo(AgentState::Executing);
    if (m_pendingResumeSnapshotValid)
        m_actor->bindConfirmedResumeSnapshot(m_pendingResumeRunId, m_pendingResumeSessionId, m_pendingResumeState,
                                             m_pendingResumeLifecycleGeneration);
    if (m_continueAfterPendingTools) {
        extendAgentLoop(calls, confirmationId);
        clearResumeSnapshot();
        return;
    }

    // Bridge/手动确认路径：携带原始 callId 执行，逐 call 恰有一条审计
    QList<AgentActor::ToolInvocation> tools;
    for (int callIndex = 0; callIndex < calls.size(); ++callIndex) {
        QJsonObject tc = calls.at(callIndex).toObject();
        const QString name = toolCallName(tc);
        const QString callId = tc.value(QStringLiteral("id")).toString();
        if (!name.isEmpty()) {
            const QJsonObject cameraSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("camera-call:") + callId).toObject();
            const QJsonObject variableSnapshot =
                m_pendingFileTargetPaths.value(variableSnapshotKey(callIndex)).toObject();
            const QJsonObject importSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("data-source-import:%1").arg(callIndex)).toObject();
            const QJsonObject annotationExportSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("annotation-export:") + callId).toObject();
            const QJsonObject annotationTransformSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("annotation-transform:") + callId).toObject();
            tools.append({callId, name, toolCallParams(tc), cameraSnapshot,
                          executionSideEffectAuthorization(tc, confirmationId, m_pendingFileTargetPaths),
                          variableSnapshot, importSnapshot, annotationExportSnapshot, annotationTransformSnapshot});
        }
    }
    QJsonObject batchResult = m_actor->executeTools(tools, QString("Confirmed Agent tools"));
    clearResumeSnapshot();
    QJsonArray resultsArray = batchResult["results"].toArray();
    for (int i = 0; i < resultsArray.size() && i < tools.size(); ++i) {
        QJsonObject result = resultsArray[i].toObject()["result"].toObject();
        AgentActionLogEntry e;
        e.timestamp = QDateTime::currentDateTime();
        e.actor = "Agent";
        e.action = tools[i].tool;
        e.params = compactAuditResultParams(e.action, tools[i].params, result);
        e.result = confirmedToolAuditOutcome(result);
        // 阶段1 §6：undoable = 本次成功且确属可撤销的工程编辑命令
        e.undoable = isUndoableChange(tools[i].tool, result);
        emit actionLogEntryAdded(e);
    }
    transitionTo(AgentState::Idle);
}

bool AgentController::rejectPendingTools(const QString& confirmationId) {
    const AgentState currentState = state();
    if (currentState != AgentState::Confirming) {
        qWarning() << "[AgentController] rejectPendingTools called in state" << stateName(currentState);
        return false;
    }
    // 阶段1 审核整改 R2 + 复审2 R1：与 confirm 同口径——空 id 或错配一律忽略
    if (confirmationId.isEmpty() || confirmationId != m_pendingConfirmationId) {
        Logger::instance().warning(QStringLiteral("[AgentController] 拒绝请求被忽略（id 为空或错配）: 提交=%1 当前=%2")
                                       .arg(confirmationId, m_pendingConfirmationId),
                                   QStringLiteral("Agent"));
        return false;
    }
    // 阶段1 §4：拒绝零副作用且可追踪——每个待确认 call 记一条 rejected 审计
    const QJsonArray calls = m_pendingToolCalls;
    for (const QJsonValue& v : calls) {
        const QJsonObject tc = v.toObject();
        AgentActionLogEntry e;
        e.timestamp = QDateTime::currentDateTime();
        e.actor = "Agent";
        e.action = toolCallName(tc);
        e.params = compactAuditParams(e.action, toolCallParams(tc));
        e.result = QStringLiteral("rejected");
        e.undoable = false;
        emit actionLogEntryAdded(e);
    }
    // assistant 的 tool_calls 已在聊天历史中；拒绝也必须逐 call 回一条 tool 结果，
    // 否则下一次请求会触发 OpenAI 协议错误（insufficient tool messages）。
    // Bridge 路径没有 assistant tool_calls，不能补 tool 消息。
    const bool fromChat = m_continueAfterPendingTools;
    m_pendingToolCalls = QJsonArray();
    m_pendingConfirmationId.clear();
    m_dispatchQueued = false;
    m_pendingFileTargetPaths = QJsonObject();
    m_pendingFileTargetsCaptured = false;
    if (m_actor)
        m_actor->clearConfirmedResumeSnapshot();
    m_pendingResumeSnapshotValid = false;
    m_pendingResumeRunId.clear();
    m_pendingResumeSessionId.clear();
    m_pendingResumeState = 0;
    m_pendingResumeLifecycleGeneration = 0;
    m_continueAfterPendingTools = true;
    if (fromChat) {
        appendToolResultsForCalls(calls, QJsonObject{{"status", QStringLiteral("rejected")},
                                                     {"errorCode", QStringLiteral("USER_REJECTED")},
                                                     {"error", QStringLiteral("User rejected the tool call")},
                                                     {"retryable", false}});
    }
    transitionTo(AgentState::Idle);
    emit llmResponseReceived("Tool execution cancelled by user.", {});
    return true;
}

// ========== Agent Loop Core ==========

void AgentController::extendAgentLoop(const QJsonArray& toolCalls, const QString& confirmationId) {
    const AgentState currentState = state();
    if (currentState != AgentState::Executing) {
        qWarning() << "[AgentController] Unexpected extendAgentLoop in state" << stateName(currentState);
        return;
    }
    // 中止代号：工具执行（可能含嵌套运行等待）返回后据此判定本轮是否已被用户中止
    const quint64 turn = m_turnGeneration;
    // 阶段1 审核整改 R3：超限路径也必须保证"每个原始 call ID 恰有一个结果"——
    // assistant 消息（含 tool_calls）已入历史,若不逐 call 补结构化失败结果,
    // 对话将留下孤立 tool_calls（违反 OpenAI 协议与计划 §12.5 对话合法性）
    auto appendLimitResults = [this](const QJsonArray& calls, const QString& errorCode, const QString& message) {
        for (const QJsonValue& v : calls) {
            const QJsonObject tc = v.toObject();
            const QJsonObject result{{"error", message}, {"errorCode", errorCode}, {"retryable", false}};
            AgentActionLogEntry e;
            e.timestamp = QDateTime::currentDateTime();
            e.actor = "Agent";
            e.action = toolCallName(tc);
            e.params = compactAuditParams(e.action, toolCallParams(tc));
            e.result = QStringLiteral("skipped");
            e.undoable = false;
            emit actionLogEntryAdded(e);

            const QString callId = tc.value(QStringLiteral("id")).toString();
            if (callId.isEmpty())
                continue; // 无 id 无法回 tool 消息（协议垃圾,审计已留痕）
            AgentMessage tm;
            tm.role = "tool";
            tm.toolCallId = callId;
            tm.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
            m_conversationHistory.append(tm);
        }
        trimHistoryIfNeeded();
    };
    // L9 检查点 1：超时批次不执行，并为原始调用补齐协议结果。
    if (turnDeadlineExceeded()) {
#ifdef DEEPLUX_BUILD_TESTS
        const int deadline = m_turnDeadlineOverride >= 0 ? m_turnDeadlineOverride : turnDeadlineMs();
#else
        const int deadline = turnDeadlineMs();
#endif
        const QString message = QStringLiteral("已超过单轮总时长上限（%1 ms），本轮已中止").arg(deadline);
        appendLimitResults(toolCalls, QStringLiteral("TURN_DEADLINE_EXCEEDED"), message);
        endTurnWithReason(message, message, false);
        return;
    }
    if (toolCalls.size() > kMaxToolCallsPerTurn) {
        appendLimitResults(toolCalls, QStringLiteral("BATCH_LIMIT_EXCEEDED"),
                           QStringLiteral("已达到单轮工具数量上限（%1），本批次未执行").arg(kMaxToolCallsPerTurn));
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("已达到单轮工具数量上限");
        return;
    }
    if (m_agentTurnCount >= kMaxAgentToolTurns) {
        appendLimitResults(toolCalls, QStringLiteral("TURN_LIMIT_EXCEEDED"),
                           QStringLiteral("已达到自动执行轮数上限（%1），本批次未执行").arg(kMaxAgentToolTurns));
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("已达到自动执行轮数上限");
        return;
    }

    int invalidIndex = -1;
    for (int i = 0; i < toolCalls.size(); ++i) {
        const QJsonObject call = toolCalls.at(i).toObject();
        const QString name = toolCallName(call);
        // Observer 的写操作优先返回 PERMISSION_DENIED，即使模型给了非法参数。
        if (evaluateToolGate(name, toolCallParams(call)) == ToolGate::Deny)
            continue;
        bool valid = false;
        toolCallParams(call, &valid);
        if (!valid) {
            invalidIndex = i;
            break;
        }
    }
    if (invalidIndex >= 0) {
        QList<ToolOutcome> outcomes;
        outcomes.reserve(toolCalls.size());
        for (int i = 0; i < toolCalls.size(); ++i) {
            const QJsonObject call = toolCalls.at(i).toObject();
            const QString name = toolCallName(call);
            const bool denied = evaluateToolGate(name, toolCallParams(call)) == ToolGate::Deny;
            const bool malformed = i == invalidIndex;
            const QJsonObject result =
                denied ? deniedToolResult(name, permissionLevelName(m_permissionLevel), m_actor, toolCallParams(call))
                       : QJsonObject{{"error", malformed ? QStringLiteral("Malformed tool arguments")
                                                         : QStringLiteral("Batch skipped due to invalid arguments")},
                                     {"errorCode", malformed ? QStringLiteral("INVALID_PARAM")
                                                             : QStringLiteral("SKIPPED_DUE_TO_PREVIOUS_ERROR")},
                                     {"retryable", false}};
            outcomes.append({call.value(QStringLiteral("id")).toString(), name, result, false, denied});
            AgentActionLogEntry entry;
            entry.timestamp = QDateTime::currentDateTime();
            entry.actor = "Agent";
            entry.action = name;
            entry.params = compactAuditParams(name, toolCallParams(call));
            entry.result =
                denied ? QStringLiteral("denied") : (malformed ? QStringLiteral("error") : QStringLiteral("skipped"));
            entry.undoable = false;
            emit actionLogEntryAdded(entry);

            const QString callId = call.value(QStringLiteral("id")).toString();
            if (!callId.isEmpty()) {
                AgentMessage message;
                message.role = "tool";
                message.toolCallId = callId;
                message.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
                m_conversationHistory.append(message);
            }
        }
        trimHistoryIfNeeded();
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(batchFailureSummary(QStringLiteral("批次未执行（工具参数格式无效）"), outcomes, false));
        return;
    }
    ++m_agentTurnCount;

    // 解析 tool_calls（阶段1 §7：保留原始 callId；§2：逐项权限门禁——被拒绝的
    // 调用不进入 Actor（零副作用），但同样获得结构化结果，保证对话合法性：
    // 每个原始 tool call 恰有一条 tool 消息与一条审计）
    struct ParsedCall {
        QString id;
        QString name;
        QJsonObject params;
        int callIndex = -1;
        bool denied = false;
    };
    QList<ParsedCall> parsed;
    QList<AgentActor::ToolInvocation> allowed;
    QList<int> allowedPositions;
    for (int callIndex = 0; callIndex < toolCalls.size(); ++callIndex) {
        const QJsonValue& v = toolCalls.at(callIndex);
        QJsonObject tc = v.toObject();
        ParsedCall pc;
        pc.id = tc["id"].toString();
        pc.name = toolCallName(tc);
        pc.params = toolCallParams(tc);
        pc.callIndex = callIndex;
        if (pc.name.isEmpty()) {
            if (pc.id.isEmpty())
                continue;     // 无名无 id 的协议垃圾：无法路由也无法回消息
            pc.denied = true; // 有名缺失但有 id → UNKNOWN_TOOL 结果回给模型
        } else if (evaluateToolGate(pc.name, pc.params) == ToolGate::Deny) {
            pc.denied = true;
        } else {
            const QJsonObject cameraSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("camera-call:") + pc.id).toObject();
            const QJsonObject variableSnapshot =
                m_pendingFileTargetPaths.value(variableSnapshotKey(pc.callIndex)).toObject();
            const QJsonObject importSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("data-source-import:%1").arg(pc.callIndex)).toObject();
            const QJsonObject annotationExportSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("annotation-export:") + pc.id).toObject();
            const QJsonObject annotationTransformSnapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("annotation-transform:") + pc.id).toObject();
            allowed.append({pc.id, pc.name, pc.params, cameraSnapshot,
                            executionSideEffectAuthorization(v.toObject(), confirmationId, m_pendingFileTargetPaths),
                            variableSnapshot, importSnapshot, annotationExportSnapshot, annotationTransformSnapshot});
            allowedPositions.append(parsed.size());
        }
        parsed.append(pc);
    }
    if (parsed.isEmpty()) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("No valid tool calls to execute");
        return;
    }

    // 批量执行允许项（callId 贯通；遇错停止后未执行项由 Actor 补 SKIPPED 结果）
    QJsonObject batchResult;
    if (!allowed.isEmpty())
        batchResult = m_actor->executeTools(allowed, QString("Agent turn %1").arg(m_agentTurnCount));

    // 按原始顺序归并结果：执行项取 Actor 结果,被拒项取 PERMISSION_DENIED/UNKNOWN_TOOL
    QJsonArray resultsArray = batchResult["results"].toArray();
    QMap<int, QJsonObject> resultByPosition;
    QMap<int, bool> executedByPosition;
    for (int i = 0; i < resultsArray.size() && i < allowedPositions.size(); ++i) {
        resultByPosition[allowedPositions[i]] = resultsArray[i].toObject()["result"].toObject();
        executedByPosition[allowedPositions[i]] = resultsArray[i].toObject().value(QStringLiteral("executed")).toBool();
    }

    QList<ToolOutcome> outcomes;
    outcomes.reserve(parsed.size());
    for (int i = 0; i < parsed.size(); ++i) {
        QJsonObject result = parsed[i].denied ? deniedToolResult(parsed[i].name, permissionLevelName(m_permissionLevel),
                                                                 m_actor, parsed[i].params)
                                              : resultByPosition.value(i);
        const int durationMs = takeElapsedMs(result);
        outcomes.append({parsed[i].id, parsed[i].name, result, executedByPosition.value(i, false), parsed[i].denied});
        AgentActionLogEntry e;
        e.timestamp = QDateTime::currentDateTime();
        e.actor = "Agent";
        e.action = parsed[i].name;
        e.params = compactAuditResultParams(e.action, parsed[i].params, result);
        e.result = parsed[i].denied ? QStringLiteral("denied") : confirmedToolAuditOutcome(result);
        e.durationMs = durationMs;
        // 阶段1 §6：undoable = 本次成功且确属可撤销的工程编辑命令
        e.undoable = !parsed[i].denied && isUndoableChange(parsed[i].name, result);
        emit actionLogEntryAdded(e);

        AgentMessage tm;
        tm.role = "tool";
        tm.toolCallId = parsed[i].id;
        tm.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
        m_conversationHistory.append(tm);
        if (!parsed[i].denied && result.value(QStringLiteral("status")) == QLatin1String("pending") &&
            (parsed[i].name == QLatin1String("wait_for_run") || parsed[i].name == QLatin1String("get_module_output"))) {
            QJsonObject boundParams = parsed[i].params;
            boundParams["runId"] = result.value(QStringLiteral("runId"));
            m_pendingQueries.append({parsed[i].id, parsed[i].name, boundParams});
        }
    }
    // 中止后仍保留审计与 tool 消息（保持 assistant.tool_calls 配对），但不续跑 LLM
    if (turn != m_turnGeneration || state() != AgentState::Executing) {
        qWarning() << "[AgentController] tool batch finished after abort; dropping continuation";
        return;
    }
    attachToolImages(resultsArray);
    // L9 检查点 2：等待类工具可能跨过 deadline
    if (turnDeadlineExceeded()) {
#ifdef DEEPLUX_BUILD_TESTS
        const int deadline = m_turnDeadlineOverride >= 0 ? m_turnDeadlineOverride : turnDeadlineMs();
#else
        const int deadline = turnDeadlineMs();
#endif
        const QString message = QStringLiteral("已超过单轮总时长上限（%1 ms），本轮已中止").arg(deadline);
        endTurnWithReason(message, message, false);
        return;
    }
    trimHistoryIfNeeded();

    // 已经执行过任一工具的批次不自动重试。只有全批预校验零执行、明确可修正的参数错误
    // 才回传给模型；相同工具/错误码/有效参数重复两次或达到纠错上限时终止。
    if (batchResult["status"].toString() == "partial_failed") {
        if (batchResult.value(QStringLiteral("errorCode")).toString() == QLatin1String("PROJECT_SWITCHED")) {
            m_parameterFailureSignatures.clear();
            continueAgentLoop();
            return;
        }
        cancelPendingQueries(QStringLiteral("batch_failed"));
        bool attemptedAny = false;
        bool parameterCorrectionEligible =
            batchResult.value(QStringLiteral("executed")).toInt() == 0 && !outcomes.isEmpty();
        bool repeatedParameterFailure = false;
        bool hasParameterFailure = false;
        QSet<QString> currentFailureSignatures;
        for (const ToolOutcome& outcome : outcomes) {
            attemptedAny |= outcome.executed;
            if (outcome.executed || outcome.denied) {
                parameterCorrectionEligible = false;
                continue;
            }
            const QString errorCode = outcome.result.value(QStringLiteral("errorCode")).toString();
            if (errorCode == QLatin1String("SKIPPED_DUE_TO_PREVIOUS_ERROR"))
                continue;
            if ((errorCode != QLatin1String("INVALID_PARAM") && errorCode != QLatin1String("MISSING_PARAM") &&
                 errorCode != QLatin1String("UNKNOWN_TOOL")) ||
                !outcome.result.value(QStringLiteral("retryable")).toBool()) {
                parameterCorrectionEligible = false;
                continue;
            }

            hasParameterFailure = true;
            const auto parsedCall = std::find_if(parsed.cbegin(), parsed.cend(), [&outcome](const ParsedCall& call) {
                return call.id == outcome.callId;
            });
            if (parsedCall == parsed.cend()) {
                parameterCorrectionEligible = false;
                continue;
            }
            const QString signature =
                outcome.toolName + QLatin1Char('|') + errorCode + QLatin1Char('|') +
                QString::fromUtf8(QJsonDocument(parsedCall->params).toJson(QJsonDocument::Compact));
            repeatedParameterFailure |= m_parameterFailureSignatures.contains(signature);
            currentFailureSignatures.insert(signature);
        }

        if (parameterCorrectionEligible && hasParameterFailure && !repeatedParameterFailure &&
            m_parameterCorrectionRetries < kMaxParameterCorrectionRetries) {
            // Compare only with the immediately previous correction batch. A→B→A
            // is still progress between attempts; only consecutive repeats stop.
            m_parameterFailureSignatures = currentFailureSignatures;
            ++m_parameterCorrectionRetries;
            continueAgentLoop();
            return;
        }

        QString heading =
            attemptedAny ? QStringLiteral("批次部分失败") : QStringLiteral("批次未执行（参数校验未通过）");
        if (parameterCorrectionEligible && hasParameterFailure) {
            heading =
                repeatedParameterFailure
                    ? QStringLiteral("连续两次相同参数错误，已停止自动纠错")
                    : QStringLiteral("参数纠错次数达到上限（%1），已停止自动纠错").arg(kMaxParameterCorrectionRetries);
        }
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(batchFailureSummary(heading, outcomes, attemptedAny));
        return;
    }

    Logger::instance().addLog(QString("[AgentLoop] Executed %1 tool(s)").arg(resultsArray.size()), LogLevel::Debug,
                              "Agent");

    // A successful batch breaks the sequence of consecutive validation failures.
    m_parameterFailureSignatures.clear();
    if (!m_pendingQueries.isEmpty()) {
        m_pendingQueryDeadline = QDateTime::currentDateTimeUtc().addSecs(30);
        const int generation = ++m_waitGeneration;
        QTimer::singleShot(50, this, [this, generation]() { pollPendingQueries(generation); });
        return;
    }
    continueAgentLoop();
}

void AgentController::cancelPendingQueries(const QString& reason) {
    for (const PendingQuery& query : m_pendingQueries) {
        const QJsonObject result{{"status", QStringLiteral("cancelled")},
                                 {"runId", query.params.value(QStringLiteral("runId"))},
                                 {"reason", reason},
                                 {"retryable", false}};
        for (int i = m_conversationHistory.size() - 1; i >= 0; --i) {
            AgentMessage& message = m_conversationHistory[i];
            if (message.role == QLatin1String("tool") && message.toolCallId == query.callId) {
                message.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
                break;
            }
        }
        AgentActionLogEntry audit;
        audit.timestamp = QDateTime::currentDateTime();
        audit.actor = QStringLiteral("Agent");
        audit.action = query.name;
        audit.params = compactAuditParams(audit.action, query.params);
        audit.result = QStringLiteral("cancelled");
        emit actionLogEntryAdded(audit);
    }
    m_pendingQueries.clear();
}

void AgentController::pollPendingQueries(int generation) {
    if (generation != m_waitGeneration || state() != AgentState::Executing)
        return;
    const bool expired = QDateTime::currentDateTimeUtc() >= m_pendingQueryDeadline;
    for (int i = m_pendingQueries.size() - 1; i >= 0; --i) {
        const PendingQuery query = m_pendingQueries.at(i);
        if (!expired &&
            !RunEngine::instance().runObservation(query.params.value(QStringLiteral("runId")).toString()).finished)
            continue;
        QJsonObject result = m_actor->executeTool(query.name, query.params);
        if (!expired && result.value(QStringLiteral("status")) == QLatin1String("pending"))
            continue;
        if (expired && result.value(QStringLiteral("status")) == QLatin1String("pending"))
            result = QJsonObject{{"status", QStringLiteral("timeout")},
                                 {"runId", result.value(QStringLiteral("runId"))},
                                 {"retryable", true}};
        for (int j = m_conversationHistory.size() - 1; j >= 0; --j) {
            AgentMessage& message = m_conversationHistory[j];
            if (message.role == QLatin1String("tool") && message.toolCallId == query.callId) {
                message.content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
                break;
            }
        }
        AgentActionLogEntry audit;
        audit.timestamp = QDateTime::currentDateTime();
        audit.actor = QStringLiteral("Agent");
        audit.action = query.name;
        audit.params = compactAuditParams(audit.action, query.params);
        const QString status = result.value(QStringLiteral("status")).toString();
        audit.result = result.contains(QStringLiteral("error"))
                           ? QStringLiteral("error")
                           : ((status == QLatin1String("timeout") || status == QLatin1String("failed") ||
                               status == QLatin1String("uncertain") || status == QLatin1String("cancelled") ||
                               status == QLatin1String("unavailable"))
                                  ? status
                                  : QStringLiteral("success"));
        emit actionLogEntryAdded(audit);
        m_pendingQueries.removeAt(i);
    }
    if (!m_pendingQueries.isEmpty()) {
        QTimer::singleShot(50, this, [this, generation]() { pollPendingQueries(generation); });
        return;
    }
    continueAgentLoop();
}

void AgentController::continueAgentLoop() {
    // 内部等待不发起新 LLM 轮次，也不消耗工具轮数；结果就绪后再续接协议。
    emit agentLoopIterating();
    if (!m_llmClient) {
        m_parameterCorrectionRetries = 0;
        m_parameterFailureSignatures.clear();
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred("LLM disconnected during agent loop");
        return;
    }
    if (turnDeadlineExceeded()) {
#ifdef DEEPLUX_BUILD_TESTS
        const int deadline = m_turnDeadlineOverride >= 0 ? m_turnDeadlineOverride : turnDeadlineMs();
#else
        const int deadline = turnDeadlineMs();
#endif
        const QString message = QStringLiteral("已超过单轮总时长上限（%1 ms），本轮已中止").arg(deadline);
        endTurnWithReason(message, message, false);
        return;
    }
    transitionTo(AgentState::Thinking);
    sendPreparedContext();
}

void AgentController::sendLLMRequest(AgentConversation ctx) {
    if (!m_llmClient)
        return;
    ctx.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_activeLLMRequestId = ctx.requestId;
    m_llmClient->sendRequest(ctx, ToolSchema::instance().allTools());
}

// ========== Helpers ==========

void AgentController::appendToolResultsForCalls(const QJsonArray& calls, const QJsonObject& result) {
    const QString content = QString(QJsonDocument(result).toJson(QJsonDocument::Compact));
    for (const QJsonValue& v : calls) {
        const QString callId = v.toObject().value(QStringLiteral("id")).toString();
        if (callId.isEmpty())
            continue; // 无 id 无法回 tool 消息（协议垃圾）
        AgentMessage tm;
        tm.role = QStringLiteral("tool");
        tm.toolCallId = callId;
        tm.content = content;
        m_conversationHistory.append(tm);
    }
    trimHistoryIfNeeded();
}

void AgentController::attachToolImages(const QJsonArray& results) {
    // 视觉可用性：模型能力 + 会话级降级记忆（设计 §6.5/§6.6）
    const bool visionAvailable = m_llmClient && m_llmClient->supportsImageInput() && !m_visionDisabledForSession;

    QStringList imageLines; // "<path> (<W>x<H>)"，磁盘原图尺寸
    QStringList notices;    // 超限/读取失败的文字说明
    QList<AgentImageAttachment> attachments;
    QSet<QString> seenPaths;
    qint64 totalBytes = 0;

    for (const QJsonValue& value : results) {
        const QJsonObject entry = value.toObject();
        const QString toolName = entry.value(QStringLiteral("tool")).toString();
        if (!isImageProducingTool(toolName))
            continue;
        const QJsonObject result = entry.value(QStringLiteral("result")).toObject();
        const QString path = result.value(QStringLiteral("imagePath")).toString();
        if (path.isEmpty() || seenPaths.contains(path))
            continue;
        if (!QFileInfo::exists(path)) {
            qWarning() << "[AgentController] tool image path does not exist:" << path;
            continue;
        }
        const QImage image(path);
        if (image.isNull()) {
            qWarning() << "[AgentController] failed to read tool image:" << path;
            continue;
        }
        seenPaths.insert(path);
        const int width = image.width();
        const int height = image.height();
        const QString imageLine = QStringLiteral("%1 (%2x%3)").arg(path).arg(width).arg(height);

        if (!visionAvailable) {
            // 视觉关闭：全部产物以文本降级说明（不占图片数量/字节预算，路径不丢）
            imageLines.append(imageLine);
            continue;
        }
        if (attachments.size() >= kMaxToolImagesPerTurn) {
            notices.append(QStringLiteral("- %1 skipped: per-turn image limit reached").arg(path));
            continue;
        }
        QImage scaled = image;
        const int maxLongEdge = m_llmClient ? m_llmClient->preferredImageLongEdge() : kMaxToolImageLongEdge;
        if (qMax(width, height) > maxLongEdge) {
            scaled = image.scaled(maxLongEdge, maxLongEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        QByteArray data;
        QBuffer pngBuffer(&data);
        QString mimeType = QStringLiteral("image/png");
        if (pngBuffer.open(QIODevice::WriteOnly) && scaled.save(&pngBuffer, "PNG")) {
            pngBuffer.close();
        } else {
            data.clear();
        }
        if (data.isEmpty() || data.size() > kMaxToolImageBytes) {
            // 降级 PNG→JPEG(q=85)，仍超限则丢弃并写文字说明
            data.clear();
            QBuffer jpegBuffer(&data);
            mimeType = QStringLiteral("image/jpeg");
            if (jpegBuffer.open(QIODevice::WriteOnly) && scaled.save(&jpegBuffer, "JPEG", 85))
                jpegBuffer.close();
            else
                data.clear();
        }
        if (data.isEmpty() || data.size() > kMaxToolImageBytes) {
            notices.append(QStringLiteral("- %1 skipped: image too large to attach").arg(path));
            continue;
        }
        if (totalBytes + data.size() > kMaxImageAttachmentBytes) {
            notices.append(QStringLiteral("- %1 skipped: per-turn image budget exhausted").arg(path));
            continue;
        }
        totalBytes += data.size();
        attachments.append({data, mimeType, path}); // description = 磁盘路径（非视觉链路兜底）
        imageLines.append(imageLine);
    }

    if (imageLines.isEmpty() && notices.isEmpty())
        return; // 无图：不产生空 user 消息，避免破坏 tool_calls 配对

    AgentMessage message;
    message.role = QStringLiteral("user");
    message.internalAutoInjected = true; // 属于原用户任务，不产生新的任务边界
    if (visionAvailable && !attachments.isEmpty()) {
        QStringList lines;
        for (const QString& line : imageLines)
            lines.append(QStringLiteral("- %1").arg(line));
        message.content = QStringLiteral("Images produced by the tool calls above (paths kept for reference):\n") +
                          lines.join(QLatin1Char('\n'));
        if (!notices.isEmpty())
            message.content += QLatin1Char('\n') + notices.join(QLatin1Char('\n'));
        message.images = attachments;
    } else {
        // 设计 §6.5：视觉关闭/非视觉模型的唯一文本降级口径（路径必须保留）
        QStringList blocks;
        for (const QString& line : imageLines) {
            blocks.append(QStringLiteral("Tool produced an image, but the configured model does not support image "
                                         "input.\nImage path: %1. If the user needs visual inspection, ask them to "
                                         "enable image input in Agent settings or switch to a vision-capable model.")
                              .arg(line));
        }
        message.content = blocks.join(QLatin1Char('\n'));
        if (!notices.isEmpty())
            message.content += QLatin1Char('\n') + notices.join(QLatin1Char('\n'));
    }
    m_conversationHistory.append(message);
    Logger::instance().addLog(QString("[AgentImages] attached %1 image(s)%2")
                                  .arg(attachments.size())
                                  .arg(visionAvailable ? QString() : QStringLiteral(" (text fallback)")),
                              LogLevel::Info, "Agent");
}

bool AgentController::stripImagesFromLastUserMessage() {
    for (int i = m_conversationHistory.size() - 1; i >= 0; --i) {
        AgentMessage& message = m_conversationHistory[i];
        if (message.role == QStringLiteral("user") && !message.images.isEmpty()) {
            message.images.clear();
            return true;
        }
    }
    return false;
}

int AgentController::contextBudgetTokens() const {
#ifdef DEEPLUX_BUILD_TESTS
    if (m_contextBudgetOverride >= 0)
        return m_contextBudgetOverride;
#endif
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "contextBudgetTokens", 128000)
                               : 128000;
    return qBound(16000, configured, 1048576);
}

int AgentController::toolResultMaxChars() const {
#ifdef DEEPLUX_BUILD_TESTS
    if (m_toolResultMaxCharsOverride >= 0)
        return m_toolResultMaxCharsOverride;
#endif
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "toolResultMaxChars", 6000)
                               : 6000;
    return qBound(1000, configured, 64000);
}

int AgentController::flowSummaryThreshold() const {
#ifdef DEEPLUX_BUILD_TESTS
    if (m_summaryThresholdOverride >= 0)
        return m_summaryThresholdOverride;
#endif
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "flowSummaryThreshold", 24)
                               : 24;
    return qBound(1, configured, 1000);
}

int AgentController::flowSummaryPreviewCount() const {
#ifdef DEEPLUX_BUILD_TESTS
    if (m_summaryPreviewOverride >= 0)
        return m_summaryPreviewOverride;
#endif
    const int configured = ConfigManager::instance().isInitialized()
                               ? ConfigManager::instance().groupInt("agent", "flowSummaryPreviewCount", 24)
                               : 24;
    return qBound(1, configured, 200);
}

#ifdef DEEPLUX_BUILD_TESTS
void AgentController::setContextLimitsForTest(int budgetTokens, int toolResultMaxChars, int summaryThreshold,
                                              int summaryPreviewCount) {
    if (budgetTokens >= 0)
        m_contextBudgetOverride = qBound(16000, budgetTokens, 1048576);
    if (toolResultMaxChars >= 0)
        m_toolResultMaxCharsOverride = qBound(1000, toolResultMaxChars, 64000);
    if (summaryThreshold >= 0)
        m_summaryThresholdOverride = qBound(1, summaryThreshold, 1000);
    if (summaryPreviewCount >= 0)
        m_summaryPreviewOverride = qBound(1, summaryPreviewCount, 200);
}
#endif

QString AgentController::truncateToolResult(const QString& json, int maxChars) {
    maxChars = qBound(1000, maxChars, 64000);
    if (json.size() <= maxChars)
        return json;
    const int head = maxChars * 2 / 3;
    const int tail = maxChars - head;
    return json.left(head) + QStringLiteral("\n…[truncated %1 chars]…\n").arg(json.size() - maxChars) +
           json.right(tail);
}

bool AgentController::echoesReasoning() const {
    if (!ConfigManager::instance().isInitialized())
        return false;
    return ConfigManager::instance().groupString("agent", "reasoningEcho", "none").trimmed().toLower() !=
           QStringLiteral("none");
}

qint64 AgentController::estimateTokensFor(const QList<AgentMessage>& messages, const QString& systemPrompt,
                                          bool countReasoning) const {
    // 每字符 ≈0.3 token，每消息 +20；reasoning 仅在回传时计入；图片按字节估算
    qint64 estimated = static_cast<qint64>(systemPrompt.length()) * 3 / 10;
    for (const AgentMessage& message : messages) {
        estimated += static_cast<qint64>(message.content.length()) * 3 / 10 + 20;
        if (!message.toolCalls.isEmpty()) {
            // tool_calls 的参数同样回传模型，必须计入预算
            estimated +=
                static_cast<qint64>(QJsonDocument(message.toolCalls).toJson(QJsonDocument::Compact).size()) * 3 / 10;
        }
        if (countReasoning && !message.reasoningContent.isEmpty())
            estimated += static_cast<qint64>(message.reasoningContent.length()) * 3 / 10;
        for (const AgentImageAttachment& image : message.images)
            estimated += m_llmClient ? m_llmClient->estimateImageTokens(image) : (image.data.size() + 2) / 3;
    }
    return estimated;
}

qint64 AgentController::estimateHistoryTokens() {
    // 真实发送的系统提示是 buildContext()（包含 m_systemPrompt 与当前工程摘要）
    return estimateTokensFor(m_conversationHistory, buildContext(), echoesReasoning());
}

void AgentController::repairOrphanToolGroups() {
    QList<AgentMessage> repaired;
    repaired.reserve(m_conversationHistory.size());
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role == QStringLiteral("tool")) {
            // I1：tool 必须属于前一条 assistant(tool_calls) 的某个 id
            int assistantIndex = repaired.size() - 1;
            while (assistantIndex >= 0 && repaired.at(assistantIndex).role == QLatin1String("tool"))
                --assistantIndex;
            if (assistantIndex < 0 || repaired.at(assistantIndex).role != QLatin1String("assistant") ||
                repaired.at(assistantIndex).toolCalls.isEmpty()) {
                qWarning() << "[AgentController] dropping orphan tool message" << message.toolCallId;
                continue;
            }
            QSet<QString> ids;
            for (const QJsonValue& call : repaired.at(assistantIndex).toolCalls)
                ids.insert(call.toObject().value(QStringLiteral("id")).toString());
            bool duplicate = false;
            for (int j = assistantIndex + 1; j < repaired.size(); ++j)
                duplicate = duplicate || repaired.at(j).toolCallId == message.toolCallId;
            if (!ids.contains(message.toolCallId) || duplicate) {
                qWarning() << "[AgentController] dropping unmatched tool message" << message.toolCallId;
                continue;
            }
            repaired.append(message);
            continue;
        }
        if (message.role == QStringLiteral("assistant") && !message.toolCalls.isEmpty()) {
            // 末尾的 assistant(tool_calls) 是"待执行"的合法状态（onLLMResponse 在工具
            // 执行前会 trim）：保留，等工具结果写入后下一次裁剪再校验完整性。
            if (i + 1 >= m_conversationHistory.size()) {
                repaired.append(message);
                continue;
            }
            // I2：assistant(tool_calls) 的每个 id 都必须有紧邻的 tool 响应，否则整组移除
            QSet<QString> ids;
            for (const QJsonValue& call : message.toolCalls) {
                const QString id = call.toObject().value(QStringLiteral("id")).toString();
                if (!id.isEmpty())
                    ids.insert(id);
            }
            QSet<QString> found;
            int j = i + 1;
            for (; j < m_conversationHistory.size() && m_conversationHistory.at(j).role == QStringLiteral("tool");
                 ++j) {
                found.insert(m_conversationHistory.at(j).toolCallId);
            }
            if (!ids.isEmpty() && !(ids - found).isEmpty()) {
                qWarning() << "[AgentController] dropping incomplete tool group at index" << i;
                i = j - 1; // 跳过该 assistant 及其全部 tool 响应
                continue;
            }
            repaired.append(message);
            continue;
        }
        repaired.append(message);
    }
    m_conversationHistory = repaired;
}

QSet<int> AgentController::protectedHistoryIndices() const {
    QSet<int> protectedIndices;
    int lastRealUser = -1;
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role == QStringLiteral("user") && !message.internalAutoInjected)
            lastRealUser = i;
    }
    if (lastRealUser >= 0)
        protectedIndices.insert(lastRealUser);

    // 从尾部向前：保护在途组（缺结果）与最近一个完整工具组；后者连同其后的自动回灌消息
    bool completeGroupProtected = false;
    for (int i = m_conversationHistory.size() - 1; i >= 0; --i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role != QStringLiteral("assistant") || message.toolCalls.isEmpty())
            continue;
        QSet<QString> ids;
        for (const QJsonValue& call : message.toolCalls) {
            const QString id = call.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                ids.insert(id);
        }
        QSet<QString> found;
        int end = i + 1;
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("tool")) {
            found.insert(m_conversationHistory.at(end).toolCallId);
            ++end;
        }
        if (!(ids - found).isEmpty()) {
            // 在途/不完整组：不得当作历史损坏删除
            for (int k = i; k < end; ++k)
                protectedIndices.insert(k);
            continue;
        }
        if (completeGroupProtected)
            continue;
        for (int k = i; k < end; ++k)
            protectedIndices.insert(k);
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("user") &&
               m_conversationHistory.at(end).internalAutoInjected) {
            protectedIndices.insert(end);
            ++end;
        }
        completeGroupProtected = true;
    }
    return protectedIndices;
}

bool AgentController::dropOldestUserTurn() {
    int firstRealUser = -1;
    int secondRealUser = -1;
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role != QStringLiteral("user") || message.internalAutoInjected)
            continue;
        if (firstRealUser < 0) {
            firstRealUser = i;
        } else {
            secondRealUser = i;
            break;
        }
    }
    if (secondRealUser < 0)
        return false;

    const QSet<int> protectedIndices = protectedHistoryIndices();
    for (int i = 0; i < secondRealUser; ++i) {
        if (protectedIndices.contains(i))
            return false;
    }
    m_conversationHistory = m_conversationHistory.mid(secondRealUser);
    while (!m_conversationHistory.isEmpty() && (m_conversationHistory.constFirst().role != QStringLiteral("user") ||
                                                m_conversationHistory.constFirst().internalAutoInjected))
        m_conversationHistory.removeFirst();
    return true;
}

bool AgentController::dropOldestToolGroup() {
    // 从 index=1 起，只删保护集合之外的完整工具组（含其后的自动回灌消息）
    const QSet<int> protectedIndices = protectedHistoryIndices();
    for (int i = 1; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role != QStringLiteral("assistant") || message.toolCalls.isEmpty())
            continue;
        int end = i + 1;
        QSet<QString> ids;
        for (const QJsonValue& call : message.toolCalls) {
            const QString id = call.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty())
                ids.insert(id);
        }
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("tool")) {
            if (!ids.contains(m_conversationHistory.at(end).toolCallId))
                qWarning() << "[AgentController] tool group contains unmatched tool while dropping group";
            ++end;
        }
        while (end < m_conversationHistory.size() && m_conversationHistory.at(end).role == QStringLiteral("user") &&
               m_conversationHistory.at(end).internalAutoInjected)
            ++end;

        bool overlapsProtected = false;
        for (int k = i; k < end; ++k) {
            if (protectedIndices.contains(k)) {
                overlapsProtected = true;
                break;
            }
        }
        if (overlapsProtected)
            continue;

        for (int k = end - 1; k >= i; --k)
            m_conversationHistory.removeAt(k);
        return true;
    }
    return false;
}

void AgentController::trimHistoryIfNeeded() {
    constexpr int kMaxHistoryMessages = 64; // 既有硬上限（设计 §7.6）
    if (m_conversationHistory.isEmpty())
        return;

    repairOrphanToolGroups();

    // I5：首条必须为真实 user（system 由 systemPrompt 单独承载；自动回灌不构成边界）
    while (!m_conversationHistory.isEmpty() && (m_conversationHistory.constFirst().role != QStringLiteral("user") ||
                                                m_conversationHistory.constFirst().internalAutoInjected))
        m_conversationHistory.removeFirst();

    const int budget = contextBudgetTokens();
    int guard = 0;
    while ((estimateHistoryTokens() > budget || m_conversationHistory.size() > kMaxHistoryMessages) &&
           guard++ < 100000) {
        if (dropOldestUserTurn())
            continue;
        if (dropOldestToolGroup())
            continue;
        break; // 只剩保护集合：由发送前校验失败关闭（context_budget_exceeded）
    }

    while (!m_conversationHistory.isEmpty() && (m_conversationHistory.constFirst().role != QStringLiteral("user") ||
                                                m_conversationHistory.constFirst().internalAutoInjected))
        m_conversationHistory.removeFirst();
}

bool AgentController::historyPairsValid(QString* error) const {
    QSet<QString> pending;
    bool inGroup = false;
    for (int i = 0; i < m_conversationHistory.size(); ++i) {
        const AgentMessage& message = m_conversationHistory.at(i);
        if (message.role == QStringLiteral("assistant") && !message.toolCalls.isEmpty()) {
            if (inGroup) {
                if (error)
                    *error = QStringLiteral("tool group at message %1 is missing results").arg(i - 1);
                return false;
            }
            pending.clear();
            for (const QJsonValue& call : message.toolCalls) {
                const QString id = call.toObject().value(QStringLiteral("id")).toString();
                if (id.isEmpty()) {
                    if (error)
                        *error = QStringLiteral("assistant tool_call without id at message %1").arg(i);
                    return false;
                }
                pending.insert(id);
            }
            if (pending.isEmpty()) {
                if (error)
                    *error = QStringLiteral("empty tool_calls at message %1").arg(i);
                return false;
            }
            inGroup = true;
            continue;
        }
        if (message.role == QStringLiteral("tool")) {
            if (!inGroup || !pending.contains(message.toolCallId)) {
                if (error)
                    *error = QStringLiteral("unexpected tool result '%1' at message %2").arg(message.toolCallId).arg(i);
                return false;
            }
            pending.remove(message.toolCallId);
            if (pending.isEmpty())
                inGroup = false;
            continue;
        }
        if (inGroup) {
            if (error)
                *error = QStringLiteral("tool group interrupted at message %1").arg(i);
            return false;
        }
    }
    if (inGroup) {
        if (error)
            *error = QStringLiteral("in-flight tool group is missing results");
        return false;
    }
    return true;
}

void AgentController::failContextSend(const QString& code, const QString& detail) {
    const QString message = QStringLiteral("Context send refused (%1); no request was sent: %2").arg(code, detail);
    Logger::instance().addLog(QStringLiteral("[Context] %1").arg(message), LogLevel::Warning, "Agent");
    AgentActionLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.actor = QStringLiteral("Agent");
    entry.action = code;
    entry.result = QStringLiteral("error");
    entry.undoable = false;
    emit actionLogEntryAdded(entry);
    transitionTo(AgentState::Idle);
    emit llmErrorOccurred(message);
}

bool AgentController::sendPreparedContext() {
    trimHistoryIfNeeded();

    QString pairError;
    if (!historyPairsValid(&pairError)) {
        failContextSend(QStringLiteral("context_invalid_history"), pairError);
        return false;
    }

    AgentConversation ctx;
    ctx.messages = m_conversationHistory;
    ctx.systemPrompt = buildContext();

    // buildContext 会把超长系统提示截断到 16KB；预算校验仍以"用户配置的完整系统提示"
    // 为准，避免截断掩盖系统提示本身超限（B0/B3 契约）。
    const QString budgetPrompt = m_systemPrompt.size() > ctx.systemPrompt.size() ? m_systemPrompt : ctx.systemPrompt;
    const qint64 estimated = estimateTokensFor(ctx.messages, budgetPrompt, echoesReasoning());
    constexpr int kMaxHistoryMessages = 64;
    if (estimated > contextBudgetTokens() || ctx.messages.size() > kMaxHistoryMessages) {
        failContextSend(QStringLiteral("context_budget_exceeded"),
                        QStringLiteral("estimated %1 tokens / %2 messages, budget %3 tokens")
                            .arg(estimated)
                            .arg(ctx.messages.size())
                            .arg(contextBudgetTokens()));
        return false;
    }

    if (!m_llmClient) {
        transitionTo(AgentState::Idle);
        emit llmErrorOccurred(QStringLiteral("LLM client not configured"));
        return false;
    }
    sendLLMRequest(ctx);
    return true;
}

void AgentController::onGuiEvent(const GuiEvent& event) {
    // AgentObserver DEBUG 镜像日志过于冗余，降级为不输出
    Q_UNUSED(event)
}

QJsonObject AgentController::handleToolCall(const QString& toolName, const QJsonObject& params) {
    if (state() != AgentState::Idle)
        return QJsonObject{{"error", QStringLiteral("Agent is busy processing another request")},
                           {"errorCode", QStringLiteral("ENGINE_BUSY")},
                           {"retryable", true}};
    if (!ToolSchema::instance().hasTool(toolName)) {
        QJsonObject result = m_actor->executeTool(toolName, params); // UNKNOWN_TOOL,零副作用
        takeElapsedMs(result);
        return result;
    }

    const QJsonObject strictParamError = preConfirmationParameterError(toolName, params);
    if (!strictParamError.isEmpty())
        return strictParamError;

    // 阶段1 §2（F08）：Bridge 路径与聊天路径共用同一权限判定
    switch (evaluateToolGate(toolName, params)) {
    case ToolGate::Deny: {
        const QJsonObject userPermissionError =
            m_actor ? m_actor->userPermissionPreflight(toolName, params) : QJsonObject();
        if (!userPermissionError.isEmpty())
            return userPermissionError;
        return QJsonObject{{"error", QStringLiteral("Permission denied: %1 mode does not allow '%2'")
                                         .arg(permissionLevelName(m_permissionLevel), toolName)},
                           {"errorCode", QStringLiteral("PERMISSION_DENIED")},
                           {"retryable", false}};
    }
    case ToolGate::RequireConfirmation: {
        QJsonObject selectorError = cameraCyclePreflight(toolName, params);
        if (selectorError.isEmpty())
            selectorError = cameraSessionPreflight(toolName);
        if (selectorError.isEmpty())
            selectorError = m_actor->flowSelectorPreflight(toolName, params);
        if (selectorError.isEmpty() && toolName == QLatin1String("edit_active_annotation_masks"))
            selectorError = executeDataSourceViewAction(QStringLiteral("__sam_mask_edit_preflight"), params);
        if (selectorError.isEmpty() && toolName == QLatin1String("edit_active_annotation_categories"))
            selectorError = executeDataSourceViewAction(QStringLiteral("__sam_category_edit_preflight"), params);
        if (selectorError.isEmpty() && toolName == QLatin1String("cancel_active_annotation_prompt"))
            selectorError = executeDataSourceViewAction(QStringLiteral("__sam_prompt_cancel_preflight"), params);
        if (selectorError.isEmpty() && toolName == QLatin1String("save_active_annotation_session"))
            selectorError = executeDataSourceViewAction(QStringLiteral("__sam_session_save_preflight"), params);
        if (selectorError.isEmpty() && toolName == QLatin1String("open_active_annotation_session"))
            selectorError = executeDataSourceViewAction(QStringLiteral("__sam_session_open_preflight"), params);
        if (selectorError.isEmpty())
            selectorError = unresolvedFileTargetPreflight(toolName);
        if (!selectorError.isEmpty()) {
            Logger::instance().warning(QStringLiteral("[ToolCall] %1 rejected: %2")
                                           .arg(toolName, selectorError.value(QStringLiteral("errorCode")).toString()),
                                       QStringLiteral("Agent"));
            AgentActionLogEntry entry;
            entry.timestamp = QDateTime::currentDateTime();
            entry.actor = QStringLiteral("Agent");
            entry.action = toolName;
            entry.params = compactAuditParams(toolName, params);
            entry.result = QStringLiteral("error");
            emit actionLogEntryAdded(entry);
            return selectorError;
        }
        QJsonArray calls;
        calls.append(makeToolCall(toolName, params));
        beginPendingConfirmation(calls, QString(), false);
        QJsonObject response{{"status", QStringLiteral("pending_confirmation")},
                             {"tools", pendingToolCallsForUiAndBridge(m_pendingToolCalls)}};
        response.insert(QStringLiteral("confirmation"), pendingConfirmationPayload());
        return response;
    }
    case ToolGate::Allow:
        break;
    }

    QJsonObject selectorError = cameraCyclePreflight(toolName, params);
    if (selectorError.isEmpty())
        selectorError = cameraSessionPreflight(toolName);
    if (selectorError.isEmpty())
        selectorError = m_actor->flowSelectorPreflight(toolName, params);
    if (selectorError.isEmpty())
        selectorError = unresolvedFileTargetPreflight(toolName);
    if (!selectorError.isEmpty()) {
        AgentActionLogEntry entry;
        entry.timestamp = QDateTime::currentDateTime();
        entry.actor = QStringLiteral("Agent");
        entry.action = toolName;
        entry.params = compactAuditParams(toolName, params);
        entry.result = QStringLiteral("error");
        emit actionLogEntryAdded(entry);
        return selectorError;
    }

    Logger::instance().addLog(QString("[ToolCall] %1").arg(toolName), LogLevel::Info, "Agent");
    emit agentActionReceived(toolName, params);
    QJsonObject result = m_actor->executeTool(toolName, params);
    AgentActionLogEntry e;
    e.timestamp = QDateTime::currentDateTime();
    e.actor = "Agent";
    e.action = toolName;
    e.params = compactAuditParams(toolName, params);
    e.durationMs = takeElapsedMs(result);
    e.result = result.contains("error")
                   ? "error"
                   : (result.value(QStringLiteral("status")) == QLatin1String("pending") ? "pending" : "success");
    // 阶段1 §6：undoable = 本次成功且确属可撤销的工程编辑命令
    e.undoable = isUndoableChange(toolName, result);
    emit actionLogEntryAdded(e);
    return result;
}

bool AgentController::toolRequiresConfirmation(PermissionLevel level, const QString& toolName) {
    const bool known = ToolSchema::instance().hasTool(toolName);
    const bool readOnly = known && isReadOnlyToolName(toolName);
    const bool dangerous = known && isDangerousToolName(toolName);
    if (level == PermissionLevel::Observer)
        return !readOnly;
    if (level == PermissionLevel::Advisor)
        return !readOnly;
    return dangerous || isExecutionEntryPoint(toolName);
}

AgentController::ToolGate AgentController::evaluateToolGate(const QString& toolName, const QJsonObject& params) const {
    if (m_actor && !m_actor->userPermissionPreflight(toolName, params).isEmpty())
        return ToolGate::Deny;
    const bool readOnly = isReadOnlyToolName(toolName);
    switch (m_permissionLevel) {
    case PermissionLevel::Observer:
        // 阶段1 §2（F08）：Observer 只读工具三入口一致放行,写操作一致拒绝
        return readOnly ? ToolGate::Allow : ToolGate::Deny;
    case PermissionLevel::Advisor:
        return readOnly ? ToolGate::Allow : ToolGate::RequireConfirmation;
    case PermissionLevel::Autopilot:
        // 阶段1 §5：run_flow 等执行入口按保守口径必须确认（不因未标 dangerous 绕过）
        return (isDangerousToolName(toolName) || isExecutionEntryPoint(toolName, params) ||
                hasImplicitProjectSaveTarget(toolName, params, ProjectManager::instance().currentProject()))
                   ? ToolGate::RequireConfirmation
                   : ToolGate::Allow;
    }
    return ToolGate::Deny;
}

QJsonObject AgentController::unresolvedFileTargetPreflight(const QString& toolName) const {
    static const QStringList fileExecutionTools{QStringLiteral("run_flow"), QStringLiteral("step_once"),
                                                QStringLiteral("resume_flow")};
    if (!fileExecutionTools.contains(toolName) ||
        (m_permissionLevel != PermissionLevel::Autopilot && m_permissionLevel != PermissionLevel::Advisor))
        return {};

    const Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return {};

    const QJsonObject execution = cameraExecutionSnapshot(toolName, project);
    QSet<QString> executionModuleIds;
    for (const QJsonValue& value : execution.value(QStringLiteral("moduleIds")).toArray())
        executionModuleIds.insert(value.toString());
    const bool executionScopeKnown = execution.value(QStringLiteral("complete")).toBool();

    const PluginManager& plugins = PluginManager::instance();
    QJsonArray unresolvedCandidates;
    QJsonArray unresolvedExecutionTargets;
    for (const ModuleInstance& instance : project->modules()) {
        if (!instance.enabled)
            continue;
        const QString pluginId = plugins.pluginInfo(instance.moduleId).id;
        if (pluginId != QLatin1String("com.deeplux.plugin.writetext") &&
            pluginId != QLatin1String("com.deeplux.plugin.savedata") &&
            pluginId != QLatin1String("com.deeplux.plugin.folder"))
            continue;

        PluginManager::ModulePtr module = PluginManager::instance().createManagedModule(instance.moduleId);
        QJsonObject effective = module ? module->defaultParams() : QJsonObject();
        for (auto it = instance.params.begin(); it != instance.params.end(); ++it)
            effective.insert(it.key(), it.value());
        const QString configuredFilePath = effective.value(QStringLiteral("filePath")).toString();
        const bool isFileOutput = pluginId == QLatin1String("com.deeplux.plugin.writetext") ||
                                  pluginId == QLatin1String("com.deeplux.plugin.savedata") ||
                                  pluginId == QLatin1String("com.deeplux.plugin.saveimage");
        const bool unresolvedFilePath = isFileOutput && (configuredFilePath.trimmed().isEmpty() ||
                                                         resolvedFileTargetPath(configuredFilePath).isEmpty());
        const bool unresolvedFolderCreate =
            pluginId == QLatin1String("com.deeplux.plugin.folder") &&
            effective.value(QStringLiteral("operation")).toString() == QLatin1String("Create") &&
            effective.value(QStringLiteral("folderPath")).toString().trimmed().isEmpty();
        if (!unresolvedFilePath && !unresolvedFolderCreate)
            continue;

        unresolvedCandidates.append(instance.id);
        if (executionScopeKnown && executionModuleIds.contains(instance.id))
            unresolvedExecutionTargets.append(instance.id);
    }
    if (unresolvedCandidates.isEmpty())
        return {};

    if (!executionScopeKnown) {
        return QJsonObject{
            {QStringLiteral("error"),
             QStringLiteral("Cannot confirm file side effects because the execution scope is unresolved")},
            {QStringLiteral("errorCode"), QStringLiteral("EXECUTION_SCOPE_UNRESOLVED")},
            {QStringLiteral("retryable"), false},
            {QStringLiteral("details"), QJsonObject{{QStringLiteral("instanceIds"), unresolvedCandidates},
                                                    {QStringLiteral("candidateScopeComplete"), false}}}};
    }
    if (unresolvedExecutionTargets.isEmpty())
        return {};

    return QJsonObject{
        {QStringLiteral("error"), QStringLiteral("%1 cannot confirm enabled file side effects whose target path is "
                                                 "unresolved at confirmation time")
                                      .arg(permissionLevelName(m_permissionLevel))},
        {QStringLiteral("errorCode"), QStringLiteral("SIDE_EFFECT_TARGET_UNRESOLVED")},
        {QStringLiteral("retryable"), false},
        {QStringLiteral("details"), QJsonObject{{QStringLiteral("instanceIds"), unresolvedExecutionTargets}}}};
}

QJsonObject AgentController::cameraCyclePreflight(const QString& toolName, const QJsonObject& params) const {
    if (toolName != QLatin1String("run_flow") ||
        (m_permissionLevel != PermissionLevel::Autopilot && m_permissionLevel != PermissionLevel::Advisor) ||
        params.value(QStringLiteral("mode")).toString(QStringLiteral("once")) != QLatin1String("cycle") ||
        params.value(QStringLiteral("maxCycles")).toInt() <= 1)
        return {};

    const QJsonObject snapshots = cameraCycleCapabilitySnapshot(ProjectManager::instance().currentProject());
    QJsonArray rejectedInstances;
    QJsonArray unavailableInstances;
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it) {
        const QJsonObject snapshot = it.value().toObject();
        const QString instanceId = snapshot.value(QStringLiteral("instanceId")).toString();
        const QString configuredDeviceId = snapshot.value(QStringLiteral("configuredDeviceId")).toString();
        const QJsonObject targetSnapshot = snapshot.value(QStringLiteral("targetSnapshot")).toObject();
        if (!snapshot.value(QStringLiteral("sourceResolved")).toBool() ||
            !snapshot.value(QStringLiteral("queryAvailable")).toBool() ||
            snapshot.value(QStringLiteral("pendingDiscoveries")).toInt(-1) != 0 ||
            (!configuredDeviceId.isEmpty() && !targetSnapshot.value(QStringLiteral("found")).toBool())) {
            rejectedInstances.append(instanceId);
            unavailableInstances.append(instanceId);
        } else if (snapshot.value(QStringLiteral("asyncCapable")).toBool()) {
            rejectedInstances.append(instanceId);
        }
    }
    if (rejectedInstances.isEmpty())
        return {};

    QJsonObject details{{QStringLiteral("instanceIds"), rejectedInstances}};
    if (!unavailableInstances.isEmpty())
        details.insert(QStringLiteral("unavailableInstanceIds"), unavailableInstances);
    return QJsonObject{{QStringLiteral("error"), QStringLiteral("Multi-cycle execution is unsupported for the "
                                                                "configured asynchronous camera lifecycle")},
                       {QStringLiteral("errorCode"), QStringLiteral("CAMERA_CYCLE_UNSUPPORTED")},
                       {QStringLiteral("retryable"), false},
                       {QStringLiteral("details"), details}};
}

QJsonObject AgentController::cameraSessionPreflight(const QString& toolName) const {
    if ((toolName != QLatin1String("run_flow") && toolName != QLatin1String("step_once") &&
         toolName != QLatin1String("resume_flow")) ||
        (m_permissionLevel != PermissionLevel::Autopilot && m_permissionLevel != PermissionLevel::Advisor))
        return {};

    const Project* project = ProjectManager::instance().currentProject();
    if (!project)
        return {};
    const QJsonObject snapshot = cameraExecutionSnapshot(toolName, project);
    const QJsonArray candidates = snapshot.value(QStringLiteral("candidates")).toArray();
    if (!snapshot.value(QStringLiteral("complete")).toBool()) {
        if (toolName == QLatin1String("resume_flow"))
            return {};

        QJsonArray unresolvedInstances;
        PluginManager& plugins = PluginManager::instance();
        for (const ModuleInstance& instance : project->modules()) {
            if (!instance.enabled ||
                plugins.pluginInfo(instance.moduleId).id != QLatin1String("com.deeplux.plugin.grabimage"))
                continue;
            PluginManager::ModulePtr module = plugins.createManagedModule(instance.moduleId);
            QJsonObject effective = module ? module->defaultParams() : QJsonObject();
            for (auto it = instance.params.begin(); it != instance.params.end(); ++it)
                effective.insert(it.key(), it.value());
            const bool possibleCameraSource =
                !effective.contains(QStringLiteral("grabSource")) ||
                effective.value(QStringLiteral("grabSource")).toString() == QLatin1String("Camera");
            if (possibleCameraSource && !effective.value(QStringLiteral("cameraId")).toString().trimmed().isEmpty())
                unresolvedInstances.append(instance.id);
        }
        if (unresolvedInstances.isEmpty())
            return {};
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("Camera execution scope does not match a "
                                                     "current, fully analyzed flow plan")},
            {QStringLiteral("errorCode"), QStringLiteral("CAMERA_LIFECYCLE_UNAVAILABLE")},
            {QStringLiteral("retryable"), false},
            {QStringLiteral("details"), QJsonObject{{QStringLiteral("instanceIds"), unresolvedInstances}}}};
    }
    if (candidates.isEmpty())
        return {};

    QJsonArray instanceIds;
    QJsonArray unavailableIds;
    QJsonArray missingSessionIds;
    int asyncCandidateCount = 0;
    bool hasRepeatableAsyncCapture = false;
    for (const QJsonValue& value : candidates) {
        const QJsonObject candidate = value.toObject();
        const QString instanceId = candidate.value(QStringLiteral("instanceId")).toString();
        const QString deviceId = candidate.value(QStringLiteral("deviceId")).toString();
        if (!candidate.value(QStringLiteral("sourceResolved")).toBool() ||
            (!deviceId.isEmpty() && !candidate.value(QStringLiteral("queryAvailable")).toBool())) {
            unavailableIds.append(instanceId);
            continue;
        }
        if (!candidate.value(QStringLiteral("asyncCapable")).toBool())
            continue;

        ++asyncCandidateCount;
        instanceIds.append(instanceId);
        hasRepeatableAsyncCapture = hasRepeatableAsyncCapture || snapshot.value(QStringLiteral("mayRepeat")).toBool();
        const QJsonObject lifecycle = candidate.value(QStringLiteral("lifecycle")).toObject();
        const QString state = lifecycle.value(QStringLiteral("state")).toString();
        const bool settled = candidate.value(QStringLiteral("hasAsyncSession")).toBool() &&
                             (state == QLatin1String("connected") || state == QLatin1String("grabbing")) &&
                             lifecycle.value(QStringLiteral("hasSession")).toBool() &&
                             !lifecycle.value(QStringLiteral("operationPending")).toBool();
        if (!deviceId.isEmpty() && !settled)
            missingSessionIds.append(
                QJsonObject{{QStringLiteral("instanceId"), instanceId}, {QStringLiteral("deviceId"), deviceId}});
    }

    if (!unavailableIds.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("Camera execution candidates or lifecycle "
                                                                    "cannot be resolved safely")},
                           {QStringLiteral("errorCode"), QStringLiteral("CAMERA_LIFECYCLE_UNAVAILABLE")},
                           {QStringLiteral("retryable"), false},
                           {QStringLiteral("details"), QJsonObject{{QStringLiteral("instanceIds"), unavailableIds}}}};
    }
    if (asyncCandidateCount == 0)
        return {};
    if (asyncCandidateCount > 1 || hasRepeatableAsyncCapture) {
        return QJsonObject{
            {QStringLiteral("error"),
             QStringLiteral("A single async camera session cannot safely serve multiple captures")},
            {QStringLiteral("errorCode"), QStringLiteral("CAMERA_SESSION_MULTIPLE_CAPTURES_UNSUPPORTED")},
            {QStringLiteral("retryable"), false},
            {QStringLiteral("details"), QJsonObject{{QStringLiteral("instanceIds"), instanceIds}}}};
    }
    if (!missingSessionIds.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("Connect the async camera and wait for its session "
                                                                    "to settle before running this flow")},
                           {QStringLiteral("errorCode"), QStringLiteral("CAMERA_SESSION_REQUIRED")},
                           {QStringLiteral("retryable"), false},
                           {QStringLiteral("details"), QJsonObject{{QStringLiteral("targets"), missingSessionIds}}}};
    }
    return {};
}

QJsonObject AgentController::cameraCycleBatchPreflight(const QJsonArray& calls) const {
    if (m_permissionLevel != PermissionLevel::Autopilot && m_permissionLevel != PermissionLevel::Advisor)
        return {};

    bool hasCameraLifecycleChange = false;
    bool hasMultiCycle = false;
    for (int callIndex = 0; callIndex < calls.size(); ++callIndex) {
        const QJsonObject call = calls.at(callIndex).toObject();
        const QString name = toolCallName(call);
        hasCameraLifecycleChange = hasCameraLifecycleChange || name == QLatin1String("discover_cameras") ||
                                   name == QLatin1String("connect_camera");
        const QJsonObject params = toolCallParams(call);
        hasMultiCycle = hasMultiCycle || (name == QLatin1String("run_flow") &&
                                          params.value(QStringLiteral("mode")).toString() == QLatin1String("cycle") &&
                                          params.value(QStringLiteral("maxCycles")).toInt() > 1);
    }
    if (!hasCameraLifecycleChange || !hasMultiCycle)
        return {};

    const QJsonObject snapshots = cameraCycleCapabilitySnapshot(ProjectManager::instance().currentProject());
    if (snapshots.isEmpty())
        return {};

    QJsonArray instanceIds;
    for (auto it = snapshots.begin(); it != snapshots.end(); ++it)
        instanceIds.append(it.value().toObject().value(QStringLiteral("instanceId")));
    return QJsonObject{
        {QStringLiteral("error"), QStringLiteral("Camera lifecycle changes and multi-cycle camera execution cannot "
                                                 "share a tool batch")},
        {QStringLiteral("errorCode"), QStringLiteral("CAMERA_CYCLE_UNSUPPORTED")},
        {QStringLiteral("retryable"), false},
        {QStringLiteral("details"),
         QJsonObject{{QStringLiteral("instanceIds"), instanceIds},
                     {QStringLiteral("reason"), QStringLiteral("CAMERA_LIFECYCLE_BATCH_CONFLICT")}}}};
}

namespace {
// 阶段1 §4：工程内容指纹——序列化 sha1，可检出任何来源（Agent/GUI/迁移）的窗口期修改
QByteArray projectFingerprint(const Project* proj) {
    if (!proj)
        return {};
    return QCryptographicHash::hash(QJsonDocument(proj->toJson()).toJson(QJsonDocument::Compact),
                                    QCryptographicHash::Sha1)
        .toHex();
}
} // namespace

void AgentController::beginPendingConfirmation(const QJsonArray& calls, const QString& content, bool continueAfter) {
    QJsonArray normalizedCalls = calls;
    for (int index = 0; index < normalizedCalls.size(); ++index) {
        QJsonObject call = normalizedCalls.at(index).toObject();
        if (toolCallName(call) != QLatin1String("edit_active_annotation_categories"))
            continue;
        QJsonObject params = toolCallParams(call);
        for (const QString& key : {QStringLiteral("category"), QStringLiteral("label")}) {
            if (params.value(key).isString())
                params.insert(key, params.value(key).toString().trimmed());
        }
        if (call.contains(QStringLiteral("name"))) {
            call.insert(QStringLiteral("arguments"), params);
        } else {
            QJsonObject function = call.value(QStringLiteral("function")).toObject();
            function.insert(QStringLiteral("arguments"), params);
            call.insert(QStringLiteral("function"), function);
        }
        normalizedCalls.replace(index, call);
    }
    m_pendingToolCalls = normalizedCalls;
    m_continueAfterPendingTools = continueAfter;
    m_pendingConfirmationId = QStringLiteral("cfm_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    // 阶段1 §4：绑定当前工程与配置版本快照（filePath + Agent 撤销栈位置 + 内容指纹）
    Project* proj = ProjectManager::instance().currentProject();
    m_pendingHasProject = (proj != nullptr);
    m_pendingProjectPath = proj ? proj->filePath() : QString();
    m_pendingProjectName = proj ? proj->name() : QString();
    m_pendingProjectFingerprint = projectFingerprint(proj);
    m_pendingDataSourceEpochCaptured = false;
    m_pendingDataSourceProjectIdentity = 0;
    m_pendingDataSourceGeneration = 0;
    m_pendingDataSourceViewOwnerCaptured = false;
    m_pendingDataSourceViewOwner.clear();
    m_pendingDataSourceViewOwnerIdentity = 0;
    m_pendingFileTargetsCaptured = false;
    for (const QJsonValue& call : normalizedCalls) {
        const QString toolName = toolCallName(call.toObject());
        const bool dataSourceTool = toolName == QLatin1String("remove_data_source_reference") ||
                                    toolName == QLatin1String("display_data_source") ||
                                    toolName == QLatin1String("set_data_source_visibility") ||
                                    toolName == QLatin1String("display_data_source_in_dedicated_viewport") ||
                                    toolName == QLatin1String("copy_data_source_path_to_clipboard") ||
                                    toolName == QLatin1String("open_data_source_folder_for_user") ||
                                    toolName == QLatin1String("import_data_source");
        if (dataSourceTool && !m_pendingDataSourceEpochCaptured) {
            m_pendingDataSourceEpochCaptured = true;
            m_pendingDataSourceProjectIdentity = proj ? proj->runtimeIdentity() : 0;
            m_pendingDataSourceGeneration = proj ? proj->dataSourceCollectionGeneration() : 0;
        }
        if (!m_pendingDataSourceViewOwnerCaptured &&
            (toolName == QLatin1String("display_data_source") ||
             toolName == QLatin1String("set_data_source_visibility") ||
             toolName == QLatin1String("display_data_source_in_dedicated_viewport") ||
             toolName == QLatin1String("copy_data_source_path_to_clipboard") ||
             toolName == QLatin1String("open_data_source_folder_for_user") ||
             toolName == QLatin1String("open_user_knowledge_folder_for_user") ||
             toolName == QLatin1String("save_viewport_snapshot_for_user"))) {
            m_pendingDataSourceViewOwnerCaptured = true;
            m_pendingDataSourceViewOwner = m_dataSourceViewOwner;
            m_pendingDataSourceViewOwnerIdentity = reinterpret_cast<quintptr>(m_dataSourceViewOwner.data());
        }
        if (!m_pendingDataSourceViewOwnerCaptured && (toolName == QLatin1String("begin_quick_measurement") ||
                                                      toolName == QLatin1String("edit_active_annotation_masks") ||
                                                      toolName == QLatin1String("edit_active_annotation_categories") ||
                                                      toolName == QLatin1String("cancel_active_annotation_prompt") ||
                                                      toolName == QLatin1String("save_active_annotation_session") ||
                                                      toolName == QLatin1String("open_active_annotation_session"))) {
            m_pendingDataSourceViewOwnerCaptured = true;
            m_pendingDataSourceViewOwner = m_dataSourceViewOwner;
            m_pendingDataSourceViewOwnerIdentity = reinterpret_cast<quintptr>(m_dataSourceViewOwner.data());
        }
    }
    for (const QJsonValue& call : normalizedCalls) {
        const QString name = toolCallName(call.toObject());
        if (name == QLatin1String("open_project") || name == QLatin1String("run_flow") ||
            name == QLatin1String("step_once") || name == QLatin1String("resume_flow") ||
            name == QLatin1String("save_project") || name == QLatin1String("sam_annotate") ||
            name == QLatin1String("sam_predict") || name == QLatin1String("export_annotation_session") ||
            name == QLatin1String("transform_annotation_session") || name == QLatin1String("load_module_plugin") ||
            name == QLatin1String("unload_module_plugin") || name == QLatin1String("connect_camera") ||
            name == QLatin1String("disconnect_camera") || name == QLatin1String("import_data_source")) {
            m_pendingFileTargetsCaptured = true;
            break;
        }
        if (name == QLatin1String("add_knowledge_document") || name == QLatin1String("remove_knowledge_document")) {
            m_pendingFileTargetsCaptured = true;
            break;
        }
        if (name == QLatin1String("create_global_variable") || name == QLatin1String("set_global_variable") ||
            name == QLatin1String("remove_global_variable")) {
            m_pendingFileTargetsCaptured = true;
            break;
        }
        if (hasImplicitProjectSaveTarget(name, toolCallParams(call.toObject()), proj)) {
            m_pendingFileTargetsCaptured = true;
            break;
        }
    }
    m_pendingFileTargetPaths =
        m_pendingFileTargetsCaptured ? fileTargetPathsForCalls(normalizedCalls, proj) : QJsonObject();
    for (int callIndex = 0; callIndex < normalizedCalls.size(); ++callIndex) {
        const QJsonObject call = normalizedCalls.at(callIndex).toObject();
        const QString name = toolCallName(call);
        const bool isQuickMeasure = name == QLatin1String("begin_quick_measurement");
        const bool isSamMaskEdit = name == QLatin1String("edit_active_annotation_masks");
        const bool isSamCategoryEdit = name == QLatin1String("edit_active_annotation_categories");
        const bool isSamPromptCancel = name == QLatin1String("cancel_active_annotation_prompt");
        const bool isSamSessionSave = name == QLatin1String("save_active_annotation_session");
        const bool isSamSessionOpen = name == QLatin1String("open_active_annotation_session");
        if (!isQuickMeasure && !isSamMaskEdit && !isSamCategoryEdit && !isSamPromptCancel && !isSamSessionSave &&
            !isSamSessionOpen)
            continue;
        QJsonObject snapshot;
        if (m_dataSourceViewOwner && m_dataSourceViewHandler) {
            const QString snapshotTool =
                isQuickMeasure
                    ? QStringLiteral("__quick_measurement_confirmation_snapshot")
                    : (isSamMaskEdit
                           ? QStringLiteral("__sam_mask_edit_confirmation_snapshot")
                           : (isSamCategoryEdit
                                  ? QStringLiteral("__sam_category_edit_confirmation_snapshot")
                                  : (isSamPromptCancel
                                         ? QStringLiteral("__sam_prompt_cancel_confirmation_snapshot")
                                         : (isSamSessionSave
                                                ? QStringLiteral("__sam_session_save_confirmation_snapshot")
                                                : QStringLiteral("__sam_session_open_confirmation_snapshot")))));
            snapshot = m_dataSourceViewHandler(snapshotTool, toolCallParams(call));
        }
        QString snapshotKey;
        if (isQuickMeasure)
            snapshotKey = QStringLiteral("quick-measure-call:%1").arg(callIndex);
        else if (isSamMaskEdit)
            snapshotKey = QStringLiteral("sam-mask-edit-call:%1").arg(callIndex);
        else if (isSamCategoryEdit)
            snapshotKey = QStringLiteral("sam-category-edit-call:%1").arg(callIndex);
        else if (isSamPromptCancel)
            snapshotKey = QStringLiteral("sam-prompt-cancel-call:%1").arg(callIndex);
        else if (isSamSessionSave)
            snapshotKey = QStringLiteral("sam-session-save-path:") +
                          QDir::cleanPath(toolCallParams(call).value(QStringLiteral("outputPath")).toString());
        else
            snapshotKey = QStringLiteral("sam-session-open-path:") +
                          QDir::cleanPath(toolCallParams(call).value(QStringLiteral("sessionPath")).toString());
        m_pendingFileTargetPaths.insert(snapshotKey, snapshot);
    }
    for (int callIndex = 0; callIndex < normalizedCalls.size(); ++callIndex) {
        const QJsonObject call = normalizedCalls.at(callIndex).toObject();
        const QString name = toolCallName(call);
        if (name != QLatin1String("create_global_variable") && name != QLatin1String("set_global_variable") &&
            name != QLatin1String("remove_global_variable"))
            continue;
        m_pendingFileTargetPaths.insert(variableSnapshotKey(callIndex),
                                        GlobalVarManager::instance().userVariableSnapshotJson(
                                            toolCallParams(call).value(QStringLiteral("name")).toString().trimmed()));
    }
    QUndoStack* stack = m_actor ? m_actor->undoStack() : nullptr;
    m_pendingUndoIndex = stack ? stack->index() : -1;
    m_pendingUndoCount = stack ? stack->count() : -1;

    m_pendingResumeSnapshotValid = false;
    m_pendingResumeRunId.clear();
    m_pendingResumeSessionId.clear();
    m_pendingResumeState = 0;
    m_pendingResumeLifecycleGeneration = 0;
    for (const QJsonValue& call : normalizedCalls) {
        if (toolCallName(call.toObject()) == QLatin1String("resume_flow")) {
            const RunResumeSnapshot snapshot = RunEngine::instance().resumeSnapshot();
            m_pendingResumeSnapshotValid = true;
            m_pendingResumeRunId = snapshot.runId;
            m_pendingResumeSessionId = snapshot.sessionId;
            m_pendingResumeState = static_cast<int>(snapshot.state);
            m_pendingResumeLifecycleGeneration = snapshot.lifecycleGeneration;
            break;
        }
    }

    transitionTo(AgentState::Confirming);
    if (!content.isNull())
        emit llmResponseReceived(content, normalizedCalls); // 聊天路径;Bridge 路径不发对话消息
    emit toolsPendingConfirmation(normalizedCalls);
}

bool AgentController::confirmationContextStillValid(const QJsonArray& calls, QString* reason) const {
    Project* proj = ProjectManager::instance().currentProject();
    const bool hasProject = (proj != nullptr);
    if (hasProject != m_pendingHasProject) {
        if (reason)
            *reason = hasProject ? QStringLiteral("确认窗口内工程已被切换") : QStringLiteral("确认窗口内工程已被关闭");
        return false;
    }
    if (hasProject && (proj->filePath() != m_pendingProjectPath || proj->name() != m_pendingProjectName)) {
        if (reason)
            *reason = QStringLiteral("确认窗口内工程身份已变化");
        return false;
    }
    if (projectFingerprint(proj) != m_pendingProjectFingerprint) {
        if (reason)
            *reason = QStringLiteral("确认窗口内工程内容已被修改（含 GUI 编辑）");
        return false;
    }
    if (m_pendingDataSourceEpochCaptured &&
        ((proj ? proj->runtimeIdentity() : 0) != m_pendingDataSourceProjectIdentity ||
         (proj && proj->dataSourceCollectionGeneration() != m_pendingDataSourceGeneration))) {
        if (reason)
            *reason = QStringLiteral("确认窗口内工程或数据源集合身份已变化");
        return false;
    }
    if (m_pendingDataSourceViewOwnerCaptured) {
        const QObject* capturedOwner = m_pendingDataSourceViewOwner.data();
        const QObject* currentOwner = m_dataSourceViewOwner.data();
        const quintptr currentIdentity = reinterpret_cast<quintptr>(currentOwner);
        if (m_pendingDataSourceViewOwnerIdentity != currentIdentity || capturedOwner != currentOwner) {
            if (reason)
                *reason = QStringLiteral("确认窗口内 GUI 主窗口已关闭或被替换");
            return false;
        }
    }
    for (int callIndex = 0; callIndex < calls.size(); ++callIndex) {
        const QJsonObject call = calls.at(callIndex).toObject();
        const QString toolName = toolCallName(call);
        const bool isQuickMeasure = toolName == QLatin1String("begin_quick_measurement");
        const bool isSamMaskEdit = toolName == QLatin1String("edit_active_annotation_masks");
        const bool isSamCategoryEdit = toolName == QLatin1String("edit_active_annotation_categories");
        const bool isSamPromptCancel = toolName == QLatin1String("cancel_active_annotation_prompt");
        const bool isSamSessionSave = toolName == QLatin1String("save_active_annotation_session");
        const bool isSamSessionOpen = toolName == QLatin1String("open_active_annotation_session");
        if (!isQuickMeasure && !isSamMaskEdit && !isSamCategoryEdit && !isSamPromptCancel && !isSamSessionSave &&
            !isSamSessionOpen)
            continue;
        if (!m_dataSourceViewOwner || !m_dataSourceViewHandler) {
            if (reason)
                *reason = QStringLiteral("确认窗口内 GUI 主窗口不可用");
            return false;
        }
        QString snapshotKey;
        if (isQuickMeasure)
            snapshotKey = QStringLiteral("quick-measure-call:%1").arg(callIndex);
        else if (isSamMaskEdit)
            snapshotKey = QStringLiteral("sam-mask-edit-call:%1").arg(callIndex);
        else if (isSamCategoryEdit)
            snapshotKey = QStringLiteral("sam-category-edit-call:%1").arg(callIndex);
        else if (isSamPromptCancel)
            snapshotKey = QStringLiteral("sam-prompt-cancel-call:%1").arg(callIndex);
        else if (isSamSessionSave)
            snapshotKey = QStringLiteral("sam-session-save-path:") +
                          QDir::cleanPath(toolCallParams(call).value(QStringLiteral("outputPath")).toString());
        else
            snapshotKey = QStringLiteral("sam-session-open-path:") +
                          QDir::cleanPath(toolCallParams(call).value(QStringLiteral("sessionPath")).toString());
        const QJsonObject approved = m_pendingFileTargetPaths.value(snapshotKey).toObject();
        if (isSamSessionOpen && !approved.value(QStringLiteral("available")).toBool()) {
            if (reason)
                *reason = QStringLiteral("确认期间源 SAM 会话无法保持有效快照，必须重新发起操作");
            return false;
        }
        const QString snapshotTool =
            isQuickMeasure
                ? QStringLiteral("__quick_measurement_confirmation_snapshot")
                : (isSamMaskEdit ? QStringLiteral("__sam_mask_edit_confirmation_snapshot")
                                 : (isSamCategoryEdit
                                        ? QStringLiteral("__sam_category_edit_confirmation_snapshot")
                                        : (isSamPromptCancel
                                               ? QStringLiteral("__sam_prompt_cancel_confirmation_snapshot")
                                               : (isSamSessionSave
                                                      ? QStringLiteral("__sam_session_save_confirmation_snapshot")
                                                      : QStringLiteral("__sam_session_open_confirmation_snapshot")))));
        const QJsonObject current = m_dataSourceViewHandler(snapshotTool, toolCallParams(call));
        if (approved != current) {
            if (reason)
                *reason = isQuickMeasure ? QStringLiteral("确认期间测量目标视口、工程或内容发生变化")
                                         : QStringLiteral("确认期间 SAM 对话框、图像、活动会话或源会话文件发生变化");
            return false;
        }
    }
    if (m_pendingFileTargetsCaptured) {
        if (containsInvalidatedExecutionImageSnapshot(m_pendingFileTargetPaths)) {
            if (reason)
                *reason = QStringLiteral("确认快照期间运行上下文发生变化，必须重新发起操作");
            return false;
        }
        QJsonObject currentTargets = fileTargetPathsForCalls(calls, proj);
        for (int callIndex = 0; callIndex < calls.size(); ++callIndex) {
            const QJsonObject call = calls.at(callIndex).toObject();
            const QString name = toolCallName(call);
            if (name != QLatin1String("create_global_variable") && name != QLatin1String("set_global_variable") &&
                name != QLatin1String("remove_global_variable"))
                continue;
            const QString variableName = toolCallParams(call).value(QStringLiteral("name")).toString().trimmed();
            currentTargets.insert(variableSnapshotKey(callIndex),
                                  GlobalVarManager::instance().userVariableSnapshotJson(variableName));
        }
        for (const QJsonValue& value : calls) {
            const QJsonObject call = value.toObject();
            const QString toolName = toolCallName(call);
            if (toolName != QLatin1String("add_knowledge_document") &&
                toolName != QLatin1String("remove_knowledge_document"))
                continue;
            const QString snapshotKey = QStringLiteral("knowledge-call:") + call.value(QStringLiteral("id")).toString();
            if (!m_pendingFileTargetPaths.value(snapshotKey)
                     .toObject()
                     .value(QStringLiteral("fileIdentityAvailable"))
                     .toBool() ||
                !currentTargets.value(snapshotKey).toObject().value(QStringLiteral("fileIdentityAvailable")).toBool()) {
                if (reason)
                    *reason = QStringLiteral("知识库文档文件身份不可验证，已拒绝确认操作");
                return false;
            }
        }
        for (int callIndex = 0; callIndex < calls.size(); ++callIndex) {
            const QJsonObject call = calls.at(callIndex).toObject();
            if (toolCallName(call) != QLatin1String("import_data_source"))
                continue;
            const QString snapshotKey = QStringLiteral("data-source-import:%1").arg(callIndex);
            const QJsonObject approved = m_pendingFileTargetPaths.value(snapshotKey).toObject();
            const QJsonObject current = currentTargets.value(snapshotKey).toObject();
            if (approved.value(QStringLiteral("readable")).toBool() &&
                (!approved.value(QStringLiteral("fileIdentityAvailable")).toBool() ||
                 !current.value(QStringLiteral("fileIdentityAvailable")).toBool())) {
                if (reason)
                    *reason = QStringLiteral("数据源文件身份不可验证，已拒绝确认操作");
                return false;
            }
        }
        if (containsInvalidatedExecutionImageSnapshot(currentTargets)) {
            if (reason)
                *reason = QStringLiteral("确认复核期间运行上下文发生变化，已拒绝执行");
            return false;
        }
        if (!pluginLifecycleTargetsAreHashable(m_pendingFileTargetPaths) ||
            !pluginLifecycleTargetsAreHashable(currentTargets)) {
            if (reason)
                *reason = QStringLiteral("确认窗口内插件 metadata 或候选二进制无法计算 SHA-256，已拒绝执行");
            return false;
        }
        if (currentTargets != m_pendingFileTargetPaths) {
            if (reason)
                *reason = QStringLiteral(
                    "确认窗口内工程候选/相机目标/会话、插件目标/加载状态、文件输出或 SAM 输入图像目标/内容已变化");
            return false;
        }
    }
    if (m_pendingResumeSnapshotValid) {
        const RunResumeSnapshot current = RunEngine::instance().resumeSnapshot();
        if (current.runId != m_pendingResumeRunId || current.sessionId != m_pendingResumeSessionId ||
            static_cast<int>(current.state) != m_pendingResumeState ||
            current.lifecycleGeneration != m_pendingResumeLifecycleGeneration) {
            if (reason)
                *reason = QStringLiteral("确认窗口内暂停运行会话或位置已变化");
            return false;
        }
    }
    QUndoStack* stack = m_actor ? m_actor->undoStack() : nullptr;
    const int index = stack ? stack->index() : -1;
    const int count = stack ? stack->count() : -1;
    if (index != m_pendingUndoIndex || count != m_pendingUndoCount) {
        if (reason)
            *reason = QStringLiteral("确认窗口内 Agent 撤销栈已变化");
        return false;
    }
    return true;
}

QJsonObject AgentController::pendingConfirmationPayload() const {
    if (m_pendingToolCalls.isEmpty())
        return {};
    QStringList summaryLines;
    QJsonArray sideEffects;
    bool undoableIfSuccessful = true;
    bool implicitProjectSaveDisclosed = false;
    for (int callIndex = 0; callIndex < m_pendingToolCalls.size(); ++callIndex) {
        const QJsonValue& v = m_pendingToolCalls.at(callIndex);
        const QJsonObject tc = v.toObject();
        const QString name = toolCallName(tc);
        const QJsonObject params = toolCallParams(tc);
        undoableIfSuccessful = undoableIfSuccessful && isUndoCapableToolName(name);
        if (name == QLatin1String("sam_predict")) {
            const QString serverUrl =
                params.value(QStringLiteral("serverUrl")).toString(QStringLiteral("http://127.0.0.1:8990"));
            summaryLines << QStringLiteral("sam_predict server=%1").arg(safeSamServerOrigin(serverUrl));
        } else if (name == QLatin1String("sam_annotate")) {
            const QString imagePath = params.value(QStringLiteral("imagePath")).toString(QStringLiteral("<未配置>"));
            const QString sessionPath =
                params.value(QStringLiteral("sessionPath")).toString(QStringLiteral("<未配置>"));
            const QString serverUrl =
                params.value(QStringLiteral("serverUrl")).toString(QStringLiteral("http://127.0.0.1:8990"));
            const int promptCount = params.value(QStringLiteral("items")).isArray()
                                        ? params.value(QStringLiteral("items")).toArray().size()
                                        : 0;
            summaryLines << QStringLiteral("sam_annotate image=%1 session=%2 server=%3 prompts=%4")
                                .arg(imagePath, sessionPath, safeSamServerOrigin(serverUrl))
                                .arg(promptCount);
        } else if (name == QLatin1String("export_annotation_session")) {
            summaryLines << QStringLiteral("导出标注 session=%1 format=%2 new_target=%3 categories=%4")
                                .arg(params.value(QStringLiteral("sessionPath")).toString(),
                                     params.value(QStringLiteral("format")).toString(),
                                     params.value(QStringLiteral("outputPath")).toString())
                                .arg(params.value(QStringLiteral("categories")).toArray().size());
        } else if (name == QLatin1String("transform_annotation_session")) {
            const QString label = params.value(QStringLiteral("label")).toString().trimmed();
            summaryLines << QStringLiteral("变换标注会话副本 session=%1 operation=%2 objects=%3 label=%4 new_target=%5")
                                .arg(params.value(QStringLiteral("sessionPath")).toString(),
                                     params.value(QStringLiteral("operation")).toString())
                                .arg(params.value(QStringLiteral("objectIds")).toArray().size())
                                .arg(label.isEmpty() ? QStringLiteral("<不适用>") : label,
                                     params.value(QStringLiteral("outputPath")).toString());
        } else if (name == QLatin1String("add_knowledge_document")) {
            const QJsonObject snapshot =
                m_pendingFileTargetPaths
                    .value(QStringLiteral("knowledge-call:") + tc.value(QStringLiteral("id")).toString())
                    .toObject();
            const QString source = snapshot.value(QStringLiteral("path")).toString();
            summaryLines << QStringLiteral("导入知识库文档 source=%1 target=%2")
                                .arg(source, snapshot.value(QStringLiteral("targetName")).toString());
        } else if (name == QLatin1String("remove_knowledge_document")) {
            const QString documentId = params.value(QStringLiteral("documentId")).toString();
            const QJsonObject snapshot =
                m_pendingFileTargetPaths
                    .value(QStringLiteral("knowledge-call:") + tc.value(QStringLiteral("id")).toString())
                    .toObject();
            summaryLines << QStringLiteral("从知识库移除文档 %1 target=%2")
                                .arg(documentId.mid(QStringLiteral("user:").size()),
                                     snapshot.value(QStringLiteral("path")).toString());
        } else if (name == QLatin1String("import_data_source")) {
            const QJsonObject snapshot =
                m_pendingFileTargetPaths.value(QStringLiteral("data-source-import:%1").arg(callIndex)).toObject();
            summaryLines << QStringLiteral("验证并登记数据源引用 path=%1；不显示、不修改源文件")
                                .arg(snapshot.value(QStringLiteral("resolvedPath"))
                                         .toString(params.value(QStringLiteral("path")).toString()));
        } else if (name == QLatin1String("create_global_variable")) {
            const QString value = QString::fromUtf8(
                QJsonDocument(QJsonArray{params.value(QStringLiteral("value"))}).toJson(QJsonDocument::Compact));
            summaryLines << QStringLiteral("创建全局变量 %1 type=%2 value=%3")
                                .arg(params.value(QStringLiteral("name")).toString(),
                                     params.value(QStringLiteral("type")).toString(), value.mid(1, value.size() - 2));
        } else if (name == QLatin1String("set_global_variable")) {
            const QString value = QString::fromUtf8(
                QJsonDocument(QJsonArray{params.value(QStringLiteral("value"))}).toJson(QJsonDocument::Compact));
            const QJsonObject snapshot = m_pendingFileTargetPaths.value(variableSnapshotKey(callIndex)).toObject();
            summaryLines << QStringLiteral("设置全局变量 %1 type=%2 value=%3；保留现有 expression/link 元数据")
                                .arg(params.value(QStringLiteral("name")).toString(),
                                     snapshot.value(QStringLiteral("type")).toString(), value.mid(1, value.size() - 2));
        } else if (name == QLatin1String("remove_global_variable")) {
            const QJsonObject snapshot = m_pendingFileTargetPaths.value(variableSnapshotKey(callIndex)).toObject();
            summaryLines << QStringLiteral("移除全局变量 %1 type=%2")
                                .arg(params.value(QStringLiteral("name")).toString(),
                                     snapshot.value(QStringLiteral("type")).toString());
        } else if (name == QLatin1String("display_data_source")) {
            summaryLines << QStringLiteral("在主视图显示数据源 %1 (只读源文件)")
                                .arg(params.value(QStringLiteral("dataSourceId")).toString());
        } else if (name == QLatin1String("display_data_source_in_dedicated_viewport")) {
            summaryLines << QStringLiteral("在专用视口显示数据源 %1 (只读源文件，不改变主视图)")
                                .arg(params.value(QStringLiteral("dataSourceId")).toString());
        } else if (name == QLatin1String("open_data_source_folder_for_user")) {
            summaryLines << QStringLiteral("请求桌面文件管理器显示数据源 %1 的所在目录；不返回目录路径")
                                .arg(params.value(QStringLiteral("dataSourceId")).toString());
        } else if (name == QLatin1String("open_user_knowledge_folder_for_user")) {
            summaryLines << QStringLiteral("请求桌面文件管理器打开用户知识库目录；不显示目录路径或文档信息");
        } else if (name == QLatin1String("copy_data_source_path_to_clipboard")) {
            QString path = QStringLiteral("<路径不可用>");
            if (const Project* project = ProjectManager::instance().currentProject()) {
                int matches = 0;
                for (const DataSource& source : project->dataSources()) {
                    if (source.id == params.value(QStringLiteral("dataSourceId")).toString()) {
                        ++matches;
                        path = source.filePath;
                    }
                }
                if (matches > 1)
                    path = QStringLiteral("<数据源 ID 不唯一>");
                else if (matches == 0 || path.isEmpty())
                    path = QStringLiteral("<路径不可用>");
            }
            summaryLines << QStringLiteral("复制数据源 %1 的路径 %2 到系统剪贴板 (将替换剪贴板内容)")
                                .arg(params.value(QStringLiteral("dataSourceId")).toString(), path);
        } else if (name == QLatin1String("begin_quick_measurement")) {
            summaryLines << QStringLiteral("在人机协作视口 %1 中开始 %2 %3；用户按步骤手动采点，不修改工程或文件")
                                .arg(params.value(QStringLiteral("viewportId")).toString(),
                                     params.value(QStringLiteral("dimension")).toString(),
                                     params.value(QStringLiteral("measurementType")).toString());
        } else if (name == QLatin1String("edit_active_annotation_masks")) {
            const QString operation = params.value(QStringLiteral("operation")).toString();
            const QJsonArray strokePoints = params.value(QStringLiteral("strokePoints")).toArray();
            summaryLines << QStringLiteral("编辑活动 SAM 会话 operation=%1 objects=%2 radiusPx=%3 strokePointCount=%4；"
                                           "不改工程或磁盘文件")
                                .arg(operation,
                                     QString::number(params.value(QStringLiteral("objectIds")).toArray().size()),
                                     params.contains(QStringLiteral("radiusPx"))
                                         ? QString::number(params.value(QStringLiteral("radiusPx")).toInt())
                                         : QStringLiteral("<不适用>"),
                                     QString::number(strokePoints.size()));
        } else if (name == QLatin1String("edit_active_annotation_categories")) {
            const QString operation = params.value(QStringLiteral("operation")).toString();
            if (operation == QLatin1String("set_object_label")) {
                summaryLines << QStringLiteral(
                                    "修改活动 SAM 对象类别 label=%1 objects=%2；保留其他对象字段并可在 GUI 撤销")
                                    .arg(params.value(QStringLiteral("label")).toString(),
                                         QString::number(params.value(QStringLiteral("objectIds")).toArray().size()));
            } else {
                summaryLines << QStringLiteral("%1 活动 SAM 临时类别“%2”；不修改已有对象或磁盘会话")
                                    .arg(operation == QLatin1String("add_category") ? QStringLiteral("添加")
                                                                                    : QStringLiteral("移除"),
                                         params.value(QStringLiteral("category")).toString());
            }
        } else if (name == QLatin1String("cancel_active_annotation_prompt")) {
            summaryLines << QStringLiteral(
                "取消活动 SAM 对话框中的临时点/框提示、预测预览、候选及当前预测请求；保留已确认标注和 GUI 撤销/重做");
        } else if (name == QLatin1String("save_active_annotation_session")) {
            const QString pathKey = QStringLiteral("sam-session-save-path:") +
                                    QDir::cleanPath(params.value(QStringLiteral("outputPath")).toString());
            const QJsonObject snapshot = m_pendingFileTargetPaths.value(pathKey).toObject();
            const QJsonObject target = snapshot.value(QStringLiteral("outputTarget")).toObject();
            const QString resolvedPath = QDir(target.value(QStringLiteral("outputCanonicalParent")).toString())
                                             .filePath(target.value(QStringLiteral("outputFileName")).toString());
            summaryLines << QStringLiteral(
                                "创建新的活动 SAM 会话文件 %1，包含 %2 个已确认对象的几何/掩膜和图像路径元数据；"
                                "不覆盖现有文件")
                                .arg(QDir::cleanPath(resolvedPath))
                                .arg(snapshot.value(QStringLiteral("objectCount")).toInt());
        } else if (name == QLatin1String("open_active_annotation_session")) {
            const QString pathKey = QStringLiteral("sam-session-open-path:") +
                                    QDir::cleanPath(params.value(QStringLiteral("sessionPath")).toString());
            const QJsonObject snapshot = m_pendingFileTargetPaths.value(pathKey).toObject();
            const QString binding = snapshot.value(QStringLiteral("imageBound")).toBool()
                                        ? QStringLiteral("会话绑定图像 %1×%2")
                                              .arg(snapshot.value(QStringLiteral("imageWidth")).toInt())
                                              .arg(snapshot.value(QStringLiteral("imageHeight")).toInt())
                                        : QStringLiteral("未绑定图像，保留当前画面");
            summaryLines << QStringLiteral("在活动 SAM 窗口打开 %1 个对象的会话 %2（%3）；替换当前会话并清除 "
                                           "Undo/Redo、未确认点/框提示、预测预览和候选；会创建 SAM 后端临时图像快照，"
                                           "快照在被替换或对话框销毁时自动删除")
                                .arg(snapshot.value(QStringLiteral("objectCount")).toInt())
                                .arg(snapshot.value(QStringLiteral("sessionPath")).toString(), binding);
            if (snapshot.value(QStringLiteral("legacyImageBindingUnverified")).toBool())
                summaryLines << QStringLiteral("警告：旧会话无像素摘要，无法验证图像内容历史");
        } else if (name == QLatin1String("set_data_source_visibility")) {
            summaryLines << QStringLiteral("设置数据源 %1 可见=%2 (GUI 临时状态)")
                                .arg(params.value(QStringLiteral("dataSourceId")).toString(),
                                     params.value(QStringLiteral("visible")).toBool() ? QStringLiteral("true")
                                                                                      : QStringLiteral("false"));
        } else {
            summaryLines << QStringLiteral("%1 %2").arg(name,
                                                        QString(QJsonDocument(params).toJson(QJsonDocument::Compact)));
        }
        const QString directTarget =
            m_pendingFileTargetPaths.value(QStringLiteral("call:") + tc.value(QStringLiteral("id")).toString())
                .toString();
        const QJsonObject imageSnapshot =
            m_pendingFileTargetPaths.value(QStringLiteral("sam-image:") + tc.value(QStringLiteral("id")).toString())
                .toObject();
        const QJsonObject cameraSnapshot =
            m_pendingFileTargetPaths.value(QStringLiteral("camera-call:") + tc.value(QStringLiteral("id")).toString())
                .toObject();
        const QJsonObject projectInputSnapshot =
            m_pendingFileTargetPaths.value(QStringLiteral("project-input:") + QString::number(callIndex)).toObject();
        const QJsonObject knowledgeSnapshot =
            m_pendingFileTargetPaths
                .value(QStringLiteral("knowledge-call:") + tc.value(QStringLiteral("id")).toString())
                .toObject();
        const QJsonObject dataSourceImportSnapshot =
            m_pendingFileTargetPaths.value(QStringLiteral("data-source-import:%1").arg(callIndex)).toObject();
        const QStringList effects =
            sideEffectsForToolCall(name, params, m_pendingFileTargetPaths, directTarget, imageSnapshot, cameraSnapshot,
                                   projectInputSnapshot, knowledgeSnapshot, dataSourceImportSnapshot);
        for (const QString& effect : effects) {
            const bool implicitProjectSave =
                effect.contains(QLatin1String("(implicit current project save before switch)"));
            if (implicitProjectSave && implicitProjectSaveDisclosed)
                continue;
            if (implicitProjectSave)
                implicitProjectSaveDisclosed = true;
            sideEffects.append(effect);
        }
    }
    QJsonObject context{
        {"summary", summaryLines.join(QStringLiteral("; "))},
        {"projectName", m_pendingProjectName},
        {"projectPath", m_pendingProjectPath},
        {"projectVersion", QJsonObject{{"filePath", m_pendingProjectPath},
                                       {"contentFingerprint", QString::fromLatin1(m_pendingProjectFingerprint)},
                                       {"undoIndex", m_pendingUndoIndex},
                                       {"undoCount", m_pendingUndoCount}}},
        {"sideEffects", sideEffects}};
    if (!undoableIfSuccessful)
        context.insert(QStringLiteral("reversibilityLabel"), QStringLiteral("不可撤销：Agent Undo 不会回滚外部副作用"));
    if (m_pendingResumeSnapshotValid) {
        context.insert(
            QStringLiteral("executionSnapshot"),
            QJsonObject{{QStringLiteral("runId"), m_pendingResumeRunId},
                        {QStringLiteral("sessionId"), m_pendingResumeSessionId},
                        {QStringLiteral("state"), m_pendingResumeState},
                        {QStringLiteral("pauseEpoch"), QString::number(m_pendingResumeLifecycleGeneration)}});
    }
    return QJsonObject{{"confirmationId", m_pendingConfirmationId},
                       {"tools", pendingToolCallsForUiAndBridge(m_pendingToolCalls)},
                       {"context", context}};
}

void AgentController::abortCurrentTurn() {
    const AgentState previous = state();
    if (previous == AgentState::Idle)
        return;
    ++m_turnGeneration;
    ++m_waitGeneration;
    cancelPendingQueries(QStringLiteral("user_cancelled"));
    if (m_llmClient && !m_activeLLMRequestId.isEmpty())
        m_llmClient->cancelRequest();
    m_activeLLMRequestId.clear();
    if (m_actor)
        m_actor->abortRunningTool();
    if (m_streamActive) {
        m_streamActive = false;
        emit agentStreamAborted();
    }
    if (previous == AgentState::Confirming) {
        if (m_dispatchQueued)
            endTurnWithReason(QStringLiteral("Stopped by user."), {}, true);
        else
            rejectPendingTools(m_pendingConfirmationId);
    } else {
        if (previous == AgentState::Executing && !m_pendingToolCalls.isEmpty() && m_continueAfterPendingTools)
            appendToolResultsForCalls(
                m_pendingToolCalls,
                QJsonObject{{QStringLiteral("errorCode"), QStringLiteral("USER_CANCELLED")},
                            {QStringLiteral("error"), QStringLiteral("Tool execution cancelled by user")},
                            {QStringLiteral("retryable"), false}});
        m_pendingToolCalls = QJsonArray();
        m_pendingFileTargetPaths = QJsonObject();
        m_pendingFileTargetsCaptured = false;
        transitionTo(AgentState::Idle);
        if (previous == AgentState::Thinking)
            emit llmResponseReceived(QStringLiteral("Stopped by user."), {});
    }
    emit agentTurnAborted();
}

void AgentController::flushStreamPending() {
    if (m_streamFlushTimer)
        m_streamFlushTimer->stop();
    if (!m_pendingContent.isEmpty()) {
        const QString text = m_pendingContent;
        m_pendingContent.clear();
        emit agentStreamContentDelta(text);
    }
    if (!m_pendingReasoning.isEmpty()) {
        const QString text = m_pendingReasoning;
        m_pendingReasoning.clear();
        emit agentStreamReasoningDelta(text);
    }
}

void AgentController::resetStreamBuffer() {
    if (m_streamFlushTimer)
        m_streamFlushTimer->stop();
    m_pendingContent.clear();
    m_pendingReasoning.clear();
    m_streamActive = false;
}

void AgentController::onStreamStarted() {
    m_streamActive = true;
    m_pendingContent.clear();
    m_pendingReasoning.clear();
    emit agentStreamStarted();
}

void AgentController::onStreamContentDelta(const QString& text) {
    if (text.isEmpty())
        return;
    m_pendingContent += text;
    if (m_streamFlushTimer && !m_streamFlushTimer->isActive())
        m_streamFlushTimer->start();
}

void AgentController::onStreamReasoningDelta(const QString& text) {
    if (text.isEmpty())
        return;
    m_pendingReasoning += text;
    if (m_streamFlushTimer && !m_streamFlushTimer->isActive())
        m_streamFlushTimer->start();
}

void AgentController::onStreamFinished() {
    const QString reasoning = m_pendingReasoning;
    flushStreamPending();
    if (!m_streamActive)
        return;
    m_streamActive = false;
    m_lastResponseReasoning = reasoning;
    emit agentStreamFinished();
}

void AgentController::onStreamFailed() {
    flushStreamPending();
    m_streamActive = false;
}

void AgentController::onStreamFallback(const QString& reason) {
    flushStreamPending();
    m_streamActive = false;
    Logger::instance().addLog(QStringLiteral("[AgentStream] fallback: %1").arg(reason), LogLevel::Info, "Agent");
    AgentActionLogEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.actor = QStringLiteral("System");
    entry.action = QStringLiteral("LLM stream fallback");
    entry.params = reason;
    entry.result = QStringLiteral("success");
    entry.undoable = false;
    emit actionLogEntryAdded(entry);
    emit agentStreamFallback(reason);
}

void AgentController::onLLMErrorClassified(const LLMErrorInfo& info) {
    m_lastErrorCategory = llmErrorCategoryName(info.category);
}

void AgentController::onLLMErrorClassifiedForRequest(const QString& requestId, const LLMErrorInfo& info) {
    if (!requestId.isEmpty() && requestId == m_activeLLMRequestId)
        onLLMErrorClassified(info);
}

void AgentController::onLLMError(const QString& error) {
    m_activeLLMRequestId.clear();
    m_parameterCorrectionRetries = 0;
    m_parameterFailureSignatures.clear();
    m_pendingToolCalls = QJsonArray();
    m_pendingFileTargetPaths = QJsonObject();
    m_pendingFileTargetsCaptured = false;
    cancelPendingQueries(QStringLiteral("llm_error"));
    ++m_waitGeneration;
    transitionTo(AgentState::Idle);
    emit llmErrorOccurred(error);
    m_lastErrorCategory.clear();
    m_turnTimer.invalidate();
}

void AgentController::onLLMRequestFailed(const QString& requestId, const QString& error) {
    if (requestId.isEmpty() || requestId != m_activeLLMRequestId)
        return;
    onLLMError(error);
}

QString AgentController::defaultSystemPrompt() {
    return QString(
        "You are DeepLux Agent, an AI assistant embedded in DeepLux Vision "
        "(an industrial machine vision software).\n\n"
        "Your tool-enabled capabilities:\n"
        "- Create and manage vision inspection projects\n"
        "- Add/remove/configure image processing modules\n"
        "- Connect modules into execution flows, run flows, interpret results, and inspect the viewport with "
        "capture_viewport_image\n\n"
        "Knowledge Base: When tools are available, use read_documentation(topic) to learn about modules and "
        "workflows.\n"
        "Topics: module names, 'workflow', 'params', 'all'.\n\n"
        "Critical rules:\n"
        "1. When tools are available, if get_flow_state returns \"No project opened\", you MUST call "
        "create_project first.\n"
        "   Never call get_flow_state again without creating a project — it will keep returning the same error.\n"
        "2. When tools are available, use them for actions and facts that require current application state; never "
        "describe pseudo-code as if it had been executed.\n"
        "3. When you have completed all the user's requests, respond in plain text (no more tool calls).\n"
        "4. If the user only asked a question or gave a simple instruction that is already fulfilled, do NOT call "
        "additional tools just to verify — respond directly.\n"
        "5. When tools are available, use read_documentation when unsure about module parameters.\n"
        "6. Before running a flow you just modified, call validate_workflow. If ok=false, fix the reported issues "
        "instead of running.\n"
        "7. run_flow wait defaults to true for a single run; use wait=false to return immediately with runId, then "
        "wait_for_run for completion. Cycle mode always returns immediately.\n"
        "   After a run, use get_node_runtime_outputs or get_execution_logs to inspect per-node results and "
        "diagnostics before explaining failures.\n"
        "   At most 2 tool images are attached per turn.\n"
        "8. When tools are available, for quick measurement, list viewports and begin one exact request. Explain its "
        "listed manual pick/drag "
        "steps and then wait for the user; query only after the user reports the picks are done. Use the same "
        "requestId, cancel only when asked, and never claim a numeric result that the status tool does not return.\n"
        "9. When tools are available and a tool returns INVALID_PARAM or MISSING_PARAM, correct the parameters using "
        "its schema and error "
        "details. Never repeat an unchanged failing call; permission denials, user rejections, and uncertain side "
        "effects are final and must not be retried. The controller allows at most three parameter corrections.\n"
        "10. Be concise. Industrial users prefer direct answers.\n");
}

} // namespace DeepLux
