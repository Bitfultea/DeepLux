# G8 当前验收矩阵（进行中；不是最终 PASS）

> 本矩阵把当前插件 fixture、软件操作证据、历史基线差异和现场验收分开记录。`PASS_SOFTWARE_FIXTURES` 只表示独立审核的本地业务与关键失败 fixture 通过，不代表真实设备、模型/provider、GPU、Windows 或现场验收通过。

- 当前源码插件：69；当前构建二进制：67。
- Qt 选择：请求 Qt6；实际链接 Qt5；回退：true。
- 最新全量 CTest：113/113 通过，0 跳过；独立 fixture 审核日志：113/113 通过。
- 历史/当前二进制差异：66 → 67（新增存在：com.deeplux.plugin.defectdetection）。
- 独立 fixture 审核：业务 `{"FAIL": 1, "NOT_AUDITED": 2, "PASS": 66}`；关键失败 `{"NOT_AUDITED": 2, "PASS": 67}`。
- 插件软件状态：`{"BLOCKED_PLATFORM": 1, "BLOCKED_SDK_OR_BUILD_OPTION": 1, "FAIL_BUSINESS_FIXTURE": 1, "PASS_SOFTWARE_FIXTURES": 66}`。
- GUI 操作：126；映射类型：`{"DIRECT_TOOL": 66, "HUMAN_HANDOFF": 37, "TOOL_COMPOSITION": 5, "UNSUPPORTED": 18}`；证据链接：`{"EVIDENCE_LINKED": 126}`。
- 现场验收：0；总状态：`NOT_READY`。

## 插件 fixture 与现场状态

| Plugin ID | 插件 | 平台 | 当前二进制 | SHA-256 (前12) | 业务 fixture | 关键失败 | fixture 审核 | 现场 | 当前软件状态 | 原因 / 证据 |
|---|---|---|---:|---|---|---|---|---|---|---|
| `com.deeplux.plugin.calculateoffset` | CalculateOffset | any | True | `598831b5f016` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentCalculatesOffsetFromPointPortAndRejectsInvalidPoint3D (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.npointcalibration` | NPointCalibration | any | True | `109fab15e131` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agentphase0_repro::testG3NPointCalibrationOutputPortsByRunId (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.camera.directshow` | DirectShowCamera | windows | False | `—` | NOT_AUDITED | NOT_AUDITED | NOT_REVIEWED | NOT_RUN | BLOCKED_PLATFORM | No current binary for target platform windows; field/platform evidence remains outstanding. |
| `com.deeplux.camera.hikvision` | HikvisionCamera | linux | False | `—` | NOT_AUDITED | NOT_AUDITED | NOT_REVIEWED | NOT_RUN | BLOCKED_SDK_OR_BUILD_OPTION | Required SDK/build option unavailable. |
| `com.deeplux.camera.v4l2` | V4L2Camera | linux | True | `d21853b7b174` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_v4l2discovery::testAgentListsFakeV4L2DiscoveryAndReleasesSkippedDeviceFd (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.plccommunicate` | PLCCommunicatePlugin | any | True | `c0ec9007f783` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_plccommunicate_execution_g5::testStepOnceExpiresDriftRejectsAndConfirmsSingleFC03 (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.plcread` | PLCReadPlugin | any | True | `23a2323e2b4e` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfigurationReadsModbusAndReportsProtocolFailure (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.plcwrite` | PLCWritePlugin | any | True | `173870e7b168` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_plcwrite_execution_g5::testPLCWriteRequiresConfirmationForRunFlowAndStepOnce (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.serialport` | SerialPort | any | True | `75f7b87edaba` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_serialport_execution_g5::testRunFlowRejectsAndExpiresSerialTargetThenReadsPTYReply (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.tcpclient` | TCPClient | any | True | `71883e396277` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_tcpclient_execution_g5::testRunFlowBindsEndpointExpiresOnPortChangeAndReadsResponse (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.tcpserver` | TCPServer | any | True | `2c3fc10016c0` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_tcpserver_execution_g5::testRunFlowBindsLocalEndpointExpiresOnPortChangeAndReadsRequest (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.colorrecognition` | ColorRecognition | any | True | `f66e044f7348` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentRecognizesConfiguredColorAndReportsAbsentTarget (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.edgedefectdetection` | EdgeDefectDetection | any | True | `5d332f061cca` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_acceptance_flows::testAgentEdgeDefectDeviationUnitsRequireImageLocalPointSetSource (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.findcircle` | FindCircle | any | True | `9e1ac98f3040` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentFindsConfiguredCircleAndReportsNoCircle (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.jierhandefectsdet` | JiErHanDefectsDet | any | True | `6fad77d79e96` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_acceptance_flows::testAgentJiErHanDefectRegionMapsThroughCropOrigin (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.matching` | Matching | any | True | `3a34b942e93b` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_acceptance_flows::testAgentMatchingScalarsDeclareLocalPixelUnits (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.measurecircle` | MeasureCircle | any | True | `f6f7e2df64a1` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresConfiguredCircleAndReportsInsufficientEdges (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.measureline` | MeasureLine | any | True | `a0d9601cf364` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresConfiguredLineAndReportsNoLine (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.measurerect` | MeasureRect | any | True | `88683c7b5f0c` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresRectangleAndReportsNoContour (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.qrcode` | QRCode | any | True | `781b9a722c45` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentLoadsDecodesAndDisplaysRealImageOutput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.sampredict` | SamPredict | any | True | `c46e3c8f7e33` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_sam_region_flow::testAgentSamPredictRegionCoordinatesAfterNonzeroCrop (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.distancepl` | DistancePL | any | True | `cf53a7255347` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresPointToLineAndValidatesRequiredLineInput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.distancepp` | DistancePP | any | True | `578b067d7fb8` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresPointPairAndValidatesRequiredDistanceInput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.fitcircle` | FitCircle | any | True | `aa646cfed81d` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentFitsPointSetAndReportsRadiusConstraintFailure (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.fitellipse` | FitEllipse | any | True | `3b01985244c8` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentFitsEllipseAndReportsAxisRangeFailure (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.fitline` | FitLine | any | True | `aa10b05f44b3` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentFitsPointSetAndRejectsDegenerateLine (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.fitplane` | FitPlane | any | True | `d45bb6e2a365` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentFitsHeightMapAndReportsNoValidPixels (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.freeformsurface` | FreeformSurface | any | True | `5ef9ff6dc026` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentLoadPointCloudFeedsFreeformSurfaceAndRejectsLegacyParameter (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.gapmeasure3d` | GapMeasure3D | any | True | `971d2cc4aed4` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresGapFromHeightImageAndReportsNotFoundAndInvalidRoi (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.linesdistance` | LinesDistance | any | True | `bf930abd2be3` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresFiniteLineSegmentsAndRequiresBothInputs (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.measuregap` | MeasureGap | any | True | `b41136f638a0` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasures2DGapAndRequiresSecondPoint (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.measurementinput` | MeasurementInput | any | True | `2b268e479a80` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentSuppliesPointPairAndReportsDimensionMismatch (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.pointsurfacedistance` | PointSurfaceDistance | any | True | `180aa659a701` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentMeasuresWorldPointToPlaneAndRequiresPlane (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.regionstats` | RegionStats | any | True | `1032e9cafea2` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_sam_region_flow::testAgentRegionStatsCentroidUnknownProducerHasNoSourceMapping (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.defectdetection` | DefectDetection | any | True | `58a08073fa07` | FAIL | PASS | PASS | NOT_RUN | FAIL_BUSINESS_FIXTURE | test_agent_core_plugins_g8::testAgentReportsHymsonSdkUnavailableForDefectDetection (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.blob` | Blob | any | True | `c0f4d11db4e4` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentDetectsConfiguredBlobAndReportsEmptyDetection (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.cropimage` | CropImage | any | True | `e736073d8184` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentCropsMultipleRectanglesAndReportsNoIntersection (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.displaydata` | DisplayData | any | True | `f13901f575ca` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentOverlaysDisplayDataAndAttributesMissingImageFailure (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.grabimage` | GrabImage | any | True | `bc51f0c01610` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentLoadsGrabImagePathDemoAndFolderModes (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.imagescript` | ImageScript | any | True | `f303dad9a1dc` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentImageScriptInvertAndRejectsInvalidConfigurationAndMissingImage (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.jigsawsolver` | JigsawPuzzle | any | True | `2bda270736ee` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentJigsawPuzzleSplitsAndReassemblesGridAndRejectsUndersizedInput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.loadpointcloud` | LoadPointCloud | any | True | `9c18f32cfa30` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentLoadPointCloudFeedsFreeformSurfaceAndRejectsLegacyParameter (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.perprocessing` | PerProcessing | any | True | `2fd3e8f7a598` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentPerProcessingGaussianBlurAndRejectsUnsupportedTypeAndMissingImage (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.3dpreprocessing` | PreProcessing3D | any | True | `db8be6a0fb8f` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentPreProcessing3DReportsHeightMapAndRejectsColorInput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.saveimage` | SaveImage | any | True | `0aac3b43f592` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentConfirmsSaveImageAndReportsEncoderFailure (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.showimage` | ShowImage | any | True | `3902820b8366` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentLoadsDecodesAndDisplaysRealImageOutput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.condition` | Condition | any | True | `4f78373ae5ad` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentEvaluatesConditionFromUpstreamAndReportsMissingVariable (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.delay` | Delay | any | True | `345de972a8ed` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentRunsConfiguredDelayAndRejectsZeroDuration (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.if` | If | any | True | `31aab6c8870d` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_acceptance_flows::testAgentRunFlowPreservesIfBranchSemantics (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.loop` | Loop | any | True | `199eadb2997c` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_acceptance_flows::testAgentRunFlowPreservesLoopCount (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.parallel` | Parallel | any | True | `c269dbca791c` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_acceptance_flows::testAgentRunFlowPreservesParallelAllJoin (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.queuein` | QueueIn | any | True | `9a98dc6eaff7` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentQueueInRoundTripsConfiguredStringAndRejectsMissingData (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.queueout` | QueueOut | any | True | `b801dbfdd5a9` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentQueueOutEmptyResultUsesConfiguredOutputAndReportsZeroSize (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.stopwhile` | StopWhile | any | True | `cdac42c17371` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentStopWhileExitsWhileBodyAndContinuesAfterFlow (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.while` | While | any | True | `d4048f917fd2` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_visual_plugins_g8::testAgentStopWhileExitsWhileBodyAndContinuesAfterFlow (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.datacheck` | DataCheck | any | True | `158bc9a41a43` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentChecksUpstreamValueAndReportsMissingCheckData (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.folder` | Folder | any | True | `e185e61be6e9` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_folder_execution_g5::testFolderDeleteRequiresConfirmationForRunFlowAndStepOnce (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.savedata` | SaveData | any | True | `b0b8d206570d` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfiguresSaveDataAndReportsWriteFailure (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.showpoint` | ShowPoint | any | True | `6414f536c0cb` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_acceptance_flows::testAgentShowPointOutputsDeclareCurrentImagePixelUnits (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.systemtime` | SystemTime | any | True | `c9b3290dd237` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfigurationReadsSystemTimeAndRejectsEmptyFormat (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.tableoutput` | TableOutPut | any | True | `dea51af26573` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfiguresTableOutputAndRejectsInvalidDimensions (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.timeslice` | TimeSlice | any | True | `e170f93bf480` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentRunsTimeSliceAndRejectsStopWithoutStartInput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.writetext` | WriteText | any | True | `6f09e77681f1` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfiguresWriteTextAndPropagatesOpenFailure (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.createstring` | CreateString | any | True | `ddb0ef970821` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfigurationCreatesStringAndRejectsEmptyOutputName (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.math` | MathPlugin | any | True | `22da01462406` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfigurationCalculatesAndRejectsDivisionByZero (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.splitstring` | SplitString | any | True | `e244df6e1f88` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfigurationSplitsStringAndReportsEmptyInput (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.strformat` | StrFormatPlugin | any | True | `4827dfc40a11` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_core_plugins_g8::testAgentConfigurationFormatsStringAndRejectsEmptyOutputName (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.vardefine` | VarDefinePlugin | any | True | `dcebf29e3df9` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_variable_plugins_g8::testLocalizedPluginNamesConfigureRunAndReportBusinessResults (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |
| `com.deeplux.plugin.varset` | VarSetPlugin | any | True | `7becc8cc2ab5` | PASS | PASS | PASS | NOT_RUN | PASS_SOFTWARE_FIXTURES | test_agent_variable_plugins_g8::testLocalizedPluginNamesConfigureRunAndReportBusinessResults (`build-codex-agent/agent-phase8-current-matrix-evidence/slice47-full-ctest.log`) |

## GUI 操作证据索引

映射类型和证据链接是独立维度。`EVIDENCE_LINKED` 仅表示测试函数与通过的 CTest suite 可解析；它仍需对应阶段/操作的语义审核。`HUMAN_HANDOFF` 和 `UNSUPPORTED` 是明确的产品映射状态，不作为自动化通过项。

| Operation ID | 操作 | 映射 | 阶段 | 映射状态 | 证据状态 | Test ID / 说明 |
|---|---|---|---:|---|---|---|
| `FILE_NEW_SOLUTION` | 新建方案(工程) | 直接工具 | 1 | DIRECT_TOOL | EVIDENCE_LINKED | testProjectSwitchClearsUndoHistory (`tests/test_agentactor.cpp`) |
| `FILE_SOLUTION_LIST` | 方案列表(浏览既有工程) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testListRecentProjectsIsReadOnlyAndPreservesInvalidEntries (`tests/test_agent_recent_projects.cpp`) |
| `FILE_OPEN` | 打开工程 | 直接工具 | 1 | DIRECT_TOOL | EVIDENCE_LINKED | testOpenProjectMenuActionLoadsAndSyncsProject (`tests/test_mainwindow.cpp`) |
| `FILE_SAVE` | 保存工程 | 直接工具 | 1 | DIRECT_TOOL | EVIDENCE_LINKED | testProjectOperationSlotsEnforceProductPermissionsAndRecheckSave (`tests/test_mainwindow.cpp`) |
| `FILE_EXIT` | 退出应用 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testFileExitMenuClosesMainWindowWithoutQuittingTestHost (`tests/test_fileexit_handoff.cpp`) |
| `PARAM_UNDO` | 撤销(参数撤销栈) | 未支持 | 4 | UNSUPPORTED | EVIDENCE_LINKED | testG4AgentGuiParameterUndoIsKeyScopedAndStaleSafe (`tests/test_mainwindow.cpp`) |
| `PARAM_REDO` | 重做 | 未支持 | 4 | UNSUPPORTED | EVIDENCE_LINKED | testG4AgentGuiParameterUndoIsKeyScopedAndStaleSafe (`tests/test_mainwindow.cpp`) |
| `PARAM_GLOBAL_VAR` | 全局变量管理(查询/增删改) | 通用工具组合 | 6 | TOOL_COMPOSITION | EVIDENCE_LINKED | testGlobalVariableTypedCrudIsLiteralAndFailClosed (`tests/test_agentactor.cpp`) |
| `PARAM_USER_LOGIN` | 用户登录 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenUserLoginDialogOpensReusesAndPreservesUserState (`tests/test_mainwindow.cpp`) |
| `VIEW_TOGGLE_TOOL_PANEL` | 显示/隐藏工具面板 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentWorkspacePanelVisibilityWithoutProjectAndInFocusMode (`tests/test_mainwindow.cpp`) |
| `VIEW_TOGGLE_PROCESS_PANEL` | 显示/隐藏流程面板 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentWorkspacePanelVisibilityWithoutProjectAndInFocusMode (`tests/test_mainwindow.cpp`) |
| `VIEW_TOGGLE_BOTTOM_PANEL` | 显示/隐藏底部面板 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentWorkspacePanelVisibilityWithoutProjectAndInFocusMode (`tests/test_mainwindow.cpp`) |
| `VIEW_QUICK_MODE` | 快捷运行当前流程一次 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testQuickModeRunsCurrentFlowOnce (`tests/test_mainwindow.cpp`) |
| `VIEW_TOGGLE_THEME` | 深浅主题设置 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testSetThemePermissionPersistenceAndFailures (`tests/test_agent_set_theme.cpp`) |
| `VIEW_RENDER_MODE` | 3D 渲染模式(统一色/RGB/高度/强度/法向/BlinnPhong) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAvailableModesUseGuiActionAndPersistIntoLazy3DContent (`tests/test_agent_viewport_render_mode.cpp`) |
| `TOOL_CAMERA_SETTINGS` | 打开相机设置页 | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCameraSettingsMenuRequiresCameraPermissions (`tests/test_mainwindow.cpp`) |
| `TOOL_COMM_SETTINGS` | 打开通讯设置页 | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCommunicationSettingsMenuRequiresCommunicationPermission (`tests/test_mainwindow.cpp`) |
| `TOOL_HARDWARE_CONFIG` | 硬件配置(系统参数:自动加载/自动保存/语言/主题/循环间隔/日志) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentSystemSettingsDialogLifecycleAndThemeSync (`tests/test_mainwindow.cpp`) |
| `AGENT_SETTINGS_OPEN` | 打开/复用 Agent 设置窗口 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testOpenCancelManualSaveAndModalReuse (`tests/test_agent_settings_handoff.cpp`) |
| `TOOL_AGENT_SETTINGS` | Agent 设置(端点/密钥/模型/温度/maxTokens/权限级/系统提示/开关) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testOpenCancelManualSaveAndModalReuse (`tests/test_agent_settings_handoff.cpp`) |
| `TOOL_KNOWLEDGE_BASE` | 打开知识库管理对话框 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testNavigationLifecycleAndModalSettingsReuse (`tests/test_agent_knowledgebase_handoff.cpp`) |
| `HELP_ABOUT` | 关于对话框 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testNavigationLifecycleAndModalMenuReuse (`tests/test_agent_about_handoff.cpp`) |
| `TB_RUN_ONCE` | 单次运行流程 | 直接工具 | 3 | DIRECT_TOOL | EVIDENCE_LINKED | testConnectionChangeRebuildsExecutionTopology (`tests/test_mainwindow.cpp`) |
| `TB_STEP` | 单步运行 | 直接工具 | 4 | DIRECT_TOOL | EVIDENCE_LINKED | testG4AgentAndGuiSingleStepSharePosition (`tests/test_mainwindow.cpp`) |
| `TB_LOOP_RUN` | 循环运行 | 未支持 | 4 | UNSUPPORTED | EVIDENCE_LINKED | testG4AgentCanStopPausedGuiCycle (`tests/test_mainwindow.cpp`) |
| `TB_STOP` | 停止运行 | 直接工具 | 3 | DIRECT_TOOL | EVIDENCE_LINKED | testG4AgentStopReportsStoppingUntilNonCooperativeModuleSettles (`tests/test_agentactor.cpp`) |
| `TB_QUICK_MEASURE` | 快速测量入口(进入拾取模式) | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentAndGuiQuickMeasurementModesAreMutuallyExclusive (`tests/test_mainwindow.cpp`) |
| `TB_QUICK_ANNOTATE` | 快速标注入口(打开 SAM 对话框) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testSafeOpenReusePreservesBindingAndDoesNotPrepareBackend (`tests/test_agent_sam_annotator_handoff.cpp`) |
| `TOOLBOX_SEARCH` | 搜索过滤工具(模块) | 通用工具组合 | 5 | TOOL_COMPOSITION | EVIDENCE_LINKED | testToolBoxSearchFiltersItemsAndRestoresOnClear (`tests/test_mainwindow.cpp`) |
| `TOOLBOX_DRAG_ADD` | 拖拽模块到画布/流程树添加节点 | 直接工具 | 2 | DIRECT_TOOL | EVIDENCE_LINKED | testG4SourceLessDropRequiresMatchingPayload (`tests/test_mainwindow.cpp`) |
| `PTREE_SELECT` | 选中节点(联动检查器) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testProcessTreeClickOverridesPinnedInspector (`tests/test_mainwindow.cpp`) |
| `PTREE_DELETE` | 删除节点(右键菜单) | 直接工具 | 4 | DIRECT_TOOL | EVIDENCE_LINKED | testG4AgentRemoveUndoKeepsProcessTreeOrder (`tests/test_mainwindow.cpp`) |
| `PTREE_DBLCLICK_INSPECTOR` | 双击打开检查器参数页 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testProcessTreeDoubleClickOpensInspectorParams (`tests/test_mainwindow.cpp`) |
| `PTREE_REORDER` | 节点排序/移动 | 直接工具 | 4 | DIRECT_TOOL | EVIDENCE_LINKED | testG4AgentMoveUndoKeepsProcessTreeOrder (`tests/test_mainwindow.cpp`) |
| `CANVAS_SELECT_MOVE` | 框选/移动节点(位置持久化) | 未支持 | 4 | UNSUPPORTED | EVIDENCE_LINKED | testG4GuiNodeDragPersistsAndUndoRedoReopensExactPosition (`tests/test_mainwindow.cpp`) |
| `CANVAS_CONNECT_PORTS` | 端口拖拽连线/断开 | 直接工具 | 4 | DIRECT_TOOL | EVIDENCE_LINKED | testFlowCanvasPortDragAddsProjectConnection (`tests/test_mainwindow.cpp`) |
| `INSP_PIN` | 显式固定/取消固定当前检查器 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testInspectorPinIsExplicitStrictAndReadOnly (`tests/test_agent_inspector_pin.cpp`) |
| `INSP_COLLAPSE` | 折叠/展开检查器 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testInspectorCollapseAndVisibilityPreserveProjectSelection (`tests/test_agent_inspector_pin.cpp`) |
| `INSP_VISIBILITY` | 关闭/重新打开检查器 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testInspectorCollapseAndVisibilityPreserveProjectSelection (`tests/test_agent_inspector_pin.cpp`) |
| `INSP_TAB_PARAMS` | 参数页编辑(Schema 生成控件) | 直接工具 | 2 | DIRECT_TOOL | EVIDENCE_LINKED | testG4AgentGuiParameterUndoIsKeyScopedAndStaleSafe (`tests/test_mainwindow.cpp`) |
| `INSP_TAB_RESULTS` | 结果页查看模块输出 | 未支持 | 3 | UNSUPPORTED | EVIDENCE_LINKED | testSetOutputShowsResultsData (`tests/test_moduleinspectorpanel.cpp`) |
| `INSP_ENABLED` | 启用/禁用当前节点 | 直接工具 | 4 | DIRECT_TOOL | EVIDENCE_LINKED | testG4SetModuleEnabledSyncsEngineAndUndo (`tests/test_agentactor.cpp`) |
| `INSP_BREAKPOINT` | 设置/取消断点 | 直接工具 | 4 | DIRECT_TOOL | EVIDENCE_LINKED | testG4SetBreakpointSyncsEngineAndUndo (`tests/test_agentactor.cpp`) |
| `INSP_NOTE` | 编辑节点备注 | 直接工具 | 4 | DIRECT_TOOL | EVIDENCE_LINKED | testG4SetModuleNoteSupportsClearAndUndo (`tests/test_agentactor.cpp`) |
| `INSP_RERUN` | 重跑当前模块 | 未支持 | 4 | UNSUPPORTED | EVIDENCE_LINKED | testInspectorRerunButtonRunsWholeFlow (`tests/test_mainwindow.cpp`) |
| `INSP_ADVANCED_CONFIG` | 高级配置弹窗(草稿副本编辑,单条可撤销提交) | 通用工具组合 | 2 | TOOL_COMPOSITION | EVIDENCE_LINKED | testCropImageAdvancedConfigDialogGuardsInvalidEdit (`tests/test_mainwindow.cpp`) |
| `INSP_RESET_PARAMS` | 恢复默认参数 | 未支持 | 2 | UNSUPPORTED | EVIDENCE_LINKED | testG4ResetDefaultsIsOneAtomicUndoCommand (`tests/test_mainwindow.cpp`) |
| `DS_IMPORT` | 导入数据源并加载到主视图 | 通用工具组合 | 6 | TOOL_COMPOSITION | EVIDENCE_LINKED | testImportImageMenuActionAddsAndDisplaysDataSource (`tests/test_mainwindow.cpp`) |
| `DS_BIND_GRABIMAGE` | 将图像路径绑定到指定 GrabImage 实例 | 通用工具组合 | 6 | TOOL_COMPOSITION | EVIDENCE_LINKED | testG4NewImageImportIsRejectedWhileBusyThenCommitsAfterStop (`tests/test_mainwindow.cpp`) |
| `DS_LIST` | 列举工程已登记数据源 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testDataSourceListAndReferenceRemovalPreserveFile (`tests/test_agentactor.cpp`) |
| `DS_DISPLAY_MAIN` | 在主视口显示已登记数据源 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentDataSourceDisplayAndVisibilityTools (`tests/test_mainwindow.cpp`) |
| `DS_VIEW_STATE` | 查询主视图显示与数据源可见状态 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentDataSourceDisplayAndVisibilityTools (`tests/test_mainwindow.cpp`) |
| `DS_SET_VISIBILITY` | 设置主视图数据源可见性 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentDataSourceDisplayAndVisibilityTools (`tests/test_mainwindow.cpp`) |
| `DS_DISPLAY_NEW_VIEWPORT` | 在新视口显示数据源 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testDedicatedViewportLoadsWithoutChangingPrimaryAndHonorsConfirmationSnapshots (`tests/test_agent_datasource_dedicated_viewport.cpp`) |
| `DS_SHOW_IN_FOLDER` | 在文件夹中显示 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testConfirmedRequestOpensExactParentAndKeepsPathPrivate (`tests/test_agent_datasource_folder_handoff.cpp`) |
| `DS_COPY_PATH` | 复制路径 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testDedicatedViewportLoadsWithoutChangingPrimaryAndHonorsConfirmationSnapshots (`tests/test_agent_datasource_dedicated_viewport.cpp`) |
| `DS_DELETE_REF` | 移除数据源引用(不删磁盘文件) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentDataSourceRemovalRefreshesGuiAndRejectsBusyMutation (`tests/test_mainwindow.cpp`) |
| `VP_ZOOM_FIT` | 放大/缩小/适应窗口/1:1 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testSchemaActionsPermissionsAndZoomBehavior (`tests/test_agent_viewport_zoom.cpp`) |
| `VP_POINT_SIZE` | 点云点大小调整 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testPointSizeSchemaPermissionsAndValidation (`tests/test_agent_viewport_point_size.cpp`) |
| `VIEWPORT_LIST` | 列举视口 ID、模式、可见性与显示内容供测量目标选择 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentQuickMeasurementLifecycleAndTargetIsolation (`tests/test_mainwindow.cpp`) |
| `VP_TOGGLE_2D3D` | 2D/3D 视图切换 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testDualViewportModeSwitchIsExplicitAndIdempotent (`tests/test_agent_viewport_display_mode.cpp`) |
| `VP_SNAPSHOT` | 保存视图快照 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAdvisorConfirmsThenUserSaves2dImageWithoutPathDisclosure (`tests/test_agent_viewport_snapshot_handoff.cpp`) |
| `VP_CLOSE` | 关闭当前视口 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testCloseViewportKeepsLazyPrimaryAndDestroysAdditional (`tests/test_agent_viewport_render_mode.cpp`) |
| `MEAS_PP` | 两点距离(2D/3D) | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testGuiQuickMeasureDistanceUsesImageLocalCoordinatesAfterViewportTransform (`tests/test_mainwindow.cpp`) |
| `MEAS_PL` | 点到线距离 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testGuiQuickMeasurePointLineAndSegmentGapUseImageLocalCoordinatesAfterViewportTransform (`tests/test_mainwindow.cpp`) |
| `MEAS_LL` | 两线段间距 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testGuiQuickMeasurePointLineAndSegmentGapUseImageLocalCoordinatesAfterViewportTransform (`tests/test_mainwindow.cpp`) |
| `MEAS_ANGLE3` | 三点角度 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testGuiQuickMeasureAngleAndCircleUseImageLocalCoordinatesAfterViewportTransform (`tests/test_mainwindow.cpp`) |
| `MEAS_FIT_CIRCLE3` | 三点拟合圆 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testGuiQuickMeasureAngleAndCircleUseImageLocalCoordinatesAfterViewportTransform (`tests/test_mainwindow.cpp`) |
| `MEAS_RECT_AREA` | 矩形面积(拖拽) | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testGuiQuickMeasureRectangleUsesImageLocalCoordinatesAndSingleRoiConnection (`tests/test_mainwindow.cpp`) |
| `LOG_CLEAR` | 清空运行日志 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentClearRuntimeLogsRequiresConfirmationAndPreservesDiskAndActionHistory (`tests/test_mainwindow.cpp`) |
| `LOG_COLLAPSE` | 折叠/展开底部面板 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentWorkspacePanelVisibilityWithoutProjectAndInFocusMode (`tests/test_mainwindow.cpp`) |
| `LOG_FILTER` | 日志级别过滤菜单 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentLogLevelFilterIsBoundedToRuntimeTable (`tests/test_mainwindow.cpp`) |
| `LOG_TABS` | 运行日志/Agent 日志/终端页签切换 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentSelectsLogPanelTabWithoutProject (`tests/test_mainwindow.cpp`) |
| `TERMINAL_INPUT` | 内嵌终端交互(人类专用) | 人机协作 | 7 | HUMAN_HANDOFF | EVIDENCE_LINKED | testBuiltinDocsStaySourcedAndAuditArtifactsStayExcluded (`tests/test_agentphase0_repro.cpp`) |
| `CHAT_SEND_TEXT` | 发送文本消息 | 人机协作 | 7 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentComposerPreservesDraftAndAttachmentsWhileBusy (`tests/test_mainwindow.cpp`) |
| `CHAT_ATTACH_IMAGE` | 附加图片(≤4 张,超限提示) | 人机协作 | 7 | HUMAN_HANDOFF | EVIDENCE_LINKED | attachmentCapShowsUserFeedback (`tests/test_agent_draft_restore.cpp`) |
| `CHAT_SCROLL_LATEST` | 回到最新消息 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentScrollsConversationToLatestWithoutProject (`tests/test_mainwindow.cpp`) |
| `CHAT_TOOL_CONFIRM` | 工具预览卡确认/拒绝 | 人机协作 | 1 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentClearRuntimeLogsRequiresConfirmationAndPreservesDiskAndActionHistory (`tests/test_mainwindow.cpp`) |
| `AGENTLOG_VIEW` | 审计日志查看 | 未支持 | 7 | UNSUPPORTED | EVIDENCE_LINKED | testAgentSelectsLogPanelTabWithoutProject (`tests/test_mainwindow.cpp`) |
| `SAM_ENV_SETUP` | 打开 SAM 环境设置并检查前置条件 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testOpenSamEnvironmentSetupReportsMissingEnvironmentWithScreenshots (`tests/test_mainwindow.cpp`) |
| `SAM_OPEN_IMAGE` | 打开图片 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testUserOpensImageFromSamDialogFilePicker (`tests/test_samannotatordialog.cpp`) |
| `SAM_INIT_ENV` | 初始化 Python 环境 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenSamEnvironmentSetupOpensAndReusesWithoutStartingBackend (`tests/test_mainwindow.cpp`) |
| `SAM_IMPORT_MODEL` | 导入 SAM 权重 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenSamEnvironmentSetupOpensAndReusesWithoutStartingBackend (`tests/test_mainwindow.cpp`) |
| `SAM_RESTART_SERVER` | 重启 SAM 服务 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenSamEnvironmentSetupOpensAndReusesWithoutStartingBackend (`tests/test_mainwindow.cpp`) |
| `SAM_PROMPT_POINTS_BOX` | 正点/负点/框选提示生成掩膜 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | samPredictSuccessAgainstTestServer (`tests/test_agent_sam_tools.cpp`) |
| `SAM_CANDIDATE_NAV` | 多掩膜候选上一个/下一个 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | candidateCyclingSelectsAlternateRegion (`tests/test_samannotatordialog.cpp`) |
| `SAM_MASK_OPS` | 编辑活动 SAM 会话掩膜:膨胀/腐蚀/橡皮擦/合并/拆分 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | dilateGrowsMaskAndUndoRestores (`tests/test_samannotatordialog.cpp`) |
| `SAM_CONFIRM_OBJECT` | 确认提示为标注对象(写入会话) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | samAnnotateWritesSessionAndAppends (`tests/test_agent_sam_tools.cpp`) |
| `SAM_CANCEL_PROMPT` | 取消当前提示(GUI 编辑态) | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentActiveSamPromptCancelPermissionsAndSnapshot (`tests/test_mainwindow.cpp`) |
| `SAM_CATEGORY_MGMT` | 添加/删除类别,对象标签修改 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testUserAddsAndRemovesSamCategoryThroughGui (`tests/test_samannotatordialog.cpp`) |
| `SAM_SESSION_SAVE` | 保存当前标注会话(.deeplux-anno.json) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentSaveActiveSamSessionAsNewFile (`tests/test_mainwindow.cpp`) |
| `SAM_SESSION_OPEN` | 打开既有标注会话 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testAgentOpenActiveSamSession (`tests/test_mainwindow.cpp`) |
| `SAM_SESSION_QUERY` | 查询指定标注会话及对象摘要 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | getAnnotationSessionReturnsBoundedPagedOriginalImageData (`tests/test_agent_sam_tools.cpp`) |
| `SAM_EXPORT` | 导出 LabelMe/YOLO/COCO | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | exportAnnotationSessionMatchesAllExistingFormatsAndRejectsStaleTargets (`tests/test_agent_sam_tools.cpp`) |
| `KB_SEARCH` | 按主题检索知识库(分节匹配) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testReadDocumentationSearchesBuiltInAndUserKnowledge (`tests/test_agentactor.cpp`) |
| `KB_LIST_DOCS` | 列举知识库文档(内置+用户) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testKnowledgeDocumentToolsManageOnlyCopiedUserDocumentsAndPageSearch (`tests/test_agentactor.cpp`) |
| `KB_PREVIEW_DOC` | 预览指定文档内容 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testKnowledgeDocumentToolsManageOnlyCopiedUserDocumentsAndPageSearch (`tests/test_agentactor.cpp`) |
| `KB_ADD_DOC` | 添加用户文档(复制入库) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testKnowledgeDocumentToolsManageOnlyCopiedUserDocumentsAndPageSearch (`tests/test_agentactor.cpp`) |
| `KB_REMOVE_DOC` | 移除用户文档(内置只读) | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testKnowledgeDocumentToolsManageOnlyCopiedUserDocumentsAndPageSearch (`tests/test_agentactor.cpp`) |
| `KB_OPEN_DIR` | 打开用户知识库目录 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testConfirmedOpenInitializesDirectoryAndHidesKnowledgePath (`tests/test_agent_knowledge_folder_handoff.cpp`) |
| `ASET_LLM_CONFIG` | 端点/API Key/模型/温度/maxTokens 配置与持久化 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenCancelManualSaveAndModalReuse (`tests/test_agent_settings_handoff.cpp`) |
| `ASET_PERMISSION_LEVEL` | 权限级别(Observer/Advisor/Autopilot) | 人机协作 | 1 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenCancelManualSaveAndModalReuse (`tests/test_agent_settings_handoff.cpp`) |
| `ASET_SYSTEM_PROMPT` | 系统提示词编辑 | 人机协作 | 7 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenCancelManualSaveAndModalReuse (`tests/test_agent_settings_handoff.cpp`) |
| `ASET_TOGGLES` | 工具/视觉/思考开关 | 人机协作 | 7 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpenCancelManualSaveAndModalReuse (`tests/test_agent_settings_handoff.cpp`) |
| `ASET_TEST_CONNECTION` | 测试 LLM 连接 | 人机协作 | 7 | HUMAN_HANDOFF | EVIDENCE_LINKED | testConnectionButtonSendsLocalRequestAndReportsResponse (`tests/test_agent_settings_handoff.cpp`) |
| `CAM_REFRESH` | 刷新相机列表(发现) | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCameraSetViewShowsAsyncDiscoveryAndRefreshResults (`tests/test_cameramanager.cpp`) |
| `CAM_CONNECT_DISCONNECT` | 连接/断开相机 | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCameraSetViewStartsAsyncOperationsAndRecoversErrors (`tests/test_cameramanager.cpp`) |
| `CAM_DELETE_CONFIG` | 删除相机配置 | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCameraSetViewDeleteAndApplyRemainGuiOnly (`tests/test_cameramanager.cpp`) |
| `CAM_APPLY_PARAMS` | 应用相机参数(IP/端口/协议) | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCameraSetViewDeleteAndApplyRemainGuiOnly (`tests/test_cameramanager.cpp`) |
| `COMM_ADD_DELETE` | 添加/删除通讯配置(TCP/串口/PLC) | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCommunicationViewAddsAndDeletesConfigThroughGui (`tests/test_communicationmanager.cpp`) |
| `COMM_CONNECT_DISCONNECT` | 连接/断开通讯通道 | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCommunicationViewConnectsAndDisconnectsLoopbackTcp (`tests/test_communicationmanager.cpp`) |
| `COMM_APPLY` | 应用通讯配置 | 未支持 | 5 | UNSUPPORTED | EVIDENCE_LINKED | testCommunicationSetViewApplyCreatesConfig (`tests/test_communicationmanager.cpp`) |
| `GV_CREATE` | 创建类型化全局变量 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testGlobalVariableTypedCrudIsLiteralAndFailClosed (`tests/test_agentactor.cpp`) |
| `GV_READ` | 列出变量元数据或读取单个变量值 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testGlobalVariableTypedCrudIsLiteralAndFailClosed (`tests/test_agentactor.cpp`) |
| `GV_SET` | 以严格类型字面值设置现有全局变量 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testGlobalVariableTypedCrudIsLiteralAndFailClosed (`tests/test_agentactor.cpp`) |
| `GV_REMOVE` | 移除现有全局变量 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testGlobalVariableTypedCrudIsLiteralAndFailClosed (`tests/test_agentactor.cpp`) |
| `GV_MOVE` | 上移/下移变量显示顺序 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpensReusesAndClosesWithoutReadingOrChangingVariables (`tests/test_agent_globalvars_handoff.cpp`) |
| `GV_EXECUTE` | 执行变量表达式 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpensReusesAndClosesWithoutReadingOrChangingVariables (`tests/test_agent_globalvars_handoff.cpp`) |
| `GV_APPLY` | 应用变量变更 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testOpensReusesAndClosesWithoutReadingOrChangingVariables (`tests/test_agent_globalvars_handoff.cpp`) |
| `SYS_GENERAL` | 自动加载/自动保存/间隔 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentSystemSettingsDialogLifecycleAndThemeSync (`tests/test_mainwindow.cpp`) |
| `SYS_DISPLAY` | 语言/主题设置 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentSystemSettingsDialogLifecycleAndThemeSync (`tests/test_mainwindow.cpp`) |
| `SYS_RUN_LOG` | 循环间隔/日志级别/文件日志 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentSystemSettingsDialogLifecycleAndThemeSync (`tests/test_mainwindow.cpp`) |
| `SYS_RESET_SAVE` | 重置/保存系统参数 | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testAgentSystemSettingsDialogLifecycleAndThemeSync (`tests/test_mainwindow.cpp`) |
| `LOGIN_DIALOG_OPEN` | 打开/复用登录窗口 | 直接工具 | 6 | DIRECT_TOOL | EVIDENCE_LINKED | testOpenUserLoginDialogOpensReusesAndPreservesUserState (`tests/test_mainwindow.cpp`) |
| `LOGIN_SUBMIT` | 用户登录提交(用户/密码) | 人机协作 | 6 | HUMAN_HANDOFF | EVIDENCE_LINKED | testUserSubmitsLoginThroughDialogUi (`tests/test_mainwindow.cpp`) |
| `STATUSBAR_INFO` | 运行状态/用户/时间显示 | 未支持 | 3 | UNSUPPORTED | EVIDENCE_LINKED | testStatusBarShowsRunDurationClockAndUserPlaceholder (`tests/test_mainwindow.cpp`) |

## 判定边界

- 插件 business/failure fixture 状态来自独立复审记录；源名匹配不升级为 fixture PASS。
- 操作证据未链接的行保留 `NOT_LINKED`；不从插件 fixture 或通用权限机制推断操作闭环。
- DefectDetection 算法 fixture 的失败、DirectShow/Hikvision 的目标环境限制和所有 0/69 现场状态均保持可见。
- 历史能力矩阵中的构建数及运行时状态只作为历史快照；当前 build options、二进制数量和 CTest 摘要来自当前构建与 G8 证据日志。
