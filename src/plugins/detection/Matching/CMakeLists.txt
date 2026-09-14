# Matching Plugin

set(PLUGIN_SOURCES
    MatchingPlugin.cpp
)

set(PLUGIN_HEADERS
    MatchingPlugin.h
)

add_library(MatchingPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(MatchingPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(MatchingPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(MatchingPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()