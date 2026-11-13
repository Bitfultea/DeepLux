#!/usr/bin/env python3
"""Validate G6-3 producer evidence references and render the Markdown view."""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
JSON_PATH = ROOT / "docs/baseline/agent-g6-coordinate-producer-matrix.json"
MARKDOWN_PATH = ROOT / "docs/baseline/agent-g6-coordinate-producer-matrix.md"
ACCEPTANCE_PATH = ROOT / "docs/baseline/agent-phase-6-acceptance.md"
REQUIRED_ROW_FIELDS = {
    "id",
    "producer",
    "inputDomain",
    "coordinateRule",
    "transforms",
    "lifecycle",
    "outputs",
    "testIds",
    "acceptanceRefs",
    "evidencePath",
    "additionalEvidencePaths",
    "status",
    "remaining",
}
STATUSES = {"covered", "partial", "not_covered"}
STATUS_LABELS = {"covered": "已覆盖", "partial": "部分覆盖", "not_covered": "未覆盖"}


def validate(data: dict) -> list[str]:
    errors: list[str] = []
    if data.get("schemaVersion") != 1 or data.get("criterion") != "G6-3":
        errors.append("matrix header must declare schemaVersion=1 and criterion=G6-3")
    assessment = data.get("criterionAssessment", {})
    if assessment.get("decision") not in {"NOT_READY_FOR_PASS", "PASS"}:
        errors.append("criterionAssessment.decision must explicitly state the G6-3 decision")
    if assessment.get("decision") == "NOT_READY_FOR_PASS" and not assessment.get("remainingGaps"):
        errors.append("G6-3 cannot be held without listing concrete remaining gaps")

    acceptance = ACCEPTANCE_PATH.read_text(encoding="utf-8")
    acceptance_slices = {
        f"Slice {number}" for number in re.findall(r"^## Slice (\d+)[：:]", acceptance, re.MULTILINE)
    }
    seen_ids: set[str] = set()
    for index, row in enumerate(data.get("rows", [])):
        prefix = f"rows[{index}]"
        missing = REQUIRED_ROW_FIELDS - row.keys()
        if missing:
            errors.append(f"{prefix}: missing fields: {', '.join(sorted(missing))}")
            continue
        row_id = row["id"]
        if row_id in seen_ids:
            errors.append(f"{prefix}: duplicate id {row_id}")
        seen_ids.add(row_id)
        if row["status"] not in STATUSES:
            errors.append(f"{prefix}: invalid status {row['status']}")
        if not isinstance(row["additionalEvidencePaths"], list) or any(
            not isinstance(path, str) for path in row["additionalEvidencePaths"]
        ):
            errors.append(f"{prefix}: additionalEvidencePaths must be a list of paths")
        elif row["evidencePath"] in row["additionalEvidencePaths"] or len(
            row["additionalEvidencePaths"]
        ) != len(set(row["additionalEvidencePaths"])):
            errors.append(f"{prefix}: evidence paths must be unique")
        if row["status"] == "covered" and not row["testIds"]:
            errors.append(f"{prefix}: covered row must cite at least one test")
        if not row["testIds"] and not row["remaining"].strip():
            errors.append(f"{prefix}: a row without tests must explain its remaining gap")

        for test_ref in row["testIds"]:
            try:
                relative_path, symbol = test_ref.split("::", 1)
                source = (ROOT / relative_path).read_text(encoding="utf-8")
            except (ValueError, OSError) as error:
                errors.append(f"{prefix}: cannot resolve test reference {test_ref}: {error}")
                continue
            if not re.search(rf"\b{re.escape(symbol)}\s*\(", source):
                errors.append(f"{prefix}: test symbol not found: {test_ref}")

        for slice_ref in row["acceptanceRefs"]:
            if slice_ref not in acceptance_slices:
                errors.append(f"{prefix}: exact acceptance slice heading not found: {slice_ref}")

        for evidence_path in [row["evidencePath"], *row["additionalEvidencePaths"]]:
            evidence = ROOT / evidence_path
            if not evidence.is_dir():
                errors.append(f"{prefix}: evidence path does not exist: {evidence_path}")

        expected_evidence_paths = {
            f"build-codex-agent/agent-phase6-slice{int(match.group(1))}-evidence"
            for slice_ref in row["acceptanceRefs"]
            if (match := re.fullmatch(r"Slice (\d+)", slice_ref))
        }
        listed_evidence_paths = {row["evidencePath"], *row["additionalEvidencePaths"]}
        if listed_evidence_paths != expected_evidence_paths:
            missing = sorted(expected_evidence_paths - listed_evidence_paths)
            extra = sorted(listed_evidence_paths - expected_evidence_paths)
            errors.append(f"{prefix}: evidence paths must match acceptance slices; missing={missing}, extra={extra}")

    if not data.get("rows"):
        errors.append("matrix contains no rows")
    return errors


def render_markdown(data: dict) -> str:
    headers = [
        "ID",
        "生产入口",
        "输入域",
        "坐标规则",
        "变换与生命周期",
        "输出/消费",
        "回归测试",
        "验收引用 / 证据",
        "状态",
        "未覆盖边界",
    ]
    lines = [
        "# G6-3 坐标生产者与证据覆盖矩阵",
        "",
        f"唯一数据源：[`agent-g6-coordinate-producer-matrix.json`](agent-g6-coordinate-producer-matrix.json)。",
        "",
        "状态只描述该行明确列出的回归范围；覆盖比例不用于推导 G6-3 总体通过。`partial` 与 `not_covered` 中的边界必须保持可见。",
        "",
        "| " + " | ".join(headers) + " |",
        "|" + "|".join(["---"] * len(headers)) + "|",
    ]
    for row in data["rows"]:
        values = [
            row["id"],
            row["producer"],
            row["inputDomain"],
            row["coordinateRule"],
            f"变换：{row['transforms']}<br>生命周期：{row['lifecycle']}",
            row["outputs"],
            "<br>".join(f"`{test_id}`" for test_id in row["testIds"]) or "—",
            "<br>".join(
                row["acceptanceRefs"]
                + [f"`{path}`" for path in [row["evidencePath"], *row["additionalEvidencePaths"]]]
            ),
            STATUS_LABELS[row["status"]],
            row["remaining"],
        ]
        escaped = [value.replace("|", "\\|").replace("\n", "<br>") for value in values]
        lines.append("| " + " | ".join(escaped) + " |")

    assessment = data["criterionAssessment"]
    lines.extend(
        [
            "",
            "## G6-3 判定",
            "",
            f"**{assessment['decision']}**。本矩阵用于定位实际 producer 与测试证据，不按覆盖行数推定整体通过。",
            "",
            f"- 输入路线：{assessment['inputRoutes']}",
            f"- 坐标框架：{assessment['coordinateFrames']}",
            f"- 变换与生命周期：{assessment['transformsAndLifecycle']}",
            f"- 消费与 overlay：{assessment['consumersAndOverlays']}",
            f"- SAM 会话与导出：{assessment['samSessionAndExporters']}",
            "- 剩余缺口：<br>" + "<br>".join(assessment["remainingGaps"]),
            f"- 放行规则：{assessment['passRule']}",
            "",
            "fake 服务、CPU 投影和 offscreen GUI 证据不替代真实模型、GPU、设备或跨平台验收。",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="regenerate the Markdown view from JSON")
    args = parser.parse_args()
    try:
        data = json.loads(JSON_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        print(f"matrix load failed: {error}", file=sys.stderr)
        return 2

    errors = validate(data)
    if errors:
        print("G6-3 matrix validation FAILED:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    rendered = render_markdown(data)
    if args.write:
        MARKDOWN_PATH.write_text(rendered, encoding="utf-8")
        print(f"G6-3 matrix validation OK: {len(data['rows'])} rows; Markdown regenerated")
        return 0

    if not MARKDOWN_PATH.exists() or MARKDOWN_PATH.read_text(encoding="utf-8") != rendered:
        print("Markdown view differs from JSON; run with --write", file=sys.stderr)
        return 1
    print(f"G6-3 matrix validation OK: {len(data['rows'])} rows; JSON/Markdown agree")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
