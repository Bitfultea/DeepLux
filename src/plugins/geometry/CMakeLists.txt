# Geometry plugins

if(NOT OpenCV_FOUND)
    message(STATUS "Skipping geometry plugins (OpenCV not found)")
    return()
endif()

# FitLine Plugin
add_subdirectory(FitLine)

# FitCircle Plugin
add_subdirectory(FitCircle)

# DistancePL Plugin
add_subdirectory(DistancePL)

# DistancePP Plugin
add_subdirectory(DistancePP)

# LinesDistance Plugin
add_subdirectory(LinesDistance)

# MeasureGap Plugin
add_subdirectory(MeasureGap)

# PointSurfaceDistance Plugin
add_subdirectory(PointSurfaceDistance)

# FreeformSurface Plugin
add_subdirectory(FreeformSurface)
