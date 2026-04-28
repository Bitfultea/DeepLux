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

target_include_directories(deeplux_terminal
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(deeplux_terminal
    PUBLIC
        Qt5::Core
        Qt5::Widgets
)
