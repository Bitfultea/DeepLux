# Loop Plugin

set(PLUGIN_SOURCES
    LoopPlugin.cpp
)

set(PLUGIN_HEADERS
    LoopPlugin.h
)

add_library(LoopPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(LoopPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
