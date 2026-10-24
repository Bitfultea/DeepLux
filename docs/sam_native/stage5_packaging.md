# SAM 原生部署：阶段 5（Linux 安装与插件）

## 本批范围

`DEEPLUX_NATIVE_SAM` 默认 `ON`，需要显式提供 `ONNXRUNTIME_DIR`（CMake 参数或同名环境变量）；
不需要原生推理时可设 `-DDEEPLUX_NATIVE_SAM=OFF`。启用时在配置阶段检查头文件和平台运行库。
Linux 安装将 ONNX Runtime 主库及现有 Provider
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

CUDA 安装验收须用含 `libonnxruntime_providers_cuda.so` 的 GPU SDK 单独构建安装包，
不能把 CPU-only SDK 构建的包切换为 `DEEPLUX_SAM_DEVICE=cuda`。在本机系统 Qt 环境中，
新建构建目录时显式传入 `-DQt5_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt5`，避免
自动选中本机另一套不兼容 OpenSSL 3 的 Qt 5.15.2。安装后将整个前缀复制到新路径，
在可访问 GPU 的环境执行包内脚本并选择 `cuda`；脚本的负对照仍必须先通过。

模型包独立迁移并保持 `manifest.json`、`encoder.onnx`、`decoder.onnx` 原样；GUI 用户在
SAM 标注页选择“本机 ONNX”后通过“模型设置…”选择模型包和设备，路径保存在应用配置中。
安装包若包含 `models/sam1_vit_b_onnx`，程序会自动发现；外置模型需用户首次选择。
自动设备优先尝试 CUDA，失败后使用 CPU；显式选择 CUDA 时不回退。自动化部署仍可用
`DEEPLUX_SAM_PACKAGE` 和 `DEEPLUX_SAM_DEVICE` 覆盖 GUI 配置。模型目录选择后会先检查清单和
文件结构；完整 SHA-256 与模型契约在首次推理加载时校验。模型文件不应提交到仓库。

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
- 第四批用上述 GPU SDK 全新配置、构建并安装 66 个插件，包迁移到另一目录后
  `verify-sam-native-install.sh` 通过；在本机 RTX 3090 可访问环境，包内
  `verify-sam-native-runtime.sh` 的 CPU 与 CUDA 产品流程均退出码 0。
  对照：CPU-only SDK 构建包请求 CUDA 会明确报 Provider 未启用；受限沙箱中
  GPU SDK 包报 `no CUDA-capable device is detected`，不能把这两个失败记为
  产品 CUDA 通过或替代目标机验证。
- 上述运行仍依赖本机 Qt/OpenCV、CUDA 12/cuDNN 9 和驱动；未在另一台目标机、
  无 Python 的纯净系统或 Windows 上验收。真实约 3000×3000 工业原图也尚未取得。
  这些均不得由本批本机测试替代为生产 PASS。
- `DEEPLUX_NATIVE_SAM=OFF` 的插件管理/标注对话框/掩膜定向 CTest 3/3 通过；
  CPU ON 全量 CTest 78/78 通过。错误 SDK 路径在配置阶段按预期拒绝；
  验收脚本对包外绝对/相对越界链接、缺失 UI 库均以退出码 1 拒绝。

第四批生成机复核命令（`MODEL_PACKAGE` 指向本机独立提供的 ViT-B ONNX 包，
`GPU_ONNXRUNTIME_DIR` 指向含 CUDA Provider 的 GPU SDK；本地路径不入库）：

```bash
cmake -S . -B /tmp/deeplux-sam-gpu-build -DQt5_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt5 \
  -DDEEPLUX_NATIVE_SAM=ON -DONNXRUNTIME_DIR="$GPU_ONNXRUNTIME_DIR" \
  -DBUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/deeplux-sam-gpu-build -j4
DESTDIR=/tmp/deeplux-sam-gpu-stage cmake --install /tmp/deeplux-sam-gpu-build --prefix /usr/local
bash scripts/verify-sam-native-install.sh /tmp/deeplux-sam-gpu-stage/usr/local
cp -a /tmp/deeplux-sam-gpu-stage/usr/local /tmp/deeplux-sam-gpu-relocated
/tmp/deeplux-sam-gpu-relocated/bin/verify-sam-native-runtime.sh "$MODEL_PACKAGE" cuda
```

上述步骤在本机同构临时目录实跑：配置、构建、安装、包结构校验、迁移后
CUDA smoke 均退出码 0；包结构校验输出 `PASS: relative RUNPATH, bundled
ONNX Runtime, and 66 packaged plugins`，包内脚本输出 `PASS: installed
GrabImage -> SamPredict(native/cuda) -> RegionStats workflow`。
额外直接运行迁移包 `DeepLux run` 捕获完整 CLI 输出，ORT 日志出现
`CUDAExecutionProvider`，末行 `Run completed: 2628 ms`，退出码 0。
本轮原始 CLI 日志暂存 `/tmp/dl_stage5_batch4_cuda_cli_raw.log`，SHA-256 为
`9c92bee2730581372dcac773d92348c7fb7761ec7b0cb00792c22771b526d265`；
临时日志不是仓库交付物，复核时应以上述命令重新生成。独立代码审核者的环境
无法访问 GPU，故本项仍是执行者在生成机上的实测，不是独立目标机 PASS。
