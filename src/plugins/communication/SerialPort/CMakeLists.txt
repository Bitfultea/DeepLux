# SerialPort Plugin

set(PLUGIN_SOURCES
    SerialPortPlugin.cpp
)

set(PLUGIN_HEADERS
    SerialPortPlugin.h
)

if(Qt6_FOUND)
    qt6_add_plugin(SerialPortPlugin SHARED SOURCES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
else()
    add_library(SerialPortPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})
endif()

target_link_libraries(SerialPortPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::SerialPort)
