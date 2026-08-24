#!/usr/bin/env python3
"""Generate a reproducible inventory mapping for the legacy hotfix plugins.

The legacy implementation is intentionally queried at one pinned commit.  The
generated files are audit inputs: an automatic name match is never treated as
proof that two plugins have identical parameters or behaviour.

Usage:
    python3 scripts/audit_hotfix_plugins.py
"""

from __future__ import annotations

import json
import subprocess
from collections import Counter
from pathlib import Path


REPOSITORY = "qhchen-sz/DeepLux"
COMMIT = "47d76c1225e9dba5cfd3674df54cc3327894839b"
ROOT = Path(__file__).resolve().parents[1]
OUTPUT_DIR = ROOT / "docs" / "baseline"

# These names differ for historical or spelling reasons and need a deliberate
# candidate rather than a fuzzy match.
ALIASES = {
    "barcodereader": "QRCode",
    "diplaydata": "DisplayData",
    "npointcal": "NPointCalibration",
    "perprocessing": "PerProcessing",
    "pointsurfacedistance": "PointSurfaceDistance",
    "distancepp": "DistancePP",
    "distancepl": "DistancePL",
    "distancell": "linesdistance",
    "measurecalib": "NPointCalibration",
    "colorrecognition": "ColorRecognition",
    "jierhandefectsdet": "JiErHanDefectsDet",
}


def normalize(name: str) -> str:
    return "".join(ch.lower() for ch in name.replace("Plugin.", "") if ch.isalnum())


def run_gh_api(endpoint: str) -> dict:
    completed = subprocess.run(
        ["gh", "api", endpoint], check=True, capture_output=True, text=True
    )
    return json.loads(completed.stdout)


def legacy_plugins() -> list[dict]:
    tree = run_gh_api(f"repos/{REPOSITORY}/git/trees/{COMMIT}?recursive=1")
    paths = []
    for item in tree["tree"]:
        path = item["path"]
        if item["type"] != "tree" or not path.startswith("02Plugins/"):
            continue
        if len(path.split("/")) == 3:
            paths.append(path)
    return [
        {"path": path, "category": path.split("/")[1], "name": path.split("/")[2].removeprefix("Plugin.")}
        for path in sorted(paths)
    ]


def current_plugins() -> dict[str, dict]:
    result: dict[str, dict] = {}
    for metadata in ROOT.glob("src/plugins/**/metadata.json"):
        payload = json.loads(metadata.read_text(encoding="utf-8"))
        if payload.get("category") == "camera" or not payload.get("name"):
            continue
        entry = {
            "name": payload["name"],
            "id": payload.get("id", ""),
            "folder": metadata.parent.name,
        }
        # The display name may be Chinese while projects and plugin folders
        # still use English identifiers.  Every stable spelling is indexed.
        for key in (entry["name"], entry["id"].rsplit(".", 1)[-1], entry["folder"].removesuffix("Plugin")):
            result[normalize(key)] = entry
    return result


def classify(legacy: dict, current: dict[str, dict]) -> dict:
    normalized = normalize(legacy["name"])
    candidate = current.get(normalized)
    match_kind = "direct" if candidate else "missing"
    if not candidate and normalized in ALIASES:
        candidate = current.get(normalize(ALIASES[normalized]))
        match_kind = "candidate" if candidate else "missing"
    if legacy["category"] in {"015包膜机", "016密封钉"}:
        match_kind = "business_pack"
    return {
        "legacyPath": legacy["path"],
        "legacyCategory": legacy["category"],
        "legacyPlugin": legacy["name"],
        "currentCandidate": candidate["folder"] if candidate else "",
        "currentPluginId": candidate["id"] if candidate else "",
        "matchKind": match_kind,
        "reviewState": "pending",
    }


def load_existing_reviews() -> dict[str, dict]:
    """Load prior review decisions keyed by legacyPath so regeneration
    preserves human review state, conclusions and evidence.

    G2-fix5: 以 legacyPath 为键（而非 legacyPlugin），并记录当时的候选身份
    （currentCandidate/currentPluginId/matchKind）。合并时仅当候选身份未变化
    才继承审核结论，避免候选/ID/匹配类型改变后保留失效的 equivalent。
    """
    mapping_file = OUTPUT_DIR / "hotfix-plugin-mapping.json"
    if not mapping_file.exists():
        return {}
    try:
        payload = json.loads(mapping_file.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {}
    reviews: dict[str, dict] = {}
    for row in payload.get("plugins", []):
        key = row.get("legacyPath")
        if not key:
            continue
        preserved = {}
        for field in ("reviewState", "reviewConclusion", "evidence", "dependencyNote"):
            if field in row:
                preserved[field] = row[field]
        # 记录审核时的候选身份，用于合并时校验是否仍有效
        preserved["_candidateIdentity"] = {
            "currentCandidate": row.get("currentCandidate", ""),
            "currentPluginId": row.get("currentPluginId", ""),
            "matchKind": row.get("matchKind", ""),
        }
        if preserved:
            reviews[key] = preserved
    return reviews


def markdown(rows: list[dict]) -> str:
    counts = Counter(row["matchKind"] for row in rows)
    review_counts = Counter(row.get("reviewState", "pending") for row in rows)
    conclusion_counts = Counter(
        row.get("reviewConclusion", "-") for row in rows if row.get("reviewState") == "reviewed"
    )
    lines = [
        "# Hotfix 插件映射清单",
        "",
        f"- 旧版仓库：`{REPOSITORY}`",
        f"- 固定提交：`{COMMIT}`",
        f"- 生成命令：`python3 scripts/audit_hotfix_plugins.py`",
        "- 说明：`direct` 仅代表规范化名称相同；它不是参数、数据契约或运行结果等价的证明。",
        "- 重跑脚本会保留已有 `reviewState`/`reviewConclusion`/`evidence` 字段（见 `load_existing_reviews`）。",
        "",
        "| 状态 | 数量 | 含义 |",
        "| --- | ---: | --- |",
        f"| direct | {counts['direct']} | 名称直接匹配，仍需人工核验能力和参数 |",
        f"| candidate | {counts['candidate']} | 需要确认的历史别名或替代候选 |",
        f"| missing | {counts['missing']} | 当前没有候选实现 |",
        f"| business_pack | {counts['business_pack']} | 业务专用插件，作为可选业务包评审 |",
        "",
        "| 审核状态 | 数量 |",
        "| --- | ---: |",
        f"| reviewed | {review_counts.get('reviewed', 0)} |",
        f"| dependency_recorded | {review_counts.get('dependency_recorded', 0)} |",
        f"| pending | {review_counts.get('pending', 0)} |",
        "",
        "| 审核结论 | 数量 | 含义 |",
        "| --- | ---: | --- |",
        f"| equivalent | {conclusion_counts.get('equivalent', 0)} | 已证明与旧版能力等价（需旧版参数/端口/结果对照） |",
        f"| partial | {conclusion_counts.get('partial', 0)} | 当前存在候选实现，旧版等价未证明 |",
        f"| unverified | {conclusion_counts.get('unverified', 0)} | 依赖硬件/SDK，行为未验证 |",
        f"| not_equivalent | {conclusion_counts.get('not_equivalent', 0)} | 不等价 |",
        "",
        "完整逐项数据见 `hotfix-plugin-mapping.json`。以下列出需要决策的项目：",
        "",
        "| 旧版插件 | 旧版目录 | 当前候选 | 状态 | 审核结论 |",
        "| --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        if row["matchKind"] != "direct":
            conclusion = row.get("reviewConclusion", "-")
            lines.append(
                f"| {row['legacyPlugin']} | `{row['legacyPath']}` | {row['currentCandidate'] or '-'} | {row['matchKind']} | {conclusion} |"
            )
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    current = current_plugins()
    rows = [classify(item, current) for item in legacy_plugins()]
    if len(rows) != 110:
        raise RuntimeError(f"expected 110 legacy plugins, found {len(rows)}")

    # G-fix2 + G2-fix5: 合并已有审核状态，重跑脚本不丢失人工审核结论。
    # 以 legacyPath 为键；仅当候选身份（候选/ID/匹配类型）未变化时才继承结论，
    # 否则重置为 pending，避免保留失效的 equivalent。
    existing = load_existing_reviews()
    merged_count = 0
    invalidated_count = 0
    for row in rows:
        key = row["legacyPath"]
        if key not in existing:
            continue
        saved = existing[key]
        identity = saved.get("_candidateIdentity", {})
        identity_unchanged = (
            identity.get("currentCandidate", "") == row["currentCandidate"]
            and identity.get("currentPluginId", "") == row["currentPluginId"]
            and identity.get("matchKind", "") == row["matchKind"]
        )
        if identity_unchanged:
            for field in ("reviewState", "reviewConclusion", "evidence", "dependencyNote"):
                if field in saved:
                    row[field] = saved[field]
            merged_count += 1
        else:
            # 候选身份变化：旧审核结论失效，回退待审
            row["reviewState"] = "pending"
            row.pop("reviewConclusion", None)
            invalidated_count += 1

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    payload = {"repository": REPOSITORY, "commit": COMMIT, "plugins": rows}
    (OUTPUT_DIR / "hotfix-plugin-mapping.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (OUTPUT_DIR / "hotfix-plugin-mapping.md").write_text(markdown(rows), encoding="utf-8")
    print(
        f"wrote {len(rows)} legacy plugin mappings "
        f"({merged_count} review states preserved, {invalidated_count} invalidated by candidate change)"
    )


if __name__ == "__main__":
    main()
