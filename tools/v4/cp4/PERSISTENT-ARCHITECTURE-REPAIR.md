# Persistent Vulkan architecture repair — 2026-09-23

## Identity and scope

Branch: codex/cp4f-material-frame-repair, base efa4f3694fad904b3546e7c846c756403950f0d2.
Uncommitted local candidate on top of the previously audited repairs. This is
the first performance implementation batch from FULL-SOURCE-AUDIT-2026-09-23.md,
not a declaration that all audit findings or Vulkan compatibility are closed.
The package manifest records the executable hash and every dirty code/tool file.
No GitHub push, ordinary save migration, or mod installation was performed.

## Mechanisms implemented

1. **Persistent evaluated geometry:** exact byte comparison of OSG source
   arrays, primitive metadata/data, UV selection, TexMat and TexGen inputs.
   Unchanged geometry retains a private, validated neutral mesh owner. Material,
   texture identity, controller state, and world placement are still captured
   live. Unknown primitive subclasses use the ordinary conversion path.
   Publication shares immutable geometry rather than copying it again; completed
   GPU residents with that exact owner skip redundant stream updates.
2. **Shader family and pipeline reuse:** keep the six built-in compatibility
   families and their compiled variants across frame-local sharing arenas.
   A bounded 256-entry immutable graphics-pipeline cache never owns a per-draw
   configurator, descriptor, frame vertex array, or material. Existing render-pass,
   traversal-mask and view-lifetime pipeline checks remain in place.
3. **UI image reuse:** immutable texture backing can reuse the existing weak
   image cache. Draw graphs and descriptors remain fresh per overlay; mutable
   images and external render targets do not enter this optimization. The
   crash-prone mutable UI slot path has NOT been reinstated.
4. **Static revision gates:** successful static and asset publications carry
   separate revisions. Unchanged static frames avoid scanning every dependency;
   already-validated population groups avoid repeat group/dependency walks when
   the relevant revisions match. Changed placements and assets retain exact
   validation. This does not yet eliminate all plan copies or replace changed
   population geometry with placement-only GPU updates.
5. **Controls and diagnostics:** sampled capture_work rows now include
   geometry_snapshot_hits and geometry_snapshot_misses. Existing phase timing,
   residency, visibility, memory and launch identity evidence is retained.

The geometry cache is owner-thread local, limited to 4096 entries / 128 MiB
of accounted cached source/mesh data (container/scratch overhead is additional).
It weakly observes OSG objects, so it does not retain unloaded gameplay objects.
Eviction releases only cache ownership; published frames keep their own owners.
This is not a process memory cap, nor proof of a long-session memory plateau.

## Independent same-executable controls

All new mechanisms are opt-in outside the candidate launcher. Presence enables
the corresponding environment variable (unset it to disable; do not set it to 0).

| Mechanism | Variable | Individual launcher selection |
|---|---|---|
| Geometry capture | OPENMW_V4_PERSISTENT_CAPTURE | capture |
| Shader families | OPENMW_V4_PERSISTENT_SHADERS | shaders |
| Graphics pipelines | OPENMW_V4_PERSISTENT_PIPELINES | pipelines |
| UI sampled images | OPENMW_V4_PERSISTENT_UI_IMAGES | ui-images |
| Static revisions | OPENMW_V4_STATIC_REVISION_GATE | static-revisions |

gameplay-diagnostics.py launch --cpu-fastpaths all enables these plus the earlier
parallel publication, validated reuse, and bulk-stream mechanisms. Individual
selections isolate one mechanism; control disables all of those CPU fast paths.
The manifest records effective controls. No settings in the normal user profile
are edited. New geometry capture stays on the owning thread; bounded publication
workers still touch neutral data only.

## Test launch

In the separate candidate directory, use **Start-Persistent-Vulkan.cmd**.
It enables the CPU mechanisms and existing static frustum / terrain occlusion.
It leaves the experimental native post target off. It does NOT implement
Rafael/F2/.omwfx parity or silently substitute a different effect chain.

Choose NEW GAME in this isolated profile. Regular saves are not copied or exposed.
Test the same exterior route and a short interior/NPC/menu segment, then quit
normally. The helper writes logs, reports, effective configuration and a ZIP to
Test-Results/<timestamp>-gameplay-<pid> beside the executable.
Start-Persistent-Control.cmd is available for targeted questions in this same
binary; no rerun of an old rejected executable is requested.
Start-Persistent-Profile.cmd uses the same optimized settings with the installed
Nsight hotkey capture, if an additional trace is needed. It does not self-elevate.

## Validation and remaining work

Final candidate: C:\OpenMW-Persistent-Test\openmw.exe

SHA256: DA11F24BB8B1F44A9DFADE308FCD32DB986F33A3BA37CC1E0BF2CFFD4151E20C.

- Final rendering CTest: 13/13 passed (2.28 seconds).
- Capture checks: 27/27 in ordinary and persistent modes.
- Synthetic production-capture workload: 12000 vertices, 200 warm repetitions,
  ordinary 76.4523 ms, persistent 2.8046 ms with 200 shared owners. This is an
  isolated CPU mechanism observation, not exterior frame timing or an FPS claim.
- Final validation-enabled Vulkan pixel matrix: passed; no validation messages.
- Diagnostic helpers: gameplay 26, configuration 16, runtime 17 passed / 1 skipped;
  shader resources 10 passed.
- Application route, streaming, population/environment, water/environment,
  video/UI, host-memory and runtime-blocker source gates passed.
- Generated-output verification passed: six source omissions and four provenance
  outputs match the frozen canonical-LF hashes; no generated output was changed.
- Real Nsight argument transport and candidate --version smoke passed. No game
  session, elevated CPU sampling, or GPU profiling was performed by that check.
- Incremental production build logs: Vulkan 0 errors / 99 warning lines; OpenGL
  0 errors / 77 warning lines. These counts are not unique/new warning counts.
- Package shader verification: 82 files passed. Package size at staging:
  319282988 bytes (about 305 MiB). The old staged builds were not overwritten.

Both full Release production targets (Vulkan and OpenGL) linked successfully.
Compiler warnings remain; this is not a warning-free build.
The rendering regression suite includes both capture modes, exact unmarked
mutation detection, immutable publication ownership, pipeline/image identity
and eviction, static asset invalidation, actor/static classification, failed
publication, reparenting, and the existing visibility/fence tests.
The Vulkan pixel matrix passed on the RTX 5050 Laptop GPU with shader/pipeline
reuse enabled and validation requested, without emitted validation messages.

The stale effect-update source guard now checks the real validated/ordinary
writer paths rather than an obsolete call spelling. The generated-output gate
now explicitly hashes canonical LF UTF-8 for its frozen text allowlist; it
preserves all non-newline bytes and no generated payload was rewritten.

Remaining audit work includes earlier multi-view visibility for evaluated
opaque draws, more incremental placement updates, pipeline-audit/UI traversal
costs, command-recording parallelism, asynchronous readback, and shader/mod/save
compatibility gaps. Existing terrain/chunk occlusion and NPC picking fixes remain
enabled/preserved; broad building occluders and previously rejected unsafe
approaches were not reintroduced.

Source tests and synthetic capture timings are not gameplay FPS or compatibility
acceptance. Promotion requires the user's new capture: whole-frame time,
capture/present subphases, actual cache reuse, bounded memory, accurate picking,
and no new visual/mod regressions.

## Files changed in this batch

- apps/openmw/mwrender/v4effectcapture.hpp
- apps/openmw/mwrender/v4geometrysnapshotcache.hpp (new)
- components/debug/gameplaydiagnostics.hpp
- components/rendercore/effectframe.hpp
- components/rendercore/renderworld.hpp
- components/render/backend/vsg/immediateeffectcontract.hpp
- components/render/backend/vsg/immediateeffectrealizer.hpp
- components/render/backend/vsg/persistentpipelinecache.hpp (new)
- components/render/backend/vsg/staticassetrealizer.cpp
- components/render/backend/vsg/staticpopulationresidency.hpp
- components/render/backend/vsg/staticworldplan.hpp
- components/render/backend/vsg/staticworldsyncstate.hpp
- components/render/backend/vsg/vsgruntimehost.cpp
- components/vsgmygui/rendermanager.cpp
- tools/v4/V4-CP0A-Verify-Materialized-Generated-Outputs.py
- tools/v4/cp4/gameplay-diagnostics.py
- tools/v4/cp4/runtime-blocker-contract.py
- tools/v4/cp4/frame-handoff-tests.cpp
- tools/v4/cp4/rendering-capture-tests.cpp
- tools/v4/cp4/static-sync-state-tests.cpp
- tools/v4/cp4/persistent-resource-tests.cpp (new)
- tools/v4/cp4/rendering-tests/CMakeLists.txt
- tools/v4/cp4/Start-Persistent-Vulkan.cmd (new)
- tools/v4/cp4/Start-Persistent-Control.cmd (new)
- tools/v4/cp4/Start-Persistent-Profile.cmd (new)
- tools/v4/cp4/stage-persistent-candidate.ps1 (new)
- this implementation report (new)

Other pre-existing dirty files in the worktree are retained unchanged by this batch.
