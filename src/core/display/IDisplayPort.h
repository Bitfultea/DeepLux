#pragma once

#include <QString>

// Forward declaration instead of include
namespace DeepLux {
class DisplayData;
}

namespace DeepLux {

/**
 * @brief Optional interface for modules that produce display output
 *
 * This interface is COMPLETELY SEPARATE from IModule.
 * Modules can implement this alongside IModule if they have display output.
 *
 * Note: Does NOT inherit from QObject to avoid multiple QObject inheritance issues.
 * Use dynamic_cast to check if a QObject implements this interface.
 *
 * Implementation:
 *   class MyPlugin : public ModuleBase, public IDisplayPort {
 *       ...
 *   };
 *
 * To check if a QObject implements IDisplayPort:
 *   if (auto* port = dynamic_cast<IDisplayPort*>(someQObject)) {
 *       port->hasDisplayOutput();
 *   }
 */
class IDisplayPort {
public:
    virtual ~IDisplayPort() = default;

    // Check if this module has display output
    virtual bool hasDisplayOutput() const = 0;

    // Get the current display data (called by DisplayManager)
    virtual DisplayData getDisplayData() const = 0;

    // Which viewport does this module prefer? (empty = any available)
    virtual QString preferredViewport() const = 0;
};

// Helper to check if a QObject implements IDisplayPort via dynamic_cast
// This works because both IDisplayPort (has virtual dtor) and QObject (has virtual dtor) are polymorphic
template<typename T>
inline IDisplayPort* getDisplayPort(T* obj) {
    if (!obj) return nullptr;
    return dynamic_cast<IDisplayPort*>(obj);
}

} // namespace DeepLux