# FreeformSurface Plugin

set(PLUGIN_SOURCES
    FreeformSurfacePlugin.cpp
)

set(PLUGIN_HEADERS
    FreeformSurfacePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(FreeformSurfacePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(FreeformSurfacePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(FreeformSurfacePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(FreeformSurfacePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(FreeformSurfacePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()