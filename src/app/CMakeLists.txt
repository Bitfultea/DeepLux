# DeepLux Application

set(APP_SOURCES
    main.cpp
    ../ui/resources/icons.qrc
)

add_executable(DeepLux ${APP_SOURCES})

target_link_libraries(DeepLux
    PRIVATE
        deeplux_core
        deeplux_ui
)

if(Qt6_FOUND)
    target_link_libraries(DeepLux PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
else()
    target_link_libraries(DeepLux PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
endif()

if(OpenCV_FOUND)
    if(TARGET OpenCV::OpenCV)
        target_link_libraries(DeepLux PRIVATE OpenCV::OpenCV)
    else()
        target_include_directories(DeepLux PRIVATE ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(DeepLux PRIVATE ${OpenCV_LIBS})
    endif()
endif()

if(WIN32)
    set_target_properties(DeepLux PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "DeepLux"
    )
else()
    set_target_properties(DeepLux PROPERTIES
        OUTPUT_NAME "DeepLux"
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}"
    )
endif()

install(TARGETS DeepLux
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install CLI wrapper (Linux only)
if(LINUX)
    install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/deeplux
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            RENAME deeplux)
    install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/deeplux.sh
            DESTINATION ${CMAKE_INSTALL_BINDIR}
            RENAME deeplux.sh)
endif()

# Windows CLI wrapper: DeepLux.exe is GUI-subsystem, cmd.exe is needed so that
# shells wait for the process and receive its stdout/stderr.
if(WIN32)
    install(PROGRAMS ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/deeplux.cmd
            DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ========== MCP stdio 桥（第二迭代下半，t20）==========
# 复用 deeplux_core 的 ToolSchema 作为工具清单单源；控制台子系统（stdout 为协议通道）。
add_library(deeplux_mcp STATIC
    mcp/AgentBridgeClient.cpp
    mcp/AgentBridgeClient.h
    mcp/McpStdioServer.cpp
    mcp/McpStdioServer.h
)

target_include_directories(deeplux_mcp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(deeplux_mcp PUBLIC deeplux_core)

if(Qt6_FOUND)
    target_link_libraries(deeplux_mcp PUBLIC Qt6::Core Qt6::Network)
else()
    target_link_libraries(deeplux_mcp PUBLIC Qt5::Core Qt5::Network)
endif()

add_executable(deeplux-mcp-bridge mcp/main.cpp)
target_link_libraries(deeplux-mcp-bridge PRIVATE deeplux_mcp)

install(TARGETS deeplux-mcp-bridge
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

