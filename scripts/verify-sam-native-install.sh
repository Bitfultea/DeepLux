#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "Usage: bash $0 INSTALL_PREFIX [LIBDIR]" >&2
    exit 2
fi

prefix="$(realpath -m "$1")"
libdir="${2:-lib}"
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

echo "PASS: relative RUNPATH, bundled ONNX Runtime, and resolvable host dependencies"
