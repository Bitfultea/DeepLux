# DeepLux Vision CLI 接口文档

## 概述

DeepLux Vision 支持两种运行模式：

- **GUI 模式**（默认）：启动图形界面
- **CLI 模式**：通过命令行执行操作，无 GUI

CLI 模式会在检测到命令参数时自动启用，也可用 `--cli` 强制启用。

## 快速开始

```bash
# 查看帮助
./DeepLux --help

# 查看版本
./DeepLux --version

# 强制 CLI 模式
./DeepLux --cli info
```

## 全局选项

| 选项 | 说明 |
|------|------|
| `-c, --cli` | 强制使用 CLI 模式（无 GUI） |
| `-g, --gui` | 强制使用 GUI 模式 |
| `-v, --verbose` | 详细输出 |
| `-h, --help` | 显示帮助信息 |
| `-V, --version` | 显示版本信息 |

## 命令列表

### 1. help - 显示帮助

显示帮助信息，可指定具体命令。

```bash
# 显示所有命令帮助
./DeepLux help

# 显示特定命令帮助
./DeepLux help add-module
```

### 2. version - 显示版本

显示 DeepLux Vision 版本、系统平台、Qt 版本和 OpenCV 状态。

```bash
./DeepLux version
```

**输出示例**：
```
DeepLux Vision 1.0.0
Platform: Linux
Qt Version: 5.15.2
OpenCV: 4.8.1
```

### 3. info - 系统信息

显示完整的系统信息，包括版本、插件目录、已加载插件数量等。

```bash
./DeepLux info
```

**输出示例**：
```
=== DeepLux Vision System Info ===
Version:    1.0.0
Platform:   Linux
Qt:         5.15.2
Plugin Dir: /home/user/.deeplux/plugins
Plugins:    31 loaded
================================
```

### 4. list-plugins - 列出插件

列出所有已加载的插件及其状态。

```bash
./DeepLux list-plugins
```

**输出示例**：
```
=== Loaded Plugins (31) ===
[OK] GrabImage v1.0.0 (image_processing)
[OK] ShowImage v1.0.0 (image_processing)
[OK] FindCircle v1.0.0 (detection)
...
=========================
```

### 5. connect-camera - 连接相机

连接指定 ID 的相机设备。

```bash
./DeepLux connect-camera <camera_id>
```

**参数**：
- `<camera_id>` （必需）：相机设备 ID

**示例**：
```bash
./DeepLux connect-camera GigE0
```

**输出**：
```
Camera connected: GigE0
```

### 6. list-commands - 列出所有命令

显示所有可用的 CLI 命令。

```bash
./DeepLux list-commands
```

### 7. create-project - 创建工程

创建新的 DeepLux 工程文件（.dproj）。

```bash
./DeepLux create-project <工程名称> [--path <保存目录>]
```

**参数**：
- `<工程名称>` （必需）：新工程的名称
- `--path <目录>` （可选）：工程保存路径，默认为当前目录

**示例**：
```bash
# 在当前目录创建工程
./DeepLux create-project MyProject

# 指定保存路径
./DeepLux create-project MyProject --path /home/user/projects
```

**输出**：
```
Created project: MyProject
Saved to: /home/user/projects/MyProject.dproj
```

### 7. add-module - 添加模块

向工程中添加一个模块实例。

```bash
./DeepLux add-module <工程文件> <模块类型> [--name <名称>] [--pos <x,y>]
```

**参数**：
- `<工程文件>` （必需）：.dproj 工程文件路径
- `<模块类型>` （必需）：插件模块类型（如 GrabImage、ShowImage 等）
- `--name <名称>` （可选）：模块显示名称，默认为模块类型名
- `--pos <x,y>` （可选）：模块在画布上的位置，默认为 (0, 0)

**可用模块类型**：
```
GrabImage, ShowImage, ShowPoint, Blob, FindCircle, FitLine, FitCircle,
MeasureLine, MeasureRect, MeasureGap, Matching, QRCode, ColorRecognition,
SaveImage, SaveData, WriteText, DisplayData, JigsawPuzzle, ImageScript,
PerProcessing, FreeformSurface, DistancePL, DistancePP, LinesDistance,
PointSurfaceDistance, Folder, SystemTime, TimeSlice, TableOutPut,
DataCheck, JiErHanDefectsDet
```

**示例**：
```bash
# 添加 GrabImage 模块
./DeepLux add-module MyProject.dproj GrabImage --name "Grab1" --pos 100,100

# 添加 ShowImage 模块
./DeepLux add-module MyProject.dproj ShowImage --name "Display1" --pos 300,100

# 添加检测模块
./DeepLux add-module MyProject.dproj FindCircle --name "FindCircle1" --pos 200,200
```

**输出**：
```
Added module 'Grab1' (GrabImage) to project
Module ID: a3154373-ea3a-4b14-9dec-a098f847a62d
Project saved.
```

### 8. connect - 连接模块

将两个模块连接起来，建立数据流。

```bash
./DeepLux connect <工程文件> <源模块ID> <目标模块ID>
```

**参数**：
- `<工程文件>` （必需）：.dproj 工程文件路径
- `<源模块ID>` （必需）：源模块的 UUID（从 `list-modules` 获取）
- `<目标模块ID>` （必需）：目标模块的 UUID

**示例**：
```bash
./DeepLux connect MyProject.dproj a3154373-ea3a-4b14-9dec-a098f847a62d a94a49fc-e033-4d6e-aa20-98f7f7a011c0
```

**输出**：
```
Connected Grab1 -> Display1
Project saved.
```

### 9. list-modules - 列出模块

显示工程中的所有模块及其连接关系。

```bash
./DeepLux list-modules <工程文件>
```

**参数**：
- `<工程文件>` （必需）：.dproj 工程文件路径

**示例**：
```bash
./DeepLux list-modules MyProject.dproj
```

**输出**：
```
=== Modules in MyProject (2) ===
[a3154373-ea3a-4b14-9dec-a098f847a62d] Grab1 (GrabImage) at (100, 100)
[a94a49fc-e033-4d6e-aa20-98f7f7a011c0] Display1 (ShowImage) at (300, 100)

=== Connections (1) ===
a3154373-ea3a-4b14-9dec-a098f847a62d -> a94a49fc-e033-4d6e-aa20-98f7f7a011c0
```

### 10. save-project - 保存工程

保存工程文件，可指定新路径进行另存。

```bash
./DeepLux save-project <工程文件> [--as <新路径>]
```

**参数**：
- `<工程文件>` （必需）：.dproj 工程文件路径
- `--as <新路径>` （可选）：另存为新路径

**示例**：
```bash
# 保存到原路径
./DeepLux save-project MyProject.dproj

# 另存为新路径
./DeepLux save-project MyProject.dproj --as /backup/MyProject_backup.dproj
```

### 11. run - 运行工程

打开工程文件。流程执行功能尚未完全实现，目前仅加载并显示工程信息。

```bash
./DeepLux run <工程文件> [--flow <流程名称>]
```

**参数**：
- `<工程文件>` （必需）：.dproj 工程文件路径
- `--flow <名称>` （可选）：指定要运行的流程名称

**示例**：
```bash
./DeepLux run MyProject.dproj
./DeepLux run MyProject.dproj --flow flow1
```

## 完整工作流示例

### 创建图像处理流程

假设要创建这样一个流程：

```
GrabImage → FindCircle → ShowImage
```

**步骤 1：创建工程**

```bash
./DeepLux create-project CircleDetection --path /tmp
```

**步骤 2：添加模块**

```bash
./DeepLux add-module /tmp/CircleDetection.dproj GrabImage --name "采集图像" --pos 50,100
./DeepLux add-module /tmp/CircleDetection.dproj FindCircle --name "找圆" --pos 250,100
./DeepLux add-module /tmp/CircleDetection.dproj ShowImage --name "显示结果" --pos 450,100
```

**步骤 3：连接模块**

首先查看模块 ID：
```bash
./DeepLux list-modules /tmp/CircleDetection.dproj
```

输出：
```
=== Modules in CircleDetection (3) ===
[id1] 采集图像 (GrabImage) at (50, 100)
[id2] 找圆 (FindCircle) at (250, 100)
[id3] 显示结果 (ShowImage) at (450, 100)
```

连接模块：
```bash
./DeepLux connect /tmp/CircleDetection.dproj <id1> <id2>
./DeepLux connect /tmp/CircleDetection.dproj <id2> <id3>
```

**步骤 4：验证流程**

```bash
./DeepLux list-modules /tmp/CircleDetection.dproj
```

**步骤 5：运行**

```bash
./DeepLux run /tmp/CircleDetection.dproj
```

## 工程文件格式

DeepLux 工程文件（.dproj）为 JSON 格式，包含以下结构：

```json
{
    "version": "2.0",
    "id": "uuid",
    "name": "工程名称",
    "created": "创建时间",
    "modified": "修改时间",
    "modules": [
        {
            "id": "模块UUID",
            "moduleId": "模块类型",
            "name": "显示名称",
            "posX": 100,
            "posY": 100,
            "params": {
                "__displayName": "采集图像",
                "otherParam": "value"
            }
        }
    ],
    "connections": [
        {
            "fromModuleId": "源模块UUID",
            "toModuleId": "目标模块UUID",
            "fromOutput": 0,
            "toInput": 0
        }
    ],
    "cameras": []
}
```

> **注意**：`params` 中的 `__displayName` 字段用于 GUI 显示名称的持久化，CLI 用户通常不需要手动设置此字段。

## 插件模块类型参考

### 图像采集
| 模块类型 | 说明 |
|----------|------|
| `GrabImage` | 相机采图 |

### 图像处理
| 模块类型 | 说明 |
|----------|------|
| `ShowImage` | 显示图像 |
| `ShowPoint` | 显示点 |
| `Blob` | Blob 检测 |
| `PerProcessing` | 预处理 |
| `ImageScript` | 图像脚本 |

### 检测
| 模块类型 | 说明 |
|----------|------|
| `FindCircle` | 找圆 |
| `Matching` | 模板匹配 |
| `QRCode` | 二维码识别 |
| `ColorRecognition` | 颜色识别 |
| `JiErHanDefectsDet` | 缺陷检测 |

### 几何测量
| 模块类型 | 说明 |
|----------|------|
| `FitLine` | 拟合直线 |
| `FitCircle` | 拟合圆 |
| `MeasureLine` | 测量线 |
| `MeasureRect` | 测量矩形 |
| `MeasureGap` | 测量间隙 |
| `FreeformSurface` | 自由曲面 |
| `DistancePL` | 点到线距离 |
| `DistancePP` | 点到点距离 |
| `LinesDistance` | 线到线距离 |

### 数据操作
| 模块类型 | 说明 |
|----------|------|
| `SaveImage` | 保存图像 |
| `SaveData` | 保存数据 |
| `WriteText` | 写入文本 |
| `DisplayData` | 显示数据 |
| `TableOutPut` | 表格输出 |

### 系统
| 模块类型 | 说明 |
|----------|------|
| `Folder` | 文件夹操作 |
| `SystemTime` | 系统时间 |
| `TimeSlice` | 时间切片 |
| `DataCheck` | 数据检查 |

## 常见问题

### Q: 工程文件路径有中文或空格怎么办？
A: 使用引号包裹路径：
```bash
./DeepLux add-module "/path/with spaces/My Project.dproj" GrabImage
```

### Q: 如何查看可用的模块类型？
A: 使用 `list-plugins` 命令查看所有已加载的插件。

### Q: CLI 模式和 GUI 模式有什么区别？
A: CLI 模式只输出文本结果，不启动图形界面，适合脚本和自动化。GUI 模式提供完整的图形界面操作。

### Q: 可以同时使用 CLI 和 GUI 吗？
A: 可以。默认情况下带参数运行会使用 CLI，但可以通过 `--gui` 强制启动 GUI。

## 许可

DeepLux Vision CLI 接口可被其他 AI 工具自由调用使用。
