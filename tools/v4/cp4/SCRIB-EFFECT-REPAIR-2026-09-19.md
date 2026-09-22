# CP4F scrib and effect-residency repair

## State and scope

Branch `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Uncommitted changes atop preserved
recovery WIP; no commit, push, CI dispatch or manual game launch in this repair.
Cached Shared Archive control/decision locks and relevant CP4F history through
111 were consulted, followed by the current local repair records and actual
source. This is not a fresh remote archive snapshot.

Production executable:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc/openmw.exe`

SHA256: `4604ba2a88b7f8faa54376ddfda2a3904b0c8d342a3306335a903e9a1fee795e`.
MSVC 14.44.35207 RelWithDebInfo application compile/link passed. Existing compiler
warnings remain. CP4F runtime/performance acceptance is still blocked.

## Trigger and measured evidence

Manual run `20260919-182233-gameplay-77792`, executable `c8226382...`, exited 1.
User confirmed the left shoulder repair but reported worse performance and a
new exterior crash. First frame failure at 18:25:49 names `meshes/r/xminescrib.nif`
and rejects the absence of a currently drawable skinned/morphed mesh. The much
later fatal-dialog dismissal/process exit must not be counted as load duration.

The independently timed exterior transition was 95.621 seconds, including
34.184 seconds in terrain preparation and 50.820 seconds in central Seyda Neen
render/physics insertion. These scopes are inclusive and nested. They are not
GPU timings or a controlled performance benchmark.

The earlier run `20260919-171639-gameplay-62516` reused 672 effects with zero
rebuilds in late gameplay samples. The new run repeatedly rebuilt about
100 effects while reusing others (one late sample rebuilt 285). Actor residents
continued to reuse all four actors. This localizes resource churn to immediate
effects, not actor reconstruction. Sampled animation/particle update increased
by less than one millisecond; the large measured increase was in dynamic
realization/compile. Different routes/timings and sparse sampling prevent a
formal cross-run benchmark claim.

## Repairs

1. **Rigid animated actors:** dynamic actor planning no longer requires a skin
   or morph stream. Valid rigid geometry under animated bones uses the existing
   evaluated model-node transform path. Empty assets, missing dependencies,
   unsupported controllers, and failed pose/deformation remain errors. No model
   filename special cases, actor skipping, save conversion or skeleton invention
   beyond the existing canonical forced-skeleton route was added.
2. **Mutable effect bounds:** bounds changes no longer invalidate immutable
   effect layout. After validating every destination stream, in-place updates
   refresh DepthSorted bounds from current vertices. Ordinary geometry uses its
   local AABB sphere; billboards use a pivot-centred sphere matching the actual
   conformance realizer. Index/topology, material, texture interpretation/sampler,
   semantic flags, billboard mode, tangents, and stream layout still invalidate
   reuse. Fence-completed resource acquisition remains mandatory and unchanged.
   `OPENMW_V4_IMMUTABLE_EFFECT_BOUNDS=1` restores the old bounds-triggered rebuild
   predicate for same-executable attribution. It does not freeze particles.
3. **Loading attribution:** the existing per-cell insertion accumulator now
   records canonical rendering, physics, mechanics, looping effects, Lua scene
   insertion, navigation and V4 publication separately when gameplay diagnostics
   are enabled. One aggregate event per cell avoids per-reference log flooding.
   The regular diagnostic report includes these stage totals and retains cell
   identities in JSON. Repeated effect rebuilds are automatically flagged for
   investigation, not mislabeled as a proven failure or GPU bottleneck.

The 95-second exterior-loading problem is NOT claimed repaired. The retained
current-grid terrain preload still supports canonical paged-reference accounting;
it was not removed speculatively. The added insertion breakdown accompanies the
substantive crash/resource repair, rather than being a telemetry-only build.

## Tests and limits

- 13/13 CTest native/package cases passed after rebuilding affected binaries.
- 7/7 CP4 source contracts passed; 15/15 Python diagnostic tests passed.
- Synthetic all-rigid actor fixture verifies evaluated transforms for two parts
  (5 and 7 units); immutable-dependency/unsupported-controller guards remain.
- Actual winning scrib mesh identified through normal configured data-root order:
  `C:/OpenMWMods/i-heart-vanilla-directors-cut/TexturePacks/VFCR - Meshes-55093-1-0-1723146646/meshes/r/xminescrib.nif`.
  SHA256 `e737e1e2b8376c17c0a5a0fbc3d147a0aea38a89247e613ce8e50642403f48be`.
  Full static/material translation, publication, forced skeleton, dynamic actor
  planning and pose evaluation retain all **43 rigid draws**.
- Kwama regression: real installed model, full 30-second/902-frame canonical
  timeline, 902 body captures and 771 particle captures, passed.
- 120 particle size/bounds updates preserve the same vertex-array object and
  update sorting bounds. Old bounds-control predicate still rejects resizing.
- Actual VSG conformance graphs, ordinary and billboard, verify updated sort
  bounds equal fresh realization; existing material/index/UV/sampler/semantic
  invalidation and transactional-update tests still pass.
- Integration uses the winning loose mesh directory plus the base Morrowind BSA,
  not the entire modded runtime. It does not establish GPU pixels, actual creature
  animation visual parity, saved-game acceptance, exterior completion or FPS.
- During development, MSVC caught fixture namespace/vector-constructor and
  explicit string conversion errors. Real-model testing caught use of structural
  translation without material publication; it now uses production static
  translation. The fresh-graph sorting fixture needed explicit Sorted policy.
  These were corrected before the final passing results, not waived.

Build-directory evidence: `scrib-repair-build.log`, `scrib-test-build.log`,
`scrib-regression-build.log`, `scrib-repair-ctest.log`,
`integration-xminescrib.nif.log`, `integration-xkwama forager.nif.log`.

## Supplied native development build

Read ZIP directory and `resources/version` directly from
`C:/Users/LSCha/Downloads/OpenMW_MSVC2022_64_RelWithDebInfo_master.zip`.
It identifies OpenMW 0.52.0, commit
`6fb84ff6319779a6021e3d454c234898b01a9c53`. It contains native executable,
dependencies, Lua API documentation and animation resources. It was not
installed, launched, mixed with custom DLLs, or treated as proof of Vulkan
feature parity. No upstream feature merge is claimed.

## Preservation and next test

All five normal config/storage hashes match the pre-run manifest. Protected
`_zZz__Wake_up.omwsave` hash remains
`d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`.
Shoulder/skin-space code, mod order, saves, PBR/shader setup and threaded Lua are
unchanged. PBR material interpretation and broad CP6 optimization remain open.

Use the existing manual diagnostic runner with the new executable, record its
hash and repeat the same ship-to-Seyda Neen route. Check continuing effect churn,
the new insertion breakdown, actual exterior rendering and any next explicit
failure. Do not promote on compile/test success alone.

## Exact files changed this turn

- `components/render/backend/vsg/dynamicactorplan.hpp`
- `components/render/backend/vsg/immediateeffectrealizer.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `apps/openmw/mwworld/scene.cpp`
- `tools/v4/cp3d/dynamic-actor-plan-smoke.cpp`
- `tools/v4/cp4/effect-capture-tests.cpp`
- `tools/v4/cp4/gameplay-diagnostics.py`
- `tools/v4/cp4/test-gameplay-diagnostics.py`
- This repair record.
