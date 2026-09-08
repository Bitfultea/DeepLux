# MeasureCircle Plugin

set(PLUGIN_SOURCES
    MeasureCirclePlugin.cpp
)

set(PLUGIN_HEADERS
    MeasureCirclePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(MeasureCirclePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(MeasureCirclePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(MeasureCirclePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(MeasureCirclePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(MeasureCirclePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
