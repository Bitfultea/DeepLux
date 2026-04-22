# StrFormatPlugin

set(PLUGIN_SOURCES
    StrFormatPlugin.cpp
)

set(PLUGIN_HEADERS
    StrFormatPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(StrFormatPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(StrFormatPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(StrFormatPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
