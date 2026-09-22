# CP4F exterior loading and gameplay runtime repair

## Scope and provenance

User priority: repair pathological exterior loading and gameplay runtime now;
defer broader rendering optimization/features to CP6. Preserve normal mods,
threaded Lua, settings, save format and existing saves. No lower-quality preset,
actor omission, OpenGL fallback, adaptive deferral or GPU-eviction workaround.

Read cached authoritative archive AI_CONTEXT_CONTROL_BLOCK and relevant current
history through event 111; reconciled against newer local repair records/source.
Archive snapshot is historical, not current runtime acceptance.
Actual checkout: `C:/Users/LSCha/Documents/ChatGPT/OpenMW custom Build-cp4f`.
Branch `v4.0-cp4f-exterior-closeout`; HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Preserved prior dirty WIP.
This batch is uncommitted. No push, GitHub build or game launch.

Baseline evidence: normal-profile `runtime-qc-evidence/20260919-141905-gameplay-76100`,
exit 1, executable SHA256
`5e771ba828cd3ff222766acb485ab697f4daa1c4918630c63427e3dd2acf3e09`.
The main Seyda Neen cell took 126533.3433 ms; the exterior operation took
188080.1296 ms and returned without exception unwinding. The first exterior
gameplay frame subsequently failed with `active actor has no evaluated OpenMW skeleton`.
33 sampled gameplay frames (exclude menus lacking dynamic_capture) had median
dynamic_capture 36.21 ms, vulkan_present 24.70 ms, dynamic_realization 7.94 ms.
These are inclusive CPU measurements, not additive components or GPU timings.
User screenshots report roughly 13 FPS. Exact offending creature identity was
not included in the old failure; supporting the legal non-skeletal path does not
prove all exterior actors are now compatible.

## Repairs

1. Publication no longer rescans every immutable mesh/skin/morph/skeleton payload
   after every batch. Every incoming create/update is still fully validated by
   RenderWorld before mutation. Candidate world cross-record checks and atomic
   shadow-copy commit remain. `valid()` still performs the full independent audit.
2. A single light create/update/retire uses its validated, allocation-free record
   mutation directly. Light records have no cross-resource handles or owned heap
   data; compile-time no-throw move checks protect this atomic path. Failed
   validation leaves world revision, publisher sequence and record unchanged.
   Multiple-operation batches still use rollback-capable candidate worlds.
   `OPENMW_V4_FULL_WORLD_PUBLICATION=1` restores the old copy/full-scan control.
3. Production static lifecycle and actor model translation now pass owner-local,
   bounded texture identity caches through both legacy and BS shader material
   translation. Shared textures are no longer fully rehashed for every material
   and model. No material interpretation or content-key weakening.
4. Dynamic capture observes each texture's metadata once per frame snapshot,
   not per drawable. A new snapshot checks timestamps again; VFS generation
   change/explicit clear invalidates both levels immediately. Missing resources
   are not cached. Cache scopes do not cross loading callbacks. The environment
   control `OPENMW_V4_UNCACHED_TEXTURE_IDENTITIES=1` forces uncached resolution.
5. Non-skinned CreatureAnimation models may legally have no SceneUtil::Skeleton.
   Their already-evaluated MatrixTransform hierarchy now supplies poses using
   canonical case-insensitive first-match naming/composition, without changing
   or reparenting the OpenMW graph. Skinned actors still require their real
   evaluated skeleton; missing/non-finite rigid transforms fail with identity.
6. Existing opt-in diagnostics now include independent `v4_model_load` operations
   and sampled `static_sync`, to separate residual model loading from gameplay
   static-plan/submission work. These remain bounded by the existing trace cap.

## Verification

- MSVC RelWithDebInfo production `openmw.exe` build and link: PASS.
- 11 CTest cases: PASS, including repaired/full-control publication and semantic
  tests, effect capture, update-only lifecycle, actor skin/attachment/rigid pose,
  dynamic actor/deformation and enabled/disabled diagnostic emission.
- Effect suite: 46 cases, zero failures. Texture tests include same-time VFS
  winner replacement, timestamp edit, capacity, reset, per-frame metadata calls,
  and shared legacy/shader hashes across separate bundles.
- Actor tests: 32 skin-space combinations plus attachment and rigid-creature
  oracle checks. Missing transform remains rejected.
- Publication fixture: invalid light and NaN mesh rejection, mixed batch rollback,
  stale handles, retirement, reset and final independent full-world audit pass.
- 12 Python diagnostic tests and all seven CP4 source contracts: PASS.
- `git diff --check`: PASS. Existing compiler warnings remain (numeric narrowing,
  YAML DLL interface, unused historical variables); no compiler errors.

Narrow same-executable CPU fixture measurements, NOT gameplay benchmarks:

| Fixture | Repaired | Full-publication control |
|---|---:|---:|
| 100 light updates, one 300000-vertex immutable mesh | 0.1146 ms | 140.636 ms |
| 24 mesh publications, 300000 vertices per record | 33.2784 ms | 455.761 ms |

Mesh payloads in this fixture share immutable backing; it specifically measures
redundant validation/publication work, not disk, GPU upload, frame rate or RAM
under the modded exterior. It does not predict a gameplay speedup factor.

Built executable:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc/openmw.exe`.
SHA256: `a577fb901200bbbff41f9a45132184104a3234cf046378b54c8b6214cc5bf56a`.
Existing `_zZz__Wake_up.omwsave` checksum remains
`d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`.

## Exact files changed in this batch

- apps/openmw/CMakeLists.txt
- apps/openmw/mwrender/v4enginerenderbridge.cpp
- apps/openmw/mwrender/v4rigidactorpose.hpp (new)
- apps/openmw/mwrender/v4scenerenderlifecycle.cpp
- apps/openmw/mwrender/v4scenerenderlifecycle.hpp
- components/nifrender/niftranslator.hpp
- components/nifrender/staticniftranslator.cpp
- components/nifrender/materialpass.hpp
- components/nifrender/texturepass.hpp
- components/nifrender/shadermaterialpass.hpp
- components/nifrender/textureidentitycache.hpp
- components/rendercore/renderworld.hpp
- components/rendercore/updatebatch.hpp
- components/render/backend/vsg/vsgruntimehost.cpp
- tools/v4/cp4/effect-capture-tests.cpp
- tools/v4/cp4/actor-skin-space-tests.cpp
- tools/v4/cp4/publication-runtime-tests.cpp (new)
- tools/v4/cp4/population-environment-contract.py
- tools/v4/cp4/runtime-blocker-contract.py
- tools/v4/cp4/GAMEPLAY-DIAGNOSTICS.md
- tools/v4/cp4/EXTERIOR-RUNTIME-REPAIR-2026-09-19.md (this record)

## Next acceptance gate

User-monitored, same-profile New Game -> name -> hatch -> exterior, recording
load times and real gameplay before/after transition; then existing-save load.
Review new trace, all fatal/error records and actor visuals before promotion.
Use a separate diagnostics-off run for performance acceptance. The collector
already records inherited OPENMW controls, executable hash and dirty-source
manifest. CP4F is not accepted yet; material artifacts, legacy debug HUD/map
coverage, exact save runtime parity and long-run memory safety remain open.
No promise that 13 FPS or the entire exterior delay is resolved until measured.
