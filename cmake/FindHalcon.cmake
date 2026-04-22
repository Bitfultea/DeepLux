# FindHalcon.cmake - 查找 Halcon 库
#
# 用法:
#   find_package(Halcon REQUIRED)
#
# 定义变量:
#   HALCON_FOUND        - 是否找到 Halcon
#   HALCON_INCLUDE_DIR  - 头文件目录
#   HALCON_LIBRARY_DIR  - 库文件目录
#   HALCON_LIBRARIES    - 库文件列表
#   HALCON_VERSION      - Halcon 版本

# 设置默认路径
if(NOT HALCON_ROOT)
    if(WIN32)
        set(HALCON_ROOT "C:/Program Files/MVTec/HALCON-21.11-Progress")
    else()
        set(HALCON_ROOT "/opt/halcon")
    endif()
endif()

message(STATUS "Looking for Halcon in: ${HALCON_ROOT}")

# 查找头文件
if(WIN32)
    find_path(HALCON_INCLUDE_DIR
        NAMES Halcon.h
        PATHS "${HALCON_ROOT}/include"
    )
else()
    find_path(HALCON_INCLUDE_DIR
        NAMES Halcon.h
        PATHS "${HALCON_ROOT}/include"
    )
endif()

# 查找库文件
if(WIN32)
    set(HALCON_LIBRARY_DIR "${HALCON_ROOT}/lib/x64-win64")
    find_library(HALCON_LIBRARY
        NAMES halcon
        PATHS "${HALCON_LIBRARY_DIR}"
    )
    find_library(HALCON_CPP_LIBRARY
        NAMES halconcpp
        PATHS "${HALCON_LIBRARY_DIR}"
    )
    find_library(HALCON_DEV_LIBRARY
        NAMES hdevengine
        PATHS "${HALCON_LIBRARY_DIR}"
    )
else()
    set(HALCON_LIBRARY_DIR "${HALCON_ROOT}/lib/x64-linux")
    find_library(HALCON_LIBRARY
        NAMES halcon
        PATHS "${HALCON_LIBRARY_DIR}"
    )
    find_library(HALCON_CPP_LIBRARY
        NAMES halconcpp
        PATHS "${HALCON_LIBRARY_DIR}"
    )
    find_library(HALCON_DEV_LIBRARY
        NAMES hdevengine
        PATHS "${HALCON_LIBRARY_DIR}"
    )
endif()

# 设置库列表
set(HALCON_LIBRARIES
    ${HALCON_CPP_LIBRARY}
    ${HALCON_LIBRARY}
)

if(HALCON_DEV_LIBRARY)
    list(APPEND HALCON_LIBRARIES ${HALCON_DEV_LIBRARY})
endif()

# 查找版本
if(EXISTS "${HALCON_ROOT}/version.txt")
    file(READ "${HALCON_ROOT}/version.txt" HALCON_VERSION)
    string(STRIP "${HALCON_VERSION}" HALCON_VERSION)
endif()

# 标准查找包处理
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Halcon
    REQUIRED_VARS
        HALCON_INCLUDE_DIR
        HALCON_LIBRARY
    VERSION_VAR HALCON_VERSION
)

# 导出变量
if(HALCON_FOUND)
    message(STATUS "Halcon found:")
    message(STATUS "  Version: ${HALCON_VERSION}")
    message(STATUS "  Include: ${HALCON_INCLUDE_DIR}")
    message(STATUS "  Library: ${HALCON_LIBRARY}")
    message(STATUS "  C++ Lib: ${HALCON_CPP_LIBRARY}")
    
    mark_as_advanced(
        HALCON_INCLUDE_DIR
        HALCON_LIBRARY_DIR
        HALCON_LIBRARY
        HALCON_CPP_LIBRARY
        HALCON_DEV_LIBRARY
    )
endif()
