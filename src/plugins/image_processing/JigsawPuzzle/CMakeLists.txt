# JigsawPuzzle Plugin

set(PLUGIN_SOURCES
    JigsawPuzzlePlugin.cpp
)

set(PLUGIN_HEADERS
    JigsawPuzzlePlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(JigsawPuzzlePlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(JigsawPuzzlePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(JigsawPuzzlePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(JigsawPuzzlePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(JigsawPuzzlePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()