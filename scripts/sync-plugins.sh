#!/bin/bash
# =============================================================================
# DeepLux Plugin Sync Script
# =============================================================================
# 将本次构建的插件 .so 暂存到可迁移的构建树目录，不修改用户插件目录。
#
# 用法:
#   ./scripts/sync-plugins.sh [BUILD_DIR] [SOURCE_DIR] [DESTINATION]
#
#   BUILD_DIR  可选，CMake 构建目录（默认: 脚本所在目录的父目录，即项目根目录）
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${1:-$PROJECT_DIR}"
SOURCE_DIR="${2:-$PROJECT_DIR}"
PLUGINS_HOME="${3:-$BUILD_DIR/plugins}"

mkdir -p "$PLUGINS_HOME"
manifest_entries=$(mktemp)
trap 'rm -f -- "$manifest_entries"' EXIT

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
declare -A SO_MAP
for so in "$BUILD_DIR"/lib/lib*Plugin.so "$BUILD_DIR"/plugins/camera/lib*Plugin.so; do
    [ -f "$so" ] || continue
    # 从文件名提取插件名: libFooPlugin.so -> Foo
    base=$(basename "$so")
    name=$(echo "$base" | sed 's/^lib//;s/Plugin\.so$//')
    SO_MAP["$name"]="$so"
done

if [ ${#SO_MAP[@]} -eq 0 ]; then
    echo "[WARN] No plugin .so files found in $BUILD_DIR; staging an empty build manifest"
fi

echo "[INFO] Found ${#SO_MAP[@]} plugin .so files in build directory"
echo "[INFO] Target runtime plugins: $PLUGINS_HOME"
echo ""

copied=0
skipped=0
cleaned=0
prepared=0

# 新增插件首次构建时，暂存目录下还没有对应目录。
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
done < <(find "$SOURCE_DIR/src/plugins" -path '*/metadata.json' -print0)

# 遍历暂存目录下的每个插件目录
for plugin_dir in "$PLUGINS_HOME"/*/; do
    [ -d "$plugin_dir" ] || continue
    [ -f "$plugin_dir/metadata.json" ] || continue

    # 读取 metadata.json 中的 name 字段
    meta_name=$(json_field "$plugin_dir/metadata.json" "name")

    [ -n "$meta_name" ] || continue

    dir_name=$(basename "$plugin_dir")

    # 清理该目录下不匹配的 .so 文件（防止旧脚本错误导致的多余 .so 堆积）
    short_dir_name="${dir_name%Plugin}"
    for existing_so in "$plugin_dir"/*.so; do
        [ -f "$existing_so" ] || continue
        existing_base=$(basename "$existing_so")
        existing_name=$(echo "$existing_base" | sed 's/^lib//;s/Plugin\.so$//')
        if [ "$existing_name" != "$meta_name" ] && [ "$existing_name" != "$dir_name" ] &&
            [ "$existing_name" != "$short_dir_name" ]; then
            rm -f "$existing_so"
            echo "[CLEAN] $dir_name: removed mismatched $existing_base"
            ((cleaned++)) || true
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
        printf '%s\t%s\t%s\n' "$meta_name" "$(basename "$dst_so")" "$(sha256sum "$dst_so" | cut -d' ' -f1)" >> "$manifest_entries"
    else
        echo "[MISS]  $meta_name ($dir_name): no matching .so in build directory"
    fi
done

echo ""
echo "[DONE] prepared=$prepared copied=$copied skipped=$skipped cleaned=$cleaned"

python3 - "$PLUGINS_HOME/deeplux-plugin-manifest.json" "$manifest_entries" <<'PY'
import json
import pathlib
import sys

entries = []
for line in pathlib.Path(sys.argv[2]).read_text(encoding="utf-8").splitlines():
    name, library, digest = line.split("\t")
    entries.append({"name": name, "library": library, "sha256": digest})
target = pathlib.Path(sys.argv[1])
temporary = target.with_suffix(target.suffix + ".tmp")
temporary.write_text(json.dumps({"formatVersion": 1, "mode": "development", "plugins": entries},
                               ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
temporary.replace(target)
print(f"[MANIFEST] {len(entries)} build-matched plugins: {target}")
PY
