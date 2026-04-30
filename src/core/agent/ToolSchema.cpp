#include "ToolSchema.h"

namespace DeepLux {

QJsonObject ToolParam::toJson() const
{
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
        for (const QString& v : enumValues) arr.append(v);
        obj["enum"] = arr;
    }
    return obj;
}

QJsonObject ToolDefinition::toOpenAIFunction() const
{
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

QJsonObject ToolDefinition::toClaudeTool() const
{
    // Claude Tool Use format is similar to OpenAI
    return toOpenAIFunction();
}

QJsonObject ToolDefinition::toJsonSchema() const
{
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

ToolSchema& ToolSchema::instance()
{
    static ToolSchema instance;
    return instance;
}

void ToolSchema::registerTool(const ToolDefinition& tool)
{
    // Replace if already exists
    for (int i = 0; i < m_tools.size(); ++i) {
        if (m_tools[i].name == tool.name) {
            m_tools[i] = tool;
            return;
        }
    }
    m_tools.append(tool);
}

QList<ToolDefinition> ToolSchema::allTools() const
{
    return m_tools;
}

ToolDefinition ToolSchema::findTool(const QString& name) const
{
    for (const ToolDefinition& t : m_tools) {
        if (t.name == name) return t;
    }
    return ToolDefinition();
}

bool ToolSchema::hasTool(const QString& name) const
{
    for (const ToolDefinition& t : m_tools) {
        if (t.name == name) return true;
    }
    return false;
}

QJsonArray ToolSchema::toOpenAIFunctions() const
{
    QJsonArray arr;
    for (const ToolDefinition& t : m_tools) {
        arr.append(t.toOpenAIFunction());
    }
    return arr;
}

QJsonArray ToolSchema::toClaudeTools() const
{
    QJsonArray arr;
    for (const ToolDefinition& t : m_tools) {
        arr.append(t.toClaudeTool());
    }
    return arr;
}

void ToolSchema::registerDefaultTools()
{
    // create_project
    registerTool({"create_project",
                  "Create a new project with the given name",
                  {
                      {"name", "string", "Project name", true}
                  }});

    // add_module
    registerTool({"add_module",
                  "Add a module (plugin) to the current project",
                  {
                      {"plugin", "string", "Plugin type name (e.g. GrabImage, FindCircle)", true},
                      {"instanceName", "string", "Unique instance name for this module", false}
                  }});

    // remove_module
    registerTool({"remove_module",
                  "Remove a module from the current project",
                  {
                      {"instanceId", "string", "Module instance ID to remove", true}
                  },
                  true}); // dangerous

    // set_param
    registerTool({"set_param",
                  "Set a parameter value for a module instance",
                  {
                      {"instanceId", "string", "Module instance ID", true},
                      {"key", "string", "Parameter key", true},
                      {"value", "string", "Parameter value (as string, will be converted)", true}
                  }});

    // connect_modules
    registerTool({"connect_modules",
                  "Connect two modules in the flow",
                  {
                      {"fromId", "string", "Source module instance ID", true},
                      {"toId", "string", "Target module instance ID", true},
                      {"port", "string", "Output port name (default: output)", false}
                  }});

    // disconnect_modules
    registerTool({"disconnect_modules",
                  "Disconnect two modules",
                  {
                      {"fromId", "string", "Source module instance ID", true},
                      {"toId", "string", "Target module instance ID", true}
                  }});

    // run_flow
    registerTool({"run_flow",
                  "Run the current flow once",
                  {
                      {"mode", "enum", "Run mode: once or cycle", false, QJsonValue("once"), {"once", "cycle"}}
                  }});

    // stop_flow
    registerTool({"stop_flow",
                  "Stop the current running flow",
                  {}});

    // get_flow_state
    registerTool({"get_flow_state",
                  "Get the current flow state (modules, connections, params)",
                  {}});

    // get_available_plugins
    registerTool({"get_available_plugins",
                  "List all available plugin modules",
                  {}});

    // save_project
    registerTool({"save_project",
                  "Save the current project",
                  {
                      {"path", "string", "Optional save path", false}
                  },
                  true}); // dangerous

    // get_module_params_schema
    registerTool({"get_module_params_schema",
                  "Get the parameter schema for a specific module instance",
                  {
                      {"instanceId", "string", "Module instance ID", true}
                  }});

    // get_run_results
    registerTool({"get_run_results",
                  "Get the run engine statistics and results",
                  {}});

    // open_project
    registerTool({"open_project",
                  "Open an existing project by path",
                  {
                      {"path", "string", "Path to the project file (.dproj)", true}
                  }});

    // pause_flow
    registerTool({"pause_flow",
                  "Pause the currently running flow",
                  {}});

    // resume_flow
    registerTool({"resume_flow",
                  "Resume a paused flow",
                  {}});
}

} // namespace DeepLux
