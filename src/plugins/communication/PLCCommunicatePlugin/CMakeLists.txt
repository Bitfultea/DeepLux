# PLCCommunicate Plugin

set(PLUGIN_SOURCES
    PLCCommunicatePlugin.cpp
)

set(PLUGIN_HEADERS
    PLCCommunicatePlugin.h
)

add_library(PLCCommunicatePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(PLCCommunicatePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
