#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build}"
OUTPUT_DIR="${2:-/tmp/deeplux-ui-review}"

cmake --build "${BUILD_DIR}" --target ui_capture_mainwindow -j"$(nproc)"
mkdir -p "${OUTPUT_DIR}"

QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
    "${BUILD_DIR}/bin/ui_capture_mainwindow" "${OUTPUT_DIR}"

printf 'Wrote screenshots:\n'
find "${OUTPUT_DIR}" -maxdepth 1 -type f -name '*.png' -printf '  %p\n' | sort
