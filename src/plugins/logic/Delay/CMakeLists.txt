# Delay Plugin

set(PLUGIN_SOURCES
    DelayPlugin.cpp
)

set(PLUGIN_HEADERS
    DelayPlugin.h
)

add_library(DelayPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(DelayPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
