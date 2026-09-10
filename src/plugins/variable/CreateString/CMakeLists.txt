# CreateString Plugin

set(PLUGIN_SOURCES
    CreateStringPlugin.cpp
)

set(PLUGIN_HEADERS
    CreateStringPlugin.h
)

add_library(CreateStringPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(CreateStringPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
