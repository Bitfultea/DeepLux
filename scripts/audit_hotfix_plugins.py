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


def markdown(rows: list[dict]) -> str:
    counts = Counter(row["matchKind"] for row in rows)
    lines = [
        "# Hotfix 插件映射清单",
        "",
        f"- 旧版仓库：`{REPOSITORY}`",
        f"- 固定提交：`{COMMIT}`",
        f"- 生成命令：`python3 scripts/audit_hotfix_plugins.py`",
        "- 说明：`direct` 仅代表规范化名称相同；它不是参数、数据契约或运行结果等价的证明。",
        "",
        "| 状态 | 数量 | 含义 |",
        "| --- | ---: | --- |",
        f"| direct | {counts['direct']} | 名称直接匹配，仍需人工核验能力和参数 |",
        f"| candidate | {counts['candidate']} | 需要确认的历史别名或替代候选 |",
        f"| missing | {counts['missing']} | 当前没有候选实现 |",
        f"| business_pack | {counts['business_pack']} | 业务专用插件，作为可选业务包评审 |",
        "",
        "完整逐项数据见 `hotfix-plugin-mapping.json`。以下列出需要决策的项目：",
        "",
        "| 旧版插件 | 旧版目录 | 当前候选 | 状态 |",
        "| --- | --- | --- | --- |",
    ]
    for row in rows:
        if row["matchKind"] != "direct":
            lines.append(
                f"| {row['legacyPlugin']} | `{row['legacyPath']}` | {row['currentCandidate'] or '-'} | {row['matchKind']} |"
            )
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    current = current_plugins()
    rows = [classify(item, current) for item in legacy_plugins()]
    if len(rows) != 110:
        raise RuntimeError(f"expected 110 legacy plugins, found {len(rows)}")

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    payload = {"repository": REPOSITORY, "commit": COMMIT, "plugins": rows}
    (OUTPUT_DIR / "hotfix-plugin-mapping.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (OUTPUT_DIR / "hotfix-plugin-mapping.md").write_text(markdown(rows), encoding="utf-8")
    print(f"wrote {len(rows)} legacy plugin mappings")


if __name__ == "__main__":
    main()
