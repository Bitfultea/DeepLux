# Windows 构建与部署

本文记录 DeepLux 在 Windows（MSVC）上的构建、插件同步和可分发部署流程。Linux 流程见
[README](../README.md) 与 `scripts/deploy-linux.sh`。

## 1. 依赖

| 依赖 | 说明 |
| --- | --- |
| Visual Studio 2022/2026 | 需要 “使用 C++ 的桌面开发” 工作负载（MSVC x64 工具链 + Windows SDK） |
| CMake 3.16+ | Visual Studio 自带版本即可，从 “Developer PowerShell” 打开时在 `PATH` 中 |
| Qt 6.6+ | `msvc2022_64` 套件，必须包含 `Core Gui Widgets Network Sql Test Concurrent OpenGL OpenGLWidgets` 与 **SerialPort** 模块 |
| OpenCV 4.x | 官方 Windows 预编译包（`OpenCV_DIR` 指向 `<opencv>/build`）或 vcpkg `opencv4` |
| Eigen 3.4 | 头文件库；提供 `Eigen3Config.cmake`（vcpkg 或自行生成最小配置文件） |
| ONNX Runtime（可选） | 启用原生 SAM 推理时提供 `ONNXRUNTIME_DIR`（含 `include/onnxruntime_c_api.h`、`lib/onnxruntime.lib`、`lib|bin/onnxruntime.dll`） |

例如可从 Eigen 3.4 头文件目录生成一个最小 `Eigen3Config.cmake`：

```cmake
set(EIGEN3_INCLUDE_DIR "C:/deps/eigen-3.4.0")
set(Eigen3_FOUND TRUE)
if(NOT TARGET Eigen3::Eigen)
    add_library(Eigen3::Eigen INTERFACE IMPORTED)
    set_target_properties(Eigen3::Eigen PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${EIGEN3_INCLUDE_DIR}")
endif()
```

> VS 2022 及更新的 MSVC（如 19.5x）可以链接 OpenCV 官方 `vc16` 预编译库。若
> `OpenCVConfig.cmake` 因编译器版本不在映射表中报 “no binaries compatible”，
> 优先改用 vcpkg 自行编译的 OpenCV。
>
> 中文版 Visual Studio + Ninja 组合曾出现 `cl /showIncludes` 前缀探测异常，表现为修改
> 头文件后不触发重编。遇到时删除 `build-*` 重新配置（或改用 Visual Studio 生成器）。

## 2. 配置与编译

在 “Developer PowerShell for VS” 中执行：

```powershell
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DDEEPLUX_QT_PATH=C:/Qt/6.8.3/msvc2022_64 `
  -DOpenCV_DIR=C:/opencv/build `
  -DEigen3_DIR=C:/deps/eigen3 `
  -DBUILD_TESTS=OFF -DDEEPLUX_NATIVE_SAM=OFF

cmake --build build-win --parallel
```

也可以使用 Visual Studio 生成器（多配置）：

```powershell
cmake -S . -B build-win -G "Visual Studio 17 2022" -A x64 `
  -DDEEPLUX_QT_PATH=C:/Qt/6.8.3/msvc2022_64 -DOpenCV_DIR=C:/opencv/build `
  -DEigen3_DIR=C:/deps/eigen3 -DBUILD_TESTS=OFF -DDEEPLUX_NATIVE_SAM=OFF
cmake --build build-win --config Release --parallel
```

产物位于 `build-win\bin`：`DeepLux.exe`、`deeplux_core.dll`、各 `*Plugin.dll`
（默认 66 个；启用 `ENABLE_HYMSON3D=ON` 后为 67 个）。插件与主程序必须来自同一次构建，
接口 ABI 版本不匹配会被 `PluginManager` 拒绝加载。

## 3. 开发期插件同步

应用运行时从以下位置加载插件：

1. `<exe>/../plugins`（随包插件目录）；
2. `~/.deeplux/plugins`（用户插件目录，Windows 上同样是 `%USERPROFILE%\.deeplux\plugins`）。

开发期把新构建的插件同步到用户目录：

```powershell
cmake --build build-win --target sync-plugins
```

`scripts/sync-plugins.ps1` 会递归扫描 `build-win\bin`（含多配置的 `bin\Release`）或
`build-win\lib`，收集 `*Plugin.dll`，按 `metadata.json` 的目录名落到
`~/.deeplux/plugins/<PluginName>/`，并清理残留的旧 DLL。

验证（Visual Studio 生成器把 `bin` 换成 `bin\Release`）：

```powershell
build-win\bin\DeepLux.exe --cli info
build-win\bin\DeepLux.exe --cli list-plugins
```

PowerShell 直接调用 GUI 子系统的 `DeepLux.exe --cli` 不会等待进程；需要在构建目录测试
CLI 时用 `cmd /c build-win\bin\DeepLux.exe --cli info`，或使用部署包中的 `deeplux.cmd`。

从构建目录直接运行 GUI 前，需要把 Qt 与 OpenCV 的 `bin` 加入 `PATH`
（例如 `C:\Qt\6.8.3\msvc2022_64\bin`、`C:\opencv\build\x64\vc16\bin`；VS 生成器
的 exe 在 `build-win\bin\Release`），更简单的方式是使用第 4 节的部署目录。

## 4. 可分发部署

推荐在 Developer PowerShell 中执行；普通 PowerShell 下脚本会从 `CMakeCache.txt` 定位
CMake，并自动补齐 VC++ 运行库与 Windows SDK 的 DirectX 编译 DLL。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/deploy-windows.ps1 `
  -BuildDir build-win -Prefix dist\DeepLux-win64 -Zip
```

也可以在构建目录内直接使用 CMake 目标（输出到 `<build>/deploy/DeepLux-win64`）：

```powershell
cmake --build build-win --target deploy-windows
```

脚本执行：

1. `cmake --install` 生成 `<Prefix>\bin`、`<Prefix>\plugins\<PluginName>`、`<Prefix>\share\deeplux`、
   `<Prefix>\tools\sam_server`；
2. 调用 `windeployqt` 复制 Qt 运行库、平台插件（`platforms\qwindows.dll`）、TLS 与图像格式插件、
   VC 运行库；
3. 从 OpenCV 的多个运行库目录（`build\bin`、`build\x64\vc16\bin`、vcpkg `bin` 等）
   收集 Release 版 OpenCV DLL 并复制到 `<Prefix>\bin`（跳过 `opencv_*d.dll` 调试库）；
4. 在干净 `PATH` 下运行 `DeepLux.exe --cli list-plugins` 冒烟测试，插件数不足即失败。

成功输出包含 `plugins: 66` 与 `[SMOKE] exit=0 loaded=65`。`-Prefix` 目录会被删除后重建，
请指向专用输出目录。

Windows CI（`.github/workflows/ci.yml` 的 `windows-msvc` job）在编译后同样执行
`sync-plugins` 与 `deploy-windows`，覆盖安装树、运行时依赖和插件加载回归。

最终目录：

```text
dist\DeepLux-win64\
├── bin\DeepLux.exe          # GUI / CLI 主程序
├── bin\deeplux.cmd          # Windows CLI 包装脚本
├── bin\Qt6*.dll、opencv_world*.dll、platforms\qwindows.dll ...
├── plugins\<PluginName>\    # 随包插件 DLL + metadata.json
├── share\deeplux\           # 插件清单
└── tools\sam_server\        # 可选 Python SAM 服务
```

目标机器无需安装 Qt/OpenCV；若系统未装 Visual C++ 运行库，先运行一次
`bin\vc_redist.x64.exe`，然后运行 `bin\DeepLux.exe --gui`。

> PowerShell 调用 GUI 子系统的 `DeepLux.exe --cli ...` 时不会等待进程，
> 命令行模式请使用 `bin\deeplux.cmd --cli ...`（或 `Start-Process -Wait`），
> 否则可能看到空输出。`deeplux.cmd` 只是 `"%~dp0DeepLux.exe" %*` 的包装。

## 5. 运行测试

```powershell
cmake -S . -B build-win-tests -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DDEEPLUX_QT_PATH=C:/Qt/6.8.3/msvc2022_64 -DOpenCV_DIR=C:/opencv/build `
  -DEigen3_DIR=C:/deps/eigen3 -DBUILD_TESTS=ON -DDEEPLUX_NATIVE_SAM=OFF
cmake --build build-win-tests --parallel
ctest --test-dir build-win-tests --output-on-failure --timeout 300
```

本次在 Windows 上 77/77 通过。测试串口/相机依赖硬件时按用例内的跳过逻辑处理；
`ui_capture_mainwindow` 使用 offscreen 平台生成并自校验界面截图。

## 6. Windows 平台实现说明

* **用户数据目录**：`~/.deeplux`（与 Linux 一致，`DEEPLUX_APP_DATA_DIR` 可覆盖）；
  CLI 与 GUI 使用同一目录。
* **终端**：`BashProcess` 通过 ConPTY（`CreatePseudoConsole`）运行 `bash`/`cmd.exe`/`powershell.exe`，
  需要 Windows 10 1809+。
* **相机**：Windows 下只构建 `DirectShowCamera`；V4L2 仅 Linux。海康 MVS 插件目前仅在
  Linux（`ENABLE_CAMERA_HIKVISION=ON` + SDK）下构建，Windows 上打开该选项会给出明确提示，
  Windows MVS SDK 目录结构整合待补。
* **原生 SAM**：Windows 上已验证 `ONNXRUNTIME_DIR` 配置、`onnxruntime.lib` 链接、
  `onnxruntime.dll` 安装与程序启动；真实模型推理精度/性能尚未验收
  （见 `docs/sam_native/stage5_packaging.md`）。
* **多配置生成器**：Visual Studio / Ninja Multi-Config 的产物位于 `bin/<Config>`，
  `sync-plugins` 递归查找，`deploy-windows` 会自动探测配置目录并使用
  `cmake --install --config <Config>`。
* **测试**：`cmake -S . -B build-win-tests -G Ninja -DBUILD_TESTS=ON ...` 后可运行完整
  CTest；本次在 Windows 上 **77/77 测试通过**。测试所需的插件库/metadata 由顶层
  CMake 汇总为 `DEEPLUX_TEST_PLUGIN_MANIFEST`（`$<TARGET_FILE:...>` + 源码
  `metadata.json`），由 `tests/TestPluginRegistry.h` 解析，因此不再硬编码
  `lib*.so`，并兼容 Visual Studio 多配置输出目录。
* **测试内部访问**：少数 Agent 测试直接驱动内部状态机。MSVC 会把成员访问级别
  编进符号名，测试构建通过 `DEEPLUX_BUILD_TESTS` 把相关类的 private 段落统一
  扩为 public，保证 DLL 与测试的符号一致（见 `Platform.h` 的
  `DEEPLUX_TEST_VISIBILITY`）。

## 7. 已知问题

* OpenCV 官方 Windows 预编译包使用 `vc16` 目录；更新版本 MSVC 需要在 OpenCV 配置中
  接受 `vc16` 运行时，或改用 vcpkg 构建。
* SAM Python 服务在 Windows 上依赖系统 `python`（无 `python3`/`pip3` 别名）；应用会优先
  创建 `~/.deeplux/sam_env` 虚拟环境，再回退到 `python -m pip`。
* `windeployqt` 提示 “Translations will not be available” 属于未安装 Qt 翻译文件的正常提示，
  不影响运行。
* 中文版 Visual Studio + Ninja 组合曾出现 `cl /showIncludes` 前缀探测异常，表现为修改
  头文件后不触发重编。遇到时删除 `build-*` 重新配置（或改用 Visual Studio 生成器）。
* 海康 MVS 的 Windows SDK 目录结构与插件构建脚本尚未整合（见第 6 节）；Windows
  相机目前使用 DirectShow。
