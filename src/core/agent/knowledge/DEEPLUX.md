# DeepLux Vision — Agent Knowledge Base

## How DeepLux Works

DeepLux is an industrial machine vision software. You create inspection **projects**, add **modules** (image processing plugins) to a **flow**, connect them in sequence, configure parameters, and run the flow to inspect images.

### Core Concepts

- **Project (.dproj)**: Contains modules, connections, and camera configs. Create with `create_project`, open with `open_project`.
- **Module**: A processing step (e.g. GrabImage captures, FindCircle detects). Add with `add_module(plugin, instanceName)`.
- **Flow**: Modules connected in order. Data flows from one module to the next (ImageData pipeline).
- **Connection**: Links two modules. Use `connect_modules(fromId, toId)`.
- **Run**: Execute the flow. Use `run_flow(mode="once"|"cycle")`. Results and images are displayed.

### Workflow Order (Critical)
1. `create_project` or `open_project` — must be first
2. `add_module` — add modules in processing order
   - **GrabImage** first (captures input)
   - Then detection/measurement modules
3. `set_param` — configure module parameters
4. `connect_modules` — link module outputs to next inputs
5. `save_project` — persist to disk
6. `run_flow` — execute

### Parameter Setting
- Use `get_module_params_schema(instanceId)` to discover what parameters a module accepts
- Use `set_param(instanceId, key, value)` to set values
- Parameters are stored as JSON values in the project

### Important Rules
- Always check `get_flow_state()` before modifying to understand current state
- Never add duplicate module instances with the same name
- `save_project` is a dangerous operation (triggers undo point)
- `remove_module` and `disconnect_modules` are dangerous
- `run_flow` returns immediately (async), use `get_run_results` to check completion

---
## Module Reference

### Image Acquisition
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| GrabImage | `GrabImage` | Capture image from camera, file, or image folder | `filePath` (auto-detects file or folder), `folderLoop`, `grabSource` (Path/Camera/Demo), `cameraId`; legacy `File`, `Folder`, and `folderPath` remain readable |
| SaveImage | `SaveImage` | Save processed image to disk | `filePath` (string), `format` (enum: PNG/JPG/BMP) |
| ShowImage | `ShowImage` | Display image in viewport | `title` (string), `delay` (int ms) |

### Image Processing
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| PerProcessing | `PerProcessing` | Image preprocessing (filter, threshold) | `filterType` (enum), `threshold` (int), `kernelSize` (int) |
| ColorRecognition | `ColorRecognition` | Detect colors in image | `targetColor` (string hex), `tolerance` (int) |
| Blob | `Blob` | Blob analysis (connected components) | `minArea` (int), `maxArea` (int), `threshold` (int) |

### Detection
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| Matching | `Matching` | Template matching | `templatePath` (string), `score` (float 0-1), `angle` (float) |
| QRCode | `QRCode` | QR/Barcode reading | `codeType` (enum), `timeout` (int ms) |

### Geometry Measurement
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| FindCircle | `FindCircle` | Detect circles in image | `minRadius` (int), `maxRadius` (int), `edgeThreshold` (int), `centerX/Y` (int) |
| FitCircle | `FitCircle` | Fit circle to points | `points` (string), `method` (enum) |
| FitLine | `FitLine` | Fit line to points | `points` (string), `method` (enum) |
| DistancePP | `DistancePP` | Point-to-point distance | `point1X/Y` (int), `point2X/Y` (int) |
| DistancePL | `DistancePL` | Point-to-line distance | `pointX/Y` (int), `lineA/B/C` (float) |
| LinesDistance | `LinesDistance` | Distance between two lines | `line1/2 A/B/C` (float) |
| MeasureRect | `MeasureRect` | Rectangle measurement | `x/y/width/height` (int) |
| MeasureLine | `MeasureLine` | Line measurement | `startX/Y` (int), `endX/Y` (int) |
| MeasureGap | `MeasureGap` | 2D/3D gap measurement between two points | takes `point1` [x,y] or [x,y,z] and `point2` [x,y] or [x,y,z] via pipeline metadata |
| PointSurfaceDistance | `PointSurfaceDistance` | Point-to-plane distance | takes `point` [x,y,z] and `plane` [x1,y1,z1,x2,y2,z2,x3,y3,z3] via pipeline metadata |
| FreeformSurface | `FreeformSurface` | Surface roughness from point cloud | accepts `PointCloudData` via `point_cloud` key |
| MeasurementInput | `MeasurementInput` | Adapter: writes measurement coordinates into pipeline | `mode` (point_pair/point_line/point_plane/custom), `point1`, `point2`, `point`, `line`, `plane` as coordinate arrays |

### Calibration
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| NPointCalibration | `NPointCalibration` | N-point calibration (pixel→world) | `points` (string array), `method` (enum) |

### Logic/Control Flow
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| If | `com.deeplux.plugin.if` | Conditional branch | `condition` (string expression) |
| Loop | `com.deeplux.plugin.loop` | Fixed-count loop | `loopCount` (int) |
| While | `com.deeplux.plugin.while` | Conditional loop | `condition` (string), `maxIterations` (int) |
| Delay | `Delay` | Wait/delay | `delayMs` (int) |
| StopWhile | `com.deeplux.plugin.stopwhile` | Break from while loop | — |

### 3D / Point Cloud
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| LoadPointCloud | `LoadPointCloud` | Load PLY/TIFF file as point cloud data | `filePath` (string), `tiffStep` (int), `scaleX/Y/Z` (float) |

### System
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| SystemTime | `SystemTime` | Get system timestamp | `format` (string) |
| Folder | `Folder` | File/folder operations | `operation` (enum), `path` (string) |
| SaveData | `SaveData` | Save data to file | `filePath` (string), `format` (enum) |
| WriteText | `WriteText` | Write text output | `filePath` (string), `content` (string) |
| TableOutPut | `TableOutPut` | Export table data | `filePath` (string), `format` (enum) |

### Variables
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| VarDefine | `VarDefine` | Define a variable | `name` (string), `type` (enum), `value` (string) |
| VarSet | `VarSet` | Set variable value | `name` (string), `value` (string) |
| Math | `Math` | Math expression evaluation | `expression` (string) |
| DataCheck | `DataCheck` | Validate data against rules | `rule` (string), `value` (string) |

### Communication
| Module | Plugin ID | Purpose | Key Parameters |
|--------|-----------|---------|----------------|
| PLCCommunicate | `PLCCommunicate` | PLC communication | `ip` (string), `port` (int), `protocol` (enum) |
| PLCRead | `PLCRead` | Read PLC register | `address` (string), `type` (enum) |
| PLCWrite | `PLCWrite` | Write PLC register | `address` (string), `value` (string) |
| TCPClient | `TCPClient` | TCP client connection | `ip` (string), `port` (int) |
| TCPServer | `TCPServer` | TCP server | `port` (int) |
| SerialPort | `SerialPort` | Serial port communication | `port` (string), `baudRate` (int) |

---
## Common Workflows

### Basic Inspection Flow
```
GrabImage → PerProcessing → FindCircle → SaveImage
```
1. GrabImage captures from camera/file
2. PerProcessing filters/enhances
3. FindCircle detects circles
4. SaveImage saves result

### Measurement Flow
```
GrabImage → FitLine → FitCircle → DistancePP → ShowImage
```

### QC with PLC
```
GrabImage → Matching → DistancePP → DataCheck → PLCWrite
```

### 2D Point Distance Measurement
```
MeasurementInput(point1=[10,20], point2=[40,60]) → DistancePP
```

### 2.5D / 3D Gap Measurement
```
MeasurementInput(point1=[10,20,1.2], point2=[40,60,4.2]) → MeasureGap
```
MeasureGap auto-detects 2D vs 3D: if any point has z≠0, it runs in 3D mode.

### Point Cloud Surface Roughness
```
LoadPointCloud(filePath=/path/to/cloud.ply) → FreeformSurface
```

### Point-to-Plane Distance
```
MeasurementInput(point=[0,0,5], plane=[0,0,0, 1,0,0, 0,1,0]) → PointSurfaceDistance
```

---
## Tips for Effective Agent Work

1. **Always call `get_flow_state()` first** when the user asks about current project status
2. **Use `get_module_params_schema(instanceId)`** before setting params on an unfamiliar module
3. **Use descriptive instanceNames**: e.g. "grab_left_camera" not "grab_1"
4. **Check connections**: modules won't execute without proper connections
5. **Save before running**: call `save_project` then `run_flow`
6. **Param values are strings**: JSON values on set_param are always quoted strings in the request
7. **Order matters**: modules execute in the order they were added, unless connected differently
