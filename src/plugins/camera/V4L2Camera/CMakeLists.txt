cmake_minimum_required(VERSION 3.14)
project(V4L2CameraPlugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt
find_package(Qt5 COMPONENTS Core Widgets REQUIRED)

# Define sources
set(SOURCES
    V4L2Camera.cpp
    V4L2CameraPlugin.cpp
)

set(HEADERS
    V4L2Camera.h
    V4L2CameraPlugin.h
)

# Create plugin library
add_library(V4L2CameraPlugin SHARED
    ${SOURCES}
    ${HEADERS}
    metadata.json
)

# Set plugin binary output directory
set_target_properties(V4L2CameraPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/camera
)

# Include directories
target_include_directories(V4L2CameraPlugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${Qt5Core_PRIVATE_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(V4L2CameraPlugin PRIVATE
    Qt5::Core
    Qt5::Widgets
    dl
    deeplux_core
)

# Install rule
install(TARGETS V4L2CameraPlugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/camera
)
