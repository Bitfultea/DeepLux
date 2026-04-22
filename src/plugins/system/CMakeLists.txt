# System plugins

if(NOT OpenCV_FOUND)
    message(STATUS "Skipping system plugins (OpenCV not found)")
    return()
endif()

# DataCheck Plugin
add_subdirectory(DataCheck)

# Folder Plugin
add_subdirectory(Folder)

# SaveData Plugin
add_subdirectory(SaveData)

# ShowPoint Plugin
add_subdirectory(ShowPoint)

# SystemTime Plugin
add_subdirectory(SystemTime)

# TableOutPut Plugin
add_subdirectory(TableOutPut)

# TimeSlice Plugin
add_subdirectory(TimeSlice)

# WriteText Plugin
add_subdirectory(WriteText)