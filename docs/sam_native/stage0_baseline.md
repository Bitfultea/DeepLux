# SAM 原生推理计划 · 阶段0 基线报告

## 结论摘要（阶段0 状态：**部分完成，CUDA 基线项未通过**）

| 项 | 状态 |
|---|---|
| 性能脚本（同图 100 次混合 prompt，首次编码/重复预测分测） | ✅ `tools/sam_bench` + 配方固化 + 参数守卫回归 16/16 |
| 开发机真实 ViT-B **CPU** 基线（现有 Python HTTP 路径） | ✅ 原始 P50/P95 见下，JSON 入库 |
| 路径开销基线（内嵌 stub，无 Python） | ✅ P95 0.43ms |
| 隔离脏工作区 | ✅ 34 个用户 WIP 文件未混入；阶段0 仅提交 bench/构建接入/文档 |
| 基线提交 + 定向测试 + 全量 CTest 证据 | ✅ c2ee793；隔离树 75/76（唯一失败为既有 test_runengine 环境项）+ SAM 定向 9 套件全绿 |
| **CUDA 基线** | ❌ **取不到**：RTX 3090 在 PCI 总线可见但驱动未加载（nvidia-smi 失败、torch.cuda.is_available()=False）→ 按计划书如实标记**阶段0（该项）未通过**，待目标机驱动可用后补测 |
| 目标机（生产机台）清单 | ❌ 未提供（缺口 G1） |
| 原版 SAM 测试图集（真实工业图） | ❌ 本机缺失（缺口 G2）→ 基线用确定性合成图 v2 替代 |

## 基线提交与工作区隔离

- 基线提交：`c2ee793`（revert 批次，树与批4 交付 `c55d290` 逐字节一致，`git diff` 为空）
- 当前工作区含用户未提交 WIP 34 文件（AgentActor 重构、connect_modules 多端口、Agent 聊天 UI 重构、SAM 对话框下拉框改版等）——阶段0 提交**仅**包含：`tools/sam_bench/*`（新增）、顶层 `CMakeLists.txt`（+2 行 add_subdirectory）、`docs/sam_native/*`（新增）。共享文件零触碰。
- 隔离树验证：`/tmp/dl_verify_b4` @ c55d290（构建 EXIT=0），测试证据见 `stage0/ctest_evidence.txt`。

## 硬件与系统清单（开发机=当前执行机；生产机台=G1 待补）

- CPU：Intel i9-12900K（16C/24T）；内存 62 GiB
- OS：Ubuntu 22.04.5 LTS，kernel 6.8.0-138-generic，x86_64
- GPU：**NVIDIA GeForce RTX 3090（GA102，lspci 0000:01:00.0 在位）——驱动未加载**：`nvidia-smi` 报 "couldn't communicate with the NVIDIA driver"；sam_env torch 2.13.0+cu130 `cuda_available=False`
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
- 性能口径只依赖分辨率（ViT 输入统一 resize 1024²），合成图 CPU 基线对生产机有效；**内容相关的精度门禁（阶段1）必须换真实图集**

## 性能脚本与口径

- 工具：`tools/sam_bench`（走产品同款 `SamBackendClient` HTTP 路径；口径=HTTP 客户端至预测结果，**不含 GUI 候选显示段**——0.5s 交互目标须另测 GUI 段，见 G4）
- 固定配方：`i%10` → 0–3 单正点(40%) / 4–6 正+负点(30%) / 7–9 框选(30%)；坐标纯整数取模，跨机台一致（README 有完整公式，禁止改动）
- 失败策略：默认 `--on-error fail`（产品语义失败关闭）；基线采集用 `--on-error continue`（F1 违约样本记录进 JSON `violations`，不计入延迟统计，连续 5 次失败仍中止）
- 参数守卫回归：`check_arg_guards.sh` **16/16**（含 `--warmup abc` 的 ok 复用回归——修复了四个数值参数共享单个 `ok` 被后续成功解析覆盖的缺陷）
- 真实后端操作规程：先 `GET /health` 预热模型（本次 1664ms 至 ready），再跑 bench，避免模型加载计入首次编码（`stage0/server_health_preload.txt`）

## 基线原始数据

### A. 真实 ViT-B CPU（现有 Python sam_server HTTP 路径）

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

**解读**：与 0.5s 目标差 ~5.8×（P95 2885ms）。结合 C 项（客户端+HTTP 路径开销 <1ms），耗时 >99.9% 在服务端：ViT-B CPU 解码器推理 + `sam_server` 对 3 个 3000² 候选掩膜的**纯 Python 逐像素 `_encode_rle` + 全尺寸 PNG base64 编码**（F2）。CPU 路径按计划不承诺 0.5s；该基线的意义是量化旧路径并为阶段2/4 提供对照。

### B. CUDA（目标机）

**未获取**——本机驱动未加载（F5）。阶段0 该项**未通过**。目标机命令模板（README）：`sam_bench --server <生产 sam_server> --image <真实3000²图> --runs 100 --warmup 3 --json … --tag …`。

### C. 路径开销（内嵌 stub，无 Python；同合成图 v2，runs=100 warmup=3，on-error=fail）

首次编码 0.696 ms；预测 P50 0.2 / **P95 0.43** / max ~0.6 ms；有效 100/100，违约 0。
→ `SamBackendClient`+HTTP+JSON 严格解析（含 3 候选）全链开销 **<1ms**，不是瓶颈。原始 JSON：`stage0/stage0_dev_stub_synth3000.json`

## 关键发现与风险

- **F1（阻塞级，需阶段2/3 裁决）**：官方 segment_anything `MaskDecoder.iou_prediction_head` 为纯 MLP，**无 sigmoid/clamp**（`sigmoid_output=False` 仅作用于 mask 分支；源码证据见 `stage0/env_evidence.txt` 末节），IoU score 可 >1（实测违约 prompt 原始值 1.000425、1.001420；v1 图 1.0134/1.0138）。`sam_server` 直接透传 `float(scores[best])`；DeepLux 批3 严格契约 score∈[0,1] 拒绝整次预测 → **现有生产路径对真实模型输出以约 5%（本图集）概率失败关闭**。候选 score 同样受影响（批4 candidates 严格解析同口径）。裁决选项：服务端/导出侧 clamp（改变 score 语义）vs 客户端契约放宽（有限值 + clamp 用于展示、保留原值排序）。**阶段1「分数绝对差≤0.01」门禁须以未 clamp 原始值定义对比口径**。
- **F2**：旧 CPU 路径 predict ~2.8s 的大头是 `sam_server._encode_rle` 纯 Python 逐像素循环（3000²×3 候选≈27M 像素迭代）+ 全尺寸掩膜 PNG base64；阶段2 原生实现的明确优化空间（RLE C++ 化/掩膜按需编码）。
- **F3**：sam_env 版本漂移超出 requirements.txt 锁定（见上）。
- **F4**：隔离树 `test_runengine::testMultipleBreakpointsCanResumeAndStop` 崩溃为既有环境/布局敏感项（基线 c51dce3 隔离树同签名 28/1、主工作区 76/76 通过、复核方历轮无法复现）——非阶段0 引入。
- **F5**：GPU 驱动未加载（卡在位）——CUDA 基线与阶段4 的前置阻塞；需装/修驱动或换目标机。
- **F6**：bench 两次 fail 模式全量运行均正确失败关闭（v1 图 predict[0]、v2 图 predict[23]，exit 4、无部分 JSON）——工具失败语义实测有效。

## 测试证据（命令 + 退出码）

- 隔离树 @ c55d290（≡c2ee793）：`ctest` → **exit 8**，75/76（唯一失败 test_runengine，单跑 exit 134，28 passed/1 failed）；SAM 定向 9 套件全绿 exit 0（25/16/57/16/12/15/8/5/8）——明细 `stage0/ctest_evidence.txt`
- 主工作区（含用户 WIP）：全量 CTest **76/76**（阶段0 提交后复跑，见提交信息）
- bench：arg guards **16/16**（exit 0）；stub 冒烟 runs=10 exit 0；真实后端 probe runs=10 exit 0；全量 fail 模式 exit 4×2（F6）；continue 模式 exit 0
- sam_server.py 本阶段零改动

## 证据文件清单（docs/sam_native/stage0/）

- `stage0_cpu_vitb_synth3000.json` —— CPU 真实基线原始数据（逐次延迟数组+violations）
- `stage0_dev_stub_synth3000.json` —— 路径开销原始数据
- `f1_raw_scores.txt` —— F1 违约 prompt 原始 score 复现（5 例）+ v1 图对照 + 源码证据摘录
- `env_evidence.txt` —— 硬件/系统/工具链/Python 环境/权重/F1 源码 grep/requirements 对照
- `ctest_evidence.txt` —— 隔离树 ctest 与定向测试（命令+退出码）
- `server_health_preload.txt` —— sam_server 启动命令/health 响应/预热耗时/运行日志摘要（请求计数+首末行；完整日志 `/tmp/sam_bench_run/server.log` 因 `.gitignore *.log` 规则不入库）

## 缺口与待办

| # | 缺口 | 责任/时机 |
|---|---|---|
| G1 | 生产机台清单（GPU/驱动/系统）+ 生产机端到端基线 | 用户提供机台后按 README 命令执行 |
| G2 | 真实工业测试图集（1×3000² + 细小缺陷样本） | 用户提供；阶段1 精度门禁硬依赖 |
| G3 | **CUDA 基线 → 阶段0 该项未通过** | 目标机驱动可用后补测（F5） |
| G4 | GUI 段时间计时（点选→候选显示） | 阶段3/4 接入时补测量设施 |
| G5 | F1 契约裁决（clamp vs 放宽） | 阶段2/3 设计决策，需复核方确认 |
| G6 | 阶段1 导出链环境（sam_env 有 torch 无 onnx；conda openpcdet/yolo 环境有 onnx 1.15/1.17 但无 segment_anything） | 阶段1 开工时确定导出环境并记录 |
