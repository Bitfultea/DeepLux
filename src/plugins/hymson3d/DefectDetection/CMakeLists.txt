# DefectDetection Plugin - HymsonVision3D integration

set(PLUGIN_SOURCES
    DefectDetectionPlugin.cpp
)

set(PLUGIN_HEADERS
    DefectDetectionPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(DefectDetectionPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(DefectDetectionPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(DefectDetectionPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

# HymsonVision3D dependency (optional)
# Note: HymsonVision3D headers have complex internal dependencies that require
#       proper SDK installation. For now, build in demo mode without direct includes.
if(HymsonVision3D_FOUND AND FALSE)  # Disabled until proper SDK setup
    target_compile_definitions(DefectDetectionPlugin PRIVATE DEEPLUX_HAS_HYMSON3D)
    target_include_directories(DefectDetectionPlugin PRIVATE ${HYMSON3D_INCLUDE_DIRS})
    target_link_libraries(DefectDetectionPlugin PRIVATE ${HYMSON3D_LIBRARIES})
    message(STATUS "HymsonVision3D: Enabled for DefectDetectionPlugin")
else()
    message(STATUS "HymsonVision3D: Not found, DefectDetectionPlugin running in demo mode")
endif()
