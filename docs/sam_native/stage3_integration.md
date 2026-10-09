# SAM 原生接入：阶段 3 首批

本批将阶段 2 的 C++ ONNX 推理核心接入共享 `runSamPredictBatch()` 入口。Agent 的
`sam_predict` / `sam_annotate` 和 SamPredict 插件可显式设置 `serverUrl` 为 `native`；
默认值仍是 `http://127.0.0.1:8990`，现有 HTTP/Python 路径不变。

## 启用

1. 构建时设置 `-DDEEPLUX_NATIVE_SAM=ON` 并提供 `ONNXRUNTIME_DIR`。
2. 启动前设置 `DEEPLUX_SAM_PACKAGE=/path/to/sam1_vit_b_onnx`。
3. 在工具调用或插件参数中设置 `serverUrl=native`。

未启用原生构建、未配置模型包或模型包校验失败时明确报错，不回退到 HTTP。
同一工作线程复用模型会话与图像 embedding；换图或换模型由推理核心使缓存失效。

## 已验证范围

- 原生模型包上的点 prompt、多候选选择、Region2D/RLE 输出、Agent `sam_predict`
  工具调用、SamPredict 插件执行和错误路径由
  `test_sam_onnx_inference::nativeProductFlow` 覆盖。
- `DEEPLUX_NATIVE_SAM=OFF` 时显式 native 请求失败关闭；既有 HTTP 路径回归通过。
- 本批全量构建通过，CTest 77/77 通过。

## 尚未验收

- 当前原生会话为 CPU；CUDA Execution Provider、GUI 标注对话框切换及交互耗时门禁
  留待后续批次，不能将本批视为无 Python 的完整 GUI 交付。
- `timeoutMs` 和取消只在模型操作之间检查；正在执行的 ONNX Runtime 调用无法被抢占。
- 当前生成/验证机上的测试不代替目标机迁移部署与约 3000x3000 真实工业原图验收。
