# TCPServer Plugin

set(PLUGIN_SOURCES
    TCPServerPlugin.cpp
)

set(PLUGIN_HEADERS
    TCPServerPlugin.h
)

add_library(TCPServerPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(TCPServerPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
