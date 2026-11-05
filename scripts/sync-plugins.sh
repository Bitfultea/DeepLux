#!/bin/bash
# =============================================================================
# DeepLux Plugin Sync Script
# =============================================================================
# 将 CMake 构建目录中新编译的插件 .so 同步到 ~/.deeplux/plugins/ 对应目录。
# 解决因插件库与主程序不同步导致的跨 DLL 虚表不匹配问题。
#
# 用法:
#   ./scripts/sync-plugins.sh [--force] [--plugins-home DIR] [BUILD_DIR]
#
#   BUILD_DIR       可选，CMake 构建目录（默认: <项目根>/build）
#   --force         允许删除插件目录中不匹配的旧 .so（默认只保留并告警）
#   --plugins-home  显式指定插件目录；缺省时优先 DEEPLUX_APP_DATA_DIR/plugins，
#                   再回退 $HOME/.deeplux/plugins
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# 参数解析：--force 允许删除不匹配的旧 .so；--plugins-home 显式指定目标目录。
FORCE=0
BUILD_DIR=""
PLUGINS_HOME_OVERRIDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --force) FORCE=1; shift ;;
        --plugins-home)
            if [ $# -lt 2 ]; then
                echo "[ERROR] --plugins-home requires a directory" >&2
                exit 2
            fi
            PLUGINS_HOME_OVERRIDE="$2"
            shift 2
            ;;
        -*) echo "[ERROR] Unknown option: $1" >&2; exit 2 ;;
        *) if [ -z "$BUILD_DIR" ]; then BUILD_DIR="$1"; shift; else echo "[ERROR] Unexpected argument: $1" >&2; exit 2; fi ;;
    esac
done
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build}"

# 目标插件目录解析：显式参数 > DEEPLUX_APP_DATA_DIR/plugins > $HOME/.deeplux/plugins。
# 与应用 PathUtils::pluginPath() 同源，避免隔离环境写入真实用户目录。
if [ -n "$PLUGINS_HOME_OVERRIDE" ]; then
    PLUGINS_HOME="$PLUGINS_HOME_OVERRIDE"
elif [ -n "${DEEPLUX_APP_DATA_DIR:-}" ]; then
    PLUGINS_HOME="${DEEPLUX_APP_DATA_DIR%/}/plugins"
else
    PLUGINS_HOME="${HOME}/.deeplux/plugins"
fi

mkdir -p "$PLUGINS_HOME"

json_field() {
    local json_path="$1"
    local field="$2"
    python3 - "$json_path" "$field" <<'PY'
import json
import sys

try:
    with open(sys.argv[1], encoding="utf-8") as f:
        value = json.load(f).get(sys.argv[2], "")
    print(value if isinstance(value, str) else "")
except Exception:
    sys.exit(0)
PY
}

find_so_match() {
    local meta_name="$1"
    local dir_name="$2"
    local short_dir="${dir_name%Plugin}"

    for candidate in "$meta_name" "$dir_name" "$short_dir"; do
        [ -n "$candidate" ] || continue
        if [ -n "${SO_MAP[$candidate]+x}" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

# 收集所有候选 .so 文件（普通插件 + 相机插件）
declare -A SO_MAP=()
for so in "$BUILD_DIR"/lib/lib*Plugin.so "$BUILD_DIR"/plugins/camera/lib*Plugin.so; do
    [ -f "$so" ] || continue
    # 从文件名提取插件名: libFooPlugin.so -> Foo
    base=$(basename "$so")
    name=$(echo "$base" | sed 's/^lib//;s/Plugin\.so$//')
    SO_MAP["$name"]="$so"
done

if [ "${#SO_MAP[@]}" -eq 0 ]; then
    echo "[ERROR] No plugin .so files found in $BUILD_DIR (expected lib/*Plugin.so or plugins/camera/lib*Plugin.so)" >&2
    echo "[ERROR] Pass the correct build directory, e.g. ./scripts/sync-plugins.sh build" >&2
    exit 1
fi

echo "[INFO] Found ${#SO_MAP[@]} plugin .so files in build directory"
echo "[INFO] Target plugin home: $PLUGINS_HOME"
echo ""

copied=0
skipped=0
cleaned=0
prepared=0

# 新增插件首次构建时，~/.deeplux/plugins 下还没有对应目录。
# 先根据源码 metadata 创建运行时目录，避免只同步已有插件导致 GUI 扫描不到新插件。
while IFS= read -r -d '' metadata_path; do
    plugin_src_dir=$(dirname "$metadata_path")
    dir_name=$(basename "$plugin_src_dir")
    meta_name=$(json_field "$metadata_path" "name")

    [ -n "$meta_name" ] || continue

    match_name=$(find_so_match "$meta_name" "$dir_name" || true)

    [ -n "$match_name" ] || continue

    target_dir="$PLUGINS_HOME/$dir_name"
    for existing_meta in "$PLUGINS_HOME"/*/metadata.json; do
        [ -f "$existing_meta" ] || continue
        existing_name=$(json_field "$existing_meta" "name")
        if [ "$existing_name" = "$meta_name" ]; then
            target_dir=$(dirname "$existing_meta")
            break
        fi
    done

    mkdir -p "$target_dir"
    if [ ! -f "$target_dir/metadata.json" ] || ! cmp -s "$metadata_path" "$target_dir/metadata.json"; then
        cp "$metadata_path" "$target_dir/metadata.json"
        echo "[META]  $(basename "$target_dir"): refreshed metadata.json"
        ((prepared++)) || true
    fi

    icon_name=$(json_field "$metadata_path" "icon")
    if [ -n "$icon_name" ] && [ -f "$plugin_src_dir/$icon_name" ]; then
        if [ ! -f "$target_dir/$icon_name" ] || ! cmp -s "$plugin_src_dir/$icon_name" "$target_dir/$icon_name"; then
            cp "$plugin_src_dir/$icon_name" "$target_dir/$icon_name"
        fi
    fi
done < <(find "$PROJECT_DIR/src/plugins" -path '*/metadata.json' -print0)

# 遍历 ~/.deeplux/plugins/ 下的每个插件目录
for plugin_dir in "$PLUGINS_HOME"/*/; do
    [ -d "$plugin_dir" ] || continue
    [ -f "$plugin_dir/metadata.json" ] || continue

    # 读取 metadata.json 中的 name 字段
    meta_name=$(json_field "$plugin_dir/metadata.json" "name")

    [ -n "$meta_name" ] || continue

    dir_name=$(basename "$plugin_dir")

    # 清理该目录下不匹配的 .so 文件（防止旧脚本错误导致的多余 .so 堆积）。
    # 仅在显式 --force 时删除，避免默认行为误删用户文件。
    short_dir_name="${dir_name%Plugin}"
    for existing_so in "$plugin_dir"/*.so; do
        [ -f "$existing_so" ] || continue
        existing_base=$(basename "$existing_so")
        existing_name=$(echo "$existing_base" | sed 's/^lib//;s/Plugin\.so$//')
        if [ "$existing_name" != "$meta_name" ] && [ "$existing_name" != "$dir_name" ] &&
            [ "$existing_name" != "$short_dir_name" ]; then
            if [ "$FORCE" -eq 1 ]; then
                rm -f "$existing_so"
                echo "[CLEAN] $dir_name: removed mismatched $existing_base"
                ((cleaned++)) || true
            else
                echo "[KEEP]  $dir_name: mismatched $existing_base (use --force to remove)"
            fi
        fi
    done

    # 查找匹配的构建产物并复制
    # 优先通过 metadata.json 中的 name 匹配，其次通过目录名匹配
    match_name=$(find_so_match "$meta_name" "$dir_name" || true)

    if [ -n "$match_name" ]; then
        src_so="${SO_MAP[$match_name]}"
        dst_so="$plugin_dir/$(basename "$src_so")"

        # 只在文件确实不同（或不存在）时才复制
        if [ ! -f "$dst_so" ] || ! diff -q "$src_so" "$dst_so" >/dev/null 2>&1; then
            cp -v "$src_so" "$dst_so"
            ((copied++)) || true
        else
            echo "[SKIP]  $meta_name ($dir_name): already up-to-date"
            ((skipped++)) || true
        fi
    else
        echo "[MISS]  $meta_name ($dir_name): no matching .so in build directory"
    fi
done

echo ""
echo "[DONE] prepared=$prepared copied=$copied skipped=$skipped cleaned=$cleaned"
