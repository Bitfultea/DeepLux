#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build}"
OUTPUT_DIR="${2:-/tmp/deeplux-ui-review}"

cmake --build "${BUILD_DIR}" --target ui_capture_mainwindow -j"$(nproc)"
mkdir -p "${OUTPUT_DIR}"

# 阶3: 优先 xvfb-run（真实 X 显示，解决离屏截图依赖）；无 xvfb 时回退 offscreen
if command -v xvfb-run >/dev/null 2>&1; then
    xvfb-run -a -s "-screen 0 1920x1080x24" \
        "${BUILD_DIR}/bin/ui_capture_mainwindow" "${OUTPUT_DIR}"
else
    echo "xvfb-run not found; falling back to QT_QPA_PLATFORM=offscreen" >&2
    QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}" \
        "${BUILD_DIR}/bin/ui_capture_mainwindow" "${OUTPUT_DIR}"
fi

printf 'Wrote screenshots:\n'
find "${OUTPUT_DIR}" -maxdepth 1 -type f -name '*.png' -printf '  %p\n' | sort
