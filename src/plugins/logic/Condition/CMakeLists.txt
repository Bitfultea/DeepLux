# Condition Plugin

set(PLUGIN_SOURCES
    ConditionPlugin.cpp
)

set(PLUGIN_HEADERS
    ConditionPlugin.h
)

add_library(ConditionPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(ConditionPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)
