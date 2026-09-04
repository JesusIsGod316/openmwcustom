# OpenMW Custom Build V4.0 CP0 — Final V3.25 Freeze Manifest

Status: **CP0 ACTIVE — source/build identity frozen; final corpus capture still required**

Event: `evt.v4.0.cp0.freeze_preparation.001`

## 1. Authoritative V3.25 control identity

- Repository: `JesusIsGod316/openmwcustom`
- Final V3 source branch at closeout: `v3.25-engine-ownership-bridge-cp2-actorbatch`
- Immutable source commit: `f7557829bcb14e339410cefb32b6612e5009e46d`
- Freeze ref: `v3.25-final-freeze` -> exact commit above
- Final accepted runtime mode: **Mode151**
- Mode150: retained serial batching foundation/control
- Mode151: Mode150 + actor-batched parallel NIF controller preparation
- Mode152: declined; do not implement in V3.x
- No V3.26. V4.0 begins from this V3.25 control.

## 2. Final Windows build identity

- GitHub Actions run: `33829721075`
- Artifact ID: `9921964097`
- Artifact SHA-256: `6fc63c564cd71455b1ab3285c3632558c65d5f98fafa5ead6e0d461ff151d72f`
- `openmw.exe` SHA-256: `34d7a715e25d92dcad6b20f807e8b44a272fd3382e2d2f0a22e03bedac3e25c2`
- CI verification at closeout: Windows MSVC success; ZIP hash match; ZIP integrity pass; CI identity exact; `openmw.exe`, `V3_Lab.ps1`, and `V3_Unified_Test.bat` present.

## 3. Accepted runtime cohort identity

From the same-binary Mode150/151 acceptance A/B:

- `openmw.cfg` SHA-256: `de4048a4f8766e23f13a9c81eb1960924bec285894f1939a3c91ae184bef63b9`
- benchmark `settings.cfg` SHA-256: `461d64eab6f5d7b97d6bf008d2c41444c242d1b41d4a93861f6b4089b8f7c304`
- same executable: yes
- deep telemetry: off
- frame pacer: off/nonbinding for causal benchmarks
- resolution: 1920x1080
- groundcover density: 1.0
- stock Lua runtime

Final normal-gaming artifacts to preserve alongside the build:

- `V3.25_Mode151_Gaming.bat`
- `settings-v325-mode151-gaming.cfg`

These normal-gaming files must be hashed during the final local CP0 capture. Do not silently substitute the aggressive cadence-3 wrapper for the accepted cadence-2 control.

## 4. Accepted Mode151 process-local gates

The final normal-gaming control uses process-local environment gates so other builds/tests are not contaminated:

- `OPENMW_V319_FOCUS_CADENCE=2`
- `OPENMW_V320_ENGINE_LUA_FASTPATHS=1`
- `OPENMW_V320_SOUND_CONVERSION_CACHE=1`
- `OPENMW_V320_SOUND_QUERY_COALESCING=1`
- `OPENMW_V320_LUA_PROFILER_CAPABLE=1`
- `OPENMW_V321_CP2_FAIRNESS=1`
- `OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON=1`
- `OPENMW_V321_CP4_SHADOW_COMPAT=1`
- `OPENMW_V324_FRAME_JOB_QOS=1`
- `OPENMW_V325_ACTOR_SOURCE_BATCH=1`
- `OPENMW_V325_PARALLEL_ACTOR_BINDING=1`

Explicitly off for the frozen normal control:

- V3.20 adaptive focus scheduling
- V3.21 completion governor
- V3.22 CP1 MSOC hot path
- V3.22 CP2 occluder-efficiency experiment
- V3.22 actor-avoidance experiment
- V3.23 parallel MSOC experiment
- V3.24 async MSOC
- V3.24 deep telemetry
- V3.25 jobgroup stats output
- explicit OSG threading override (`Automatic/default` remains the legacy control)

## 5. Visual/render settings control

The final gaming profile is intended to preserve the accepted high-quality cohort, including:

- native render scale 1.0
- 4x MSAA
- anisotropy 16
- per-pixel/shader rendering enabled
- automatic object/terrain normal and specular maps enabled
- groundcover density 1.0
- full-body first person enabled
- 3 shadow maps
- shadow map resolution 2048
- maximum shadow distance 4096
- PBR/shader semantics preserved
- postprocessing enabled
- current chain: `HBAO,VAIO,godrays,DIVE,wetworld,tonemap,SMAA,SMB`

The exact local copies used for the final CP0 capture remain authoritative; hash them before declaring CP0 closed.

## 6. Existing Mode151 performance reference

The accepted same-binary Mode151 runtime A/B provides an initial reference, not the final dedicated CP0 baseline run:

- canonical normal exterior crossing wall median: **24.069 ms**
- wall p95: **31.194 ms**
- wall p99: **38.967 ms**
- frames >33.3 ms: **1.775%**
- frames >50 ms: **0.424%**
- render median: **13.249 ms**
- mechanics median: **1.499 ms**
- physics median: **0.860 ms**
- update median: **1.900 ms**
- Lua sync median: **2.923 ms**
- adapter peak: **7449.5 MB**
- minimum adapter free: **701.5 MB**
- hard VRAM pressure events: **0**

Do not use the broad draw/GPU shift in that paired run as causal Mode151 evidence; the closeout explicitly classified it as run variance. The final CP0 clean baseline should capture GPU/draw again under the frozen control.

## 7. CP0 compatibility/visual corpus

Capture reference screenshots and/or deterministic scene checks for all of the following before CP0 is closed:

1. normal exterior, including distant statics and terrain
2. normal interior
3. crowded actor scene
4. native first person
5. full-body first person
6. equipment/weapon combinations
7. skinned actor animation
8. animated NIFs/controllers
9. alpha-tested foliage/geometry
10. alpha-blended geometry
11. RootCollisionNode/RCN semantics
12. terrain transitions
13. groundcover, wind/stomp/fade-sensitive scenes
14. water surface
15. water reflection
16. water refraction/underwater
17. sky and weather
18. sun/point lighting, including clustered-lighting cases
19. player/actor/object/terrain shadows
20. particles/effects/projectiles
21. HBAO and full postprocessing chain
22. world map
23. local map
24. character/inventory previews
25. GUI/HUD
26. loading screens
27. exterior cell crossings
28. interior/exterior transitions
29. save/load around animated actors
30. FBFP secondary views: shadows/reflection/refraction

## 8. Final CP0 baseline capture requirements

One clean Mode151-only final run is still required after the local cohort is frozen. Record:

- source commit and freeze ref
- artifact/exe hashes
- `settings.cfg` hash
- `openmw.cfg` hash
- test save hash
- mod/content manifest hash or an immutable manifest sufficient to reconstruct the cohort
- gaming wrapper hash
- resolution and all quality settings
- frame cap/pacer state
- GPU driver version
- CPU/GPU/RAM hardware snapshot
- wall median/p95/p99/p99.5
- >25 ms, >33.3 ms, >50 ms counts/rates
- render/cull/draw/GPU medians and tails
- mechanics/physics/update/Lua sync
- VRAM peak/free/hard-pressure counters
- transition totals for the canonical route

Deep telemetry must remain off for the performance baseline.

## 9. CP0 closure gate

CP0 closes only when all are true:

- [x] V3.25 feature line closed at Mode151; Mode152 declined
- [x] exact final V3.25 source commit identified
- [x] exact Windows build artifact/executable identified and hashed
- [x] exact source freeze ref created
- [x] accepted runtime config hashes recovered from the Mode151 A/B
- [ ] final gaming wrapper/config files hashed
- [ ] canonical test save hashed
- [ ] mod/content cohort manifest frozen and hashed
- [ ] compatibility/visual reference corpus captured
- [ ] one clean final Mode151-only baseline capture archived

Until the unchecked items are complete, **do not begin CP1 implementation on the V4 line**. CP0A donor/convergence source audit may proceed in parallel because it cannot change the frozen runtime control.
