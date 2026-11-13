#include "PluginAbi.h"

#if defined(_WIN32) || defined(_WIN64)
#define DEEPLUX_PLUGIN_ABI_EXPORT __declspec(dllexport)
#else
#define DEEPLUX_PLUGIN_ABI_EXPORT __attribute__((visibility("default")))
#endif

extern "C" DEEPLUX_PLUGIN_ABI_EXPORT quint32 deeplux_plugin_abi_version() noexcept {
    return DeepLux::pluginAbiVersion();
}

extern "C" DEEPLUX_PLUGIN_ABI_EXPORT const char* deeplux_plugin_abi_fingerprint() noexcept {
    return DeepLux::pluginAbiFingerprint();
}
