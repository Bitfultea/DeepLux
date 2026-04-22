# DistancePP Plugin

set(PLUGIN_SOURCES
    DistancePPPlugin.cpp
)

set(PLUGIN_HEADERS
    DistancePPPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(DistancePPPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(DistancePPPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(DistancePPPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(DistancePPPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(DistancePPPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()