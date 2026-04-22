# QueueIn Plugin

set(PLUGIN_SOURCES
    QueueInPlugin.cpp
)

set(PLUGIN_HEADERS
    QueueInPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(QueueInPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(QueueInPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(QueueInPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
