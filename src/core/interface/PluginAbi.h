#pragma once

#include "../deeplux/DataContract.h"
#include "../engine/DisplayFrame.h"

#include <QByteArray>
#include <QtGlobal>

#define DEEPLUX_MODULE_INTERFACE_VERSION 2
#define DEEPLUX_CAMERA_PLUGIN_INTERFACE_VERSION 1
#define DEEPLUX_CAMERA_INTERFACE_VERSION 2
#define DEEPLUX_PLUGIN_ABI_VERSION 1
#define DEEPLUX_ABI_STRINGIFY_IMPL(value) #value
#define DEEPLUX_ABI_STRINGIFY(value) DEEPLUX_ABI_STRINGIFY_IMPL(value)
#define DEEPLUX_MODULE_INTERFACE_IID "com.deeplux.IModule/" DEEPLUX_ABI_STRINGIFY(DEEPLUX_MODULE_INTERFACE_VERSION) ".0"
#define DEEPLUX_CAMERA_PLUGIN_INTERFACE_IID                                                                            \
    "com.deeplux.ICameraPlugin/" DEEPLUX_ABI_STRINGIFY(DEEPLUX_CAMERA_PLUGIN_INTERFACE_VERSION) ".0"
#define DEEPLUX_CAMERA_INTERFACE_IID "com.deeplux.ICamera/" DEEPLUX_ABI_STRINGIFY(DEEPLUX_CAMERA_INTERFACE_VERSION) ".0"

namespace DeepLux {

static constexpr quint32 pluginAbiCompilerId() {
#if defined(__clang__)
    return 2;
#elif defined(_MSC_VER)
    return 3;
#elif defined(__GNUC__)
    return 1;
#else
    return 255;
#endif
}

static constexpr quint32 pluginAbiVersion() {
    static_assert(QT_VERSION_MAJOR < 16 && QT_VERSION_MINOR < 16, "Qt version does not fit the ABI field");
    static_assert(DEEPLUX_PLUGIN_ABI_VERSION >= 0 && DEEPLUX_PLUGIN_ABI_VERSION <= 0xffff,
                  "Plugin ABI version does not fit the ABI field");
    return (pluginAbiCompilerId() << 24) | (static_cast<quint32>(QT_VERSION_MAJOR) << 20) |
           (static_cast<quint32>(QT_VERSION_MINOR) << 16) | DEEPLUX_PLUGIN_ABI_VERSION;
}

static inline const char* pluginAbiFingerprint() {
    static const QByteArray fingerprint = [] {
#if defined(__clang__)
        const char* compiler = "clang-" __clang_version__;
#elif defined(_MSC_VER)
#define DEEPLUX_MSVC_STRINGIFY_IMPL(value) #value
#define DEEPLUX_MSVC_STRINGIFY(value) DEEPLUX_MSVC_STRINGIFY_IMPL(value)
        const char* compiler = "msvc-" DEEPLUX_MSVC_STRINGIFY(_MSC_VER);
#elif defined(__GNUC__)
        const char* compiler = "gcc-" __VERSION__;
#else
        const char* compiler = "unknown";
#endif
#if defined(_GLIBCXX_USE_CXX11_ABI)
        const char* stdlibAbi = "libstdc++-cxx11-" DEEPLUX_ABI_STRINGIFY(_GLIBCXX_USE_CXX11_ABI);
#elif defined(_LIBCPP_VERSION)
        const char* stdlibAbi = "libc++-" DEEPLUX_ABI_STRINGIFY(_LIBCPP_VERSION);
#else
        const char* stdlibAbi = "stdlib-default";
#endif
#if defined(_ITERATOR_DEBUG_LEVEL)
        constexpr int iteratorDebugLevel = _ITERATOR_DEBUG_LEVEL;
#else
        constexpr int iteratorDebugLevel = 0;
#endif
#if defined(QT_DEBUG)
        constexpr int qtDebug = 1;
#else
        constexpr int qtDebug = 0;
#endif
        return QByteArray(compiler) + "|arch=" + QByteArray::number(static_cast<qulonglong>(sizeof(void*) * 8)) + "|" +
               stdlibAbi + "|iteratorDebug=" + QByteArray::number(iteratorDebugLevel) + "|Qt" QT_VERSION_STR +
               "|qtDebug=" + QByteArray::number(qtDebug) +
               "|sizeof(DisplayFrame)=" + QByteArray::number(static_cast<qulonglong>(sizeof(DisplayFrame))) +
               "|sizeof(ExecutionContext)=" + QByteArray::number(static_cast<qulonglong>(sizeof(ExecutionContext))) +
               "|sizeof(ExecutionResult)=" + QByteArray::number(static_cast<qulonglong>(sizeof(ExecutionResult))) +
               "|moduleIID=" DEEPLUX_MODULE_INTERFACE_IID "|cameraPluginIID=" DEEPLUX_CAMERA_PLUGIN_INTERFACE_IID
               "|cameraIID=" DEEPLUX_CAMERA_INTERFACE_IID;
    }();
    return fingerprint.constData();
}

using PluginAbiVersionFunction = quint32 (*)() noexcept;
using PluginAbiFingerprintFunction = const char* (*) () noexcept;

} // namespace DeepLux
