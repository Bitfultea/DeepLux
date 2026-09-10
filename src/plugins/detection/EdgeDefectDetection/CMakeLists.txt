# EdgeDefectDetection Plugin

set(PLUGIN_SOURCES
    EdgeDefectDetectionPlugin.cpp
)

set(PLUGIN_HEADERS
    EdgeDefectDetectionPlugin.h
)

add_library(EdgeDefectDetectionPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(EdgeDefectDetectionPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(EdgeDefectDetectionPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(EdgeDefectDetectionPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
