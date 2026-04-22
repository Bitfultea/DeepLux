# TCPClient Plugin

set(PLUGIN_SOURCES
    TCPClientPlugin.cpp
)

set(PLUGIN_HEADERS
    TCPClientPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(TCPClientPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(TCPClientPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(TCPClientPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
