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
#define DISABLE_COPY(ClassName) \
    ClassName(const ClassName&) = delete; \
    ClassName& operator=(const ClassName&) = delete;

#define DISABLE_MOVE(ClassName) \
    ClassName(ClassName&&) = delete; \
    ClassName& operator=(ClassName&&) = delete;

#define DISABLE_COPY_AND_MOVE(ClassName) \
    DISABLE_COPY(ClassName) \
    DISABLE_MOVE(ClassName)
