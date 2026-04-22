# ImageScript Plugin

set(PLUGIN_SOURCES
    ImageScriptPlugin.cpp
)

set(PLUGIN_HEADERS
    ImageScriptPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(ImageScriptPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(ImageScriptPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(ImageScriptPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(ImageScriptPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(ImageScriptPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()