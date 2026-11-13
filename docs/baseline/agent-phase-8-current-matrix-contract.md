# G8 current acceptance matrix contract

> 历史矩阵契约；当前矩阵/发布状态见 [`docs/STATUS.md`](../STATUS.md)。

## Goal

Replace the candidate-only presentation with one deterministic, reviewable
snapshot that separates current plugin fixture evidence, GUI-operation evidence,
historical baseline differences, and external field acceptance. The artifact
must keep failures and missing environment evidence visible; it must not infer a
G8 pass from source-name matches, an old capability matrix, or successful test
suite names alone.

## Inputs and outputs

- Inputs: the 69-plugin historical capability JSON (identity and historical
  baseline only), the current independently reviewed fixture audits, the
  candidate inventory and its reviewed fixture CTest log, a latest complete
  CTest log for the current source tree, and the 126-operation GUI operation
  list.
- Current binary presence is rechecked from the configured build directory and
  reconciled against inventory entries. The old 66-binary baseline remains an
  explicit historical value; the current 67-binary snapshot is not written
  back into that historical source.
- The authoritative generated output is
  `docs/baseline/agent-g8-current-acceptance-matrix.json`; the Markdown view is
  generated from that JSON by the same script.
- External field acceptance remains `NOT_RUN` unless separately supplied with
  independently reviewed evidence. This local matrix build must not change it.

## Required classifications

- Every source plugin appears exactly once, including platform/SDK-blocked
  camera plugins and DefectDetection's business-fixture failure.
- Every GUI operation appears exactly once and keeps its stable operation ID,
  owner API, mapping, phase, and notes. An operation is `EVIDENCE_LINKED` only
  when its test source is registered in `tests/CMakeLists.txt`, the test ID
  exists in that source, and its CTest suite passed in the latest full CTest
  log. This classification does not claim the full operation passed. Missing
  evidence is explicit, while malformed/stale supplied references are rejected.
- Plugin fixture status is kept separate for business and critical-failure
  fixtures. Missing fixtures are `NOT_AUDITED`, failed business behavior remains
  `FAIL`, and platform/SDK absence remains `BLOCKED`.
- The matrix reports source, binary, fixture, operation, and field counts, plus
  the exact difference between the historical plugin/binary baseline and the
  current configured workspace.

## Validation gates

1. Reject duplicate/missing plugin or operation identities and stale references.
2. Reject a malformed/incomplete CTest summary or a fixture audit pointing at a
   test case not present in its source or not represented by the audited CTest
   result.
3. Verify the generated Markdown is byte-for-byte reproducible from the JSON
   snapshot and that `--check` detects stale outputs.
4. Unit-test fail-closed behavior for inventory drift, missing fixture evidence,
   unregistered operation sources, and invalid test references.
5. Independently review the generator, schema, status derivation, generated
   counts, and historical/current delta before recording this slice complete.

## Exclusions

This is an evidence index, not new plugin behavior and not an overall G8 pass.
It does not re-run the full build/CTest matrix, enable unavailable SDK code,
validate Windows, or provide real camera/PLC/SAM/provider/GPU/field acceptance.
