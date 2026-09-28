# sam_bench —— SAM 性能基线工具（原生推理计划·阶段0）

固定「同图 N 次混合 prompt」测量口径的性能工具，走产品同款
`SamBackendClient` HTTP 路径。三类使用场景：

1. **阶段0 基线**：生产机/开发机对现有 Python `sam_server`（真实 ViT-B）测
   首次编码与重复预测的 P50/P95 原始数据。
2. **阶段2/4 对照**：原生 C++ 推理接入后，同口径复测并对比（后续阶段为
   本工具增加本地原生模式）。
3. **工具自检**：`--embedded-stub` 内嵌 `SamTestServer`（无 Python 依赖），
   测得值为客户端+HTTP+JSON 路径开销（推理≈0），**不代表模型性能**。

## 构建

```bash
cmake --build build --target sam_bench
```

## 用法

```bash
# 真实后端（生产机基线；口径=HTTP 客户端至预测结果，不含 GUI 段，见「测量口径」）
./build/bin/sam_bench --server http://127.0.0.1:8990 \
    --image /path/to/real_3000x3000.png \
    --runs 100 --warmup 3 --timeout 120000 \
    --json stage0_prod_baseline.json --tag "prod-rtx3090-vitb"

# 无 Python 自检（路径开销）
./build/bin/sam_bench --embedded-stub \
    --gen-image /tmp/sam_bench_synth_3000.png --size 3000 \
    --runs 100 --json stage0_dev_stub.json --tag "dev-stub-path-overhead"
```

退出码：`0` 成功；`2` 参数错误；`3` 图像/输出文件失败；`4` 后端错误或超时
（失败关闭：不输出部分 JSON）；`5` 内部计时不变量违背（predict 完成后又
收到 embeddingReady——工具缺陷守卫）。

`--on-error fail|continue`（默认 fail）：`continue` **仅**放行明确识别的
F1 score 越界（错误串含「score 必须为 [0,1] 有限数值」，覆盖顶层与
candidates[i] 同口径拒绝）——真实 SAM 的 IoU score 可 >1，超出批3 严格
契约 [0,1]，客户端拒绝该次预测。违约样本记录进 JSON `violations`
（index/kind/error/measured/classification=F1-score-out-of-range），
**不计入延迟统计**（拒绝发生在 mask PNG 解码前，延迟不可比）。
网络错误、超时及其他任何解析失败**无论模式一律失败关闭 exit 4**，
不得混入违约统计（复核二轮 P2-4）。`setImage` 失败任何模式下都失败关闭。
延迟统计始终只包含完整走通产品路径（严格解析成功）的样本；`runsValid`
与 `violationCount` 如实报告覆盖度。

计时口径（复核二轮 P1-2）：完成信号按操作类型隔离——`set_image` 只以
`embeddingReady` 收口；`predict` 只以 `predictionReady` 收口。predict
期间出现 `embeddingReady` 只能来自客户端对 `invalid_embedding` 的自动
重试链（重 setImage → 重发 predict），记入 JSON `retryEvents` 并继续等待
真正的预测结果，不得提前结束计时。

注入自检（仅 `--embedded-stub`）：
- `--inject-invalid-embedding N`：前 N 次 /predict 返回 invalid_embedding，
  驱动客户端自动重试链，验证上述计时口径；
- `--inject-predict-error`：/predict 持续返回 {"status":"error"}（非 F1
  口径），验证 continue 模式对非 score 越界错误同样失败关闭。
回归脚本：`check_retry_regression.sh`（重试计时 + continue 收窄，5 用例）
与 `check_arg_guards.sh`（参数守卫，20 用例）。

## 测量口径

**口径边界**：本工具测的是 **HTTP 客户端至预测结果**——`SamBackendClient`
发起请求 → 收到并解析 `predictionReady`（含候选解析）为止。**不包含**
GUI 段（overlay 渲染、候选循环行显示等）。计划的 0.5 秒交互目标口径是
「点选/框选 → 候选显示」，其中 GUI 段须由后续阶段的界面计时另行测量；
**本工具的数据不能单独证明 0.5s 交互目标达标**，只能证明推理+传输段
占用多少预算。

- **首次编码** `initialEncodeMs`：`set_image → embeddingReady` 全程
  （真实后端含服务器读图 + ViT 编码；同图第二次起服务器仍会重新编码——
  现有 sam_server 无 embedding 跨请求缓存，原生阶段的缓存收益以此为对照）。
- **重复预测** `latenciesMs[]`：同一 embedding 上逐次
  `predict → predictionReady` 耗时，报告 P50/P95/P99/max/mean（nearest-rank：
  `idx = ceil(p/100·N) − 1`）及分组细分。
- `--warmup N`：计时前的额外预测（不计入统计，JSON 记录 warmupStats）。
- **网络/后端失败的失败关闭**（超时、连接拒绝、畸形响应）不在本工具内
  验证——由 `tests/test_sambackendclient`（超时/网络失败/严格解析用例）
  与 `tests/test_agent_sam_tools`（服务不可达失败关闭）覆盖；本工具仅
  保证任何失败 → 非零退出码且不输出部分 JSON。参数级守卫回归见
  `check_arg_guards.sh`。

## 确定性 prompt 配方（禁止改动，跨机台/跨阶段可比）

`runs+warmup` 个 prompt，第 `i` 个按 `i % 10` 分组：

| i%10 | 类型 | 构成 |
|---|---|---|
| 0–3 | `pos`（40%） | 单正点 `(100+(i·127) mod (w−200), 100+(i·311) mod (h−200))` |
| 4–6 | `posneg`（30%） | 同上正点 + 负点（以 `i+runs` 代入同式） |
| 7–9 | `box`（30%） | 框 `(50+(i·211) mod (w−w/8−100), 50+(i·97) mod (h−h/8−100), w/8, h/8)` |

全部为整数运算，同尺寸图像在任何机台生成完全相同的坐标序列。

## 合成基准图（--gen-image）

v2 内容：低频平滑底 `v=96+((x/32+y/48) mod 64)` + 12×12 均匀灰度单元
（灰度 `30+((ix·31+iy·57) mod 200)`，2px 暗边界）+ 每单元中心 1/4 边长
对比小块，仅由 `--size` 整数运算决定，跨平台逐像素一致。v1 的高频斜纹底
会诱导真实 SAM 输出退化全图掩膜与越界 score（实测 1.0134>1，见基线文档
发现 F1），已废弃。**用途限于路径开销自检与无真实图机台的流程基线**；
阶段1 精度门禁必须使用真实工业图像集（见
`docs/sam_native/stage0_baseline.md` 的图集规格与缺口清单）。

## JSON 输出字段

`tool/toolVersion/tag/mode/serverUrl/modelName/embeddingId`、
`environment{os,kernel,arch,qt}`、`image{path,width,height,sha256}`、
`runs/runsValid/warmup/timeoutMs/promptRecipe`、`initialEncodeMs`、
`firstCandidates/lastCandidates/firstScore/lastScore`、
`latenciesMs[]`（原始数据）、`predictStats`、`predictStatsByKind{pos,posneg,box}`、
`warmupStats`、`onError/injectedInvalidEmbedding/injectedPredictError/retryEvents`、
`violationCount/violations[]{index,kind,error,measured,classification}`。
`image.sha256` 用于跨机台核对「同图」。
