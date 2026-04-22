# PointSurfaceDistance Plugin

set(PLUGIN_SOURCES
    PointSurfaceDistancePlugin.cpp
)

set(PLUGIN_HEADERS
    PointSurfaceDistancePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(PointSurfaceDistancePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(PointSurfaceDistancePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(PointSurfaceDistancePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(PointSurfaceDistancePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(PointSurfaceDistancePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()