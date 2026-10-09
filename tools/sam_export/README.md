# sam_export —— SAM1 ViT-B 离线 ONNX 模型包工具（原生推理计划·阶段1）

开发机将 `.pth` 离线转换成 DeepLux 模型包（**Python 仅作开发机转换工具，
不是生产依赖**）：

```
<package_dir>/
├── manifest.json    # 格式版本/family=sam1/variant=vit_b/SHA-256/实测 IO 规格/
│                    # 预处理与坐标变换规格/选择语义/不支持清单
├── encoder.onnx     # image_encoder（ViT-B）
└── decoder.onnx     # prompt_encoder + mask_decoder(multimask_output=True) + postprocess
```

模型包与权重**不入库**（仓库规则：模型文件不提交）；仅 SHA-256 与规格
记录于 `docs/sam_native/stage1_*.md` 证据。

## 环境（不侵入托管环境 sam_env）

导出/校验/对照依赖装进独立目录，经 `PYTHONPATH` 提供（sam_env 保持只读）：

```bash
pip3 install --no-deps --target ~/Data/Worktrees/sam_export_deps \
    onnx onnxruntime protobuf flatbuffers packaging coloredlogs humanfriendly ml_dtypes
# 运行时：
export PYTHONPATH=~/Data/Worktrees/sam_export_deps
PY=~/.deeplux/sam_env/bin/python
```

（sam_env 已含 torch/segment_anything/numpy/cv2；`--no-deps` 避免 numpy
重复安装造成版本遮蔽。）

## 用法

```bash
# 1) 导出模型包（ViT-B，opset 17；encoder CPU 导出约 1-3 分钟）
$PY export_sam_package.py --checkpoint ~/Downloads/sam_vit_b_01ec64.pth \
    --model-type vit_b --out ~/Data/Models/sam1_vit_b_onnx --opset 17

# 2) 包校验（manifest 严格 schema + 哈希 + onnx.checker + ORT 实测 IO +
#    三种 prompt 形态功能冒烟，含零尺寸维）
$PY verify_sam_package.py --package ~/Data/Models/sam1_vit_b_onnx \
    --json verify_report.json

# 3) 精度门禁（vs PyTorch 原版权照；内置细小缺陷合成图逐缺陷复核 +
#    --image 追加图像走阶段0 同配方混合 prompt）
$PY compare_onnx_pytorch.py --package ~/Data/Models/sam1_vit_b_onnx \
    --checkpoint ~/Downloads/sam_vit_b_01ec64.pth \
    --image /path/to/synth_3000_v2.png --cases-per-image 12 \
    --json compare_report.json
```

```bash
# 4) 真实图像 + 同名 YOLO-seg 标注，逐缺陷生成多边形内部正点
$PY compare_onnx_pytorch.py --package ~/Data/Models/sam1_vit_b_onnx \
    --checkpoint ~/Downloads/sam_vit_b_01ec64.pth \
    --image /path/to/images/train/example.png \
    --yolo-labels-dir /path/to/labels/train --json compare_real_report.json

# 5) 不依赖模型的门禁回归
$PY check_manifest_guards.py     # 31/31：语义、路径、哈希、大小、符号链接等
$PY check_compare_guards.py      # 排序平手与非平手、YOLO 标注解析
```

退出码（脚本一致口径）：`0` 通过；`1` 回归存在失败（guards）；`2` 参数/
前置失败；`3` 导出失败（仅 export）；`4` 校验/门禁不通过。**失败关闭**：
export 先写临时目录、全部成功后原子落位，失败不留半成品包；compare
任一门禁不过即 exit 4；标签缺失/畸形为参数错误 exit 2。load_manifest
仅接受当前 SAM1 ViT-B v1 完整运行语义，并拒绝文件穿越及符号链接。

YOLO-seg 每行需是 `class x1 y1 x2 y2 x3 y3 ...`，坐标归一化到 [0,1]。
脚本将多边形栅格化，以内部距离变换最大点作为确定性正点；原始 JSON 记录
图像/标注 SHA-256、类别、标注行、prompt、逐候选指标。此门禁检查 ONNX
与 PyTorch **结果等价**，不衡量预测掩膜相对人工标注的准确率。真实
3000×3000 工业图像与部署机性能仍需单独验收。

compare 门禁额外包含 **transformParity**：ONNX 侧坐标/框变换走
`sam_package_common` 参考实现（阶段2 C++ 的对照基准），每 case 与安装版
`ResizeLongestSide.apply_coords/apply_boxes` 逐值对照，max|Δ|>1e-9 判失败
——参考实现本身在门禁内被验证（复核二轮 P1-1）。prompt 配方与阶段0
bench 序列严格对齐：负点偏移用阶段0 序列总长 `neg_seq=103`
（runs=100+warmup=3），**不是** compare 的取用条数。

## 门禁定义（计划书阶段1，compare_onnx_pytorch.py）

| 门禁 | 阈值 | 口径 |
|---|---|---|
| 候选数一致 | 相等 | 两侧均 multimask_output=True → 3 |
| 选中候选一致 | rank-0 相同 | 两侧同按 sam_server 语义 `np.argsort(-scores)`；仅当交换的两个原始分数在两侧都相差 <0.01 时允许秩交换，仍须逐秩指标全过，JSON 记 `tieAdjusted` |
| bbox 偏差 | ≤ 1 px（逐分量） | bbox 均由二值掩膜按 sam_server `_mask_to_bbox` 口径导出 |
| 掩膜 IoU | ≥ 0.99 | 二值掩膜（PyTorch 侧 predictor 阈值 >0；ONNX 侧 logits >0） |
| 分数绝对差 | ≤ 0.01 | **未 clamp 原始值**直接相减（阶段0 F1 口径：官方 iou head 无 sigmoid/clamp，score 可 >1，两侧同源同口径） |
| 编码器单独验证 | max_abs ≤ 1e-2 且 cosine ≥ 0.999999 | ORT embedding vs `predictor.features`（官方仅提供解码器导出，编码器须单独验证） |
| 细小缺陷逐图复核 | 每缺陷一行全指标 | 内置确定性合成缺陷图（6 个 3-10px 斑）；真实图使用 `--yolo-labels-dir` 为每条标注生成内部正点 |

任一关键项不通过 → **停止原生迁移**，不以近似算法代替（计划书）。

## decoder.onnx IO 约定（manifest v1）

输入：`image_embeddings [1,256,64,64] f32`；`point_coords [N,2] f32`
（1024 尺度，`ResizeLongestSide.apply_coords` 口径）；`point_labels [N] i64`
（1=正 0=负）；`boxes [M,4] f32`（1024 尺度 xyxy）。N/M 支持 0（未提供
该类 prompt）。
输出：`low_res_masks [1,3,256,256] f32` logits；`iou_predictions [1,3] f32`
原始值（可 >1，F1）。
**postprocess=caller 模式**（与官方 wrapper 的图内 postprocess 差异，
manifest.semantics.postprocess 记录）：调用方按安装版
`Sam.postprocess_masks` 精确复刻——bilinear 256→1024²
（align_corners=False）→ **去填充裁剪 `[..., :input_h, :input_w]`**
（input_size=ResizeLongestSide.get_preprocess_shape(原图)；非方形图缺此步
会拉伸错位，方形图裁剪为 no-op）→ bilinear input_size→原图 → 阈值 >0。
原因：动态 size 插值在 torch 2.13 legacy tracer 不可导出
（upsample_bilinear2d 拒绝张量 size）；caller 模式全静态图，零动态形状
风险，阶段2 C++ 以两级 resize + 裁剪 + 阈值实现（重采样器与门禁对照）。
**不支持**：mask_input/has_mask_input（未导出；导入含该输入的包须报错）、
orig_im_size 图内 postprocess、vit_l/vit_h（首版仅 vit_b）、sam2/sam3
（family 不符须明确报错——verify 脚本的 load_manifest 即该「导入尚不
支持的模型必须明确报错」的参考实现）。

## 预处理规格（manifest.preprocessing，调用方执行）

RGB → 长边 resize 1024（**安装版 `ResizeLongestSide.apply_image` =
torchvision `resize(to_pil_image)` PIL 双线性**；阶段2 C++ 重采样器须
与门禁对照验证，不得想当然用 cv2.INTER_LINEAR）→ **先归一化后填充**
（`Sam.preprocess` 顺序：`(x - [123.675,116.28,103.53]) / [58.395,57.12,
57.375]`，0-255 尺度不除 255；再右下零填充至 1024²，填充值为归一化
空间的 0）→ NCHW。坐标/框仅缩放（无偏移），`new = int(old * 1024 /
max(h,w) + 0.5)` 口径（`apply_coords/apply_boxes`）。
