# DeepLux C++ 重构版能力基线对照矩阵（阶段 0.1）

> 本文是"hotfix 旧版 → C++ 重构版 → 目标平台"的能力对照基线，用于指导后续阶段的重构、迁移与验收。
>
> **对照来源**：hotfix 旧版 110 个插件目录已固定到 `qhchen-sz/DeepLux` 的
> `47d76c1225e9dba5cfd3674df54cc3327894839b`。逐项映射位于
> `docs/baseline/hotfix-plugin-mapping.json`，可由
> `python3 scripts/audit_hotfix_plugins.py` 重建。自动名称匹配只表示候选关系；参数、数据契约和
> 运行结果仍须在对应插件迁移阶段人工确认。

## 迁移决定枚举

| 决定 | 含义 |
| --- | --- |
| 保留 | 能力已可用，后续仅随 IModule ABI v2 统一重编，不改变职责。 |
| 重构 | 能力方向保留，但实现需重写（接口升级、强类型端口、执行语义修正等）。 |
| 替代 | 由新实现或新架构取代，旧实现不保留。 |
| 业务包 | 产线专用算法，移出通用核心，作为可选业务插件包交付。 |
| 淘汰 | 无对应目标能力或已被更优方案覆盖，计划移除。 |

## 状态枚举

| 状态 | 含义 |
| --- | --- |
| 可用 | process() 有真实实现，参数影响输出，已通过现有测试。 |
| 部分 | 有实现但存在参数不生效/能力子集/边界未覆盖。 |
| 实验性 | 明确标记实验性，未开放进入生产流程。 |
| 依赖硬件 | 需相机 SDK/设备/网络才可运行，带模拟器或契约测试前不可离线验收。 |

## 一、图像处理（9）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| GrabImage | com.deeplux.plugin.grabimage | 相机/文件/文件夹/演示采集；文件夹顺序+末尾循环 | 可用 | 待补 | 保留 | P0 |
| SaveImage | com.deeplux.plugin.saveimage | 按路径与质量保存图像 | 可用 | 待补 | 保留 | P1 |
| ShowImage | com.deeplux.plugin.showimage | 主视图显示图像 | 可用 | 待补 | 保留 | P1 |
| PerProcessing | com.deeplux.plugin.perprocessing | 滤波/锐化/形态学等预处理 | 部分 | 待补 | 重构 | P1 |
| Blob | com.deeplux.plugin.blob | 阈值+连通域，面积/圆度过滤 | 可用 | 待补 | 保留 | P1 |
| LoadPointCloud | com.deeplux.plugin.loadpointcloud | 加载点云/TIFF 深度图 | 可用 | 待补 | 保留 | P1 |
| DisplayData | com.deeplux.plugin.displaydata | 显示数据/文本叠加 | 部分 | 待补 | 重构 | P2 |
| ImageScript | com.deeplux.plugin.imagescript | 内置图像操作（反转/灰度/模糊/锐化），严格 0–3 校验，失败关闭 | 可用 | 待补 | 重构 | P2 |
| JigsawPuzzle | com.deeplux.plugin.jigsawsolver | 拼图切分 | 部分 | 待补 | 业务包 | P3 |

## 二、检测识别（7）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| FindCircle | com.deeplux.plugin.findcircle | Hough 找圆，半径/阈值/累加器参数 | 可用 | 待补 | 保留 | P0 |
| MeasureLine | com.deeplux.plugin.measureline | Hough 检线，长度/角度过滤 | 可用 | 待补 | 保留 | P1 |
| MeasureRect | com.deeplux.plugin.measurerect | 轮廓+最小外接矩形测量 | 可用 | 待补 | 保留 | P1 |
| Matching | com.deeplux.plugin.matching | 模板匹配 | 部分 | 待补 | 重构 | P1 |
| ColorRecognition | com.deeplux.plugin.colorrecognition | HSV 颜色识别（枚举校验，红色覆盖色相环两端，非法值失败关闭） | 可用 | 待补 | 重构 | P2 |
| QRCode | com.deeplux.plugin.qrcode | 仅 QR 码识别（条码未开放） | 部分 | 待补 | 保留 | P2 |
| JiErHanDefectsDet | com.deeplux.plugin.jierhandefectsdet | 实验性候选检测（边缘+轮廓启发式+置信度阈值过滤，非真实工业模型） | 实验性 | 待补 | 业务包 | P3 |

## 三、几何测量（9）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| MeasurementInput | com.deeplux.plugin.measurementinput | 点/线/面拾取输入适配器 | 可用 | 待补 | 保留 | P0 |
| DistancePP | com.deeplux.plugin.distancepp | 两点距离 | 可用 | 待补 | 保留 | P0 |
| DistancePL | com.deeplux.plugin.distancepl | 点到线距离 | 可用 | 待补 | 保留 | P1 |
| LinesDistance | com.deeplux.plugin.linesdistance | 两线段距离 | 可用 | 待补 | 保留 | P1 |
| MeasureGap | com.deeplux.plugin.measuregap | 间隙测量 | 可用 | 待补 | 保留 | P1 |
| FitLine | com.deeplux.plugin.fitline | RANSAC/LS 直线拟合 | 可用 | 待补 | 保留 | P1 |
| FitCircle | com.deeplux.plugin.fitcircle | RANSAC 圆拟合，半径过滤 | 可用 | 待补 | 保留 | P1 |
| PointSurfaceDistance | com.deeplux.plugin.pointsurfacedistance | 点到面距离（3D） | 可用 | 待补 | 保留 | P1 |
| FreeformSurface | com.deeplux.plugin.freeformsurface | 自由曲面采样 | 部分 | 待补 | 重构 | P2 |

## 四、坐标标定（1）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| N点标定 | com.deeplux.plugin.npointcalibration | 透视/仿射 N 点标定，输出尺寸控制 | 可用 | 待补 | 保留 | P1 |

## 五、相机驱动（3）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| Video4Linux2 Camera | com.deeplux.camera.v4l2 | Linux V4L2 采集 | 依赖硬件 | 待补 | 保留 | P1 |
| DirectShow Camera | com.deeplux.camera.directshow | Windows DirectShow 采集 | 依赖硬件 | 待补 | 保留 | P1 |
| Hikvision Camera | com.deeplux.camera.hikvision | 海康 MVS 采集（需 SDK） | 依赖硬件 | 待补 | 保留 | P2 |

> 相机能力按"先 SDK、模拟器、契约测试，再按真实项目加品牌驱动"推进；Basler/HikVision 插件源码未随仓库分发，对应 CMake 选项开启时给出明确错误。

## 六、通信（6）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| 串口通信 | com.deeplux.plugin.serialport | 串口读写，波特率/校验等 | 依赖硬件 | 待补 | 保留 | P1 |
| TCP客户端 | com.deeplux.plugin.tcpclient | TCP 客户端读写 | 可用 | 待补 | 保留 | P1 |
| TCP服务器 | com.deeplux.plugin.tcpserver | TCP 服务端 | 可用 | 待补 | 保留 | P2 |
| PLC读取 | com.deeplux.plugin.plcread | Modbus TCP 读寄存器 | 依赖硬件 | 待补 | 保留 | P1 |
| PLC写入 | com.deeplux.plugin.plcwrite | Modbus TCP 写寄存器 | 依赖硬件 | 待补 | 保留 | P1 |
| PLC通信测试 | com.deeplux.plugin.plccommunicate | Modbus 连通性测试 | 依赖硬件 | 待补 | 保留 | P2 |

## 七、逻辑控制（9）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| 条件分支 | com.deeplux.plugin.if | If/Else/End 条件分支 | 可用 | 待补 | 保留 | P0 |
| 条件判断 | com.deeplux.plugin.condition | 变量条件判断 | 可用 | 待补 | 保留 | P1 |
| 循环 | com.deeplux.plugin.loop | 固定次数循环 | 可用 | 待补 | 保留 | P1 |
| 条件循环 | com.deeplux.plugin.while | While 条件循环 | 可用 | 待补 | 保留 | P1 |
| 停止循环 | com.deeplux.plugin.stopwhile | 提前终止循环 | 可用 | 待补 | 保留 | P1 |
| 延时 | com.deeplux.plugin.delay | 可取消延时 | 可用 | 待补 | 保留 | P2 |
| 队列输入 | com.deeplux.plugin.queuein | 变量入队 | 部分 | 待补 | 重构 | P2 |
| 队列输出 | com.deeplux.plugin.queueout | 变量出队 | 部分 | 待补 | 重构 | P2 |
| 并行执行 | com.deeplux.plugin.parallel | 显式分支、受控线程池、all/any 汇合 | 可用 | 待补 | 重构完成 | P1 |

## 八、系统工具（8）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| SystemTime | com.deeplux.plugin.systemtime | 输出系统时间 | 可用 | 待补 | 保留 | P2 |
| Folder | com.deeplux.plugin.folder | 文件夹创建/枚举 | 可用 | 待补 | 保留 | P2 |
| SaveData | com.deeplux.plugin.savedata | 结果保存（CSV 等） | 部分 | 待补 | 重构 | P1 |
| WriteText | com.deeplux.plugin.writetext | 文本写文件 | 可用 | 待补 | 保留 | P2 |
| DataCheck | com.deeplux.plugin.datacheck | 数据范围/长度校验 | 部分 | 待补 | 重构 | P2 |
| ShowPoint | com.deeplux.plugin.showpoint | 点叠加显示 | 部分 | 待补 | 重构 | P2 |
| TableOutPut | com.deeplux.plugin.tableoutput | 表格输出 | 部分 | 待补 | 重构 | P2 |
| TimeSlice | com.deeplux.plugin.timeslice | 时间切片 | 部分 | 待补 | 重构 | P3 |

## 九、变量（6）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| 变量定义 | com.deeplux.plugin.vardefine | 定义全局变量 | 可用 | 待补 | 保留 | P1 |
| 变量赋值 | com.deeplux.plugin.varset | 变量赋值 | 可用 | 待补 | 保留 | P1 |
| 数学运算 | com.deeplux.plugin.math | 数学表达式计算 | 可用 | 待补 | 保留 | P1 |
| 创建字符串 | com.deeplux.plugin.createstring | 拼接字符串 | 可用 | 待补 | 保留 | P2 |
| 分割字符串 | com.deeplux.plugin.splitstring | 分割字符串 | 可用 | 待补 | 保留 | P2 |
| 字符串格式化 | com.deeplux.plugin.strformat | 模板格式化 | 可用 | 待补 | 保留 | P2 |

## 十、Hymson 3D（1）

| 插件 | 插件 ID | 当前能力 | 状态 | 旧版能力 | 迁移决定 | 优先级 |
| --- | --- | --- | --- | --- | --- | --- |
| DefectDetection | com.deeplux.plugin.defectdetection | 3D 点云缺陷检测 | 部分 | 待补 | 业务包 | P3 |

## 迁移决定汇总

| 迁移决定 | 数量 | 说明 |
| --- | ---: | --- |
| 保留 | 44 | 随 IModule ABI v2 统一重编，职责不变 |
| 重构 | 13 | PerProcessing、DisplayData、ImageScript、Matching、ColorRecognition、FreeformSurface、QueueIn、QueueOut、Parallel、SaveData、DataCheck、ShowPoint、TableOutPut（+TimeSlice 视阶段定） |
| 业务包 | 3 | JigsawPuzzle、JiErHanDefectsDet、DefectDetection |
| 替代 | 0 | 待与 hotfix 对齐后确认 |
| 淘汰 | 0 | 待与 hotfix 对齐后确认 |

> "部分/重构"状态仅为基于当前代码的初步判断，进入对应阶段前需逐个复核 process() 与参数契约测试结果。

## 53 个 missing 迁移范围冻结（阶段 1）

> 逐项决策见 `hotfix-plugin-mapping.json`（`migrationDecision`/`priority`/`evidence`）
> 与 `hotfix-plugin-mapping.md` 的"迁移范围决策"段；由 `testMigrationDecisionConsistency` 保证一致。
>
> **契约口径**：逐项记录中的 `input/output/keyParams/scenario` 均为**旧版契约**（按旧版 ViewModel 源码核验）；
> 目标 C++ 契约（强类型端口/载荷类型等）在阶段 7 另行设计评审，届时单独标注"目标输入/目标输出"，不与旧版证据混淆。

| 决策 | 数量 | 说明 |
| --- | ---: | --- |
| rebuild | 37 | 需真实算法+端口契约+参数验证+行为测试；仅阶段 1 审核批准且 P0/P1 者进入阶段 7 实现 |
| replace | 1 | CreatePoints→com.deeplux.plugin.measurementinput |
| retire | 5 | ShowChart/GreenRegion/Matching1/CSharpScript/RunProject，淘汰理由见 reason |
| business_pack | 10 | 015/016 业务+AI/模型类（AI/AIPost/Jigsaw/Solder/Yolo/BumpDent/LidWeld/HKSetOutPut/LightControl）+GSD（GapStepDetect.dll），依赖见 dependencies |

> **reviewState 与 migrationDecision 的区别**：`reviewState=pending`(53) 指这 53 项**没有当前候选实现可供等价性复核**（等价性维度未审）；
> 而 `migrationDecision`(rebuild37/replace1/retire5/business_pack10) 是**范围决策**（已冻结，决定该项去重建/替代/淘汰/业务包）。
> 两者是独立维度：范围决策完成 ≠ 等价性复核完成。rebuild 项的等价性将在阶段 7 实现后通过行为测试确认。

## 迁移前待办

1. 对映射中的 45 个 `direct` 和 5 个 `candidate` 逐项比对参数、端口和确定性结果。
2. ~~评审 53 个 `missing` 项~~ 已于阶段 1 冻结（rebuild37/replace1/retire5/business_pack10）。
3. 将业务包项从通用核心能力中分离并明确交付依赖：015/016 分类 7 项（`matchKind=business_pack`）与阶段 1 决策 10 项（`migrationDecision=business_pack`，含 GSD）。

## 阶段 2 复核：其余"部分"插件清单（只列不改）

> 阶段 2 已修复 ImageScript（删无效脚本输入+严格 0–3 校验+失败关闭）、
> JiErHanDefectsDet（阈值真实过滤+实验性标注）、ColorRecognition（颜色枚举校验+红色覆盖色相环两端）。
> 其余 13 个"部分"状态插件逐项复核如下；按约定本阶段只列出不修复，
> 修复随各插件的迁移决定进入阶段 7（仅 P0/P1 rebuild 项）或对应业务包验收。

| 插件 | 复核发现 | 处置 |
| --- | --- | --- |
| PerProcessing | processType 无枚举校验，非法类型静默直通返回成功（假成功）；sigmaX 仅高斯/双边生效、iterations 仅形态学生效、Canny 阈值 50/150 硬编码；metadata 无 max/_options，与 UI 范围（核 1–31）不同步 | 阶段 7 重构：枚举校验失败关闭+阈值参数化+metadata 同步 |
| DisplayData | doValidateParams 为空；displayText 为空且载体无命名数据时不绘制任何内容仍返回成功（空操作成功）；fontSize/position 仅 min 0 无 max，与 UI（8–100/0–2000）不同步 | 阶段 7 重构：参数范围校验+空文本语义 |
| Matching | 模板取自载体元数据键 template_qimage 而非正式端口；未提供模板时静默改用图像中心区域作模板（猜测行为）；metadata matchThreshold 仅 min 0 无 max，与校验 (0,1]、UI (0.1–1.0) 不同步 | 阶段 7 重构：模板端口化+metadata 同步 |
| QRCode | 条码分支（Code_128/Code_39/EAN_13/EAN_8）在 decodeBarCode 恒失败；校验已拒绝非 QR 类型，该分支不可达但仍存在；metadata codeType 用 options 而非统一 _options | 阶段 7：移除死分支或显式标注未实现 |
| FreeformSurface | samplingInterval 生效、点云必需；边界覆盖薄弱（少于 3 点/空点云/退化输入） | 阶段 7 重构：补边界行为测试 |
| QueueIn | dataVariable 为空回退固定键 item_data；任意类型一律 toString() 转换（结构化数据丢失） | 阶段 7 重构：类型化入队 |
| QueueOut | 队列空时返回成功，下游无法区分"队列空"与"取到数据"；peekOnly/outputVariable 生效 | 阶段 7 重构：空队列语义 |
| SaveData | JSON"追加"实为合并覆盖（同名键被覆盖）；CSV 追加跳过表头且不校验列序一致（错列风险） | 阶段 7 重构：追加语义 |
| DataCheck | Range/Length/Null 校验已实现且失败关闭；非法 checkType 未校验、静默输出 check_passed=false（与校验失败不可区分）；metadata minValue/maxValue 仅 min 0（无法表达负范围） | 阶段 7 重构：枚举校验+metadata 同步 |
| ShowPoint | markerSize/colorRGB 生效、缺点失败关闭；metadata 仅 min 0 无 max，与 UI 范围（颜色 0–255）不同步 | 阶段 7 重构：metadata 同步 |
| TableOutPut | rowCount/colCount 有范围校验（1–100/1–20）；数据超出表大小被静默截断且无告警输出 | 阶段 7 重构：截断语义显式化 |
| TimeSlice | mode 严格校验 Start/Stop/Reset；Stop 对上游 timeslice_start_time 缺失/非法均失败关闭；实现诚实 | P3 不进阶段 7（仅 P0/P1），维持现状观察 |
| DefectDetection | rcornerNormalDegree/rcornerCurvatureThreshold/minPoints 三个公开参数被 Q_UNUSED，不影响输出（假配置）；实际算法经 hymson3d detect_smooth_surface_dll | 业务包：修复假参数并现场验收 |

> 另：JigsawPuzzle（业务包）源码注释自认"暂时忽略碎片数据"，切分能力不完整，随业务包现场验收。
