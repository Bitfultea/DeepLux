# G8 documentation synchronization audit contract

## Purpose

Audit G8 acceptance criterion 5 in `docs/agent-capability-improvement-plan.md`:
the user manual, plugin manual, embedded Agent knowledge, and runtime tool
schema must describe the same supported behavior, and audit artifacts must
not be loaded as runtime instructions. This audit may recommend PASS only if
all checks below pass; otherwise it records each concrete mismatch and leaves
the plan criterion unchecked.

## Frozen scope

Read-only audit of:

- `docs/user-manual.md`
- `docs/plugins.md`
- `src/core/agent/knowledge/DEEPLUX.md`
- `src/core/agent/agent_resources.qrc`
- `src/core/agent/ToolSchema.cpp`
- `src/core/agent/AgentActor.cpp` documentation source discovery and retrieval
- one focused runtime assertion in `tests/test_agentphase0_repro.cpp`
- the current DefectDetection build guard, metadata, and error behavior
- runtime documentation coverage in `test_toolschema`,
  `test_knowledgebasedialog`, and `test_agentphase0_repro`

No product behavior, manual prose, runtime resource list, or tool schema is
changed by the audit. The only permitted code change is a test assertion that
locks the documented source and shell boundary. If the audit finds a product
or documentation mismatch, repair it in a separate reviewed slice.

## Acceptance checks

1. Discover all 69 plugin `metadata.json` files under `src/plugins`. Every
   metadata file must be linked exactly once by `docs/plugins.md`; there must
   be no duplicate, missing, or extra plugin metadata links. Independently
   review every plugin article block against its linked metadata and the
   cited implementation claims; the live runtime module schema remains the
   authority for exact parameters.
2. `agent_resources.qrc` must bundle exactly the Agent knowledge overview,
   user manual, and plugin manual. Inspect `AgentActor` runtime document
   discovery as well as the QRC. `list_knowledge_documents` in an isolated
   app-data directory must enumerate those three built-ins and no baseline,
   phase acceptance, evidence, or audit documents.
3. The Agent knowledge overview and user manual must state that exact
   instance parameters come from `get_module_params_schema`, documentation
   retrieval carries source attribution, and arbitrary shell use is not an
   Agent capability. The `read_documentation` runtime schema and
   `AgentActor::readDocumentation` behavior must agree. The focused test must
   query unique content from USER_MANUAL.md and DefectDetection content from
   all three built-ins, asserting each `[来源: filename]` attribution. It must
   assert the registered tool catalog has no shell execution/input tool.
4. The user manual, plugin manual, and embedded knowledge must all accurately
   disclose that DefectDetection is unavailable in the current SDK-disabled
   build, state the current failure rather than implying successful results,
   and distinguish declared ports from produced values. Verify both
   `ENABLE_HYMSON3D=OFF` and the additional `HymsonVision3D_FOUND AND FALSE`
   CMake guard against implementation and test evidence.
5. Run the exact registered CTest suites `test_toolschema`,
   `test_knowledgebasedialog`, and `test_agentphase0_repro`; the current full
   CTest baseline must also contain all three as passing suites.
6. Any mismatch, missing evidence, or claim exceeding its source/test evidence
   is a FAIL; do not edit documents during this audit to force a pass.

## Required evidence

Record the metadata-link set comparison, QRC resource list, source locations
for the synchronized claims, exact CTest command/results, and independent
review conclusion. A PASS applies only to criterion 5's documentation and
runtime-resource synchronization scope; it does not satisfy other G8 gates or
prove real provider, SDK, device, model, or field behavior.
