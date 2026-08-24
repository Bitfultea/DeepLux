# 阶段 H 交接报告：A→H 全轮能力建设

> 日期：2026-08-24
> 分支：`main`；测试基线：`ctest` **64/64 通过**；`git diff --check` 干净。
> 本报告为本轮（阶段 A→H）交接文档，汇总各阶段交付、验证与遗留。

## 一、总体结论

本轮完成从基线冻结到插件收口的全链路能力建设：**阶段 A–G 主体交付**，
阶段 H（本验收）确认测试基线、证据链与遗留清单。

- 测试：**64/64** 全部通过（较上轮 58 新增 6 个插件行为级测试文件）。
- 格式：全部改动 C++ 文件 `clang-format --dry-run --Werror` 0 违规。
- 证据：截图、TSan、旧版对照、hotfix 映射均已落盘并可重建。

## 二、各阶段交付摘要

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| A | 基线冻结、能力矩阵、hotfix 映射、状态文档 | 完成 |
| B | 工程 3.0 模块字段 note/enabled/breakpoint | 完成 |
| C | 显式控制边契约（端口兼容/四元组去重/环检测） | 完成 |
| D | 显式控制图执行（D1 顺序+If、D2 循环、D3 删旧路径） | 完成 |
| E | 多输入聚合（multiple）+ controlJoinPolicy + Parallel 接入主循环 + blocking 排除 | 完成 |
| F | 画布端口级交互（字符串端口/四元组连接/拖线/数据控制边样式） | 完成 |
| G | 插件收口（13 重构插件行为测试 + execution 标记 + 旧版对照 + 业务包依赖） | 部分完成（见遗留） |
| H | 本验收 + 交接报告 | 完成 |

## 三、关键验证证据

### 3.1 测试基线
```
ctest --test-dir build --output-on-failure  →  64/64 通过
```
新增插件行为级测试（阶段 G）：`test_timeslice`(11)、`test_queueplugins`(11)、
`test_systemplugins`(16)、`test_imageplugins`(8)、`test_remainingplugins`(12)、`test_finalplugins`(10)。
覆盖：参数影响结果 / 结构化错误 / clone 独立 / 确定性正常+失败样例。

### 3.2 正式尺寸截图（收尾2）
- 环境可离屏渲染；产出 `docs/baseline/screenshots/formal_{1920,1280}_{dark,light}.png` 共 4 张。
- 深/浅主题经"切换主题"菜单动作切换，像素差异已验证（深色均值~40、浅色~247）。

### 3.3 TSan 并发验证（收尾1）
- 已实际执行：`build-tsan`（Qt5.15.3 + `-fsanitize=thread`），`test_runengine` TSan 下 97/97 通过。
- **环境级限制**：内核 6.8 ASLR 与 gcc11 TSan 冲突，最小程序也报 `FATAL: unexpected memory mapping`；
  需 `setarch -R` 关 ASLR 才可运行（已验证最小程序能检出植入竞争）。
- 检出 20 处 data race 警告，集中于并行路径，多为未插桩 Qt5（QMutex/QHash）误报。
- **判定：警告未清零，不标"通过"**。详见 `tsan-report.md` 与 `tsan-runengine-full.txt`。

### 3.4 旧版对照（收尾3）
- 克隆旧版 `qhchen-sz/DeepLux` 固定提交 `47d76c12`（C#），解析 `AddOutputParam`（595 处/92 文件）。
- 对照全部 50 个 matched 插件输出端口：**29 输出面收窄 / 6 相当 / 15 旧版无显式输出**。
- 详见 `legacy-comparison.md`；结论写入 50 个映射项 evidence，均维持 `partial`。
- **静态端口对照不能证明逐值运行等价，故未升级任何插件到 `equivalent`。**

### 3.5 hotfix 映射（可重建）
- `scripts/audit_hotfix_plugins.py` 以 `legacyPath` 为键保留审核状态；候选身份变化则回退 `pending`。
- 结论分布：`partial` 46 / `unverified`（硬件）4+7 业务包；`equivalent` 0（未做逐值等价）。

## 四、遗留清单（如实）

| 项 | 说明 | 阻塞性质 |
| --- | --- | --- |
| 逐值运行结果等价核验 | 旧版对照仅静态端口级；`equivalent` 需逐值运行对照 | 待办（非外部阻塞） |
| 13 插件中 29 项输出面收窄 | 当前输出端口少于旧版，需确认有意收窄或补齐 | 待办 |
| TSan 警告清零 | 需以 TSan 重编 Qt5 复测，或逐条确认潜在真实竞争 | 待办 |
| 7 个 business_pack | 依赖现场硬件/协议，本环境不可验证 | 外部阻塞（已记录依赖） |
| 阶段 1 复杂载荷类型 | Mask/Region/DetectionList 仍过渡性宽松类型 | 待办 |

## 五、复现命令

```
# 构建与测试
cmake -B build -S . && cmake --build build -j && ctest --test-dir build --output-on-failure

# TSan（需关 ASLR）
cmake -B build-tsan -S . -DCMAKE_BUILD_TYPE=Debug -DUSE_QT6=OFF \
  -DQt5_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt5 \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --target test_runengine
setarch $(uname -m) -R build-tsan/bin/test_runengine

# 截图
QT_QPA_PLATFORM=offscreen build/bin/ui_capture_mainwindow <output-dir>

# hotfix 映射重建（保留审核状态）
python3 scripts/audit_hotfix_plugins.py
```

## 六、关键文档索引

- `capability-matrix.md` — 能力矩阵
- `hotfix-plugin-mapping.json` / `.md` — 旧版插件映射与审核结论
- `legacy-comparison.md` — 旧版输出端口静态对照
- `tsan-report.md` / `tsan-runengine-full.txt` — TSan 验证证据
- `screenshots/formal_*.png` — 正式尺寸截图
- `round-todo.md` — 本轮待办与状态
