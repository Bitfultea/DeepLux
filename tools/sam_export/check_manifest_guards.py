#!/usr/bin/env python3
"""manifest/包校验拒绝用例回归（阶段1 复核二轮 P1-2）。

复核发现：load_manifest 初版在缺少 preprocessing/coordinateTransform/
semantics 时仍接受（内存删字段可复现），files.*.file 接受绝对路径，
不能保证包自包含。本脚本以临时目录构造最小合法包，逐项突变并断言
load_manifest 失败关闭（PackageError）；正向对照断言合法包通过。

用法：
  PYTHONPATH=<deps> ~/.deeplux/sam_env/bin/python check_manifest_guards.py
退出码：0 全部通过；1 存在失败。
"""

import hashlib
import json
import os
import shutil
import sys
import tempfile

import sam_package_common as C

PASS = 0
FAIL = 0


def build_valid_manifest(files_meta):
    return {
        "formatVersion": 1,
        "family": "sam1",
        "variant": "vit_b",
        "files": files_meta,
        **C.v1_runtime_semantics(),
    }


def make_package(root, mutate=None):
    """构造最小合法包（哑元 onnx 文件 + 正确哈希），可选突变 manifest。"""
    pkg = os.path.join(root, "pkg")
    os.makedirs(pkg, exist_ok=True)
    files_meta = {}
    for name in ("encoder", "decoder"):
        path = os.path.join(pkg, f"{name}.onnx")
        with open(path, "wb") as f:
            f.write(b"DUMMY-" + name.encode())
        files_meta[name] = {
            "file": f"{name}.onnx",
            "sha256": hashlib.sha256(open(path, "rb").read()).hexdigest(),
            "sizeBytes": os.path.getsize(path),
        }
    manifest = build_valid_manifest(files_meta)
    if mutate:
        mutate(manifest, pkg)
    with open(os.path.join(pkg, "manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f)
    return pkg


def expect_reject(desc, mutate):
    global PASS, FAIL
    root = tempfile.mkdtemp(prefix="sam_guard_")
    try:
        pkg = make_package(root, mutate)
        try:
            C.load_manifest(pkg)
        except C.PackageError as exc:
            print(f"PASS [{desc}] 拒绝：{exc}")
            PASS += 1
            return
        print(f"FAIL [{desc}] 未被拒绝（load_manifest 正常返回）")
        FAIL += 1
    finally:
        shutil.rmtree(root, ignore_errors=True)


def expect_accept(desc):
    global PASS, FAIL
    root = tempfile.mkdtemp(prefix="sam_guard_")
    try:
        pkg = make_package(root)
        C.load_manifest(pkg)
        print(f"PASS [{desc}]")
        PASS += 1
    except C.PackageError as exc:
        print(f"FAIL [{desc}] 合法包被误拒：{exc}")
        FAIL += 1
    finally:
        shutil.rmtree(root, ignore_errors=True)


def main():
    # 正向对照
    expect_accept("正向对照：最小合法包")

    # P1-2 必需语义字段（复核方以内存删字段复现的三类 + 空对象）
    for field in C.REQUIRED_SEMANTIC_FIELDS:
        expect_reject(f"缺少 {field}", lambda m, _p, f=field: m.pop(f))
        expect_reject(f"{field} 为空对象", lambda m, _p, f=field: m.__setitem__(f, {}))
        expect_reject(f"{field} 非对象", lambda m, _p, f=field: m.__setitem__(f, "x"))
        expect_reject(f"{field} 任意非空对象", lambda m, _p, f=field: m.__setitem__(f, {"junk": 1}))
    expect_reject("预处理顺序改变", lambda m, _p: m["preprocessing"].__setitem__("order", "先填充后归一化"))
    expect_reject("坐标缩放公式改变", lambda m, _p: m["coordinateTransform"].__setitem__("factor", "round"))
    expect_reject("图内后处理不支持", lambda m, _p: m["semantics"]["postprocess"].__setitem__("mode", "in-graph"))

    # P1-2 包自包含：files.*.file 路径
    expect_reject("encoder.file 绝对路径",
                  lambda m, p: m["files"]["encoder"].__setitem__("file", os.path.join(p, "encoder.onnx")))
    expect_reject("decoder.file .. 穿越",
                  lambda m, _p: m["files"]["decoder"].__setitem__("file", "../decoder.onnx"))
    expect_reject("encoder.file 子目录",
                  lambda m, _p: m["files"]["encoder"].__setitem__("file", "sub/encoder.onnx"))
    expect_reject("encoder.file Windows 分隔符",
                  lambda m, _p: m["files"]["encoder"].__setitem__("file", "sub\\encoder.onnx"))
    expect_reject("decoder.file 空串",
                  lambda m, _p: m["files"]["decoder"].__setitem__("file", ""))
    expect_reject("encoder.file 缺失",
                  lambda m, _p: m["files"]["encoder"].pop("file"))

    # 既有契约（回归保护）
    expect_reject("formatVersion 不符", lambda m, _p: m.__setitem__("formatVersion", 2))
    expect_reject("formatVersion 布尔值", lambda m, _p: m.__setitem__("formatVersion", True))
    expect_reject("family=sam2 明确报错", lambda m, _p: m.__setitem__("family", "sam2"))
    expect_reject("variant=vit_l 明确报错", lambda m, _p: m.__setitem__("variant", "vit_l"))
    expect_reject("sha256 不符",
                  lambda m, _p: m["files"]["encoder"].__setitem__("sha256", "0" * 64))
    expect_reject("sizeBytes 不符",
                  lambda m, _p: m["files"]["decoder"].__setitem__("sizeBytes", 1))
    expect_reject("sizeBytes 缺失", lambda m, _p: m["files"]["decoder"].pop("sizeBytes"))
    expect_reject("encoder 符号链接指向包外", move_encoder_outside)
    expect_reject("decoder 文件不存在",
                  lambda m, _p: os.remove(os.path.join(_p, "decoder.onnx")))

    print("-----")
    print(f"manifest guards: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


def move_encoder_outside(_manifest, package_dir):
    source = os.path.join(package_dir, "encoder.onnx")
    outside = os.path.join(os.path.dirname(package_dir), "outside.onnx")
    os.replace(source, outside)
    os.symlink(outside, source)


if __name__ == "__main__":
    sys.exit(main())
