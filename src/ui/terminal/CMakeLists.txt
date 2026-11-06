# DeepLux Terminal Components

set(TERMINAL_SOURCES
    AnsiParser.cpp
    TerminalScreen.cpp
    TerminalRenderer.cpp
)

set(TERMINAL_HEADERS
    AnsiParser.h
    AnsiConstants.h
    TerminalScreen.h
    TerminalRenderer.h
)

add_library(deeplux_terminal STATIC ${TERMINAL_SOURCES} ${TERMINAL_HEADERS})
# Linux 上 deeplux_terminal 会链入共享库 deeplux_ui，静态库必须编译为 PIC。
set_target_properties(deeplux_terminal PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(deeplux_terminal
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(deeplux_terminal
    PUBLIC
        Qt${QT_VERSION_MAJOR}::Core
        Qt${QT_VERSION_MAJOR}::Widgets
)
