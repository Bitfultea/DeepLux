#include "core/interface/PluginAbi.h"

#if defined(_WIN32)
#define ABI_FIXTURE_EXPORT __declspec(dllexport)
#else
#define ABI_FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" ABI_FIXTURE_EXPORT quint32 deeplux_plugin_abi_version() noexcept {
    return DeepLux::pluginAbiVersion();
}

extern "C" ABI_FIXTURE_EXPORT const char* deeplux_plugin_abi_fingerprint() noexcept {
    return DeepLux::pluginAbiFingerprint();
}

extern "C" ABI_FIXTURE_EXPORT void abi_fixture_dependency_anchor() {}
