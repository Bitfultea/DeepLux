# While Plugin

set(PLUGIN_SOURCES
    WhilePlugin.cpp
)

set(PLUGIN_HEADERS
    WhilePlugin.h
)

add_library(WhilePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(WhilePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
