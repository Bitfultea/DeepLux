#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: bash $0 INSTALL_PREFIX [LIBDIR] [DATAROOTDIR]" >&2
    exit 2
fi

prefix="$(realpath -m "$1")"
libdir="${2:-lib}"
datarootdir="${3:-share}"
libroot="$(realpath -m "${prefix}/${libdir}")"
core="${libroot}/libdeeplux_core.so.1"
ui="${libroot}/libdeeplux_ui.so.1"
app="${prefix}/bin/DeepLux"

for file in "$core" "$ui" "$app" "${libroot}/libonnxruntime.so"; do
    if [[ ! -f "$file" ]]; then
        echo "Missing installed file: $file" >&2
        exit 1
    fi
done

runpath() {
    readelf -d "$1" | sed -n 's/.*Library runpath: \[\(.*\)\].*/\1/p'
}

if [[ "$(runpath "$core")" != '$ORIGIN' ||
      "$(runpath "$ui")" != '$ORIGIN' ||
      "$(runpath "$app")" != "\$ORIGIN/../${libdir}" ]]; then
    echo "Installed binaries do not use package-relative RUNPATH" >&2
    exit 1
fi

for link in "${libroot}"/libonnxruntime*.so*; do
    if [[ -L "$link" ]]; then
        resolved_link="$(realpath -e "$link")" || {
            echo "Broken ONNX Runtime symlink: $link" >&2
            exit 1
        }
        if [[ "$(readlink "$link")" = /* || "$resolved_link" != "${libroot}/"* ]]; then
            echo "External or broken ONNX Runtime symlink: $link" >&2
            exit 1
        fi
    fi
done

resolved="$(env -u LD_LIBRARY_PATH ldd "$core" | awk '/libonnxruntime\.so/{print $3; exit}')"
if [[ "$resolved" != "${libroot}/libonnxruntime.so."* ]]; then
    echo "Core loads ONNX Runtime outside install prefix: $resolved" >&2
    exit 1
fi

for library in "$app" "$core" "$ui" "${libroot}"/libonnxruntime_providers_*.so; do
    if [[ -f "$library" ]]; then
        if ! dependencies="$(env -u LD_LIBRARY_PATH ldd "$library")"; then
            echo "Cannot inspect runtime dependencies: $library" >&2
            exit 1
        fi
        if grep -q 'not found' <<< "$dependencies"; then
            echo "Unresolved runtime dependency: $library" >&2
            exit 1
        fi
    fi
done

plugin_count=0
plugin_root="${prefix}/plugins"
if [[ ! -d "$plugin_root" || -L "$plugin_root" ]]; then
    echo "Missing or linked plugin root: $plugin_root" >&2
    exit 1
fi
manifest="${prefix}/${datarootdir}/deeplux/plugins.manifest"
if [[ ! -f "$manifest" || -L "$manifest" ]]; then
    echo "Missing or linked plugin manifest: $manifest" >&2
    exit 1
fi
mapfile -t expected_plugins < "$manifest"
if [[ ${#expected_plugins[@]} -eq 0 ]]; then
    echo "Plugin manifest is empty: $manifest" >&2
    exit 1
fi
for plugin_name in "${expected_plugins[@]}"; do
    if [[ ! "$plugin_name" =~ ^[A-Za-z0-9_-]+$ || ! -d "${prefix}/plugins/${plugin_name}" ]]; then
        echo "Missing or invalid manifest plugin: $plugin_name" >&2
        exit 1
    fi
done
for plugin_dir in "${prefix}"/plugins/*; do
    [[ -d "$plugin_dir" ]] || continue
    if [[ -L "$plugin_dir" ]]; then
        echo "Linked plugin directory is not relocatable: $plugin_dir" >&2
        exit 1
    fi
    if [[ ! -f "$plugin_dir/metadata.json" ]]; then
        echo "Plugin directory has no metadata.json: $plugin_dir" >&2
        exit 1
    fi
    plugin_libraries=("$plugin_dir"/lib*Plugin.so)
    if [[ ${#plugin_libraries[@]} -ne 1 || ! -f "${plugin_libraries[0]}" ]]; then
        echo "Plugin directory must contain exactly one library: $plugin_dir" >&2
        exit 1
    fi
    plugin_library="${plugin_libraries[0]}"
    for plugin_file in "$plugin_dir/metadata.json" "$plugin_library"; do
        if [[ -L "$plugin_file" ]]; then
            echo "Linked plugin file is not relocatable: $plugin_file" >&2
            exit 1
        fi
        resolved_file="$(realpath -e "$plugin_file")" || {
            echo "Broken plugin file: $plugin_file" >&2
            exit 1
        }
        if [[ "$resolved_file" != "${prefix}/"* ]]; then
            echo "Plugin file escapes install prefix: $plugin_file" >&2
            exit 1
        fi
    done
    if [[ "$(runpath "$plugin_library")" != "\$ORIGIN/../../${libdir}" ]]; then
        echo "Plugin has non-relative RUNPATH: $plugin_library" >&2
        exit 1
    fi
    if ! dependencies="$(env -u LD_LIBRARY_PATH ldd "$plugin_library")" || grep -q 'not found' <<< "$dependencies"; then
        echo "Unresolved plugin dependency: $plugin_library" >&2
        exit 1
    fi
    resolved_core="$(awk '/libdeeplux_core\.so/{print $3; exit}' <<< "$dependencies")"
    if [[ -n "$resolved_core" && "$(realpath -e "$resolved_core")" != "$(realpath -e "$core")" ]]; then
        echo "Plugin resolves DeepLux core outside install prefix: $plugin_library" >&2
        exit 1
    fi
    ((plugin_count += 1))
done
if [[ "$plugin_count" -ne "${#expected_plugins[@]}" ]]; then
    echo "Plugin count differs from manifest: installed=$plugin_count expected=${#expected_plugins[@]}" >&2
    exit 1
fi

echo "PASS: relative RUNPATH, bundled ONNX Runtime, and ${plugin_count} packaged plugins"
