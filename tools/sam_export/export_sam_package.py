#!/usr/bin/env python3
"""DeepLux SAM 原生推理计划 · 阶段1：SAM1 ViT-B 离线 ONNX 导出 → 模型包。

模型包 = manifest.json + encoder.onnx + decoder.onnx（计划书「接口与兼容
决策」）：
- encoder.onnx：image_encoder（ViT-B）。输入 input_image [1,3,1024,1024]
  float32（RGB，长边 resize 1024 + 零填充 + (x-pixel_mean)/pixel_std，
  0-255 尺度，归一化由调用方完成）；输出 image_embeddings [1,256,64,64]。
  SAM 官方仅提供解码器导出脚本，编码器导出由本工具完成并经
  compare_onnx_pytorch.py 的 encoderCheck 单独数值验证（阶段1 证据）。
- decoder.onnx：prompt_encoder + mask_decoder(multimask_output=True) +
  postprocess（与官方 segment-anything 仓库 scripts/export_onnx_model.py
  同语义；区别：输出全部 3 个掩膜通道与未 clamp 的原始 IoU 分数，
  候选排序/选择留给下游——支撑批4 candidates 协议与 sam_server 现行
  语义）。坐标/框输入为模型输入空间（1024 尺度，调用方经
  ResizeLongestSide.apply_coords/apply_boxes 变换）。
  mask_input/has_mask_input 不导出（当前工作流不使用掩膜 prompt；
  manifest 记录该限制，导入尚不支持的模型须明确报错——计划书边界）。
- manifest.json：格式版本、family=sam1、variant=vit_b、两个文件与源
  权重的 SHA-256、实测输入输出规格（从导出图读取，非手填）、预处理与
  坐标变换规格、选择语义、导出环境版本。

用法（开发机，环境不侵入 sam_env）：
  PYTHONPATH=<sam_export_deps> ~/.deeplux/sam_env/bin/python \
      export_sam_package.py --checkpoint ~/Downloads/sam_vit_b_01ec64.pth \
      --model-type vit_b --out ~/Data/Models/sam1_vit_b_onnx --opset 17

退出码：0 成功；2 参数/前置检查失败；3 导出失败；4 写出/清单失败。
失败关闭：任何一步失败不留下半成品包（先写临时目录，全部成功后原子
落位）。
"""

import argparse
import datetime
import hashlib
import json
import os
import shutil
import sys
import tempfile

import torch

import sam_package_common as C

FORMAT_VERSION = 1
FAMILY = "sam1"

# Sam 官方预处理常量（segment_anything.modeling.Sam 默认值，导出时从模型读取核对）
PIXEL_MEAN_DEFAULT = [123.675, 116.28, 103.53]
PIXEL_STD_DEFAULT = [58.395, 57.12, 57.375]


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def onnx_io_spec(model_path: str):
    """读取实际输入输出规格（名称/形状/类型），供 manifest 记录与导入校验。
    规格以 ORT 会话实测为准（阶段2 C++ 导入器同口径校验；onnx 自带
    shape_inference 对 ViT 窗口注意力输出可能残留符号维，ORT 推导更完整）；
    opset 与 onnx.checker 仍取 onnx 模型本体。"""
    import onnx
    import onnxruntime as ort

    m = onnx.load(model_path)
    onnx.checker.check_model(model_path)
    opset = m.opset_import[0].version

    so = ort.SessionOptions()
    so.log_severity_level = 3
    sess = ort.InferenceSession(model_path, so, providers=["CPUExecutionProvider"])
    dtype_map = {"tensor(float)": "float32", "tensor(double)": "float64", "tensor(float16)": "float16",
                 "tensor(int64)": "int64", "tensor(int32)": "int32", "tensor(bool)": "bool"}

    def spec(e):
        return {"name": e.name, "dtype": dtype_map.get(e.type, e.type), "shape": list(e.shape)}

    return ([spec(i) for i in sess.get_inputs()], [spec(o) for o in sess.get_outputs()]), opset


class EncoderWrapper(torch.nn.Module):
    def __init__(self, image_encoder):
        super().__init__()
        self.image_encoder = image_encoder

    def forward(self, input_image):
        return self.image_encoder(input_image)


class DecoderWrapper(torch.nn.Module):
    """prompt_encoder 等价重构 + mask_decoder(multimask_output=True)，不含 postprocess。

    与官方 export_onnx_model.py 的 SamOnnxExportableModel 语义对齐，三点差异
    （manifest.semantics 记录，阶段2 C++ 按规格执行）：
    1) 输出全部 3 个掩膜通道（logits）与未 clamp 原始 IoU 分数，不做单掩膜
       选择——下游按 sam_server 语义 argsort(-scores) 取候选（批4 协议）；
    2) 官方 wrapper 把 postprocess（256→1024→原图两级双线性 + 阈值）放进
       图内，其动态 size 插值在 torch 2.13 legacy tracer 下不可导出
       （upsample_bilinear2d 拒绝张量 size）；本包输出 low_res_masks
       [1,3,256,256]，上采样由调用方按 Sam.postprocess_masks 同口径执行；
    3) 不直接调 prompt_encoder.forward——其 None 分支不可追踪且零尺寸张量
       在 ORT 触发常量化 Reshape 失败；改为按**安装版** PromptEncoder 语义
       逐行等价重构（pad=(boxes is None) 时点集追加 not_a_point pad token；
       boxes 单框 [1,4] 约束与 predictor 一致）：points/boxes 各追加哑元行
       消除零尺寸张量，再以 Shape→Range→index_select 动态裁剪，三种 prompt
       形态（仅点/仅框/混合）的 token 集与安装版 predictor 完全一致。
    """

    def __init__(self, model):
        super().__init__()
        self.model = model

    # ---- 安装版 PromptEncoder 的等价重构（动态形状安全）----
    # _embed_points/_embed_boxes/forward_with_coords 内的掩码索引赋值与
    # 原位切片赋值在 legacy tracer 下被常量化（Where/Reshape 广播维固化），
    # 运行时 N/M 变化即崩；以下逐行等价改写为纯广播算术 + 动态安全算子，
    # 数值等价由阶段1 精度门禁（vs SamPredictor）逐项验证。

    def _pe_fwd_coords(self, coords):
        """PositionEmbeddingRandom.forward_with_coords 等价（去原位切片赋值）。"""
        pe = self.model.prompt_encoder.pe_layer
        h, w = self.model.prompt_encoder.input_image_size
        cx = coords[..., 0:1] / float(w)
        cy = coords[..., 1:2] / float(h)
        return pe._pe_encoding(torch.cat([cx, cy], dim=-1))

    def _embed_points_equiv(self, points, labels):
        """PromptEncoder._embed_points(pad=False) 等价（掩码赋值 → 广播加权）。
        points [1,N,2]（未 +0.5），labels [1,N]。"""
        pe = self.model.prompt_encoder
        pos = self._pe_fwd_coords(points + 0.5)
        m_neg1 = (labels == -1).to(pos.dtype).unsqueeze(-1)
        m0 = (labels == 0).to(pos.dtype).unsqueeze(-1)
        m1 = (labels == 1).to(pos.dtype).unsqueeze(-1)
        return (pos * (1.0 - m_neg1) + m_neg1 * pe.not_a_point_embed.weight
                + m0 * pe.point_embeddings[0].weight + m1 * pe.point_embeddings[1].weight)

    def _embed_boxes_equiv(self, boxes):
        """PromptEncoder._embed_boxes 等价。boxes [1,K,4] → [1,2K,C]。"""
        pe = self.model.prompt_encoder
        coords = (boxes + 0.5).reshape(-1, 2, 2)  # [K,2,2]
        corner = self._pe_fwd_coords(coords)  # [K,2,C]
        c0, c1 = corner.unbind(dim=1)  # 动态 K 安全（Split）
        stacked = torch.stack([c0 + pe.point_embeddings[2].weight, c1 + pe.point_embeddings[3].weight], dim=1)
        return stacked.reshape(1, -1, pe.embed_dim)  # [1,2K,C]

    @torch.no_grad()
    def forward(self, image_embeddings, point_coords, point_labels, boxes):
        from torch.onnx.operators import shape_as_tensor

        pe = self.model.prompt_encoder
        dev = point_coords.device

        # 点：追加哑元行（label -1 → not_a_point token，与安装版 pad token 同值），
        # 规避零尺寸张量；有无框时是否保留由下方动态索引决定
        pts = torch.cat(
            [point_coords.unsqueeze(0), torch.zeros((1, 1, 2), dtype=point_coords.dtype, device=dev)], dim=1
        )
        labs = torch.cat(
            [point_labels.unsqueeze(0), -torch.ones((1, 1), dtype=point_labels.dtype, device=dev)], dim=1
        )
        pts_emb = self._embed_points_equiv(pts, labs)  # [1, N+1, C]，末行=pad token

        # 框：追加哑元框消除零尺寸；[1, 2(M+1), C]，末 2 行=哑元
        boxes_p = torch.cat([boxes, torch.zeros((1, 4), dtype=boxes.dtype, device=dev)], dim=0).unsqueeze(0)
        box_emb = self._embed_boxes_equiv(boxes_p)

        tokens = torch.cat([pts_emb, box_emb], dim=1)  # [1, N+1+2(M+1), C]

        # 安装版语义：pad=(boxes is None) —— 无框时保留 pad token，有框时剔除；
        # 框 token 取前 2M 行（哑元剔除）。索引全部由运行时形状导出（Shape→Range）。
        n_pts = shape_as_tensor(point_coords)[0]
        n_box = shape_as_tensor(boxes)[0]
        pad_count = 1 - torch.clamp(n_box, max=1)
        idx_pts = torch.arange(n_pts + pad_count, device=dev, dtype=torch.int64)
        idx_box = torch.arange(n_pts + 1, n_pts + 1 + 2 * n_box, device=dev, dtype=torch.int64)
        indices = torch.cat([idx_pts, idx_box])
        sparse_embeddings = tokens.index_select(1, indices)  # [1, L, C]

        emb_hw = pe.image_embedding_size
        dense_embeddings = pe.no_mask_embed.weight.reshape(1, -1, 1, 1).expand(1, -1, emb_hw[0], emb_hw[1])

        low_res_masks, iou_predictions = self.model.mask_decoder(
            image_embeddings=image_embeddings,
            image_pe=pe.get_dense_pe(),
            sparse_prompt_embeddings=sparse_embeddings,
            dense_prompt_embeddings=dense_embeddings,
            multimask_output=True,
        )
        return low_res_masks, iou_predictions


def export_encoder(sam, out_path: str, opset: int) -> None:
    wrapper = EncoderWrapper(sam.image_encoder).eval()
    dummy = torch.randn(1, 3, 1024, 1024, dtype=torch.float32)
    torch.onnx.export(
        wrapper,
        (dummy,),
        out_path,
        export_params=True,
        do_constant_folding=True,
        opset_version=opset,
        input_names=["input_image"],
        output_names=["image_embeddings"],
        dynamo=False,
    )


def export_decoder(sam, out_path: str, opset: int) -> None:
    wrapper = DecoderWrapper(sam).eval()
    dummy = {
        "image_embeddings": torch.randn(1, 256, 64, 64, dtype=torch.float32),
        # 追踪用非空哑元（points+boxes 同现，两分支均入图）；运行时支持零尺寸
        "point_coords": torch.tensor([[512.0, 512.0], [256.0, 256.0]], dtype=torch.float32),
        "point_labels": torch.tensor([1, 0], dtype=torch.int64),
        "boxes": torch.tensor([[100.0, 100.0, 400.0, 400.0]], dtype=torch.float32),
    }
    _ = wrapper(**dummy)  # 追踪前自检
    torch.onnx.export(
        wrapper,
        tuple(dummy.values()),
        out_path,
        export_params=True,
        do_constant_folding=True,
        opset_version=opset,
        input_names=list(dummy.keys()),
        output_names=["low_res_masks", "iou_predictions"],
        dynamic_axes={
            "point_coords": {0: "num_points"},
            "point_labels": {0: "num_points"},
            "boxes": {0: "num_boxes"},
        },
        dynamo=False,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="SAM1 ViT-B → DeepLux ONNX 模型包导出")
    ap.add_argument("--checkpoint", required=True, help=".pth 权重路径")
    ap.add_argument("--model-type", default="vit_b", choices=["vit_b"], help="首版仅验收 vit_b（计划书）")
    ap.add_argument("--out", required=True, help="模型包输出目录")
    ap.add_argument("--opset", type=int, default=17)
    args = ap.parse_args()

    if args.model_type != "vit_b":
        print("仅支持 vit_b（首版验收范围）；vit_l/vit_h 请继续使用显式远程模式", file=sys.stderr)
        return 2
    if not os.path.isfile(args.checkpoint):
        print(f"权重不存在：{args.checkpoint}", file=sys.stderr)
        return 2
    if args.opset < 12 or args.opset > 22:
        print("opset 必须在 [12,22]", file=sys.stderr)
        return 2

    try:
        import onnxruntime  # noqa: F401  导出后即时自检需要
    except ImportError:
        print("缺少 onnxruntime（导出后自检需要）；请设置 PYTHONPATH 指向 sam_export_deps", file=sys.stderr)
        return 2

    from segment_anything import sam_model_registry

    tmpdir = tempfile.mkdtemp(prefix="sam_pkg_", dir=os.path.dirname(os.path.abspath(args.out)) or ".")
    try:
        print(f"[1/5] 加载权重 {args.checkpoint} (sha256={sha256_file(args.checkpoint)[:16]}…)")
        sam = sam_model_registry[args.model_type](checkpoint=args.checkpoint)
        sam.eval()

        # 预处理常量核对（导出图不含归一化，调用方按 manifest 执行）
        pixel_mean = sam.pixel_mean.reshape(-1).tolist()
        pixel_std = sam.pixel_std.reshape(-1).tolist()
        if [round(v, 3) for v in pixel_mean] != PIXEL_MEAN_DEFAULT or [round(v, 3) for v in pixel_std] != PIXEL_STD_DEFAULT:
            print(f"警告：pixel_mean/std 与官方默认不一致 mean={pixel_mean} std={pixel_std}", file=sys.stderr)

        enc_path = os.path.join(tmpdir, "encoder.onnx")
        dec_path = os.path.join(tmpdir, "decoder.onnx")
        print(f"[2/5] 导出 encoder（opset {args.opset}，ViT-B CPU，约 1-3 分钟）…")
        export_encoder(sam, enc_path, args.opset)
        print(f"[3/5] 导出 decoder…")
        export_decoder(sam, dec_path, args.opset)

        print("[4/5] 读取实际 IO 规格…")
        (enc_in, enc_out), enc_opset = onnx_io_spec(enc_path)
        (dec_in, dec_out), dec_opset = onnx_io_spec(dec_path)

        manifest = {
            "formatVersion": FORMAT_VERSION,
            "family": FAMILY,
            "variant": args.model_type,
            "createdAt": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "sourceCheckpoint": {
                "fileName": os.path.basename(args.checkpoint),
                "sha256": sha256_file(args.checkpoint),
                "sizeBytes": os.path.getsize(args.checkpoint),
            },
            "files": {
                "encoder": {
                    "file": "encoder.onnx",
                    "sha256": sha256_file(enc_path),
                    "sizeBytes": os.path.getsize(enc_path),
                    "opset": enc_opset,
                    "inputs": enc_in,
                    "outputs": enc_out,
                },
                "decoder": {
                    "file": "decoder.onnx",
                    "sha256": sha256_file(dec_path),
                    "sizeBytes": os.path.getsize(dec_path),
                    "opset": dec_opset,
                    "inputs": dec_in,
                    "outputs": dec_out,
                },
            },
            "exportTool": {
                "script": "tools/sam_export/export_sam_package.py",
                "torch": torch.__version__,
                "python": sys.version.split()[0],
            },
        }
        manifest.update(C.v1_runtime_semantics(pixel_mean, pixel_std, int(sam.image_encoder.img_size)))
        try:
            import onnx as onnx_mod

            manifest["exportTool"]["onnx"] = onnx_mod.__version__
            import onnxruntime as ort

            manifest["exportTool"]["onnxruntime"] = ort.__version__
            import segment_anything as sa

            manifest["exportTool"]["segmentAnything"] = getattr(sa, "__version__", "unknown(pip: segment-anything)")
        except ImportError:
            pass

        man_path = os.path.join(tmpdir, "manifest.json")
        with open(man_path, "w", encoding="utf-8") as f:
            json.dump(manifest, f, ensure_ascii=False, indent=2)

        print("[5/5] ORT 加载自检…")
        import onnxruntime as ort

        so = ort.SessionOptions()
        so.log_severity_level = 3
        ort.InferenceSession(enc_path, so, providers=["CPUExecutionProvider"])
        ort.InferenceSession(dec_path, so, providers=["CPUExecutionProvider"])

        # 原子落位：目标目录整体替换
        out_abs = os.path.abspath(args.out)
        if os.path.exists(out_abs):
            backup = out_abs + ".bak"
            if os.path.exists(backup):
                shutil.rmtree(backup)
            shutil.move(out_abs, backup)
            print(f"既有包已备份为 {backup}")
        shutil.move(tmpdir, out_abs)
        tmpdir = None
        print(f"模型包已写出：{out_abs}")
        print(json.dumps({k: manifest[k] for k in ("formatVersion", "family", "variant")}, ensure_ascii=False))
        for name in ("encoder", "decoder"):
            print(f"  {name}.onnx sha256={manifest['files'][name]['sha256']}")
        return 0
    except Exception as exc:  # 失败关闭：不留半成品
        print(f"导出失败：{exc}", file=sys.stderr)
        import traceback

        traceback.print_exc()
        return 3
    finally:
        if tmpdir and os.path.isdir(tmpdir):
            shutil.rmtree(tmpdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
