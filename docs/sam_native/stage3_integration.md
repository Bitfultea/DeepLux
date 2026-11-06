# SAM 原生接入：阶段 3

首批将阶段 2 的 C++ ONNX 推理核心接入共享 `runSamPredictBatch()` 入口。Agent 的
`sam_predict` / `sam_annotate` 和 SamPredict 插件可显式设置 `serverUrl` 为 `native`；
默认值仍是 `http://127.0.0.1:8990`，现有 HTTP/Python 路径不变。
第二批在 SAM 标注对话框的“推理后端”中增加“本机 ONNX”选项；Python 服务仍为默认。

## 启用

1. 原生 SAM 默认参与构建；配置时提供 `ONNXRUNTIME_DIR`。不构建原生推理时显式设置 `-DDEEPLUX_NATIVE_SAM=OFF`。
2. 在标注对话框切换到“本机 ONNX”，点击“模型设置…”选择 SAM1 ViT-B 模型包；选择“自动”设备可优先使用 CUDA 并在 CUDA 初始化失败时回退 CPU。
3. 工具调用或插件参数设置 `serverUrl=native` 时，读取相同的应用级模型设置。自动化部署可用 `DEEPLUX_SAM_PACKAGE` 与 `DEEPLUX_SAM_DEVICE` 覆盖应用设置。

未启用原生构建、未配置模型包或模型包校验失败时明确报错，不回退到 HTTP。
同一工作线程复用模型会话与图像 embedding；换图或换模型由推理核心使缓存失效。
候选分数保留模型原始值（可能大于 1）；写入 `Region2D.score` 时才归一到
`[0,1]`，现有 HTTP 协议仍对越界分数失败关闭。

## 已验证范围

- 原生模型包上的点 prompt、多候选选择、Region2D/RLE 输出、Agent `sam_predict`
  工具调用、SamPredict 插件执行和错误路径由
  `test_sam_onnx_inference::nativeProductFlow` 覆盖。
- `DEEPLUX_NATIVE_SAM=OFF` 时显式 native 请求失败关闭；既有 HTTP 路径回归通过。
- 标注对话框经原生后端加载图像、点选、预览三候选并确认所选掩膜；换图取消旧预测、只接受新图结果。
- 原生 GUI 推理池由应用持有；关闭标注窗口只取消请求、丢弃结果，不等待正在执行的 ONNX 调用。
- 选中候选掩膜无效则失败关闭；非选中候选为空只跳过该预览，不丢弃有效的选中结果。
- 仅负点提示与正点/框提示同样进入原生推理；三类提示全空才先行拒绝。
- 本批 ON 构建及全量 CTest 78/78 通过，OFF 构建的标注对话框测试 57/57 通过；另有首次 GUI
  加载与长寿命线程池退出回归。

## 尚未验收

- 阶段 3 交付时原生会话仅支持 CPU；CUDA Execution Provider 和交互耗时门禁
  由[阶段 4](stage4_cuda.md)单独验证，目标机打包仍留待后续批次。
- `timeoutMs` 和取消只在模型操作之间检查；正在执行的 ONNX Runtime 调用无法被抢占。
- Agent GUI 主线程调用会转到固定工作线程，等待期间仍处理 Qt 事件；应用退出会等待在途推理完成。
- ONNX 会话持有运行时环境，避免并行插件等长寿命工作线程在环境析构后释放会话。
- 当前生成/验证机上的测试不代替目标机迁移部署与约 3000x3000 真实工业原图验收。
