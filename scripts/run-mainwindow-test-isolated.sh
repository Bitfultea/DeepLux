#!/usr/bin/env bash
set -euo pipefail

project_dir="$1"
build_dir="$2"
test_binary="$3"
shift 3

test_home=$(mktemp -d "${TMPDIR:-/tmp}/deeplux-mainwindow-home.XXXXXX")
trap 'rm -rf -- "$test_home"' EXIT

env -u DEEPLUX_APP_DATA_DIR HOME="$test_home" bash "$project_dir/scripts/sync-plugins.sh" "$build_dir"
if [[ "$(basename "$test_binary")" == "test_mainwindow" ]]; then
    # These plugins are also linked into test_mainwindow; use the same ELF files to avoid loading duplicate Qt metadata.
    for plugin in CropImage NPointCalibration; do
        library="$build_dir/lib/lib${plugin}Plugin.so"
        installed="$test_home/.deeplux/plugins/$plugin/lib${plugin}Plugin.so"
        if [[ -f "$library" && -f "$installed" ]]; then
            ln -sf -- "$library" "$installed"
        fi
    done
fi
env -u DEEPLUX_APP_DATA_DIR HOME="$test_home" "$test_binary" "$@"
