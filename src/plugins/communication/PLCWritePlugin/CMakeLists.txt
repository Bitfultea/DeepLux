# PLCWrite Plugin

set(PLUGIN_SOURCES
    PLCWritePlugin.cpp
)

set(PLUGIN_HEADERS
    PLCWritePlugin.h
)

add_library(PLCWritePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(PLCWritePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
