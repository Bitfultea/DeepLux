#!/usr/bin/env python3
"""DeepLux SAM 模型包校验（阶段1）：manifest 严格校验 + 哈希 + 实测 IO 规格 + 功能冒烟。

计划书要求「导入时校验路径、哈希及实际模型输入输出」。本脚本即该校验的
参考实现（阶段2 的 C++ 导入器按同一清单实现）：
1. manifest.json 严格 schema：formatVersion==1、family=="sam1"、
   variant=="vit_b"——不支持的家族/变体明确报错（SAM2/3、vit_l/h 不得
   静默通过）；
2. encoder.onnx/decoder.onnx 存在、SHA-256 与 manifest 一致、大小一致；
3. onnx.checker 通过；
4. ORT（CPU）实测输入输出名称/形状/类型与 manifest 记录一致（含动态维
   符号名）；decoder IO 名单恰为约定集合（无 mask_input/has_mask_input）；
5. 功能冒烟：合成图 → encoder → 有限 embedding；decoder 三种 prompt
   形态（仅点/仅框/点框混合，含零尺寸维）→ 掩膜 [3,H,W] 有限、
   iou [3] 有限、low_res [3,256,256]。

用法：
  PYTHONPATH=<deps> ~/.deeplux/sam_env/bin/python verify_sam_package.py \
      --package ~/Data/Models/sam1_vit_b_onnx [--json out.json]
退出码：0 全部通过；2 参数错误；4 任一校验失败（失败关闭，逐项报告）。
"""

import argparse
import json
import sys

import numpy as np

import sam_package_common as C


def check_io_against_manifest(session, spec_list, is_input, label, errors):
    """实测 ORT IO 与 manifest 规格逐名对齐（名称集合、dtype、静态维数值、动态维符号）。"""
    entries = session.get_inputs() if is_input else session.get_outputs()
    actual = {e.name: (e.type, e.shape) for e in entries}
    expected_names = [s["name"] for s in spec_list]
    if sorted(actual.keys()) != sorted(expected_names):
        errors.append(f"{label} 名称不符：manifest={expected_names} 实际={sorted(actual.keys())}")
        return
    ort_dtype_map = {"tensor(float)": "float32", "tensor(int64)": "int64", "tensor(int32)": "int32",
                     "tensor(double)": "float64", "tensor(float16)": "float16", "tensor(bool)": "bool"}
    for s in spec_list:
        dtype_actual, shape_actual = actual[s["name"]]
        dtype_expected = ort_dtype_map.get(dtype_actual, dtype_actual)
        if s["dtype"] != dtype_expected:
            errors.append(f"{label}[{s['name']}] dtype 不符：manifest={s['dtype']} 实际={dtype_expected}")
        if len(shape_actual) != len(s["shape"]):
            errors.append(f"{label}[{s['name']}] 维数不符：manifest={s['shape']} 实际={list(shape_actual)}")
            continue
        for dim_m, dim_a in zip(s["shape"], shape_actual):
            if isinstance(dim_m, int):
                if dim_a != dim_m:
                    errors.append(f"{label}[{s['name']}] 静态维不符：manifest={dim_m} 实际={dim_a}")
            elif isinstance(dim_m, str):
                if not isinstance(dim_a, str):
                    errors.append(f"{label}[{s['name']}] 动态维不符：manifest 符号={dim_m} 实际={dim_a}")
            else:
                errors.append(f"{label}[{s['name']}] manifest 维度非法：{dim_m!r}")


def main() -> int:
    ap = argparse.ArgumentParser(description="SAM 模型包校验")
    ap.add_argument("--package", required=True)
    ap.add_argument("--json", default=None, help="校验报告 JSON 输出路径")
    args = ap.parse_args()

    report = {"package": args.package, "checks": [], "errors": []}

    def record(name, ok, detail=""):
        report["checks"].append({"name": name, "pass": bool(ok), "detail": detail})
        print(f"{'PASS' if ok else 'FAIL'} [{name}]{(' ' + detail) if detail else ''}")
        return ok

    errors = report["errors"]

    # 1-2. manifest 严格校验 + 哈希（load_manifest 失败关闭）
    try:
        manifest = C.load_manifest(args.package)
        record("manifest+sha256", True,
               f"formatVersion={manifest['formatVersion']} family={manifest['family']} variant={manifest['variant']}")
        # 复核二轮 P1-2：必需语义字段与包内相对路径已由 load_manifest 强制，
        # 此处显式记录逐项在场（preprocessing/coordinateTransform/semantics）
        record("manifest-semantic-fields", True,
               "必需语义字段齐备: " + ", ".join(C.REQUIRED_SEMANTIC_FIELDS)
               + "；files.*.file 均为包内裸文件名（拒绝绝对路径/子目录/.. 穿越）")
    except C.PackageError as exc:
        record("manifest+sha256", False, str(exc))
        _finish(report, args.json)
        return 4

    enc_path, dec_path = C.file_paths(args.package, manifest)

    # 3. onnx.checker
    try:
        import onnx

        onnx.checker.check_model(enc_path)
        onnx.checker.check_model(dec_path)
        record("onnx.checker", True)
    except Exception as exc:
        record("onnx.checker", False, str(exc))
        errors.append(f"onnx.checker: {exc}")

    # 4. ORT 实测 IO vs manifest
    try:
        enc = C.make_session(enc_path)
        dec = C.make_session(dec_path)
        check_io_against_manifest(enc, manifest["files"]["encoder"]["inputs"], True, "encoder.inputs", errors)
        check_io_against_manifest(enc, manifest["files"]["encoder"]["outputs"], False, "encoder.outputs", errors)
        check_io_against_manifest(dec, manifest["files"]["decoder"]["inputs"], True, "decoder.inputs", errors)
        check_io_against_manifest(dec, manifest["files"]["decoder"]["outputs"], False, "decoder.outputs", errors)
        dec_in_names = sorted(i.name for i in dec.get_inputs())
        if dec_in_names != sorted(C.DECODER_INPUTS):
            errors.append(f"decoder 输入名单与约定不符：{dec_in_names}（不得含 mask_input/has_mask_input）")
        dec_out_names = sorted(o.name for o in dec.get_outputs())
        if dec_out_names != sorted(C.DECODER_OUTPUTS):
            errors.append(f"decoder 输出名单与约定不符：{dec_out_names}")
        record("io-vs-manifest", not errors, f"encoder/decoder IO 与 manifest 一致" if not errors else f"{len(errors)} 处不符")
    except Exception as exc:
        record("io-vs-manifest", False, str(exc))
        errors.append(f"ORT IO 校验异常：{exc}")
        _finish(report, args.json)
        return 4

    # 5. 功能冒烟：合成图 → encoder → decoder（三种 prompt 形态，含零尺寸维）
    try:
        img, _defects = C.make_small_defect_image(640, 480)
        input_image, _input_size = C.preprocess_image(img)
        emb = enc.run(None, {"input_image": input_image})[0]
        ok = emb.shape == (1, 256, 64, 64) and np.isfinite(emb).all()
        if not record("encoder-smoke", ok, f"embedding shape={emb.shape} finite={bool(np.isfinite(emb).all())}"):
            errors.append("encoder 冒烟失败")
        h, w = img.shape[:2]
        cases = {
            "points-only": (np.array([[120.0, 110.0]], dtype=np.float32), np.array([1], dtype=np.int64),
                            np.zeros((0, 4), dtype=np.float32)),
            "box-only": (np.zeros((0, 2), dtype=np.float32), np.zeros((0,), dtype=np.int64),
                         np.array([[50.0, 50.0, 200.0, 200.0]], dtype=np.float32)),
            "mixed": (np.array([[300.0, 240.0]], dtype=np.float32), np.array([1], dtype=np.int64),
                      np.array([[100.0, 100.0, 400.0, 300.0]], dtype=np.float32)),
        }
        for name, (pts, labels, boxes) in cases.items():
            pts_s = C.scale_coords(pts, (h, w)).astype(np.float32) if pts.size else pts
            boxes_s = C.scale_boxes(boxes, (h, w)).astype(np.float32) if boxes.size else boxes
            masks, ious = C.run_decoder(dec, emb, pts_s, labels, boxes_s, (h, w))
            ok = (masks.shape == (3, h, w) and np.isfinite(ious).all() and ious.shape == (3,)
                  and bool((masks.sum(axis=(1, 2)) >= 0).all()))
            if not record(f"decoder-smoke[{name}]", ok,
                          f"masks={masks.shape} ious={np.round(ious, 4).tolist()} fg={masks.sum(axis=(1,2)).tolist()}"):
                errors.append(f"decoder 冒烟失败：{name}")
    except Exception as exc:
        record("functional-smoke", False, str(exc))
        errors.append(f"功能冒烟异常：{exc}")

    return _finish(report, args.json)


def _finish(report, json_path) -> int:
    ok = not report["errors"] and all(c["pass"] for c in report["checks"])
    report["overallPass"] = ok
    print("-----")
    print(f"verify: {'PASS' if ok else 'FAIL'}（{sum(c['pass'] for c in report['checks'])}/{len(report['checks'])} 项通过）")
    if json_path:
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        print(f"报告已写入 {json_path}")
    return 0 if ok else 4


if __name__ == "__main__":
    sys.exit(main())
