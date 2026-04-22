# DeepLux Vision

跨平台机器视觉软件，基于 Qt 6 + Halcon C++ 开发。

## 功能特性

- 图像采集（相机/文件）
- 图像处理与保存
- 检测识别（二维码/条码/测量）
- 坐标标定
- 逻辑控制
- 外部通讯

## 系统要求

### 必需组件

- **Halcon Runtime 21.11+**（免费，从 MVTec 官网下载）
- **Qt 6.6+**

### 可选组件

- Basler Pylon SDK（跨平台相机支持）
- 海康威视 MVS SDK
- 其他相机 SDK

## 编译

### Linux

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt/6.6.0/gcc_64
make -j$(nproc)
```

### Windows

```batch
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.6.0/msvc2019_64
cmake --build . --config Release
```

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
