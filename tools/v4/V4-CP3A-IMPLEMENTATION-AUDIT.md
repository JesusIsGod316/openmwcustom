# V4 CP3A implementation audit

Status: implementation started from exact accepted CP2 parent `dbfdbae94c6c96271a2c9e6f57ffec53d31e5406` on `v4.0-cp3a-semantic-publication-core`.

CP3A is the semantic publication foundation for the first real OpenMW Vulkan renderer. It intentionally does not render game assets yet and makes no performance claim.

## Implemented in this head

- Expanded backend-neutral immutable CPU semantic data for mesh surfaces/geometry, textures, material state, sampler/UV semantics, skeleton hierarchy/bind data, instance LOD/attachments, chunks and lights.
- Large mesh/skeleton payloads are immutable shared payloads so publication copies do not duplicate their bulk arrays.
- Material-to-texture and instance dependency lifetime checks now fail closed; attachment cycles and invalid bone references are rejected.
- FrameRenderState now carries frame-local dynamic material/color/alpha/emissive/UV overrides rather than requiring resource revision churn for controller-driven values.
- `RenderWorldUpdateBatch` provides sealed epoch+sequence ordered operation streams. `RenderWorldPublisher` applies a batch all-or-nothing against a shadow world, rejects stale epochs, requires contiguous sequences, and restarts sequence after a destructive world epoch change.
- The shadow-copy implementation is deliberately a CP3A correctness baseline, not a permanent high-volume optimization. Before CP3C producer traffic becomes large, replace it with COW/undo-log publication if profiling or memory accounting justifies it without changing the public batch contract.
- Added the minimal semantic renderer/backend startup selection contract with explicit fallback behavior; no backend object crosses RenderCore.
- Added contract tests for semantic payload validation, dependency lifetime, attachment cycles, frame-local material state, atomic batch publication, sequence/epoch behavior, and backend selection.

## Compatibility requirement

`tools/v4/V4-COMPATIBILITY-CONTRACT.md` is a project-level requirement. V4 is intended to remain a downloadable general OpenMW build with existing OpenMW mods, shaders/post-processing, saves, content, scripts and configuration workflows. A custom settings profile/launcher option/BAT may select backend or compatibility mode, but a private fork-only mod ecosystem is rejected as a release direction. OSG/OpenGL remains the switchable compatibility control until the VSG/Vulkan path reaches parity.

This requirement is also recorded in the structured archive as `evt.project.mod_shader_compatibility_contract.030`; all later CP3+ design decisions inherit it as a primary release constraint.

## Next after this build gate

1. Repair any deterministic compiler/test issue exposed by CI.
2. Audit the resulting CP3A data shapes against the first real NIF translator inputs before freezing names or payload details.
3. Begin CP3B only after CP3A full OpenGL control QC and CP2 Vulkan foundation regression QC are green.
4. Donor code remains optional implementation evidence; adapt it only where it is cleaner than local code.
