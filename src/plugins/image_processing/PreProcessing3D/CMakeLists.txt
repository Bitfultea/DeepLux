# 3DPreProcessing Plugin（阶7 批3：0143D 重建，目录名 PreProcessing3D 规避 C++ 标识符数字开头）

set(PLUGIN_SOURCES
    PreProcessing3DPlugin.cpp
)

set(PLUGIN_HEADERS
    PreProcessing3DPlugin.h
)

add_library(PreProcessing3DPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(PreProcessing3DPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(PreProcessing3DPlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(PreProcessing3DPlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
