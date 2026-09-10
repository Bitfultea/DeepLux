# CropImage Plugin（阶7 批4：001图像处理 重建）

set(PLUGIN_SOURCES
    CropImagePlugin.cpp
)

set(PLUGIN_HEADERS
    CropImagePlugin.h
)

add_library(CropImagePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(CropImagePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(CropImagePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(CropImagePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
