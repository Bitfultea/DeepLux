# 单元测试报告 v2

## 测试环境

- 操作系统: Linux (Ubuntu 22.04)
- Qt 版本: 5.15.3
- 编译器: GCC 11.3.0
- 测试框架: Qt Test

## 测试结果总览

```
Test project /home/charles/Data/Repo/DeepLux_CPP/build
    Start 1: test_pathtutils
1/6 Test #1: test_pathtutils ..................   Passed    0.02 sec
    Start 2: test_modulebase
2/6 Test #2: test_modulebase ..................   Passed    0.02 sec
    Start 3: test_mainviewmodel
3/6 Test #3: test_mainviewmodel ...............   Passed    0.02 sec
    Start 4: test_project
4/6 Test #4: test_project .....................   Passed    0.02 sec
    Start 5: test_configmanager
5/6 Test #5: test_configmanager ...............   Passed    0.03 sec
    Start 6: test_pluginmanager
6/6 Test #6: test_pluginmanager ...............   Passed    0.02 sec

100% tests passed, 0 tests failed out of 6
Total Test time (real) =   0.13 sec
```

## 测试用例详情

### 1. test_pathtutils (11 tests)
路径工具类测试

| 测试用例 | 状态 |
|----------|------|
| testAppDataPath | ✅ |
| testPluginPath | ✅ |
| testConfigPath | ✅ |
| testLogPath | ✅ |
| testProjectPath | ✅ |
| testNormalize | ✅ |
| testJoin | ✅ |
| testEnsureDirExists | ✅ |
| testApplicationDirPath | ✅ |

### 2. test_modulebase (10 tests)
模块基类测试

| 测试用例 | 状态 |
|----------|------|
| testModuleInfo | ✅ |
| testInitialize | ✅ |
| testShutdown | ✅ |
| testExecute | ✅ |
| testExecuteWithoutInit | ✅ |
| testExecuteTwice | ✅ |
| testParams | ✅ |
| testSerialization | ✅ |

### 3. test_mainviewmodel (7 tests)
主视图模型测试

| 测试用例 | 状态 |
|----------|------|
| testInstance | ✅ |
| testNewProject | ✅ |
| testProjectName | ✅ |
| testModified | ✅ |
| testCurrentTime | ✅ |

### 4. test_project (9 tests) 🆕
项目模型测试

| 测试用例 | 状态 |
|----------|------|
| testCreate | ✅ |
| testName | ✅ |
| testModules | ✅ |
| testConnections | ✅ |
| testCameras | ✅ |
| testSerialization | ✅ |
| testSaveLoad | ✅ |

### 5. test_configmanager (9 tests) 🆕
配置管理器测试

| 测试用例 | 状态 |
|----------|------|
| testInitialize | ✅ |
| testSetValue | ✅ |
| testGroupValue | ✅ |
| testTypedGetters | ✅ |
| testDefaults | ✅ |
| testPersistence | ✅ |
| testRemove | ✅ |

### 6. test_pluginmanager (5 tests) 🆕
插件管理器测试

| 测试用例 | 状态 |
|----------|------|
| testInitialize | ✅ |
| testPluginPaths | ✅ |
| testScanPlugins | ✅ |
| testAvailableModules | ✅ |
| testPluginInfo | ✅ |

## 总结

| 指标 | 数值 |
|------|------|
| 测试模块数 | 6 |
| 总测试用例数 | 51 |
| 通过 | 51 |
| 失败 | 0 |
| **通过率** | **100%** |

## 新增模块

本次开发新增了以下核心模块：

1. **Project** - 项目模型，支持模块、连接、相机配置的序列化
2. **ProjectManager** - 项目管理器，支持新建、打开、保存、最近项目列表
3. **ConfigManager** - 配置管理器，支持分组配置和默认值
4. **PluginManager** - 插件管理器，支持扫描、加载、创建插件实例

## 编译产物

| 文件 | 大小 |
|------|------|
| libdeeplux_core.so | ~340KB |
| libdeeplux_ui.so | ~150KB |
| DeepLux (主程序) | ~43KB |
| 测试程序 (6个) | ~570KB |
