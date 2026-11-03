#include "ToolSchema.h"

namespace DeepLux {

QJsonObject ToolParam::toJson() const {
    QJsonObject obj;
    // OpenAI/DeepSeek JSON Schema: type must be "string", "number", "boolean", "object", "array", "null".
    // "enum" is NOT a valid type. If enumValues is present, type should be "string".
    if (type == QLatin1String("json")) {
        QJsonArray alternatives;
        for (const QString& kind : {QStringLiteral("string"), QStringLiteral("number"), QStringLiteral("boolean"),
                                    QStringLiteral("array"), QStringLiteral("object"), QStringLiteral("null")}) {
            QJsonObject schema{{"type", kind}};
            if (kind == QLatin1String("array"))
                schema.insert(QStringLiteral("items"), QJsonObject());
            if (kind == QLatin1String("object"))
                schema.insert(QStringLiteral("additionalProperties"), true);
            alternatives.append(schema);
        }
        obj["anyOf"] = alternatives;
    } else if (type == QLatin1String("string_array")) {
        obj["type"] = "array";
        obj["items"] = QJsonObject{{"type", "string"}};
    } else if (type == QLatin1String("array") && name == QLatin1String("strokePoints")) {
        const QJsonObject pointSchema{
            {"type", "array"}, {"minItems", 2}, {"maxItems", 2}, {"items", QJsonObject{{"type", "number"}}}};
        obj["type"] = "array";
        obj["minItems"] = 1;
        obj["maxItems"] = 4096;
        obj["items"] = pointSchema;
    } else if (!enumValues.isEmpty()) {
        obj["type"] = "string";
    } else {
        obj["type"] = type;
    }
    obj["description"] = description;
    if (!defaultValue.isUndefined() && !defaultValue.isNull()) {
        obj["default"] = defaultValue;
    }
    if (!enumValues.isEmpty()) {
        QJsonArray arr;
        for (const QString& v : enumValues)
            arr.append(v);
        obj["enum"] = arr;
    }
    return obj;
}

QJsonObject ToolDefinition::toOpenAIFunction() const {
    QJsonObject function;
    function["name"] = name;
    function["description"] = description;

    QJsonObject properties;
    QJsonArray required;
    for (const ToolParam& p : params) {
        properties[p.name] = p.toJson();
        if (p.required) {
            required.append(p.name);
        }
    }

    QJsonObject parameters;
    parameters["type"] = "object";
    parameters["properties"] = properties;
    if (!required.isEmpty()) {
        parameters["required"] = required;
    }
    function["parameters"] = parameters;

    QJsonObject result;
    result["type"] = "function";
    result["function"] = function;
    return result;
}

QJsonObject ToolDefinition::toClaudeTool() const {
    // Claude Tool Use format is similar to OpenAI
    return toOpenAIFunction();
}

QJsonObject ToolDefinition::toJsonSchema() const {
    QJsonObject schema;
    schema["name"] = name;
    schema["description"] = description;
    schema["dangerous"] = dangerous;

    QJsonArray paramArray;
    for (const ToolParam& p : params) {
        QJsonObject obj = p.toJson();
        obj["name"] = p.name;
        obj["required"] = p.required;
        paramArray.append(obj);
    }
    schema["params"] = paramArray;
    return schema;
}

ToolSchema& ToolSchema::instance() {
    static ToolSchema instance;
    return instance;
}

void ToolSchema::registerTool(const ToolDefinition& tool) {
    // Replace if already exists
    for (int i = 0; i < m_tools.size(); ++i) {
        if (m_tools[i].name == tool.name) {
            m_tools[i] = tool;
            return;
        }
    }
    m_tools.append(tool);
}

QList<ToolDefinition> ToolSchema::allTools() const {
    return m_tools;
}

ToolDefinition ToolSchema::findTool(const QString& name) const {
    for (const ToolDefinition& t : m_tools) {
        if (t.name == name)
            return t;
    }
    return ToolDefinition();
}

bool ToolSchema::hasTool(const QString& name) const {
    for (const ToolDefinition& t : m_tools) {
        if (t.name == name)
            return true;
    }
    return false;
}

QJsonArray ToolSchema::toOpenAIFunctions() const {
    QJsonArray arr;
    for (const ToolDefinition& t : m_tools) {
        arr.append(t.toOpenAIFunction());
    }
    return arr;
}

QJsonArray ToolSchema::toClaudeTools() const {
    QJsonArray arr;
    for (const ToolDefinition& t : m_tools) {
        arr.append(t.toClaudeTool());
    }
    return arr;
}

void ToolSchema::registerDefaultTools() {
    // create_project
    registerTool({"create_project",
                  "Create a new project with the given name. If the current project has unsaved changes "
                  "the call fails with PROJECT_UNSAVED unless onUnsaved is given: 'save' saves first "
                  "(aborts on save failure), 'discard' explicitly drops changes, 'cancel' (default) "
                  "keeps the current project.",
                  {{"name", "string", "Project name", true},
                   {"onUnsaved",
                    "enum",
                    "Unsaved-changes policy: save | discard | cancel",
                    false,
                    QJsonValue("cancel"),
                    {"save", "discard", "cancel"}}}});

    registerTool({"list_recent_projects",
                  "List the application's recent project paths in their existing order. Reports only path, file name, "
                  "and current file existence/type; does not load or open projects. Use open_project separately to "
                  "open one under its existing permission gate, onUnsaved policy, and candidate validation; it asks "
                  "for confirmation only when its existing gate requires it.",
                  {}});

    registerTool({"set_theme",
                  "Set the application's display theme to an explicit target. Advisor calls require the standard "
                  "confirmation; Autopilot may apply it directly. The choice is persisted when configuration is "
                  "initialized.",
                  {{"theme", "string", "Theme to apply", true, QJsonValue(), {"light", "dark"}}}});

    registerTool({"open_system_settings",
                  "Open the existing system settings dialog for the user. This only requests the dialog to be shown; "
                  "the user must review, change, save, or reset settings manually. The Agent does not read or change "
                  "settings values, and opening the dialog does not mean anything was saved.",
                  {}});
    registerTool({"open_global_variables_dialog",
                  "Requires a logged-in user with GlobalVar permission. Open or reuse the global variables window for "
                  "the user. The user may inspect and operate the "
                  "existing controls manually. This navigation does not select variables, reorder rows, execute "
                  "expressions, apply changes, or expose variable data in the response or audit.",
                  {}});
    registerTool({"open_sam_environment_setup",
                  "Open or reuse the SAM setup dialog and report a point-in-time readiness snapshot. This is a GUI "
                  "handoff only: it does not initialize Python, import weights, query or start the server, or run a "
                  "prediction. The user performs setup actions in the dialog. Paths and credentials are never "
                  "returned; repeat the call after the user changes setup to refresh the snapshot.",
                  {}});
    registerTool({"open_sam_annotator_dialog",
                  "Open or reuse the SAM annotation window for the user. This only hands off the GUI; it does not "
                  "prepare the backend, initialize Python, start or query a server, load a model, or run a "
                  "prediction. An existing annotation image/session is preserved. If no session or viewport image "
                  "is available, the tool returns NO_IMAGE. The response does not include image, session, or path "
                  "data.",
                  {}});
    registerTool({"open_sam_image_picker_for_user",
                  "Open or reuse the SAM annotation window and show its existing Open Image control. The user must "
                  "choose a file manually. This does not select or read an image, replace existing annotation "
                  "state, start a backend, or run a prediction. The response does not include image, session, or "
                  "path data.",
                  {}});
    registerTool({"open_user_login_dialog",
                  "Open or reuse the user login dialog. The user must enter and submit credentials manually; the "
                  "Agent never reads or submits them. This only requests the dialog to be shown and does not mean "
                  "the user is logged in or authenticated.",
                  {}});
    registerTool({"open_agent_settings_dialog",
                  "Open or reuse the Agent settings dialog for the user. The user reviews and edits settings, then "
                  "chooses OK or Cancel manually. Opening does not save settings or test the connection. Agent does "
                  "not read or return the API key, endpoint, permission level, system prompt, or setting values.",
                  {}});
    registerTool({"open_about_dialog",
                  "Open or reuse the DeepLux About window so the user can view product information. This only opens "
                  "the window; it does not check licensing, updates, connectivity, or installation status.",
                  {}});
    registerTool({"open_knowledge_base_dialog",
                  "Open or reuse the knowledge base management window. The user reviews documents and manually "
                  "chooses whether to add or remove them. Opening may initialize an empty user knowledge directory; "
                  "the Agent does not read or return document contents through this navigation action.",
                  {}});

    registerTool({"set_inspector_pinned",
                  "Set whether the current GUI module inspector stays pinned to its selected module. This is an "
                  "explicit view-state setting and does not require a project.",
                  {{"pinned", "boolean", "Whether the inspector is pinned", true}}});
    registerTool({"set_inspector_collapsed",
                  "Set the inspector's collapsed state. Does not require a project or change inspector visibility.",
                  {{"collapsed", "boolean", "Whether the inspector is collapsed", true}}});
    registerTool({"set_inspector_visibility",
                  "Set the inspector's visibility. Closing clears its displayed content but preserves pin and flow "
                  "selection; reopening restores the current selected module. Does not require a project.",
                  {{"visible", "boolean", "Whether the inspector is visible", true}}});

    registerTool({"set_primary_viewport_zoom",
                  "Apply one explicit zoom action to the primary 2D image viewport. Requires an active 2D image; "
                  "does not control dedicated data-source viewports or 3D views.",
                  {{"action", "string", "Zoom action", true, QJsonValue(), {"zoom_in", "zoom_out", "fit", "actual"}}}});

    registerTool({"set_primary_viewport_point_size",
                  "Set the primary 3D point cloud viewport point size from 1.0 to 8.0 with at most one decimal "
                  "place. Does not control dedicated viewports.",
                  {{"size", "number", "Point size in the inclusive range 1.0–8.0, at most one decimal place", true}}});

    registerTool({"set_primary_viewport_display_mode",
                  "Set the primary data-source viewport to 2D or 3D. Switching requires valid cached image and "
                  "non-empty point cloud data; setting the already active mode is idempotent.",
                  {{"mode", "string", "Target primary viewport mode", true, QJsonValue(), {"2d", "3d"}}}});

    registerTool({"set_point_cloud_render_mode",
                  "Set the 3D point cloud render mode using the same available modes and global action as the View "
                  "menu.",
                  {{"mode",
                    "string",
                    "Render mode",
                    true,
                    QJsonValue(),
                    {"uniform", "rgb", "height", "intensity", "normal", "blinn_phong"}}}});

    registerTool({"close_viewport",
                  "Close one exact viewport from the current GUI. The primary or only viewport is cleared and "
                  "retained; an additional viewport is destroyed. Does not change project data or source files.",
                  {{"viewportId", "string", "Exact viewport ID from the current view state", true}}});

    registerTool({"save_viewport_snapshot_for_user",
                  "Request a snapshot save for one exact viewport. The user chooses the destination and confirms "
                  "saving in the native file dialog; until then no file is written. The result and audit never include "
                  "the chosen path, filename, or image content.",
                  {{"viewportId", "string", "Exact viewport ID from list_viewports", true}},
                  true});

    registerTool({"list_viewports",
                  "List current GUI viewports by exact ID, mode, visibility, and displayed image/point-cloud content. "
                  "Does not require an open project.",
                  {}});
    registerTool({"begin_quick_measurement",
                  "Begin one human-assisted measurement in an exact compatible viewport. The user must perform "
                  "the listed image-coordinate clicks/ROI drag or point-cloud picks; query the returned requestId "
                  "and do not claim numeric results.",
                  {{"viewportId", "string", "Exact viewport ID from list_viewports", true},
                   {"dimension", "string", "Coordinate space to pick in", true, QJsonValue(), {"2d", "3d"}},
                   {"measurementType",
                    "string",
                    "Measurement type",
                    true,
                    QJsonValue(),
                    {"distance_2_points", "point_to_line", "segment_gap", "angle_3_points", "circle_3_points",
                     "rectangle_area"}}}});
    registerTool({"get_quick_measurement_status",
                  "Read progress for the exact human-assisted measurement request. Terminal states are immutable; "
                  "the latest terminal result remains queryable after its GUI window closes. Picked 2D coordinates "
                  "are labeled ImageLocalPixel and include SourceImagePixel only when the displayed image carries a "
                  "verified source mapping; do not treat missing source coordinates as available.",
                  {{"requestId", "string", "Exact requestId returned by begin_quick_measurement", true}}});
    registerTool({"cancel_quick_measurement",
                  "Cancel the exact pending human-assisted measurement and clear its partial overlay. Does not "
                  "change project data or source files.",
                  {{"requestId", "string", "Exact requestId returned by begin_quick_measurement", true}}});

    registerTool({"copy_data_source_path_to_clipboard",
                  "Copy one exact registered data-source path to the system clipboard. This replaces the current "
                  "clipboard contents and requires confirmation; the source file and project are unchanged.",
                  {{"dataSourceId", "string", "Exact data source ID from list_data_sources", true}},
                  true});

    registerTool({"open_data_source_folder_for_user",
                  "After confirmation, request that the desktop file manager show the parent folder of one exact "
                  "registered data source. The response and audit never include its path or file name.",
                  {{"dataSourceId", "string", "Exact data source ID from list_data_sources", true}},
                  true});

    registerTool({"open_user_knowledge_folder_for_user",
                  "After confirmation, request that the desktop file manager open the user's knowledge base folder. "
                  "The response and audit never include the folder path or document information.",
                  {},
                  true});

    // add_module
    registerTool({"add_module",
                  "Add a module (plugin) to the current project",
                  {{"plugin", "string", "Plugin type name (e.g. GrabImage, FindCircle)", true},
                   {"instanceName", "string", "Unique instance name for this module", false}}});

    // remove_module
    registerTool({"remove_module",
                  "Remove a module from the current project",
                  {{"instanceId", "string", "Module instance ID to remove", true}},
                  true}); // dangerous

    registerTool({"move_module",
                  "Move a module to a zero-based index in the current project order; no-op when already there",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"index", "integer", "Zero-based target index, within the current module list", true}}});

    registerTool({"set_module_position",
                  "Set a module's absolute integer position on the flow canvas; no-op when already there",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"x", "integer", "Absolute canvas X coordinate (signed 32-bit integer)", true},
                   {"y", "integer", "Absolute canvas Y coordinate (signed 32-bit integer)", true}}});

    registerTool({"rename_module",
                  "Change an instance display name without changing its stable ID or connections",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"name", "string", "New non-empty display name", true}}});

    registerTool({"set_module_enabled",
                  "Enable or disable a module instance; a disabled module is bypassed by the existing run engine",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"enabled", "boolean", "Whether the module is enabled", true}}});

    registerTool({"set_breakpoint",
                  "Set or clear a breakpoint on a module instance; stop the flow before changing it",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"enabled", "boolean", "Whether the breakpoint is set", true}}});

    registerTool({"set_module_note",
                  "Set or clear a module note. Use an empty string to clear the note.",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"note", "string", "New note text; empty string clears it", true}}});

    // set_param
    registerTool({"set_param",
                  "Set a parameter value for a module instance",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"key", "string", "Parameter key", true},
                   {"value", "json", "Typed JSON value; legacy numeric and boolean strings remain compatible", true}}});

    registerTool({"set_module_params",
                  "Atomically set multiple typed module parameters after validating the final merged configuration",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"values", "object", "Parameter key to typed JSON value map", true}}});

    registerTool({"reset_module_params",
                  "Atomically remove module parameter overrides and restore plugin defaults",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"keys", "string_array", "Parameter keys to reset", true}}});

    // connect_modules
    registerTool({"connect_modules",
                  "Connect compatible named data or control ports in the current project flow. Use the implicit "
                  "control input name 'control' on any target; use the implicit control output 'next' on any source "
                  "for sequential flow. Modules may also expose named control outputs such as body, done, or stop. "
                  "Multiple control edges may enter one control input. Implicit or declared Any control inputs use OR "
                  "activation; a declared joinPolicy=all control input waits for all incoming edges. Data input fan-in "
                  "follows the declared port multiplicity and join policy.",
                  {{"fromId", "string", "Source module instance ID", true},
                   {"toId", "string", "Target module instance ID", true},
                   {"fromPort", "string", "Source output port name (default: image)", false},
                   {"toPort", "string", "Target input port name (default: matching source port or image)", false},
                   {"port", "string", "Legacy alias for fromPort", false}}});

    // disconnect_modules
    registerTool({"disconnect_modules",
                  "Disconnect two modules",
                  {{"fromId", "string", "Source module instance ID", true},
                   {"toId", "string", "Target module instance ID", true},
                   {"fromPort", "string", "Source output port name", false},
                   {"toPort", "string", "Target input port name", false}}});

    // run_flow
    registerTool({"run_flow",
                  "Run the current project flow once, or for cycle mode provide maxCycles (1..10); each cycle is "
                  "confirmed as a bounded session. An explicit flowId selector is not supported and returns "
                  "FLOW_UNAVAILABLE.",
                  {{"mode", "enum", "Run mode: once or cycle", false, QJsonValue("once"), {"once", "cycle"}},
                   {"flowId", "string",
                    "Optional flow selector; explicit selection is currently unavailable and "
                    "returns FLOW_UNAVAILABLE",
                    false},
                   {"maxCycles", "integer", "Required for cycle mode; maximum complete iterations, 1..10", false}}});

    registerTool({"step_once",
                  "Execute exactly one module in the current flow using the GUI single-step engine",
                  {{"flowId", "string",
                    "Optional flow selector; explicit selection is currently unavailable and "
                    "returns FLOW_UNAVAILABLE",
                    false}},
                  true}); // execution entry point; may trigger module side effects
    registerTool({"reset_step",
                  "Reset the current single-step position so the next step starts at the flow entry",
                  {{"flowId", "string",
                    "Optional flow selector; explicit selection is currently unavailable and "
                    "returns FLOW_UNAVAILABLE",
                    false}}});
    registerTool({"validate_flow",
                  "Validate the current project flow before running; report missing required inputs and the first "
                  "graph/port construction error with locations when available. An explicit flowId selector is "
                  "unavailable and returns FLOW_UNAVAILABLE.",
                  {{"flowId", "string",
                    "Optional flow selector; explicit selection is currently unavailable and "
                    "returns FLOW_UNAVAILABLE",
                    false}}});

    // stop_flow
    registerTool({"stop_flow", "Stop the current flow; returns stopping while an active module settles", {}});

    // get_flow_state
    registerTool(
        {"get_flow_state",
         "Get the current project, ordered modules, ports, and connections with explicit or inferred edge types",
         {{"flowId", "string",
           "Optional flow selector; explicit selection is currently unavailable and "
           "returns FLOW_UNAVAILABLE",
           false}}});

    // get_available_plugins
    registerTool({"get_available_plugins",
                  "List discovered module plugins and a separate camera-driver catalog with metadata, binary "
                  "presence, loaded state, platformCompatibility, currently executable lifecycle actions, and "
                  "machine-readable blocked action reasons. This is read-only: listed load/unload actions can still "
                  "fail during actual plugin validation or initialization. Platform compatibility does not prove "
                  "that a device, SDK, or model is ready.",
                  {}});

    // list_cameras
    registerTool({"list_cameras",
                  "Requires a logged-in user with Camera permission. List camera devices already present in "
                  "CameraManager's cached snapshot without probing drivers or "
                  "changing device state. An empty list does not mean a fresh discovery found no devices.",
                  {}});

    registerTool({"get_camera_discovery_targets",
                  "Requires a logged-in user with Camera permission. Read registered camera plugins that support "
                  "asynchronous discovery and the legacy plugins that "
                  "will not be scanned. This only reads the registry and never probes a driver.",
                  {}});
    registerTool(
        {"discover_cameras",
         "Requires a logged-in user with Camera permission. Start one asynchronous discovery for each exact plugin "
         "in a current registry snapshot. Legacy "
         "plugins are not scanned. This is a device operation and requires confirmation; poll the returned "
         "requestId with get_camera_discovery_operation.",
         {{"registryGeneration", "string", "Canonical registry generation from the target query", true},
          {"pluginIds", "string_array", "Exact sorted async-capable plugin IDs from the target query", true},
          {"unsupportedPluginIds", "string_array", "Exact sorted legacy plugin IDs from the target query", true}},
         true});
    registerTool({"get_camera_discovery_operation",
                  "Requires a logged-in user with Camera permission. Read a pending or terminal aggregate camera "
                  "discovery result. Poll the requestId returned by "
                  "discover_cameras; do not restart the operation.",
                  {{"requestId", "string", "Canonical nonzero decimal aggregate request ID", true}}});

    registerTool({"list_communications",
                  "List configured global communication channels and their current states without connecting, "
                  "disconnecting, sending, or receiving data. Returns config IDs and names as stored; endpoint and "
                  "extra configuration fields are omitted.",
                  {}});

    // get_camera_details
    registerTool({"get_camera_details",
                  "Requires a logged-in user with Camera permission. Read the capabilities and current parameters of "
                  "an already connected camera. Async-session cameras "
                  "return pending with a requestId; poll get_camera_operation for details. This tool never discovers, "
                  "connects, configures, triggers, or captures from a camera. Copy serialNumber and "
                  "cameraSessionGeneration from this result when using set_camera_parameter.",
                  {{"deviceId", "string", "Stable ID of an already connected camera", true}}});

    registerTool(
        {"get_camera_operation",
         "Requires a logged-in user with Camera permission. Read the pending or terminal result for an asynchronous "
         "camera connect, disconnect, details query, or "
         "parameter write. Pass "
         "the requestId string returned by the camera tool; do not repeat the operation.",
         {{"requestId", "string", "Canonical unsigned decimal request ID returned by the camera operation", true}}});

    registerTool({"set_camera_parameter",
                  "Requires a logged-in user with CameraSet permission. Change exactly one parameter on a connected "
                  "camera after confirmation. Copy cameraSessionGeneration "
                  "and serialNumber from get_camera_details. Async sessions return pending; poll "
                  "get_camera_operation. A write is not automatically retried if readback is uncertain.",
                  {{"deviceId", "string", "Exact discovered camera ID", true},
                   {"cameraSessionGeneration", "string", "Current decimal binding version from camera details", true},
                   {"serialNumber", "string", "Exact serial number from camera details", true},
                   {"parameter",
                    "string",
                    "One parameter to set",
                    true,
                    {},
                    {"exposureTime", "gain", "frameRate", "triggerMode", "roi"}},
                   {"value", "json", "Finite number, trigger mode string, or ROI object {x,y,width,height}", true}},
                  true});

    registerTool({"load_module_plugin",
                  "Requires a logged-in user with SystemConfig permission. Load a module plugin already discovered "
                  "in the configured local plugin directories. This changes "
                  "the running process and requires confirmation. Camera plugins and arbitrary paths are rejected.",
                  {{"plugin", "string", "Exact name of a scanned module plugin", true}},
                  true});

    registerTool({"unload_module_plugin",
                  "Requires a logged-in user with SystemConfig permission. Unload a previously loaded module plugin "
                  "from the configured local plugin directories. This "
                  "changes the running process and requires confirmation; active module instances block unloading.",
                  {{"plugin", "string", "Exact name of a scanned module plugin", true}},
                  true});

    registerTool({"connect_camera",
                  "Requires a logged-in user with Camera permission. Connect a camera device already listed by "
                  "CameraManager. This may access hardware and requires "
                  "confirmation. The tool does not accept arbitrary addresses or discover unknown devices.",
                  {{"deviceId", "string", "Exact ID of a camera already present in CameraManager", true}},
                  true});

    registerTool({"disconnect_camera",
                  "Requires a logged-in user with Camera permission. Disconnect a camera device already listed by "
                  "CameraManager. This changes device state and "
                  "requires confirmation; a held camera lease may defer disconnection.",
                  {{"deviceId", "string", "Exact ID of a camera already present in CameraManager", true}},
                  true});

    // save_project
    registerTool({"save_project",
                  "Save the current project",
                  {{"path", "string", "Optional save path", false}},
                  true}); // dangerous

    // get_module_params_schema
    registerTool({"get_module_params_schema",
                  "Describe a plugin before creation or an existing module instance. Use nested parameter descriptions "
                  "to interpret coordinate fields; for NPointCalibration, image points use current-image local "
                  "pixels, while world points and reprojection_error retain the caller-defined target-plane scale. "
                  "Do not infer millimeters, a fixed world reference, or image-source provenance from these fields.",
                  {{"instanceId", "string", "Existing module instance ID", false},
                   {"plugin", "string", "Plugin type name or stable ID (before instance creation)", false}}});

    // get_run_results
    registerTool({"get_run_results",
                  "Get a run's status by runId (or the latest run when omitted), with cumulative statistics. "
                  "For a cycle session, lastCompletedRunId identifies the latest finished tick whose output can "
                  "be queried; bounded Agent cycle sessions also report maxCycles, completedCycles, and "
                  "lastSuccessfulRunId. Unknown runId returns RUN_NOT_FOUND; a finished run reports completed, failed "
                  "or cancelled. A PLC write whose request was accepted by the local socket but has no valid "
                  "success echo reports uncertain with retryable=false; do not repeat it automatically.",
                  {{"runId", "string", "Run ID returned by run_flow (defaults to latest)", false}}});

    // get_module_output（阶段 3）
    registerTool(
        {"get_module_output",
         "Query a module's business output from a successful completed run. Without a key, "
         "returns a summary (image metadata + keys, optional bounded preview). Use key or port to "
         "select the exact output namespace; arrays are paginated and region masks expose bounded "
         "maskOffset chunks through maskExport. Region2D polygon vertices can be paged with "
         "polygonOffset/polygonLimit; source vertices are included only if the whole requested page maps. "
         "PointCloudData summaries and previews use coordinateSystem=unspecified; do not infer units, "
         "world-frame transforms, or source-image mappings. For FreeformSurface surface_roughness and "
         "surface_area, the values depend on the input point-cloud XYZ scales. Their physical dimensions are "
         "length and length squared only when all three axes use a common physical unit/scale; PointCloudData "
         "does not declare units and LoadPointCloud can scale axes independently, so do not infer mm, mm^2, "
         "or a world frame. CalculateOffset offset_x and offset_y are raw current-minus-target numeric differences; "
         "only compare or interpret them physically when both coordinates use the same frame and unit. The plugin "
         "does not transform frames or units. current_point overrides only X/Y and ignores Z; current_angle, when "
         "present, overrides the parameter angle. offset_a is the degree difference normalized to (-180,180]. "
         "For cycle mode, use the "
         "lastCompletedRunId from get_run_results(sessionId), not the sessionId. Returns pending while "
         "the run is active, or STALE_OUTPUT if the project changed. If a PLC write may have occurred "
         "without a valid success echo, returns uncertain with retryable=false; do not repeat it. "
         "For image-coordinate outputs, coordinateSystem=image_pixels means current-image local pixel "
         "coordinates. If the image summary contains sourceCoordinates.localToSource, use its row-major "
         "3x3 matrix M and treat integer (x,y) as pixel centers. Compute [u,v,w]^T = M * [x,y,1]^T, "
         "then x'=u/w=(m00*x+m01*y+m02)/w and y'=v/w=(m10*x+m11*y+m12)/w, where "
         "w=m20*x+m21*y+m22 must satisfy abs(w) > 16 * double epsilon * "
         "max(1, abs(m20*x)+abs(m21*y)+abs(m22)); x' and y' must be finite. "
         "If sourceCoordinates is absent or a point cannot be mapped, keep the local coordinate label "
         "and do not claim source-image coordinates; query the image summary first if needed. "
         "For FitPlane port plane, value is nine numbers for three XYZ points in height_map_local_scaled_xyz: "
         "X=column*pixelSizeX, Y=row*pixelSizeY, Z=height*zScale. unitBasis lists the scaling parameter names, "
         "not units or values. Unless the project explicitly supplies engineering units, units are unknown; "
         "do not infer mm or a world frame. This guidance applies only to FitPlane port plane, not key plane "
         "or other Plane3D outputs. FitPlane scalar outputs plane_d, flatness, max_deviation, min_deviation, and "
         "rms retain numeric values from the pixelSizeX/pixelSizeY/zScale-scaled coordinates. Those parameters "
         "do not declare units or establish a shared physical scale; physical interpretation requires external "
         "confirmation that all axes use a consistent engineering unit. Do not infer mm. The plane_nx/ny/nz "
         "components are the dimensionless unit-normal components of that plane in those scaled XYZ axes, "
         "oriented toward +Z by the FitPlane algorithm; they do not declare a physical unit or external world "
         "reference. valid_pixel_count is a count. This guidance applies only to those "
         "five scalar outputs. For 3DPreProcessing min_height and max_height, values preserve the numeric "
         "scale of valid input height pixels; the plugin performs no physical unit conversion, and input-height "
         "units and an external Z reference are not declared. Do not infer mm or a World3D frame. This guidance "
         "does not apply to valid_pixel_count, filtered_pixel_count, or height_invalid_value. For MeasurementInput "
         "ports point and plane when measurement_input_mode="
         "point_plane and coordinate_frame=World3D, type is Point3D or Plane3D and coordinateFrame repeats the "
         "plugin frame label. A Point3D value is three XYZ numbers; a Plane3D value is three XYZ points in order. "
         "These are plugin input values: units, axis definition, and external reference are unknown. Do not infer "
         "physical units or add image-source coordinates; this is not evidence that the values came from a point "
         "cloud. For PointSurfaceDistance port foot_point, Point3D/coordinateFrame=World3D is returned only when "
         "the same run carries coordinate_frame=World3D and the computed foot is finite. The frame label is "
         "inherited from the 3D input; it does not declare units or an external reference. Do not map this 3D "
         "point through image provenance. For PointSurfaceDistance keys foot_x, foot_y, and foot_z, "
         "coordinateFrame=World3D is included only when each queried finite numeric key matches its component in "
         "the same output's valid foot_point. This repeats the input frame only; it does not declare units or an "
         "external reference and does not add image-source coordinates. Without that evidence, the legacy key "
         "remains a raw value without a frame claim. PointSurfaceDistance distance is the raw perpendicular distance "
         "from "
         "input XYZ values; the plugin does not convert units. coordinate_frame=World3D labels only the input frame; "
         "it does not declare units, axis definitions, or an external reference. Do not infer mm or other physical "
         "units. This guidance applies only to distance, not foot_x/y/z or foot_point. For EdgeDefectDetection "
         "defect_regions, columnUnits marks only max_deviation and mean_deviation as px when the same run reports "
         "measurement_input_mode=point_set and coordinate_frame=ImageLocalPixel and image/reference_edge are "
         "connected to image/fit_points from one MeasurementInput instance. The table has no single unit; angle, "
         "index, count, and polarity columns do not inherit px. For FitLine keys line_row1, line_col1, line_row2, "
         "and line_col2, unit=px is reported only when the same ImageLocalPixel run has a valid finite Line2D "
         "whose corresponding endpoint exactly matches the queried scalar. row is y and col is x in the current "
         "image-local pixel grid; this guidance does not assign units or redefine line_phi/line_rho. For LinesDistance "
         "nearest_x1/y1/x2/y2, px requires same-source MeasurementInput image/line1/line2, line_pair, "
         "ImageLocalPixel, and an exact match to the same run's nearest_segment endpoint. The scalar has no source "
         "coordinate mapping; distance remains unlabelled. For MeasureGap gap, gap_distance, gap_delta_x, and "
         "gap_delta_y, px requires a same-run 2D point_pair from same-source MeasurementInput point1/point2 with "
         "matching finite outputs. Three-element points remain unlabelled even when z=0; gap_delta_z is unlabelled. "
         "ImageLocalPixel alone does not prove the source or output correspondence, and no source-image coordinates "
         "are inferred. GapMeasure3D "
         "gap_width, "
         "gap_offset_width, and corner_dx_mm use mm "
         "when gap_found is true; query gap_found first because otherwise these values are the measureFailValue "
         "sentinel, not measurements. The legacy keys corner_dz_mm and corner_dist_mm do not guarantee mm; "
         "their physical units are unknown because zScale and input-height units are unspecified. "
         "corner_dist_mm is the legacy numeric hypot(corner_dx_mm, corner_dz_mm); interpret it as a physical "
         "Euclidean distance only when X and Z are calibrated in the same physical unit.",
         {{"runId", "string", "Completed run ID; cycle mode uses lastCompletedRunId", true},
          {"instanceId", "string", "Module instance ID", true},
          {"port", "string", "Output port ID (mutually exclusive with key)", false},
          {"key", "string", "Named data key (omit for summary)", false},
          {"preview", "boolean", "Include a bounded image, region, or point-cloud preview", false},
          {"offset", "integer", "Zero-based row offset for array results (default 0)", false},
          {"limit", "integer", "Rows per page, 1..1000 (default 100)", false},
          {"maskOffset", "integer", "Zero-based RLE character offset for a region mask chunk", false},
          {"polygonOffset", "integer", "Zero-based vertex offset for a Region2D polygon page (default 0)", false},
          {"polygonLimit", "integer", "Vertices per Region2D polygon page, 1..1000 (default 100)", false}}});

    // wait_for_run（阶段 3）
    registerTool({"wait_for_run",
                  "Poll a run without blocking the Qt event loop. Returns pending with retryAfterMs while busy, "
                  "or completed, failed, cancelled, or uncertain when finished. Uncertain PLC write outcomes "
                  "include retryable=false and must not be repeated automatically.",
                  {{"runId", "string", "Run ID returned by run_flow (defaults to latest)", false}}});

    // open_project
    registerTool({"open_project",
                  "Open an existing project by path. If the current project has unsaved changes the call "
                  "fails with PROJECT_UNSAVED unless onUnsaved is given (same policy as create_project). "
                  "The candidate project is fully loaded and validated before the current one is replaced; "
                  "on failure the current project stays intact.",
                  {{"path", "string", "Path to the project file (.dproj)", true},
                   {"onUnsaved",
                    "enum",
                    "Unsaved-changes policy: save | discard | cancel",
                    false,
                    QJsonValue("cancel"),
                    {"save", "discard", "cancel"}}}});

    // pause_flow
    registerTool({"pause_flow", "Pause the running flow and report paused only after the engine transitions", {}});

    // resume_flow
    registerTool({"resume_flow",
                  "Resume the currently paused flow after the Agent confirmation gate; the confirmation is bound to "
                  "the paused run/session and current project version. If the flow is not paused, approval returns "
                  "no_op. Query run results for its eventual terminal state.",
                  {}});

    // read_documentation
    registerTool({"read_documentation",
                  "Read DeepLux module/usage documentation, including the built-in knowledge base and "
                  "user-added documents (managed via Agent Settings -> Knowledge Base). Results are paged by "
                  "matching sections; requires the signed-in user's OpenFile permission. Use offset/limit to "
                  "continue. Use this to learn about module parameters, "
                  "workflows, and best practices when you're unsure.",
                  {{"topic", "string", "Topic to look up (e.g. 'FindCircle', 'workflow', 'GrabImage params')", true},
                   {"offset", "integer", "Zero-based matching-section offset", false, QJsonValue(0)},
                   {"limit", "integer", "Page size from 1 to 50", false, QJsonValue(10)}}});

    registerTool({"list_knowledge_documents",
                  "List built-in read-only and user knowledge documents. Returns stable document IDs and metadata, "
                  "not document contents. Requires the signed-in user's OpenFile permission.",
                  {}});
    registerTool({"preview_knowledge_document",
                  "Preview up to 512 KiB of one knowledge document by its ID from list_knowledge_documents. "
                  "Requires the signed-in user's OpenFile permission.",
                  {{"documentId", "string", "Exact knowledge document ID", true}}});
    registerTool({"add_knowledge_document",
                  "Copy a readable Markdown or text file into the user knowledge base. Existing names are never "
                  "overwritten. Requires the signed-in user's SaveFile permission and confirmation; leaves the "
                  "source file unchanged.",
                  {{"sourcePath", "string", "Readable .md, .markdown, or .txt source file", true}},
                  true});
    registerTool({"remove_knowledge_document",
                  "Remove one user knowledge document by ID. Built-in documents are read-only. Requires confirmation; "
                  "the signed-in user needs SaveFile permission; the original source file is not affected.",
                  {{"documentId", "string", "Exact user document ID from list_knowledge_documents", true}},
                  true});

    registerTool(
        {"get_annotation_session",
         "Read one explicitly named DeepLux annotation session. Returns paged object summaries in the pixel "
         "coordinates of that session's bound image; if the bound image is a CropImage output, coordinates are "
         "crop-local and do not map to the parent image. Mask data is never returned. Does not open the GUI session "
         "or run SAM. Requires the signed-in user's OpenFile permission.",
         {{"sessionPath", "string", "Path to one existing .deeplux-anno.json session", true},
          {"offset", "integer", "Zero-based object offset", false, QJsonValue(0)},
          {"limit", "integer", "Page size from 1 to 50", false, QJsonValue(20)},
          {"includePolygon", "boolean", "Include polygon coordinates in the session image pixel frame", false,
           QJsonValue(false)}}});

    registerTool(
        {"export_annotation_session",
         "Export one explicitly named annotation session to a new LabelMe JSON, YOLO Seg text, or COCO JSON "
         "file. The destination must not exist; this creates a user-visible file and always requires "
         "the signed-in user's SaveFile permission and confirmation. The original image and session are not modified.",
         {{"sessionPath", "string", "Path to one existing .deeplux-anno.json session", true},
          {"format", "string", "Export format", true, QJsonValue(), {"labelme", "yolo_seg", "coco"}},
          {"outputPath", "string", "Explicit new destination path (.json or .txt)", true},
          {"categories", "string_array", "Ordered export categories; required for YOLO Seg", false, QJsonArray()}},
         true});

    registerTool(
        {"transform_annotation_session",
         "Create a new annotation session copy with explicitly selected objects relabeled or deleted. The source is "
         "never modified, the destination must not exist, and every operation requires the signed-in user's "
         "SaveFile permission and confirmation. This does not change the open SAM dialog or its in-memory undo state.",
         {{"sessionPath", "string", "Path to one existing .deeplux-anno.json session", true},
          {"outputPath", "string", "Explicit new .deeplux-anno.json destination path", true},
          {"operation", "string", "Single transform to apply", true, QJsonValue(), {"set_label", "delete_objects"}},
          {"objectIds", "string_array", "Exact unique annotation object IDs to transform", true},
          {"label", "string", "New label for set_label; forbidden for delete_objects", false}},
         true});

    registerTool({"list_active_annotation_objects",
                  "Requires the signed-in user's OpenFile permission. List paged summaries of objects in the visible "
                  "active SAM annotation dialog. Returns exact IDs, "
                  "labels, and ImageLocalPixel bounding boxes only; when the displayed image has verified source "
                  "provenance, each object also includes a sourceImageId, sourceImageSize, and sourceBbox. Never "
                  "returns masks, polygons, image data, or paths.",
                  {{"offset", "integer", "Zero-based object offset", false, 0},
                   {"limit", "integer", "Page size from 1 to 50", false, 20}}});
    registerTool({"edit_active_annotation_masks",
                  "Requires the signed-in user's Edit permission. Edit exact objects in the visible active SAM dialog "
                  "using its existing undoable MaskOps path. Requires confirmation. strokePoints is [[x,y],...] in "
                  "the active annotation image pixel "
                  "coordinates; if the active image is a CropImage output, these coordinates are crop-local and do "
                  "not map to the parent image. The confirmation card shows only its point count, never coordinates.",
                  {{"operation",
                    "string",
                    "Single mask operation",
                    true,
                    QJsonValue(),
                    {"dilate", "erode", "erase_stroke", "merge", "split"}},
                   {"objectIds", "string_array", "Exact object IDs from list_active_annotation_objects", true},
                   {"radiusPx", "integer",
                    "Required for dilate/erode/erase_stroke; active annotation image pixel units; range 1-32", false},
                   {"strokePoints", "array", "erase_stroke only: 1-4096 points, each exactly [x,y]", false}},
                  true});
    registerTool({"list_active_annotation_categories",
                  "Requires the signed-in user's OpenFile permission. List the temporary category names and paged "
                  "object labels in the visible active SAM dialog. "
                  "Never returns masks, prompts, image data, or paths.",
                  {{"offset", "integer", "Zero-based object offset", false, 0},
                   {"limit", "integer", "Object page size from 1 to 50", false, 20}}});
    registerTool({"edit_active_annotation_categories",
                  "Requires the signed-in user's Edit permission. Add or remove a temporary category, or set labels "
                  "on exact objects in the visible active SAM dialog. Requires confirmation; label edits are undoable "
                  "in the GUI.",
                  {{"operation",
                    "string",
                    "Single category or object-label operation",
                    true,
                    QJsonValue(),
                    {"add_category", "remove_category", "set_object_label"}},
                   {"category", "string", "Category for add_category/remove_category", false},
                   {"objectIds", "string_array", "Exact object IDs for set_object_label", false},
                   {"label", "string", "Existing category for set_object_label", false}},
                  true});
    registerTool({"get_active_annotation_prompt_status",
                  "Requires the signed-in user's OpenFile permission. Read whether the visible SAM dialog has an "
                  "active point/box prompt, prediction request, or mask "
                  "candidates. Does not expose prompt coordinates, masks, image data, or paths.",
                  {}});
    registerTool(
        {"cancel_active_annotation_prompt",
         "Cancel the transient prompt, prediction preview, candidates, and current SAM prediction request "
         "in the visible SAM dialog. This cancellation does not require a product login/permission, but the Agent "
         "confirmation policy still applies. Preserves confirmed annotations and GUI undo/redo.",
         {},
         true});
    registerTool({"save_active_annotation_session",
                  "Save the confirmed annotations in the visible SAM dialog's current in-memory session to a new "
                  ".deeplux-anno.json file. Requires the signed-in user's SaveFile permission and confirmation. "
                  "The destination must be an absolute path that does not already exist; existing files are never "
                  "replaced.",
                  {{"outputPath", "string", "Absolute path for a new .deeplux-anno.json file", true}},
                  true});
    registerTool({"open_active_annotation_session",
                  "Open an existing native .deeplux-anno.json session in the visible active SAM dialog. Requires "
                  "confirmation and the signed-in user's OpenFile permission; permission is rechecked when approved. "
                  "It replaces the in-memory SAM session and clears its prompts and undo history. The source session "
                  "and image are never modified.",
                  {{"sessionPath", "string", "Absolute path to an existing .deeplux-anno.json file", true}},
                  true});

    registerTool({"select_flow_module",
                  "Select one exact flow module instance in the open GUI, synchronize the tree, canvas, and inspector, "
                  "and show the inspector. This is navigation only; it does not edit or run the project.",
                  {{"instanceId", "string", "Exact module instance ID from get_flow_state", true}}});

    registerTool({"list_global_variables",
                  "Requires a logged-in user with GlobalVar permission. List current-process global variable names "
                  "and types without exposing their values",
                  {{"type",
                    "string",
                    "Optional exact type filter",
                    false,
                    QJsonValue(),
                    {"int", "double", "string", "bool", "int[]", "double[]", "string[]", "bool[]"}}}});
    registerTool({"get_global_variable",
                  "Requires a logged-in user with GlobalVar permission. Read one named global variable value. "
                  "Request only values needed for the current task.",
                  {{"name", "string", "Exact global variable name", true}}});
    registerTool({"create_global_variable",
                  "Requires a logged-in user with GlobalVar permission. Create a typed process-wide global variable. "
                  "Requires confirmation; this is not saved with the "
                  "project and does not evaluate expressions.",
                  {{"name", "string", "Variable name", true},
                   {"type",
                    "string",
                    "Supported scalar or array type",
                    true,
                    QJsonValue(),
                    {"int", "double", "string", "bool", "int[]", "double[]", "string[]", "bool[]"}},
                   {"value", "json", "Typed JSON literal matching type", true}},
                  true});
    registerTool({"set_global_variable",
                  "Requires a logged-in user with GlobalVar permission. Set one existing global variable from a "
                  "strictly typed JSON literal. Requires confirmation; it "
                  "does not evaluate expressions.",
                  {{"name", "string", "Exact global variable name", true},
                   {"value", "json", "Typed JSON literal matching the existing variable type", true}},
                  true});
    registerTool({"remove_global_variable",
                  "Requires a logged-in user with GlobalVar permission. Remove one existing global variable from "
                  "the current process. Requires confirmation and is not "
                  "undoable.",
                  {{"name", "string", "Exact global variable name", true}},
                  true});

    registerTool(
        {"list_data_sources",
         "List data source references in the open project. This does not load source files or change the "
         "project. fileExists reports filesystem presence only.",
         {{"type", "string", "Optional exact source type filter", false, QJsonValue(), {"image", "pointcloud"}}}});
    registerTool({"remove_data_source_reference",
                  "Remove one data source reference from the open project after confirmation. The source file is "
                  "preserved. Fails if the ID is absent or ambiguous, the project changed, or the engine is busy.",
                  {{"dataSourceId", "string", "Exact data source ID from list_data_sources", true}},
                  true});
    registerTool({"import_data_source",
                  "Validate a local image, PLY point cloud, or TIFF height map and register only its file reference "
                  "in the open project. Requires confirmation. Does not display or cache the file, configure "
                  "GrabImage, save the project, or modify the source. Use display_data_source separately.",
                  {{"path", "string", "Path to an existing supported local image, PLY, or TIFF file", true}},
                  true});
    registerTool({"get_data_source_view_state",
                  "Read the current GUI display, visibility, and dedicated-viewport state for project data sources. "
                  "Requires an open GUI window and does not load or change data.",
                  {}});
    registerTool({"display_data_source",
                  "Load one registered image or point cloud into the current main viewport. Requires GUI and "
                  "confirmation; does not create a viewport, change Project, or configure GrabImage.",
                  {{"dataSourceId", "string", "Exact data source ID from list_data_sources", true}},
                  true});
    registerTool({"display_data_source_in_dedicated_viewport",
                  "Load or reuse one registered image, TIFF height map, or PLY point cloud in its dedicated data "
                  "viewport. Requires GUI and confirmation; does not alter the primary viewport, project, or source "
                  "file. Reuses the existing dedicated viewport when present.",
                  {{"dataSourceId", "string", "Exact data source ID from list_data_sources", true}},
                  true});
    registerTool({"set_data_source_visibility",
                  "Set one registered data source visible or hidden in the current GUI. Showing an uncached source "
                  "loads it first; failures preserve the prior GUI state. Requires confirmation.",
                  {{"dataSourceId", "string", "Exact data source ID from list_data_sources", true},
                   {"visible", "boolean", "Whether the data source should be shown", true}},
                  true});
    registerTool(
        {"set_workspace_panel_visibility",
         "Set a main workspace panel's desired visibility in the open GUI. This changes only view state, "
         "does not require a project, and does not require confirmation. Focus mode may temporarily "
         "suppress effective visibility; inspect both returned states.",
         {{"panel", "string", "Workspace panel to control", true, QJsonValue(), {"tools", "process", "bottom"}},
          {"visible", "boolean", "Whether the panel should be shown outside focus mode", true}}});
    registerTool({"set_log_level_filter",
                  "Set the minimum level shown in the current GUI runtime log table. Existing rows are filtered; "
                  "future low-level logs omitted on arrival are not buffered for later redisplay. Does not clear logs "
                  "or change Logger persistence.",
                  {{"minimumLevel",
                    "string",
                    "Minimum runtime-log level to display",
                    true,
                    QJsonValue(),
                    {"all", "debug", "info", "warning", "error"}}}});
    registerTool(
        {"select_log_panel_tab",
         "Select the runtime log, Agent log, or terminal page in the current GUI bottom panel. Does not "
         "open the dock; selecting the Agent conversation page and sending terminal commands are separate.",
         {{"tab", "string", "Bottom-panel page to select", true, QJsonValue(), {"runtime", "agent", "terminal"}}}});
    registerTool({"scroll_agent_chat_to_latest",
                  "Scroll the current Agent conversation to its latest message and resume following new messages. "
                  "Does not send a message, switch panels, or open the conversation panel.",
                  {}});
    registerTool(
        {"clear_runtime_logs",
         "Clear all current in-memory Logger entries across categories and clear connected runtime log tables. "
         "Requires explicit confirmation. Does not truncate the on-disk log file or clear Agent action history.",
         {},
         true});

    // sam_predict（SAM 批3）：经 SAM 服务发送图像和 prompt 进行外部推理，Autopilot 需确认。
    registerTool({"sam_predict",
                  "Requires the signed-in user's OpenFile permission. Run SAM segmentation on an image file with "
                  "point/box prompts via the local "
                  "SAM server (must be running). Returns label/score/bbox/polygon of the region. "
                  "Fails closed on timeout, backend error or contract-invalid region.",
                  {{"imagePath", "string", "Absolute path to an existing image file", true},
                   {"points", "array", "Positive prompt points [[x,y],...] with finite numbers", false},
                   {"negativePoints", "array", "Negative prompt points [[x,y],...]", false},
                   {"box", "array", "Prompt box [x,y,w,h] with w,h>0 (optional)", false},
                   {"label", "string", "Label for the predicted region", false, QJsonValue("object")},
                   {"candidateIndex", "integer",
                    "Pick among SAM multi-mask candidates sorted by score desc (0=best, default); "
                    "fails closed if the server returned no candidates or the index is out of range",
                    false, QJsonValue(0)},
                   {"serverUrl", "string", "SAM server base URL", false, QJsonValue("http://127.0.0.1:8990")},
                   {"timeoutMs", "integer", "Per-request timeout in ms, [100,120000]", false, QJsonValue(30000)}},
                  true});

    // sam_annotate（SAM 批3，批4 扩展）：批量 prompt 分割并写入标注会话文件。
    // dangerous：写磁盘文件（与 save_project 同口径，Autopilot 需确认）
    registerTool({"sam_annotate",
                  "Run SAM segmentation for a batch of prompts and append the results as annotation "
                  "objects to a DeepLux annotation session file (.deeplux-anno.json, created if absent). "
                  "Requires the signed-in user's SaveFile permission. "
                  "Prompts come from 'items' OR 'labelMePath' (exactly one; mutually exclusive). "
                  "All-or-nothing: nothing is written if any prediction fails. Refuses to mix images: "
                  "an existing session bound to a different image (path/size/digest) is rejected.",
                  {{"imagePath", "string", "Absolute path to an existing image file", true},
                   {"sessionPath", "string", "Annotation session file to create or append to", true},
                   {"items", "array",
                    "1-16 prompt items [{points?,negativePoints?,box?,label?,candidateIndex?}]; "
                    "each item needs points or box (mutually exclusive with labelMePath)",
                    false},
                   {"labelMePath", "string",
                    "Path to a LabelMe JSON file whose rectangle shapes become box prompts "
                    "(1-16 usable rectangles; non-rectangles skipped and reported; "
                    "mutually exclusive with items)",
                    false},
                   {"serverUrl", "string", "SAM server base URL", false, QJsonValue("http://127.0.0.1:8990")},
                   {"timeoutMs", "integer", "Per-request timeout in ms, [100,120000]", false, QJsonValue(30000)}},
                  true}); // dangerous（写文件）
}

} // namespace DeepLux
