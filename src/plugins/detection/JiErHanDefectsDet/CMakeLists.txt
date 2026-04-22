# JiErHanDefectsDet Plugin

set(PLUGIN_SOURCES
    JiErHanDefectsDetPlugin.cpp
)

set(PLUGIN_HEADERS
    JiErHanDefectsDetPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(JiErHanDefectsDetPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(JiErHanDefectsDetPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(JiErHanDefectsDetPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(JiErHanDefectsDetPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(JiErHanDefectsDetPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()