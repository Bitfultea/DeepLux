set(PLUGIN_SOURCES QRCodePlugin.cpp)
set(PLUGIN_HEADERS QRCodePlugin.h)
add_library(QRCodePlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
target_link_libraries(QRCodePlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets)

if(OpenCV_FOUND)
    target_link_libraries(QRCodePlugin PRIVATE ${OpenCV_LIBS})
    target_include_directories(QRCodePlugin PRIVATE ${OpenCV_INCLUDE_DIRS})
endif()
