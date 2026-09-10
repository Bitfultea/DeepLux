# SaveData Plugin

set(PLUGIN_SOURCES
    SaveDataPlugin.cpp
)

set(PLUGIN_HEADERS
    SaveDataPlugin.h
)

add_library(SaveDataPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(SaveDataPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(SaveDataPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(SaveDataPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()