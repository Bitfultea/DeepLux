# Geometry plugins

if(NOT OpenCV_FOUND)
    message(STATUS "Skipping geometry plugins (OpenCV not found)")
    return()
endif()

# FitLine Plugin
add_subdirectory(FitLine)

# FitCircle Plugin
add_subdirectory(FitCircle)

# FitEllipse Plugin（阶7 批1）
add_subdirectory(FitEllipse)

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

# MeasurementInput Plugin
add_subdirectory(MeasurementInput)

# FitPlane Plugin（阶7 批3：0143D 重建）
add_subdirectory(FitPlane)

# GapMeasure3D Plugin（阶7 批3：0143D 重建）
add_subdirectory(GapMeasure3D)
