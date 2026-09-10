# StrFormatPlugin

set(PLUGIN_SOURCES
    StrFormatPlugin.cpp
)

set(PLUGIN_HEADERS
    StrFormatPlugin.h
)

add_library(StrFormatPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(StrFormatPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
