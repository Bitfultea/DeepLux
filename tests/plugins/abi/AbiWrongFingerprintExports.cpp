#include "core/interface/PluginAbi.h"
#include "core/platform/Platform.h"

extern "C" DEEPLUX_PLUGIN_API quint32 deeplux_plugin_abi_version() noexcept {
    return DeepLux::pluginAbiVersion();
}

extern "C" DEEPLUX_PLUGIN_API const char* deeplux_plugin_abi_fingerprint() noexcept {
    return "intentionally-wrong-abi-fingerprint";
}
