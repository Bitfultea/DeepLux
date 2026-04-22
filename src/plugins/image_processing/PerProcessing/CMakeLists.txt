# PerProcessing Plugin

set(PLUGIN_SOURCES
    PerProcessingPlugin.cpp
)

set(PLUGIN_HEADERS
    PerProcessingPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(PerProcessingPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(PerProcessingPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(PerProcessingPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(PerProcessingPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(PerProcessingPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()