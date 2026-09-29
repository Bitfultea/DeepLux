#!/usr/bin/env python3
"""DeepLux SAM 模型包共享工具：manifest 严格加载、预处理、ONNX 推理封装。

verify_sam_package.py 与 compare_onnx_pytorch.py 共用；语义以包内
manifest.json 为准（导入方校验），常量与官方 segment_anything 对齐。
"""

import hashlib
import json
import os

import numpy as np

FORMAT_VERSION = 1
FAMILY = "sam1"
SUPPORTED_VARIANTS = ("vit_b",)

DECODER_INPUTS = ["image_embeddings", "point_coords", "point_labels", "boxes"]
DECODER_OUTPUTS = ["low_res_masks", "iou_predictions"]


class PackageError(RuntimeError):
    pass


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


REQUIRED_SEMANTIC_FIELDS = ("preprocessing", "coordinateTransform", "semantics")


def load_manifest(package_dir: str) -> dict:
    """严格加载并校验 manifest——失败关闭。校验面（复核二轮 P1-2 补全）：
    1) 格式版本/family/variant（不支持项明确报错）；
    2) 必需语义字段：preprocessing/coordinateTransform/semantics 必须为
       非空对象——调用方（阶段2 C++ 导入器）依赖这些契约执行预处理/坐标
       变换/候选选择，缺失即不可安全使用；
    3) files.encoder/decoder：file 必须为**包内相对文件名**（拒绝绝对路径、
       路径分隔符与 .. 穿越——保证包自包含），文件存在、SHA-256 与大小
       逐字节核对。"""
    man_path = os.path.join(package_dir, "manifest.json")
    if not os.path.isfile(man_path):
        raise PackageError(f"模型包缺少 manifest.json：{package_dir}")
    with open(man_path, encoding="utf-8") as f:
        try:
            manifest = json.load(f)
        except json.JSONDecodeError as exc:
            raise PackageError(f"manifest.json 非法 JSON：{exc}") from exc
    if not isinstance(manifest, dict):
        raise PackageError("manifest.json 必须为 JSON 对象")
    fv = manifest.get("formatVersion")
    if fv != FORMAT_VERSION:
        raise PackageError(f"不支持的模型包格式版本：{fv!r}（本工具支持 {FORMAT_VERSION}）")
    family = manifest.get("family")
    if family != FAMILY:
        raise PackageError(f"不支持的模型家族：{family!r}（本工具仅支持 {FAMILY!r}；SAM2/3 须经新增模型专属实现接入）")
    variant = manifest.get("variant")
    if variant not in SUPPORTED_VARIANTS:
        raise PackageError(f"不支持的模型变体：{variant!r}（首版仅验收 {SUPPORTED_VARIANTS}；vit_l/vit_h 请继续使用显式远程模式）")
    # 必需语义字段（缺失/非对象/空对象均拒绝）
    for field in REQUIRED_SEMANTIC_FIELDS:
        value = manifest.get(field)
        if not isinstance(value, dict) or not value:
            raise PackageError(f"manifest 缺少必需语义字段 {field!r}（必须为非空对象；调用方依赖该契约）")
    files = manifest.get("files")
    if not isinstance(files, dict) or "encoder" not in files or "decoder" not in files:
        raise PackageError("manifest.files 必须包含 encoder 与 decoder")
    for name in ("encoder", "decoder"):
        entry = files[name]
        if not isinstance(entry, dict):
            raise PackageError(f"manifest.files.{name} 必须为对象")
        fname = entry.get("file")
        # 包自包含：仅允许包目录内的裸文件名（拒绝绝对路径/子目录/.. 穿越）
        if not isinstance(fname, str) or not fname or fname != os.path.basename(fname) or os.path.isabs(fname):
            raise PackageError(f"manifest.files.{name}.file 必须为包内相对文件名（拒绝绝对路径与穿越）：{fname!r}")
        path = os.path.join(package_dir, fname)
        if not os.path.isfile(path):
            raise PackageError(f"模型包缺少 {name} 文件：{fname!r}")
        actual = sha256_file(path)
        if actual != entry.get("sha256"):
            raise PackageError(f"{name} SHA-256 不符：manifest={entry.get('sha256')} 实际={actual}")
        size = os.path.getsize(path)
        if entry.get("sizeBytes") is not None and size != entry["sizeBytes"]:
            raise PackageError(f"{name} 大小不符：manifest={entry['sizeBytes']} 实际={size}")
    return manifest


def file_paths(package_dir: str, manifest: dict):
    return (
        os.path.join(package_dir, manifest["files"]["encoder"]["file"]),
        os.path.join(package_dir, manifest["files"]["decoder"]["file"]),
    )


def make_session(onnx_path: str):
    import onnxruntime as ort

    so = ort.SessionOptions()
    so.log_severity_level = 3
    return ort.InferenceSession(onnx_path, so, providers=["CPUExecutionProvider"])


def preprocess_image(img_rgb: np.ndarray, target: int = 1024) -> tuple:
    """安装版 Sam 预处理精确复刻（compare 门禁直接调安装版变换，此函数供
    verify 冒烟与阶段2 C++ 实现参照）：
    1) ResizeLongestSide.apply_image：torchvision resize(to_pil_image(img))
       ——PIL 双线性（**非 cv2.INTER_LINEAR**，重采样器差异阶段2 须对照门禁）；
    2) Sam.preprocess 顺序：**先 (x-mean)/std 归一化（0-255 尺度），后右下
       零填充至 target²**（填充值为归一化空间的 0）。
    返回 (input_image [1,3,T,T] float32, input_size (new_h,new_w))。"""
    import torch
    from torchvision.transforms.functional import resize as tv_resize, to_pil_image

    h, w = img_rgb.shape[:2]
    scale = target / max(h, w)
    new_h, new_w = int(h * scale + 0.5), int(w * scale + 0.5)  # get_preprocess_shape 口径
    resized = np.array(tv_resize(to_pil_image(img_rgb), (new_h, new_w)))
    t = torch.as_tensor(resized).permute(2, 0, 1).contiguous()[None, ...].float()
    mean = torch.tensor(PIXEL_MEAN, dtype=torch.float32).reshape(1, 3, 1, 1)
    std = torch.tensor(PIXEL_STD, dtype=torch.float32).reshape(1, 3, 1, 1)
    t = (t - mean) / std  # 先归一化
    pad_h, pad_w = target - new_h, target - new_w
    t = torch.nn.functional.pad(t, (0, pad_w, 0, pad_h))  # 后零填充（归一化空间 0）
    return t.numpy(), (new_h, new_w)


PIXEL_MEAN = [123.675, 116.28, 103.53]
PIXEL_STD = [58.395, 57.12, 57.375]


def get_preprocess_shape(orig_hw: tuple, target: int = 1024) -> tuple:
    """安装版 ResizeLongestSide.get_preprocess_shape 精确复刻：
    scale = target / max(h,w)；new = int(old*scale + 0.5)——**不得再 round()**
    （复核二轮 P1-1：int(round(x+0.5)) 与 int(x+0.5) 在小数部分 ≥0.5 时差 1，
    如 395→404 vs 405）。"""
    h, w = orig_hw
    scale = target * 1.0 / max(h, w)
    return (int(h * scale + 0.5), int(w * scale + 0.5))


def scale_coords(coords: np.ndarray, orig_hw: tuple, target: int = 1024) -> np.ndarray:
    """安装版 ResizeLongestSide.apply_coords 精确复刻（纯缩放，无偏移）。
    coords [...,2] (x,y)。逐值对照安装版变换由 compare 门禁强制（transformParity）。"""
    h, w = orig_hw
    new_h, new_w = get_preprocess_shape(orig_hw, target)
    out = np.array(coords, dtype=np.float64).copy()
    out[..., 0] = out[..., 0] * (new_w / w)
    out[..., 1] = out[..., 1] * (new_h / h)
    return out


def scale_boxes(boxes: np.ndarray, orig_hw: tuple, target: int = 1024) -> np.ndarray:
    """安装版 ResizeLongestSide.apply_boxes 精确复刻：reshape(-1,2,2) 后对
    **全部四个分量**逐点 apply_coords（复核二轮 P1-1：初版直接对 [x1,y1,x2,y2]
    调 scale_coords 只缩放了前两个值——640×480 框 [50,50,200,200] 安装版得
    [80,80,320,320]，初版误得 [80,80,200,200]）。"""
    b = np.array(boxes, dtype=np.float64).reshape(-1, 2, 2)
    return scale_coords(b, orig_hw, target).reshape(-1, 4)


def compute_input_size(orig_hw: tuple, target: int = 1024) -> tuple:
    """ResizeLongestSide.get_preprocess_shape 口径：(new_h, new_w)。"""
    h, w = orig_hw
    scale = target / max(h, w)
    return (int(h * scale + 0.5), int(w * scale + 0.5))


def postprocess_masks_caller(low_res_masks: np.ndarray, orig_hw: tuple, input_size: tuple,
                             img_size: int = 1024) -> np.ndarray:
    """安装版 Sam.postprocess_masks 精确复刻（manifest semantics.postprocess=
    caller 模式）：256→1024² bilinear → **去填充裁剪 [..., :input_h, :input_w]**
    → 原图 bilinear → >0 阈值（三步缺一不可；方形图裁剪为 no-op，非方形图
    缺裁剪会导致掩膜拉伸错位）。使用 torch F.interpolate 与 PyTorch 侧
    重采样器逐位对齐（精度门禁口径）。"""
    import torch
    import torch.nn.functional as F

    t = torch.from_numpy(np.asarray(low_res_masks, dtype=np.float32))
    t = F.interpolate(t, size=(img_size, img_size), mode="bilinear", align_corners=False)
    t = t[..., : input_size[0], : input_size[1]]
    h, w = orig_hw
    t = F.interpolate(t, size=(h, w), mode="bilinear", align_corners=False)
    return t.numpy() > 0.0


def run_decoder(session, embedding: np.ndarray, points_xy: np.ndarray, labels: np.ndarray,
                boxes_xyxy: np.ndarray, orig_hw: tuple, input_size: tuple = None):
    """decoder.onnx 推理 + caller 侧 postprocess。points_xy [N,2] 1024 尺度（可空
    [0,2]）；labels [N] int64；boxes_xyxy [M,4] 1024 尺度 xyxy（可空 [0,4]）；
    input_size 缺省按 orig_hw 推导（与 ResizeLongestSide 一致）。
    返回 (masks_bool [3,H,W], ious_raw [3])。"""
    feeds = {
        "image_embeddings": embedding.astype(np.float32),
        "point_coords": np.asarray(points_xy, dtype=np.float32).reshape(-1, 2),
        "point_labels": np.asarray(labels, dtype=np.int64).reshape(-1),
        "boxes": np.asarray(boxes_xyxy, dtype=np.float32).reshape(-1, 4),
    }
    low_res, ious = session.run(None, feeds)
    if input_size is None:
        input_size = compute_input_size(orig_hw)
    masks = postprocess_masks_caller(low_res, orig_hw, input_size)  # [1,3,H,W] bool
    return masks[0], ious[0].astype(np.float64)


def mask_to_bbox(mask: np.ndarray):
    """sam_server._mask_to_bbox 同口径：[x1, y1, w, h]（w=x2-x1+1）。空掩膜 → [0,0,0,0]。"""
    ys, xs = np.where(mask > 0)
    if len(xs) == 0 or len(ys) == 0:
        return [0.0, 0.0, 0.0, 0.0]
    x1, x2 = float(xs.min()), float(xs.max())
    y1, y2 = float(ys.min()), float(ys.max())
    return [x1, y1, x2 - x1 + 1.0, y2 - y1 + 1.0]


def mask_iou(a: np.ndarray, b: np.ndarray) -> float:
    a = a > 0
    b = b > 0
    inter = np.logical_and(a, b).sum()
    union = np.logical_or(a, b).sum()
    if union == 0:
        return 1.0 if inter == 0 else 0.0
    return float(inter) / float(union)


def make_small_defect_image(width: int = 1280, height: int = 720) -> tuple:
    """确定性合成细小缺陷图（阶段1 逐图复核用；真实缺陷图集到位前替代）。
    平滑渐变底 + 6 个半径 3-10px 的深/浅色斑（整数运算，跨机一致）。
    返回 (img_rgb uint8, defects[(cx,cy,radius)])。"""
    yy, xx = np.mgrid[0:height, 0:width]
    base = ((xx * 3 + yy * 5) // 16 % 48 + 96).astype(np.float32)
    img = np.repeat(base[..., None], 3, axis=2)
    defects = []
    for k in range(6):
        cx = 120 + k * 190
        cy = 110 + (k * 137) % (height - 220)
        r = 3 + (k * 7) % 8  # 3..10 px
        dark = k % 2 == 0
        d2 = (xx - cx) ** 2 + (yy - cy) ** 2
        region = d2 <= r * r
        img[region] = 30.0 if dark else 225.0
        defects.append((int(cx), int(cy), int(r)))
    return np.clip(img, 0, 255).astype(np.uint8), defects
