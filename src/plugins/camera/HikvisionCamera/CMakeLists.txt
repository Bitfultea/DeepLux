cmake_minimum_required(VERSION 3.14)
project(HikvisionCameraPlugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Hikvision MVS SDK path
set(MVS_SDK_ROOT "/opt/MVS" CACHE PATH "Hikvision MVS SDK root directory")
set(MVS_SDK_LIB "${MVS_SDK_ROOT}/lib/64")
set(MVS_SDK_INCLUDE "${MVS_SDK_ROOT}/include")
find_library(MVS_CAMERA_CONTROL_LIBRARY NAMES MvCameraControl PATHS "${MVS_SDK_LIB}" NO_DEFAULT_PATH)

if(NOT EXISTS "${MVS_SDK_INCLUDE}/MvCameraControl.h" OR NOT MVS_CAMERA_CONTROL_LIBRARY)
    message(FATAL_ERROR
        "ENABLE_CAMERA_HIKVISION=ON requires the Hikvision MVS SDK. "
        "Set MVS_SDK_ROOT to the SDK directory or disable the plugin.")
endif()

# Define sources
set(SOURCES
    HikvisionCamera.cpp
    HikvisionCameraPlugin.cpp
)

set(HEADERS
    HikvisionCamera.h
    HikvisionCameraPlugin.h
)

# Create plugin library
add_library(HikvisionCameraPlugin SHARED
    ${SOURCES}
    ${HEADERS}
    metadata.json
)

# Set plugin binary output directory
set_target_properties(HikvisionCameraPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/camera
)

# Set RPATH so the plugin can find MVS SDK at runtime
set_target_properties(HikvisionCameraPlugin PROPERTIES
    BUILD_RPATH "${MVS_SDK_LIB}"
    INSTALL_RPATH "${MVS_SDK_LIB}"
)

# Include directories
target_include_directories(HikvisionCameraPlugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${MVS_SDK_INCLUDE}
)

# Link libraries
target_link_libraries(HikvisionCameraPlugin PRIVATE
    Qt${QT_VERSION_MAJOR}::Core
    Qt${QT_VERSION_MAJOR}::Widgets
    dl
    deeplux_core
    ${MVS_CAMERA_CONTROL_LIBRARY}
    pthread
)

# Install rule
install(TARGETS HikvisionCameraPlugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/camera
)
