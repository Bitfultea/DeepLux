# MeasureRect Plugin

set(PLUGIN_SOURCES
    MeasureRectPlugin.cpp
)

set(PLUGIN_HEADERS
    MeasureRectPlugin.h
)

add_library(MeasureRectPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(MeasureRectPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(MeasureRectPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(MeasureRectPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()