# DeepLux 2D/3D Measurement Data Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make geometry measurement work predictably across image, depth-map, and point-cloud data sources.

**Architecture:** Keep the current `IModule::process(const ImageData&, ImageData&)` pipeline and pass 3D payloads through `ImageData` metadata with a small shared measurement-data helper. Add source/input adapter plugins instead of rewriting `RunEngine`, then connect viewport picking to those adapters.

**Tech Stack:** C++17, Qt `QVariant`, Qt Test, OpenCV where already required, Eigen-backed `PointCloudData`, existing CMake plugin layout.

---

## Scope

This plan deliberately keeps the current module interface. A full `DisplayData`-based runtime pipeline can be designed separately if the metadata bridge becomes a bottleneck.

The implementation must preserve existing 2D plugin names and output keys so current flows and tests keep working.

## Current Gaps

- `DataSource` and display code distinguish `"image"` and `"pointcloud"`, but `RunEngine` still passes only `ImageData`.
- 3D files can be imported and displayed, but they are not naturally available to measurement plugins during flow execution.
- `DistancePP`, `DistancePL`, and `LinesDistance` are 2D calculators; `MeasureGap`, `PointSurfaceDistance`, and `FreeformSurface` contain partial 3D logic with inconsistent parsing.
- The 3D viewport exposes a `pointClicked` signal, but picking is not wired into measurement input.
- There is no small bridge plugin that lets users inject picked points, lines, planes, or point clouds into the flow.

## File Map

Create:
- `src/core/geometry/MeasurementData.h` - shared keys, point/line/plane structs, parser declarations.
- `src/core/geometry/MeasurementData.cpp` - parser implementation for 2D points, 3D points, 2D lines, 3D planes, and point clouds.
- `tests/test_measurementdata.cpp` - focused parser tests.
- `tests/test_measuregap.cpp` - 2D-as-z0 and true 3D gap tests.
- `tests/test_pointsurfacedistance.cpp` - point-to-plane tests and degenerate-plane failure.
- `tests/test_freeformsurface.cpp` - `PointCloudData` input path test.
- `src/plugins/geometry/MeasurementInput/` - small adapter plugin that writes configured points/lines/planes into pipeline metadata.
- `tests/test_measurementinput.cpp` - adapter output tests.
- `src/plugins/image_processing/LoadPointCloud/` - source plugin that loads PLY/TIFF as `PointCloudData` into pipeline metadata.
- `tests/test_loadpointcloud.cpp` - point-cloud loader plugin test.

Modify:
- `src/core/CMakeLists.txt` - add `geometry/MeasurementData.cpp` and header.
- `tests/CMakeLists.txt` - add new tests and plugin links.
- `src/plugins/geometry/CMakeLists.txt` - add `MeasurementInput`.
- `src/plugins/image_processing/CMakeLists.txt` - add `LoadPointCloud`.
- `src/plugins/geometry/DistancePP/DistancePPPlugin.cpp` - use shared 2D parsing.
- `src/plugins/geometry/DistancePL/DistancePLPlugin.cpp` - use shared 2D parsing.
- `src/plugins/geometry/LinesDistance/LinesDistancePlugin.cpp` - use shared 2D parsing.
- `src/plugins/geometry/MeasureGap/MeasureGapPlugin.cpp` - use shared 3D parsing and remove the current `QVector<QPointF>` z parsing bug.
- `src/plugins/geometry/PointSurfaceDistance/PointSurfaceDistancePlugin.cpp` - use shared point/plane parsing and return false for invalid plane input.
- `src/plugins/geometry/FreeformSurface/FreeformSurfacePlugin.cpp` - accept `PointCloudData` in addition to list-based `point_cloud`.
- `src/core/common/ModuleIconProvider.cpp` - add abbreviations for new plugins.
- `src/ui/views/MainWindow.cpp` - add `MeasurementInput` to geometry tools and `LoadPointCloud` to 3D tools.
- `src/ui/widgets/HImageWidget.h/.cpp` - emit image click coordinates.
- `src/ui/display/3d/Viewport3DContent.h/.cpp` - implement `Ctrl+LeftClick` nearest-point picking.
- `src/ui/widgets/ViewportWidget.h/.cpp` - forward 2D/3D picked coordinates.
- `docs/cli.md` and `src/core/agent/knowledge/DEEPLUX.md` - document new plugin names and expected keys.

---

### Task 1: Add Measurement Data Contract

**Files:**
- Create: `src/core/geometry/MeasurementData.h`
- Create: `src/core/geometry/MeasurementData.cpp`
- Modify: `src/core/CMakeLists.txt`
- Test: `tests/test_measurementdata.cpp`

- [ ] **Step 1: Write parser tests**

Create `tests/test_measurementdata.cpp` with these cases:

```cpp
#include <QtTest/QtTest>
#include "core/geometry/MeasurementData.h"

using namespace DeepLux;

class TestMeasurementData : public QObject {
    Q_OBJECT

private slots:
    void parsePoint2DRejects3DList();
    void parsePoint3DAccepts2DListAsZ0();
    void parseLine2DAcceptsFourValueList();
    void parsePlane3DRejectsDegeneratePlane();
    void pointCloudRoundTripThroughImageData();
};

void TestMeasurementData::parsePoint2DRejects3DList() {
    QString error;
    QVERIFY(!MeasurementData::parsePoint2D(QVariantList{1.0, 2.0, 3.0}, &error).has_value());
    QVERIFY(error.contains("2D"));
}

void TestMeasurementData::parsePoint3DAccepts2DListAsZ0() {
    auto point = MeasurementData::parsePoint3D(QVariantList{1.0, 2.0}, nullptr);
    QVERIFY(point.has_value());
    QCOMPARE(point->x, 1.0);
    QCOMPARE(point->y, 2.0);
    QCOMPARE(point->z, 0.0);
}

void TestMeasurementData::parseLine2DAcceptsFourValueList() {
    auto line = MeasurementData::parseLine2D(QVariantList{1.0, 2.0, 3.0, 4.0}, nullptr);
    QVERIFY(line.has_value());
    QCOMPARE(line->p1.x, 1.0);
    QCOMPARE(line->p2.y, 4.0);
}

void TestMeasurementData::parsePlane3DRejectsDegeneratePlane() {
    QString error;
    auto plane = MeasurementData::parsePlane3D(QVariantList{
        0.0, 0.0, 0.0,
        1.0, 1.0, 1.0,
        2.0, 2.0, 2.0
    }, &error);
    QVERIFY(!plane.has_value());
    QVERIFY(error.contains("plane"));
}

void TestMeasurementData::pointCloudRoundTripThroughImageData() {
    PointCloudData cloud;
    cloud.points.push_back(Eigen::Vector3d(1.0, 2.0, 3.0));

    ImageData image;
    MeasurementData::setPointCloud(image, cloud);

    auto parsed = MeasurementData::pointCloud(image, nullptr);
    QVERIFY(parsed.has_value());
    QCOMPARE(static_cast<int>(parsed->points.size()), 1);
    QCOMPARE(parsed->points[0].z(), 3.0);
}

QTEST_MAIN(TestMeasurementData)
#include "test_measurementdata.moc"
```

- [ ] **Step 2: Add the shared contract**

Create `src/core/geometry/MeasurementData.h` with these public names:

```cpp
#pragma once

#include "display/DisplayData.h"
#include "model/ImageData.h"

#include <QPointF>
#include <QVariant>
#include <optional>

namespace DeepLux {

struct MeasurementPoint2D {
    double x = 0.0;
    double y = 0.0;
};

struct MeasurementPoint3D {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct MeasurementLine2D {
    MeasurementPoint2D p1;
    MeasurementPoint2D p2;
};

struct MeasurementPlane3D {
    MeasurementPoint3D p1;
    MeasurementPoint3D p2;
    MeasurementPoint3D p3;
};

namespace MeasurementKeys {
inline constexpr const char* Point = "point";
inline constexpr const char* Point1 = "point1";
inline constexpr const char* Point2 = "point2";
inline constexpr const char* Line = "line";
inline constexpr const char* Line1 = "line1";
inline constexpr const char* Line2 = "line2";
inline constexpr const char* Plane = "plane";
inline constexpr const char* PointCloud = "point_cloud";
inline constexpr const char* Dimension = "measurement_dimension";
}

class MeasurementData {
public:
    static std::optional<MeasurementPoint2D> parsePoint2D(const QVariant& value, QString* error);
    static std::optional<MeasurementPoint3D> parsePoint3D(const QVariant& value, QString* error);
    static std::optional<MeasurementLine2D> parseLine2D(const QVariant& value, QString* error);
    static std::optional<MeasurementPlane3D> parsePlane3D(const QVariant& value, QString* error);

    static void setPointCloud(ImageData& image, const PointCloudData& cloud);
    static std::optional<PointCloudData> pointCloud(const ImageData& image, QString* error);
};

}
```

Implement `MeasurementData.cpp` with strict parsing:
- 2D point accepts `QPointF` or a two-value `QVariantList`.
- 2D point rejects three-value lists with an error telling users to use 3D measurement.
- 3D point accepts `[x,y]` as `z=0` and `[x,y,z]`.
- 2D line accepts `QVector<QPointF>` with two points or `[x1,y1,x2,y2]`.
- 3D plane accepts exactly three 3D points encoded as nine numeric values.
- Plane parser rejects collinear points using normal length `< 1e-10`.
- Point cloud parser accepts `QVariant::fromValue(PointCloudData)` under `point_cloud`.

- [ ] **Step 3: Wire core build**

Modify `src/core/CMakeLists.txt`:

```cmake
set(CORE_SOURCES
    ...
    geometry/MeasurementData.cpp
)

set(CORE_HEADERS
    ...
    geometry/MeasurementData.h
)
```

- [ ] **Step 4: Wire test build**

Modify `tests/CMakeLists.txt`:

```cmake
set(TEST_SOURCES
    ...
    test_measurementdata.cpp
)
```

- [ ] **Step 5: Verify**

Run:

```bash
cmake --build build -j$(nproc)
./build/tests/test_measurementdata -v2
```

Expected: `PASS`.

Commit:

```bash
git add src/core/geometry src/core/CMakeLists.txt tests/CMakeLists.txt tests/test_measurementdata.cpp
git commit -m "feat: add measurement data contract"
```

---

### Task 2: Normalize Existing Geometry Plugins

**Files:**
- Modify: `src/plugins/geometry/DistancePP/DistancePPPlugin.cpp`
- Modify: `src/plugins/geometry/DistancePL/DistancePLPlugin.cpp`
- Modify: `src/plugins/geometry/LinesDistance/LinesDistancePlugin.cpp`
- Modify: `src/plugins/geometry/MeasureGap/MeasureGapPlugin.cpp`
- Modify: `src/plugins/geometry/PointSurfaceDistance/PointSurfaceDistancePlugin.cpp`
- Modify: `src/plugins/geometry/FreeformSurface/FreeformSurfacePlugin.cpp`
- Modify: `tests/CMakeLists.txt`
- Create: `tests/test_measuregap.cpp`
- Create: `tests/test_pointsurfacedistance.cpp`
- Create: `tests/test_freeformsurface.cpp`

- [ ] **Step 1: Add 3D gap tests**

Create `tests/test_measuregap.cpp`:

```cpp
#include <QtTest/QtTest>
#include "plugins/geometry/MeasureGap/MeasureGapPlugin.h"

using namespace DeepLux;

class TestMeasureGap : public QObject {
    Q_OBJECT
private slots:
    void calculates3DGap();
    void treats2DPointAsZ0();
};

void TestMeasureGap::calculates3DGap() {
    MeasureGapPlugin plugin;
    QVERIFY(plugin.initialize());
    ImageData input;
    input.setData("point1", QVariantList{0.0, 0.0, 0.0});
    input.setData("point2", QVariantList{3.0, 4.0, 12.0});
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("gap_distance").toDouble(), 13.0);
    QCOMPARE(output.data("gap_delta_z").toDouble(), 12.0);
    QCOMPARE(output.data("measurement_dimension").toString(), QString("3d"));
}

void TestMeasureGap::treats2DPointAsZ0() {
    MeasureGapPlugin plugin;
    QVERIFY(plugin.initialize());
    ImageData input;
    input.setData("point1", QVariantList{0.0, 0.0});
    input.setData("point2", QVariantList{3.0, 4.0});
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data("gap_distance").toDouble(), 5.0);
    QCOMPARE(output.data("gap_delta_z").toDouble(), 0.0);
    QCOMPARE(output.data("measurement_dimension").toString(), QString("2d"));
}

QTEST_MAIN(TestMeasureGap)
#include "test_measuregap.moc"
```

- [ ] **Step 2: Add point-surface tests**

Create `tests/test_pointsurfacedistance.cpp` with:
- point `[0,0,5]`
- plane `[0,0,0, 1,0,0, 0,1,0]`
- expected distance `5`
- expected foot z `0`
- degenerate plane `[0,0,0, 1,1,1, 2,2,2]` returns false

- [ ] **Step 3: Add freeform surface point-cloud test**

Create `tests/test_freeformsurface.cpp`:
- Build a `PointCloudData` with at least four points.
- Store it with `MeasurementData::setPointCloud(input, cloud)`.
- Execute `FreeformSurfacePlugin`.
- Assert `point_count == 4` and `surface_roughness` is valid.

- [ ] **Step 4: Replace duplicated parsing**

Use `MeasurementData` in the six plugin files:
- `DistancePP`: `parsePoint2D(input.data("point1"))`, `parsePoint2D(input.data("point2"))`.
- `DistancePL`: `parsePoint2D(input.data("point"))`, `parseLine2D(input.data("line"))`.
- `LinesDistance`: `parseLine2D(input.data("line1"))`, `parseLine2D(input.data("line2"))`.
- `MeasureGap`: `parsePoint3D(input.data("point1"))`, `parsePoint3D(input.data("point2"))`.
- `PointSurfaceDistance`: `parsePoint3D(input.data("point"))`, `parsePlane3D(input.data("plane"))`.
- `FreeformSurface`: first try `MeasurementData::pointCloud(input)`, then keep old list fallback only if no `PointCloudData` exists.

Keep existing output keys. Add `measurement_dimension` only where the plugin can run in both 2D and 3D mode.

- [ ] **Step 5: Wire tests and plugin links**

Modify `tests/CMakeLists.txt`:

```cmake
set(TEST_SOURCES
    ...
    test_measuregap.cpp
    test_pointsurfacedistance.cpp
    test_freeformsurface.cpp
)

if(TEST_NAME STREQUAL "test_measuregap" OR TEST_NAME STREQUAL "test_plugincontracts")
    target_link_libraries(${TEST_NAME} PRIVATE MeasureGapPlugin)
endif()
if(TEST_NAME STREQUAL "test_pointsurfacedistance" OR TEST_NAME STREQUAL "test_plugincontracts")
    target_link_libraries(${TEST_NAME} PRIVATE PointSurfaceDistancePlugin)
endif()
if(TEST_NAME STREQUAL "test_freeformsurface" OR TEST_NAME STREQUAL "test_plugincontracts")
    target_link_libraries(${TEST_NAME} PRIVATE FreeformSurfacePlugin)
endif()
```

- [ ] **Step 6: Verify**

Run:

```bash
cmake --build build -j$(nproc)
./build/tests/test_distancepp -v2
./build/tests/test_distancepl -v2
./build/tests/test_linesdistance -v2
./build/tests/test_measuregap -v2
./build/tests/test_pointsurfacedistance -v2
./build/tests/test_freeformsurface -v2
```

Expected: all `PASS`.

Commit:

```bash
git add src/plugins/geometry tests/CMakeLists.txt tests/test_measuregap.cpp tests/test_pointsurfacedistance.cpp tests/test_freeformsurface.cpp
git commit -m "fix: normalize geometry measurement inputs"
```

---

### Task 3: Add MeasurementInput Adapter Plugin

**Files:**
- Create: `src/plugins/geometry/MeasurementInput/CMakeLists.txt`
- Create: `src/plugins/geometry/MeasurementInput/metadata.json`
- Create: `src/plugins/geometry/MeasurementInput/MeasurementInputPlugin.h`
- Create: `src/plugins/geometry/MeasurementInput/MeasurementInputPlugin.cpp`
- Modify: `src/plugins/geometry/CMakeLists.txt`
- Modify: `src/core/common/ModuleIconProvider.cpp`
- Modify: `src/ui/views/MainWindow.cpp`
- Create: `tests/test_measurementinput.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write adapter tests**

Create `tests/test_measurementinput.cpp`:
- `mode="point_pair"` writes `point1` and `point2`.
- `mode="point_plane"` writes `point` and `plane`.
- Invalid point values make `execute()` return false.

- [ ] **Step 2: Implement plugin parameters**

`MeasurementInputPlugin` default params:

```cpp
m_defaultParams = QJsonObject{
    {"mode", "point_pair"},
    {"point1", QJsonArray{0.0, 0.0}},
    {"point2", QJsonArray{0.0, 0.0}},
    {"point", QJsonArray{0.0, 0.0, 0.0}},
    {"line", QJsonArray{0.0, 0.0, 100.0, 0.0}},
    {"plane", QJsonArray{0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0}}
};
```

`process()` must:
- Copy `input` to `output`.
- Validate values through `MeasurementData`.
- Write canonical keys into `output`.
- Write `measurement_input_mode`.

- [ ] **Step 3: Add config widget**

Use existing Qt widgets only:
- `QComboBox` for mode.
- `QLineEdit` for comma-separated point/line/plane values.
- No custom editor library.

- [ ] **Step 4: Add plugin to build and UI**

Modify `src/plugins/geometry/CMakeLists.txt`:

```cmake
add_subdirectory(MeasurementInput)
```

Modify `src/ui/views/MainWindow.cpp` geometry category:

```cpp
addToolBoxItem(geometryItem, tr("📍 测量输入"), "MeasurementInput");
```

Modify `ModuleIconProvider.cpp`:

```cpp
{"MeasurementInput", "MI"},
```

- [ ] **Step 5: Verify**

Run:

```bash
cmake --build build -j$(nproc)
./build/tests/test_measurementinput -v2
./build/tests/test_plugincontracts -v2
```

Commit:

```bash
git add src/plugins/geometry/MeasurementInput src/plugins/geometry/CMakeLists.txt src/core/common/ModuleIconProvider.cpp src/ui/views/MainWindow.cpp tests/CMakeLists.txt tests/test_measurementinput.cpp
git commit -m "feat: add measurement input adapter"
```

---

### Task 4: Add LoadPointCloud Source Plugin

**Files:**
- Create: `src/plugins/image_processing/LoadPointCloud/CMakeLists.txt`
- Create: `src/plugins/image_processing/LoadPointCloud/metadata.json`
- Create: `src/plugins/image_processing/LoadPointCloud/LoadPointCloudPlugin.h`
- Create: `src/plugins/image_processing/LoadPointCloud/LoadPointCloudPlugin.cpp`
- Modify: `src/plugins/image_processing/CMakeLists.txt`
- Modify: `src/core/common/ModuleIconProvider.cpp`
- Modify: `src/ui/views/MainWindow.cpp`
- Create: `tests/test_loadpointcloud.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write loader plugin test**

Use a temporary ASCII PLY file in `QTemporaryDir`.

Expected output:
- `point_cloud` contains `PointCloudData`.
- `point_count == 2`.
- `point_cloud_path` equals input file path.

- [ ] **Step 2: Implement plugin**

Default params:

```cpp
m_defaultParams = QJsonObject{
    {"filePath", ""},
    {"tiffStep", 1},
    {"scaleX", 1.0},
    {"scaleY", 1.0},
    {"scaleZ", 1.0}
};
```

`process()`:
- Validate `filePath`.
- Use `PlyLoader` for `.ply`.
- Use `TiffLoader` for `.tif/.tiff`.
- Store cloud with `MeasurementData::setPointCloud(output, cloud)`.
- Store `point_count` and `point_cloud_path`.

- [ ] **Step 3: Add to 3D tool category**

Modify `src/ui/views/MainWindow.cpp` under `11 - 3D 工具`:

```cpp
addToolBoxItem(tool3DItem, tr("☁️ 加载点云"), "LoadPointCloud");
```

- [ ] **Step 4: Verify**

Run:

```bash
cmake --build build -j$(nproc)
./build/tests/test_loadpointcloud -v2
./build/tests/test_plugincontracts -v2
```

Manual flow:

```text
LoadPointCloud -> FreeformSurface
```

Expected:
- `FreeformSurface` receives `point_cloud`.
- Logs show point count and roughness.

Commit:

```bash
git add src/plugins/image_processing/LoadPointCloud src/plugins/image_processing/CMakeLists.txt src/core/common/ModuleIconProvider.cpp src/ui/views/MainWindow.cpp tests/CMakeLists.txt tests/test_loadpointcloud.cpp
git commit -m "feat: add point cloud source plugin"
```

---

### Task 5: Wire 2D and 3D Viewport Picking

**Files:**
- Modify: `src/ui/widgets/HImageWidget.h`
- Modify: `src/ui/widgets/HImageWidget.cpp`
- Modify: `src/ui/display/3d/Viewport3DContent.h`
- Modify: `src/ui/display/3d/Viewport3DContent.cpp`
- Modify: `src/ui/widgets/ViewportWidget.h`
- Modify: `src/ui/widgets/ViewportWidget.cpp`
- Modify: `src/ui/views/MainWindow.h`
- Modify: `src/ui/views/MainWindow.cpp`
- Test: existing UI tests plus manual screenshots.

- [ ] **Step 1: Add 2D image click signal**

Add signal to `HImageWidget`:

```cpp
void imageClicked(const QPointF& imagePoint);
```

Emit it on left-button release when movement is below drag threshold.

- [ ] **Step 2: Implement 3D nearest-point picking**

In `Viewport3DContent`:
- Store the last displayed point positions in a lightweight `std::vector<QVector3D>`.
- On `Ctrl+LeftClick`, project points using the current view/projection matrix.
- Pick the nearest screen point within 12 px.
- Emit existing `pointClicked(index, point)`.

Add this comment above the scan:

```cpp
// ponytail: O(n) picking is enough for interactive setup; replace with spatial index if point clouds exceed UI latency budget.
```

- [ ] **Step 3: Forward picked points through ViewportWidget**

Add signals:

```cpp
void point2DClicked(const QPointF& point);
void point3DClicked(const QVector3D& point);
```

Connect `HImageWidget::imageClicked` and `Viewport3DContent::pointClicked`.

- [ ] **Step 4: Connect picked points to MeasurementInput**

In `MainWindow`, when a point is picked:
- If selected flow node is `MeasurementInput`, update its next empty point field.
- If no `MeasurementInput` node is selected, log the coordinate and copy a comma-separated value to clipboard.

No global selection model is needed in this phase.

- [ ] **Step 5: Verify manually**

Run:

```bash
cmake --build build -j$(nproc)
QT_QPA_PLATFORM=xcb ./build/bin/DeepLux --gui
```

Manual checks:
- Load a 2D image.
- Click image and confirm status/log shows `x,y`.
- Add `MeasurementInput`, select it, click two points, confirm its params update.
- Load a PLY/TIFF point cloud.
- `Ctrl+LeftClick` a point, confirm log shows `x,y,z`.

Capture screenshots after each manual check.

Commit:

```bash
git add src/ui/widgets src/ui/display/3d src/ui/views
git commit -m "feat: wire viewport picking to measurement input"
```

---

### Task 6: Add Measurement Result Visibility

**Files:**
- Modify: `src/ui/views/MainWindow.cpp`
- Modify: existing log/result panel code only if a reusable result renderer already exists.
- Test: `tests/test_mainwindow.cpp`

- [ ] **Step 1: Decide the smallest UI surface**

Use the existing bottom log/agent area and append compact measurement summaries. Do not add a new dock panel in this phase.

- [ ] **Step 2: Add result formatting helper**

Add a local helper in `MainWindow.cpp`:
- Reads known keys from `ImageData::allData()`.
- Formats one-line summaries:
  - `distance`
  - `gap_distance`
  - `line_length`
  - `rect_width/rect_height`
  - `surface_roughness`
  - `point_count`

- [ ] **Step 3: Wire after module completion**

Use the existing run completion path. Append only non-empty measurement summaries.

- [ ] **Step 4: Verify**

Run:

```bash
cmake --build build -j$(nproc)
./build/bin/test_mainwindow -v2
```

Manual checks:
- `MeasurementInput -> DistancePP`
- `LoadPointCloud -> FreeformSurface`
- Confirm result lines are readable and not clipped in the bottom panel.

Commit:

```bash
git add src/ui/views/MainWindow.cpp tests/test_mainwindow.cpp
git commit -m "feat: show measurement result summaries"
```

---

### Task 7: Document Agent and CLI Usage

**Files:**
- Modify: `docs/cli.md`
- Modify: `src/core/agent/knowledge/DEEPLUX.md`

- [ ] **Step 1: Add plugin list entries**

Document:
- `MeasurementInput`
- `LoadPointCloud`
- `MeasureGap`
- `PointSurfaceDistance`
- `FreeformSurface`

- [ ] **Step 2: Add example flows**

Add these examples:

```text
2D point distance:
MeasurementInput(point1=[10,20], point2=[40,60]) -> DistancePP

2.5D / 3D gap:
MeasurementInput(point1=[10,20,1.2], point2=[40,60,4.2]) -> MeasureGap

Point-cloud roughness:
LoadPointCloud(filePath=/path/to/cloud.ply) -> FreeformSurface

Point-to-plane:
MeasurementInput(point=[0,0,5], plane=[0,0,0, 1,0,0, 0,1,0]) -> PointSurfaceDistance
```

- [ ] **Step 3: Verify docs do not contradict UI names**

Run:

```bash
rg -n "MeasurementInput|LoadPointCloud|MeasureGap|PointSurfaceDistance|FreeformSurface" docs/cli.md src/core/agent/knowledge/DEEPLUX.md
```

Commit:

```bash
git add docs/cli.md src/core/agent/knowledge/DEEPLUX.md
git commit -m "docs: document 2d and 3d measurement flows"
```

---

### Task 8: Final Verification Pass

**Files:**
- No planned source changes unless a verification failure identifies a root cause.

- [ ] **Step 1: Run focused test set**

```bash
./build/tests/test_measurementdata -v2
./build/tests/test_measuregap -v2
./build/tests/test_pointsurfacedistance -v2
./build/tests/test_freeformsurface -v2
./build/tests/test_measurementinput -v2
./build/tests/test_loadpointcloud -v2
./build/tests/test_plugincontracts -v2
./build/tests/test_mainwindow -v2
```

- [ ] **Step 2: Run all tests if focused tests pass**

```bash
ctest --test-dir build --output-on-failure
```

- [ ] **Step 3: Manual GUI scenario**

Run:

```bash
QT_QPA_PLATFORM=xcb ./build/bin/DeepLux --gui
```

Check:
- 2D image load and click coordinate.
- 3D point cloud load and `Ctrl+LeftClick` coordinate.
- `MeasurementInput -> DistancePP`.
- `MeasurementInput -> MeasureGap`.
- `LoadPointCloud -> FreeformSurface`.
- Result summaries visible in bottom area.
- Tool categories show no duplicate or mismatched plugin names.

- [ ] **Step 4: Screenshot audit**

Capture:
- Main window with geometry tools open.
- 2D point-picking example.
- 3D point-picking example.
- Bottom result summary after a flow run.

- [ ] **Step 5: Commit fixes from verification**

Use one small commit per root cause, for example:

```bash
git add <files>
git commit -m "fix: correct 3d point picking result display"
```

---

## Execution Order

1. Task 1: shared contract.
2. Task 2: normalize current plugins.
3. Task 3: coordinate adapter plugin.
4. Task 4: point-cloud source plugin.
5. Task 5: viewport picking.
6. Task 6: result visibility.
7. Task 7: docs and agent knowledge.
8. Task 8: verification.

## Acceptance Criteria

- Existing 2D measurement tests still pass.
- 2D plugins reject accidental 3D points instead of silently dropping z.
- `MeasureGap` supports both `[x,y]` and `[x,y,z]`.
- `PointSurfaceDistance` rejects degenerate planes.
- `FreeformSurface` accepts real `PointCloudData`.
- Point cloud files can enter a flow through `LoadPointCloud`.
- Picked 2D/3D coordinates can populate `MeasurementInput`.
- User can run at least these flows from the GUI:
  - `MeasurementInput -> DistancePP`
  - `MeasurementInput -> MeasureGap`
  - `LoadPointCloud -> FreeformSurface`
- Agent and docs describe the same plugin names shown in the UI.
