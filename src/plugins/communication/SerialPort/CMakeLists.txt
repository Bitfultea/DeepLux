# SerialPort Plugin

if(NOT Qt${QT_VERSION_MAJOR}SerialPort_FOUND)
    message(STATUS "SerialPortPlugin: Skipped (Qt SerialPort not found)")
    return()
endif()

set(PLUGIN_SOURCES
    SerialPortPlugin.cpp
)

set(PLUGIN_HEADERS
    SerialPortPlugin.h
)

add_library(SerialPortPlugin SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

target_link_libraries(SerialPortPlugin PRIVATE deeplux_core Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Gui Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::SerialPort)
