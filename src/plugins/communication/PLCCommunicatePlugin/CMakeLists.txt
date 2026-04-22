# PLCCommunicate Plugin

set(PLUGIN_SOURCES
    PLCCommunicatePlugin.cpp
)

set(PLUGIN_HEADERS
    PLCCommunicatePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(PLCCommunicatePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(PLCCommunicatePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(PLCCommunicatePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
