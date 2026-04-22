# While Plugin

set(PLUGIN_SOURCES
    WhilePlugin.cpp
)

set(PLUGIN_HEADERS
    WhilePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(WhilePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(WhilePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(WhilePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
