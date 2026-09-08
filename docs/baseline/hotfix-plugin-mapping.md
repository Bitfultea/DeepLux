# Hotfix 插件映射清单

- 旧版仓库：`qhchen-sz/DeepLux`
- 固定提交：`47d76c1225e9dba5cfd3674df54cc3327894839b`
- 生成命令：`python3 scripts/audit_hotfix_plugins.py`
- 说明：`direct` 仅代表规范化名称相同；它不是参数、数据契约或运行结果等价的证明。
- 重跑脚本会保留已有 `reviewState`/`reviewConclusion`/`evidence` 字段（见 `load_existing_reviews`）。

| 状态 | 数量 | 含义 |
| --- | ---: | --- |
| direct | 48 | 名称直接匹配，仍需人工核验能力和参数 |
| candidate | 5 | 需要确认的历史别名或替代候选 |
| missing | 50 | 当前没有候选实现 |
| business_pack | 7 | 业务专用插件，作为可选业务包评审 |

| 审核状态 | 数量 |
| --- | ---: |
| reviewed | 53 |
| dependency_recorded | 7 |
| pending | 50 |

- 审核结论统计仅包含 `reviewState=reviewed` 的条目。

| 审核结论 | 数量 | 含义 |
| --- | ---: | --- |
| equivalent | 0 | 已证明与旧版能力等价（需旧版参数/端口/结果对照） |
| intentionally_changed | 15 | 已确认采用不同于旧版的契约或行为 |
| partial | 34 | 当前存在候选实现，旧版等价未证明 |
| unverified | 4 | 依赖硬件/SDK，行为未验证 |
| not_equivalent | 0 | 不等价 |

## 迁移范围决策（阶段 1 冻结范围，存在 migrationDecision）

> 下表 `input/output/keyParams/scenario` 记录的是**旧版契约**（按旧版 ViewModel 源码核验）；
> 目标 C++ 契约（强类型端口/载荷类型等）在阶段 7 另行设计评审，不得与旧版证据混淆。

| 决策 | 数量 | 含义 |
| --- | ---: | --- |
| rebuild | 37 | 需重建：真实算法+端口契约+参数验证+行为测试 |
| replace | 1 | 由当前已有能力/流程替代 |
| retire | 5 | 淘汰：无产品需求或已被覆盖 |
| business_pack | 10 | 业务包：依赖硬件/模型，需现场验收 |
| pending | 0 | 未决策 |

| 旧版插件 | 分类 | 决策 | 优先级 | 证据/替代/理由 |
| --- | --- | --- | --- | --- |
| ShowChart | 000常用工具 | retire | P3 | 淘汰理由：旧版为 LiveCharts 饼图/图表显示插件，纯展示无测量语义；当前主视图叠加+检查器结果页已覆盖结果展示，无独立图表产品需求 |
| AreaSpray | 001图像处理 | rebuild | P2 | 输入:Image2D+ROI（整幅图/链接 ROI/手绘喷绘区域三选一）；输出:区域填充后图像 Image2D；关键参数:GrayValue(0-255)/ROI 类型(FullImage/RoiLink/SprayRegion)/笔刷形状(圆形/矩形)；场景:指定区域像素填充掩蔽（如去除干扰区域）后送下游检测 |
| CameraReadyWait | 001图像处理 | rebuild | P2 | 输入:相机句柄（按序列号选择相机）；输出:就绪结果 bool（OK/NG）；关键参数:Timeout（默认 10000ms，-1=无限等待）/取消支持（CancelWaitForMeasureReady）；场景:采集前就绪同步：确保相机测量就绪后再触发采集，超时或取消判 NG |
| CropImage | 001图像处理 | rebuild | P1 | 输入:Image2D+多个旋转矩形（手动输入 DataList 或链接数组 CenterX/CenterY/Length1/Length2/Angle）；输出:裁剪图组（ReduceDomain+CropDomain）+ROI 区域组 HRegion；关键参数:矩形参数 X/Y/Deg/L1/L2/HomMat2D 仿射补正/IsOutputCropImage 开关；场景:多 ROI 批量裁剪：数组参数驱动多个旋转矩形，输出多幅裁剪图与区域 |
| DepthToGray | 001图像处理 | rebuild | P2 | 输入:深度图 HImage（链接）；输出:灰度图/最小 Z/最大 Z/映射比例/建议映射分辨率/饱和比例/源图宽度/源图高度；关键参数:MapResolution(映射分辨率，k=KConst/MapResolution)/ResolutionZ(Z 像素当量)/ConvertMinZ~ConvertMaxZ(转换 Z 范围，越界涂 0)/eFitForm(凹凸模式：全部/凸起/凹陷)/TranslateSize(平移)/HeightFilter(高度过滤)；场景:深度图按 Z 范围与凹凸模式映射为灰度图，输出映射统计（饱和/建议分辨率/源尺寸） |
| ImageMerge | 001图像处理 | rebuild | P2 | 输入:多 Image2D；输出:合并图像 HImage+裁剪图像列表 HImage[]+裁剪区域列表 HRegion[]；关键参数:DataList（裁剪矩形参数数组）/合并布局；场景:多幅裁剪子图按布局拼接合并为整图，并输出裁剪图/区域列表 |
| ImageOperation | 001图像处理 | rebuild | P2 | 输入:Image2D；输出:运算后图像 HImage；关键参数:SelectedOperation（运算类型）/OperandMode（操作数图像/常量）/ConstantValue/MultFactor/AddFactor/OffsetX/OffsetY/OffsetAngle；场景:图像与图像/常量的算术运算及旋转平移变换 |
| ShowShape | 001图像处理 | rebuild | P2 | 输入:Image2D+形状参数组（LineParams/CircleParams/Rect1Params/Rect2Params）；输出:直线/圆/矩形形状对象组（object）+Image2D；关键参数:各形状坐标参数（端点/圆心半径/矩形中心边长角度）/引用上游 ROI 的颜色与填充显示；场景:参数化生成参考形状几何对象，供下游测量/检测作为输入 |
| AreaOperations | 002检测识别 | rebuild | P2 | 输入:Region2D 集合；输出:运算后区域 HRegion（可选输出区域图像）；关键参数:SelectedOperationType（并/交/差等）/区域1/区域2 链接/OutputRegionImage 开关；场景:两个输入区域布尔运算，输出结果区域及可选区域图像 |
| ColorExtraction | 002检测识别 | rebuild | P2 | 输入:Image2D+目标色；输出:掩膜图像/区域/颜色数量/匹配面积/中心 XY/各颜色面积与中心；关键参数:SelectedColorSpace/ColorItems（目标颜色列表）/MinArea/ExtractionMode/AutoRoiType（自动圆/矩形）；场景:按颜色空间阈值提取目标颜色区域，输出面积/中心统计 |
| CreatePoints | 002检测识别 | replace | P2 | 替代流程：点创建由 MeasurementInput(point) 承担；无需独立建点插件 |
| CreateROI | 002检测识别 | rebuild | P2 | 输入:交互绘制；输出:区域 HRegion+矩形/圆形参数值（中心/角度/半长及仿射变换后值）；关键参数:形状类型（矩形/圆形）/中心/尺寸/角度参数/HomMat2D 仿射补正；场景:参数化创建 ROI 区域供下游检测使用，支持仿射补正 |
| EdgeDefectDetection | 002检测识别 | rebuild | P1 | 输入:Image2D+参考边缘；输出:是否有缺陷/缺陷区域数/凸出凹陷数/最大平均偏差与标准差/缺陷区域列表；关键参数:参考边缘/缺陷阈值/拟合参数（拟合点数）/凸凹判定（IsConvex）；场景:相对参考边缘拟合基准检测凸出/凹陷缺陷，输出缺陷区域与偏差统计 |
| GrayMeasure | 002检测识别 | rebuild | P2 | 输入:Image2D+区域；输出:平均灰度/最大灰度/最小灰度/灰度方差；关键参数:输入图像链接+测量区域链接；场景:区域内灰度统计测量，用于成像质量/曝光判定 |
| GreenRegion | 002检测识别 | retire | P3 | 淘汰理由：单一绿色区域提取为业务定制；可由 ColorExtraction(rebuild) 通用颜色提取覆盖 |
| LabelRegion | 002检测识别 | rebuild | P2 | 输入:Region2D；输出:合并区域/区域数量/区域{i}/提取图像{i}；关键参数:PixelMin/PixelMax（灰度范围）/MinArea（最小面积）/IsOutputExtractImage 开关；场景:按灰度范围阈值分割并提取标签连通区域及子图 |
| Matching1 | 002检测识别 | retire | P3 | 淘汰理由：与 Matching 功能重复的旧版变体；保留 Matching 即可 |
| MeasureCircle | 002检测识别 | rebuild | P1 | 输入:HImage+初始圆（圆心/半径，可链接变量或 HomMat2D 仿射补正）；输出:测量圆对象/圆心 X/圆心 Y/半径/直径/圆度；关键参数:MeasInfo（Threshold/Length1/Length2/MeasSelect/MeasNum/MeasMode/ExclusionPoint）/屏蔽区域过滤测量点（FilterMeasurePoints+重拟合）/Scale 实际坐标输出；场景:初始圆区域内提取圆周测量点拟合圆；屏蔽区域剔除干扰点后重新拟合 |
| AffineeRegion | 004几何关系 | rebuild | P2 | 输入:HImage+HRegion（链接，必需，区域无效判 NG）+起点/终点仿射参数；输出:仿射变换后区域；关键参数:起点(X1/Y1/Angle1)与终点(X2/Y2/Angle2)/SelectedInterpolationMethod(插值方法)；场景:VectorAngleToRigid 由起点/终点位姿求刚性矩阵，AffineTransRegion 变换输入区域 |
| BuildLl | 004几何关系 | rebuild | P2 | 输入:HImage+两条直线对象（Line1/Line2 链接）；输出:交点 X/交点 Y/弧度/角度/平行标志；关键参数:两条直线链接（Line1LinkText/Line2LinkText）；场景:IntersectionLl 由两条直线构建交点/夹角/平行关系 |
| FitEllipse | 004几何关系 | rebuild | P1 | 输入:边缘点集；输出:中心 X/中心 Y/角度 Phi/长轴 R/短轴 R/椭圆度；关键参数:输入轮廓/边缘点链接；场景:轮廓点拟合椭圆并输出椭圆度 |
| RegionProcess | 004几何关系 | rebuild | P2 | 输入:Region2D；输出:拟合区域 HRegion+逐区域中心/面积/半径或宽高/角度；关键参数:拟合形状（圆/矩形）/连通区域数量；场景:连通区域逐个拟合几何形状并输出形状参数 |
| CalculateOffset | 005坐标标定 | rebuild | P1 | 输入:当前坐标+目标坐标；输出:OffsetX/OffsetY/OffsetA；关键参数:当前坐标（X/Y/Φ）与基准坐标（ModeCoord 对 MathCoord）；场景:对位偏移计算，输出平移+角度偏移供补正 |
| Coordinate | 005坐标标定 | rebuild | P2 | 输入:HImage+当前坐标链接（X/Y/Deg）；输出:刚性变换矩阵 HomMat2D 及其逆（供下游 ROI/测量补正）；关键参数:基准坐标 ModeCoord(X/Y/Φ)/当前坐标链接（X/Y/Deg）/AxisLength 轴长显示；场景:生成基准→当前坐标补正矩阵（VectorAngleToRigid），供多工位对位 |
| CalibrationConversion | 006对位工具 | rebuild | P2 | 输入:标定参数+像素坐标；输出:新点 X/新点 Y；关键参数:标定矩阵/参数链接+像素坐标输入；场景:像素坐标经标定参数换算为物理坐标点 |
| RotateNewPoint | 006对位工具 | rebuild | P2 | 输入:点+旋转中心+角度；输出:新点 X/新点 Y；关键参数:原点坐标/旋转中心/旋转角度；场景:绕旋转中心旋转补偿，计算旋转后点坐标 |
| CSharpScript | 007逻辑工具 | retire | P3 | 淘汰理由：C# 脚本无法在 C++ 运行时执行；安全与可移植性差；建议以固定内置操作替代 |
| RunProject | 007逻辑工具 | retire | P3 | 淘汰理由：子工程嵌套执行复杂度高、易循环依赖；当前无此产品需求 |
| QueueClear | 009变量工具 | rebuild | P2 | 输入:队列名；输出:无数据输出（仅执行状态）；关键参数:QueueKey（队列名）；场景:清空指定变量队列（s_QueueDic），与 QueueIn/QueueOut 配套 |
| HKSetOutPut | 010文件通讯 | business_pack | P3 | 依赖：海康专用输出协议/硬件；属设备集成，需现场联调 |
| LightControl | 010文件通讯 | business_pack | P3 | 依赖：光源控制器串口/网络协议；属硬件集成 |
| ReceiveStr | 010文件通讯 | rebuild | P2 | 输入:TCP/串口连接；输出:接收文本 string；关键参数:CurKey（连接键）/IsEnableTimeOut+TimeOut/ReceiveAsHex/IsClearCache；场景:从 TCP/串口连接接收文本，供协议解析 |
| SendStr | 010文件通讯 | rebuild | P2 | 输入:字符串+连接；输出:无数据输出（发送动作）；关键参数:SendStr（发送内容）/IsSendByHex/终止符 eEnableEndstr/超时 IsEnableTimeOut/Continue；场景:向 TCP/串口发送字符串，支持十六进制/终止符/超时 |
| AI | 012深度学习 | business_pack | P3 | 依赖：GPU+深度学习模型权重；属模型集成，保留手工/夜间验收 |
| AIPost | 012深度学习 | business_pack | P3 | 依赖：AI 后处理模型；属模型集成 |
| Jigsaw | 012深度学习 | business_pack | P3 | 依赖：拼图检测模型；属业务模型 |
| Solder | 012深度学习 | business_pack | P3 | 依赖：焊点检测模型/硬件；属业务模型 |
| Yolo | 012深度学习 | business_pack | P3 | 依赖：YOLO 模型+GPU；属模型集成 |
| 3DPreProcessing | 0143D | rebuild | P1 | 输入:深度图 HImage（2 通道图 Decompose2 提取深度通道）+可选 ROI（全图/ROI 链接）；输出:预处理图像 HImage（深度图）；关键参数:高度筛选（HeightFilterEnabled/HeightFilterMin/Max/FillValue 越界填充）/3D 预处理参数；场景:深度图通道提取与高度筛选（阈值区间外像素填无效值）等测量前预处理 |
| BumpDentDetect | 0143D | business_pack | P3 | 依赖：凹凸检测工艺模型；属业务检测 |
| ContourDetection | 0143D | rebuild | P2 | 输入:深度图/高度图 HImage（链接）；输出:各轮廓检测点值/Row/Col 数组+结果总数（含高度差 A/B 值）；关键参数:strip 条带/workface 工位配置/检测阈值；场景:高度图上提取截面轮廓并检测特征点，输出逐点高度与坐标 |
| DepthToImage | 0143D | rebuild | P2 | 输入:深度图；输出:结果图像/最小 Z/最大 Z/映射比例/建议映射分辨率/饱和比例；关键参数:映射范围（最小/最大 Z）/映射分辨率；场景:深度图线性映射为灰度图像并输出映射统计 |
| FitPlane | 0143D | rebuild | P1 | 输入:深度图 HImage+ROI 矩形（中心/长边/短边/角度，可链接+HomMat2D 仿射补正）；输出:法向量 Nx/Ny/Nz/平面距离 D/平面度/最大最小偏差/RMS/拟合平面图像；关键参数:ROI 参数（InitRoiCenterX/Y/Length1/Length2/Angel）/仿射补正/拟合方法；场景:深度图 ROI 内拟合平面，输出平面系数与平面度 |
| Flatness | 0143D | rebuild | P2 | 输入:深度图 HImage+ROI 矩形（可链接+HomMat2D 仿射补正）；输出:平面度/最大偏差/最小偏差/RMS/Alpha/Beta/Gamma；关键参数:ROI 参数（中心/长边/短边/角度）/评定方法；场景:深度图 ROI 内平面度评定与姿态角输出 |
| GSD | 0143D | business_pack | P3 | 依赖：GapStepDetect.dll 私有台阶缝隙算法库（源码不在仓库）；旧版名"台阶缝隙检测"，取 ROI 高度图点(≥1000)输出 step_width/step_height，无法离线等价验收 |
| GapMeasure3D | 0143D | rebuild | P1 | 输入:高度图 HImage（单幅）+截面矩形 ROI（中心 X/Y/长度/高度，可链接）；输出:测量宽度/拐角水平垂直欧氏距离/偏移后宽度/是否合格/算法名称/ROI 参数回显；关键参数:PixelSizeX(X 像素间距)/ZScale(手动 Z 比例)/OffsetMm(偏移距离)/SpecUpperLimit(规格上限)/DerivativeThreshold(导数阈值)/DzZeroThreshold(导数平顶阈值)/EdgeTrim(边缘剔除点数)/MinPeakDistance(峰最小间距)/RowWidth(行取宽)/SinglePeakOffsetRatio(单波峰偏移比例)/平滑参数（SmoothSigma 高斯/MedianSize 中值/Savgol 窗口阶数）/TimeoutMs/MeasureFailValue；场景:高度图 ROI 提取单截面轮廓，导数寻峰拟合拐角并计算间隙宽度 |
| HeightMeasurement | 0143D | rebuild | P2 | 输入:3D 高度图 HImage+基准 ROI（手动绘制/链接区域/链接基准平面图像三选一）+测量 ROI；输出:断差（HeightDifference）；关键参数:基准模式（外部基准平面/拟合平面/平均高度）/基准与测量 ROI 参数/仿射补正；场景:基准 ROI 计算参考高度（三种模式），测量 ROI 提取断差（实际高度-基准高度） |
| LidWeldDetection | 0143D | business_pack | P3 | 依赖：盖帽焊接工艺与硬件；属业务检测 |
| LinePlaneAngle | 0143D | rebuild | P2 | 输入:Line3D+Plane3D；输出:角度；关键参数:Line3D 链接+Plane3D 链接；场景:线与面夹角计算 |
| PlaneAngle | 0143D | rebuild | P2 | 输入:两 Plane3D；输出:角度；关键参数:两个 Plane3D 链接；场景:两面夹角计算 |
| PlaneCorrection | 0143D | rebuild | P2 | 输入:高度图 HImage（第 1 通道为 Z 高度）+基准平面（链接基准平面图像 或 手动 Nx/Ny/Nz/D）+可选 ROI 矩形；输出:校正后图像/平面度/最大最小偏差/RMS/高度距离；关键参数:ePlaneMode（PlaneImage 链接图像/Manual 手动平面 Nx/Ny/Nz/D）/eCorrectionMode（Quick/Projection/PointToPlaneDistance 三种校正模式）/ROI 参数（中心/长边/短边/角度，仿射补正）/TranslateZ（Z 平移）/ResolutionX/Y/Z；场景:高度图按基准平面校正倾斜：支持链接图像或手动平面两种来源、三种校正模式、可选 ROI、Z 平移与 XYZ 分辨率 |
| PointFilter | 0143D | rebuild | P2 | 输入:高度图/深度图 HImage（1 通道高度或 2 通道高度+灰度；使用图像 domain 作为过滤范围）；输出:滤波后图像/原始点数/保留点数；关键参数:eFilterType 8 种滤波算法及独立参数：VoxelDownSample(VoxelSize)/UniformDownSample(EveryKPoints)/IndexDownSample 与 SelectByIndex(IndicesText/Invert)/StatisticalOutliers(NbNeighbors/StdRatio)/RadiusOutliers(NbPoints/SearchRadius)/AxiFilter(MinVal/MaxVal/Axis 0=X/1=Y/2=Z)/HeightFilter(HeightThreshold)；InvalidValue(丢弃填充值)；旧版经 PointCloudFilter.dll 在物理坐标空间执行；场景:点云滤波/降采样/离群剔除（8 种算法可选），按 ScaleX/Y/Z 物理坐标执行，输出滤波后图像与点数统计 |
| VolumeMeasurement | 0143D | rebuild | P2 | 输入:高度图 HImage+可选基准平面图像（链接；未链接时以零平面为基准）+测量区域（整图/手动绘制/链接区域）；输出:体积/体积单位/有效点数；关键参数:eVolumeCalcMode（AbovePlane 平面上方/BelowPlane 平面下方）/eMeasureRegionSource（整图/手动绘制/链接区域）/RemoveHeight（移除高度）/ResolutionX/Y/Z/测量 ROI 参数（中心/长边/短边/角度）；场景:相对基准面（链接图像一阶曲面拟合，未链接用零平面）按测量区域积分平面上方/下方体积，支持移除高度与 XYZ 分辨率 |

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
| GrayMeasure | `02Plugins/002检测识别/Plugin.GrayMeasure` | - | missing | - |
| GreenRegion | `02Plugins/002检测识别/Plugin.GreenRegion` | - | missing | - |
| LabelRegion | `02Plugins/002检测识别/Plugin.LabelRegion` | - | missing | - |
| Matching1 | `02Plugins/002检测识别/Plugin.Matching1` | - | missing | - |
| DistanceLL | `02Plugins/003几何测量/Plugin.DistanceLL` | LinesDistance | candidate | intentionally_changed |
| AffineeRegion | `02Plugins/004几何关系/Plugin.AffineeRegion` | - | missing | - |
| BuildLl | `02Plugins/004几何关系/Plugin.BuildLl` | - | missing | - |
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
