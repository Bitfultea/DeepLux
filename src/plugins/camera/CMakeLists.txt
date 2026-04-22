cmake_minimum_required(VERSION 3.14)

# Camera plugins
add_subdirectory(V4L2Camera)

# Hikvision camera plugin (Linux)
if(UNIX AND NOT APPLE)
    add_subdirectory(HikvisionCamera)
endif()

# Windows camera plugins (DirectShow)
if(WIN32)
    add_subdirectory(DirectShowCamera)
endif()
