#!/usr/bin/env python3
"""Generate a conservative current Stage 8 plugin and operation evidence matrix."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import platform
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_PATH = ROOT / "docs/baseline/agent-capability-matrix.json"
INVENTORY_PATH = ROOT / "docs/baseline/agent-g8-plugin-coverage-inventory.json"
OPERATIONS_PATH = ROOT / "docs/baseline/agent-gui-operations.json"
JSON_PATH = ROOT / "docs/baseline/agent-g8-current-acceptance-matrix.json"
MARKDOWN_PATH = ROOT / "docs/baseline/agent-g8-current-acceptance-matrix.md"
INVENTORY_AUDIT = ROOT / "scripts/audit_agent_g8_plugin_coverage.py"
DEFAULT_CTEST_LOG = ROOT / "build-codex-agent/agent-phase8-slice52-evidence/ctest-full.log"

_spec = importlib.util.spec_from_file_location("g8_inventory_audit", INVENTORY_AUDIT)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"Cannot import inventory audit helper: {INVENTORY_AUDIT}")
_inventory_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_inventory_module)


def _read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _cache_values(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise ValueError(f"missing configured build cache: {path}")
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("//") or line.startswith("#") or ":" not in line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.split(":", 1)[0]
        values[key] = value
    return values


def _selected_qt(root: Path, cache: dict[str, str]) -> dict:
    requested_major = 6 if cache.get("USE_QT6", "OFF").upper() in {"ON", "TRUE", "1"} else 5
    link_path = root / "build-codex-agent/tests/CMakeFiles/test_mainwindow.dir/link.txt"
    if link_path.is_file():
        text = link_path.read_text(encoding="utf-8", errors="replace")
        majors = {int(value) for value in re.findall(r"Qt([56])(?:Core|Gui|Widgets|Test|Network|Sql|SerialPort)", text)}
        if len(majors) == 1:
            selected_major = majors.pop()
            return {
                "requestedMajor": requested_major,
                "selectedMajor": selected_major,
                "fallback": selected_major != requested_major,
                "evidencePath": link_path.relative_to(root).as_posix(),
            }
        if len(majors) > 1:
            return {
                "requestedMajor": requested_major,
                "selectedMajor": None,
                "fallback": None,
                "evidencePath": link_path.relative_to(root).as_posix(),
            }

    package_majors = set()
    for major in (5, 6):
        package_dir = cache.get(f"Qt{major}Core_DIR", "")
        if package_dir and "NOTFOUND" not in package_dir.upper():
            package_majors.add(major)
    if len(package_majors) == 1:
        selected_major = package_majors.pop()
        return {
            "requestedMajor": requested_major,
            "selectedMajor": selected_major,
            "fallback": selected_major != requested_major,
            "evidencePath": "build-codex-agent/CMakeCache.txt",
        }
    return {
        "requestedMajor": requested_major,
        "selectedMajor": None,
        "fallback": None,
        "evidencePath": "build-codex-agent/CMakeCache.txt",
    }


def _resolve_binary_path(root: Path, plugin: dict) -> str | None:
    configured_path = plugin.get("binaryPath")
    candidates = []
    target = plugin.get("cmakeTarget")
    if target:
        candidates.extend(
            root / path
            for path in (
                f"build-codex-agent/lib/lib{target}.so",
                f"build-codex-agent/lib/lib{target}.dylib",
                f"build-codex-agent/lib/{target}.dll",
                f"build-codex-agent/bin/{target}.dll",
            )
        )
    if configured_path:
        candidates.append(root / configured_path)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.relative_to(root).as_posix()
    return None


def classify_plugin(plugin: dict) -> str:
    business = plugin.get("validBusinessFixtureStatus", "NOT_AUDITED")
    failure = plugin.get("criticalFailureFixtureStatus", "NOT_AUDITED")
    binary = bool(plugin.get("binaryPresentNow"))
    if not binary and plugin.get("platform") not in {"any", "linux"}:
        return "BLOCKED_PLATFORM"
    if not binary and plugin.get("buildDisabledReason"):
        return "BLOCKED_SDK_OR_BUILD_OPTION"
    if business == "FAIL":
        return "FAIL_BUSINESS_FIXTURE"
    if not binary:
        return "NOT_BUILT"
    if business == "PASS" and failure == "PASS" and plugin.get("fixtureAuditReviewStatus") == "PASS":
        return "PASS_SOFTWARE_FIXTURES"
    return "NOT_AUDITED"


def classify_operation(operation: dict, root: Path, passed_suites: set[str]) -> str:
    test_id = operation.get("testId")
    evidence_path = operation.get("evidencePath")
    if not test_id and not evidence_path:
        return "NOT_LINKED"
    if not test_id or not evidence_path:
        raise ValueError(f"incomplete operation evidence reference: {operation.get('operationId')}")
    if test_id and evidence_path:
        source_path = root / evidence_path
        suite = source_path.stem
        cmake_sources_path = root / "tests/CMakeLists.txt"
        registered_sources = _inventory_module.test_sources_from_cmake(cmake_sources_path)
        if source_path.parent != cmake_sources_path.parent or source_path.name not in registered_sources:
            raise ValueError(f"operation evidence source is not registered in tests/CMakeLists.txt: {operation.get('operationId')} {evidence_path}")
        if not source_path.is_file():
            raise ValueError(f"operation evidence source does not exist: {operation.get('operationId')} {evidence_path}")
        if suite not in passed_suites:
            raise ValueError(f"operation evidence suite is not passing in full CTest: {operation.get('operationId')} {suite}")
        case_names = _inventory_module.qt_test_case_names(source_path.read_text(encoding="utf-8", errors="replace"))
        if test_id not in case_names:
            raise ValueError(f"operation testId does not resolve in source: {operation.get('operationId')} {test_id}")
        return "EVIDENCE_LINKED"
    return "NOT_LINKED"


def classify_operation_mapping(operation: dict) -> str:
    mapping = operation.get("toolMapping")
    if mapping == "未支持":
        return "UNSUPPORTED"
    if mapping == "人机协作":
        return "HUMAN_HANDOFF"
    if mapping == "通用工具组合":
        return "TOOL_COMPOSITION"
    if mapping == "直接工具":
        return "DIRECT_TOOL"
    return "UNKNOWN_MAPPING"


def _validate_unique(rows: list[dict], identity_key: str, label: str) -> dict[str, dict]:
    by_id: dict[str, dict] = {}
    for row in rows:
        identity = row.get(identity_key)
        if not identity:
            raise ValueError(f"{label} missing {identity_key}")
        if identity in by_id:
            raise ValueError(f"duplicate {label} identity: {identity}")
        by_id[identity] = row
    return by_id


def build_matrix(root: Path = ROOT, ctest_log: Path = DEFAULT_CTEST_LOG) -> dict:
    baseline = _read_json(root / BASELINE_PATH.relative_to(ROOT))
    inventory = _read_json(root / INVENTORY_PATH.relative_to(ROOT))
    operations = _read_json(root / OPERATIONS_PATH.relative_to(ROOT))
    baseline_plugins = _validate_unique(baseline.get("plugins", []), "pluginId", "historical plugin")
    inventory_plugins = _validate_unique(inventory.get("plugins", []), "pluginId", "inventory plugin")
    operation_rows = _validate_unique(operations, "operationId", "GUI operation")
    if set(baseline_plugins) != set(inventory_plugins):
        raise ValueError("historical baseline and current inventory plugin IDs differ")
    baseline_operation_rows = _validate_unique(baseline.get("operations", []), "operationId", "baseline operation")
    if set(operation_rows) != set(baseline_operation_rows):
        raise ValueError("GUI operation IDs differ from historical baseline")
    if len(inventory_plugins) != 69 or len(operation_rows) != 126:
        raise ValueError(f"unexpected denominator: plugins={len(inventory_plugins)}, operations={len(operation_rows)}")

    ctest_log = ctest_log.resolve()
    fixture_ctest_log = root / inventory["fullCtestLog"]
    fresh_inventory = _inventory_module.build_inventory(fixture_ctest_log)
    if fresh_inventory != inventory:
        raise ValueError("fixture candidate inventory is stale; regenerate it against its reviewed fixture evidence log")
    test_rows = _inventory_module.ctest_result_rows(ctest_log)
    passed_suites = {row[3] for row in test_rows if row[4] == "Passed"}
    current_ctest_summary = _inventory_module.full_ctest_summary(ctest_log)
    if current_ctest_summary is None:
        raise ValueError(f"current full CTest log is incomplete or failed: {ctest_log}")

    build_dir = root / "build-codex-agent"
    cache = _cache_values(build_dir / "CMakeCache.txt")
    configured_home = Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve()
    if configured_home != root.resolve():
        raise ValueError(f"configured build belongs to a different source tree: {configured_home}")

    plugins = []
    for plugin_id, current in inventory_plugins.items():
        historical = baseline_plugins[plugin_id]
        fixture_status = current["fixtureAuditReviewStatus"]
        status_inputs = dict(current)
        status_inputs["buildDisabledReason"] = historical.get("buildDisabledReason")
        status = classify_plugin(status_inputs)
        reason = ""
        if status == "FAIL_BUSINESS_FIXTURE":
            reason = "Reviewed business fixture fails; see fixture assertion and cited test evidence."
        elif status == "BLOCKED_PLATFORM":
            reason = f"No current binary for target platform {current.get('platform')}; field/platform evidence remains outstanding."
        elif status == "BLOCKED_SDK_OR_BUILD_OPTION":
            reason = current.get("buildDisabledReason") or "Required SDK/build option unavailable."
        elif status == "NOT_AUDITED":
            reason = "No independently reviewed valid business and critical-failure fixture pair."
        elif status == "PASS_SOFTWARE_FIXTURES":
            reason = "Independent software fixture review passed; this is not field acceptance."
        audit_ref = current.get("fixtureAuditEvidencePath")
        audit_test = current.get("fixtureAuditTestId")
        binary_path = _resolve_binary_path(root, historical) if current["binaryPresentNow"] else None
        binary_file = root / binary_path if binary_path else None
        if current["binaryPresentNow"] and (binary_file is None or not binary_file.is_file()):
            raise ValueError(f"current binary is missing despite inventory status: {plugin_id} {binary_path}")
        plugins.append(
            {
                "pluginId": plugin_id,
                "name": current["name"],
                "dirName": current["dirName"],
                "kind": current["kind"],
                "platform": current.get("platform"),
                "currentBinaryPresent": bool(current["binaryPresentNow"]),
                "currentBinaryPath": binary_path,
                "currentBinarySha256": _sha256(binary_file) if binary_file else None,
                "historicalBuildEnabled": historical.get("buildEnabled"),
                "fixtureAuditStatus": fixture_status,
                "businessFixtureStatus": current["validBusinessFixtureStatus"],
                "criticalFailureFixtureStatus": current["criticalFailureFixtureStatus"],
                "fixtureTestId": audit_test,
                "fixtureEvidencePath": audit_ref,
                "fieldAcceptanceStatus": "NOT_RUN",
                "currentSoftwareStatus": status,
                "reason": reason,
            }
        )

    operation_matrix = []
    for operation_id, operation in operation_rows.items():
        row = dict(operation)
        row["mappingStatus"] = classify_operation_mapping(operation)
        row["evidenceStatus"] = classify_operation(operation, root, passed_suites)
        if row["evidenceStatus"] == "EVIDENCE_LINKED":
            row["evidenceScope"] = "A cited QtTest case exists in a suite passing in the current full CTest log; semantic review is separate."
        else:
            row["evidenceScope"] = "No resolving per-operation test/source evidence link is recorded; mapping status is separate."
        operation_matrix.append(row)

    binary_count = sum(plugin["currentBinaryPresent"] for plugin in plugins)
    historical_binary_count = int(baseline.get("counts", {}).get("builtBinaries", 0))
    added_binaries = [
        plugin["pluginId"]
        for plugin in plugins
        if plugin["currentBinaryPresent"] and not plugin["historicalBuildEnabled"]
    ]
    removed_binaries = [
        plugin["pluginId"]
        for plugin in plugins
        if not plugin["currentBinaryPresent"] and plugin["historicalBuildEnabled"]
    ]
    plugin_status_counts = {key: sum(item["currentSoftwareStatus"] == key for item in plugins) for key in sorted({item["currentSoftwareStatus"] for item in plugins})}
    operation_status_counts = {key: sum(item["evidenceStatus"] == key for item in operation_matrix) for key in sorted({item["evidenceStatus"] for item in operation_matrix})}
    operation_mapping_counts = {key: sum(item["mappingStatus"] == key for item in operation_matrix) for key in sorted({item["mappingStatus"] for item in operation_matrix})}
    business_fixture_counts = {
        key: sum(item["businessFixtureStatus"] == key for item in plugins)
        for key in sorted({item["businessFixtureStatus"] for item in plugins})
    }
    failure_fixture_counts = {
        key: sum(item["criticalFailureFixtureStatus"] == key for item in plugins)
        for key in sorted({item["criticalFailureFixtureStatus"] for item in plugins})
    }
    cache_keys = ("ENABLE_HYMSON3D", "ENABLE_CAMERA_HIKVISION", "USE_QT6", "CMAKE_BUILD_TYPE")

    return {
        "schemaVersion": 1,
        "purpose": "current G8 evidence matrix; incomplete rows remain explicit and this artifact is not an overall G8 pass",
        "sources": {
            "historicalBaseline": "docs/baseline/agent-capability-matrix.json",
            "currentInventory": "docs/baseline/agent-g8-plugin-coverage-inventory.json",
            "fixtureAudits": "docs/baseline/agent-g8-plugin-fixture-audits.json",
            "guiOperations": "docs/baseline/agent-gui-operations.json",
            "fixtureAuditCtestLog": inventory["fullCtestLog"],
            "currentFullCtestLog": ctest_log.relative_to(root).as_posix() if ctest_log.is_relative_to(root) else str(ctest_log),
            "currentFullCtestLogSha256": _sha256(ctest_log),
        },
        "currentBuild": {
            "sourceDirectory": configured_home.as_posix(),
            "buildDirectory": "build-codex-agent",
            "platform": platform.system().lower(),
            "options": {key: cache[key] for key in cache_keys if key in cache},
            "qtSelection": _selected_qt(root, cache),
            "binaryCount": binary_count,
        },
        "historicalDelta": {
            "historicalBinaryCount": historical_binary_count,
            "currentBinaryCount": binary_count,
            "binaryCountDelta": binary_count - historical_binary_count,
            "newlyPresentPluginIds": sorted(added_binaries),
            "noLongerPresentPluginIds": sorted(removed_binaries),
        },
        "fixtureAuditCtest": inventory["fullCtestSummary"],
        "currentFullCtest": {
            "total": current_ctest_summary[0],
            "passed": current_ctest_summary[1],
            "skipped": current_ctest_summary[2],
            "failed": current_ctest_summary[3],
        },
        "counts": {
            "plugins": len(plugins),
            "currentBinaries": binary_count,
            "softwareFixtureStatus": plugin_status_counts,
            "businessFixtureStatus": business_fixture_counts,
            "criticalFailureFixtureStatus": failure_fixture_counts,
            "operations": len(operation_matrix),
            "operationEvidenceStatus": operation_status_counts,
            "operationMappingStatus": operation_mapping_counts,
            "fieldAccepted": 0,
        },
        "overallStatus": "NOT_READY",
        "plugins": plugins,
        "operations": operation_matrix,
    }


def render_markdown(data: dict) -> str:
    counts = data["counts"]
    lines = [
        "# G8 当前验收矩阵（进行中；不是最终 PASS）",
        "",
        "> 本矩阵把当前插件 fixture、软件操作证据、历史基线差异和现场验收分开记录。`PASS_SOFTWARE_FIXTURES` 只表示独立审核的本地业务与关键失败 fixture 通过，不代表真实设备、模型/provider、GPU、Windows 或现场验收通过。",
        "",
        f"- 当前源码插件：{counts['plugins']}；当前构建二进制：{counts['currentBinaries']}。",
        f"- Qt 选择：请求 Qt{data['currentBuild']['qtSelection']['requestedMajor']}；实际链接 Qt{data['currentBuild']['qtSelection']['selectedMajor'] if data['currentBuild']['qtSelection']['selectedMajor'] is not None else '未知'}；回退：{str(data['currentBuild']['qtSelection']['fallback']).lower()}。",
        f"- 最新全量 CTest：{data['currentFullCtest']['passed']}/{data['currentFullCtest']['total']} 通过，{data['currentFullCtest']['skipped']} 跳过；独立 fixture 审核日志：{data['fixtureAuditCtest']['passed']}/{data['fixtureAuditCtest']['total']} 通过。",
        f"- 历史/当前二进制差异：{data['historicalDelta']['historicalBinaryCount']} → {data['historicalDelta']['currentBinaryCount']}（新增存在：{', '.join(data['historicalDelta']['newlyPresentPluginIds']) or '无'}）。",
        f"- 独立 fixture 审核：业务 `{json.dumps(counts['businessFixtureStatus'], ensure_ascii=False, sort_keys=True)}`；关键失败 `{json.dumps(counts['criticalFailureFixtureStatus'], ensure_ascii=False, sort_keys=True)}`。",
        f"- 插件软件状态：`{json.dumps(counts['softwareFixtureStatus'], ensure_ascii=False, sort_keys=True)}`。",
        f"- GUI 操作：{counts['operations']}；映射类型：`{json.dumps(counts['operationMappingStatus'], ensure_ascii=False, sort_keys=True)}`；证据链接：`{json.dumps(counts['operationEvidenceStatus'], ensure_ascii=False, sort_keys=True)}`。",
        f"- 现场验收：{counts['fieldAccepted']}；总状态：`{data['overallStatus']}`。",
        "",
        "## 插件 fixture 与现场状态",
        "",
        "| Plugin ID | 插件 | 平台 | 当前二进制 | SHA-256 (前12) | 业务 fixture | 关键失败 | fixture 审核 | 现场 | 当前软件状态 | 原因 / 证据 |",
        "|---|---|---|---:|---|---|---|---|---|---|---|",
    ]
    for plugin in data["plugins"]:
        ref = plugin.get("fixtureTestId") or plugin.get("reason") or "—"
        evidence = plugin.get("fixtureEvidencePath")
        if evidence:
            ref += f" (`{evidence}`)"
        binary_hash = (plugin["currentBinarySha256"] or "")[:12] or "—"
        lines.append(
            f"| `{plugin['pluginId']}` | {plugin['dirName']} | {plugin.get('platform') or '未声明'} | "
            f"{plugin['currentBinaryPresent']} | `{binary_hash}` | {plugin['businessFixtureStatus']} | "
            f"{plugin['criticalFailureFixtureStatus']} | {plugin['fixtureAuditStatus']} | "
            f"{plugin['fieldAcceptanceStatus']} | {plugin['currentSoftwareStatus']} | {ref} |"
        )
    lines += [
        "",
        "## GUI 操作证据索引",
        "",
        "映射类型和证据链接是独立维度。`EVIDENCE_LINKED` 仅表示测试函数与通过的 CTest suite 可解析；它仍需对应阶段/操作的语义审核。`HUMAN_HANDOFF` 和 `UNSUPPORTED` 是明确的产品映射状态，不作为自动化通过项。",
        "",
        "| Operation ID | 操作 | 映射 | 阶段 | 映射状态 | 证据状态 | Test ID / 说明 |",
        "|---|---|---|---:|---|---|---|",
    ]
    for operation in data["operations"]:
        test_ref = operation.get("testId") or operation.get("evidenceScope", "")
        if operation.get("evidencePath"):
            test_ref += f" (`{operation['evidencePath']}`)"
        lines.append(
            f"| `{operation['operationId']}` | {operation['description']} | {operation['toolMapping']} | "
            f"{operation['phase']} | {operation['mappingStatus']} | {operation['evidenceStatus']} | {test_ref} |"
        )
    lines += [
        "",
        "## 判定边界",
        "",
        "- 插件 business/failure fixture 状态来自独立复审记录；源名匹配不升级为 fixture PASS。",
        "- 操作证据未链接的行保留 `NOT_LINKED`；不从插件 fixture 或通用权限机制推断操作闭环。",
        "- DefectDetection 算法 fixture 的失败、DirectShow/Hikvision 的目标环境限制和所有 0/69 现场状态均保持可见。",
        "- 历史能力矩阵中的构建数及运行时状态只作为历史快照；当前 build options、二进制数量和 CTest 摘要来自当前构建与 G8 证据日志。",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctest-log", type=Path, default=DEFAULT_CTEST_LOG)
    parser.add_argument("--check", action="store_true", help="validate generated JSON/Markdown without rewriting")
    args = parser.parse_args()
    try:
        data = build_matrix(ROOT, args.ctest_log)
    except (OSError, ValueError, json.JSONDecodeError, KeyError) as error:
        print(f"G8 acceptance matrix audit failed: {error}", file=sys.stderr)
        return 2
    json_text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    markdown_text = render_markdown(data)
    if args.check:
        if not JSON_PATH.is_file() or JSON_PATH.read_text(encoding="utf-8") != json_text:
            print("G8 acceptance JSON is stale; regenerate it", file=sys.stderr)
            return 1
        if not MARKDOWN_PATH.is_file() or MARKDOWN_PATH.read_text(encoding="utf-8") != markdown_text:
            print("G8 acceptance Markdown is stale; regenerate it", file=sys.stderr)
            return 1
        print(
            f"G8 current acceptance matrix OK: {data['counts']['plugins']} plugins, "
            f"{data['counts']['operations']} operations, overall {data['overallStatus']}"
        )
        return 0
    JSON_PATH.write_text(json_text, encoding="utf-8")
    MARKDOWN_PATH.write_text(markdown_text, encoding="utf-8")
    print(
        f"G8 current acceptance matrix generated: {data['counts']['plugins']} plugins, "
        f"{data['counts']['operations']} operations, overall {data['overallStatus']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
