cmake_minimum_required(VERSION 3.16)
project(DeepLux VERSION 1.0.0 LANGUAGES CXX)

# ========== C++ 标准 ==========
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ========== Qt 自动化 ==========
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ========== 平台判断 ==========
if(WIN32)
    set(DEEPLUX_PLATFORM "windows")
    add_definitions(-DDEEPLUX_PLATFORM_WINDOWS)
    message(STATUS "Platform: Windows")
elseif(UNIX AND NOT APPLE)
    set(DEEPLUX_PLATFORM "linux")
    add_definitions(-DDEEPLUX_PLATFORM_LINUX)
    message(STATUS "Platform: Linux")
elseif(APPLE)
    set(DEEPLUX_PLATFORM "macos")
    add_definitions(-DDEEPLUX_PLATFORM_MACOS)
    message(STATUS "Platform: macOS")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# ========== 编译选项 ==========
if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ========== 输出目录 ==========
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ========== 查找 Qt ==========
option(USE_QT6 "Use Qt6 instead of Qt5" ON)

# 允许显式指定 Qt 安装路径（用于自定义 Qt 或 CI/CD）
set(DEEPLUX_QT_PATH "" CACHE PATH "Path to Qt installation (e.g. /opt/Qt/6.5.0/gcc_64). Leave empty to use system Qt.")
if(DEEPLUX_QT_PATH)
    list(APPEND CMAKE_PREFIX_PATH ${DEEPLUX_QT_PATH})
    message(STATUS "Using custom Qt path: ${DEEPLUX_QT_PATH}")
endif()

set(QT_CORE_COMPONENTS Core Gui Widgets Network Sql Test)

if(USE_QT6)
    find_package(Qt6 QUIET COMPONENTS ${QT_CORE_COMPONENTS} Concurrent)
endif()

if(NOT Qt6_FOUND)
    message(STATUS "Qt6 not found, trying Qt5...")
    find_package(Qt5 REQUIRED COMPONENTS ${QT_CORE_COMPONENTS})
    # Qt5 Concurrent is part of Core, no separate find_package needed
    set(Qt5Concurrent_FOUND TRUE)
    set(QT_VERSION_MAJOR 5)
else()
    set(QT_VERSION_MAJOR 6)
endif()

# ========== Qt SSL 兼容性检查 ==========
# Qt 5.15.2 及以下不支持 OpenSSL 3.x。
# 如果系统 OpenSSL >= 3.0 且 Qt < 5.15.3，在配置阶段就给出明确错误。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(OPENSSL QUIET openssl>=3.0)
endif()

if(QT_VERSION_MAJOR EQUAL 5 AND OPENSSL_FOUND)
    if(Qt5_VERSION VERSION_LESS "5.15.3")
        message(FATAL_ERROR
            "Qt ${Qt5_VERSION} does not support OpenSSL 3.x (found ${OPENSSL_VERSION}).\n"
            "Please use one of the following options:\n"
            "  1. Use system Qt 5.15.3+ (unset DEEPLUX_QT_PATH)\n"
            "  2. Install a custom Qt 5.15.11+ or Qt 6.2+ and set DEEPLUX_QT_PATH\n"
            "  3. Downgrade system OpenSSL to 1.1.1 (not recommended)")
    endif()
endif()

# 运行期 SSL 可用性检查（编译小测试程序验证）
if(QT_VERSION_MAJOR EQUAL 5)
    set(_qt_network_lib Qt5::Network)
else()
    set(_qt_network_lib Qt6::Network)
endif()

set(CMAKE_REQUIRED_INCLUDES ${Qt${QT_VERSION_MAJOR}Network_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${_qt_network_lib})
include(CheckCXXSourceRuns)
check_cxx_source_runs("
#include <QSslSocket>
#include <QCoreApplication>
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    return QSslSocket::supportsSsl() ? 0 : 1;
}
" QT_SSL_WORKS)

if(NOT QT_SSL_WORKS)
    message(WARNING
        "Qt SSL backend is not available at runtime.\n"
        "HTTPS requests (Agent Chat, etc.) will fail with 'SSL handshake failed'.\n"
        "Common causes:\n"
        "  - Qt was built against a different OpenSSL version than the one installed\n"
        "  - LD_LIBRARY_PATH points to an incompatible Qt installation\n"
        "  - Missing libssl/libcrypto development packages\n")
endif()

# 可选组件: SerialPort
find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS SerialPort)
if(Qt${QT_VERSION_MAJOR}SerialPort_FOUND)
    message(STATUS "Qt SerialPort: Found")
else()
    message(STATUS "Qt SerialPort: Not found (optional)")
endif()

# ========== 查找 OpenCV ==========
find_package(OpenCV QUIET)
if(OpenCV_FOUND)
    message(STATUS "OpenCV: Found (version: ${OpenCV_VERSION})")
    message(STATUS "  Include: ${OpenCV_INCLUDE_DIRS}")
    message(STATUS "  Libraries: ${OpenCV_LIBS}")
else()
    message(STATUS "OpenCV: Not found (build without vision features)")
endif()

# ========== 输出配置信息 ==========
message(STATUS "Qt version: ${Qt${QT_VERSION_MAJOR}_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ========== 全局定义 ==========
if(OpenCV_FOUND)
    add_definitions(-DDEEPLUX_HAS_OPENCV)
endif()

# ========== 全局包含目录 ==========
include_directories(${CMAKE_SOURCE_DIR}/src)

# ========== 子目录 ==========
add_subdirectory(src/core)
add_subdirectory(src/ui)
add_subdirectory(src/app)

# ========== 插件 ==========
add_subdirectory(src/plugins/image_processing)
add_subdirectory(src/plugins/detection)
add_subdirectory(src/plugins/geometry)
add_subdirectory(src/plugins/system)
add_subdirectory(src/plugins/calibration)
add_subdirectory(src/plugins/variable)
add_subdirectory(src/plugins/communication)
add_subdirectory(src/plugins/logic)
add_subdirectory(src/plugins/hymson3d)
add_subdirectory(src/plugins/camera)

# ========== 条件编译插件 ==========
option(ENABLE_CAMERA_BASLER "Enable Basler camera support" OFF)
option(ENABLE_CAMERA_HIKVISION "Enable HikVision camera support" OFF)

if(ENABLE_CAMERA_BASLER)
    add_subdirectory(src/plugins/cameras/CameraBasler)
endif()

if(ENABLE_CAMERA_HIKVISION)
    add_subdirectory(src/plugins/cameras/CameraHikVision)
endif()

# ========== 测试 ==========
option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ========== 安装 ==========
install(DIRECTORY resources/ DESTINATION .)
