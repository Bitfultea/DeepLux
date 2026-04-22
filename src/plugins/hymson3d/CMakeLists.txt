# HymsonVision3D algorithm plugins

# Option to enable HymsonVision3D support
option(ENABLE_HYMSON3D "Enable HymsonVision3D algorithm plugins" OFF)

if(NOT ENABLE_HYMSON3D)
    message(STATUS "HymsonVision3D plugins: Disabled (set ENABLE_HYMSON3D=ON to enable)")
    return()
endif()

# Find HymsonVision3D package
find_package(HymsonVision3D QUIET)
if(NOT HymsonVision3D_FOUND)
    # Try to find HymsonVision3D in common locations
    find_path(HYMSON3D_INCLUDE_DIRS
        NAMES geometry/3D/PointCloud.h
        PATHS /home/charles/Data/Repo/HymsonVision3D
              /usr/local/include/hymson3d
    )
    find_library(HYMSON3D_GEOMETRY_LIB NAMES geometry_3d_lib PATHS /home/charles/Data/Repo/HymsonVision3D/build/geometry/3D)
    find_library(HYMSON3D_PIPELINE_LIB NAMES pipeline PATHS /home/charles/Data/Repo/HymsonVision3D/build/pipeline)
    if(HYMSON3D_INCLUDE_DIRS AND HYMSON3D_GEOMETRY_LIB AND HYMSON3D_PIPELINE_LIB)
        set(HymsonVision3D_FOUND TRUE)
        set(HYMSON3D_LIBRARIES ${HYMSON3D_GEOMETRY_LIB} ${HYMSON3D_PIPELINE_LIB})
        message(STATUS "HymsonVision3D: Found at ${HYMSON3D_INCLUDE_DIRS}")
    endif()
endif()

# Always add the DefectDetection plugin (builds in demo mode without HymsonVision3D headers)
message(STATUS "HymsonVision3D: Plugin framework ready (demo mode)")
add_subdirectory(DefectDetection)

# Note: DEEPLUX_HAS_HYMSON3D is NOT defined globally because:
# 1. HymsonVision3D headers have complex internal dependencies
# 2. The plugin framework should work without HymsonVision3D being available
# Individual plugins can define it locally when they have proper SDK setup
