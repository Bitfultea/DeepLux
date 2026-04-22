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
