# Detection plugins

if(NOT OpenCV_FOUND)
    message(STATUS "Skipping detection plugins (OpenCV not found)")
    return()
endif()

# QRCode Plugin
add_subdirectory(QRCode)

# FindCircle Plugin
add_subdirectory(FindCircle)

# MeasureLine Plugin
add_subdirectory(MeasureLine)

# MeasureRect Plugin
add_subdirectory(MeasureRect)

# Matching Plugin
add_subdirectory(Matching)

# ColorRecognition Plugin
add_subdirectory(ColorRecognition)

# JiErHanDefectsDet Plugin
add_subdirectory(JiErHanDefectsDet)
