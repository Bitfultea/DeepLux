# SAM 原生部署：阶段 5 第一批（Linux 安装路径）

## 本批范围

`DEEPLUX_NATIVE_SAM=ON` 现在要求显式提供 `ONNXRUNTIME_DIR`（CMake 参数或同名环境变量），
并在配置阶段检查头文件和平台运行库。Linux 安装将 ONNX Runtime 主库及现有 Provider
复制为真实文件，版本符号链接改为包内相对链接；DeepLux、核心库和 UI 库使用相对
RUNPATH。Windows 配置识别 `lib/onnxruntime.lib` 及 `lib` 或 `bin` 中的
`onnxruntime.dll`，并从所选目录安装运行库；本批未在 Windows 构建或运行。
macOS 原生推理仍不支持。

本批只解决运行库的安装可重定位性，不是完整离线应用发行包：当前 CMake 安装规则
仍只安装相机插件，不安装其余全部业务插件；Qt/OpenCV、CUDA/cuDNN 和显卡驱动
仍需目标系统提供。Python HTTP 服务仍保留为可选路径，但原生推理不调用 Python。

## 生成机复现

```bash
cmake -S . -B build -DDEEPLUX_NATIVE_SAM=ON -DONNXRUNTIME_DIR=/path/to/onnxruntime
cmake --build build -j4
cmake --install build --prefix /tmp/deeplux-native-install
bash scripts/verify-sam-native-install.sh /tmp/deeplux-native-install
```

模型包独立迁移并保持 `manifest.json`、`encoder.onnx`、`decoder.onnx` 原样；运行时设置
`DEEPLUX_SAM_PACKAGE=/path/to/sam1_vit_b_onnx`，选用 CUDA 时另设
`DEEPLUX_SAM_DEVICE=cuda`。复制模型包后需重新校验 manifest 内的大小与 SHA-256，
不能只凭文件名确认模型身份。

## 本机证据与剩余门禁

- CPU SDK 安装两次均成功，`install_manifest.txt` 包含主库与相对符号链接；
  `DESTDIR` 暂存安装的清单仍统一记录逻辑安装前缀。
  `readelf` 显示应用 RUNPATH 为 `$ORIGIN/../lib`、核心/UI 库为 `$ORIGIN`。
  清空 `LD_LIBRARY_PATH`、令 `HOME=/tmp` 后，安装版及搬迁副本的
  `DeepLux --version` 均退出码 0，日志显示当前隔离目录下 0 个插件。
- GPU SDK 的 1.23.2 主库、CUDA Provider（约 416 MiB）和 shared Provider 均
  复制为包内真实文件；`LD_LIBRARY_PATH` 仅指向安装包 `lib` 时，
  `cudaProvider` 与 `cudaDualSessionShutdown` 4/4 通过，退出码 0。
- 上述运行仍依赖本机 Qt/OpenCV、CUDA 12/cuDNN 9 和驱动；未在另一台目标机、
  无 Python 的纯净系统或 Windows 上验收。真实约 3000×3000 工业原图也尚未取得。
  这些均不得由本批本机测试替代为生产 PASS。
- `DEEPLUX_NATIVE_SAM=OFF` 的标注对话框/掩膜定向 CTest 2/2 通过；
  CPU ON 全量 CTest 78/78 通过。错误 SDK 路径在配置阶段按预期拒绝；
  验收脚本对包外绝对/相对越界链接、缺失 UI 库均以退出码 1 拒绝。
