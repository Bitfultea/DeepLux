# Parallel Plugin

set(PLUGIN_SOURCES
    ParallelPlugin.cpp
)

set(PLUGIN_HEADERS
    ParallelPlugin.h
)

add_library(ParallelPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(ParallelPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
