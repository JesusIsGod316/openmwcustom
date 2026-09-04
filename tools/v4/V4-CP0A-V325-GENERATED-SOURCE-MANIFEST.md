# V4.0 CP0A — Final V3.25 Generated Source Manifest

Final accepted build raw commit: `f7557829bcb14e339410cefb32b6612e5009e46d`  
Preflight artifact carrying exact generated source delta: `9921224442`  
`V3-applied-source.patch` SHA-256: `ac15332aeff34a5ce6a442e169704aab6f5eb0f1600b914d863b4561452d4c35`  
`V3-applied-source-stat.txt` SHA-256: `c01f35f1a047b624b14daec279580805722fe4dbacdd5bb2b178e071ca28f045`  
Delta size: **103 files, +11,021 / -515 lines**.

## Authority rule

For CP0A/CP1, `f7557829` raw source alone is not the final V3.25 semantic source. The accepted executable was compiled after `tools/v3/apply_diagnostic_harness.py` materialized this exact patch. Any V4 base must preserve the union of raw source plus this exact generated delta.

## Triage priorities

- **P0 renderer/build overlap:** must be reconciled before or while defining CP1. Includes renderer, resource, SceneUtil, shader, terrain, root/app CMake, and engine render integration.
- **P1 semantic/API overlap:** must be audited before broad renderer/math refactors. Includes MWWorld streaming, Lua/camera, mechanics/physics seams, settings and Lua API.
- **P2 adjacent runtime:** preserve through V4 but not a renderer-boundary blocker unless donor edits collide.
- **P3 V3 harness:** provenance only; must not remain a hidden V4 runtime/source dependency.

## Complete 103-file generated delta inventory

```text
CMakeLists.txt
apps/openmw/CMakeLists.txt
apps/openmw/engine.cpp
apps/openmw/mwlua/camerabindings.cpp
apps/openmw/mwlua/camerabindings.hpp
apps/openmw/mwlua/engineevents.cpp
apps/openmw/mwlua/globalscripts.hpp
apps/openmw/mwlua/localscripts.hpp
apps/openmw/mwlua/luamanagerimp.cpp
apps/openmw/mwlua/luamanagerimp.hpp
apps/openmw/mwlua/soundbindings.cpp
apps/openmw/mwmechanics/actors.cpp
apps/openmw/mwmechanics/mechanicsmanagerimp.cpp
apps/openmw/mwphysics/physicssystem.cpp
apps/openmw/mwrender/animation.cpp
apps/openmw/mwrender/animation.hpp
apps/openmw/mwrender/camera.cpp
apps/openmw/mwrender/camera.hpp
apps/openmw/mwrender/groundcover.cpp
apps/openmw/mwrender/groundcover.hpp
apps/openmw/mwrender/nisscaler.cpp
apps/openmw/mwrender/nisscaler.hpp
apps/openmw/mwrender/npcanimation.cpp
apps/openmw/mwrender/npcanimation.hpp
apps/openmw/mwrender/objectpaging.cpp
apps/openmw/mwrender/objectpaging.hpp
apps/openmw/mwrender/objects.cpp
apps/openmw/mwrender/objects.hpp
apps/openmw/mwrender/occlusionculling.cpp
apps/openmw/mwrender/occlusionculling.hpp
apps/openmw/mwrender/pingpongcanvas.cpp
apps/openmw/mwrender/pingpongcanvas.hpp
apps/openmw/mwrender/pingpongcull.cpp
apps/openmw/mwrender/postprocessor.cpp
apps/openmw/mwrender/postprocessor.hpp
apps/openmw/mwrender/renderingmanager.cpp
apps/openmw/mwrender/renderingmanager.hpp
apps/openmw/mwrender/v318_nis_config.hpp
apps/openmw/mwrender/v318_nis_shader.hpp
apps/openmw/mwrender/water.cpp
apps/openmw/mwsound/ffmpegdecoder.cpp
apps/openmw/mwsound/ffmpegdecoder.hpp
apps/openmw/mwsound/headcache.cpp
apps/openmw/mwsound/headcache.hpp
apps/openmw/mwsound/openaloutput.cpp
apps/openmw/mwsound/openaloutput.hpp
apps/openmw/mwsound/sfxpredecodecache.cpp
apps/openmw/mwsound/sfxpredecodecache.hpp
apps/openmw/mwsound/soundbuffer.cpp
apps/openmw/mwsound/soundbuffer.hpp
apps/openmw/mwsound/soundmanagerimp.cpp
apps/openmw/mwsound/soundmanagerimp.hpp
apps/openmw/mwsound/soundoutput.hpp
apps/openmw/mwworld/cellpreloader.cpp
apps/openmw/mwworld/cellpreloader.hpp
apps/openmw/mwworld/scene.cpp
apps/openmw/mwworld/scene.hpp
components/debug/v36controllertrace.hpp
components/debug/v36gpuprofiler.hpp
components/debug/v36luaaddscripttrace.hpp
components/debug/v36structuretrace.hpp
components/debug/v3diagnostics.hpp
components/debug/v3gpumemory.hpp
components/debug/v3hitchtelemetry.hpp
components/detournavigator/navigatorimpl.cpp
components/lua/luastate.cpp
components/lua/luastate.hpp
components/lua/scriptscontainer.cpp
components/lua/scriptscontainer.hpp
components/resource/bulletshapemanager.cpp
components/resource/bulletshapemanager.hpp
components/resource/imagemanager.cpp
components/resource/multiobjectcache.cpp
components/resource/multiobjectcache.hpp
components/resource/objectcache.hpp
components/resource/resourcemanager.hpp
components/resource/resourcesystem.cpp
components/resource/resourcesystem.hpp
components/resource/scenemanager.cpp
components/resource/scenemanager.hpp
components/sceneutil/framejobservice.hpp
components/sceneutil/mwshadowtechnique.cpp
components/sceneutil/mwshadowtechnique.hpp
components/sceneutil/occlusionculling.cpp
components/sceneutil/occlusionculling.hpp
components/sceneutil/shadow.cpp
components/sceneutil/workqueue.cpp
components/sceneutil/workqueue.hpp
components/settings/categories/camera.hpp
components/settings/categories/cells.hpp
components/settings/categories/lua.hpp
components/settings/categories/shadows.hpp
components/settings/categories/sound.hpp
components/settings/categories/video.hpp
components/settings/ramcache.hpp
components/settings/sanitizerimpl.cpp
components/settings/sanitizerimpl.hpp
components/settings/v36profile.hpp
components/shader/shadermanager.cpp
components/terrain/chunkmanager.cpp
files/lua_api/openmw/camera.lua
files/settings-default.cfg
tools/v3/launchers/V3_Lab.ps1
```

## P0 renderer/build overlap

```text
CMakeLists.txt
apps/openmw/CMakeLists.txt
apps/openmw/engine.cpp
apps/openmw/mwrender/* [all generated changes listed above]
components/resource/* [generated changes listed above]
components/sceneutil/* [generated changes listed above]
components/shader/shadermanager.cpp
components/terrain/chunkmanager.cpp
```

These include Mode150/151 animation ownership, FBFP/camera behavior, NIS/render scaling, object paging, occlusion, postprocessing, water, shadow/QoS, resource-cache behavior and shader changes. Donor work touching these files must be reconciled against the generated source, not raw `f7557829`.

## P1 semantic/API overlap

```text
apps/openmw/mwlua/* [generated changes listed above]
apps/openmw/mwmechanics/actors.cpp
apps/openmw/mwmechanics/mechanicsmanagerimp.cpp
apps/openmw/mwphysics/physicssystem.cpp
apps/openmw/mwworld/cellpreloader.cpp
apps/openmw/mwworld/cellpreloader.hpp
apps/openmw/mwworld/scene.cpp
apps/openmw/mwworld/scene.hpp
components/lua/* [generated changes listed above]
components/settings/* [generated changes listed above]
files/lua_api/openmw/camera.lua
files/settings-default.cfg
```

These are not automatically renderer implementation files, but they define behavior, ownership, settings and API surfaces that CP1/SDL3/GLM changes can accidentally alter.

## P2 / P3 handling

Generated sound/cache, diagnostics and navigator changes remain part of the final V3.25 source authority but are not CP1 renderer blockers unless a donor collides with them. `tools/v3/launchers/V3_Lab.ps1` is provenance/harness only. V4 should materialize required source semantics and stop depending on the V3 patch harness for correctness.

## Materialization gate

Before CP1 modifies renderer ownership, create a V4 base whose source tree visibly contains all required final V3.25 generated semantics. Validate it against this patch hash and the frozen V3.25 behavior corpus. Do not use the raw freeze branch alone as the CP1 source authority.
