# DeepLux Image Processing Plugins

if(NOT OpenCV_FOUND)
    message(STATUS "Skipping image processing plugins (OpenCV not found)")
    return()
endif()

# GrabImage Plugin
add_subdirectory(GrabImage)

# SaveImage Plugin
add_subdirectory(SaveImage)

# ShowImage Plugin
add_subdirectory(ShowImage)

# Blob Plugin
add_subdirectory(Blob)

# DisplayData Plugin
add_subdirectory(DisplayData)

# PerProcessing Plugin
add_subdirectory(PerProcessing)

# JigsawPuzzle Plugin
add_subdirectory(JigsawPuzzle)

# ImageScript Plugin
add_subdirectory(ImageScript)
