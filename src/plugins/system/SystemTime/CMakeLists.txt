# SystemTime Plugin

set(PLUGIN_SOURCES
    SystemTimePlugin.cpp
)

set(PLUGIN_HEADERS
    SystemTimePlugin.h
)

add_library(SystemTimePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(SystemTimePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(SystemTimePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(SystemTimePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()