# VarSetPlugin

set(PLUGIN_SOURCES
    VarSetPlugin.cpp
)

set(PLUGIN_HEADERS
    VarSetPlugin.h
)

add_library(VarSetPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(VarSetPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
