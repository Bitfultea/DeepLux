#!/usr/bin/env python3
"""阶段 0 审计脚本（docs/agent-capability-improvement-plan.md §5）。

生成 Agent 能力基线矩阵：
  - docs/baseline/agent-capability-matrix.json （机器可读，唯一数据源）
  - docs/baseline/agent-capability-matrix.md   （由 JSON 渲染，供审核；禁止独立维护）

子命令：
  prepare-probe  在隔离目录构造运行时插件树（不触碰 ~/.deeplux）
  generate       扫描元数据/CMake/二进制/运行时探针输出/操作清单，生成矩阵 JSON+MD

运行时探针（generate 之前执行，输出文件作为证据）：
  DEEPLUX_APP_DATA_DIR=<probe>/appdata QT_QPA_PLATFORM=offscreen \
      ./build-codex-agent/bin/DeepLux list-plugins > <evidence>/runtime-list-plugins.log 2>&1

设计约束（对应计划条款）：
  §5.1  模块与相机分别记录；与 CMake target、二进制、PluginManager 运行时结果核对
  §5.2  未构建/加载失败/SDK 缺失/平台不适用/演示模式逐项标记，禁止静默跳过
  §5.3  JSON 唯一数据源，MD 由 JSON 生成
  G0    69 项可核对（不硬编码 69；以实际扫描数为准并报告差异）
"""

import argparse
import ast
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

#证据文件（仓库根相对路径；审核整改 R1——每个维度单元格必须附有效证据位置）
#阶段2 起：复现 / 回归日志指向当前阶段的独立运行，
#阶段0 原始日志在 agent - phase0 - evidence 永久保留（含 v1）
EV_REPRO = "build-codex-agent/agent-phase3-evidence/repro-current.log"
EV_CTEST = "build-codex-agent/agent-phase3-evidence/ctest-current.xml"
EV_F05_REPRO = "build-codex-agent/agent-phase-5-slice-16-evidence/f05-current.log"
EV_CAMERA_DISCOVERY_CTEST = "build-codex-agent/agent-phase-5-slice-16-evidence/ctest-camera-discovery.log"
EV_PROBE = "build-codex-agent/agent-phase-5-slice-38-evidence/runtime-list-plugins.log"
EV_RECORD = "docs/baseline/agent-phase-0-acceptance.md"
REPRO_SUITE = "test_agentphase0_repro"

REPO = Path(__file__).resolve().parent.parent

#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
#构建门槛与平台适用性（来源：src / plugins / camera / CMakeLists.txt、
#src / plugins / hymson3d / CMakeLists.txt、顶层 CMakeLists.txt；审核可对照原文）
#-- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -
BUILD_GATES = {
#dirName : (gateKind, cacheOption | None, applicablePlatform, reason)
    "V4L2Camera": ("platform", None, "linux", "camera/CMakeLists.txt: UNIX AND NOT APPLE 分支"),
    "DirectShowCamera": ("platform", None, "windows", "camera/CMakeLists.txt: WIN32 分支；Linux 构建不适用"),
    "HikvisionCamera": ("option", "ENABLE_CAMERA_HIKVISION", "linux", "需海康 MVS SDK；默认 OFF"),
    "DefectDetection": ("option", "ENABLE_HYMSON3D", "any",
                        "hymson3d/CMakeLists.txt: ENABLE_HYMSON3D=OFF 时提前 return()，"
                        "注释宣称的 demo 模式构建实际不可达"),
}

#运行时外部依赖（非构建期）：执行需要的服务 / 设备
RUNTIME_DEPS = {
    "SamPredict": "本地 SAM FastAPI 服务（默认 http://127.0.0.1:8990，需 GPU/CPU 权重）",
    "GrabImage": "相机模式需已连接相机设备；文件模式需有效图像路径",
    "LoadPointCloud": "需有效 PLY/TIFF 高度图文件",
    "PLCCommunicatePlugin": "需可达 PLC 设备（生产 IP 不用于自动测试）",
    "PLCReadPlugin": "需可达 PLC 设备",
    "PLCWritePlugin": "需可达 PLC 设备（写操作，副作用）",
    "SerialPort": "需可用串口设备",
    "TCPClient": "需可达 TCP 服务端",
    "TCPServer": "端口监听（网络副作用）",
    "V4L2Camera": "需 /dev/video* 设备",
    "DirectShowCamera": "需 Windows DirectShow 相机设备",
    "HikvisionCamera": "需海康相机设备与 MVS SDK 运行时",
}

#副作用分类审计判定（阶段 0 人工核对源码得出；evidence 由 scan 的 grep 佐证）
#none = 纯计算; file = 写 / 删文件; network = 网络收发 / 监听; device = 设备控制 / 采集
SIDE_EFFECT_OVERRIDE = {
    "Folder": "file",        # process() 支持递归删除目录（F13 主证据）
    "SaveImage": "file",
    "SaveData": "file",     # SaveDataPlugin.cpp 含 QFile 写入
    "WriteText": "file",    # WriteTextPlugin.cpp 含 QFile 写入
    "TableOutPut": "none",  # 仅表格显示输出，无文件/网络/设备访问（源码 grep 核对）
    "PLCWritePlugin": "device",
    "PLCReadPlugin": "device",
    "PLCCommunicatePlugin": "device",
    "SerialPort": "device",
    "TCPClient": "network",
    "TCPServer": "network",
    "SamPredict": "network",
    "GrabImage": "device",   # 相机模式控制设备；文件模式只读
    "ShowImage": "none",
    "DisplayData": "none",
    "ShowPoint": "none",
    "V4L2Camera": "device",
    "DirectShowCamera": "device",
    "HikvisionCamera": "device",
}

SIDE_EFFECT_PATTERNS = {
    "file": [r"QFile::remove", r"removeRecursively", r"imwrite", r"QIODevice::WriteOnly", r"QSaveFile",
             r"QTextStream\s*\(\s*&?\w*[Ff]ile"],
    "network": [r"QTcpSocket", r"QTcpServer", r"QUdpSocket", r"QNetworkAccessManager", r"QSerialPort"],
    "device": [r"v4l2", r"MvCamera", r"MVS", r"ICamera\b"],
    "process": [r"QProcess"],
}

#F 编号 → 收口阶段（计划 §2 表格）
ISSUE_PHASES = {
    "F01": [3], "F02": [2], "F03": [2, 4], "F04": [2], "F05": [5], "F06": [1], "F07": [1],
    "F08": [1], "F09": [4, 6], "F10": [1, 7], "F11": [3, 7], "F12": [0, 8], "F13": [1, 5],
}


def read_cmake_cache_options(build_dir: Path) -> dict:
    opts = {}
    cache = build_dir / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            m = re.match(r"^(ENABLE_[A-Z0-9_]+|USE_QT6|CMAKE_BUILD_TYPE):[A-Z]+=(.*)$", line)
            if m:
                opts[m.group(1)] = m.group(2)
    return opts


def parse_cmake_target(plugin_dir: Path):
    cml = plugin_dir / "CMakeLists.txt"
    if not cml.exists():
        return None
    m = re.search(r"add_library\(\s*([A-Za-z0-9_]+)", cml.read_text(encoding="utf-8", errors="replace"))
    return m.group(1) if m else None


def find_binary(build_dir: Path, target: str):
    if not target:
        return None
    for cand in (build_dir / "lib" / f"lib{target}.so",):
        if cand.exists():
            return str(cand.relative_to(REPO))
    hits = sorted(build_dir.rglob(f"lib{target}.so"))
    return str(hits[0].relative_to(REPO)) if hits else None


def grep_side_effects(plugin_dir: Path):
    evidence = {}
    for f in sorted(plugin_dir.rglob("*.cpp")):
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            for kind, pats in SIDE_EFFECT_PATTERNS.items():
                for p in pats:
                    if re.search(p, line):
                        evidence.setdefault(kind, []).append(f"{f.relative_to(REPO)}:{i}")
                        break
    return evidence


def parse_legacy_matrix(path: Path):
    """解析 docs/baseline/capability-matrix.md 的插件状态表（历史台账，来源标注）。"""
    result = {}
    if not path.exists():
        return result
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^\|\s*([^|]+?)\s*\|\s*(com\.deeplux[^|]*?)\s*\|\s*([^|]*?)\s*\|\s*(可用|部分|实验性|依赖硬件)\s*\|\s*([^|]*?)\s*\|\s*([^|]*?)\s*\|", line)
        if m:
            result[m.group(1).strip()] = {"status": m.group(4), "migration": m.group(6).strip(),
                                          "source": str(path.relative_to(REPO))}
    return result


def parse_runtime_probe(log_path: Path):
    """解析 list-plugins 探针输出：模块 OK/FAIL + 相机加载日志（相机经 cameraInfos 分类，
    不出现在 list-plugins 表格中，其运行时证据来自 'Plugin loaded: "..."' 日志行）。"""
    modules, cameras_log, camera_loaded = {}, [], set()
    if not log_path or not log_path.exists():
        return modules, cameras_log, camera_loaded
    text = log_path.read_text(encoding="utf-8", errors="replace")
    for m in re.finditer(r"^\[(OK|FAIL)\]\s+(.+?)\s+v([\d.]+)\s+\((\w+)\)\s*$", text, re.MULTILINE):
        modules[m.group(2)] = {"loaded": m.group(1) == "OK", "version": m.group(3), "category": m.group(4)}
    for m in re.finditer(r'Plugin loaded:\s+"(.+?)"', text):
        camera_loaded.add(m.group(1))
    for line in text.splitlines():
        low = line.lower()
        if "camera" in low or "相机" in line:
            cameras_log.append(line)
    return modules, cameras_log, camera_loaded


def cmd_prepare_probe(args):
    build_dir = REPO / args.build_dir
    probe = Path(args.probe_dir)
    plugins_dst = probe / "appdata" / "plugins"
#审核整改 R4：绝不删除既有目录。目标已存在时拒绝执行，
#防止复现命令误删任何既有数据；需要重跑时由操作者自行选择全新目录。
    if plugins_dst.exists():
        sys.exit(f"REFUSING to run: {plugins_dst} already exists. "
                 f"Choose a fresh --probe-dir (this tool never deletes existing data).")
    installed, missing = [], []
    for meta in sorted(REPO.glob("src/plugins/**/metadata.json")):
        pdir = meta.parent
        target = parse_cmake_target(pdir)
        so = find_binary(build_dir, target) if target else None
        if not so:
            missing.append(pdir.name)
            continue
        dst = plugins_dst / pdir.name
        dst.mkdir(parents=True, exist_ok=True)
        shutil.copy2(REPO / so, dst / Path(so).name)
        shutil.copy2(meta, dst / "metadata.json")
        installed.append(pdir.name)
    print(json.dumps({"probe": str(probe), "installed": len(installed),
                      "missingBinary": missing}, ensure_ascii=False, indent=1))


def cmd_generate(args):
    build_dir = REPO / args.build_dir
    previous_path = REPO / "docs/baseline/agent-capability-matrix.json"
    previous = json.loads(previous_path.read_text(encoding="utf-8")) if previous_path.exists() else None
    cache_opts = read_cmake_cache_options(build_dir)
    legacy = parse_legacy_matrix(REPO / "docs/baseline/capability-matrix.md")
    rt_modules, rt_cameras_log, rt_cameras_loaded = parse_runtime_probe(
        Path(args.runtime_log) if args.runtime_log else None)

    host_platform = "linux" if sys.platform.startswith("linux") else (
        "windows" if sys.platform == "win32" else sys.platform)

    plugins = []
    previous_by_id = {p.get("pluginId"): p for p in previous.get("plugins", [])} if previous else {}
    for meta in sorted(REPO.glob("src/plugins/**/metadata.json")):
        pdir = meta.parent
        try:
            md = json.loads(meta.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            md = {"_parseError": str(e)}
        name = md.get("name") or pdir.name
        category = md.get("category", "")
        kind = "camera" if (category == "camera" or pdir.parent.name == "camera") else "module"
        target = parse_cmake_target(pdir)
        binary = find_binary(build_dir, target) if target else None

        gate = BUILD_GATES.get(pdir.name)
        build_enabled, gate_reason, build_reason_code = True, None, None
        applicable = md.get("platform", "any")
        if gate:
            gate_kind, opt, plat, reason = gate
            applicable = plat
            if gate_kind == "platform" and plat != host_platform:
                build_enabled, gate_reason = False, f"平台不适用（{reason}）"
                build_reason_code = "PLATFORM_UNSUPPORTED"
            elif gate_kind == "option" and cache_opts.get(opt, "OFF") != "ON":
                build_enabled, gate_reason = False, f"{opt}=OFF（{reason}）"
                build_reason_code = "BUILD_OPTION_DISABLED"
        if not binary and build_reason_code is None:
            build_enabled = False
            gate_reason = "CMake target 缺失或构建目录中未找到对应插件二进制"
            build_reason_code = "BINARY_MISSING"

        se_override = SIDE_EFFECT_OVERRIDE.get(pdir.name)
        se_evidence = grep_side_effects(pdir) if kind == "module" or se_override is None else {}
        side_effect = se_override or ("none" if not any(
            k in se_evidence for k in ("file", "network", "device", "process")) else "mixed")

        rt = rt_modules.get(name)
        if kind == "camera":
            cam_loaded = name in rt_cameras_loaded
            runtime_reason_code = None if cam_loaded else (
                build_reason_code if not binary else "RUNTIME_LOAD_FAILED")
            runtime = {
                "scanned": bool(binary),
                "loaded": cam_loaded if binary else None,
                "classification": "camera",
                "error": None if cam_loaded else (
                    "未构建/未安装到探针目录（%s）" % (gate_reason or "二进制缺失") if not binary
                    else "已安装但运行时日志无 'Plugin loaded' 记录"),
                "reasonCode": runtime_reason_code,
            }
        else:
            runtime_reason_code = None if (rt and rt["loaded"]) else (
                build_reason_code if not binary else (
                    "RUNTIME_NOT_REPORTED" if rt is None else "RUNTIME_LOAD_FAILED"))
            runtime = {
                "scanned": rt is not None,
                "loaded": bool(rt and rt["loaded"]) if rt else None,
                "classification": "module",
                "error": None if (rt and rt["loaded"]) else (
                    "未构建/未安装到探针目录（%s）" % (gate_reason or "二进制缺失") if not binary
                    else "已安装但 list-plugins 未报告加载成功"),
                "reasonCode": runtime_reason_code,
            }

        entry = {
            "pluginId": md.get("id"),
            "name": name,
            "dirName": pdir.name,
            "kind": kind,
            "category": category,
            "version": md.get("version"),
            "sourcePath": str(pdir.relative_to(REPO)),
            "cmakeTarget": target,
            "binaryPath": binary,
            "platform": applicable,
            "buildEnabled": build_enabled,
            "buildDisabledReason": gate_reason,
            "buildReasonCode": build_reason_code,
            "dependencies": md.get("dependencies", {}),
            "runtimeDependency": RUNTIME_DEPS.get(pdir.name),
            "sideEffect": side_effect,
            "sideEffectEvidence": {k: v[:6] for k, v in se_evidence.items()},
            "legacyStatus": legacy.get(name),
            "runtime": runtime,
        }
        entry["dimensions"] = build_dimensions(entry)
        # Preserve previous matrix decisions where this generator still emits its
        # generic NOT_TESTED fallback. Explicit current PASS/FAIL/BLOCKED rules win.
        old_entry = previous_by_id.get(entry.get("pluginId"), {})
        old_dimensions = old_entry.get("dimensions", {})
        for dimension, cell_value in entry["dimensions"].items():
            old_cell = old_dimensions.get(dimension, {})
            if cell_value.get("status") == "NOT_TESTED" and old_cell:
                entry["dimensions"][dimension] = old_cell
        plugins.append(entry)

    ops_path = REPO / args.operations
    operations = json.loads(ops_path.read_text(encoding="utf-8")) if ops_path.exists() else []
    validate_owner_api_references(operations)

    current_names = {p["dirName"] for p in plugins}
    previous_names = {p["dirName"] for p in previous.get("plugins", [])} if previous else set()
    modules_count = sum(p["kind"] == "module" for p in plugins)
    cameras_count = sum(p["kind"] == "camera" for p in plugins)
    inventory_audit = {
        "expected": {"metadataTotal": 69, "modules": 66, "cameras": 3},
        "actual": {"metadataTotal": len(plugins), "modules": modules_count, "cameras": cameras_count},
        "addedSincePrevious": sorted(current_names - previous_names) if previous else sorted(current_names),
        "removedSincePrevious": sorted(previous_names - current_names),
    }
    inventory_audit["matchesExpected"] = (
        inventory_audit["actual"] == inventory_audit["expected"]
        and not inventory_audit["addedSincePrevious"]
        and not inventory_audit["removedSincePrevious"]
    )

    matrix = {
        "schemaVersion": 1,
        "generatedAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "generatedBy": "scripts/audit_agent_capability_baseline.py generate",
        "plan": "docs/agent-capability-improvement-plan.md",
        "baseline": {
            "head": git_head(),
            "branch": git_branch(),
            "buildDir": args.build_dir,
            "hostPlatform": host_platform,
            "cmakeOptions": cache_opts,
            "runtimeProbeLog": args.runtime_log,
            "runtimeCameraLogLines": rt_cameras_log[:40],
        },
        "inventoryAudit": inventory_audit,
        "counts": summarize(plugins, operations),
        "plugins": plugins,
        "operations": operations,
        "issues": {k: {"phases": v} for k, v in ISSUE_PHASES.items()},
    }

    validate_references(matrix)  # 审核整改 R1：引用校验失败则拒绝生成

    out_json = REPO / "docs/baseline/agent-capability-matrix.json"
    out_json.write_text(json.dumps(matrix, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    out_md = REPO / "docs/baseline/agent-capability-matrix.md"
    out_md.write_text(render_markdown(matrix), encoding="utf-8")
    print(f"written: {out_json.relative_to(REPO)} ({len(plugins)} plugins, {len(operations)} operations)")
    print(f"written: {out_md.relative_to(REPO)}")


def git_head():
    try:
        return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    except subprocess.CalledProcessError:
        return None


def git_branch():
    try:
        return subprocess.check_output(["git", "branch", "--show-current"], cwd=REPO, text=True).strip()
    except subprocess.CalledProcessError:
        return None


def cell(status, reason, phase=None, test_id=None, evidence=None):
    return {"status": status, "reason": reason, "phase": phase, "testId": test_id, "evidencePath": evidence}


def supports_async_camera_discovery(plugin_dir: Path) -> bool:
    """Return whether this camera plugin explicitly implements the optional async interface."""
    for source in list(plugin_dir.rglob("*.h")) + list(plugin_dir.rglob("*.cpp")):
        text = source.read_text(encoding="utf-8", errors="replace")
        if re.search(r"(?:public\s+|,\s*)ICameraDiscoveryAsync\b", text):
            return True
    return False


def build_dimensions(p):
    """阶段 0 基线七维度与后续阶段的已验证状态（见计划 §5）。
    审核整改 R1：每格必须附有效 evidencePath；testId 引用由 validate_references 自动校验。"""
    kind, name = p["kind"], p["dirName"]
    rt_loaded = p["runtime"]["loaded"]
    built = p["binaryPath"] is not None

#发现：F05——结构化插件目录已提供；设备发现仅对实际加载且显式支持 async 的相机插件开放。
    f05_test = f"{REPRO_SUITE}::testF05PluginAndCameraCatalogWithoutLifecycleTools"
    if kind == "module":
        discovery = cell("PASS", "阶段5 Slice1/2：get_available_plugins 返回结构化模块目录、二进制/加载/平台状态；"
                                   "此项只表示插件可被查询，不代表 ABI/SDK/运行时依赖可执行（F05）",
                         5, f05_test, EV_F05_REPRO)
    elif not built or not rt_loaded:
        discovery = cell("NOT_TESTED", "当前构建/隔离运行时没有加载该相机驱动；Agent 只枚举 CameraManager 已注册目标，"
                                        "不能据 metadata 推断设备发现能力（F05）",
                         5, f05_test, EV_F05_REPRO)
    elif supports_async_camera_discovery(REPO / p["sourcePath"]):
        discovery = cell("PASS", "阶段5 Slice16：已加载驱动实现 ICameraDiscoveryAsync，可由 Agent 异步批次发现并轮询；"
                                   "Manager 使用 fake plugin 验证批次/租约；不代表真实相机硬件扫描成功（F05）",
                         5, "test_cameramanager", EV_CAMERA_DISCOVERY_CTEST)
    else:
        discovery = cell("FAIL", "当前相机驱动没有 ICameraDiscoveryAsync；Slice16 不调用 legacy 同步扫描，"
                                   "因此 Agent 明确拒绝该目标（F05）",
                         5, "test_cameramanager", EV_CAMERA_DISCOVERY_CTEST)

#配置：阶段2只对完整 round - trip 与数值证据覆盖的插件判 PASS；其余逐插件仍未测试。
    if name == "NPointCalibration":
        configuration = cell("PASS", "阶段2：Agent 写入持久化点对、撤销/重做、保存重开、旧编号键迁移；"
                                     "非共线点得到有效标定和低重投影误差，共线点原子拒绝（F04）",
                             None, f"{REPRO_SUITE}::testF04CalibrationPointPairsRunAndMigrate", EV_REPRO)
    elif name in {"MeasurementInput", "CropImage", "StrFormatPlugin", "PLCWritePlugin", "SamPredict"} and rt_loaded:
        configuration = cell("PASS", "阶段2：经 Agent 原子写入复杂配置，逐项回读、撤销/重做、保存重开保真；"
                                     "非法数组/对象元素整批拒绝，端口与 metadata 一致",
                             None, "test_pluginparametercontracts", EV_CTEST)
    elif rt_loaded:
        configuration = cell("NOT_TESTED", "阶段2原子配置入口全局可用，但该插件尚无逐项复杂值/业务 round-trip 证据；"
                                           "不以共享机制替代逐插件验收", 8,
                             "test_pluginparametercontracts", EV_CTEST)
    else:
        configuration = cell("NOT_TESTED", f"二进制未构建（{p['buildDisabledReason'] or '缺失'}），契约测试静默跳过（F12）；"
                                           "Agent 配置链路属阶段 2/8", 2, None, EV_RECORD)

#输入
    if kind == "camera":
        inputs = cell("BLOCKED", "相机输入=设备前置条件（计划 §5）；本机无对应设备/平台不适用，模拟相机属阶段 5、真机属阶段 8",
                      5, None, EV_RECORD)
    elif name in ("SamPredict",):
        inputs = cell("BLOCKED", "需运行中的 SAM 服务与真实权重；模拟服务证据属阶段 5/7，真实模型属阶段 8", 8, None, EV_RECORD)
    elif p.get("runtimeDependency"):
        inputs = cell("NOT_TESTED", f"运行时依赖：{p['runtimeDependency']}；逐插件业务 fixture 属阶段 8", 8, None, EV_RECORD)
    else:
        inputs = cell("NOT_TESTED", "逐插件有效输入/受控 fixture 属阶段 8；基线无 Agent 链路输入证据", 8, None, EV_RECORD)

#执行
    if name == "Folder":
        execution = cell("PASS", "阶段5 Slice31/34：真实 Folder Delete 经 run_flow、step_once、resume_flow 执行；拒绝前目标存在且调用数为零，批准后每条路径恰执行一次",
                         5, "test_agent_folder_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-34-evidence/ctest-focused.log")
    elif name in ("PLCWrite", "PLCWritePlugin"):
        execution = cell("PASS", "阶段5 Slice32/33/35/68/69/70/82：真实 PLCWrite 经 run_flow、step_once、resume_flow；拒绝无 FC16，批准各发送恰一条有效 FC16；配置漂移失效，run_flow/step/resume 写请求无 echo 均为 SideEffectUncertain 且查询不重放。Slice68 用 PLCRead loopback QJsonArray 驱动 run_flow；Slice69/70 验证 step/resume 保留上游数组和同一 runId；Slice82 验证 step/resume FC16 不确定超时",
                         5, "test_agent_plcwrite_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-82-evidence/ctest-focused.log")
    elif name == "SaveData":
        execution = cell("PASS", "阶段5 Slice43/65：SaveData 经 Autopilot resume_flow 在 breakpoint 前 pending/拒绝均零写入；filePath A→B 使旧确认 expired 且零模块/运行/文件副作用，恢复暂停时原配置 A 后新确认批准仅写一次 A、保持 B 不变并复用原 runId",
                         5, "test_agent_folder_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-65-evidence/ctest-focused.log")
    elif name in {"WriteText", "SaveImage"}:
        slice_id = {"WriteText": 42, "SaveData": 43, "SaveImage": 44}[name]
        execution = cell("PASS", f"阶段5 Slice{slice_id}：Agent Autopilot run_flow 对{ name }配置文件目标确认；拒绝零模块执行，批准后临时目录输出恰执行一次",
                         5, "test_agent_folder_execution_g5",
                         f"build-codex-agent/agent-phase-5-slice-{slice_id}-evidence/ctest-focused.log")
        if name == "SaveImage":
            execution = cell("PASS", "阶段5 Slice44/71：SaveImage 经 run_flow、逐步 step_once 与断点 resume_flow 确认配置目标；拒绝均无文件写入，批准各仅保存一次并核验 PNG 像素、runId 与 RunFinished",
                             5, "test_agent_folder_execution_g5",
                             "build-codex-agent/agent-phase-5-slice-71-evidence/ctest-focused.log")
        elif name == "WriteText":
            execution = cell("PASS", "阶段5 Slice42/72：WriteText 配置文件目标经 run_flow、step_once 与断点 resume_flow；拒绝无写入，批准后每条路径只写入一次且文本内容精确匹配",
                             5, "test_agent_folder_execution_g5",
                             "build-codex-agent/agent-phase-5-slice-72-evidence/ctest-focused.log")
    elif name == "TCPClient":
        execution = cell("PASS", "阶段5 Slice48：Autopilot run_flow 确认绑定 TCPClient endpoint；旧端口批准因配置漂移失效，新端口 loopback 批准后仅完成一次收发",
                         5, "test_agent_tcpclient_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-48-evidence/ctest-focused.log")
        execution = cell("PASS", "阶段5 Slice48/73/81：TCPClient `WriteRead` 经 run_flow、step_once、resume_flow 确认 endpoint 与 timeout；拒绝零连接/发送，批准后一次连接与一次请求，run_flow/step/resume 超时后均保留不确定状态且不重放",
                         5, "test_agent_tcpclient_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-81-evidence/ctest-focused.log")
    elif name == "TCPServer":
        execution = cell("PASS", "阶段5 Slice49/74：TCPServer run_flow/step_once/resume_flow 确认 loopback listener 目标；拒绝无监听，批准后各仅接受一次连接并精确收发，resume 复用 runId",
                         5, "test_agent_tcpserver_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-74-evidence/ctest-focused.log")
    elif name == "PLCReadPlugin":
        execution = cell("PASS", "阶段5 Slice50/76：Autopilot run_flow/step_once/resume_flow 均确认绑定 loopback PLCRead FC03 目标；配置漂移使旧确认失效，拒绝零连接/请求，批准后各恰一条有效请求；timeout 不重试",
                         5, "test_agent_plcread_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-76-evidence/ctest-focused.log")
    elif name == "PLCCommunicatePlugin":
        execution = cell("PASS", "阶段5 Slice54/77：run_flow/step_once/resume_flow 均确认 PLCCommunicate loopback FC03 目标；step 的 port 漂移使旧确认过期后仅向新端口执行一次；resume 漂移使旧 id 过期，恢复暂停快照后拒绝零连接、批准原目标恰一条 FC03",
                         5, "test_agent_plccommunicate_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-77-evidence/ctest-focused.log")
    elif name == "SerialPort":
        execution = cell("PASS", "阶段5 Slice52/75/80：SerialPort run_flow/step_once/resume_flow 均确认绑定 PTY 目标；拒绝零字节、批准单次收发并复用 resume runId；已写出无响应时保留 SideEffectUncertain 且查询不重发",
                         5, "test_agent_serialport_execution_g5",
                         "build-codex-agent/agent-phase-5-slice-80-evidence/ctest-focused.log")
    elif name == "GrabImage":
        execution = cell("PASS", "阶段5 Slice56/79：GrabImage 经默认动态插件工厂执行 run_flow/step_once/resume_flow；fake async camera 的过期/拒绝均零采集，批准后恰一帧，resume 沿用 paused runId 并按 runId+instanceId 回读图像。空 cameraId 的 run/step/resume 在确认前 MISSING_PARAM。",
                         5, "test_agent_camera_lifecycle_g5",
                         "build-codex-agent/agent-phase-5-slice-79-evidence/ctest-focused.log")
    elif name == "SamPredict":
        execution = cell("PASS", "阶段5 Slice57/78：动态 SamPredict 插件经 run_flow/step_once/resume_flow 执行；拒绝零网络请求，serverUrl 漂移使旧确认过期，恢复暂停指纹后拒绝无请求，批准后仅一组 set_image/predict，step 与 resume 均匹配本次 runId",
                         5, "test_agent_sam_tools",
                         "build-codex-agent/agent-phase-5-slice-78-evidence/ctest-focused.log")
    elif not built:
        execution = cell("BLOCKED", f"二进制未构建：{p['buildDisabledReason'] or '未知原因'}", 8, None, EV_RECORD)
    elif kind == "camera":
        execution = cell("BLOCKED", "设备链路（发现→连接→采集→断开）属阶段 5（模拟）/阶段 8（真机）；"
                                    "运行时库加载成功仅证明 ABI/接口（见探针日志）", 5, None, EV_PROBE)
    elif name in ("GrabImage", "FindCircle"):
        execution = cell("NOT_TESTED", "存在固定脚本链间接证据（test_agente2e，主动等待引擎空闲后查询），"
                                       "不构成异步/失败/取消证据；Agent 链路执行属阶段 3/8", 3, "test_agente2e", EV_CTEST)
    else:
        execution = cell("NOT_TESTED", "Agent 链路执行/失败/取消证据属阶段 3/8；基线无该插件的执行证据", 3, None, EV_RECORD)

#输出：共享读取机制不等于逐插件业务验收，仅固定 fixture 覆盖的插件标 PASS。
    if kind == "camera":
        outputs = cell("BLOCKED", "采集输出通路属阶段 5；真机属阶段 8", 5, None, EV_RECORD)
    elif name == "Folder":
        outputs = cell("PASS", "阶段5 Slice59：Folder Delete 成功运行后，Agent 按同一 runId/instanceId 回读原生 boolean operation_result=true 与获批 folder_path；读取时目标仍已删除且模块没有重跑",
                       5, "test_agent_folder_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-59-evidence/ctest-focused.log")
    elif name in {"WriteText", "SaveData", "SaveImage"}:
        details = {
            "WriteText": "按同一 runId/实例读取 write_result、路径与长度，并与临时文本文件核对",
            "SaveData": "按同一 runId/实例读取 save_result 与路径，并与临时 JSON 文件核对",
            "SaveImage": "按同一 runId/实例读取图像摘要；临时 PNG 像素另与文件及引擎输出核对",
        }
        outputs = cell("PASS", f"阶段5 Slice46：AgentController get_module_output 返回{details[name]}", 5,
                       "test_agent_folder_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-46-evidence/ctest-focused.log")
    elif name in {"FindCircle", "DistancePP", "FitLine", "PointSurfaceDistance", "Blob", "GrabImage"}:
        outputs = cell("PASS", "阶段3：固定验收工程经 Agent 按 runId/实例读取，与引擎输出及期望值核对；"
                               "GrabImage 另核对 64 KiB 内预览坐标。",
                       None, "test_acceptance_flows", EV_CTEST)
    elif name == "FitPlane":
        outputs = cell("PASS", "阶段3：32F 倾斜高度图经真实 FitPlane 流程，Agent plane_nx 与引擎及固定斜率核对。",
                       None, "test_3d_batch3", EV_CTEST)
    elif name == "TCPClient":
        outputs = cell("PASS", "阶段5 Slice48：Agent 按同一 runId/实例回读 TCPClient loopback 响应与 UTF-8 字节数",
                       5, "test_agent_tcpclient_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-48-evidence/ctest-focused.log")
    elif name == "TCPServer":
        outputs = cell("PASS", "阶段5 Slice49：Agent 按同一 runId/实例回读 TCPServer loopback 收到的 UTF-8 请求和字节数",
                       5, "test_agent_tcpserver_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-49-evidence/ctest-focused.log")
    elif name == "PLCReadPlugin":
        outputs = cell("PASS", "阶段5 Slice50：Agent 以同一成功 runId 分页回读 PLCRead 原生 QJsonArray 的三个 FC03 寄存器与 _count；失败 runId 不返回旧数组/计数",
                       5, "test_agent_plcread_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-50-evidence/ctest-focused-final.log")
    elif name == "PLCCommunicatePlugin":
        outputs = cell("PASS", "阶段5 Slice54：Agent 以同一成功 runId/instanceId 回读 PLCCommunicate 本次 `plc_connected=true` 与 response time 输出",
                       5, "test_agent_plccommunicate_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-54-evidence/ctest-focused.log")
    elif name == "SerialPort":
        outputs = cell("PASS", "阶段5 Slice52：Agent 以同一成功 runId/module instance 回读 SerialPort PTY UTF-8 响应及 _bytes，字节数与响应相符",
                       5, "test_agent_serialport_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-52-evidence/ctest-focused.log")
    elif name in {"PLCWrite", "PLCWritePlugin"}:
        outputs = cell("PASS", "阶段5 Slice51：Agent 以 run_flow、step_once、resume_flow 各自成功 runId 回读 PLCWrite plc_write_success/count；FC16 uncertain run 不返回成功输出",
                       5, "test_agent_plcwrite_execution_g5",
                       "build-codex-agent/agent-phase-5-slice-51-evidence/ctest-focused.log")
    elif name == "SamPredict":
        outputs = cell("PASS", "阶段5 Slice58：SamPredict 成功流程后，Agent 按同一 runId/instanceId 经 get_module_output 回读 Region2D label/bbox/polygon/score 与 score 结果；RLE 不内联展开，回读不新增网络请求",
                       5, "test_agent_sam_tools",
                       "build-codex-agent/agent-phase-5-slice-58-evidence/ctest-focused.log")
    elif name == "MeasurementInput":
        outputs = cell("PASS", "阶段3：Agent 按 runId 读取点坐标，与引擎输出核对。",
                       None, f"{REPRO_SUITE}::testG3MeasurementOutputByRunId", EV_REPRO)
    else:
        outputs = cell("NOT_TESTED", "阶段3共享 runId/输出/分页机制已覆盖代表性插件；该插件尚无 Agent 业务输出 fixture，"
                                    "不以共享机制推断逐插件 PASS。", 8,
                       f"{REPRO_SUITE}::testF01FixedGetRunResultsReturnsRunIdentity", EV_REPRO)

#权限：阶段1 已收口保守确认——run_flow 执行入口三权限级统一门禁（F13 修复）；
#代表性端到端证据：Folder(Delete) 与 PLCWrite 覆盖 run_flow/step_once/resume_flow；
#逐插件设备目标绑定与精细副作用判定属阶段 5
    if name == "Folder":
        permission = cell("PASS", "阶段5 Slice31/34/60：真实 Folder Delete 的 run_flow/step_once/resume_flow 均确认前与拒绝后零删除；"
                                  "folderPath 漂移使旧 confirmationId expired 且两目标均保留，重新确认后仅删除批准目标一次", 5,
                          "test_agent_folder_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-60-evidence/ctest-focused.log")
    elif name in ("PLCWrite", "PLCWritePlugin"):  # 目录名带 Plugin 后缀
        permission = cell("PASS", "阶段5 Slice32/33/35/68/69/70/82：真实 PLCWrite 的 run_flow/step_once/resume_flow 均确认前与拒绝后零 FC16；"
                                  "批准后恰一条 FC16，配置漂移失效且无 echo 不自动重放。Slice68 变量来源确认不泄露运行时值；Slice69/70 step/resume 写入上游值恰一次；Slice82 step/resume 已发送无 echo 后保持 SideEffectUncertain/retryable=false", 5,
                          "test_agent_plcwrite_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-82-evidence/ctest-focused.log")
    elif name == "SaveData":
        permission = cell("PASS", "阶段5 Slice43/64/65/66/67：静态 run_flow 文件输出拒绝零写入、新确认后只执行一次；step_once 目标漂移使旧确认 expired；resume_flow 拒绝、漂移失效和恢复原配置后恰写一次；空 filePath 在 Autopilot 聊天/Bridge 的 run_flow/step_once/resume_flow 均 fail closed，operation=Delete 不误报 Folder 删除",
                          5, "test_agent_folder_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-67-evidence/ctest-focused.log")
    elif name == "WriteText":
        permission = cell("PASS", "阶段5 Slice42/66/67/72：静态 filePath 的 Autopilot run_flow/step_once/resume_flow 拒绝零写入，批准后各只写一次且目标/内容匹配；空 filePath 的聊天/Bridge run/step/resume 均 fail-closed",
                          5, "test_agent_folder_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-72-evidence/ctest-focused.log")
    elif name == "SaveImage":
        permission = cell("PASS", "阶段5 Slice44/71：run_flow、step_once、resume_flow 的临时 PNG 输出目标均在确认载荷中绑定；拒绝时零模块/文件副作用，批准后各只写一次并核对像素",
                          5, "test_agent_folder_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-71-evidence/ctest-focused.log")
    elif name == "TCPClient":
        permission = cell("PASS", "阶段5 Slice48/73/81：确认载荷显示 host/port/operation/timeout；run_flow 配置漂移旧确认失效，step_once/resume_flow 拒绝零连接与字节；批准各恰一次请求，超时不确定路径不重放",
                          5, "test_agent_tcpclient_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-81-evidence/ctest-focused.log")
    elif name == "TCPServer":
        permission = cell("PASS", "阶段5 Slice49/74：确认摘要披露 bind scope/port/timeout/operation 且不泄漏响应；run_flow/step/resume 拒绝零 listener，批准后各仅一次 loopback 收发",
                          5, "test_agent_tcpserver_execution_g5",
                          "build-codex-agent/agent-phase-5-slice-74-evidence/ctest-focused.log")
    elif name == "PLCReadPlugin":
        permission = cell("PASS", "阶段5 Slice50/76：确认摘要披露 PLCRead host/port/slave/address/count/timeout；配置漂移旧确认失效，run_flow/step_once/resume_flow 拒绝均零连接/FC03/模块启动",
                          5, "test_agent_plcread_execution_g5",
                           "build-codex-agent/agent-phase-5-slice-76-evidence/ctest-focused.log")
    elif name == "PLCCommunicatePlugin":
        permission = cell("PASS", "阶段5 Slice54/77：确认摘要披露 PLCCommunicate host/port/固定 FC03 范围与 timeout；step/resume port 漂移均使旧 confirmationId expired；拒绝零连接，批准路径每次仅一条有效请求与响应，resume 复用暂停 runId",
                           5, "test_agent_plccommunicate_execution_g5",
                           "build-codex-agent/agent-phase-5-slice-77-evidence/ctest-focused.log")
    elif name == "SerialPort":
        permission = cell("PASS", "阶段5 Slice52/75/80：确认披露串口目标与格式/operation/timeout 并隐藏负载；step/resume 拒绝零字节，已写出无响应时报 SideEffectUncertain/retryable=false，重复查询不重发",
                           5, "test_agent_serialport_execution_g5",
                           "build-codex-agent/agent-phase-5-slice-80-evidence/ctest-focused.log")
    elif name == "GrabImage":
        permission = cell("PASS", "阶段5 Slice56/79：Camera 确认绑定 instance/device/exposure/gain/timeout；step_once/resume_flow 过期/拒绝保持 session 与采集计数不变，批准仅采一帧，resume 保留原 runId。无 cameraId 的 run/step/resume 在确认前 fail-closed。",
                           5, "test_agent_camera_lifecycle_g5",
                           "build-codex-agent/agent-phase-5-slice-79-evidence/ctest-focused.log")
    elif name == "SamPredict":
        permission = cell("PASS", "阶段5 Slice57/78：SamPredict 确认摘要绑定 instanceId 和脱敏服务 origin，并披露上游图像会发送；run_flow/step/resume 拒绝零请求，serverUrl 漂移使旧确认过期，批准仅向确认 endpoint 发送一组 set_image/predict",
                          5, "test_agent_sam_tools",
                          "build-codex-agent/agent-phase-5-slice-78-evidence/ctest-focused.log")
    elif p["sideEffect"] in ("file", "network", "device", "mixed"):
        permission = cell("NOT_TESTED", "run_flow 保守确认机制已全局生效（门禁与流程内容无关），代表性端到端证据见 "
                                        "Folder/PLCWrite 用例；该插件的逐项确认与设备目标绑定证据属阶段 5", 5,
                          f"{REPRO_SUITE}::testG1PlcFlowZeroSideEffectBeforeConfirmation", EV_REPRO)
    elif kind == "camera":
        permission = cell("NOT_TESTED", "相机无 Agent 工具入口（F05）；设备链路权限证据属阶段 5", 5, None, EV_RECORD)
    else:
        permission = cell("NOT_APPLICABLE", "纯计算插件，无外部副作用；全局三模式×三入口权限机制由 "
                                            "test_agentpermissions/test_agentcontroller 覆盖"
                                            "（不计入通过分母；不适用依据见计划 §5 维度表）", None,
                          "test_agentpermissions", EV_CTEST)

#撤销 / 持久化
    if kind == "camera" or p["sideEffect"] in ("device", "network"):
        undo = cell("NOT_APPLICABLE", "设备/网络动作不可撤销，须明确标识（阶段 1 §6 undoable 语义）；无工程内可撤销状态",
                    1, None, EV_RECORD)
    elif p["sideEffect"] == "file":
        undo = cell("NOT_TESTED", "阶段1：审计 undoable 误标已修复（save_project/run_flow 记为不可撤销,见 "
                                  "testF06FixedControllerLogsFailedSaveAsSuccess→Fixed 用例）；文件动作本身不可撤销"
                                  "已显式标识,该插件的保存重开保真证据属阶段 4/8", 4,
                    f"{REPRO_SUITE}::testF06FixedControllerLogsFailedSaveAsError", EV_REPRO)
    else:
        undo = cell("NOT_TESTED", "参数级 Undo/Redo 有代表性全局证据（test_agentundo：GrabImage/MeasureCircle/"
                                  "MeasurementInput/FitCircle）；阶段4已验证共享模块顺序 move Undo/Redo 与外部变更 stale 保护"
                                  "（test_agentactor/test_mainwindow），但不外推为逐插件参数/配置保存重开 PASS；逐插件闭环仍属阶段4/8", 4,
                    "test_mainwindow", "build-codex-agent/agent-phase4-slice27-evidence/ctest-loopback.xml")

    return {"discovery": discovery, "configuration": configuration, "input": inputs,
            "execution": execution, "output": outputs, "permission": permission,
            "undo_persistence": undo}


def strip_cpp_comments(text):
    comments = re.compile(
        r'//[^\n]*|/\*[\s\S]*?\*/|(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|'
        r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')

    def preserve_literals(match):
        value = match.group()
        if value.startswith(("//", "/*")):
            return "".join("\n" if char == "\n" else " " for char in value)
        return value

    return comments.sub(preserve_literals, text)


def strip_cpp_comments_and_literals(text):
    """Keep source structure while removing comments and literal false matches."""
    clean = strip_cpp_comments(text)
    literals = re.compile(
        r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|'
        r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
    return literals.sub(lambda match: "".join("\n" if char == "\n" else " " for char in match.group()), clean)


def strip_cpp_comments_and_strings(text):
    """Remove comments and strings before scanning directives, retaining chars."""
    clean = strip_cpp_comments(text)
    strings = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|'
                         r'"(?:\\.|[^"\\])*"')
    return strings.sub(lambda match: "".join("\n" if char == "\n" else " " for char in match.group()), clean)


def strip_cpp_comments_and_raw_literals(text):
    """Keep ordinary objectName strings, but remove comments and raw strings."""
    clean = strip_cpp_comments(text)
    raw_literals = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"')
    return raw_literals.sub(lambda match: "".join("\n" if char == "\n" else " " for char in match.group()), clean)


def strip_disabled_if_zero_blocks(text):
    """Drop preprocessor branches that are provably inactive from constants."""
    kept = []
    frames = []
    active = True

    def constant_condition(expression):
        # In #if/#elif, C++ treats integer types as intmax_t/uintmax_t.
        int_width = 64
        integer_pattern = re.compile(
            r"(?<![A-Za-z0-9_.])(?:0[xX][0-9A-Fa-f]+|0[bB][01]+|0[0-7]*|[1-9][0-9]*)"
            r"(?:[uU](?:[lL]{1,2})?|[lL]{1,2}[uU]?)?(?![A-Za-z0-9_])")
        integer_literals = {}

        def replace_character(match):
            body = match.group(1)
            simple_escapes = {"\\0": 0, "\\a": 7, "\\b": 8, "\\t": 9, "\\n": 10,
                              "\\v": 11, "\\f": 12, "\\r": 13, "\\\\": 92,
                              "\\'": 39, '\\"': 34}
            if body in simple_escapes:
                return str(simple_escapes[body])
            if re.fullmatch(r"\\[0-7]{1,3}", body):
                return str(int(body[1:], 8))
            if re.fullmatch(r"\\x[0-9A-Fa-f]+", body):
                return str(int(body[2:], 16))
            if len(body) == 1:
                return str(ord(body))
            return "__unknown_character_constant"

        character_literal = re.compile(
            r"(?:u8|u|U|L)?'((?:\\x[0-9A-Fa-f]+|\\u[0-9A-Fa-f]{4}|\\U[0-9A-Fa-f]{8}|"
            r"\\[0-7]{1,3}|\\.|[^'\\]))'")
        expression = character_literal.sub(replace_character, expression.strip())

        def replace_integer(match):
            token = match.group()
            suffix_match = re.search(r"[uUlL]+$", token)
            suffix = suffix_match.group().lower() if suffix_match else ""
            digits = token[:-len(suffix)] if suffix else token
            if digits.lower().startswith("0x"):
                value = int(digits[2:], 16)
            elif digits.lower().startswith("0b"):
                value = int(digits[2:], 2)
            elif len(digits) > 1 and digits.startswith("0"):
                value = int(digits, 8)
            else:
                value = int(digits, 10)
            width = int_width
            unsigned = "u" in suffix or value >= (1 << (width - 1))
            name = f"__c_integer_{len(integer_literals)}"
            integer_literals[name] = (value, unsigned, width)
            return name

        expression = integer_pattern.sub(replace_integer, expression)
        expression = expression.replace("&&", " and ").replace("||", " or ")

        def consume_unary_operand(source, start):
            cursor = start
            while cursor < len(source) and source[cursor].isspace():
                cursor += 1
            while cursor < len(source) and source[cursor] in "!~+-":
                cursor += 1
                while cursor < len(source) and source[cursor].isspace():
                    cursor += 1
            if cursor >= len(source):
                return cursor
            if source[cursor] == "(":
                depth = 1
                cursor += 1
                while cursor < len(source) and depth:
                    if source[cursor] == "(":
                        depth += 1
                    elif source[cursor] == ")":
                        depth -= 1
                    cursor += 1
                return cursor
            token = re.match(r"[A-Za-z_]\w*|[0-9]+", source[cursor:])
            if not token:
                return cursor
            cursor += token.end()
            if re.match(r"\s*\(", source[cursor:]):
                open_paren = source.index("(", cursor)
                depth = 1
                cursor = open_paren + 1
                while cursor < len(source) and depth:
                    if source[cursor] == "(":
                        depth += 1
                    elif source[cursor] == ")":
                        depth -= 1
                    cursor += 1
            return cursor

        def convert_logical_not(source):
            output = []
            index = 0
            while index < len(source):
                if source[index] != "!" or (index + 1 < len(source) and source[index + 1] == "="):
                    output.append(source[index])
                    index += 1
                    continue
                start = index + 1
                while start < len(source) and source[start].isspace():
                    start += 1
                if start >= len(source):
                    return source
                end = consume_unary_operand(source, start)
                if end <= start:
                    return source
                operand = convert_logical_not(source[start:end].strip())
                output.append(f"(({operand}) == 0)")
                index = end
            return "".join(output)

        expression = convert_logical_not(expression)

        def convert_ternary(source):
            grouped = []
            index = 0
            while index < len(source):
                if source[index] != "(":
                    grouped.append(source[index])
                    index += 1
                    continue
                depth, end = 1, index + 1
                while end < len(source) and depth:
                    if source[end] == "(":
                        depth += 1
                    elif source[end] == ")":
                        depth -= 1
                    end += 1
                if depth:
                    return source
                grouped.append("(" + convert_ternary(source[index + 1:end - 1]) + ")")
                index = end
            source = "".join(grouped)
            depth = 0
            question = None
            for index, char in enumerate(source):
                if char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
                elif char == "?" and depth == 0:
                    question = index
                    break
            if question is None:
                return source
            depth, nested, colon = 0, 0, None
            for index in range(question + 1, len(source)):
                char = source[index]
                if char == "(":
                    depth += 1
                elif char == ")":
                    depth -= 1
                elif depth == 0 and char == "?":
                    nested += 1
                elif depth == 0 and char == ":":
                    if nested == 0:
                        colon = index
                        break
                    nested -= 1
            if colon is None:
                return source
            condition = source[:question]
            when_true = convert_ternary(source[question + 1:colon])
            when_false = convert_ternary(source[colon + 1:])
            return f"({when_true} if ({condition}) else {when_false})"

        expression = convert_ternary(expression)
        try:
            tree = ast.parse(expression, mode="eval").body
        except SyntaxError:
            return None

        def c_integer_division(left, right):
            quotient = abs(left) // abs(right)
            return -quotient if (left < 0) != (right < 0) else quotient

        def common_type(left, right):
            left_value, left_unsigned, left_width = left
            right_value, right_unsigned, right_width = right
            width = max(left_width, right_width)
            if left_unsigned == right_unsigned:
                unsigned = left_unsigned
            else:
                unsigned_width = left_width if left_unsigned else right_width
                signed_width = right_width if left_unsigned else left_width
                unsigned = unsigned_width >= signed_width
            if unsigned:
                mask = (1 << width) - 1
                left_value &= mask
                right_value &= mask
            return (left_value, unsigned, width), (right_value, unsigned, width)

        def make_integer(value, unsigned=False, width=int_width):
            if unsigned:
                value %= 1 << width
            return (value, unsigned, width)

        binary_ops = {ast.Add: lambda a, b: a + b, ast.Sub: lambda a, b: a - b,
                      ast.Mult: lambda a, b: a * b, ast.Div: c_integer_division,
                      ast.FloorDiv: c_integer_division,
                      ast.Mod: lambda a, b: a - c_integer_division(a, b) * b,
                      ast.LShift: lambda a, b: a << b, ast.RShift: lambda a, b: a >> b,
                      ast.BitAnd: lambda a, b: a & b, ast.BitOr: lambda a, b: a | b,
                      ast.BitXor: lambda a, b: a ^ b}
        compare_ops = {ast.Eq: lambda a, b: a == b, ast.NotEq: lambda a, b: a != b,
                       ast.Lt: lambda a, b: a < b, ast.LtE: lambda a, b: a <= b,
                       ast.Gt: lambda a, b: a > b, ast.GtE: lambda a, b: a >= b}

        def evaluate(node):
            if isinstance(node, ast.Constant) and isinstance(node.value, (int, bool)):
                return make_integer(int(node.value))
            if isinstance(node, ast.Name):
                return integer_literals.get(node.id)
            if isinstance(node, ast.UnaryOp):
                value = evaluate(node.operand)
                if value is None:
                    return None
                number, unsigned, width = value
                if isinstance(node.op, ast.Not):
                    return make_integer(int(not number))
                if isinstance(node.op, ast.Invert):
                    return make_integer(~number, unsigned, width)
                if isinstance(node.op, ast.UAdd):
                    return value
                if isinstance(node.op, ast.USub):
                    return make_integer(-number, unsigned, width)
            if isinstance(node, ast.BoolOp):
                values = [evaluate(value) for value in node.values]
                if isinstance(node.op, ast.And):
                    if any(value is not None and value[0] == 0 for value in values):
                        return make_integer(0)
                    return make_integer(1) if all(value is not None and value[0] != 0 for value in values) else None
                if isinstance(node.op, ast.Or):
                    if any(value is not None and value[0] != 0 for value in values):
                        return make_integer(1)
                    return make_integer(0) if all(value is not None for value in values) else None
            if isinstance(node, ast.IfExp):
                condition = evaluate(node.test)
                if condition is not None:
                    return evaluate(node.body if condition[0] != 0 else node.orelse)
                body, otherwise = evaluate(node.body), evaluate(node.orelse)
                if body is not None and otherwise is not None and body[0] == otherwise[0]:
                    return body
                return None
            if isinstance(node, ast.BinOp) and type(node.op) in binary_ops:
                left, right = evaluate(node.left), evaluate(node.right)
                if left is None or right is None:
                    return None
                left, right = common_type(left, right)
                left_value, unsigned, width = left
                right_value = right[0]
                try:
                    value = binary_ops[type(node.op)](left_value, right_value)
                    return make_integer(value, unsigned, width)
                except (ArithmeticError, ValueError):
                    return None
            if isinstance(node, ast.Compare) and len(node.ops) == 1 and type(node.ops[0]) in compare_ops:
                left, right = evaluate(node.left), evaluate(node.comparators[0])
                if left is None or right is None:
                    return None
                left, right = common_type(left, right)
                return make_integer(int(compare_ops[type(node.ops[0])](left[0], right[0])))
            return None

        value = evaluate(tree)
        return None if value is None else value[0] != 0

    for line in text.splitlines(keepends=True):
        directive = re.match(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$", line)
        if directive:
            command, expression = directive.group(1), directive.group(2).strip()
            if command in {"if", "ifdef", "ifndef"}:
                condition = constant_condition(expression) if command == "if" else None
                parent_active = active
                active = parent_active and condition is not False
                frames.append({"parent": parent_active, "guaranteed": condition is True})
            elif command == "elif" and frames:
                frame = frames[-1]
                condition = constant_condition(expression)
                active = frame["parent"] and not frame["guaranteed"] and condition is not False
                if condition is True:
                    frame["guaranteed"] = True
            elif command == "else" and frames:
                frame = frames[-1]
                active = frame["parent"] and not frame["guaranteed"]
                frame["guaranteed"] = True
            elif command == "endif" and frames:
                frame = frames.pop()
                active = frame["parent"]
            continue
        if active:
            kept.append(line)
    return "".join(kept)


def validate_owner_api_references(operations, source_texts=None):
    """Reject stale or misattributed C++ methods and named Qt objectNames."""
    if source_texts is None:
        source_files = [p for p in (REPO / "src").rglob("*")
                        if p.is_file() and p.suffix in {".h", ".hpp", ".cpp", ".ui"}]
        raw_sources = {str(p): p.read_text(encoding="utf-8", errors="ignore") for p in source_files}
    else:
        raw_sources = source_texts
    sources = {path: strip_cpp_comments_and_literals(strip_disabled_if_zero_blocks(
        strip_cpp_comments_and_strings(text))) for path, text in raw_sources.items()}
    external = {("QApplication", "quit"), ("QMessageBox", "about")}
    errors = []
    checked = set()

    def class_bodies(class_name):
        bodies = []
        for text in sources.values():
            for match in re.finditer(r"\bclass\b[^;{]*\b" + re.escape(class_name) + r"\b[^;{]*\{", text):
                start = match.end()
                depth = 1
                pos = start
                while pos < len(text) and depth:
                    if text[pos] == "{":
                        depth += 1
                    elif text[pos] == "}":
                        depth -= 1
                    pos += 1
                if depth == 0:
                    bodies.append(text[start:pos - 1])
        return bodies

    for operation in operations:
        owner = operation.get("ownerApi", "")
        for class_name, method in re.findall(r"\b([A-Za-z_]\w*)::([A-Za-z_]\w*)", owner):
            pair = (class_name, method)
            if pair in external or pair in checked:
                continue
            checked.add(pair)
            bodies = class_bodies(class_name)
            if method.startswith("m_"):
                declared = any(re.search(r"\b" + re.escape(method) + r"\b", body) for body in bodies)
                implemented = False
            else:
                declared = any(re.search(r"\b" + re.escape(method) + r"\s*\(", body) for body in bodies)
                implemented = any(re.search(r"\b" + re.escape(class_name) + r"\s*::\s*" +
                                             re.escape(method) + r"\s*\(", text)
                                  for text in sources.values())
            if len(bodies) > 1:
                errors.append(f"{operation.get('operationId')}: {class_name} is ambiguous across source namespaces")
            elif not declared and not implemented:
                errors.append(f"{operation.get('operationId')}: {class_name}::{method} not found on that class")
        for widget_name in re.findall(
                r"\b[A-Za-z_]\w*(?:SpinBox|PushButton|ToolButton|Btn|Button|Action|Edit|Combo|Check|Label|Tabs|Tab)\b",
                owner):
            if not widget_name[0].isupper():
                continue
            object_name = re.compile(r"setObjectName\s*\(\s*(?:QStringLiteral\s*\(\s*)?[\"']" +
                                     re.escape(widget_name) + r"[\"']")
            if not any(object_name.search(strip_cpp_comments_and_raw_literals(text))
                       for text in raw_sources.values()):
                errors.append(f"{operation.get('operationId')}: widget {widget_name} not found in src/")
    if errors:
        raise ValueError("GUI ownerApi reference validation failed:\n  " + "\n  ".join(errors))
    print(f"ownerApi source validation OK: {len(checked)} explicit C++ references")


def validate_references(matrix):
    """审核整改 R1：自动校验矩阵全部 testId 与 evidencePath 引用。
    - testId 形如 'test_agentphase0_repro::testXxx' → 用例必须存在于复现测试源码
    - testId 为裸套件名 → 必须存在于 tests/CMakeLists.txt 的 TEST_SOURCES
    - evidencePath 必须为仓库内实际存在的文件
    任一校验失败即拒绝生成（防止再次出现引用不存在用例/空证据的矩阵）。"""
    repro_src = (REPO / "tests" / f"{REPRO_SUITE}.cpp").read_text(encoding="utf-8")
    repro_cases = set(re.findall(r"void\s+(test\w+)\s*\(\)", repro_src))
    cmake_txt = (REPO / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    suites = set(re.findall(r"(test_\w+)\.cpp", cmake_txt))

    errors, cells = [], 0
    for p in matrix["plugins"]:
        build_code = p.get("buildReasonCode")
        runtime_code = p["runtime"].get("reasonCode")
        valid_build_codes = {None, "PLATFORM_UNSUPPORTED", "BUILD_OPTION_DISABLED", "BINARY_MISSING"}
        valid_runtime_codes = valid_build_codes | {"RUNTIME_NOT_REPORTED", "RUNTIME_LOAD_FAILED"}
        if build_code not in valid_build_codes:
            errors.append(f"{p['dirName']}: 未知 buildReasonCode: {build_code}")
        if runtime_code not in valid_runtime_codes:
            errors.append(f"{p['dirName']}: 未知 runtime.reasonCode: {runtime_code}")
        if p["runtime"].get("loaded") is True and runtime_code is not None:
            errors.append(f"{p['dirName']}: runtime loaded 但 reasonCode 非空")
        for dname, c in p["dimensions"].items():
            cells += 1
            where = f"{p['dirName']}/{dname}"
            ev = c.get("evidencePath")
            if not ev:
                errors.append(f"{where}: evidencePath 缺失")
            elif not (REPO / ev).exists():
                errors.append(f"{where}: evidencePath 不存在: {ev}")
            tid = c.get("testId")
            if tid:
                if tid.startswith(REPRO_SUITE + "::"):
                    case = tid.split("::", 1)[1]
                    if case not in repro_cases:
                        errors.append(f"{where}: testId 引用不存在的复现用例: {tid}")
                elif tid not in suites:
                    errors.append(f"{where}: testId 引用未注册的测试套件: {tid}")
        if p.get("buildReasonCode") and not p.get("buildDisabledReason"):
            errors.append(f"{p['dirName']}: buildReasonCode 缺少 buildDisabledReason")
        if not p.get("buildEnabled") and not p.get("buildReasonCode"):
            errors.append(f"{p['dirName']}: 构建不可用但缺少 buildReasonCode")
        if p["runtime"].get("reasonCode") and not p["runtime"].get("error"):
            errors.append(f"{p['dirName']}: runtime.reasonCode 缺少具体 runtime.error")
        if p.get("binaryPath") is None and p.get("buildReasonCode") not in {
                "PLATFORM_UNSUPPORTED", "BUILD_OPTION_DISABLED", "BINARY_MISSING"}:
            errors.append(f"{p['dirName']}: 缺二进制且 buildReasonCode 无法归因")
        if p["runtime"].get("loaded") is False and not p["runtime"].get("reasonCode"):
            errors.append(f"{p['dirName']}: runtime load failure 缺少 reasonCode")
    audit = matrix.get("inventoryAudit", {})
    audit_matches = (audit.get("actual") == audit.get("expected")
                     and not audit.get("addedSincePrevious")
                     and not audit.get("removedSincePrevious"))
    if audit.get("matchesExpected") != audit_matches:
        errors.append("inventoryAudit.matchesExpected 与实际清单差异不一致")
    if errors:
        print("REFERENCE VALIDATION FAILED:", file=sys.stderr)
        for e in errors:
            print("  -", e, file=sys.stderr)
        sys.exit(1)
    print(f"reference validation OK: {cells} cells, all testId/evidencePath resolved")
    print("inventory audit:", json.dumps(matrix.get("inventoryAudit", {}),
                                           ensure_ascii=False, sort_keys=True))


def summarize(plugins, operations):
    dims = {}
    for p in plugins:
        for d, c in p["dimensions"].items():
            dims.setdefault(d, {}).setdefault(c["status"], 0)
            dims[d][c["status"]] += 1
    modules = [p for p in plugins if p["kind"] == "module"]
    cameras = [p for p in plugins if p["kind"] == "camera"]
    return {
        "metadataTotal": len(plugins),
        "modules": len(modules),
        "cameras": len(cameras),
        "builtBinaries": sum(1 for p in plugins if p["binaryPath"]),
        "buildDisabled": [{
                "dirName" : p["dirName"], "reason" : p["buildDisabledReason"],
                "reasonCode": p["buildReasonCode"]} for p in plugins if not p["buildEnabled"]],
        "runtimeLoadedModules": sum(1 for p in modules if p["runtime"]["loaded"]),
        "runtimeProbeReported": sum(1 for p in plugins if p["runtime"]["loaded"] is not None),
        "dimensionStatus": dims,
        "operationsTotal": len(operations),
        "operationsByMapping": _count_by(operations, "toolMapping"),
        "operationsByPhase": _count_by(operations, "phase"),
        }

def _count_by(items, key):
    out = {}
    for it in items:
        k = str(it.get(key))
        out[k] = out.get(k, 0) + 1
    return out


def render_markdown(mx):
    L = []
    L.append("# Agent 能力基线矩阵（阶段 0）\n")
    L.append("> 本文件由 `scripts/audit_agent_capability_baseline.py` 从 "
             "`agent-capability-matrix.json`（唯一数据源）渲染，禁止手工编辑结论。\n")
    b = mx["baseline"]
    c = mx["counts"]
    L.append(f"- 生成时间：{mx['generatedAt']}；HEAD：`{b['head']}`；分支：{b['branch']}；构建目录：{b['buildDir']}")
    L.append(f"- 元数据总数：**{c['metadataTotal']}**（模块 {c['modules']} / 相机 {c['cameras']}）；"
             f"已构建二进制：{c['builtBinaries']}；运行时探针报告加载模块：{c['runtimeLoadedModules']}")
    L.append(f"- 未构建项：{', '.join(x['dirName'] + '（' + (x['reason'] or '') + '）' for x in c['buildDisabled']) or '无'}")
    inv = mx["inventoryAudit"]
    L.append(f"- 清单核验：期望 `{inv['expected']}`；实际 `{inv['actual']}`；匹配：**{inv['matchesExpected']}**；"
             f"新增：{inv['addedSincePrevious'] or '无'}；移除：{inv['removedSincePrevious'] or '无'}")
    L.append("- 构建原因码：" + (", ".join(
        f"{x['dirName']}=`{x['reasonCode']}`" for x in c["buildDisabled"]) or "无") + "\n")
    L.append(f"- GUI 操作清单：{c['operationsTotal']} 项；按映射：{c['operationsByMapping']}；按阶段：{c['operationsByPhase']}\n")

    L.append("## 维度状态分母（计划 §5：按维度报告，不给单一总百分比）\n")
    L.append("| 维度 | PASS | FAIL | BLOCKED | NOT_APPLICABLE | NOT_TESTED | 分母 |")
    L.append("| --- | --- | --- | --- | --- | --- | --- |")
    for d, st in c["dimensionStatus"].items():
        total = sum(st.values())
        L.append(f"| {d} | {st.get('PASS',0)} | {st.get('FAIL',0)} | {st.get('BLOCKED',0)} | "
                 f"{st.get('NOT_APPLICABLE',0)} | {st.get('NOT_TESTED',0)} | {total} |")
    L.append("")

    L.append("## 插件矩阵（69 项逐项，含未构建原因）\n")
    L.append("| 插件 | 类别 | 二进制 | 构建原因码 | 运行时 | 运行时原因码 | 副作用 | 台账状态 | 发现 | 配置 | 输入 | 执行 | 输出 | 权限 | 撤销 |")
    L.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for p in mx["plugins"]:
        rt = p["runtime"]
        rt_txt = "loaded" if rt["loaded"] else ("fail" if rt["loaded"] is False else "n/a")
        d = p["dimensions"]
        legacy = (p["legacyStatus"] or {}).get("status", "—")
        L.append(f"| {p['name']} | {p['kind']}/{p['category']} | {'✓' if p['binaryPath'] else '✗ ' + (p['buildDisabledReason'] or '缺失')} "
                 f"| {p['buildReasonCode'] or '—'} | {rt_txt} | {rt['reasonCode'] or '—'} | {p['sideEffect']} | {legacy} "
                 f"| {d['discovery']['status']} | {d['configuration']['status']} | {d['input']['status']} "
                 f"| {d['execution']['status']} | {d['output']['status']} | {d['permission']['status']} "
                 f"| {d['undo_persistence']['status']} |")
    L.append("")

    L.append("## 维度单元格理由（FAIL/BLOCKED 摘要，全量见 JSON）\n")
    for p in mx["plugins"]:
        for dn, cc in p["dimensions"].items():
            if cc["status"] in ("FAIL", "BLOCKED"):
                ph = f"，收口阶段 {cc['phase']}" if cc.get("phase") else ""
                L.append(f"- **{p['name']}** / {dn} = {cc['status']}：{cc['reason']}{ph}")
    L.append("")

    L.append("## GUI 操作清单\n")
    L.append("| 操作 ID | 区域 | 操作 | ownerApi | 工具映射 | 阶段 |")
    L.append("| --- | --- | --- | --- | --- | --- |")
    for o in mx["operations"]:
        L.append(f"| {o['operationId']} | {o['area']} | {o['description']} | `{o['ownerApi']}` | {o['toolMapping']} | {o['phase']} |")
    L.append("")
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("command", choices=["prepare-probe", "generate"])
    ap.add_argument("--build-dir", default="build-codex-agent")
    ap.add_argument("--probe-dir", default="/tmp/dlx-phase0-probe")
    ap.add_argument("--runtime-log", default=None,
                    help="list-plugins 探针合并输出（stdout+stderr）路径")
    ap.add_argument("--operations", default="docs/baseline/agent-gui-operations.json")
    args = ap.parse_args()
    if args.command == "prepare-probe":
        cmd_prepare_probe(args)
    else:
        cmd_generate(args)


if __name__ == "__main__":
    main()
