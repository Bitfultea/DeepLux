# StopWhile Plugin

set(PLUGIN_SOURCES
    StopWhilePlugin.cpp
)

set(PLUGIN_HEADERS
    StopWhilePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(StopWhilePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(StopWhilePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(StopWhilePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
