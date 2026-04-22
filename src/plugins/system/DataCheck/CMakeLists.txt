# DataCheck Plugin

set(PLUGIN_SOURCES
    DataCheckPlugin.cpp
)

set(PLUGIN_HEADERS
    DataCheckPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(DataCheckPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(DataCheckPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(DataCheckPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(DataCheckPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(DataCheckPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()