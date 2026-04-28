#include "GuiEvent.h"

namespace DeepLux {

QString GuiEvent::typeString() const
{
    switch (type) {
    case GuiEventType::ProjectCreated:     return "project_created";
    case GuiEventType::ProjectOpened:      return "project_opened";
    case GuiEventType::ProjectSaved:       return "project_saved";
    case GuiEventType::ProjectClosed:      return "project_closed";
    case GuiEventType::ModuleAdded:        return "module_added";
    case GuiEventType::ModuleRemoved:      return "module_removed";
    case GuiEventType::ModuleReordered:    return "module_reordered";
    case GuiEventType::PropertyChanged:    return "property_changed";
    case GuiEventType::ConnectionCreated:  return "connection_created";
    case GuiEventType::ConnectionRemoved:  return "connection_removed";
    case GuiEventType::RunStarted:         return "run_started";
    case GuiEventType::RunFinished:        return "run_finished";
    case GuiEventType::ModuleStarted:      return "module_started";
    case GuiEventType::ModuleFinished:     return "module_finished";
    case GuiEventType::PluginLoaded:       return "plugin_loaded";
    case GuiEventType::PluginUnloaded:     return "plugin_unloaded";
    case GuiEventType::ImageDisplayed:     return "image_displayed";
    case GuiEventType::LogAdded:           return "log_added";
    default:                               return "unknown";
    }
}

QJsonObject GuiEvent::toJson() const
{
    QJsonObject obj;
    obj["timestamp"] = timestamp.toString(Qt::ISODate);
    obj["type"] = typeString();
    obj["source"] = source;
    obj["details"] = details;
    return obj;
}

} // namespace DeepLux
