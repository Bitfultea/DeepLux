# SaveImage Plugin

set(PLUGIN_SOURCES SaveImagePlugin.cpp)
set(PLUGIN_HEADERS SaveImagePlugin.h)

if(Qt6_FOUND)
    qt6_add_plugin(SaveImagePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(SaveImagePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(SaveImagePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(SaveImagePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(SaveImagePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
