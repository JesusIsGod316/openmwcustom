# V4.0 CP0 — Visual / Behavioral Parity Corpus Plan

Status: **READY FOR FINAL V3.25 CAPTURE**  
Control: final V3.25 Mode151 frozen cohort  
Purpose: create the immutable visual/behavioral oracle used to judge every V4 Vulkan parity checkpoint.

This plan compresses the original 30-item compatibility checklist into a small set of high-information reference scenes plus explicit semantic checks. A scene may satisfy several checklist requirements, but no requirement is dropped.

## 1. Capture rules

All **base-control** captures must use:

- final V3.25 `openmw.exe` SHA-256 `34d7a715e25d92dcad6b20f807e8b44a272fd3382e2d2f0a22e03bedac3e25c2`;
- `V3.25_Mode151_Gaming.bat` cadence-2 wrapper, not the aggressive cadence-3 wrapper;
- final frozen gaming settings profile;
- same `openmw.cfg` / mod-content cohort;
- same canonical save whenever the scene can be reached from it;
- 1920x1080 native render scale 1.0;
- 4x MSAA;
- PBR/normal/specular behavior enabled as in the frozen profile;
- postprocessing chain `HBAO,VAIO,godrays,DIVE,wetworld,tonemap,SMAA,SMB`;
- three 2048 shadow maps, 4096 maximum shadow distance;
- groundcover density 1.0;
- diagnostic/deep telemetry disabled.

Do not use external ReShade, NIS, frame generation, Lossless Scaling or driver filters for the parity references unless they are part of the explicitly frozen cohort. The reference should represent what OpenMW itself renders.

For any deliberately changed compatibility variant, record the one setting/state changed from the base control. A variant is not allowed to silently become the new base reference.

## 2. Image requirements

For each image:

- lossless PNG preferred;
- no crop, resize, sharpening or color edit;
- capture full 1920x1080 game output;
- leave HUD/UI visible only when the scene is testing HUD/UI;
- use a stable camera/view and avoid motion during capture;
- where dynamic animation/weather matters, capture a short numbered sequence rather than relying on one arbitrary frame;
- retain source filename, copied canonical filename, byte size and SHA-256 in the corpus manifest.

Later V4 A/B captures must use the same scene definition and camera as closely as possible. Pixel-perfect equality is not expected for all effects, but missing geometry, wrong material semantics, view-mask failures, lighting shifts, alpha-order errors or changed postprocess behavior are parity failures.

## 3. Base scene set

### `EXT_DAY_01` — exterior fidelity anchor

Capture a representative exterior from the canonical route with a stable camera showing:

- near statics;
- distant statics/object paging;
- terrain and terrain transitions;
- groundcover foreground/mid-distance;
- alpha-tested foliage if present;
- sky/sun;
- terrain/object/player or actor shadows if available;
- PBR/normal/specular response.

Covers original checklist: 1, 9, 12, 13, 17, 18, 19 and part of 27.

Preferred sequence: three images from the same camera: normal view, slight left rotation, slight right rotation. The side views make culling/disocclusion errors easier to detect later.

### `EXT_WEATHER_02` — weather / precipitation / animated vegetation

Use a deterministic or repeatable weather state in an exterior with visible vegetation/groundcover and sky.

Capture:

- precipitation/weather appearance;
- weather particle occlusion;
- animated vegetation/groundcover response;
- fog/atmosphere;
- changed sun/ambient conditions;
- shadow behavior if the weather permits it.

Covers: 13, 17, 18, 20.

If weather timing cannot be made deterministic, record the exact weather ID/state and use the images as qualitative parity references rather than pixel-comparison fixtures.

### `WATER_SURFACE_03` — water surface / reflection / shore

Choose a shoreline with obvious static geometry, sky and preferably vegetation visible in/through the water.

Capture a camera angle where the same frame contains:

- water surface shading;
- reflection;
- refraction/shore visibility;
- ripples if repeatable;
- reflected sky/statics;
- alpha foliage near or across water if available.

Covers: 14, 15, 16 and part of 30.

### `WATER_UNDER_04` — underwater/refraction anchor

Capture immediately below the water surface looking toward terrain/objects through the water boundary.

Covers:

- underwater state;
- refraction;
- fog/color transition;
- waterline geometry/depth behavior.

Covers: 16.

### `INTERIOR_LIT_05` — interior material and light anchor

Choose a normal interior containing several materials and multiple local lights if possible.

Capture:

- interior ambient/fog behavior;
- point-light attenuation/color;
- normal/specular/PBR surfaces;
- alpha blend/test if available;
- object/actor shadows;
- HBAO/postprocessing.

Covers: 2, 10, 18, 19, 21.

### `CROWD_ACTORS_06` — skinned actors / equipment / lighting

Use a crowded scene with several actors and mixed equipment.

Capture a short 3-frame sequence while actors are visibly animating.

Required coverage:

- multiple skinned actors;
- body/head/skin geometry;
- armor/clothing/weapon/shield attachments;
- equipment materials;
- actor shadows;
- local lighting across actors;
- animation pose continuity.

Covers: 3, 6, 7, 18, 19.

### `ANIM_NIF_07` — non-actor animated NIF/controller

Find a scene containing an animated object/NIF controller distinct from normal actor skeleton animation. Capture at least two clearly different controller states.

Covers: 8.

### `ALPHA_EFFECTS_08` — transparency / particles / projectiles

Capture a scene containing as many as practical of:

- alpha-tested geometry;
- alpha-blended geometry;
- soft particles;
- magic VFX;
- projectile/effect;
- overlapping transparent objects.

A 3-frame sequence is preferred for animated particles/effects.

Covers: 9, 10, 20.

### `FBFP_MAIN_09` — accepted full-body first person

With the frozen base control's full-body first-person mode enabled, capture looking downward/forward so the player body is visible.

The reference must show:

- body/arms/legs placement;
- held weapon/shield if used;
- head/hair/helmet absent from the main owner scene where final V3.25 hides them;
- correct body animation and camera relationship;
- normal world geometry and postprocessing unchanged.

Covers: 5 and part of 30.

### `FBFP_SECONDARY_10` — owner secondary-view semantics

From a scene where the player produces a visible shadow and, if practical, a water reflection/refraction secondary view, capture the final V3.25 behavior.

The critical semantic oracle is that content hidden from the main owner view must not be incorrectly removed from secondary views that need it.

Capture at minimum:

- first-person main scene with body;
- corresponding player shadow in the same setup.

Add water reflection/refraction reference if the chosen location supports it.

Covers: 19 and 30.

### `FIRSTPERSON_NATIVE_VARIANT_11` — native first-person compatibility variant

This is an explicit one-setting compatibility variant, not the base control.

Disable only the V3.21 full-body-first-person feature while keeping the remaining frozen renderer/content settings unchanged. Capture the equivalent first-person view and record the setting delta in the manifest.

Covers: 4.

The purpose is to prevent the Vulkan backend from accidentally making FBFP the only valid first-person representation.

### `UI_MAPS_12` — HUD / inventory / maps / previews

Capture separate images under one scene group:

- gameplay HUD;
- inventory/character preview with equipped gear;
- local map;
- world map.

Covers: 22, 23, 24, 25.

### `LOADING_13` — loading / resize-independent compositor reference

Capture the loading screen reached by a normal interior/exterior transition, preferably one on the canonical route.

Covers: 26 and supports transition parity.

## 4. Semantic/non-image checks

Some compatibility requirements cannot be proven by one screenshot. Store these in the same corpus manifest as `PASS` checks with notes and the exact save/cell used.

### `RCN_CHECK_14`

Validate a known asset/location using RootCollisionNode / root `RCN` recursive collision semantics:

- intended collision works;
- collision-only geometry remains visually hidden;
- child collision selection is not broadened incorrectly.

Covers: 11.

### `CELL_CROSSING_CHECK_15`

Traverse the canonical exterior cell crossing route and confirm:

- no missing/duplicated statics;
- terrain/groundcover transitions are visually correct;
- paging does not leave stale objects behind;
- normal route completes.

Save the route/benchmark bundle separately for performance timing.

Covers: 27.

### `INTERIOR_EXTERIOR_CHECK_16`

Perform exterior -> interior -> exterior transition and verify:

- water/sky/terrain disappear/reappear correctly;
- environment/fog/light state switches correctly;
- UI/loading flow is intact;
- no stale previous-world geometry remains.

Covers: 28.

### `SAVELOAD_ACTOR_CHECK_17`

With visibly animated/equipped actors nearby:

1. save;
2. reload;
3. confirm actor equipment, pose/animation behavior and scene membership recover normally.

Covers: 29.

## 5. Coverage map

| Original requirement | Corpus coverage |
| --- | --- |
| 1 exterior terrain/statics | EXT_DAY_01 |
| 2 interior | INTERIOR_LIT_05 |
| 3 crowd | CROWD_ACTORS_06 |
| 4 native first person | FIRSTPERSON_NATIVE_VARIANT_11 |
| 5 FBFP | FBFP_MAIN_09 |
| 6 equipment | CROWD_ACTORS_06, UI_MAPS_12 |
| 7 skinned actor | CROWD_ACTORS_06 |
| 8 animated NIF | ANIM_NIF_07 |
| 9 alpha test | EXT_DAY_01, ALPHA_EFFECTS_08 |
| 10 alpha blend | INTERIOR_LIT_05, ALPHA_EFFECTS_08 |
| 11 RCN | RCN_CHECK_14 |
| 12 terrain transitions | EXT_DAY_01, CELL_CROSSING_CHECK_15 |
| 13 groundcover | EXT_DAY_01, EXT_WEATHER_02 |
| 14 water surface | WATER_SURFACE_03 |
| 15 reflection | WATER_SURFACE_03 |
| 16 refraction/underwater | WATER_SURFACE_03, WATER_UNDER_04 |
| 17 sky/weather | EXT_DAY_01, EXT_WEATHER_02 |
| 18 lighting | EXT_DAY_01, INTERIOR_LIT_05, CROWD_ACTORS_06 |
| 19 shadows | EXT_DAY_01, INTERIOR_LIT_05, CROWD_ACTORS_06, FBFP_SECONDARY_10 |
| 20 particles/effects/projectiles | EXT_WEATHER_02, ALPHA_EFFECTS_08 |
| 21 HBAO/postfx | INTERIOR_LIT_05 plus other base screenshots |
| 22 world map | UI_MAPS_12 |
| 23 local map | UI_MAPS_12 |
| 24 character/inventory preview | UI_MAPS_12 |
| 25 GUI/HUD | UI_MAPS_12 |
| 26 loading | LOADING_13 |
| 27 exterior crossings | CELL_CROSSING_CHECK_15 |
| 28 interior/exterior transition | INTERIOR_EXTERIOR_CHECK_16 |
| 29 save/load actors | SAVELOAD_ACTOR_CHECK_17 |
| 30 FBFP secondary views | FBFP_SECONDARY_10 |

## 6. Required manifest identity

The final visual corpus directory must include:

- `visual-corpus-manifest.json`;
- `visual-corpus-summary.txt`;
- copied canonical screenshot files;
- SHA-256 for every screenshot;
- the CP0 freeze capture JSON/content manifest from `V4-CP0-Capture.ps1`;
- hash of the canonical save;
- hash of final gaming wrapper/settings/openmw.cfg;
- notes for all semantic checks;
- explicit record of the native-first-person compatibility variant setting delta.

The corpus hash is the SHA-256 of a deterministic manifest containing ordered file IDs + their SHA-256 values, not a ZIP container hash whose metadata can vary.

## 7. CP0 visual acceptance

The corpus is accepted only when:

- every requirement 1–30 has at least one mapped reference or semantic check;
- every mandatory base screenshot exists and is lossless/unmodified;
- semantic checks 14–17 are recorded PASS;
- the base capture identity matches the final V3.25 executable and frozen cohort;
- the canonical manifest is hashed and archived;
- the later clean Mode151 performance baseline can reference the same save/mod/settings identity.

After this corpus is frozen, the CP1 implementation block is removed. CP1 itself is not expected to improve performance; its first acceptance condition is that the legacy OpenGL backend still matches these references.