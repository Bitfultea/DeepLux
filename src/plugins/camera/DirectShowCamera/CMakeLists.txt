cmake_minimum_required(VERSION 3.14)
project(DirectShowCameraPlugin VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Find Qt
find_package(Qt5 COMPONENTS Core Widgets REQUIRED)

# Define sources
set(SOURCES
    DirectShowCamera.cpp
    DirectShowCameraPlugin.cpp
)

set(HEADERS
    DirectShowCamera.h
    DirectShowCameraPlugin.h
)

# Create plugin library
add_library(DirectShowCameraPlugin SHARED
    ${SOURCES}
    ${HEADERS}
    metadata.json
)

# Set plugin binary output directory
set_target_properties(DirectShowCameraPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins/camera
)

# Include directories
target_include_directories(DirectShowCameraPlugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${Qt5Core_PRIVATE_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(DirectShowCameraPlugin PRIVATE
    Qt5::Core
    Qt5::Widgets
    strmiids
)

# Windows-specific definitions
target_compile_definitions(DirectShowCameraPlugin PRIVATE _WIN32)

# Install rule
install(TARGETS DirectShowCameraPlugin
    LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/plugins/camera
)
