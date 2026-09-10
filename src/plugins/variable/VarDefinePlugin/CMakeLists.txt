# VarDefinePlugin

set(PLUGIN_SOURCES
    VarDefinePlugin.cpp
)

set(PLUGIN_HEADERS
    VarDefinePlugin.h
)

add_library(VarDefinePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(VarDefinePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
