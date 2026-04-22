# MeasureLine Plugin

set(PLUGIN_SOURCES
    MeasureLinePlugin.cpp
)

set(PLUGIN_HEADERS
    MeasureLinePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(MeasureLinePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(MeasureLinePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(MeasureLinePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(MeasureLinePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(MeasureLinePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()