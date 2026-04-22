# MeasureGap Plugin

set(PLUGIN_SOURCES
    MeasureGapPlugin.cpp
)

set(PLUGIN_HEADERS
    MeasureGapPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(MeasureGapPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(MeasureGapPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(MeasureGapPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(MeasureGapPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(MeasureGapPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()