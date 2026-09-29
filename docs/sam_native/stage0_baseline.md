# SAM 原生推理计划 · 阶段0 基线报告

## 结论摘要（阶段0 状态：**CUDA 基线已补测（2026-09-29 驱动修复后）；余 G1 生产机台确认 / G2 真实图集两项缺口**）

| 项 | 状态 |
|---|---|
| 性能脚本（同图 100 次混合 prompt，首次编码/重复预测分测） | ✅ `tools/sam_bench` + 配方固化 + 参数守卫回归 22/22 + 重试/二次复核回归 9/9 |
| 开发机真实 ViT-B **CPU** 基线（现有 Python HTTP 路径） | ✅ 原始 P50/P95 见下，JSON 入库 |
| 路径开销基线（内嵌 stub，无 Python，小载荷口径） | ✅ P95 0.545ms（修复版 bench 复测；首测 0.43ms） |
| 隔离脏工作区 | ✅ 34 个用户 WIP 文件未混入；阶段0 仅提交 bench/构建接入/文档 |
| 基线提交 + 定向测试 + 全量 CTest 证据 | ✅ c2ee793；隔离树 75/76（唯一失败为既有 test_runengine 环境项）+ SAM 定向 9 套件全绿 |
| **CUDA 基线** | ✅ **已补测**（2026-09-29 驱动修复后）：RTX 3090 / 驱动 580.178.04 / CUDA 13.0；首次编码 395.1ms（CPU 的 7.9×加速）、重复预测 P95 2700.1ms（与 CPU 几乎持平→F2 实测归因）；见 B 节 |
| 目标机（生产机台）清单 | ⚠️ 本机清单已补全（GPU/驱动/系统，见环境节）；本机是否即「指定生产机台」待用户确认（G1） |
| 原版 SAM 测试图集（真实工业图） | ❌ 本机缺失（缺口 G2）→ 基线用确定性合成图 v2 替代；CUDA 基线同口径，真实图到位后复跑 |

## 基线提交与工作区隔离

- 基线提交：`c2ee793`（revert 批次，树与批4 交付 `c55d290` 逐字节一致，`git diff` 为空）
- 当前工作区含用户未提交 WIP 34 文件（AgentActor 重构、connect_modules 多端口、Agent 聊天 UI 重构、SAM 对话框下拉框改版等）——阶段0 提交**仅**包含：`tools/sam_bench/*`（新增）、顶层 `CMakeLists.txt`（+2 行 add_subdirectory）、`docs/sam_native/*`（新增）。共享文件零触碰。
- 隔离树验证：`/tmp/dl_verify_b4` @ c55d290（构建 EXIT=0），测试证据见 `stage0/ctest_evidence.txt`。

## 硬件与系统清单（开发机=当前执行机；生产机台=G1 待补）

- CPU：Intel i9-12900K（16C/24T）；内存 62 GiB
- OS：Ubuntu 22.04.5 LTS，kernel 6.8.0-138-generic，x86_64
- GPU：**NVIDIA GeForce RTX 3090 24GB（GA102，capability 8.6）**。首测时驱动未加载（nvidia-smi 失败、cuda_available=False）；**2026-09-29 用户修复驱动后**：nvidia-smi 正常（Driver 580.178.04 / CUDA 13.0），sam_env torch 2.13.0+cu130 `cuda_available=True`——CUDA 基线据此补测（B 节）
- 工具链：g++ 11.4.0、cmake 3.29.8、Qt 5.15.3（系统 qtbase5-dev）
- 原始输出：`stage0/env_evidence.txt`

## Python 环境清单

- 系统 python 3.10.12：**无** torch/fastapi/segment_anything；onnxruntime 1.23.2（CPU/Azure EP）
- **托管环境 `~/.deeplux/sam_env`**（本次 sam_server 实跑环境）：python 3.10.12、torch 2.13.0+cu130（cuda_avail False）、fastapi 0.139.2、uvicorn 0.51.0、segment_anything 可导入、numpy 2.2.6、PIL 12.3.0、cv2 5.0.0
- miniconda 8 环境（LLM/isat_env/m3dm/openpcdet/tiffTool/vllm/yolo 等）均无三件套；isat_env 含 isat-sam 1.4.7
- ⚠️ F3：sam_env 实际版本超出 `tools/sam_server/requirements.txt` 锁定范围（fastapi<0.111 vs 0.139.2；numpy<2.0 vs 2.2.6；torch/segment-anything 在 requirements 中被注释未锁）——环境漂移，阶段1 复现需注意
- 权重：`~/Downloads/sam_vit_b_01ec64.pth`，375,042,383 B，sha256=`ec2df62732614e57411cdcf32a23ffdf28910380d03139ee0f4fcbe91eb8c912`

## 测试图集

- 计划规格：1×约 3000×3000 真实工业图（P95 目标图）+ 细小缺陷样本集（阶段1 IoU≥0.99 逐图复核用）
- 现状：**真实图集缺失（G2）**。基线以 `sam_bench --gen-image` 确定性合成图 v2 替代：低频平滑底 + 12×12 均匀灰度单元（2px 暗边界）+ 单元中心对比小块；3000×3000，sha256=`a6a01c528d83b67ddb66117fb28cd9c5513697cbe7f825506d59e29a2329e728`
- v1 高频斜纹生成器已废弃：诱导真实 SAM 输出退化全图掩膜（bbox=[0,0,3000,3000]）且 score=1.0134>1（F1 首批证据）
- **合成图数据的适用边界（复核二轮 P1-3 修正）**：编码器成本主要由分辨率决定（ViT 输入统一 resize 1024²，架构事实），但解码器/掩膜后处理/编码成本随**掩膜形态与图像内容**变化（内容影响掩膜面积与 RLE 游程结构、PNG 压缩率）。合成图基线仅作为开发机流程验证与数量级参考，**不得替代生产机台真实图基线（G1/G2）作为验收依据**；精度门禁（阶段1）必须使用真实图集

## 性能脚本与口径

- 工具：`tools/sam_bench`（走产品同款 `SamBackendClient` HTTP 路径；口径=HTTP 客户端至预测结果，**不含 GUI 候选显示段**——0.5s 交互目标须另测 GUI 段，见 G4）
- 固定配方：`i%10` → 0–3 单正点(40%) / 4–6 正+负点(30%) / 7–9 框选(30%)；坐标纯整数取模，跨机台一致（README 有完整公式，禁止改动）
- 失败策略：默认 `--on-error fail`（产品语义失败关闭）；基线采集用 `--on-error continue`——复核二轮 P2-4 收窄 + **复核三轮 P1 二次复核**：客户端对 score 缺失/类型非法/数值越界返回同一错误串（SamBackendClient.cpp:906 口径），仅按文本分类会把协议故障误放行；continue 下文本命中后必须以原始 HTTP 重放同一 prompt（服务器对同 embedding+prompt 结果确定性）自行解析 JSON，**仅确证「字段存在、为有限数值、越出 [0,1]」才放行**（violations 记 classification=F1-score-out-of-range-confirmed + confirmedRawScore + recheckDetail）；缺失/类型非法/不可复现/复核请求失败一律 exit 4；网络错误/超时/其他解析失败无论模式一律 exit 4；违约样本不计入延迟统计；原「连续 5 次失败上限」已移除（非 F1 错误即时中止后该上限无意义）
- 计时口径（复核二轮 P1-2 修正）：完成信号按操作类型隔离——set_image 只等 `embeddingReady`，predict 只等 `predictionReady`；predict 期间的 `embeddingReady`（客户端对 invalid_embedding 的自动重试链：重 setImage→重发 predict）记为 JSON `retryEvents` 并继续等待，不再可能被误计为一次成功预测；predict 完成后又收到 embeddingReady → 内部不变量违背 exit 5。注入自检：`--inject-invalid-embedding N`（一次性 invalid_embedding）、`--inject-predict-error`（持续 status:error，验证 continue 收窄）、`--inject-score-fault missing|string|outrange`（持续 score 协议故障/确证越界，验证二次复核分类——复核三轮 P1）
- 回归：`check_arg_guards.sh` **22/22**（含 `--warmup abc` ok 复用回归 + 5 个注入参数守卫）；`check_retry_regression.sh` **9/9**（inject=1/2 重试计时×3、continue 非 F1 失败关闭、fail 对照、score=missing/string 协议故障 continue 下 exit 4×2、missing fail 对照、outrange 确证对照 violations=3 全部 confirmed rawScore=1.5）
- 真实后端操作规程：先 `GET /health` 预热模型（首次 1664ms / 重跑 1789ms 至 ready），再跑 bench，避免模型加载计入首次编码（`stage0/server_health_preload.txt`）

## 基线原始数据

### A. 真实 ViT-B CPU（现有 Python sam_server HTTP 路径）

#### A1. 首测（原 bench，探索数据）

> 复核二轮已独立核对本组数据（95 有效样本、5 次 F1 越界、P95=2885.13ms）。
> 该组数据采集于计时信号隔离修正（P1-2）之前；本次运行中 embedding 全程
> 有效、未发生 invalid_embedding 重试，embeddingReady 不可能在 predict
> 窗口内触发，故计时未受该缺陷影响——保留为探索数据，主证据以 A2 为准。

命令：`sam_bench --server http://127.0.0.1:56615 --image <synth_3000_v2> --runs 100 --warmup 3 --timeout 300000 --on-error continue`，exit 0。
环境：sam_env torch 2.13.0+cu130→CPU（i9-12900K），sam_server @ c55d290 版本。

| 指标 | 值 |
|---|---|
| 首次编码 set_image | **3191.1 ms**（含 3000² PNG 解码 + resize 1024² + ViT-B CPU 编码；模型已预热） |
| 重复预测（有效样本 **95/100**） | min 2723.0 / **P50 2803.1** / **P95 2885.1** / P99=max 2961.3 / mean 2801.8 ms |
| 分组 pos（n=38） | P50 2819.9 / P95 2885.1 / max 2892.6 ms |
| 分组 posneg（n=27） | P50 2831.7 / P95 2892.0 / max 2961.3 ms |
| 分组 box（n=30） | P50 2748.9 / P95 2803.1 / max 2825.5 ms |
| F1 违约样本 | **5/103**（predict[23,92] pos、[56,66,75] posneg；全部 score>1 被严格契约拒绝，详见 violations 与 `stage0/f1_raw_scores.txt`） |
| 候选数(首/末)、score(首/末) | 3/3、0.990/0.975 |

原始 JSON（含 95 个逐次延迟数组）：`stage0/stage0_cpu_vitb_synth3000.json`

#### A2. 复测（修复版 bench：信号隔离 + F1-only continue，主证据）

命令同 A1（端口 48975，预热 1789ms），exit 0。同图同配方（sha256 一致）。

| 指标 | 值 |
|---|---|
| 首次编码 set_image | **3162.1 ms** |
| 重复预测（有效样本 **95/100**） | **P50 2728.4 / P95 2857.6** / P99=max 2968.3 / mean 2742.8 ms |
| 分组 pos（n=38）/ posneg（n=27）/ box（n=30） | P50 2756.0 / 2793.0 / 2691.8；P95 2857.6 / 2883.3 / 2778.5 ms |
| F1 违约样本 | **5/103，索引与 A1 完全一致**（23、92 pos；56、66、75 posneg；classification=F1-score-out-of-range）——配方确定性佐证 |
| retryEvents | 0（稳定运行无 invalid_embedding，佐证 A1 计时未受 P1-2 缺陷影响） |
| 候选数(首/末)、score(首/末) | 3/3、0.990/0.975 |

与 A1 对比：P95 2885.1→2857.6ms（-1.0%，运行间正常波动），结论不变。原始 JSON：`stage0/stage0_cpu_vitb_synth3000_fixedbench.json`

#### A3. 复测（复核三轮修正版 bench：score 违约二次复核，当前主证据）

命令同 A2（端口 32773，预热 1742ms），exit 0。同图同配方（sha256 一致）。

| 指标 | 值 |
|---|---|
| 首次编码 set_image | **3129.9 ms** |
| 重复预测（有效样本 **95/100**） | **P50 2734.2 / P95 2826.5** / P99=max 2846.9 / mean 2736.0 ms |
| 分组 pos（n=38）/ posneg（n=27）/ box（n=30） | P50 2748.4 / 2761.0 / 2695.7；P95 2829.6 / 2842.3 / 2742.6 ms |
| F1 违约样本 | 5/103，索引与 A1/A2 一致（23、92 pos；56、66、75 posneg），**全部经二次复核确证为数值越界**，confirmedRawScore=1.000424861907959 / 1.00141978263855 / 1.00023353099823 / 1.000590443611145 / 1.002448201179504 |
| retryEvents | 0 |

三轮 P95：2885.1 → 2857.6 → 2826.5 ms（±1% 运行间波动，结论稳定）。违约原始值现由**生产路径内取回**（bench 二次复核重放同 prompt），F1 证据不再依赖旁路 curl。原始 JSON：`stage0/stage0_cpu_vitb_synth3000_a3.json`

**解读（复核二轮 P1-3 修正归因口径）**：与 0.5s 目标差 ~5.8×（P95 2885ms）。C 项 stub 的 P95 0.43ms **仅证明**客户端+HTTP+严格解析在**小载荷**下的开销 <1ms——stub 返回 2×2 掩膜 PNG（SamTestServer `tinyMaskPngBase64`），而真实路径需传输并解析 3 个 3000² 全尺寸掩膜的 PNG base64（载荷量级差约 10⁶），两者不可直接相减归因。「耗时主体在服务端（ViT-B CPU 解码器推理 + `sam_server` 纯 Python 逐像素 `_encode_rle` + 全尺寸掩膜 PNG base64 编码）」目前是**待剖析的推测**（依据：`_encode_rle` 为逐像素 Python 循环，3000²×3 候选≈27M 次迭代，数量级估算与 2.8s 相符；传输/解析段占比未单独测量），阶段2 以剖析数据确认，不作为已测结论。CPU 路径按计划不承诺 0.5s；该基线的意义是量化旧路径并为阶段2/4 提供对照。

### B. CUDA 基线（本机 RTX 3090，驱动修复后 2026-09-29 补测）

命令：`sam_bench --server http://127.0.0.1:38739 --image <synth_3000_v2> --runs 100 --warmup 3 --timeout 300000 --on-error continue`，exit 0。环境：`DEEPLUX_SAM_DEVICE=cuda` 显式指定；驱动 580.178.04 / CUDA 13.0 / torch 2.13.0+cu130（cuda_available=True，capability 8.6）；服务进程 GPU 显存 658MiB（加载）→ 峰值 4097MiB（运行）。同图同配方（sha256 与 A1/A2/A3 一致）。

| 指标 | CUDA | CPU（A3） | 倍率 |
|---|---|---|---|
| 首次编码 set_image | **395.1 ms** | 3129.9 ms | **7.9× 加速**（含读图/resize/上传+ViT-B GPU 编码） |
| 重复预测 P50 / P95（有效 95/100） | 2624.9 / **2700.1 ms** | 2734.2 / 2826.5 ms | **≈持平（-4%）** |
| F1 违约 | 5/103，索引与 CPU 完全一致，确证原始值 1.000432253 / 1.001420498 / 1.000236034 / 1.000603557 / 1.002443671（与 CPU 值差 <1e-5，设备无关性佐证） | 同左 | — |
| retryEvents | 0 | 0 | — |

**关键归因（天然 A/B 实验，F2 由推测升级为实测结论）**：设备 CPU→CUDA 只改变推理段，首次编码 -2735ms（ViT-B 编码确在 GPU 上加速 7.9×），而重复预测仅 -110ms——即 **~2.6s 的 predict 耗时中解码器推理仅占约 0.1s，其余 ~2.5s 为设备无关的 sam_server Python 后处理**（`_encode_rle` 纯 Python 逐像素 ×3 候选 + 全尺寸掩膜 PNG base64 + MB 级 JSON 序列化 + HTTP 传输）。佐证：运行期 GPU util 采样（2s 间隔）最大仅 16%、多数 2-4%——GPU 绝大部分时间空闲等待服务端后处理。阶段2 原生 C++ 消除该段后，CUDA 路径 P95≤0.5s 的预算充足（编码 395ms 仅首次；重复预测 = GPU 解码 ~0.1s + C++ 后处理）。原始 JSON：`stage0/stage0_cuda_vitb_synth3000.json`

> 注：本组仍为合成图 v2 口径；生产机台/真实图（G1/G2）到位后以同命令复跑即得验收数据。0.5s 目标为「点选→候选显示」交互口径，本数据为 HTTP 客户端至预测结果段（GUI 段另计，G4）。

### C. 路径开销（内嵌 stub，无 Python；同合成图 v2，runs=100 warmup=3，on-error=fail）

- 首测（原 bench）：首次编码 0.696 ms；预测 P50 0.2 / P95 0.43 / max ~0.6 ms；有效 100/100 → `stage0/stage0_dev_stub_synth3000.json`
- 修复版 bench 复测（信号隔离后）：首次编码 0.654 ms；预测 P50 0.249 / **P95 0.545** / max 1.328 ms；有效 100/100，retryEvents 0 → `stage0/stage0_dev_stub_synth3000_fixedbench.json`

→ `SamBackendClient`+HTTP+JSON 严格解析（含 3 候选）在**小载荷**（stub 掩膜为 2×2 PNG）下开销 <1.5ms。**注意（P1-3）**：真实路径的全尺寸掩膜传输与解析成本不在此数内，不能据此推断真实路径的客户端占比。

## 关键发现与风险

- **F1（阻塞级，需阶段2/3 裁决）**：官方 segment_anything `MaskDecoder.iou_prediction_head` 为纯 MLP，**无 sigmoid/clamp**（`sigmoid_output=False` 仅作用于 mask 分支；源码证据见 `stage0/env_evidence.txt` 末节），IoU score 可 >1。A3 复测中 5 例违约原始值已**经生产路径内二次复核取回并确证为数值越界**（1.000425 / 1.001420 / 1.000234 / 1.000590 / 1.002448，均略超 1；v1 斜纹图旁路 curl 另测得 1.0134/1.0138 伴全图退化掩膜）。`sam_server` 直接透传 `float(scores[best])`；DeepLux 批3 严格契约 score∈[0,1] 拒绝整次预测 → **现有生产路径对真实模型输出以约 5%（本图集 5/103）概率失败关闭**。候选 score 同样受影响（批4 candidates 严格解析同口径）。裁决选项：服务端/导出侧 clamp（改变 score 语义）vs 客户端契约放宽（有限值 + clamp 用于展示、保留原值排序）。**阶段1「分数绝对差≤0.01」门禁须以未 clamp 原始值定义对比口径**。
- **F2（已由推测升级为实测归因，CUDA 补测 A/B 实验）**：设备 CPU→CUDA 后首次编码 3130→395ms（7.9×），而重复预测 P50 仅 2734→2625ms（-4%）——**~2.6s predict 中解码器推理仅约 0.1s，其余 ~2.5s 为设备无关的服务端 Python 后处理**（`_encode_rle` 纯 Python 逐像素 ×3 候选 + 全尺寸掩膜 PNG base64 + MB 级 JSON + HTTP 传输）；运行期 GPU util 采样最大 16%（多数 2-4%）佐证 GPU 长期空闲等待。阶段2 原生 C++（RLE/掩膜编码 C++ 化、按需编码）为 P95≤0.5s 的主要收益来源；该归因基于同图同配方 A/B 对照，非分段剖析——阶段2 仍以剖析数据细化各子段占比。
- **F3**：sam_env 版本漂移超出 requirements.txt 锁定（见上）。
- **F4**：隔离树 `test_runengine::testMultipleBreakpointsCanResumeAndStop` 崩溃为既有环境/布局敏感项（基线 c51dce3 隔离树同签名 28/1、主工作区 76/76 通过、复核方历轮无法复现）——非阶段0 引入。
- **F5（已解决，2026-09-29）**：GPU 驱动未加载曾阻塞 CUDA 基线；用户修复驱动（580.178.04 / CUDA 13.0）后 G3 已补测（B 节）。阶段4 的本机前置条件就绪。
- **F6**：bench 两次 fail 模式全量运行均正确失败关闭（v1 图 predict[0]、v2 图 predict[23]，exit 4、无部分 JSON）——工具失败语义实测有效。
- **F7（复核二轮新观察，既有客户端行为，低优先）**：`SamBackendClient` 对 invalid_embedding 的自动重试**无次数上限**——每轮重试在 embedding 就绪时清除 `m_imageRetryPending`，若服务器持续返回 invalid_embedding 将无限循环「重 setImage→重 predict」（bench inject=2 实测 retryEvents=2 后成功；正常场景=服务器重启后自愈，属合理设计，但持续故障场景仅受单请求超时约束、无总重试预算）。阶段3 接入本地原生后端时该链路语义会重新设计，届时一并处置；本阶段不改动产品代码。

## 测试证据（命令 + 退出码）

- 隔离树 @ c55d290（≡c2ee793）：`ctest` → **exit 8**，75/76（唯一失败 test_runengine，单跑 exit 134，28 passed/1 failed）；SAM 定向 9 套件全绿 exit 0（25/16/57/16/12/15/8/5/8）——明细 `stage0/ctest_evidence.txt`
- 主工作区（含用户 WIP）：全量 CTest **76/76**（复核二轮修改后复跑，见提交信息）
- bench（复核三轮修正版）：arg guards **22/22**（exit 0）；retry regression **9/9**（inject=1→retryEvents=1、inject=2→retryEvents=2、continue 非 F1 exit 4、fail 对照、score=missing/string 协议故障 continue 下 exit 4×2、missing fail 对照、outrange 确证对照 violations=3 全部 confirmed rawScore=1.5）；stub 冒烟与 3000² 复测 exit 0；真实后端 probe runs=10 exit 0；全量 fail 模式 exit 4×2（F6）；continue 模式 exit 0×4（A1/A2/A3/**B-CUDA**：CUDA 补测中二次复核对 5 例违约逐一确证，GPU 原始值与 CPU 差 <1e-5）
- bench 负向验证：二轮 MUT-A（predict 完成信号回归为 embeddingReady）→ retry 回归 3 用例红（retryEvents=0）；MUT-B（continue 放宽回“所有错误放行”）→ 非 F1 用例红（exit 0≠4）；三轮 MUT-C（旁路二次复核门禁 `if (false && …)`）→ **score=missing/string 两反例红（exit 0≠4，即审核指出的误分类复现）**，确证对照与 fail 对照保持绿；三次突变均还原 cmp 逐字节一致后全绿
- `tests/helpers/SamTestServer.h` 新增 `predictInvalidEmbeddingOnce`（默认 0，向后兼容）与一次性注入分支——既有测试全量复跑不受影响（见上）
- sam_server.py 本阶段零改动

## 证据文件清单（docs/sam_native/stage0/）

- `stage0_cpu_vitb_synth3000.json` —— CPU 真实基线原始数据（A1 首测/探索，逐次延迟数组+violations；复核方已独立核对）
- `stage0_cpu_vitb_synth3000_fixedbench.json` —— CPU 真实基线原始数据（A2 复测，信号隔离版 bench）
- `stage0_cpu_vitb_synth3000_a3.json` —— CPU 真实基线原始数据（**A3 复测/CPU 主证据**，二次复核版 bench；violations 含 confirmedRawScore 与 recheckDetail）
- `stage0_cuda_vitb_synth3000.json` —— **CUDA 基线原始数据**（B 节，驱动修复后补测；GPU 显存/util 证据见 server_health_preload.txt 末节）
- `stage0_dev_stub_synth3000.json` / `stage0_dev_stub_synth3000_fixedbench.json` —— 路径开销原始数据（首测/修复版复测）
- `f1_raw_scores.txt` —— F1 违约 prompt 原始 score 复现（5 例）+ v1 图对照 + 源码证据摘录
- `env_evidence.txt` —— 硬件/系统/工具链/Python 环境/权重/F1 源码 grep/requirements 对照
- `ctest_evidence.txt` —— 隔离树 ctest 与定向测试（命令+退出码）
- `server_health_preload.txt` —— sam_server 启动命令/health 响应/预热耗时/两轮运行日志摘要（请求计数+首末行；完整日志 `/tmp/sam_bench_run/server{,_rerun}.log` 因 `.gitignore *.log` 规则不入库）

## 缺口与待办

| # | 缺口 | 责任/时机 |
|---|---|---|
| G1 | 「指定生产机台」的确认：本机（i9-12900K + RTX 3090 + 驱动 580.178.04）清单已补全；若另有生产机台，其清单+端到端基线按 README 命令执行 | 用户确认本机即目标机则 G1 关闭 |
| G2 | 真实工业测试图集（1×3000² + 细小缺陷样本） | 用户提供；阶段1 精度门禁复跑与生产验收硬依赖 |
| G3 | ~~CUDA 基线~~ **已关闭**（2026-09-29 补测，B 节：首次编码 395.1ms / 重复预测 P95 2700.1ms，合成图口径；真实图到位后复跑） | — |
| G4 | GUI 段时间计时（点选→候选显示） | 阶段3/4 接入时补测量设施 |
| G5 | F1 契约裁决（clamp vs 放宽） | 阶段2/3 设计决策，需复核方确认 |
| G6 | 阶段1 导出链环境（sam_env 有 torch 无 onnx；conda openpcdet/yolo 环境有 onnx 1.15/1.17 但无 segment_anything） | 阶段1 开工时确定导出环境并记录 |
