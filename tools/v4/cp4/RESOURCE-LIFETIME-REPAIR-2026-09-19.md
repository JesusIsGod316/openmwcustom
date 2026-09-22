# CP4F exterior failure: resource-lifetime repair checkpoint

## Status

2026-09-19. Branch `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. This batch is uncommitted, atop
preserved recovery WIP. No push or CI run. CP4F is not accepted; CP5 is blocked.
Read Shared Archive control first, then current archive through checkpoint 111.
Keep CPU/logical/GPU lifetimes separate, canonical OpenMW animation authority,
8 GB VRAM constraints, ordinary OpenGL control and existing-save compatibility.
No whole-graph eviction, asset edits, save-format changes or Lua-thread disablement.

## Latest manual run, not this new binary

Evidence: `C:\Users\LSCha\Documents\My Games\OpenMW\runtime-qc-evidence\20260919-004112-gameplay-66736`.
User reports animations now play and hatch activation succeeds, but detached
parts/material artifacts persist; exterior loading fails. This was New Game,
not a verification of the existing modded save.

- At 01:37:06.458 the first explicit error is a failed VSG population compilation
  for `meshes/x/ex_common_skywalk_01.nif`: `Context::reserve() failed-2`.
  Vulkan result -2 is `VK_ERROR_OUT_OF_DEVICE_MEMORY` (verified installed header).
- This is caught in delayed `_runStandardActivationAction`; later generic actor
  publication failure obscures the original resource failure. LuaWorker teardown
  follows the first error; it does not establish Lua as the cause.
- No fresh dump in that evidence directory. The global Sept 14 dump is stale.
- 1,212 sampled frames have advancing OSG stamps, camera callbacks and no done
  flag. 4,348 pose and geometry samples show variation. No sampled nonfinite or
  CPU-to-resident stream mismatches. Those comparisons are NOT canonical skinning
  parity or GPU readback.
- Trace stops at 01:18:10 due to the old 36,000-frame cutoff, before the exterior
  transition. An empty findings list was therefore not full-run validation.
- Manifest says original configuration unchanged; original save SHA256 remains
  `d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`.

## Implemented mechanisms

1. Bounded weak decoded-texture cache keyed by full realization key, winning
   source and content identity. Live descriptor owners can share a decoded
   payload; the cache does not retain unused payloads. Changed content, revisions
   and color-space views cannot alias. Null/throwing decodes remain retryable.
   `OPENMW_V4_UNCACHED_TEXTURE_DECODE` selects the uncached causal control when
   present. This is decoded CPU-payload sharing, not proof that every independent
   descriptor/generation shares one GPU image or that the VRAM budget now fits.
2. Prune static VSG SharedObjects at incremental synchronization boundaries and
   after completion-owned retired resources have been destroyed. Release old
   traversal references before post-publication pruning. Live graph/fence-owned
   objects remain referenced. No blanket cache clearing or resource invalidation.
3. Refuse dynamic actor capture when the rendering session/route is already
   unhealthy, preserving its original diagnostic before any further actor state
   mutation. Actor publication failures now identify actor, cell and status.
   This is safe failure handling, NOT a transactional cell rollback/retry.
4. Keep sparse diagnostics beyond 36,000 frames (every 300th, rather than every
   30th before that). The explicit 100,000-record file limit remains. Sampling
   can still miss a failure; it does not guarantee complete trace coverage.

## Exact files in this batch

- components/render/backend/vsg/livetexturecache.hpp (new)
- components/render/backend/vsg/statictexturedecode.cpp
- components/render/backend/vsg/vsgruntimehost.cpp
- apps/openmw/mwrender/v4enginerenderbridge.cpp
- components/debug/gameplaydiagnostics.hpp (already untracked WIP)
- tools/v4/cp4/effect-capture-tests.cpp (already untracked WIP)
- tools/v4/cp4/runtime-blocker-contract.py
- tools/v4/cp4/GAMEPLAY-DIAGNOSTICS.md (already untracked WIP)
- tools/v4/cp4/RESOURCE-LIFETIME-REPAIR-2026-09-19.md (this record)

## Verified checks

- MSVC RelWithDebInfo production `openmw` and all four recovery test targets
  compile/link PASS. Initial cache observer construction and fixture private
  handle construction errors corrected before this result.
- Four CTest suites PASS; effect suite expanded from 39 to 44 cases. Tests cover
  live/expired decode reuse, changed content and color space, stale revision,
  bounded weak eviction, failed-decode retry, uncached control, linked VSG prune
  ownership, and late diagnostic sampling. Existing update-only four cases pass.
- Eight Python diagnostic fixtures PASS; seven CP4 source contracts PASS.
  Added source ordering guard for failed-session rejection before actor mutation;
  this is not a runtime fault-injection test.
- `git diff --check` PASS, line-ending notices only.
- New production executable SHA256:
  `5b82dba39f1e19ba83b77aca2078ae61f4f0338987b642d037bc656383faa42b`.
- Binary: `C:\Users\LSCha\AppData\Local\Temp\openmw-cp4-local-deps\openmw-build-cp4f-qc\openmw.exe`.
- No manual launch of this binary yet. No measured memory/FPS improvement claimed.

## Separate actor-space investigation: not repaired in this batch

Canonical `RigGeometry::updateSkinToSkelMatrix` finds the skin-root ancestor in
the actual attached OSG path. If removed by CopyRig attachment it cancels up to,
but not including, the trishape transform. Its evaluated vertex buffer remains
subject to drawable node-path transforms during rendering.

Neutral `niftranslator.cpp::resolveSkinSpaces` bakes cancellation from the donor
model hierarchy. `actormodelcomposer.hpp` copies the selected rig subtree without
donor skeleton ancestors, but keeps the original mesh/skin payload. Neutral
`deformMesh` uses that baked cancellation; dynamic actor planning unconditionally
assigns identity draw placement to skinned geometry. This is a concrete contract
discrepancy for nontrivial attachment transforms, not yet proof of the precise
cause for every detached part in the user's screenshot. No speculative matrix
order change was applied.

Next repair must compare canonical and neutral final-space vertices under the
same current pose and actual composed hierarchy. Include nonidentity trishape,
named skin root, removed donor root, animated ancestor, rotation/nonuniform scale,
split body parts and multi-bone weights; preserve generic neutral producer tests.
Only then change the skin-space contract/attachment publication and its resource
identity dependencies together. Existing CPU-to-resident equality cannot detect
both consumers agreeing on incorrect geometry.

## Runtime acceptance still required

After coherent actor-space repair, use the manual diagnostics harness with normal
content and existing save visibility. Verify the original modded save as well as
New Game, animation/equipment placement, changing focus and activation, texture
semantics, map, and interior/exterior transitions. Inspect memory growth across
repeated transitions; compare diagnostics-off performance separately. Resource
repair alone does not establish all-mod/shader compatibility or eliminate OOM.
