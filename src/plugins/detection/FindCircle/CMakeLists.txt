# FindCircle Plugin

set(PLUGIN_SOURCES
    FindCirclePlugin.cpp
)

set(PLUGIN_HEADERS
    FindCirclePlugin.h
)

add_library(FindCirclePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(FindCirclePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(FindCirclePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(FindCirclePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
