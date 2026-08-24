# 旧版对照报告（收尾3）

> 旧版仓库：`qhchen-sz/DeepLux`，固定提交 `47d76c1225e9dba5cfd3674df54cc3327894839b`（C#）。
> 对照方法：解析旧版各插件 `ViewModels/*.cs` 中的 `AddOutputParam("名称","类型",...)` 提取输出端口，
> 与当前 C++ 插件 `metadata.json` 的 `ports.outputs` 逐项对照。

## 方法与局限

- 旧版仅以 `AddOutputParam` 显式定义**输出**端口（共 595 处，92 个 ViewModel），输入通过 `HImage`/变量链接，未显式声明，故本对照仅覆盖输出面。
- 旧版端口名为中文显示名（如"圆心X"），当前为英文 id + displayName，类型体系不同（`double`→`Number` 等），
  因此"端口对齐"按数量与语义近似判断，**不等同于逐值运行结果等价**。
- 结论：本对照可将明显"输出面收窄"的插件判定为 `partial`，但**无法仅凭端口对照升级为 `equivalent`**；
  `equivalent` 仍需逐值运行结果对照（超出本轮静态对照范围）。

## 汇总

- 对照插件总数：50
- 旧版成功提取输出端口的插件：35
- 当前找到输出端口的插件：50

| 判定 | 数量 |
| --- | ---: |
| 旧版无显式输出，无法对照 | 15 |
| 输出面收窄（partial） | 29 |
| 输出面相当，语义待确认（partial） | 6 |

## 逐项对照

| 旧版插件 | 当前候选 | 匹配 | 旧版输出数 | 当前输出数 | 旧版输出（前 8） | 当前输出 id（前 8） | 判定 |
| --- | --- | --- | ---: | ---: | --- | --- | --- |
| Delay | Delay | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| Blob | Blob | direct | 13 | 2 | 区域, 区域个数, 区域总面积, 面积, X, Y, 圆度, 矩形度 | image, blob_count | 当前输出数(2)<旧版(13)，输出面收窄 |
| DiplayData | DisplayData | candidate | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| GrabImage | GrabImage | direct | 2 | 1 | 图像, 亮度图 | image | 当前输出数(1)<旧版(2)，输出面收窄 |
| ImageScript | ImageScript | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| JigsawPuzzle | JigsawPuzzle | direct | 2 | 1 | 区域, 截取图像 | image | 当前输出数(1)<旧版(2)，输出面收窄 |
| PerProcessing | PerProcessing | direct | 1 | 1 | 预处理图像 | image | 当前输出数(1)≥旧版(1)，覆盖面相当，语义待逐项确认 |
| SaveImage | SaveImage | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| ShowImage | ShowImage | direct | 1 | 1 | 图像 | image | 当前输出数(1)≥旧版(1)，覆盖面相当，语义待逐项确认 |
| BarcodeReader | QRCode | candidate | 3 | 2 | 条形码信息, 状态, 时间 | image, qr_result | 当前输出数(2)<旧版(3)，输出面收窄 |
| ColorRecognition | ColorRecognition | direct | 5 | 1 | X, Y, 识别面积, 区域, 掩膜图像 | image | 当前输出数(1)<旧版(5)，输出面收窄 |
| FindCircle | FindCircle | direct | 7 | 5 | 测量圆, 圆心X, 圆心Y, 半径, 直径, 状态, 时间 | image, circle_center_x, circle_center_y, circle_radius, circle_score | 当前输出数(5)<旧版(7)，输出面收窄 |
| Matching | Matching | direct | 7 | 1 | X, Y, Deg, 角度, 分数, 匹配数量, 匹配结果列表 | image | 当前输出数(1)<旧版(7)，输出面收窄 |
| MeasureLine | MeasureLine | direct | 7 | 3 | 测量直线, 中心X, 中心Y, 角度, 状态, 时间, 直线度 | image, line_length, line_angle | 当前输出数(3)<旧版(7)，输出面收窄 |
| MeasureRect | MeasureRect | direct | 9 | 4 | 测量矩形, 中心X, 中心Y, 长边l1, 短边l2, 角度(Phi), 角度(Deg), 状态 | image, rect_width, rect_height, rect_area | 当前输出数(4)<旧版(9)，输出面收窄 |
| QRCode | QRCode | direct | 1 | 2 | 二维码信息 | image, qr_result | 当前输出数(2)≥旧版(1)，覆盖面相当，语义待逐项确认 |
| DistanceLL | LinesDistance | candidate | 3 | 2 | 距离, 状态, 时间 | image, distance | 当前输出数(2)<旧版(3)，输出面收窄 |
| DistancePL | DistancePL | direct | 5 | 2 | 距离, 垂点X, 垂点Y, 状态, 时间 | image, distance | 当前输出数(2)<旧版(5)，输出面收窄 |
| DistancePP | DistancePP | direct | 6 | 2 | 距离, 角度, 中心点X, 中心点Y, 状态, 时间 | image, distance | 当前输出数(2)<旧版(6)，输出面收窄 |
| LinesDistance | LinesDistance | direct | 3 | 2 | 距离, 状态, 时间 | image, distance | 当前输出数(2)<旧版(3)，输出面收窄 |
| FitCircle | FitCircle | direct | 3 | 5 | 中心X, 中心Y, 中心R | image, circle_center_x, circle_center_y, circle_radius, circle_error | 当前输出数(5)≥旧版(3)，覆盖面相当，语义待逐项确认 |
| FitLine | FitLine | direct | 5 | 8 | 拟合直线, 起点X, 起点Y, 终点X, 终点Y | image, line_row1, line_col1, line_row2, line_col2, line_phi, line_rho, line_error | 当前输出数(8)≥旧版(5)，覆盖面相当，语义待逐项确认 |
| MeasureCalib | NPointCalibration | candidate | 3 | 1 | 像素当量, 状态, 时间 | image | 当前输出数(1)<旧版(3)，输出面收窄 |
| NPointCal | NPointCalibration | candidate | 3 | 1 | MHomMat2DTransl, mRotateCenterX, mRotateCenterY | image | 当前输出数(1)<旧版(3)，输出面收窄 |
| If | If | direct | 0 | 3 | - | image, true, false | 旧版未定义显式输出端口，无法端口级对照 |
| Parallel | Parallel | direct | 0 | 2 | - | image, branch | 旧版未定义显式输出端口，无法端口级对照 |
| StopWhile | StopWhile | direct | 0 | 2 | - | image, stop | 旧版未定义显式输出端口，无法端口级对照 |
| While | While | direct | 4 | 3 | 索引, 索引Double, 进度, 当前值 | image, body, done | 当前输出数(3)<旧版(4)，输出面收窄 |
| DataCheck | DataCheck | direct | 3 | 1 | 状态, 时间, 总结果 | image | 当前输出数(1)<旧版(3)，输出面收窄 |
| Folder | Folder | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| SaveData | SaveData | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| ShowPoint | ShowPoint | direct | 4 | 1 | X, Y, 状态, 时间 | image | 当前输出数(1)<旧版(4)，输出面收窄 |
| SystemTime | SystemTime | direct | 8 | 1 | 年, 月, 日, 时, 分, 秒, 毫秒, 文本 | image | 当前输出数(1)<旧版(8)，输出面收窄 |
| TableOutPut | TableOutPut | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| TimeSlice | TimeSlice | direct | 3 | 1 | 状态, 片段时间, 时间 | image | 当前输出数(1)<旧版(3)，输出面收窄 |
| WriteText | WriteText | direct | 2 | 1 | 状态, 时间 | image | 当前输出数(1)<旧版(2)，输出面收窄 |
| CreateString | CreateString | direct | 3 | 1 | 结果文本, 状态, 时间 | image | 当前输出数(1)<旧版(3)，输出面收窄 |
| QueueIn | QueueIn | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| QueueOut | QueueOut | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| SplitString | SplitString | direct | 2 | 1 | 状态, 时间 | image | 当前输出数(1)<旧版(2)，输出面收窄 |
| StrFormat | StrFormatPlugin | direct | 1 | 1 | 格式化字符串 | image | 当前输出数(1)≥旧版(1)，覆盖面相当，语义待逐项确认 |
| VarDefine | VarDefinePlugin | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| VarSet | VarSetPlugin | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| PLCCommunicate | PLCCommunicatePlugin | direct | 0 | 1 | - | image | 旧版未定义显式输出端口，无法端口级对照 |
| PLCRead | PLCReadPlugin | direct | 5 | 1 | 读取int值, 读取double值, 状态, 时间, 总读取 | image | 当前输出数(1)<旧版(5)，输出面收窄 |
| PLCWrite | PLCWritePlugin | direct | 4 | 1 | 读取int值, 读取double值, 状态, 时间 | image | 当前输出数(1)<旧版(4)，输出面收窄 |
| FreeformSurface | FreeformSurface | direct | 2 | 1 | 状态, 时间 | image | 当前输出数(1)<旧版(2)，输出面收窄 |
| JiErHanDefectsDet | JiErHanDefectsDet | direct | 5 | 1 | 状态, 时间, 缺陷Flag, 缺陷数量, 缺陷总面积 | image | 当前输出数(1)<旧版(5)，输出面收窄 |
| MeasureGap | MeasureGap | direct | 7 | 2 | 缝隙宽度, 状态, 时间, ROI, 检测十字, 检测轮廓, 检测线条 | image, gap | 当前输出数(2)<旧版(7)，输出面收窄 |
| PointSurfaceDistance | PointSurfaceDistance | direct | 3 | 2 | 缝隙宽度, 状态, 时间 | image, distance | 当前输出数(2)<旧版(3)，输出面收窄 |

## 对映射结论的影响

- 所有 `direct`/`candidate` 插件维持 `partial`（当前存在候选实现，旧版等价未证明）。
- 本轮静态对照**不升级**任何插件到 `equivalent`；其中"输出面收窄"的插件在证据中补充端口对照结果，
  作为后续补齐输出端口或确认有意收窄的依据。
- 旧版无显式输出端口的插件（如纯逻辑/变量类），端口级对照不适用，维持 `partial`。
