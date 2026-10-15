# SAM 原生 CUDA：阶段 4 首批

## 范围与使用

原生 SAM1 ONNX 路径现支持 `cpu` / `cuda` 显式设备选择。设置
`DEEPLUX_SAM_DEVICE=cuda` 后，`serverUrl=native` 的 Agent、插件及标注对话框
共用 CUDA Execution Provider；默认仍是 CPU。未编入原生推理、CUDA Provider
不可用或模型包加载失败时返回错误，不隐式降级到 CPU 或 HTTP。切换设备/模型会重建
ONNX 会话并清空 embedding 缓存；失败的重载不保留旧会话供后续误用。

这是生成/验证机上的阶段 4 首批验证，不是目标机部署验收。CUDA 版 ONNX Runtime
需同时提供核心库与 CUDA Provider 及其依赖；本批通过临时运行时目录链接本机已安装
的 1.23.2 GPU 库进行构建，未把该目录或本机路径作为发布物。跨机器打包和安装路径
留待阶段 5。

## 验证证据

- 生成/验证机：RTX 3090、驱动 580.178.04；在隔离 GPU 构建上，
  `DEEPLUX_TEST_CUDA=1 test_sam_onnx_inference cudaProvider cudaExecutionProfile`
  退出码 0。640×480 图首次推理约 305 ms、同图缓存命中约 8 ms；候选掩膜与
  PyTorch 基准的 IoU ≥0.99，分数误差 ≤0.01。
- ONNX Runtime profiling 记录 encoder 的 524 个 CUDA 节点（其中 63 个
  Conv/MatMul/Gemm）和 decoder 的 320 个 CUDA 节点（其中 54 个计算节点）。
  部分形状运算仍分配到 CPU，不宣称整张计算图都在 GPU。
- `DEEPLUX_TEST_CUDA_BENCH=1 DEEPLUX_SAM_DEVICE=cuda`
  `test_sam_onnx_inference cudaProduct3000Benchmark`：3000×3000 **确定性合成图**，
  一次预热后同图运行完整 `runSamPredictBatch(native, includeCandidates=true)` 100 次；
  P50 310.764 ms、P95 320.411 ms，低于本批 500 ms 门限。原始 100 次值、
  分位数和退出码见 [结构化证据](stage4/product_synthetic3000_benchmark.json)；测试本身
  也输出原始值，可重复生成。RLE 按 64 列分块转置，跨块及带行填充的列主序等价回归通过。
- CUDA 标注对话框的选中候选确认、仅负点提示和在途关闭回归 5/5；CPU ON 完整构建及
  CTest 78/78；`DEEPLUX_NATIVE_SAM=OFF` 的标注对话框/掩膜定向 CTest 2/2。
  独立代码复审仅对阶段 4 首批差异给出 PASS，不等于整阶段或目标机验收。

## 未关闭的门禁

- 真实约 3000×3000 工业原图及细小缺陷样本尚未提供，本批 P95 只说明合成图同图
  缓存后的交互路径；不代表真实图、首次编码或不同图连续切换的 P95。
- 当前 RTX 3090 机器按生成/验证机处理。目标 NVIDIA 机台的驱动、Provider 依赖、
  无 Python 安装包、迁移部署和现场吞吐需阶段 5 独立验证。
- 取消仍在 ONNX 操作之间响应；正在执行的单个 ONNX Runtime 调用不可抢占。
