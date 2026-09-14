# If Plugin

set(PLUGIN_SOURCES
    IfPlugin.cpp
)

set(PLUGIN_HEADERS
    IfPlugin.h
)

add_library(IfPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(IfPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
