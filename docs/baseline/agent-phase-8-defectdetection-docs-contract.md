# G8 DefectDetection documentation sync contract

## Scope

Keep the operation manual, plugin manual, bundled Agent knowledge, plugin metadata, and runtime SDK-disabled behavior consistent for `DefectDetection`.

## Frozen requirements

1. All three user/Agent-facing documents state that the HymsonVision3D algorithm is currently unavailable, including the `ENABLE_HYMSON3D=OFF` demo build and the plugin CMake `AND FALSE` guard.
2. They give the runtime failure `HymsonVision3D SDK 未启用，缺陷检测不可用` and do not claim a successful algorithm result.
3. They name the declared success outputs and distinguish schema-visible ports from queryable result values; the current SDK-disabled build exposes the ports but produces no values.
4. `read_documentation(topic="DefectDetection")` retrieves all three sources and the same availability boundary.
5. The business fixture remains failed/unavailable until a valid HymsonVision3D-backed algorithm fixture proves useful output; documentation sync cannot change its matrix status.

## Verification

- Test: `test_agentactor::testReadDocumentationSearchesBuiltInAndUserKnowledge`.
- Required assertions: all three source labels, exact runtime error, `AND FALSE`, and `defect_point_cloud` appear in the returned content.
- Independent review checks consistency against CMake, plugin implementation, metadata, and the current G8 fixture audit.

## Out of scope

This slice does not enable or emulate the HymsonVision3D algorithm, mark its business fixture PASS, or complete G8.
