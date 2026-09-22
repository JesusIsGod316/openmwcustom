# CP4F recovery regression checkpoint (2026-09-18)

## Scope of this batch

- Evaluated material capture now uses canonical OpenMW type-attribute/texture-name precedence, recognizes normal-height textures as normal/data rather than diffuse/sRGB, and diagnoses unknown secondary texture semantics. This fixes classification; it does not claim full normal-height parallax support or screenshot acceptance.
- Immediate-effect reuse now compares immutable material/texture state, indices, non-updated tangent streams, bounds and visibility. A rejected mutable-stream update validates all destinations before changing any arrays.
- The asset runner preserves atomic, explicitly incomplete progress reports and treats launch failures/timeouts as asset results. Independent tests can continue. This is not yet a full-scene capture/replay tool.
- Immediate effects now use fence-completed mutable resource versions, bounded to the pinned three-slot VSG submission ring per identity. Disappeared identities are collected after final GPU use. CPU frame advancement alone never permits overwriting an in-flight version.
- Only changed position/normal/color/UV streams are dirtied for upload. Placement-only and unchanged updates do not upload identical streams. Reuse no longer recopies the entire immutable mesh/material contract.
- OpenGL behavior, normal user configuration/saves, Lua threading, and previous uncommitted residency work were preserved. Broader residency work remains experimental: actor reconstruction, per-frame VFS hashing, other resource families and real GPU budget/retirement behavior still require work.

## Tests and results

`effect-capture-tests.cpp` calls the production capture/reuse/update functions,
using an in-memory VFS and CPU-side OSG/VSG arrays. No game window or Vulkan
device is required. Checks remain active with `NDEBUG`.

- Before repair: four material-capture cases failed, seven stale-reuse cases failed, and one rejected-update case demonstrated partial mutation.
- After repair: all 15 behavioral cases passed. MSVC RelWithDebInfo compile/link passed with `/W4 /WX`, without warnings, using the existing release library inputs.
- All eight retained application/CP4 source contracts passed. These are supplementary checks, not runtime proof.
- `test-corpus-recovery.py` covers timeout byte decoding, launch failure isolation, actual subprocess exit/timeout handling, saved partial results, continued later cases and interruption without false success.
- `git diff --check` passed.

The original focused executable was compiled/linked directly with the previously
generated release build inputs and run with the existing staged runtime DLLs,
because incomplete dependency installations blocked CMake configuration.

Dependency repair on 2026-09-18 resolved that blocker:

- Restored only missing files from the project's pinned OpenMW dependency archive
  `vcpkg-x64-windows-2022-2026-02-24.7z`, after verifying its published SHA512.
  The existing release Boost Random library also matched the archive's SHA256.
  This restored the missing Boost debug libraries and other absent package files.
- Restored missing files in the separate `vsg-installed/x64-windows` prefix from
  the local vcpkg binary archives matching each installed package's recorded ABI.
  This addressed the subsequently exposed GLM/VSG/glslang package omissions.
- Existing dependency files were preserved; no release library was substituted
  for a debug library and no CMake missing-file checks were bypassed.
- Normal CMake configure and generation now pass with
  `OPENMW_V4_BUILD_RECOVERY_TESTS=ON` in `openmw-build-cp4f-qc`.
  Upstream GLM CMake deprecation warnings remain nonfatal.

The dependency-only repair initially certified configuration/generation. The
subsequent normal CMake build and test verification is recorded below; it does
not certify a packaged runtime or in-game result.

## Follow-up: effect lifetime and upload repair

The source audit found that the existing effect cache had no removal sweep and
updated resident arrays/placement without checking their final GPU use. Holding
an old root in a retirement queue prevents destruction but does not prevent its
shared arrays from being overwritten. A bounded version pool now guards mutation
using the host's existing fence-backed completion watermark. Submission tags are
assigned after successful queue submission; absent, completed versions are
removed after publication. Failed, unsubmitted preparation does not pin a version.
No new routine GPU wait or device-wide idle was introduced.

Normal CMake/CTest now runs 25 passing behavioral cases (the original 15 plus 10
lifetime/upload cases). These include separate 500-frame CPU simulations for
steady reuse and spawn/despawn, bounded capacity without completion, delayed
retirement, retained graph ownership, duplicate identities, failed preparation,
and actual VSG ModifiedCount checks for unchanged and independently changed
streams. These simulations prove pool/update behavior, not measured VRAM or FPS.
The five Python diagnostic-runner tests and all eight retained source contracts
also pass. The Python suite intentionally emits fixture failure/interruption
messages while testing recovery; its overall result is OK.

The recovery test target was compiled/linked through the repaired MSVC
RelWithDebInfo CMake build and passed via CTest. An incremental production
`openmw` link also passed after the pool integration and after the final
upload-change repair. Existing warnings in settings,
generated engine code, coordinator shadowing, and yaml-cpp remain; this is not a
warning-free full build claim. No game, GPU validation-layer run, fresh package,
or GitHub CI was launched.

Next: remove recurring VFS hashing with explicit invalidation; address evaluated
RigGeometry/MorphGeometry capture and actor resource/transform equivalence; then
map output and threaded Lua acceptance. These are not fixed by the effect pool.

Final incremental local build: SUCCESS, including both `openmw` and
`openmw-v4-effect-capture-tests` targets. Binary SHA256 values (dirty source tree,
not a release identified solely by the unchanged Git HEAD):

- `openmw.exe`: `643c09ad20fbb07b57e83842ada057c03358951f8f25d8324cb03cfff175828b`
- `openmw-v4-effect-capture-tests.exe`: `462e4e945847fc4f64ad184af53c1b03f2daf5536aec3a837413e8793cec0318`

Files changed specifically for this lifetime/upload follow-up:

- `components/render/backend/vsg/frameresourcepool.hpp`
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/immediateeffectrealizer.hpp`
- `tools/v4/cp4/effect-capture-tests.cpp`
- `tools/v4/cp4/runtime-blocker-contract.py`
- `tools/v4/cp4/RECOVERY-TESTS.md`

Archive checkpoint 111 and the legacy control block were re-read before this
repair. The design follows their separate logical/GPU lifetime and bounded
residency constraints; it does not revive whole-world eviction or introduce
silent content omission. This is a local checkpoint; Drive was read, not edited.

## Normal build integration

With a complete dependency installation, configure the OpenMW build with
`OPENMW_ENABLE_V4_VULKAN_RUNTIME=ON` and
`OPENMW_V4_BUILD_RECOVERY_TESTS=ON`. Build target
`openmw-v4-effect-capture-tests`, then run the matching CTest test with the normal
runtime DLL search path. The option defaults off and does not affect ordinary
OpenGL builds. CI has not been launched or its frozen checkpoint gate changed.

Portable runner checks:

```text
python tools/v4/cp3b4/test-corpus-recovery.py
```

## Files changed by this batch

- `CMakeLists.txt`
- `apps/openmw/CMakeLists.txt`
- `apps/openmw/mwrender/v4effectcapture.hpp`
- `components/rendercore/records.hpp`
- `components/render/backend/vsg/immediateeffectrealizer.hpp` (extends pre-existing WIP)
- `components/render/backend/vsg/frameresourcepool.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp` (extends pre-existing WIP)
- `components/render/backend/vsg/vsgruntimehost.hpp` (extends pre-existing WIP)
- `tools/v4/cp4/runtime-blocker-contract.py` (extends pre-existing WIP)
- `tools/v4/cp4/effect-capture-tests.cpp`
- `tools/v4/cp3b4/corpus-runner.py`
- `tools/v4/cp3b4/test-corpus-recovery.py`
- `tools/v4/cp3b4/README.md`
- `tools/v4/cp4/RECOVERY-TESTS.md`

Local HEAD remains `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. No new commit,
push, full CI build, gameplay launch, or runtime promotion was made in this
batch. No claim that the visible purple objects, actor placement, black map,
multi-second frame times, or Lua acceptance are resolved yet.

## Follow-up: texture identities, evaluated geometry, and actor residents (2026-09-18)

### Implemented in this pass

- Added capture-context-owned, bounded LRU texture identity caching (4096 entries).
  Repeated capture keeps the winning-VFS content hash rather than reopening and
  hashing texture bytes every frame. A VFS index generation invalidates cache
  entries across reset/rebuild, including same-name/same-timestamp winner changes.
  Last-modified checks invalidate loose-file edits; failures and missing files
  are not cached. This still performs timestamp/path checks, not zero filesystem
  work. Timestamp-preserving external edits require explicit cache clear or VFS
  rebuild. It is render/capture-thread owned, not a cross-thread global cache.
- Refactored RigGeometry and MorphGeometry CPU evaluation into supported shared
  entry points used by both canonical OpenGL cull and V4 capture. V4 copies the
  evaluated arrays immediately; no fake CullVisitor, second animation system,
  or immutable-template mutation. Capture now handles these drawable types.
  First traversal zero is no longer mistaken for an already evaluated morph.
  Invalid active morph topology gets a diagnostic before any target copy.
- Active actor placement now uses the live OpenMW root's position, attitude, and
  nonuniform scale instead of only reconstructing placement from saved reference
  fields. Unloaded/missing-root publication retains its existing reference fallback.
- Added fence-completed actor resident versions using the same bounded pool as
  effects. Unchanged dependency revisions and opacity permit vertex/normal and
  placement updates without recreating all actor buffers and state. Unchanged
  arrays are not dirtied. Dependency/opacity changes rebuild a writable version.
  World epoch plus handle slot/generation prevents cross-world/actor aliasing.
  Disappeared identities retire only after completion.
- Sorted bounds use the evaluated payload on initial realization and track
  deformation plus draw placement during reuse. Conformance routing records the
  actual published sort wrapper, not the discarded raw wrapper. Billboard draws
  retain the rebuild path because their view-dependent transform contract differs.
- Added same-build control switches. Presence of
  OPENMW_V4_UNCACHED_TEXTURE_IDENTITIES disables texture identity storage;
  presence of OPENMW_V4_REBUILD_ACTORS disables actor reuse. Unset the variable
  to re-enable (setting its value to 0 still counts as present). These controls
  do not change user settings or select OpenGL. Normal OpenGL selection remains
  available and uses the same canonical skin/morph evaluator.

### Validation

- Local MSVC RelWithDebInfo production openmw.exe compiled and linked successfully
  with OPENMW_ENABLE_V4_VULKAN_RUNTIME=ON and OPENMW_V4_BUILD_RECOVERY_TESTS=ON.
- CTest: all 3 registered recovery executables passed:
  openmw-v4-effect-capture-tests (36 named cases),
  openmw-v4-dynamic-actor-plan-tests, and openmw-v4-deformation-tests.
  The older actor-plan assertion checks are explicitly enabled in Release-style
  configurations; tests include stale material revision rejection.
- New behavioral coverage: repeated-hash open counts; same-time winning-VFS
  replacement; timestamp change; explicit clear/reset; zero-capacity control;
  bounded cache eviction; evaluated morph updates/source immutability; malformed
  morph topology; canonical rig attachment cancellation and pose advance;
  unbound rig diagnosis; live root transform parity; actor stream update
  atomicity; unchanged-stream dirty counts; and updated sorted bounds.
- First native run exposed a fixture ambiguity: unnamed rig and attachment were
  both empty strings, invoking canonical trishape-name behavior. The fixture now
  names the attachment and geometry distinctly. No production name-based exception
  was added. Existing exact-signature source contracts were updated for the cache
  parameter and strengthened with evaluator/lifetime checks.
- Python corpus recovery: 5/5 tests passed. The printed broken-asset failure is
  the deliberately failing fixture used to verify continued collection.
- All 8 source contract scripts passed; git diff --check passed.
- Existing C4305/C4244/C4189/C4457 and dependency C4275/deprecation warnings remain.
  /UNDEBUG emits the expected D9025 override warning in the assertion fixtures.
  No claim of a warning-free build.
- Final production SHA256:
  b8e13cbf406657e796b06816b4797d14f784cc51769ffc1ed65bc2ec3292e8aa
- Final effect-capture test executable SHA256:
  d42aa2fde37c189a8219454559e41542a27c75125ec181ea8804df9f3627ea77

### Files changed in this pass

- apps/openmw/CMakeLists.txt
- apps/openmw/mwrender/v4actorplacement.hpp (new)
- apps/openmw/mwrender/v4effectcapture.hpp
- apps/openmw/mwrender/v4enginerenderbridge.cpp
- apps/openmw/mwrender/v4enginerenderbridge.hpp
- apps/openmw/mwrender/v4engineframecoordinator.cpp
- apps/openmw/mwrender/v4semanticsource.cpp
- components/nifrender/textureidentitycache.hpp (new)
- components/vfs/manager.cpp
- components/vfs/manager.hpp
- components/sceneutil/riggeometry.cpp
- components/sceneutil/riggeometry.hpp
- components/sceneutil/morphgeometry.cpp
- components/sceneutil/morphgeometry.hpp
- components/render/backend/vsg/staticassetrealizer.cpp
- components/render/backend/vsg/staticassetrealizer.hpp
- components/render/backend/vsg/staticassetconformance.cpp
- components/render/backend/vsg/vsgruntimehost.cpp
- components/render/backend/vsg/vsgruntimehost.hpp
- tools/v4/cp3d/dynamic-actor-plan-smoke.cpp
- tools/v4/cp4/effect-capture-tests.cpp
- tools/v4/cp4/animated-object-contract.py
- tools/v4/cp4/runtime-blocker-contract.py
- tools/v4/cp4/RECOVERY-TESTS.md

### Still unverified / next acceptance

No game launch, packaged deployment, GPU validation run, CI dispatch, commit, or
push in this pass. HEAD remains f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b on
v4.0-cp4f-exterior-closeout; repairs remain local uncommitted WIP alongside the
preserved earlier work.

The archive refresh call failed to return usable content this pass. Work used
the previously reviewed AI_CONTEXT_CONTROL_BLOCK/checkpoint 111 and the local
recovery plan; this note is local and does not claim a Drive archive update.

Actor plan construction and CPU deformation still run each frame. This repair
reuses compatible GPU objects, not the entire CPU planning result. Full actor
composition/skin-space parity for the user's oversized character remains
unverified; no speculative skeleton matrix-order changes were made. Billboard
reuse remains deferred. Texture classification, missing world content, map
output, video/Escape transitions, same-build Lua worker acceptance, frame times,
and 8 GB GPU budget/retirement still need runtime evidence.

Next useful test: one controlled launch of the user's save with effective
renderer/settings and executable hash recorded, checking bed/materials, character
shape/placement, movement, and map output, followed by a bounded repeated-frame
resource test. Compare each control switch independently on the same executable
if performance or lifetime differs. Passing these source/native tests is not CP4F
runtime acceptance and does not authorize CP5 promotion.
