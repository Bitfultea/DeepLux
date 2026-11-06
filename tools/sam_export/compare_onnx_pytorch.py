#!/usr/bin/env python3
"""DeepLux SAM 阶段1 精度门禁：ONNX CPU 模型包 vs PyTorch 原版权照。

计划书门禁（任一关键项不通过即停止原生迁移，不以近似算法代替）：
- 候选数及选中候选一致；
- bbox 偏差 ≤ 1 px（逐分量绝对差）；
- 典型掩膜 IoU ≥ 0.99；
- 分数绝对差 ≤ 0.01 —— 按阶段0 F1 口径：以**未 clamp 原始值**比较
  （官方 iou_prediction_head 无 sigmoid/clamp，score 可 >1，两侧同为
  原始值，直接相减）；
- 细小缺陷样本逐图（逐缺陷）复核。

对照口径：
- PyTorch 侧 = SamPredictor(multimask_output=True)，与 sam_server 现行
  产品语义完全一致（predict 返回 >0 二值掩膜、原始 scores）；
- ONNX 侧 = 同一图像经 manifest 口径预处理 → encoder.onnx → 坐标经
  ResizeLongestSide 同口径变换 → decoder.onnx → masks>0；
- 候选排序两侧均按 sam_server 语义 np.argsort(-scores)；
- 排序平手（相邻原始分差 < 分数容差）时允许秩交换，但须集合级匹配
  （IoU 配对 ≥0.99 且分数集合逐对 ≤ 容差），JSON 记 tieAdjusted。

编码器单独验证（官方仅提供解码器导出）：ORT embedding 与 PyTorch
predictor 内部 embedding 逐元素对比（max_abs/mean_abs/cosine）。

用法：
  PYTHONPATH=<deps> ~/.deeplux/sam_env/bin/python compare_onnx_pytorch.py \
      --package ~/Data/Models/sam1_vit_b_onnx \
      --checkpoint ~/Downloads/sam_vit_b_01ec64.pth \
      [--image path.png ...] [--yolo-labels-dir path/to/labels] \
      [--cases-per-image 12] [--json out.json]
内置确定性细小缺陷合成图始终参与（逐缺陷复核）；--image 可追加真实图。
--yolo-labels-dir 为每张 --image 读取同名 YOLO-seg 文件并生成逐缺陷点 prompt。
退出码：0 全部通过；2 参数错误；4 任一门禁不通过（失败关闭）。
"""

import argparse
import hashlib
import json
import os
import sys

import numpy as np

import sam_package_common as C

SCORE_TOL = 0.01
IOU_MIN = 0.99
BBOX_TOL = 1.0
ENC_MAX_ABS = 1e-2  # fp32 导出经验界；超过即导出保真度存疑（记录并判失败）


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_image_rgb(path: str) -> np.ndarray:
    import cv2

    img = cv2.imread(path, cv2.IMREAD_COLOR)
    if img is None:
        raise FileNotFoundError(path)
    return cv2.cvtColor(img, cv2.COLOR_BGR2RGB)


def load_yolo_defects(label_path: str, width: int, height: int) -> list:
    """Turn each normalized YOLO-seg polygon into a deterministic interior point."""
    import cv2

    try:
        with open(label_path, encoding="utf-8") as f:
            lines = f.read().splitlines()
    except (OSError, UnicodeError) as exc:
        raise ValueError(f"标注不可读：{label_path}: {exc}") from exc
    if not lines:
        raise ValueError(f"标注为空：{label_path}")

    defects = []
    for line_no, line in enumerate(lines, 1):
        parts = line.split()
        if len(parts) < 7 or (len(parts) - 1) % 2:
            raise ValueError(f"标注第 {line_no} 行不是 YOLO 分割多边形")
        try:
            class_id = int(parts[0])
            values = np.asarray([float(value) for value in parts[1:]], dtype=np.float64)
        except ValueError as exc:
            raise ValueError(f"标注第 {line_no} 行含非法数字") from exc
        if class_id < 0 or not np.isfinite(values).all() or (values < 0).any() or (values > 1).any():
            raise ValueError(f"标注第 {line_no} 行类别或归一化坐标非法")
        points = np.rint(values.reshape(-1, 2) * [width - 1, height - 1]).astype(np.int32)
        if cv2.contourArea(points.astype(np.float32)) <= 0:
            raise ValueError(f"标注第 {line_no} 行多边形退化")
        mask = np.zeros((height, width), dtype=np.uint8)
        cv2.fillPoly(mask, [points], 1)
        if not mask.any():
            raise ValueError(f"标注第 {line_no} 行没有有效像素")
        distance = cv2.distanceTransform(mask, cv2.DIST_L2, 5)
        cy, cx = np.unravel_index(int(distance.argmax()), distance.shape)
        defects.append({"center": [int(cx), int(cy)], "classId": class_id,
                        "labelLine": line_no, "areaPixels": int(mask.sum())})
    return defects


def recipe_prompts(n: int, w: int, h: int, neg_seq: int = 103):
    """阶段0 bench 同配方（i%10：0-3 正点 / 4-6 正负点 / 7-9 框选）。
    复核二轮 P2-3：负点偏移必须用阶段0 的**序列总长** neg_seq=103
    （runs=100+warmup=3，bench buildPromptRecipe 内 (i+runs) 的 runs 即
    103），而非本脚本的取用条数 n——否则 posneg 负点坐标与阶段0 序列
    不一致（初版 n=12 时 i=4 负点 (2132,2276)≠阶段0 (2489,2577)）。
    返回 [(kind, points_pos, points_neg, box_xywh)]。"""
    span_x, span_y = max(1, w - 200), max(1, h - 200)
    box_w, box_h = max(8, w // 8), max(8, h // 8)
    span_bx, span_by = max(1, w - box_w - 100), max(1, h - box_h - 100)
    out = []
    for i in range(n):
        kind = i % 10
        pos = [[100 + (i * 127) % span_x, 100 + (i * 311) % span_y]]
        if kind <= 3:
            out.append(("pos", pos, [], None))
        elif kind <= 6:
            neg = [[100 + ((i + neg_seq) * 127) % span_x, 100 + ((i + neg_seq) * 311) % span_y]]
            out.append(("posneg", pos, neg, None))
        else:
            out.append(("box", [], [], [50 + (i * 211) % span_bx, 50 + (i * 97) % span_by, box_w, box_h]))
    return out


def compare_candidate_lists(scores_pt, masks_pt, scores_ox, masks_ox):
    """按 sam_server 语义排序后逐秩对比；双侧平手带内才允许秩交换。"""
    order_pt = np.argsort(-np.asarray(scores_pt, dtype=np.float64))
    order_ox = np.argsort(-np.asarray(scores_ox, dtype=np.float64))
    result = {
        "countEqual": len(scores_pt) == len(scores_ox),
        "selectedEqual": bool(order_pt[0] == order_ox[0]) if len(order_pt) and len(order_ox) else False,
        "ranks": [],
        "tieAdjusted": False,
        "pass": True,
    }
    if not result["countEqual"] or not len(order_pt):
        result["pass"] = False
        return result
    for r in range(len(order_pt)):
        ip, io = int(order_pt[r]), int(order_ox[r])
        sp, so = float(scores_pt[ip]), float(scores_ox[io])
        iou = C.mask_iou(masks_pt[ip], masks_ox[io])
        bb_p, bb_o = C.mask_to_bbox(masks_pt[ip]), C.mask_to_bbox(masks_ox[io])
        bbox_delta = max(abs(a - b) for a, b in zip(bb_p, bb_o))
        tied_swap = (ip != io and abs(float(scores_pt[ip]) - float(scores_pt[io])) < SCORE_TOL
                     and abs(float(scores_ox[ip]) - float(scores_ox[io])) < SCORE_TOL)
        rank_pass = (abs(sp - so) <= SCORE_TOL and iou >= IOU_MIN and bbox_delta <= BBOX_TOL
                     and (ip == io or tied_swap))
        result["tieAdjusted"] |= tied_swap
        result["ranks"].append({
            "rank": r, "ptIndex": ip, "onnxIndex": io,
            "scorePt": sp, "scoreOnnx": so, "scoreDelta": abs(sp - so),
            "iou": iou, "bboxPt": bb_p, "bboxOnnx": bb_o, "bboxMaxDelta": bbox_delta,
            "scoreRawAbove1": bool(sp > 1.0 or so > 1.0),  # F1：原始值可>1，按原始值比较
            "pass": rank_pass,
        })
        if not rank_pass:
            result["pass"] = False
    return result


def run_case(predictor, dec_session, embedding_ort, orig_hw, kind, pos, neg, box_xywh):
    """单 prompt 双侧推理 + 门禁对比。坐标两侧使用同一变换口径。"""
    h, w = orig_hw
    point_coords = np.array(pos + neg, dtype=np.float32) if (pos or neg) else None
    point_labels = np.array([1] * len(pos) + [0] * len(neg), dtype=np.int64) if (pos or neg) else None
    box_xyxy = None
    if box_xywh is not None:
        x, y, bw, bh = box_xywh
        box_xyxy = np.array([x, y, x + bw, y + bh], dtype=np.float32)

    masks_pt, scores_pt, _ = predictor.predict(
        point_coords=point_coords, point_labels=point_labels, box=box_xyxy, multimask_output=True
    )
    masks_pt = masks_pt > 0

    # ONNX 侧坐标变换走 sam_package_common 参考实现（阶段2 C++ 的对照基准，
    # 复核二轮 P1-1 要求纳入门禁），并与安装版变换逐值对照（transformParity，
    # max|Δ|>1e-9 即判失败——参考实现错一个分量都会被门禁抓住）
    parity = 0.0
    if pos or neg:
        raw = np.array(pos + neg, dtype=np.float64)
        pts_ox = C.scale_coords(raw, (h, w))
        parity = max(parity, float(np.abs(pts_ox - predictor.transform.apply_coords(raw, (h, w))).max()))
    else:
        pts_ox = np.zeros((0, 2))
    labels_ox = point_labels if point_labels is not None else np.zeros((0,), dtype=np.int64)
    if box_xyxy is not None:
        raw_box = box_xyxy.reshape(1, 4).astype(np.float64)
        boxes_ox = C.scale_boxes(raw_box, (h, w))
        parity = max(parity, float(np.abs(boxes_ox - predictor.transform.apply_boxes(raw_box, (h, w))).max()))
    else:
        boxes_ox = np.zeros((0, 4))
    masks_ox, scores_ox = C.run_decoder(dec_session, embedding_ort, pts_ox.astype(np.float32), labels_ox,
                                        boxes_ox.astype(np.float32), (h, w),
                                        input_size=tuple(predictor.input_size))
    comp = compare_candidate_lists(scores_pt, masks_pt, scores_ox, masks_ox)
    comp["kind"] = kind
    comp["prompt"] = {"pos": pos, "neg": neg, "box": box_xywh}
    comp["transformParityMaxAbsDiff"] = parity
    if parity > 1e-9:
        comp["pass"] = False
    return comp


def main() -> int:
    ap = argparse.ArgumentParser(description="SAM 阶段1 精度门禁：ONNX vs PyTorch")
    ap.add_argument("--package", required=True)
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--image", action="append", default=[], help="追加参与对照的图像（可多次）")
    ap.add_argument("--yolo-labels-dir", help="为每个 --image 读取同名 YOLO-seg 标注并逐缺陷对照")
    ap.add_argument("--cases-per-image", type=int, default=12)
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    if args.yolo_labels_dir and not args.image:
        print("--yolo-labels-dir 需要至少一个 --image", file=sys.stderr)
        return 2

    try:
        manifest = C.load_manifest(args.package)
    except C.PackageError as exc:
        print(f"模型包校验失败：{exc}", file=sys.stderr)
        return 2
    if manifest.get("sourceCheckpoint", {}).get("sha256") and args.checkpoint:
        actual = sha256_file(args.checkpoint)
        if actual != manifest["sourceCheckpoint"]["sha256"]:
            print(f"警告：--checkpoint 与 manifest 记录的源权重 sha256 不一致\n  manifest={manifest['sourceCheckpoint']['sha256']}\n  actual  ={actual}",
                  file=sys.stderr)
            return 2
    enc_path, dec_path = C.file_paths(args.package, manifest)

    import torch
    from segment_anything import SamPredictor, sam_model_registry

    torch.set_grad_enabled(False)
    sam = sam_model_registry["vit_b"](checkpoint=args.checkpoint)
    sam.eval()
    predictor = SamPredictor(sam)
    enc_session = C.make_session(enc_path)
    dec_session = C.make_session(dec_path)

    report = {
        "gates": {"scoreTol": SCORE_TOL, "iouMin": IOU_MIN, "bboxTol": BBOX_TOL, "encoderMaxAbs": ENC_MAX_ABS},
        "package": {"dir": args.package,
                    "encoderSha256": manifest["files"]["encoder"]["sha256"],
                    "decoderSha256": manifest["files"]["decoder"]["sha256"]},
        "checkpoint": {"path": args.checkpoint, "sha256": manifest.get("sourceCheckpoint", {}).get("sha256")},
        "environment": {"torch": torch.__version__, "python": sys.version.split()[0]},
        "images": [],
        "overallPass": True,
    }
    try:
        import onnxruntime as ort

        report["environment"]["onnxruntime"] = ort.__version__
    except ImportError:
        pass

    # 图像集合：内置细小缺陷合成图（逐缺陷复核）+ 追加图像
    images = []
    defect_img, defects = C.make_small_defect_image()
    images.append(("<builtin-small-defects-1280x720>", defect_img,
                   [{"center": [cx, cy], "radius": r} for cx, cy, r in defects], None))
    for path in args.image:
        try:
            img = load_image_rgb(path)
        except FileNotFoundError:
            print(f"图像不可读：{path}", file=sys.stderr)
            return 2
        label_path = None
        image_defects = None
        if args.yolo_labels_dir:
            label_path = os.path.join(args.yolo_labels_dir, os.path.splitext(os.path.basename(path))[0] + ".txt")
            try:
                image_defects = load_yolo_defects(label_path, img.shape[1], img.shape[0])
            except ValueError as exc:
                print(str(exc), file=sys.stderr)
                return 2
        images.append((path, img, image_defects, label_path))

    for name, img_rgb, defects, label_path in images:
        h, w = img_rgb.shape[:2]
        entry = {"image": name, "size": [w, h], "sha256": None, "encoderCheck": {}, "cases": [], "pass": True}
        if name != "<builtin-small-defects-1280x720>":
            entry["sha256"] = sha256_file(name)
        if label_path:
            entry["labelPath"] = label_path
            entry["labelSha256"] = sha256_file(label_path)
        print(f"== 图像 {name} ({w}x{h}) ==")

        # PyTorch embedding（set_image 内部完成安装版预处理）
        predictor.set_image(img_rgb)
        emb_pt = predictor.features  # [1,256,64,64]
        # ONNX embedding：预处理直接复用安装版管线（ResizeLongestSide.apply_image
        # + sam.preprocess），与 set_image 内部逐位同构——门禁只度量导出图本身
        resized = predictor.transform.apply_image(img_rgb)
        t = torch.as_tensor(resized).permute(2, 0, 1).contiguous()[None, ...].float()
        input_image = sam.preprocess(t).numpy()
        emb_ox = enc_session.run(None, {"input_image": input_image})[0]

        diff = np.abs(emb_pt.numpy().astype(np.float64) - emb_ox.astype(np.float64))
        cos = float(np.dot(emb_pt.numpy().ravel(), emb_ox.ravel())
                    / (np.linalg.norm(emb_pt.numpy()) * np.linalg.norm(emb_ox) + 1e-30))
        entry["encoderCheck"] = {
            "maxAbsDiff": float(diff.max()), "meanAbsDiff": float(diff.mean()), "cosine": cos,
            "pass": bool(diff.max() <= ENC_MAX_ABS and cos >= 0.999999),
        }
        ec = entry["encoderCheck"]
        print(f"  encoder: max_abs={ec['maxAbsDiff']:.3e} mean_abs={ec['meanAbsDiff']:.3e} cos={cos:.9f} "
              f"{'PASS' if ec['pass'] else 'FAIL'}")
        if not ec["pass"]:
            entry["pass"] = False

        # 常规混合 prompt（阶段0 同配方）
        prompts = recipe_prompts(args.cases_per_image, w, h)
        for kind, pos, neg, box in prompts:
            comp = run_case(predictor, dec_session, emb_ox, (h, w), kind, pos, neg, box)
            entry["cases"].append(comp)
            if not comp["pass"]:
                entry["pass"] = False
            r0 = comp["ranks"][0] if comp["ranks"] else {}
            print(f"  case {kind} {'PASS' if comp['pass'] else 'FAIL'} "
                  f"score Δ={r0.get('scoreDelta', float('nan')):.5f} IoU={r0.get('iou', float('nan')):.5f} "
                  f"bboxΔ={r0.get('bboxMaxDelta', float('nan')):.2f}"
                  f"{' tieAdjusted' if comp.get('tieAdjusted') else ''}")

        # 细小缺陷逐缺陷复核（点 prompt 置于缺陷中心）
        if defects:
            entry["defectReview"] = []
            for k, defect in enumerate(defects):
                cx, cy = defect["center"]
                comp = run_case(predictor, dec_session, emb_ox, (h, w), "defect-point", [[cx, cy]], [], None)
                row = {"defect": k, **defect,
                       **{key: comp[key] for key in ("prompt", "pass", "ranks", "countEqual",
                                                      "selectedEqual", "tieAdjusted", "transformParityMaxAbsDiff")}}
                entry["defectReview"].append(row)
                if not comp["pass"]:
                    entry["pass"] = False
                r0 = comp["ranks"][0] if comp["ranks"] else {}
                print(f"  defect[{k}] c=({cx},{cy}) {'PASS' if comp['pass'] else 'FAIL'} "
                      f"score Δ={r0.get('scoreDelta', float('nan')):.5f} IoU={r0.get('iou', float('nan')):.5f} "
                      f"bboxΔ={r0.get('bboxMaxDelta', float('nan')):.2f}")
        if not entry["pass"]:
            report["overallPass"] = False
        report["images"].append(entry)

    print("-----")
    print(f"compare: {'PASS' if report['overallPass'] else 'FAIL'}")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
        print(f"报告已写入 {args.json}")
    return 0 if report["overallPass"] else 4


if __name__ == "__main__":
    sys.exit(main())
