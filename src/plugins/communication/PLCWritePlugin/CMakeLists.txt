# PLCWrite Plugin

set(PLUGIN_SOURCES
    PLCWritePlugin.cpp
)

set(PLUGIN_HEADERS
    PLCWritePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(PLCWritePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(PLCWritePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(PLCWritePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
