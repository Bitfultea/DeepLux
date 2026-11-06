#pragma once

/**
 * @file Platform.h
 * @brief 平台相关宏定义
 */

// ========== 平台判断 ==========
#if defined(_WIN32) || defined(_WIN64)
#define DEEPLUX_PLATFORM_WINDOWS 1
#define DEEPLUX_PLATFORM_NAME "Windows"
#elif defined(__linux__)
#define DEEPLUX_PLATFORM_LINUX 1
#define DEEPLUX_PLATFORM_NAME "Linux"
#elif defined(__APPLE__)
#define DEEPLUX_PLATFORM_MACOS 1
#define DEEPLUX_PLATFORM_NAME "macOS"
#else
#error "Unsupported platform"
#endif

// ========== 导出宏 ==========
// Windows 下使用 __declspec，Linux/macOS 使用属性
#if defined(_WIN32) || defined(_WIN64)
#ifdef DEEPLUX_CORE_EXPORT
#define DEEPLUX_API __declspec(dllexport)
#else
#define DEEPLUX_API __declspec(dllimport)
#endif
#else
#define DEEPLUX_API __attribute__((visibility("default")))
#endif

// ========== 测试可见性 ==========
// 部分 Qt Test 用例需要直接驱动内部状态机/私有处理函数。Windows 的 MSVC
// 名字修饰会把成员访问级别编码进符号名，若只在测试侧用 `#define private
// public`，会与 DLL 中按 private 导出的符号不匹配（LNK2019）。测试构建时把
// 相关类的 private 段落统一扩为 public，保证 DLL 与测试的符号一致。
#if defined(DEEPLUX_BUILD_TESTS)
#define DEEPLUX_TEST_VISIBILITY public
#else
#define DEEPLUX_TEST_VISIBILITY private
#endif

// ========== 插件导出宏 ==========
// 插件目标以 DEEPLUX_PLUGIN_EXPORTS 编译（见顶层 CMakeLists.txt），插件类显式
// 导出。CMake 的 WINDOWS_EXPORT_ALL_SYMBOLS 不会导出全局数据符号，而 Qt 的
// staticMetaObject 正是数据符号；测试目标会直接链接插件库并实例化插件类，
// 缺少显式导出时会在 Windows 上出现 LNK2019。
#if defined(_WIN32) || defined(_WIN64)
#ifdef DEEPLUX_PLUGIN_EXPORTS
#define DEEPLUX_PLUGIN_API __declspec(dllexport)
#else
#define DEEPLUX_PLUGIN_API __declspec(dllimport)
#endif
#else
#define DEEPLUX_PLUGIN_API
#endif

// ========== 调试宏 ==========
#ifdef QT_DEBUG
#define DEEPLUX_DEBUG 1
#endif

// ========== 版本信息 ==========
#define DEEPLUX_VERSION_MAJOR 1
#define DEEPLUX_VERSION_MINOR 0
#define DEEPLUX_VERSION_PATCH 0
#define DEEPLUX_VERSION_STRING "1.0.0"

// ========== 禁用拷贝宏 ==========
#define DISABLE_COPY(ClassName)                                                                                        \
    ClassName(const ClassName&) = delete;                                                                              \
    ClassName& operator=(const ClassName&) = delete;

#define DISABLE_MOVE(ClassName)                                                                                        \
    ClassName(ClassName&&) = delete;                                                                                   \
    ClassName& operator=(ClassName&&) = delete;

#define DISABLE_COPY_AND_MOVE(ClassName)                                                                               \
    DISABLE_COPY(ClassName)                                                                                            \
    DISABLE_MOVE(ClassName)
