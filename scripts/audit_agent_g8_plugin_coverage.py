#!/usr/bin/env python3
"""Build a conservative Stage 8 plugin-test inventory.

This tool records source-level candidate test suites for each inventoried plugin
and whether those suites passed the supplied full CTest log. It intentionally
does not infer a valid business fixture, a critical failure fixture, or field
acceptance from a source-name match or a green suite; those fields remain
REVIEW_REQUIRED until a human maps concrete test cases and assertions.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE_PATH = ROOT / "docs/baseline/agent-capability-matrix.json"
FIXTURE_AUDITS_PATH = ROOT / "docs/baseline/agent-g8-plugin-fixture-audits.json"
JSON_PATH = ROOT / "docs/baseline/agent-g8-plugin-coverage-inventory.json"
MARKDOWN_PATH = ROOT / "docs/baseline/agent-g8-plugin-coverage-inventory.md"
DEFAULT_CTEST_LOG = ROOT / "build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log"


def test_sources_from_cmake(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8")
    sources: list[str] = []
    in_list = False
    for line in text.splitlines():
        if re.match(r"\s*set\(TEST_SOURCES\s*$", line):
            in_list = True
            continue
        if in_list and re.match(r"\s*\)\s*$", line):
            in_list = False
            continue
        if in_list:
            match = re.match(r"\s*(test_[A-Za-z0-9_]+\.cpp)\s*$", line)
            if match:
                sources.append(match.group(1))
    for match in re.finditer(r"list\(APPEND\s+TEST_SOURCES\s+(test_[A-Za-z0-9_]+\.cpp)\s*\)", text):
        sources.append(match.group(1))
    for match in re.finditer(
        r"add_executable\(\s*test_[A-Za-z0-9_]+\s+(test_[A-Za-z0-9_]+\.cpp)\s*\)", text
    ):
        sources.append(match.group(1))
    return sorted(set(sources))


def ctest_result_rows(path: Path) -> list[tuple[int, int, int, str, str]]:
    text = path.read_text(encoding="utf-8", errors="replace")
    pattern = re.compile(
        r"^\s*(\d+)/(\d+) Test\s+#(\d+):\s+(\S+)\s+\.+\s+(Passed|Skipped|Failed)\b",
        re.MULTILINE,
    )
    return [
        (int(match.group(1)), int(match.group(2)), int(match.group(3)), match.group(4), match.group(5))
        for match in pattern.finditer(text)
    ]


def full_ctest_summary(path: Path) -> tuple[int, int, int, int] | None:
    text = path.read_text(encoding="utf-8", errors="replace")
    summaries = re.findall(r"100% tests passed,\s+0 tests failed out of (\d+)", text)
    if len(summaries) != 1:
        return None
    total = int(summaries[0])
    rows = ctest_result_rows(path)
    if len(rows) != total:
        return None
    if any(row[1] != total for row in rows):
        return None
    if {row[0] for row in rows} != set(range(1, total + 1)):
        return None
    if {row[2] for row in rows} != set(range(1, total + 1)):
        return None
    if len({row[0] for row in rows}) != total or len({row[3] for row in rows}) != total:
        return None
    if any(row[4] == "Failed" for row in rows):
        return None
    passed = sum(row[4] == "Passed" for row in rows)
    skipped = sum(row[4] == "Skipped" for row in rows)
    if passed + skipped != total:
        return None
    return total, passed, skipped, 0


def passed_ctest_suites(path: Path) -> set[str]:
    return {row[3] for row in ctest_result_rows(path) if row[4] == "Passed"}


def source_inventory() -> set[tuple[str, str]]:
    result = set()
    for path in sorted((ROOT / "src/plugins").glob("**/metadata.json")):
        metadata = json.loads(path.read_text(encoding="utf-8"))
        result.add((path.parent.name, metadata.get("id", "")))
    return result


def source_mentions_plugin(text: str, plugin: dict) -> list[str]:
    token = re.escape(plugin["dirName"])
    plugin_subpath = re.escape(plugin["sourcePath"].removeprefix("src/plugins/"))
    target = re.escape(plugin.get("cmakeTarget") or "")
    if re.search(r"plugins/" + plugin_subpath + r"/[^\"\s>]+Plugin\.h", text):
        return ["direct_plugin_header"]
    if target and re.search(r"(?<![A-Za-z0-9_])" + target + r"(?![A-Za-z0-9_])", text):
        return ["direct_plugin_symbol"]
    exact_name = r"(?<![A-Za-z0-9_])" + token + r"(?![A-Za-z0-9_])"
    camel_case_name = r"(?<=[a-z])" + token + r"(?=$|[^A-Za-z0-9_]|[A-Z])"
    if re.search(exact_name, text) or re.search(camel_case_name, text):
        return ["exact_name_token"]
    return []


def qt_test_case_names(text: str) -> list[str]:
    hooks = {"init", "initTestCase", "cleanup", "cleanupTestCase"}
    slot_methods = []
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        section_start = re.match(r"^([ \t]*)(?:private|protected)\s+(?:slots|Q_SLOTS)\s*:", lines[index])
        if not section_start:
            index += 1
            continue
        indent = section_start.group(1)
        section_lines = []
        index += 1
        while index < len(lines):
            line = lines[index]
            if re.match(rf"^{re.escape(indent)}(?:public|protected|private)\s*:", line) or line == indent + "};":
                break
            section_lines.append(line)
            index += 1
        section = "\n".join(section_lines)
        slot_methods.extend(
            re.findall(r"^\s*void\s+([A-Za-z_]\w*)\s*\([^;{}]*\)\s*(?:;|\{)", section, re.MULTILINE)
        )

    out_of_class_definitions = re.findall(
        r"^\s*void\s+[A-Za-z_][A-Za-z0-9_:]*::([A-Za-z_]\w*)\s*\(", text, re.MULTILINE
    )
    candidates = set(slot_methods + [name for name in out_of_class_definitions if name.startswith("test")])
    return sorted(name for name in candidates if name not in hooks and not name.endswith("_data"))


def validate_fixture_test_id(test_id: str, identity: tuple[str, str], plugin: dict, passed: set[str], source_texts):
    suite, separator, case_name = test_id.partition("::")
    if not separator or not suite or not case_name or suite not in passed:
        raise ValueError(f"fixture audit testId is missing or not passing in CTest: {identity} {test_id}")
    source_matches = [
        source
        for source, text, test_cases in source_texts
        if source.stem == suite and case_name in test_cases and source_mentions_plugin(text, plugin)
    ]
    if not source_matches:
        raise ValueError(f"fixture audit testId does not resolve to a matching test source: {identity} {test_id}")


def build_inventory(ctest_log: Path) -> dict:
    baseline = json.loads(BASELINE_PATH.read_text(encoding="utf-8"))
    baseline_inventory = {(item["dirName"], item["pluginId"]) for item in baseline["plugins"]}
    actual_inventory = source_inventory()
    if actual_inventory != baseline_inventory:
        added = sorted(actual_inventory - baseline_inventory)
        removed = sorted(baseline_inventory - actual_inventory)
        raise ValueError(f"plugin inventory differs from baseline: added={added}, removed={removed}")
    test_sources = test_sources_from_cmake(ROOT / "tests/CMakeLists.txt")
    source_texts = []
    for source in test_sources:
        path = ROOT / "tests" / source
        text = path.read_text(encoding="utf-8", errors="replace")
        source_texts.append((path, text, qt_test_case_names(text)))
    ctest_summary = full_ctest_summary(ctest_log)
    if ctest_summary is None:
        raise ValueError("CTest log is incomplete or contains failures")
    ctest_total, ctest_passed, ctest_skipped, ctest_unreported = ctest_summary
    if ctest_unreported:
        raise ValueError(
            f"CTest log has {ctest_unreported} unreported tests (total={ctest_total}, passed={ctest_passed}, "
            f"skipped={ctest_skipped})"
        )
    passed = passed_ctest_suites(ctest_log)
    audits_doc = json.loads(FIXTURE_AUDITS_PATH.read_text(encoding="utf-8"))
    audit_by_identity: dict[tuple[str, str], dict] = {}
    baseline_by_identity = {(item["dirName"], item["pluginId"]): item for item in baseline["plugins"]}
    for audit in audits_doc.get("audits", []):
        identity = (audit.get("dirName", ""), audit.get("pluginId", ""))
        if identity not in baseline_by_identity:
            raise ValueError(f"fixture audit references unknown plugin identity: {identity}")
        if identity in audit_by_identity:
            raise ValueError(f"duplicate fixture audit for plugin identity: {identity}")
        if audit.get("reviewStatus") != "PASS" or not audit.get("reviewer"):
            raise ValueError(f"fixture audit is not independently reviewed: {identity}")
        plugin = baseline_by_identity[identity]
        test_id = audit.get("testId", "")
        supporting_test_ids = audit.get("supportingTestIds", [])
        if not isinstance(supporting_test_ids, list) or any(not isinstance(item, str) for item in supporting_test_ids):
            raise ValueError(f"fixture audit supportingTestIds must be a string array: {identity}")
        all_test_ids = [test_id, *supporting_test_ids]
        if len(set(all_test_ids)) != len(all_test_ids):
            raise ValueError(f"fixture audit test IDs must be unique: {identity}")
        for fixture_test_id in all_test_ids:
            validate_fixture_test_id(fixture_test_id, identity, plugin, passed, source_texts)
        evidence_path = ROOT / audit.get("evidencePath", "")
        if not audit.get("evidencePath") or not evidence_path.is_file():
            raise ValueError(f"fixture audit evidencePath does not exist: {identity}")
        if evidence_path.resolve() != ctest_log.resolve():
            raise ValueError(f"fixture audit evidencePath does not match the CTest log being audited: {identity}")
        for key in ("validBusinessFixture", "criticalFailureFixture"):
            fixture = audit.get(key, {})
            if fixture.get("status") not in {"PASS", "FAIL"} or not fixture.get("assertion"):
                raise ValueError(f"fixture audit has invalid {key} for plugin identity: {identity}")
        audit_by_identity[identity] = audit

    entries = []
    for plugin in baseline["plugins"]:
        identity = (plugin["dirName"], plugin["pluginId"])
        fixture_audit = audit_by_identity.get(identity)
        candidates = []
        for source, text, test_cases in source_texts:
            match_kinds = source_mentions_plugin(text, plugin)
            if not match_kinds:
                continue
            suite = source.stem
            candidates.append(
                {
                    "testSource": source.relative_to(ROOT).as_posix(),
                    "ctestSuite": suite,
                    "suiteResult": "PASS" if suite in passed else "NOT_IN_FULL_CTEST_LOG",
                    "matchKinds": match_kinds,
                    "sourceQtTestCaseNames": test_cases,
                }
            )
        binary_candidates = []
        if plugin.get("binaryPath"):
            binary_candidates.append(ROOT / plugin["binaryPath"])
        elif plugin.get("cmakeTarget"):
            target = plugin["cmakeTarget"]
            binary_candidates.extend(
                ROOT / path
                for path in (
                    f"build-codex-agent/lib/lib{target}.so",
                    f"build-codex-agent/lib/lib{target}.dylib",
                    f"build-codex-agent/lib/{target}.dll",
                    f"build-codex-agent/bin/{target}.dll",
                )
            )
        binary_present = any(path.is_file() for path in binary_candidates)
        entries.append(
            {
                "pluginId": plugin["pluginId"],
                "name": plugin["name"],
                "dirName": plugin["dirName"],
                "kind": plugin["kind"],
                "platform": plugin.get("platform"),
                "buildEnabledInBaseline": plugin.get("buildEnabled"),
                "binaryPresentNow": binary_present,
                "baselineRuntimeLoaded": plugin.get("runtime", {}).get("loaded"),
                "testCandidates": candidates,
                "candidateCoverageStatus": "REVIEW_REQUIRED" if candidates else "NO_SOURCE_CANDIDATE_FOUND",
                "validBusinessFixtureStatus": (
                    fixture_audit["validBusinessFixture"]["status"] if fixture_audit else "NOT_AUDITED"
                ),
                "criticalFailureFixtureStatus": (
                    fixture_audit["criticalFailureFixture"]["status"] if fixture_audit else "NOT_AUDITED"
                ),
                "fixtureAuditReviewStatus": fixture_audit["reviewStatus"] if fixture_audit else "NOT_REVIEWED",
                "fixtureAuditReviewer": fixture_audit["reviewer"] if fixture_audit else None,
                "fixtureAuditTestId": fixture_audit["testId"] if fixture_audit else None,
                "fixtureAuditSupportingTestIds": fixture_audit.get("supportingTestIds", []) if fixture_audit else [],
                "fixtureAuditEvidencePath": fixture_audit["evidencePath"] if fixture_audit else None,
                "fieldAcceptanceStatus": "NOT_TESTED",
            }
        )

    counts = {
        "sourceInventory": len(entries),
        "modules": sum(item["kind"] == "module" for item in entries),
        "cameras": sum(item["kind"] == "camera" for item in entries),
        "builtBinariesPresentNow": sum(item["binaryPresentNow"] for item in entries),
        "pluginsWithSourceCandidates": sum(bool(item["testCandidates"]) for item in entries),
        "pluginsWithoutSourceCandidates": sum(not item["testCandidates"] for item in entries),
        "uniqueCandidateSuitesPassingInFullLog": len(
            {
                candidate["ctestSuite"]
                for plugin in entries
                for candidate in plugin["testCandidates"]
                if candidate["ctestSuite"] in passed
            }
        ),
        "uniqueQtTestCaseNamesInCandidateSources": len(
            {
                (candidate["ctestSuite"], test_case)
                for plugin in entries
                for candidate in plugin["testCandidates"]
                for test_case in candidate["sourceQtTestCaseNames"]
            }
        ),
        "pluginsWithReviewedBusinessFixtures": sum(item["validBusinessFixtureStatus"] in {"PASS", "FAIL"} for item in entries),
        "pluginsWithPassingBusinessFixtures": sum(item["validBusinessFixtureStatus"] == "PASS" for item in entries),
        "pluginsWithReviewedCriticalFailureFixtures": sum(
            item["criticalFailureFixtureStatus"] in {"PASS", "FAIL"} for item in entries
        ),
        "pluginsWithPassingCriticalFailureFixtures": sum(
            item["criticalFailureFixtureStatus"] == "PASS" for item in entries
        ),
        "fieldAccepted": 0,
    }
    return {
        "schemaVersion": 1,
        "purpose": "candidate inventory plus independently reviewed fixture crosswalk; not an overall G8 pass matrix",
        "baselineSource": "docs/baseline/agent-capability-matrix.json",
        "fullCtestLog": ctest_log.relative_to(ROOT).as_posix() if ctest_log.is_relative_to(ROOT) else str(ctest_log),
        "fullCtestSummary": {"total": ctest_total, "passed": ctest_passed, "failed": 0, "skipped": ctest_skipped},
        "counts": counts,
        "plugins": entries,
    }


def render_markdown(data: dict) -> str:
    counts = data["counts"]
    lines = [
        "# 阶段 8 插件测试候选清单（预审，不是验收矩阵）",
        "",
        "> 本文件只从 `tests/CMakeLists.txt` 与测试源码建立候选关系，并检查候选测试套件是否出现在全量 CTest 通过日志中。名字/源码引用不证明测试具备有效业务输出或关键失败断言；所有候选仍需逐项人工审核。",
        "",
        f"- 插件清单：{counts['sourceInventory']}（模块 {counts['modules']}，相机 {counts['cameras']}）。",
        f"- 当前二进制存在：{counts['builtBinariesPresentNow']}。",
        f"- 至少有一个源码候选：{counts['pluginsWithSourceCandidates']}；无源码候选：{counts['pluginsWithoutSourceCandidates']}。",
        f"- 候选测试文件包含 QtTest 用例名：{counts['uniqueQtTestCaseNamesInCandidateSources']} 个（同 suite+用例去重；不表示这些用例已映射到对应插件）。",
        f"- 全量 CTest：{data['fullCtestSummary']['passed']}/{data['fullCtestSummary']['total']} 通过，"
        f"{data['fullCtestSummary']['skipped']} 跳过。",
        f"- 独立审核的有效业务 fixture：{counts['pluginsWithPassingBusinessFixtures']}/{counts['pluginsWithReviewedBusinessFixtures']}；关键失败 fixture：{counts['pluginsWithPassingCriticalFailureFixtures']}/{counts['pluginsWithReviewedCriticalFailureFixtures']}；现场验收：{counts['fieldAccepted']}。",
        "- G8 自动化插件矩阵状态仍为 `NOT_RUN`；本清单不能作为 PASS 依据。",
        "",
        "| 插件 | 类型/平台 | 构建启用 | 二进制 | 测试源码候选（全量 CTest 状态） | 有效业务 fixture | 关键失败 fixture | 用例审核 |",
        "|---|---|---:|---:|---|---|---|---|",
    ]
    for plugin in data["plugins"]:
        candidates = "<br>".join(
            f"`{item['ctestSuite']}` ({item['suiteResult']}; {','.join(item['matchKinds'])})"
            for item in plugin["testCandidates"]
        ) or "—"
        evidence_test = "<br>".join(
            [plugin["fixtureAuditTestId"], *plugin.get("fixtureAuditSupportingTestIds", [])]
        ) if plugin.get("fixtureAuditTestId") else "—"
        lines.append(
            f"| {plugin['dirName']} | {plugin['kind']} / {plugin.get('platform') or '未声明'} | "
            f"{plugin['buildEnabledInBaseline']} | {plugin['binaryPresentNow']} | {candidates} | "
            f"{plugin['validBusinessFixtureStatus']} | {plugin['criticalFailureFixtureStatus']} | "
            f"{plugin['fixtureAuditReviewStatus']} ({evidence_test}) |"
        )
    lines += [
        "",
        "## 解释",
        "",
        "`direct_plugin_header` 表示测试显式包含插件头文件；`direct_plugin_symbol` 表示测试源码提及 CMake target；`exact_name_token` 表示测试源码包含插件目录名。三者都只是候选索引。接下来须查看具体测试函数、输入 fixture、参数边界、真实业务输出断言及失败行为，逐项填写正式 G8 矩阵；平台/SDK 关闭的插件需有目标平台依据，不能误记为测试通过。",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ctest-log", type=Path, default=DEFAULT_CTEST_LOG)
    parser.add_argument("--check", action="store_true", help="validate generated files without rewriting them")
    args = parser.parse_args()
    ctest_log = args.ctest_log.resolve()
    if not ctest_log.is_file():
        print(f"missing full CTest log: {ctest_log}", file=sys.stderr)
        return 2
    try:
        data = build_inventory(ctest_log)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"G8 inventory audit failed: {error}", file=sys.stderr)
        return 2
    json_text = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    markdown_text = render_markdown(data)
    if args.check:
        if not JSON_PATH.is_file() or JSON_PATH.read_text(encoding="utf-8") != json_text:
            print("JSON inventory is stale; run script without --check", file=sys.stderr)
            return 1
        if not MARKDOWN_PATH.is_file() or MARKDOWN_PATH.read_text(encoding="utf-8") != markdown_text:
            print("Markdown inventory is stale; run script without --check", file=sys.stderr)
            return 1
        print(
            f"G8 candidate inventory OK: {data['counts']['sourceInventory']} plugins; "
            f"{data['counts']['pluginsWithPassingBusinessFixtures']} reviewed business fixtures; "
            "field acceptance remains untested"
        )
        return 0
    JSON_PATH.write_text(json_text, encoding="utf-8")
    MARKDOWN_PATH.write_text(markdown_text, encoding="utf-8")
    print(
        f"G8 candidate inventory generated: {data['counts']['sourceInventory']} plugins; "
        f"{data['counts']['pluginsWithSourceCandidates']} with test-source candidates; "
        f"{data['counts']['pluginsWithPassingBusinessFixtures']} reviewed business fixtures; "
        "field acceptance not inferred"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
