# GapMeasure3D Plugin（阶7 批3：0143D 重建）

set(PLUGIN_SOURCES
    GapMeasure3DPlugin.cpp
)

set(PLUGIN_HEADERS
    GapMeasure3DPlugin.h
)

add_library(GapMeasure3DPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(GapMeasure3DPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(GapMeasure3DPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(GapMeasure3DPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
