# Plugin ABI Contract

DeepLux validates a plugin's build fingerprint before creating its Qt plugin instance. Every production plugin binary must define both C exports itself:

- `deeplux_plugin_abi_version()`
- `deeplux_plugin_abi_fingerprint()`

The loader rejects a missing export, an export resolved from another shared library, or a version/fingerprint mismatch before calling `QPluginLoader::instance()`. Rejection is intentional; there is no legacy-vtable fallback. Rebuild plugins against the same DeepLux headers, compiler ABI, Qt build, architecture, and runtime configuration as the application.

`DEEPLUX_PLUGIN_ABI_VERSION` is the plugin binary compatibility version. It is distinct from `DEEPLUX_MODULE_INTERFACE_VERSION`, which identifies the `IModule` Qt interface/IID. Increment the plugin ABI version when a binary compatibility change requires all plugins to be rebuilt; update the module interface version when the Qt interface contract changes.

The exported numeric fingerprint includes compiler family, Qt major/minor, and plugin ABI version. The readable fingerprint additionally records compiler/runtime details, architecture, iterator/debug settings, Qt version, selected public structure sizes, and interface IIDs. Both must match exactly at load time.

## Validation

- `PluginManager` validates both exports and their ownership before plugin instantiation.
- ABI fixture tests cover missing exports, wrong versions/fingerprints, and symbols forwarded from a dependency.
- `scripts/check_plugin_abi_exports.py` checks that each built production plugin defines the required symbols; it is a release/CI backstop, not a substitute for runtime validation.
- A release build with `BUILD_TESTS=OFF` must not expose test hooks in the core library or packaged binaries.

When an ABI value changes, rebuild and synchronize the complete plugin set. Do not mix old user-installed plugins with new application binaries; remove or replace them using the supported plugin deployment flow.
