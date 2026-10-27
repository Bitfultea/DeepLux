#include "ToolSchema.h"

#include <QtGlobal>

namespace DeepLux {

namespace {
// 工厂共用的唯一构造路径：三个访问级工厂只决定 readOnly/dangerous 两个布尔量，
// 这里集中断言 §3.1 的不变式（dangerous ⇒ !readOnly）。
ToolDefinition makeTool(const QString& name, const QString& description, const QList<ToolParam>& params, int timeoutMs,
                        bool readOnly, bool dangerous) {
    ToolDefinition tool;
    tool.name = name;
    tool.description = description;
    tool.params = params;
    tool.dangerous = dangerous;
    tool.readOnly = readOnly;
    tool.timeoutMs = timeoutMs;
    Q_ASSERT(!(tool.readOnly && tool.dangerous));
    return tool;
}
} // namespace

ToolDefinition makeReadOnlyTool(const QString& name, const QString& description, const QList<ToolParam>& params,
                                int timeoutMs) {
    return makeTool(name, description, params, timeoutMs, /*readOnly=*/true, /*dangerous=*/false);
}

ToolDefinition makeWriteTool(const QString& name, const QString& description, const QList<ToolParam>& params,
                             int timeoutMs) {
    return makeTool(name, description, params, timeoutMs, /*readOnly=*/false, /*dangerous=*/false);
}

ToolDefinition makeDangerousTool(const QString& name, const QString& description, const QList<ToolParam>& params,
                                 int timeoutMs) {
    return makeTool(name, description, params, timeoutMs, /*readOnly=*/false, /*dangerous=*/true);
}

QJsonObject ToolParam::toJson() const {
    QJsonObject obj;
    // OpenAI/DeepSeek JSON Schema: type must be "string", "number", "boolean", "object", "array", "null".
    // "enum" is NOT a valid type. If enumValues is present, type should be "string".
    if (!enumValues.isEmpty()) {
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
    schema["readOnly"] = readOnly;
    schema["timeoutMs"] = timeoutMs;

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

int ToolSchema::effectiveTimeoutMs(const ToolDefinition& tool) {
    return tool.timeoutMs > 0 ? tool.timeoutMs : kDefaultToolTimeoutMs;
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
    registerTool(
        {"create_project", "Create a new project with the given name", {{"name", "string", "Project name", true}}});

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

    // set_param
    registerTool({"set_param",
                  "Set a parameter value for a module instance",
                  {{"instanceId", "string", "Module instance ID", true},
                   {"key", "string", "Parameter key", true},
                   {"value", "string", "Parameter value (as string, will be converted)", true}}});

    // connect_modules
    registerTool({"connect_modules",
                  "Connect two modules in the flow",
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

    // run_flow（就地扩展：wait/timeoutMs 追加在参数末尾，保持既有提示词兼容；
    // 第 4/5/6 位分别为 dangerous/readOnly/timeoutMs——见设计 §4.7）
    registerTool(
        ToolDefinition{"run_flow",
                       "Run the current flow. wait=true (default) waits for completion and reports success/elapsed/"
                       "failed node; wait=false returns immediately.",
                       {{"mode", "enum", "Run mode: once or cycle", false, QJsonValue("once"), {"once", "cycle"}},
                        {"wait", "boolean", "Wait for completion before returning", false, QJsonValue(true)},
                        {"timeoutMs", "integer", "Wait timeout in ms (wait=true), clamped to [1000,300000]", false,
                         QJsonValue(60000)}},
                       false,
                       false,
                       60000});

    // stop_flow
    registerTool({"stop_flow", "Stop the current running flow", {}});

    // get_flow_state（readOnly：只读观察）
    registerTool(makeReadOnlyTool("get_flow_state", "Get the current flow state (modules, connections, params)", {}));

    // get_available_plugins（readOnly）
    registerTool(makeReadOnlyTool("get_available_plugins", "List all available plugin modules", {}));

    // save_project
    registerTool({"save_project",
                  "Save the current project",
                  {{"path", "string", "Optional save path", false}},
                  true}); // dangerous

    // get_module_params_schema（readOnly）
    registerTool(makeReadOnlyTool("get_module_params_schema", "Get the parameter schema for a specific module instance",
                                  {{"instanceId", "string", "Module instance ID", true}}));

    // get_run_results（readOnly）
    registerTool(makeReadOnlyTool("get_run_results", "Get the run engine statistics and results", {}));

    // open_project
    registerTool({"open_project",
                  "Open an existing project by path",
                  {{"path", "string", "Path to the project file (.dproj)", true}}});

    // pause_flow
    registerTool({"pause_flow", "Pause the currently running flow", {}});

    // resume_flow
    registerTool({"resume_flow", "Resume a paused flow", {}});

    // read_documentation（readOnly）
    registerTool(makeReadOnlyTool(
        "read_documentation",
        "Read DeepLux module/usage documentation, including the built-in knowledge base and "
        "user-added documents (managed via Agent Settings -> Knowledge Base). Use this to learn "
        "about module parameters, workflows, and best practices when you're unsure.",
        {{"topic", "string", "Topic to look up (e.g. 'FindCircle', 'workflow', 'GrabImage params')", true}}));

    // sam_predict（SAM 批3）：对磁盘图像做单次 prompt 分割。
    // 非 readOnly（触发外部推理而非观察应用状态）、非 dangerous（不写任何文件）
    registerTool({"sam_predict",
                  "Run SAM segmentation on an image file with point/box prompts via an HTTP SAM server "
                  "or explicit native ONNX mode. Returns label/score/bbox/polygon of the region. "
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
                   {"serverUrl", "string", "SAM server URL, or 'native' using saved native model settings", false,
                    QJsonValue("http://127.0.0.1:8990")},
                   {"timeoutMs", "integer", "Per-request timeout in ms, [100,120000]", false, QJsonValue(30000)}}});

    // sam_annotate（SAM 批3，批4 扩展）：批量 prompt 分割并写入标注会话文件。
    // dangerous：写磁盘文件（与 save_project 同口径，Autopilot 需确认）
    registerTool({"sam_annotate",
                  "Run SAM segmentation for a batch of prompts and append the results as annotation "
                  "objects to a DeepLux annotation session file (.deeplux-anno.json, created if absent). "
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
                   {"serverUrl", "string", "SAM server URL, or 'native' using saved native model settings", false,
                    QJsonValue("http://127.0.0.1:8990")},
                   {"timeoutMs", "integer", "Per-request timeout in ms, [100,120000]", false, QJsonValue(30000)}},
                  true}); // dangerous（写文件）

    // ---- 第一迭代新增 6 个工具（设计 §4.9；顺序固定以保证 toOpenAIFunctions 输出稳定）----
    // 全部经工厂注册：契约单源化后禁止裸聚合初始化。

    // validate_workflow（readOnly：只读工程 + 结构化校验结论）
    registerTool(makeReadOnlyTool(
        "validate_workflow",
        "Validate the current flow without running it: missing required input ports, connection/type conflicts "
        "for disabled nodes, and empty projects. Returns structured issues with code/node/message.",
        {}, 30000));

    // get_node_runtime_outputs（readOnly）
    registerTool(makeReadOnlyTool(
        "get_node_runtime_outputs",
        "Read the last runtime port outputs and status of one module node: named output ports, elapsed time, "
        "success/failure and frame id. Requires a finished run; returns engine_busy while the flow is running.",
        {{"nodeName", "string", "Module instance name (preferred) or instance id", true}}));

    // get_execution_logs（readOnly：只读 Logger 内存副本）
    registerTool(makeReadOnlyTool(
        "get_execution_logs",
        "Read recent in-memory application/run logs with filters. Use category 'Run' for run-engine events, "
        "'Agent' for agent actions. Returns the newest matching entries, truncated flag when older entries exist.",
        {{"category", "string", "Exact log category, e.g. 'Run', 'Agent', 'Camera'", false},
         {"level",
          "enum",
          "Minimum level filter",
          false,
          QJsonValue("debug"),
          {"debug", "info", "warning", "error", "success"}},
         {"keyword", "string", "Case-insensitive substring match on message", false},
         {"limit", "integer", "Max entries to return, clamped to [1,200]", false, QJsonValue(50)}}));

    // tune_node_and_verify（写，非 dangerous：改工程参数可撤销；验证运行不写外部资源）
    registerTool(makeWriteTool(
        "tune_node_and_verify",
        "Apply a parameter patch to one node atomically and verify the effect. runMode=flow (default) writes the "
        "parameters, runs the whole flow once waiting for completion, and returns before/after plus this node's "
        "runtime outputs. runMode=node only applies the patch and reports the previous frame's outputs without "
        "running anything (no side effects).",
        {{"nodeName", "string", "Module instance name (preferred) or instance id", true},
         {"parameters", "object", "Parameter patch: key -> value (string/number/bool), 1..32 keys", true},
         {"runMode", "enum", "Verification mode", false, QJsonValue("flow"), {"flow", "node"}},
         {"timeoutMs", "integer", "Wait timeout for runMode=flow, clamped to [1000,300000]", false, QJsonValue(60000)}},
        300000));

    // capture_viewport_image（readOnly：只写工具自命名的 temp 产物）
    registerTool(makeReadOnlyTool(
        "capture_viewport_image",
        "Capture the image currently shown in the main data-source viewport (falls back to the last flow output) "
        "into a temporary PNG. The returned imagePath is attached to the next model turn automatically when the "
        "model supports images.",
        {}));

    // export_node_output（readOnly：只写工具自命名的 temp 产物）
    registerTool(makeReadOnlyTool(
        "export_node_output",
        "Export one node's last runtime image output (named output port 'image', or the module's recorded output) "
        "to a temporary PNG/JPG. The returned imagePath is attached to the next model turn automatically.",
        {{"nodeName", "string", "Module instance name (preferred) or instance id", true},
         {"format", "enum", "Image format", false, QJsonValue("png"), {"png", "jpg"}}}));
}

} // namespace DeepLux
