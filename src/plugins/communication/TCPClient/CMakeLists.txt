# TCPClient Plugin

set(PLUGIN_SOURCES
    TCPClientPlugin.cpp
)

set(PLUGIN_HEADERS
    TCPClientPlugin.h
)

add_library(TCPClientPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(TCPClientPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
