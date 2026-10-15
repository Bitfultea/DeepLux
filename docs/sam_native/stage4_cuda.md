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

## 真实图补充验证与退出生命周期

用户提供的 6 张真实 800×565 工业图逐文件核对阶段 1 报告中的 SHA-256，
再按同名图和提示记录复测：每图从点、
正负点、框三类中各取一条参考首位分数优势最大的用例，共 18 条。CUDA 结果对照
阶段 1 ONNX 参考分数和包围盒，同时与本 C++ CPU 路径逐像素比较首位掩膜。
全部通过：最大分数差 0.001800（门限 0.01）、最大包围盒差 1 px、最低掩膜
IoU 0.996429（门限 0.99）。逐项数据与退出码见
[真实图对照证据](stage4/real_800x565_cuda_parity.json)。这不是对 PyTorch 掩膜的
直接 C++ 比较；阶段 1 的 PyTorch/ONNX 对照另见
[原始报告](stage1/compare_real_report.json)。

补测时发现长序列 CUDA 推理可在全部用例 PASS 后、进程退出时触发堆损坏：
CUDA-only 3 次中 1 次失败；同一二进制保留 `ReleaseEnv` 的 5 次中 3 次失败，
仅跳过进程退出时的 `ReleaseEnv` 则 10/10 正常。故将 ORT 环境设为进程生命周期
单例，逐次模型会话仍正常释放；操作系统在进程终止时回收环境。清除诊断开关后，
完整 18 例连续 5/5 次退出码 0，另有单图双会话退出回归。该 A/B 证据将故障定位
到当前 GPU 运行时的环境退出释放路径，但不声称已修复运行时库内部实现。
最终生命周期代码下，完整产品路径再跑 100 次，P50 300.933 ms、P95 311.045 ms；
原始样本和退出码见[复测证据](stage4/product_synthetic3000_after_env_fix.json)。

## 未关闭的门禁

- 真实约 3000×3000 工业原图及细小缺陷样本尚未提供，本批 P95 只说明合成图同图
  缓存后的交互路径；800×565 真实图验证也不代表全分辨率图、首次编码或不同图
  连续切换的 P95。
- 当前 RTX 3090 机器按生成/验证机处理。目标 NVIDIA 机台的驱动、Provider 依赖、
  无 Python 安装包、迁移部署和现场吞吐需阶段 5 独立验证。
- 取消仍在 ONNX 操作之间响应；正在执行的单个 ONNX Runtime 调用不可抢占。
