# CP4F attached actor skin-space repair

## State and scope

2026-09-19, branch `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`, uncommitted atop preserved recovery
WIP. No commit, push, CI, game launch or hardware acceptance in this batch.
CP4F remains unaccepted and CP5 blocked. Shared Archive control plus relevant
history through 111 read before changes.

User clarified: donor code is an option, not preferred automatically over a
better original implementation. This repair retains OpenMW's asset behavior as
the compatibility reference but implements an engine-neutral posed-model/skin
contract, with no OSG dependency in RenderCore and no asset-name patches.
OpenGL, Lua threading, mod selection, save format and save files are unchanged.

## Reproduced discrepancy

The translator baked skin cancellation against a donor model's original root.
The actor composer correctly removed that root when copying a filtered rig part
to the actor, but the skin retained the baked cancellation. The dynamic actor
planner then gave every skinned draw identity placement. Canonical RigGeometry
instead computes cancellation from the actual attached path and renders its
geometry-local output under the remaining trishape/node transforms.

A linked-OSG behavioral fixture reproduced final-space vertex disagreement in
all 16 initial scenarios before implementation. It exercises the actual neutral
actor composer for detached donors, not just a hand-written transform formula.
The reference executes RigGeometry::evaluateGeometry on the matching attached
OSG path. No graphics context, OpenGL renderer, or game launch is needed.

## Repair

- SkinPayload optionally retains the original geometry bind transform, before
  donor cancellation. Both legacy NiSkinData and BSSkinInstance producers set it
  (the latter has identity overall transform, as in canonical RigGeometry).
- Geometry-local deformation resolves the current composed node ancestry,
  selects the outermost matching skin-root ancestor, or falls back to the
  geometry's parent when the donor root is absent. It no longer carries a removed
  donor's cancellation into the actor. Singular/nonfinite cancellation fails.
- Dynamic draw placement applies the remaining posed geometry transform.
  Geometry names that collide with bone names do not replace the geometry's
  local transform with a bone pose.
- Bone blending preserves affine w=1 before applying the skin transform, as
  canonical RigGeometry does even when authored weights do not sum to one.
  Empty influence lists remain undeformed, then receive normal draw placement.
- Generic neutral producers without the optional geometry-local field retain
  their existing skeleton-space output/identity draw convention. Their existing
  tests remain passing. No runtime toggle changes game/save semantics.
- Posed model-node transforms are computed once per actor in the runtime planner
  and reused across its parts. Previous-pose evaluation recomputes previous
  ancestry rather than using current-frame cached transforms. No persistent
  mutable global pose cache or additional GPU synchronization was introduced.
- Existing revision-based actor residency invalidation includes mesh skin
  contract updates. Existing mutable draw updates receive both corrected local
  vertices and corrected geometry placement without allocating replacement arrays.

## Exact changed files in this batch

- components/rendercore/records.hpp
- components/rendercore/posedmodel.hpp (new)
- components/rendercore/deformation.hpp
- components/nifrender/niftranslator.cpp
- components/render/backend/vsg/dynamicactorplan.hpp
- components/render/backend/vsg/vsgruntimehost.cpp
- apps/openmw/CMakeLists.txt
- tools/v4/cp4/actor-skin-space-tests.cpp (new)
- tools/v4/cp4/ACTOR-SKIN-SPACE-REPAIR-2026-09-19.md (this record)

Existing changes in these and other files were retained; this is not a clean
single-commit worktree. CPU animation evaluation, attachments, first-person pitch
and prior resource-lifetime fixes remain in the same candidate binary.

## Verification

- Initial characterization: 0/16 canonical/neutral vertex comparisons pass.
- After repair: 16/16 pass; expanded fixture now 32/32 pass.
- Coverage: intact versus copied-out donor hierarchy; present/absent named root;
  identity/nonidentity trishape; rotation and nonuniform scale; mixed bones and
  non-unit weight sums; uninfluenced vertices; bone/geometry name collision;
  two current poses; previous-pose reconstruction; canonical local normals;
  cached/uncached posed-node equality; real VSG mutable-buffer update and matrix
  placement; no stream reallocation; invalid geometry index rejection; skin
  revision invalidates actor plan.
- MSVC RelWithDebInfo production openmw compile/link PASS. Existing dependency,
  conversion and unused-variable warnings remain. An initial test-only unsigned
  conversion warning was corrected.
- All five native CTest suites PASS, eight Python diagnostic tests PASS, seven
  CP4 source contracts PASS, git diff --check PASS (line-ending notices only).
- Executable SHA256:
  `a4d0353acef0a655931d6ea243050e74295a32d0ca0634d17e9d663d95f88600`.
- Executable:
  `C:\Users\LSCha\AppData\Local\Temp\openmw-cp4-local-deps\openmw-build-cp4f-qc\openmw.exe`.

## Remaining acceptance

These fixtures establish a repaired transform contract, not that every reported
detached part was caused exclusively by it. Test the real modded existing save
and New Game, inspect arms/legs/clothing/armor during motion and turns, and retain
screenshots plus diagnostic manifest/log. Existing-save compatibility is required;
New Game alone is insufficient. The material artifacts, local map, performance,
and exterior VRAM/loading failure retain separate acceptance gates. No claim of
all-mod/shader parity or runtime FPS improvement is made here.
