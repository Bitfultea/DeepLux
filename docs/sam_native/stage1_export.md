# SAM 原生推理计划 · 阶段1 模型导出报告

## 结论摘要（阶段1 在用户批准的可得图集范围内通过；生产门禁递延）

| 项 | 状态 |
|---|---|
| ViT-B 编码器 ONNX 导出（官方不提供，须单独验证） | ✅ encoder.onnx + encoderCheck 数值验证（vs PyTorch 内部 embedding） |
| ViT-B 解码器 ONNX 导出（含 prompt encoder，多候选输出） | ✅ decoder.onnx（三种 prompt 形态功能冒烟通过，含零尺寸维） |
| 模型包 manifest v1（格式版本/family=sam1/variant=vit_b/SHA-256） | ✅ + verify 严格校验（不支持的 family/variant/哈希不符均明确报错） |
| ONNX Runtime CPU 跑通完整预测 | ✅ 图像→encoder→embedding→prompt→decoder→掩膜（verify 冒烟 + compare 全链） |
| 精度门禁：候选数及选中候选一致 | ✅ 合成全分辨率 90 秩；真实缺陷图 297 秩（含内置合成对照），均通过；非平手换首位回归已堵住 |
| 精度门禁：bbox 偏差 ≤1px | ✅ 全部 0.00 px |
| 精度门禁：掩膜 IoU ≥0.99 | ✅ 合成全分辨率最差 0.999764；真实缺陷图最差 0.998989 |
| 精度门禁：分数绝对差 ≤0.01（未 clamp 原始值，F1 口径） | ✅ 两组最差均 **8.625e-05**（内置合成缺陷用例） |
| 坐标参考实现纳入门禁（复核二轮 P1-1） | ✅ transformParity 全 case max\|Δ\|=0.0（逐位一致）+ 7 尺寸随机批量对照 worst diff=0.0 |
| manifest 契约校验 | ✅ 完整 SAM1 ViT-B v1 语义、包内路径/符号链接；check_manifest_guards **31/31** |
| 细小缺陷样本逐图复核 | ✅ 内置合成缺陷 6/6；真实 800×565 工业图 6 张，YOLO-seg 标注 9/9（其中针孔类 6） |
| 真实全分辨率工业图复跑 | 未覆盖：用户确认暂无原图，允许本阶段改用 800×565 真实缺陷图；不能据此宣称全分辨率生产精度 |

## 基线 / 交付 / 工作树

- 原始基线：`e0a91cc`（阶段0 三轮修正）；本次复核基线：`1c10642`；独立工作树 `~/Data/Worktrees/dl_stage1`，
  分支 `agent/sam-native-stage1`（主工作区 34 个用户 WIP 文件零接触）
- 本次修正仅涉及 `tools/sam_export/*` 与 `docs/sam_native/stage1*`；
  **无 C++/CMake 改动**，主工作区用户 WIP 未触碰。本次未重新运行全量 CTest。
- 模型包与权重**不入库**（仓库规则），仅 SHA-256/规格记录于本文档

## 模型包

- 路径（本机生成/验证）：`~/Data/Models/sam1_vit_b_onnx/`；修正前包备份于
  `~/Data/Models/sam1_vit_b_onnx.pre_stage1_fix_20260930/`，未覆盖旧 ONNX 权重
- manifest：formatVersion=1，family=sam1，variant=vit_b
- 源权重：`~/Downloads/sam_vit_b_01ec64.pth` sha256=`ec2df62732614e57411cdcf32a23ffdf28910380d03139ee0f4fcbe91eb8c912`（375,042,383 B）
- `encoder.onnx`：sha256=`f6b1ae360681154c691c1947f6931ff6977c16ad92faf97e0ee5f2ad0867be62`（359,210,524 B，opset 17）
  - IO：`input_image [1,3,1024,1024] f32` → `image_embeddings [1,256,64,64] f32`
- `decoder.onnx`：sha256=`b0d19895c9128ea20376a4bc73cedaf21163b6ba761f317b7cb52cf9c2adb797`（16.6 MB，opset 17）
  - IO：`image_embeddings [1,256,64,64] f32`、`point_coords [num_points,2] f32`、
    `point_labels [num_points] i64`、`boxes [num_boxes,4] f32`（xyxy，1024 尺度）
    → `low_res_masks [1,3,256,256] f32`（logits）、`iou_predictions [1,3] f32`（原始值，可>1）
  - N/M 支持 0（仅点/仅框/混合三形态实测通过）

## 导出环境与方式（不侵入任何既有环境）

- 解释器：`~/.deeplux/sam_env`（产品托管环境，**只读使用**）：python 3.10.12、
  torch 2.13.0+cu130（CPU 执行）、segment_anything（安装版）
- 导出依赖装入独立目录 `~/Data/Worktrees/sam_export_deps`（PYTHONPATH 注入）：
  onnx 1.23.0、onnxruntime 1.23.2（CPU）、protobuf 7.36.2、flatbuffers 25.12.19、
  packaging 26.3、coloredlogs 15.0.1、humanfriendly 10.0、ml_dtypes 0.6.0
  （`pip3 install --no-deps --target`，避免 numpy 重复安装遮蔽 sam_env 的 2.2.6）
- 导出器：torch legacy exporter（`dynamo=False`），opset 17，constant folding 开

## 设计决策（与官方导出脚本的差异均记录于 manifest）

- **D1 postprocess=caller**：官方 wrapper 把 256→1024→原图两级双线性放进图内，
  其动态 size 插值在 torch 2.13 legacy tracer 不可导出（upsample_bilinear2d
  拒绝张量 size，实测）；本包输出 `low_res_masks [1,3,256,256]`，调用方按
  `Sam.postprocess_masks` 同口径执行（bilinear align_corners=False 两级 + >0
  阈值）。阶段2 C++ 以两级 cv::resize + 阈值实现。
- **D2 PromptEncoder 等价重构**：安装版 `_embed_points/_embed_boxes/
  forward_with_coords` 的掩码索引赋值与原位切片赋值在 legacy tracer 下被
  常量化（实测 Where 广播维固化，动态 N 运行时崩）；导出 wrapper 逐行等价
  改写为纯广播算术（`pos*(1-m₋₁)+m₋₁*w_not_a_point+m₀*w₀+m₁*w₁` 等），
  点/框各追加哑元行消除零尺寸张量，再以 Shape→Range→index_select 动态裁剪。
  **安装版语义 `pad=(boxes is None)`**（仅点 prompt 时 token 集含
  not_a_point pad token）被精确复刻：三种 prompt 形态的 token 集与安装版
  predictor 逐一等价，数值等价由精度门禁验证。
- **D3 多候选输出**：不做官方的单掩膜选择，输出全部 3 通道 + 未 clamp 原始
  IoU 分数；候选排序/选择在下游按 sam_server 现行语义（argsort(-scores)，
  best=第 0）——支撑批4 candidates 协议；分数原始值口径遵循阶段0 F1。
- **D4 范围收窄**：mask_input/has_mask_input 不导出（现工作流不用掩膜
  prompt；manifest.unsupported 记录，导入含该输入的包须报错）；box 单框
  约束与 predictor 一致；vit_l/vit_h、sam2/sam3 拒绝（load_manifest 明确
  报错——「导入尚不支持的模型必须明确报错」的参考实现）。
- **manifest IO 规格以 ORT 实测为准**（onnx shape_inference 对 ViT 窗口注意力
  输出残留符号维；ORT 推导完整）；verify 脚本按同口径复核。

## 编码器单独验证（官方仅提供解码器导出）

encoderCheck（ORT embedding vs `predictor.features`，ONNX 侧预处理直接
复用安装版管线 `ResizeLongestSide.apply_image + Sam.preprocess`，与
`set_image` 内部逐位同构——门禁只度量导出图本身）：

| 图像 | max_abs | mean_abs | cosine | 门禁(≤1e-2 且 ≥0.999999) |
|---|---|---|---|---|
| 内置细小缺陷合成图 1280×720 | 4.11e-06 | 1.46e-07 | 1.000000119 | ✅ |
| synth_3000_v2 3000×3000（sha256=a6a01c52…与阶段0 同图） | 4.47e-06 | 1.25e-07 | 1.000000119 | ✅ |

## 精度门禁结果（compare_onnx_pytorch.py，vs SamPredictor 原版权照）

门禁阈值：候选数一致；选中候选一致（argsort(-scores) 同语义，平手带
<0.01 内允许秩交换但须逐秩指标全过并记 tieAdjusted）；bbox 逐分量
≤1px；掩膜 IoU ≥0.99；|Δscore| ≤0.01（**未 clamp 原始值直接相减**，
阶段0 F1 口径）。任一关键项不通过即停止原生迁移。

**结果：compare PASS（exit 0）**（复核二轮修正版：参考实现变换 +
transformParity 门禁 + 配方负点对齐阶段0 neg_seq=103），两图共 24 个
混合 prompt case（阶段0 同序列前 12 个，负点坐标逐一相同）+ 6 个逐缺陷
复核 case，合计 **90 个候选秩全部通过**：

| 图像 | encoder | case 数 | 最差 IoU | 最差 scoreΔ | 最差 bboxΔ | parity max\|Δ\| | ties |
|---|---|---|---|---|---|---|---|
| 内置细小缺陷 1280×720（非方形，覆盖去填充裁剪路径） | ✅ | 12+6 缺陷（54 秩） | 0.999807 | **8.625e-05** | 0.00 | 0.0 | 0 |
| synth_3000_v2 3000×3000（方形） | ✅ | 12（36 秩） | 0.999764 | 1.132e-05 | 0.00 | 0.0 | 0 |
| **全量（90 秩）** | max_abs≤4.47e-06 | 30 | **0.999764** | **8.625e-05** | **0.00** | **0.0（逐位一致）** | 0 |

> 复核二轮 P2-4 校正：初版文档「最差 scoreΔ 1.8e-05」漏计缺陷用例，
> 全量 90 秩实际最大 **8.625e-05**（仍在 ≤0.01 阈值内 116 倍裕量）。
> transformParity=0.0 表示参考实现（scale_coords/scale_boxes）与安装版
> 变换在全部 case 逐位一致。

原始 JSON（逐 case 全指标 + parity）：`stage1/compare_report.json`。
这份报告仅含合成图，不包含下面的真实样本。

### 真实工业缺陷样本（本轮新增）

用户提供图集 `dataset_rank_plus_pinhole_20260811/images/train`，使用同级
`labels/train` 的 YOLO-seg 多边形，每条标注栅格化后选内部距离最大点作为
确定性正点。6 张 `new_pinhole_{162,163,165,166,167,168}.png`
均为 **800×565**；合计 9 条标注（class 0 六条、class 2 三条）。
工具对每张图运行 12 条混合 prompt，逐标注再运行一点 prompt，并始终包含
内置合成对照：7 图、84 条混合用例、15 条逐缺陷用例、**297 个候选秩**。
全部通过；最差 IoU **0.99898888**，最大分数绝对差
**8.6248e-05**，最大 bbox 偏差 **0px**，坐标变换 parity **0**，
无 tieAdjusted。逐图 SHA-256、标注 SHA-256、prompt、原始指标见
`stage1/compare_real_report.json`。这证明导出模型与 PyTorch 路径等价，
**不证明预测掩膜与人工标注重合**，也不替代真实约 3000×3000 图像验收。

## 预处理/坐标/postprocess 语义核对（阶段2 必读；复核二轮校正表述）

> 本节初版标题为「安装版与官方 GitHub 版的差异」，并称官方 pad 极性
> 相反——复核查明**该表述错误**：官方仓库同为 `pad=(boxes is None)`。
> 逐项核对结论：安装版语义与官方仓库一致；下列各条实为**我方初版实现
> 错误**（按错误记忆写码），全部由精度门禁或复核暴露。阶段2 以安装版
> 源码为唯一口径，不得按记忆实现。

- **X1 pad 语义**：`PromptEncoder.forward` `pad=(boxes is None)`（安装版
  与官方一致）——仅点 prompt 时 token 集含 not_a_point pad token。导出图
  以 Shape→Range→index_select 动态裁剪精确复刻三形态 token 集。
- **X2 预处理顺序**：`Sam.preprocess` **先归一化后零填充**。初版按错误
  记忆先填充后归一化 → encoder cos 仅 0.805，门禁抓住。
- **X3 重采样器**：`ResizeLongestSide.apply_image` = torchvision
  `resize(to_pil_image)`（PIL 双线性）。初版误用 cv2.INTER_LINEAR 口径——
  阶段2 C++ 重采样器须与门禁对照，不得想当然。
- **X4 postprocess**：`Sam.postprocess_masks` 256→1024² 后含**去填充裁剪**
  `[..., :input_h, :input_w]` 再插值回原图。初版缺裁剪 → 方形图全过、
  非方形图掩膜拉伸错位（IoU 0~0.28），门禁在 1280×720 图精确暴露。
- **X5 坐标变换参考实现（复核二轮 P1-1 修正）**：`apply_coords` 用
  `int(old*scale+0.5)`（**不得再 round()**，395 边：404 vs 405 差 1）；
  `apply_boxes` = reshape(-1,2,2) 后**全部四分量**缩放。初版
  `scale_coords` 双重取整且对框只缩放前两值（640×480 框 [50,50,200,200]
  安装版得 [80,80,320,320]，初版误得 [80,80,200,200]）；verify 冒烟只查
  形状/有限性未暴露。修正：重写 `get_preprocess_shape/scale_coords` +
  新增 `scale_boxes`；与安装版 7 种尺寸×随机批量对照**逐位一致
  （worst diff=0.0）**；compare 门禁新增 `transformParity`（每 case 与
  安装版变换逐值对照，max|Δ|>1e-9 即判失败）——参考实现正式纳入门禁。
- **X6 API 名**：无 `transform_coords/transform_boxes` 方法（用
  `transform.apply_coords/apply_boxes(coords, original_size)`）；
  `MaskDecoder.forward` 参数名 `sparse_prompt_embeddings/
  dense_prompt_embeddings`。
- **X7 torch 2.13 legacy tracer 三坑**：masked-assign 常量化（Where 广播维
  固化）、动态 size 插值拒绝张量 size、零尺寸张量常量化 Reshape——
  以 D1/D2 等价重构绕开。

迭代记录（门禁与复核驱动的四轮修正，全部由对照数字/复现暴露）：
1. 直调 prompt_encoder → ORT Where/Reshape 广播崩 → D2 等价重构；
2. encoderCheck cos=0.805 → X2/X3 → 对照脚本改用安装版变换管线；
3. 非方形图 IoU 0~0.28 → X4 → caller postprocess 补裁剪步；
4. 复核二轮：坐标参考实现 X5 重写+纳入门禁；load_manifest 补必需语义
   字段与包内路径校验（当时 check_manifest_guards 21/21）；compare 配方负点
   偏移对齐阶段0（neg_seq=103，初版 n=12 时 posneg 负点与阶段0 序列
   不一致，「同配方」声明一度不实）；文档 X1 错误表述撤回。

## 包校验（verify_sam_package.py 8/8 PASS + check_manifest_guards.py 31/31）

verify：manifest 严格 schema（formatVersion/family/variant/完整 v1 运行语义
preprocessing/coordinateTransform/semantics；任意非空伪对象、错误预处理
顺序/坐标公式/后处理模式均拒绝；files.*.file 仅允许包内裸文件名，
绝对路径/子目录/`..` 穿越及包内符号链接拒绝）、SHA-256 与大小逐字节核对、onnx.checker、ORT
实测 IO 与 manifest 一致（名称/dtype/静态维/动态维符号）、encoder 冒烟
（有限 embedding）、decoder 三形态冒烟（points-only/box-only/mixed，含
零尺寸维；坐标经修正后的参考实现变换）。原始 JSON：
`stage1/verify_report.json`。

拒绝用例回归 check_manifest_guards.py **31/31**（1 正向对照 + 30 拒绝：
必需语义字段缺失/空/非对象/伪对象、错误运行语义、跨平台路径穿越、
bool 版本号、缺失大小、包外符号链接、哈希/大小不符等）。
`check_compare_guards.py` 的非平手首位换位、真平手、空候选、YOLO 标注
合法/畸形/缺失回归亦通过；旧门禁可能将非平手换首位当作通过，现仅
在两侧原始分数均处于 <0.01 平手带时容许换位。

## 证据文件清单（docs/sam_native/stage1/）

- `verify_report.json` —— 包校验逐项结果
- `compare_report.json` —— 合成 3000×3000 + 内置缺陷对照原始数据
- `compare_real_report.json` —— 6 张真实图 + 标注逐缺陷对照原始数据

## 缺口与待办

| # | 项 | 说明 |
|---|---|---|
| G1 | 机台角色 | 当前 RTX 3090 本机按**生成/验证机**使用，不认定为最终生产机；模型包应可迁移，但目标机部署、CUDA 与端到端耗时仍需独立验收 |
| G2 | 真实全分辨率工业图 | 用户确认暂无约 3000×3000 原图，批准本阶段使用其他尺寸；已用 6 张真实 800×565 缺陷图与同名标注逐缺陷复核。未来取得原图后须补跑，不得把合成图或 800×565 结果表述为全分辨率证据 |
| — | F1 契约裁决 | 本包输出原始分数（可>1）；阶段2/3 接入产品时须落地 clamp/放宽裁决（阶段0 G5） |

阶段1 的导出与当前可得真实数据等价门禁已通过，可以推进阶段2。
G1 的目标机部署/CUDA 性能和 G2 的真实全分辨率精度仍是生产验收缺口，
本阶段通过不等于这些门禁通过。
