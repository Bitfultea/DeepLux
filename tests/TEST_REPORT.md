# 单元测试报告

更新时间: 2026-06-25

## 测试环境

- 操作系统: Linux (Ubuntu 22.04)
- Qt: CMake 检测为 5.15.3，QtTest 运行库输出为 5.15.2
- OpenCV: 4.8.1
- 测试框架: Qt Test + CTest
- 无显示环境: `tests/CMakeLists.txt` 已为所有测试注册 `QT_QPA_PLATFORM=offscreen`

## 运行命令

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

## 本次结果

```
100% tests passed, 0 tests failed out of 34
Total Test time (real) = 0.60 sec
```

## 测试目标

| 序号 | 测试目标 | 状态 |
|---:|---|---|
| 1 | test_pathtutils | 通过 |
| 2 | test_modulebase | 通过 |
| 3 | test_mainviewmodel | 通过 |
| 4 | test_mainwindow | 通过 |
| 5 | test_project | 通过 |
| 6 | test_configmanager | 通过 |
| 7 | test_pluginmanager | 通过 |
| 8 | test_plugincontracts | 通过 |
| 9 | test_logger | 通过 |
| 10 | test_imagedata | 通过 |
| 11 | test_runengine | 通过 |
| 12 | test_fitline | 通过 |
| 13 | test_findcircle | 通过 |
| 14 | test_measureline | 通过 |
| 15 | test_fitcircle | 通过 |
| 16 | test_measurerect | 通过 |
| 17 | test_distancepl | 通过 |
| 18 | test_distancepp | 通过 |
| 19 | test_linesdistance | 通过 |
| 20 | test_terminalscreen | 通过 |
| 21 | test_ansiparser | 通过 |
| 22 | test_clihandler | 通过 |
| 23 | test_cameramanager | 通过 |
| 24 | test_plyloader | 通过 |
| 25 | test_tiffloader | 通过 |
| 26 | test_flowcanvas | 通过 |
| 27 | test_propertypanel | 通过 |
| 28 | test_toolschema | 通过 |
| 29 | test_agentobserver | 通过 |
| 30 | test_agentactor | 通过 |
| 31 | test_agentpermissions | 通过 |
| 32 | test_agentundo | 通过 |
| 33 | test_agentcontroller | 通过 |
| 34 | test_variable_system_plugins | 通过 |

## 本次新增/修复覆盖

- `test_configmanager`: 配置持久化使用临时 app data 目录，避免依赖用户 HOME 可写。
- `test_pathtutils`: 覆盖 `DEEPLUX_APP_DATA_DIR` 环境变量覆盖路径。
- `test_cameramanager`: 覆盖无插件时刷新相机不能死锁，CTest 超时为 2 秒。
- `test_plyloader`: 覆盖 ASCII PLY RGB 属性必须写入点云颜色数组。
- `test_tiffloader`: 覆盖 16-bit 彩色 TIFF 必须按真实像素深度读取颜色。
- `test_flowcanvas`: 覆盖画布节点/连接同步 Project，以及从 Project 重建稳定节点和连接。
- `test_modulebase`: 覆盖部分参数加载和反序列化时必须保留插件默认参数。
- `test_plugincontracts`: 覆盖代表性算法、图像处理、检测、系统、变量插件的临时部署、动态加载、独立实例、参数校验、配置控件和 clone 参数保留契约。
- `test_fitline` / `test_fitcircle` / `test_measureline`: 覆盖算法插件拒绝非法阈值、迭代次数、半径、长度和角度参数。
- `test_distancepp` / `test_distancepl`: 覆盖非数字坐标输入必须失败，避免静默转换为 0 后继续几何计算。
- `test_variable_system_plugins`: 覆盖 CreateString、SplitString、Math、SystemTime 的输出行为、参数校验、除零拒绝和非法操作数拒绝。
- `test_clihandler`: 覆盖 CLI `run` 的工程路径校验、缺失文件、空工程失败和可加载插件工程执行。
- `test_runengine`: 覆盖按 Project 连接拓扑执行、缺失连接端点拒绝加载和连接环路拒绝加载。
- `test_propertypanel`: 覆盖编辑文本/数值/布尔参数会更新模块并发出信号，实例 ID 覆盖插件 ID，重复选择模块不会残留旧参数分组。
- `test_mainwindow`: 覆盖打开工程后流程树和 FlowCanvas 同步重建，关闭工程后同步清空；覆盖属性面板变更写回 Project，以及项目参数加载到运行时模块后在属性面板显示。
- `test_agentcontroller`: 覆盖 Agent 历史裁剪后仍保留 user 起点和 tool-call 配对关系。
