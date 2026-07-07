# DeepLux Vision

跨平台机器视觉软件，基于 C++17 和 Qt 开发，支持通过插件扩展工业视觉能力。

## 功能特性

- 图像采集（相机/文件）
- 图像处理与保存
- 检测识别（二维码/条码/测量）
- 坐标标定
- 逻辑控制
- 外部通讯
- Agent 辅助创建和调整视觉流程

## 系统要求

### 必需组件

- **Qt 5.15.3+ 或 Qt 6.6+**，需包含 Core、Gui、Widgets、Network、Sql、Test、Concurrent、SerialPort

### 可选组件

- OpenCV
- Halcon Runtime 21.11+（当前默认构建未链接 Halcon；仅在接入 Halcon 插件/SDK 功能时需要）
- Basler Pylon SDK（跨平台相机支持）
- 海康威视 MVS SDK
- 其他相机 SDK

## 编译

### Linux

```bash
cmake -S . -B build -DDEEPLUX_QT_PATH=/path/to/Qt/6.6.0/gcc_64 -DBUILD_TESTS=ON
cmake --build build -j$(nproc)
```

### Windows

```batch
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.6.0/msvc2019_64
cmake --build . --config Release
```

## 测试和运行

```bash
ctest --test-dir build --output-on-failure
cmake --build build --target sync-plugins
./build/bin/DeepLux --gui
./scripts/deeplux help
```

`sync-plugins` 会把已构建插件同步到 `~/.deeplux/plugins`，用于验证运行时插件加载。Agent 能辅助创建流程、查询模块参数 schema、修改模块参数；工业现场部署前仍需要人工确认算法参数和流程结果。

## 项目结构

```
DeepLux_CPP/
├── cmake/              # CMake 模块
├── src/
│   ├── app/           # 主程序
│   ├── core/          # 核心库
│   ├── ui/            # UI 层
│   └── plugins/       # 插件
├── tests/             # 测试
├── docs/              # 文档
└── scripts/           # 构建脚本
```

## 许可证

MIT License

## 版本

当前版本: 1.0.0
