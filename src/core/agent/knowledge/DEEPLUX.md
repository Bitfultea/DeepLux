# DeepLux Vision — Agent Knowledge Base

## How DeepLux Works

DeepLux is an industrial machine vision software. You create inspection **projects**, add **modules** (image processing plugins) to a **flow**, connect them in sequence, configure parameters, and run the flow to inspect images.

### Core Concepts

- **Project (.dproj)**: Contains modules, connections, and camera configs. Create with `create_project`, open with `open_project`.
- **Module**: A processing step (e.g. GrabImage captures, FindCircle detects). Add with `add_module(plugin, instanceName)`.
- **Module instance ID**: The stable reference returned by `add_module` and `get_flow_state`; `rename_module(instanceId, name)` changes only the display name, and `move_module(instanceId, index)` changes its zero-based order.
- **Legacy connection types**: `get_flow_state` preserves the stored `edgeType`; when an older project omitted it, `effectiveEdgeType` reports the same metadata-based inference used by the run engine and `edgeTypeResolution` says `inferred`. If endpoint plugin metadata is unavailable, resolution is `unavailable` rather than guessed.
- **Enabled state**: `set_module_enabled(instanceId, enabled)` accepts a JSON boolean and changes whether the existing engine bypasses a module. Stop a running flow before editing it; Agent Undo/Redo can restore this project edit.
- **Breakpoint**: `set_breakpoint(instanceId, enabled)` accepts a JSON boolean and updates the project and existing run-engine breakpoint for that stable instance ID. Stop the flow before changing it; Agent Undo/Redo restores the edit.
- **Module note**: `set_module_note(instanceId, note)` updates the module note; pass an empty string to clear it. The project change can be undone by the Agent Undo stack.
- **Flow**: Modules connected in order. Data flows from one module to the next (ImageData pipeline).
- **Connection**: Links two modules. Use `connect_modules(fromId, toId)`.
- **Run**: Execute the flow. Use `run_flow(mode="once"|"cycle")`. Results and images are displayed.
- **Single-step**: Use `step_once` to execute one module with the same RunEngine single-step behavior as the GUI; each call is synchronous and may have file/device/network side effects, so confirmation is required. Use `reset_step` to restart the next single-step call at the flow entry.
- **Preflight validation**: Use `validate_flow` before running to check required input connections, port/edge compatibility, and execution/control graph constraints. It reports `valid`, all missing-required-input findings, and at most the first structural graph/port error per call, with locations when available; it does not execute modules, request GUI synchronization, clear outputs, or reset step position. If the engine runtime is stale or busy, it fails closed instead of replacing the active runtime.

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
- Use instance IDs, not display names, to identify modules; display names are not guaranteed unique
- `save_project` writes to disk and is not undone by Agent Undo
- `remove_module` and `disconnect_modules` are dangerous
- `sam_annotate` is dangerous (writes annotation files); `sam_predict` only runs inference (no file writes)
- `run_flow` returns immediately (async), use `get_run_results` to check completion

---
## Module and Workflow Documentation

Do not infer module parameters from a duplicated summary in this file. Use the source-backed documents bundled in
the knowledge base:

- `USER_MANUAL.md`: GUI operation, parameter tuning, 2D/3D workflows, Agent/SAM, troubleshooting, and acceptance.
- `PLUGINS.md`: all 69 runtime plugins, their upstream data, key configuration, outputs, and failure conditions.
- `get_module_params_schema(instanceId)`: the exact current parameters and values for a module instance.

For an unfamiliar module, first call `read_documentation` with the exact module name, then call
`get_module_params_schema` before changing any parameter. Runtime schema and validation take precedence over prose.

### Minimal Inspection Workflow

```text
GrabImage.image → PerProcessing.image (optional) → FindCircle.image → SaveImage.image (optional)
```

Create or open a project, add modules, inspect each instance schema, set parameters, connect compatible typed ports,
save when requested, run once, wait for completion, and then call `get_run_results`.

### Hymson 3D plugin availability

`DefectDetection` is currently an unavailable framework plugin. With `ENABLE_HYMSON3D=OFF`, it builds in demo mode;
the plugin CMake also currently disables the SDK algorithm branch with `HymsonVision3D_FOUND AND FALSE`. Running it
fails with `HymsonVision3D SDK 未启用，缺陷检测不可用` and produces no detection result. Its declared output ports
(`defect_point_cloud`, `has_defects`, `defect_count`) remain visible in the module schema, but the current build cannot
produce queryable values for them because the algorithm run fails. Do not present this plugin as a working inspection
algorithm or use its result for a product decision until the SDK path is enabled and a valid business fixture passes.

---
## Direct SAM Tools

### User-driven image selection
`open_sam_image_picker_for_user()` opens or reuses the SAM annotation window and makes its existing **Open Image** button available. The user selects the file in the GUI. This handoff does not accept a path, read an image, replace the current session, or run a prediction. Use `sam_predict(imagePath, ...)` only when the user requested a direct SAM prediction from a known image path; it is not the GUI image-open action.

### SAM Segmentation
`sam_predict` / `sam_annotate` call the local SAM server directly — no project or flow needed:
```
sam_predict(imagePath, points=[[x,y],...], negativePoints?, box=[x,y,w,h]?, label?) → {score, bbox, polygon}
sam_annotate(imagePath, sessionPath, items=[{points?, negativePoints?, box?, label?}, ...1-16]) → session file
```
- Both require a RUNNING SAM server (default `http://127.0.0.1:8990`; the user starts it from the annotation panel: initialize environment + import weights). If the server is unreachable they fail closed with an error — report it to the user, do not retry in a tight loop.
- `imagePath` must be an existing image file on disk; every prompt needs `points` or `box` (at least one), finite numbers only.
- `sam_annotate` is dangerous (writes a `.deeplux-anno.json` annotation session file). All-or-nothing: nothing is written if any prompt fails. A session is bound to one image — appending with a different `imagePath` is rejected.
- `get_annotation_session(sessionPath, offset?, limit?, includePolygon?)` is a read-only query of one explicitly named existing session. It returns stable object IDs, labels, original-image bounding boxes, prompt counts, and optionally full original-image polygons. It never returns mask RLE, starts SAM, decodes the source image, or opens the GUI session. Results are paged and limited to 64 KiB.
- For in-flow segmentation use the `SamPredict` module instead (+ `RegionStats` for area/centroid statistics of the Region2D output).

---
## Tips for Effective Agent Work

1. **Always call `get_flow_state()` first** when the user asks about current project status
2. **Use `get_module_params_schema(instanceId)`** before setting params on an unfamiliar module
3. **Use descriptive instanceNames**: e.g. "grab_left_camera" not "grab_1"
4. **Check connections**: modules won't execute without proper connections
5. **Save before running**: call `save_project` then `run_flow`
6. **Param values are strings**: JSON values on set_param are always quoted strings in the request
7. **Order matters**: modules execute in the order they were added, unless connected differently

---
## Knowledge Base

`read_documentation(topic)` searches two sources, split into `##` / `###` sections and matched by keyword:

1. **Built-in docs** — bundled with the application (read-only), including this document, the software operation
   manual, and the complete plugin manual.
2. **User docs** — Markdown/text files the user added via **Agent Settings → Knowledge Base**
   (工具 → Agent 设置 → 知识库). They are copied into the user knowledge directory
   (`~/.deeplux/knowledge` on Linux) and searched together with built-in docs.

Each returned section is prefixed with `[来源: <file>]` so you can tell which document it came from.
When the user asks about site-specific conventions, recipes, or domain knowledge that is not covered
by built-in documentation, search the knowledge base first — the user may have added custom docs.
If nothing matches, tell the user they can add a document via Agent Settings → Knowledge Base.
