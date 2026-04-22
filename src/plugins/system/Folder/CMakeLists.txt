# Folder Plugin

set(PLUGIN_SOURCES
    FolderPlugin.cpp
)

set(PLUGIN_HEADERS
    FolderPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(FolderPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(FolderPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(FolderPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(FolderPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(FolderPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()