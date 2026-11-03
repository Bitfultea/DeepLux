#!/usr/bin/env bash
set -euo pipefail

project_dir="$1"
build_dir="$2"
test_binary="$3"
shift 3

test_home=$(mktemp -d "${TMPDIR:-/tmp}/deeplux-mainwindow-home.XXXXXX")
trap 'rm -rf -- "$test_home"' EXIT

HOME="$test_home" bash "$project_dir/scripts/sync-plugins.sh" "$build_dir"
env -u DEEPLUX_APP_DATA_DIR HOME="$test_home" "$test_binary" "$@"
