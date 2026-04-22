cmake_minimum_required(VERSION 3.14)
project(HikvisionCameraPlugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt
find_package(Qt5 COMPONENTS Core Widgets REQUIRED)

# Hikvision MVS SDK path
set(MVS_SDK_ROOT "/opt/MVS" CACHE PATH "Hikvision MVS SDK root directory")
set(MVS_SDK_LIB "${MVS_SDK_ROOT}/lib/64")
set(MVS_SDK_INCLUDE "${MVS_SDK_ROOT}/include")

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
    ${Qt5Core_PRIVATE_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(HikvisionCameraPlugin PRIVATE
    Qt5::Core
    Qt5::Widgets
    dl
    deeplux_core
    ${MVS_SDK_LIB}/libMvCameraControl.so.4.7.0.3
    pthread
)

# Install rule
install(TARGETS HikvisionCameraPlugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/camera
)
