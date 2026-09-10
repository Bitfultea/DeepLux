# PLCRead Plugin

set(PLUGIN_SOURCES
    PLCReadPlugin.cpp
)

set(PLUGIN_HEADERS
    PLCReadPlugin.h
)

add_library(PLCReadPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(PLCReadPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
