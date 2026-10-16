# SAM 原生部署：阶段 5（Linux 安装与插件）

## 本批范围

`DEEPLUX_NATIVE_SAM=ON` 现在要求显式提供 `ONNXRUNTIME_DIR`（CMake 参数或同名环境变量），
并在配置阶段检查头文件和平台运行库。Linux 安装将 ONNX Runtime 主库及现有 Provider
复制为真实文件，版本符号链接改为包内相对链接；DeepLux、核心库和 UI 库使用相对
RUNPATH。Windows 配置识别 `lib/onnxruntime.lib` 及 `lib` 或 `bin` 中的
`onnxruntime.dll`，并从所选目录安装运行库；本批未在 Windows 构建或运行。
macOS 原生推理仍不支持。

第二批将所有本次构建的 Linux 插件目标及其 metadata 安装到 `plugins/<插件目录>`，
每个插件使用 `$ORIGIN/../../lib` RUNPATH；安装清单记录本次构建的插件集合。
加载器扫描包内插件后再扫描用户插件目录；同名用户插件加载或 ABI 校验失败时回退
包内版本。CLI 模式创建 `QCoreApplication`，因此从迁移后的
`bin/DeepLux` 运行时可定位旁边的 `plugins` 目录。安装验证脚本拒绝缺失 metadata、
库文件、非相对 RUNPATH、符号链接、未解析依赖或清单漏项。当前布局要求
`CMAKE_INSTALL_BINDIR=bin`，其他值在配置阶段拒绝。

这仍不是完整离线应用发行包：Qt/OpenCV、CUDA/cuDNN 和显卡驱动需目标系统提供；
可选相机 SDK 也需按设备单独部署。Python HTTP 服务仍保留为可选路径，但原生推理
不调用 Python。若启用 Hikvision 插件，保留原有 MVS SDK 路径，但当前严格安装验证
不会将含该绝对路径的插件判作可重定位包；需另行打包和验收相机 SDK。
Windows 插件打包与迁移尚未在本批验证。

第三批在原生推理构建的 Linux 包中安装 `verify-sam-native-runtime.sh` 和
`accept_sam_native_demo.json`。验收工程无需外部图像文件，使用
GrabImage 演示图 → SamPredict(native) → RegionStats 消费 Region2D；脚本隔离
`HOME`/用户插件目录、清空 `LD_LIBRARY_PATH`。同一工程先用空模型目录做
`manifest.json` 失败负对照（只接受 CLI 退出码 1），再用指定模型包运行；
两次运行均有 120 秒上限和 TERM 后 5 秒强制结束。超时、负对照失效或正例失败
均返回非零。
模型包仍单独提供，不随程序安装。

## 生成机复现

```bash
cmake -S . -B build -DDEEPLUX_NATIVE_SAM=ON -DONNXRUNTIME_DIR=/path/to/onnxruntime
cmake --build build -j4
cmake --install build --prefix /tmp/deeplux-native-install
bash scripts/verify-sam-native-install.sh /tmp/deeplux-native-install
/tmp/deeplux-native-install/bin/verify-sam-native-runtime.sh /path/to/sam1_vit_b_onnx cpu
```

模型包独立迁移并保持 `manifest.json`、`encoder.onnx`、`decoder.onnx` 原样；运行时设置
`DEEPLUX_SAM_PACKAGE=/path/to/sam1_vit_b_onnx`，选用 CUDA 时另设
`DEEPLUX_SAM_DEVICE=cuda`。复制模型包后需重新校验 manifest 内的大小与 SHA-256，
不能只凭文件名确认模型身份。

## 本机证据与剩余门禁

- CPU SDK 安装两次均成功，`install_manifest.txt` 包含主库与相对符号链接；
  `DESTDIR` 暂存安装的清单仍统一记录逻辑安装前缀。
  `readelf` 显示应用 RUNPATH 为 `$ORIGIN/../lib`、核心/UI 库为 `$ORIGIN`。
  第一批清空 `LD_LIBRARY_PATH`、隔离 `HOME` 后，安装版及搬迁副本的
  `DeepLux --version` 均退出码 0；当时尚无业务插件。
- 第二批 CMake 安装包含 66 个插件目录（65 个模块 + 1 个 V4L2 相机）；
  每个目录含动态库和 metadata，插件库 RUNPATH 均为相对路径。
  在隔离 `HOME` 和清空 `LD_LIBRARY_PATH` 下，安装版 `DeepLux list-plugins`
  能扫描包内模块；`DeepLux run tests/acceptance/projects/accept_distancepp.json`
  实际加载 MeasurementInput 和 DistancePP 并执行成功（退出码 0）。
- 第三批将安装包复制到不同路径后，执行包内脚本，真实 ViT-B CPU 模型完成
  GrabImage → SamPredict → RegionStats 工程，退出码 0；用无 manifest 的目录替换
  模型包，原生插件报错且脚本退出码 1。此项证明当前机器上搬迁后的原生 CPU
  产品路径可运行，不是无 Python 系统或目标 NVIDIA 机台验收。
- GPU SDK 的 1.23.2 主库、CUDA Provider（约 416 MiB）和 shared Provider 均
  复制为包内真实文件；`LD_LIBRARY_PATH` 仅指向安装包 `lib` 时，
  `cudaProvider` 与 `cudaDualSessionShutdown` 4/4 通过，退出码 0。
- 上述运行仍依赖本机 Qt/OpenCV、CUDA 12/cuDNN 9 和驱动；未在另一台目标机、
  无 Python 的纯净系统或 Windows 上验收。真实约 3000×3000 工业原图也尚未取得。
  这些均不得由本批本机测试替代为生产 PASS。
- `DEEPLUX_NATIVE_SAM=OFF` 的插件管理/标注对话框/掩膜定向 CTest 3/3 通过；
  CPU ON 全量 CTest 78/78 通过。错误 SDK 路径在配置阶段按预期拒绝；
  验收脚本对包外绝对/相对越界链接、缺失 UI 库均以退出码 1 拒绝。
