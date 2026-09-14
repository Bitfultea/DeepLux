# QueueIn Plugin

set(PLUGIN_SOURCES
    QueueInPlugin.cpp
)

set(PLUGIN_HEADERS
    QueueInPlugin.h
)

add_library(QueueInPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(QueueInPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
