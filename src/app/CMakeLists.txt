# DeepLux Application

set(APP_SOURCES
    main.cpp
    CLIHandler.h
    CLIHandler.cpp
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
    target_include_directories(DeepLux PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(DeepLux PRIVATE ${OpenCV_LIBS})
endif()

if(WIN32)
    set_target_properties(DeepLux PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "DeepLux"
    )
else()
    set_target_properties(DeepLux PROPERTIES
        OUTPUT_NAME "DeepLux"
    )
endif()

install(TARGETS DeepLux
    RUNTIME DESTINATION bin
)
