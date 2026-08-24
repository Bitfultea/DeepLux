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

| 审核结论 | 数量 | 含义 |
| --- | ---: | --- |
| equivalent | 39 | 能力等价 |
| partial | 11 | 部分替代（能力有缺失） |
| not_equivalent | 0 | 不等价 |

完整逐项数据见 `hotfix-plugin-mapping.json`。以下列出需要决策的项目：

| 旧版插件 | 旧版目录 | 当前候选 | 状态 | 审核结论 |
| --- | --- | --- | --- | --- |
| ShowChart | `02Plugins/000常用工具/Plugin.ShowChart` | - | missing | - |
| AreaSpray | `02Plugins/001图像处理/Plugin.AreaSpray` | - | missing | - |
| CameraReadyWait | `02Plugins/001图像处理/Plugin.CameraReadyWait` | - | missing | - |
| CropImage | `02Plugins/001图像处理/Plugin.CropImage` | - | missing | - |
| DepthToGray | `02Plugins/001图像处理/Plugin.DepthToGray` | - | missing | - |
| DiplayData | `02Plugins/001图像处理/Plugin.DiplayData` | DisplayData | candidate | equivalent |
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
| DistanceLL | `02Plugins/003几何测量/Plugin.DistanceLL` | LinesDistance | candidate | equivalent |
| AffineeRegion | `02Plugins/004几何关系/Plugin.AffineeRegion` | - | missing | - |
| BuildLl | `02Plugins/004几何关系/Plugin.BuildLl` | - | missing | - |
| FitEllipse | `02Plugins/004几何关系/Plugin.FitEllipse` | - | missing | - |
| RegionProcess | `02Plugins/004几何关系/Plugin.RegionProcess` | - | missing | - |
| CalculateOffset | `02Plugins/005坐标标定/Plugin.CalculateOffset` | - | missing | - |
| Coordinate | `02Plugins/005坐标标定/Plugin.Coordinate` | - | missing | - |
| MeasureCalib | `02Plugins/005坐标标定/Plugin.MeasureCalib` | NPointCalibration | candidate | partial |
| NPointCal | `02Plugins/005坐标标定/Plugin.NPointCal` | NPointCalibration | candidate | equivalent |
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
| Envelope | `02Plugins/015包膜机/Plugin.Envelope` | - | business_pack | - |
| EnvelopeTieJiao | `02Plugins/015包膜机/Plugin.EnvelopeTieJiao` | - | business_pack | - |
| MPHA | `02Plugins/016密封钉/Plugin.MPHA` | - | business_pack | - |
| SealingPin | `02Plugins/016密封钉/Plugin.SealingPin` | - | business_pack | - |
| SealingPinHanHou | `02Plugins/016密封钉/Plugin.SealingPinHanHou` | - | business_pack | - |
| SealingPinYuhan | `02Plugins/016密封钉/Plugin.SealingPinYuhan` | - | business_pack | - |
| ThreeDimsAI | `02Plugins/016密封钉/Plugin.ThreeDimsAI` | - | business_pack | - |
