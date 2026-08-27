# Hotfix 插件映射清单

- 旧版仓库：`qhchen-sz/DeepLux`
- 固定提交：`47d76c1225e9dba5cfd3674df54cc3327894839b`
- 生成命令：`python3 scripts/audit_hotfix_plugins.py`
- 说明：`direct` 仅代表规范化名称相同；它不是参数、数据契约或运行结果等价的证明。
- 重跑脚本会保留已有 `reviewState`/`reviewConclusion`/`evidence` 字段（见 `load_existing_reviews`）。

| 状态 | 数量 | 含义 |
| --- | ---: | --- |
| direct | 45 | 名称直接匹配，仍需人工核验能力和参数 |
| candidate | 5 | 需要确认的历史别名或替代候选 |
| missing | 53 | 当前没有候选实现 |
| business_pack | 7 | 业务专用插件，作为可选业务包评审 |

| 审核状态 | 数量 |
| --- | ---: |
| reviewed | 50 |
| dependency_recorded | 7 |
| pending | 53 |

- 审核结论统计仅包含 `reviewState=reviewed` 的条目。

| 审核结论 | 数量 | 含义 |
| --- | ---: | --- |
| equivalent | 0 | 已证明与旧版能力等价（需旧版参数/端口/结果对照） |
| intentionally_changed | 12 | 已确认采用不同于旧版的契约或行为 |
| partial | 34 | 当前存在候选实现，旧版等价未证明 |
| unverified | 4 | 依赖硬件/SDK，行为未验证 |
| not_equivalent | 0 | 不等价 |

## 迁移范围决策（missing 项，阶段 1 冻结）

| 决策 | 数量 | 含义 |
| --- | ---: | --- |
| rebuild | 32 | 需重建：真实算法+端口契约+参数验证+行为测试 |
| replace | 4 | 由当前已有能力/流程替代 |
| retire | 8 | 淘汰：无产品需求或已被覆盖 |
| business_pack | 9 | 业务包：依赖硬件/模型，需现场验收 |
| pending | 0 | 未决策 |

| 旧版插件 | 分类 | 决策 | 优先级 | 证据/替代/理由 |
| --- | --- | --- | --- | --- |
| ShowChart | 000常用工具 | replace | P2 | 替代流程：曲线/图表展示由主视图 MeasurementOverlay 与检查器结果页承担；无独立图表插件不阻塞核心测量 |
| AreaSpray | 001图像处理 | retire | P3 | 淘汰理由：区域喷洒为旧版可视化辅助，无测量语义；当前主视图叠加已覆盖显示需求 |
| CameraReadyWait | 001图像处理 | replace | P2 | 替代流程：相机就绪等待由 GrabImage 的 grabTimeout+重试承担；无需独立等待插件 |
| CropImage | 001图像处理 | rebuild | P1 | 输入:Image2D+ROI；输出:裁剪后 Image2D；关键参数:x/y/w/h；场景:感兴趣区域截取后送检测/测量 |
| DepthToGray | 001图像处理 | replace | P2 | 替代流程：深度→灰度归一化可由现有 ImageData 灰度转换承担；非独立算法 |
| ImageMerge | 001图像处理 | rebuild | P2 | 输入:多 Image2D；输出:融合 Image2D；关键参数:融合方式(加/平均/最大)；场景:多帧/多通道融合 |
| ImageOperation | 001图像处理 | rebuild | P2 | 输入:Image2D；输出:运算后 Image2D；关键参数:运算类型+常量；场景:图像算术/逻辑运算 |
| ShowShape | 001图像处理 | retire | P3 | 淘汰理由：形状绘制为旧版显示辅助；当前 MeasurementOverlay 已覆盖形状叠加显示 |
| AreaOperations | 002检测识别 | rebuild | P2 | 输入:Region2D 集合；输出:运算后区域+面积；关键参数:并/交/差；场景:区域布尔运算 |
| ColorExtraction | 002检测识别 | rebuild | P2 | 输入:Image2D+目标色；输出:掩膜+提取区域；关键参数:HSV 范围；场景:按颜色提取区域 |
| CreatePoints | 002检测识别 | replace | P2 | 替代流程：点创建由 MeasurementInput(point) 承担；无需独立建点插件 |
| CreateROI | 002检测识别 | rebuild | P2 | 输入:交互绘制；输出:ROI(矩形/圆)；关键参数:形状类型；场景:交互式 ROI 生成送检测 |
| EdgeDefectDetection | 002检测识别 | rebuild | P1 | 输入:Image2D+参考边缘；输出:缺陷列表 DetectionList；关键参数:阈值/最小缺陷尺寸；场景:边缘缺陷检测 |
| GrayMeasure | 002检测识别 | rebuild | P2 | 输入:Image2D+区域；输出:灰度统计(均值/方差)；关键参数:统计项；场景:灰度质量测量 |
| GreenRegion | 002检测识别 | retire | P3 | 淘汰理由：单一绿色区域提取为业务定制；可由 ColorExtraction(rebuild) 通用颜色提取覆盖 |
| LabelRegion | 002检测识别 | rebuild | P2 | 输入:Region2D；输出:标注后区域+标签；关键参数:标签文本；场景:区域标注 |
| Matching1 | 002检测识别 | retire | P3 | 淘汰理由：与 Matching 功能重复的旧版变体；保留 Matching 即可 |
| MeasureCircle | 002检测识别 | rebuild | P1 | 输入:边缘点集；输出:圆心/半径/圆度；关键参数:半径范围；场景:圆尺寸测量(与 FindCircle 检测互补) |
| AffineeRegion | 004几何关系 | retire | P3 | 淘汰理由：仿射区域为旧版对位辅助；可由 Coordinate/CalculateOffset 组合覆盖 |
| BuildLl | 004几何关系 | rebuild | P2 | 输入:两点/两线；输出:构造 Line2D；关键参数:构造方式；场景:由几何元素构造直线 |
| FitEllipse | 004几何关系 | rebuild | P1 | 输入:边缘点集；输出:椭圆中心/长短轴/角度；关键参数:尺寸范围；场景:椭圆拟合测量 |
| RegionProcess | 004几何关系 | rebuild | P2 | 输入:Region2D；输出:处理后区域；关键参数:膨胀/腐蚀/填充；场景:区域形态学处理 |
| CalculateOffset | 005坐标标定 | rebuild | P1 | 输入:当前坐标+目标坐标；输出:偏移量(dx,dy,dθ)；关键参数:坐标系；场景:对位偏移计算 |
| Coordinate | 005坐标标定 | rebuild | P2 | 输入:标定点；输出:坐标变换；关键参数:变换类型；场景:坐标系统一 |
| CalibrationConversion | 006对位工具 | rebuild | P2 | 输入:标定参数+像素坐标；输出:物理坐标；关键参数:标定文件；场景:像素→物理坐标换算 |
| RotateNewPoint | 006对位工具 | rebuild | P2 | 输入:点+旋转中心+角度；输出:旋转后点；关键参数:角度/中心；场景:对位旋转补偿 |
| CSharpScript | 007逻辑工具 | retire | P3 | 淘汰理由：C# 脚本无法在 C++ 运行时执行；安全与可移植性差；建议以固定内置操作替代 |
| RunProject | 007逻辑工具 | retire | P3 | 淘汰理由：子工程嵌套执行复杂度高、易循环依赖；当前无此产品需求 |
| QueueClear | 009变量工具 | rebuild | P2 | 输入:队列名；输出:清空确认；关键参数:queueName；场景:与 QueueIn/QueueOut 配套的队列清空 |
| HKSetOutPut | 010文件通讯 | business_pack | P3 | 依赖：海康专用输出协议/硬件；属设备集成，需现场联调 |
| LightControl | 010文件通讯 | business_pack | P3 | 依赖：光源控制器串口/网络协议；属硬件集成 |
| ReceiveStr | 010文件通讯 | rebuild | P2 | 输入:TCP/串口连接；输出:接收字符串；关键参数:超时/分隔符；场景:与 TCPServer 配套的字符串接收 |
| SendStr | 010文件通讯 | rebuild | P2 | 输入:字符串+连接；输出:发送确认；关键参数:编码/终止符；场景:与 TCPClient 配套的字符串发送 |
| AI | 012深度学习 | business_pack | P3 | 依赖：GPU+深度学习模型权重；属模型集成，保留手工/夜间验收 |
| AIPost | 012深度学习 | business_pack | P3 | 依赖：AI 后处理模型；属模型集成 |
| Jigsaw | 012深度学习 | business_pack | P3 | 依赖：拼图检测模型；属业务模型 |
| Solder | 012深度学习 | business_pack | P3 | 依赖：焊点检测模型/硬件；属业务模型 |
| Yolo | 012深度学习 | business_pack | P3 | 依赖：YOLO 模型+GPU；属模型集成 |
| 3DPreProcessing | 0143D | rebuild | P1 | 输入:PointCloud3D；输出:滤波/下采样后点云；关键参数:滤波/体素大小；场景:3D 预处理 |
| BumpDentDetect | 0143D | business_pack | P3 | 依赖：凹凸检测工艺模型；属业务检测 |
| ContourDetection | 0143D | rebuild | P2 | 输入:PointCloud3D/深度图；输出:轮廓；关键参数:阈值；场景:3D 轮廓提取 |
| DepthToImage | 0143D | rebuild | P2 | 输入:深度图；输出:伪彩/灰度 Image2D；关键参数:映射范围；场景:深度可视化 |
| FitPlane | 0143D | rebuild | P1 | 输入:PointCloud3D；输出:平面系数+拟合误差；关键参数:内点阈值；场景:平面拟合 |
| Flatness | 0143D | rebuild | P2 | 输入:PointCloud3D；输出:平面度；关键参数:评定方法；场景:平面度测量 |
| GSD | 0143D | retire | P3 | 淘汰理由：GSD(地面采样距离) 为测绘专用指标，非本产品核心 |
| GapMeasure3D | 0143D | rebuild | P1 | 输入:两段 3D 轮廓/点云；输出:间隙距离；关键参数:测量方向；场景:3D 间隙测量(与 MeasureGap 互补) |
| HeightMeasurement | 0143D | rebuild | P2 | 输入:PointCloud3D+基准面；输出:高度差；关键参数:基准；场景:高度测量 |
| LidWeldDetection | 0143D | business_pack | P3 | 依赖：盖帽焊接工艺与硬件；属业务检测 |
| LinePlaneAngle | 0143D | rebuild | P2 | 输入:Line3D+Plane3D；输出:夹角；关键参数:无；场景:线面角测量 |
| PlaneAngle | 0143D | rebuild | P2 | 输入:两 Plane3D；输出:夹角；关键参数:无；场景:面面角测量 |
| PlaneCorrection | 0143D | rebuild | P2 | 输入:PointCloud3D+参考面；输出:校正后点云；关键参数:校正轴；场景:平面校正 |
| PointFilter | 0143D | rebuild | P2 | 输入:PointCloud3D；输出:过滤后点云；关键参数:距离/强度阈值；场景:离群点过滤 |
| VolumeMeasurement | 0143D | rebuild | P2 | 输入:PointCloud3D+基准；输出:体积；关键参数:基准面；场景:体积测量 |

完整逐项数据见 `hotfix-plugin-mapping.json`。以下列出需要决策的项目：

| 旧版插件 | 旧版目录 | 当前候选 | 状态 | 审核结论 |
| --- | --- | --- | --- | --- |
| ShowChart | `02Plugins/000常用工具/Plugin.ShowChart` | - | missing | - |
| AreaSpray | `02Plugins/001图像处理/Plugin.AreaSpray` | - | missing | - |
| CameraReadyWait | `02Plugins/001图像处理/Plugin.CameraReadyWait` | - | missing | - |
| CropImage | `02Plugins/001图像处理/Plugin.CropImage` | - | missing | - |
| DepthToGray | `02Plugins/001图像处理/Plugin.DepthToGray` | - | missing | - |
| DiplayData | `02Plugins/001图像处理/Plugin.DiplayData` | DisplayData | candidate | partial |
| ImageMerge | `02Plugins/001图像处理/Plugin.ImageMerge` | - | missing | - |
| ImageOperation | `02Plugins/001图像处理/Plugin.ImageOperation` | - | missing | - |
| ShowShape | `02Plugins/001图像处理/Plugin.ShowShape` | - | missing | - |
| AreaOperations | `02Plugins/002检测识别/Plugin.AreaOperations` | - | missing | - |
| BarcodeReader | `02Plugins/002检测识别/Plugin.BarcodeReader` | QRCode | candidate | partial |
| ColorExtraction | `02Plugins/002检测识别/Plugin.ColorExtraction` | - | missing | - |
| CreatePoints | `02Plugins/002检测识别/Plugin.CreatePoints` | - | missing | - |
| CreateROI | `02Plugins/002检测识别/Plugin.CreateROI` | - | missing | - |
| EdgeDefectDetection | `02Plugins/002检测识别/Plugin.EdgeDefectDetection` | - | missing | - |
| GrayMeasure | `02Plugins/002检测识别/Plugin.GrayMeasure` | - | missing | - |
| GreenRegion | `02Plugins/002检测识别/Plugin.GreenRegion` | - | missing | - |
| LabelRegion | `02Plugins/002检测识别/Plugin.LabelRegion` | - | missing | - |
| Matching1 | `02Plugins/002检测识别/Plugin.Matching1` | - | missing | - |
| MeasureCircle | `02Plugins/002检测识别/Plugin.MeasureCircle` | - | missing | - |
| DistanceLL | `02Plugins/003几何测量/Plugin.DistanceLL` | LinesDistance | candidate | intentionally_changed |
| AffineeRegion | `02Plugins/004几何关系/Plugin.AffineeRegion` | - | missing | - |
| BuildLl | `02Plugins/004几何关系/Plugin.BuildLl` | - | missing | - |
| FitEllipse | `02Plugins/004几何关系/Plugin.FitEllipse` | - | missing | - |
| RegionProcess | `02Plugins/004几何关系/Plugin.RegionProcess` | - | missing | - |
| CalculateOffset | `02Plugins/005坐标标定/Plugin.CalculateOffset` | - | missing | - |
| Coordinate | `02Plugins/005坐标标定/Plugin.Coordinate` | - | missing | - |
| MeasureCalib | `02Plugins/005坐标标定/Plugin.MeasureCalib` | NPointCalibration | candidate | partial |
| NPointCal | `02Plugins/005坐标标定/Plugin.NPointCal` | NPointCalibration | candidate | partial |
| CalibrationConversion | `02Plugins/006对位工具/Plugin.CalibrationConversion` | - | missing | - |
| RotateNewPoint | `02Plugins/006对位工具/Plugin.RotateNewPoint` | - | missing | - |
| CSharpScript | `02Plugins/007逻辑工具/Plugin.CSharpScript` | - | missing | - |
| RunProject | `02Plugins/007逻辑工具/Plugin.RunProject` | - | missing | - |
| QueueClear | `02Plugins/009变量工具/Plugin.QueueClear` | - | missing | - |
| HKSetOutPut | `02Plugins/010文件通讯/Plugin.HKSetOutPut` | - | missing | - |
| LightControl | `02Plugins/010文件通讯/Plugin.LightControl` | - | missing | - |
| ReceiveStr | `02Plugins/010文件通讯/Plugin.ReceiveStr` | - | missing | - |
| SendStr | `02Plugins/010文件通讯/Plugin.SendStr` | - | missing | - |
| AI | `02Plugins/012深度学习/Plugin.AI` | - | missing | - |
| AIPost | `02Plugins/012深度学习/Plugin.AIPost` | - | missing | - |
| Jigsaw | `02Plugins/012深度学习/Plugin.Jigsaw` | - | missing | - |
| Solder | `02Plugins/012深度学习/Plugin.Solder` | - | missing | - |
| Yolo | `02Plugins/012深度学习/Plugin.Yolo` | - | missing | - |
| 3DPreProcessing | `02Plugins/0143D/Plugin.3DPreProcessing` | - | missing | - |
| BumpDentDetect | `02Plugins/0143D/Plugin.BumpDentDetect` | - | missing | - |
| ContourDetection | `02Plugins/0143D/Plugin.ContourDetection` | - | missing | - |
| DepthToImage | `02Plugins/0143D/Plugin.DepthToImage` | - | missing | - |
| FitPlane | `02Plugins/0143D/Plugin.FitPlane` | - | missing | - |
| Flatness | `02Plugins/0143D/Plugin.Flatness` | - | missing | - |
| GSD | `02Plugins/0143D/Plugin.GSD` | - | missing | - |
| GapMeasure3D | `02Plugins/0143D/Plugin.GapMeasure3D` | - | missing | - |
| HeightMeasurement | `02Plugins/0143D/Plugin.HeightMeasurement` | - | missing | - |
| LidWeldDetection | `02Plugins/0143D/Plugin.LidWeldDetection` | - | missing | - |
| LinePlaneAngle | `02Plugins/0143D/Plugin.LinePlaneAngle` | - | missing | - |
| PlaneAngle | `02Plugins/0143D/Plugin.PlaneAngle` | - | missing | - |
| PlaneCorrection | `02Plugins/0143D/Plugin.PlaneCorrection` | - | missing | - |
| PointFilter | `02Plugins/0143D/Plugin.PointFilter` | - | missing | - |
| VolumeMeasurement | `02Plugins/0143D/Plugin.VolumeMeasurement` | - | missing | - |
| Envelope | `02Plugins/015包膜机/Plugin.Envelope` | - | business_pack | unverified |
| EnvelopeTieJiao | `02Plugins/015包膜机/Plugin.EnvelopeTieJiao` | - | business_pack | unverified |
| MPHA | `02Plugins/016密封钉/Plugin.MPHA` | - | business_pack | unverified |
| SealingPin | `02Plugins/016密封钉/Plugin.SealingPin` | - | business_pack | unverified |
| SealingPinHanHou | `02Plugins/016密封钉/Plugin.SealingPinHanHou` | - | business_pack | unverified |
| SealingPinYuhan | `02Plugins/016密封钉/Plugin.SealingPinYuhan` | - | business_pack | unverified |
| ThreeDimsAI | `02Plugins/016密封钉/Plugin.ThreeDimsAI` | - | business_pack | unverified |
