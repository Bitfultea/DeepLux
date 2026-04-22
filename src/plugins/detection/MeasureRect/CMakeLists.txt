# MeasureRect Plugin

set(PLUGIN_SOURCES
    MeasureRectPlugin.cpp
)

set(PLUGIN_HEADERS
    MeasureRectPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(MeasureRectPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(MeasureRectPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(MeasureRectPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(MeasureRectPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(MeasureRectPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()