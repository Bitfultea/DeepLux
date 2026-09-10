# QueueOut Plugin

set(PLUGIN_SOURCES
    QueueOutPlugin.cpp
)

set(PLUGIN_HEADERS
    QueueOutPlugin.h
)

add_library(QueueOutPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(QueueOutPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
