#include "ToolSchema.h"

namespace DeepLux {

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

    // run_flow
    registerTool({"run_flow",
                  "Run the current flow once",
                  {{"mode", "enum", "Run mode: once or cycle", false, QJsonValue("once"), {"once", "cycle"}}}});

    // stop_flow
    registerTool({"stop_flow", "Stop the current running flow", {}});

    // get_flow_state
    registerTool({"get_flow_state", "Get the current flow state (modules, connections, params)", {}});

    // get_available_plugins
    registerTool({"get_available_plugins", "List all available plugin modules", {}});

    // save_project
    registerTool({"save_project",
                  "Save the current project",
                  {{"path", "string", "Optional save path", false}},
                  true}); // dangerous

    // get_module_params_schema
    registerTool({"get_module_params_schema",
                  "Get the parameter schema for a specific module instance",
                  {{"instanceId", "string", "Module instance ID", true}}});

    // get_run_results
    registerTool({"get_run_results", "Get the run engine statistics and results", {}});

    // open_project
    registerTool({"open_project",
                  "Open an existing project by path",
                  {{"path", "string", "Path to the project file (.dproj)", true}}});

    // pause_flow
    registerTool({"pause_flow", "Pause the currently running flow", {}});

    // resume_flow
    registerTool({"resume_flow", "Resume a paused flow", {}});

    // read_documentation
    registerTool({"read_documentation",
                  "Read DeepLux module/usage documentation. Use this to learn about "
                  "module parameters, workflows, and best practices when you're unsure.",
                  {{"topic", "string", "Topic to look up (e.g. 'FindCircle', 'workflow', 'GrabImage params')", true}}});

    // sam_predict（SAM 批3）：经本地 SAM 服务对磁盘图像做单次 prompt 分割。
    // 非 readOnly（触发外部推理而非观察应用状态）、非 dangerous（不写任何文件）
    registerTool({"sam_predict",
                  "Run SAM segmentation on an image file with point/box prompts via the local "
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
                   {"serverUrl", "string", "SAM server base URL", false, QJsonValue("http://127.0.0.1:8990")},
                   {"timeoutMs", "integer", "Per-request timeout in ms, [100,120000]", false, QJsonValue(30000)}},
                  true}); // dangerous（写文件）
}

} // namespace DeepLux
