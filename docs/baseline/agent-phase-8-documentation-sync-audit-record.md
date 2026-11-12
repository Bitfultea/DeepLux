# G8 documentation synchronization audit record

## Status

**PASS** for the documentation/runtime-resource synchronization sub-item of
G8 criterion 5 after independent Slice36 implementation review. This scoped
PASS is recorded in the plan checklist; it does not claim the other G8
criteria or full stage are complete. Overall G8 remains `NOT_READY`.

## Contract

`docs/baseline/agent-phase-8-documentation-sync-audit-contract.md` received
`CONTRACT_PASS` from `/root/slice176_final_review` before the audit.

## Findings

- The static inventory found 69 plugin `metadata.json` files, 69 distinct
  metadata links, and 69 link occurrences in `docs/plugins.md`; missing,
  extra, and duplicate links: 0. Per-plugin article headings and documented
  purpose/configuration/check sections are indexed in
  `build-codex-agent/agent-phase8-current-matrix-evidence/documentation-plugin-audit.tsv`.
  The `DefectDetection` article intentionally documents the unavailable state
  and data contract instead of promising a usable operation.
- Refreshing the candidate inventory against its reviewed fixture-audit log
  was required because the CMake test source list had changed in Slice30. The
  only semantic inventory delta is one newly indexed QtTest name (1420 →
  1421); nine candidate rows mention it, and no reviewed business/critical
  failure fixture status changed. `DefectDetection` appears as a candidate
  only because the documentation test contains its name; this is not plugin
  business evidence and its fixture remains `FAIL`.
- `agent_resources.qrc` bundles exactly `DEEPLUX.md`, `PLUGINS.md`, and
  `USER_MANUAL.md`. Runtime enumeration in `AgentActor::knowledgeDocumentPaths`
  uses those built-ins plus ordinary files from the user's knowledge folder;
  it does not scan `docs/baseline` or build evidence.
- New case
  `testBuiltinDocsStaySourcedAndAuditArtifactsStayExcluded` asserts the three
  built-in IDs in an isolated app-data directory; unique user-manual content
  is returned with `[来源: USER_MANUAL.md]`; DefectDetection content is
  returned with source attribution for all three built-ins; and the tool
  catalog exposes no shell input/execution tool.
- `docs/user-manual.md`, `docs/plugins.md`, and embedded `DEEPLUX.md` all
  state that DefectDetection is unavailable in the current build, report
  `HymsonVision3D SDK 未启用，缺陷检测不可用`, and distinguish declared output
  ports from values currently produced. The implementation has both
  `ENABLE_HYMSON3D=OFF` and `HymsonVision3D_FOUND AND FALSE` build guards.
- `get_module_params_schema` remains the source of exact instance parameters;
  `read_documentation` searches titled document sections and prefixes returned
  sections with source filenames. The user manual and Agent knowledge say the
  same; `ToolSchema` documents pagination and permission requirements.

## Slice31 finding and Slice32 correction

Slice31 found that `docs/user-manual.md` labeled itself as applicable to the
**2026-09-22** source workspace, while this audit is against the
**2026-11-03** `DeepLux_codex` working-tree snapshot. Slice32 was separately
contracted and received `/root/slice176_final_review` **CONTRACT_PASS** before
editing. It changed only line 5's applicability marker to:

> 内容核对日期：2026-11-03（DeepLux_codex 工作区快照，不代表产品发布版本）。

The adjacent operational cautions were preserved. The corrected date is a
workspace content-check marker, not a product release/version claim.

## Slice32 verification

- Rebuilt all QRC-consuming focused targets:
  `cmake --build build-codex-agent --target test_toolschema test_knowledgebasedialog test_agentphase0_repro -j2`, exit 0; log `slice32-focused-build.log`.
- Focused CTest:
  `ctest --test-dir build-codex-agent -R '^(test_agentphase0_repro|test_toolschema|test_knowledgebasedialog)$' --output-on-failure -j2`, 3/3 passed, 0 failed; log `slice32-focused-ctest.log`. The first restricted run is retained in `documentation-focused-1.log` from Slice31 and the current Slice32 restricted attempt failed only because the existing PLC loopback listener could not bind; the authorized loopback rerun passed.
- Text check: line 5 contains `内容核对日期：2026-11-03（DeepLux_codex 工作区快照，不代表产品发布版本）` exactly once; `2026-09-22` is absent from `docs/user-manual.md`.
- G8 current-matrix and plugin-inventory `--check` both pass; their 19 Python auditor tests pass. The evidence SHA-256 manifest was refreshed and verifies successfully.
- `git diff --check` passes. Independent review of the full Slice31 audit plus this narrow correction is pending. G8 criterion 5 remains unchecked until that review completes.

## Slice33 findings and corrections

- Independent review found that the TCPServer article omitted `bindAddress`,
  whose implementation default is `Any` (all network interfaces); metadata
  also offers `LocalHost` (local machine only). `docs/plugins.md` now states
  both options, the default and its reachability/security consequence, and
  recommends using the narrowest scope appropriate for deployment.
- The review found that the source-list test did not compare the complete
  isolated document inventory. It now asserts exactly three IDs:
  `builtin:DEEPLUX.md`, `builtin:PLUGINS.md`, and
  `builtin:USER_MANUAL.md`, and total count 3.
- The shell-boundary test now examines every registered tool's description
  and parameter names/descriptions for command/script payloads and
  shell/process execution phrasing. Enum values are not interpreted as
  execution capabilities, so the UI-only `select_log_panel_tab` terminal
  option is not treated as shell access. The existing no-shell-alias and
  source-attribution checks remain.
- A 69-row semantic crosswalk keyed by unique metadata ID is at
  `build-codex-agent/agent-phase8-current-matrix-evidence/documentation-plugin-semantic-crosswalk.tsv`.
  Each row includes its article claims, metadata path, a concrete source
  symbol, platform/SDK applicability assessment, and review status. All rows
  remain `NOT_REVIEWED` until the independent reviewer verifies them; the
  crosswalk does not assert a semantic PASS.
- Rebuilt QRC-consuming focused targets successfully; focused CTest passed
  3/3 in the loopback-enabled environment. Matrix and candidate-inventory
  `--check` passed, their auditor tests passed 19/19, `clang-format` and
  `git diff --check` passed. Logs use the `slice33-` prefix in the current
  matrix evidence directory.
- Crosswalk structural validation confirms 69 unique metadata IDs and 69
  source symbols resolving in source files; all statuses remain
  `NOT_REVIEWED`. TCPServer doc/source consistency checks pass. The current
  SHA-256 manifest verifies; see `slice33-crosswalk-check.log` and
  `slice33-sha256-check.log`.
- Independent row-level review of the 69 claims and the corrections above is
  pending. G8 criterion 5 remains unchecked.

## Slice34 findings and corrections

- The next independent review found that the GrabImage article did not explain
  folder input and `folderLoop`. The article now documents single-file versus
  folder input, filename-sorted progression, `folderLoop` defaulting on,
  restart at the first image when enabled, and the end-of-folder failure when
  disabled. These points were checked against `metadata.json` and
  `GrabImagePlugin::process` / `GrabImagePlugin::grabFromFolder`.
- The shell-capability assertion now incorporates every registered tool name
  into its scan, in addition to each description and parameter name and
  description. It checks shell/bash names and execution verb/object patterns,
  arbitrary command/script payload parameters and command execution phrasing;
  the UI-only terminal page selector remains allowed.
- Crosswalk implementation references were revised from constructor-only
  references to behavior/configuration methods where available. A structural
  check confirms 69 unique metadata IDs and all referenced symbols exist in
  their source files. Statuses remain `NOT_REVIEWED` until row-level
  independent review.
- Since `docs/user-manual.md` is untracked and has no Git pre-Slice32 version,
  the current manual was copied and the exact recorded one-line Slice32 patch
  was reversed to produce
  `slice32-manual-prechange-reconstructed.md`. Its unified diff against the
  current manual shows only the date-marker line. This is explicitly a
  reconstructed comparison aid, not a contemporaneous snapshot or Git
  version.
- Focused QRC target build succeeded; focused CTest passed 3/3 in the
  loopback-enabled environment. Matrix/inventory checks and auditor tests
  passed (19/19); `clang-format`, `git diff --check`, GrabImage/source
  consistency, and baseline-reconstruction checks pass. Logs use the
  `slice34-` prefix in the current matrix evidence directory.
- Final SHA-256 verification was run after the audit-record and gate updates
  and passed with zero mismatches (`slice34-sha256-check-final.log`). Independent
  review of all 69 semantic rows and the complete documentation/resource
  scope remains pending; G8 criterion 5 remains unchecked.

## Independent Slice31–34 review result

- `/root/slice176_final_review` completed a row-by-row semantic review of all
  69 plugin articles against metadata, implementation behavior, and
  platform/SDK limitations. All 69 rows passed with no plugin-article
  findings; their `reviewStatus` values are now `PASS` in the semantic
  crosswalk.
- QRC/runtime discovery, exact isolated three-document listing, source
  attribution, DefectDetection availability semantics, Slice32 date marker,
  reconstructed one-line comparison, and current evidence SHA manifest were
  independently confirmed.
- One remaining medium finding blocks the synchronization sub-item: the
  shell-capability test still uses finite name/payload term sets and could miss
  a newly named tool such as `execute_python(code)`. The current tool catalog
  contains no arbitrary command/script runner; this is a future-regression
  coverage gap. Slice35 is contracted to close it. G8 criterion 5 remains
  unchecked until Slice35 implementation review passes.

## Slice35 implementation awaiting independent review

- `/root/slice176_final_review` approved the Slice35 contract after its
  detector rules and selector exception were made explicit.
- The test normalizes tool/parameter names and scans all catalog names,
  descriptions, and parameter fields. It includes synthetic negative cases
  for `execute_python(code)`, `python_exec(script)`, and
  `run_command(command)` even when those definitions are not registered.
  The actual `select_log_panel_tab` definition remains allowed; copies with a
  `script` payload or `launch process` description are rejected.
- QRC-consuming focused targets rebuilt successfully; focused CTest passed
  3/3; matrix/inventory checks and auditor tests passed (19/19);
  `clang-format` and `git diff --check` passed. Logs use the `slice35-`
  prefix. The remaining detector gap was addressed in Slice36 and independently
  reviewed PASS.

## Slice36 implementation and final independent review

- `/root/slice176_final_review` approved the Slice36 contract after replacing
  the ambiguous `evaluate_value` sample with `value_summary`, which has no
  execution verb/object in its name. Separate synthetic cases put the verb in
  the tool description and parameter description.
- The detector now scans normalized name and all description surfaces for
  execution verbs when executable payload parameters are present. The
  `value_summary(code)` cases are both rejected. Existing three executable
  tool-name cases remain rejected; the original terminal-tab selector is
  allowed, while its payload and process-launch variants are rejected.
- QRC-consuming target rebuild succeeded; focused CTest passed 3/3;
  matrix/inventory checks and auditor tests passed 19/19; formatting and
  `git diff --check` passed. Logs use the `slice36-` prefix. Final SHA-256
  manifest verification passed for 151 entries after the full regression
  archive update; log: `slice36-final-archive-sha256.log`.
  `/root/slice176_final_review` returned
  **IMPLEMENTATION_PASS**, confirming the description-only detector,
  all synthetic cases, the UI-selector exception, and the earlier 69-row
  semantic review.

## Final disposition

G8 criterion 5's documentation and runtime-resource synchronization sub-item
is **PASS** and is checked in `docs/agent-capability-improvement-plan.md`.
The user manual, plugin articles, embedded Agent knowledge, and ToolSchema
were independently reviewed for this scope; audit evidence remains excluded
from runtime knowledge. This does not pass the other G8 criteria or the full
G8 stage: current matrix overall remains `NOT_READY`, field acceptance remains
0/69, and real providers, hardware, SDKs, models, and platform checks remain
separate gates.

## Full software regression after Slice36

- `cmake --build build-codex-agent -j2` completed successfully (all targets
  built, exit 0); `ctest --test-dir build-codex-agent --output-on-failure -j2`
  passed 113/113, with 0 failed and 0 skipped. Logs are
  `slice36-full-build.log` and `slice36-full-ctest.log`.
- The current acceptance matrix was regenerated from the new full CTest log;
  its source SHA matches that log and overall status remains `NOT_READY`.
- Matrix and inventory checks were rerun against the new log, the 19 auditor
  tests passed, and format/diff checks passed. Exact commands and exit codes
  are recorded in `slice36-full-regression-summary.txt`.
- The final refreshed 151-entry manifest passes. `/root/slice176_final_review`
  independently confirmed the full build, 113/113 CTest result, current
  matrix log/SHA reference, formatting and diff checks. G8 criterion 3 is
  checked for this Linux/Qt 5.15.3 environment; cross-platform and field gates
  remain separate.

## Slice37 camera GUI operation boundary

- New `testCameraSetViewDeleteAndApplyRemainGuiOnly` uses the fake async
  camera plugin and triggers the real GUI buttons. Delete removes the visible
  row but leaves the device in `CameraManager`; Apply accepts the dialog while
  leaving the device known and making no connect, disconnect, acquisition, or
  parameter-set call.
- This evidence describes only the current GUI handler behavior. It is not an
  Agent capability, persistent configuration operation, or real camera test.
  `CAM_DELETE_CONFIG` and `CAM_APPLY_PARAMS` remain `NOT_LINKED`/unsupported;
  `TERMINAL_INPUT` remains human-only and no shell was run.
- Full build succeeded and full CTest passed 113/113; focused
  `test_cameramanager` passed 1/1. The current acceptance matrix now references
  `slice37-full-ctest.log` and remains `NOT_READY`.
- Candidate inventory was refreshed against its existing reviewed fixture
  log after the added QtTest case changed the source candidate index from
  1421 to 1422. Business/critical fixture outcomes did not change: 66
  business PASS, one DefectDetection business FAIL, 67 critical-failure PASS,
  two platform/SDK-limited camera entries NOT_AUDITED. This GUI test is not
  counted as an Agent plugin fixture.
- Matrix/inventory checks and auditor tests passed; format, diff, and refreshed
  SHA verification pass. Slice37's initial state-only assertion finding was
  closed in Slice38; the reviewer independently approved the combined
  Slice37/38 fake-camera GUI evidence. No plugin fixture or field acceptance
  count is advanced.

## Slice38 camera state snapshot closure

- The Slice37 GUI handler test now captures a complete `CameraStatus` before
  clicking and compares `deviceId`, `name`, `state`, `lastGrabTime`, and
  `lastError` after both Delete and Apply. Both checkpoints assert
  `CameraManager::findPlugin(plugin.pluginId())` still returns the same fake
  plugin.
- Focused camera manager test passes 1/1; full build succeeds and CTest passes
  113/113 with zero failures/skips. The current matrix references the latest
  full CTest log and remains `NOT_READY`. The fixture audit statuses and
  `CAM_DELETE_CONFIG`/`CAM_APPLY_PARAMS` `NOT_LINKED` mappings are unchanged.
- Candidate inventory remains at 1422 unique QtTest names; structural
  comparison reports zero fixture status changes. Matrix/inventory checks,
  19 auditor tests, format/diff checks and SHA verification pass.
  `/root/slice176_final_review` returned **IMPLEMENTATION_PASS** for Slice38;
  this remains GUI-only fake-device evidence.

## Verification evidence

- Focused build: `cmake --build build-codex-agent --target test_agentphase0_repro test_toolschema test_knowledgebasedialog -j2`, exit 0.
- Focused CTest: `ctest --test-dir build-codex-agent -R '^(test_agentphase0_repro|test_toolschema|test_knowledgebasedialog)$' --output-on-failure -j2`, 3/3 passed in the authorized localhost-listener environment. Logs are `documentation-focused-authorized.log`, `documentation-focused-2.log`, and `documentation-focused-3.log` in the current matrix evidence directory.
- Full build: `cmake --build build-codex-agent -j2`, exit 0; see `build-full-docsync.log`.
- Full CTest: 113/113 passed, 0 failed, 0 skipped; see `ctest-full-docsync-authorized.log`.
- The restricted-sandbox first attempt is retained in `documentation-focused-1.log`; its shell-boundary assertion exposed a nonmatching topic query and an existing localhost listener permission failure. The query was corrected to a unique documented tool name, and all three focused suites passed in the authorized environment.
- `clang-format --dry-run --Werror tests/test_agentphase0_repro.cpp` and `git diff --check` pass.

## Review boundary

Independent review confirmed all 69 article rows against their linked
metadata, implementation behavior, and platform/SDK limits, along with the
three runtime documents, `ToolSchema`, and document discovery behavior. The
PASS applies only to G8 criterion 5 documentation and runtime-resource
synchronization. Real provider, plugin algorithm, Windows, device, model, GPU,
field, and the other G8 criteria remain separate.
