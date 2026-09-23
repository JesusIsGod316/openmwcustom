# CP4F evaluated bump and lit auxiliary-view repair

Base: `ddcb8cecfad3f8a9199bd3411033be676be6720f`.
This is a combined source candidate, not a user-runtime acceptance or measured
FPS claim. Preserve the memory-pressure policy, native terrain route, UV sharing,
actor-plan reuse, normal saves, and previously implemented rendering contracts.

## New capture: observations and limits

The matching ddcb Standard Vulkan capture reaches the ship, exterior and race
preview before aborting on the Excise Office transition. The fatal reports the
legacy BumpTexture metadata contract. The prior generated shadow-sentinel error
is absent. Race-preview revisions 4 through 9 reach capture/publication, despite
the user's black/incorrect preview and apparently unresponsive buttons. This
proves some updates reach the renderer, not that every UI event or the guard's
quest/AI state works correctly. No input-coordinate or quest-state trace proves
those separate reports resolved.

The recorded exterior transition is about 6.06 seconds, with the legacy render
terrain route disabled. Exterior frame samples remain about 269 ms median and
large native capture/submission CPU costs remain. The host policy actually
reclaims optional preload owners under pressure while retaining normal useful
caching. These are one-run observations, not a controlled benchmark against the
earlier e26 capture. Do not use on-screen system RAM as process-private commit.

## Evaluated BumpTexture parameters

NifLoader's OSG state carries `bumpMapMatrix` (mat2) and `envMapLumaBias` (vec2).
Evaluated capture previously retained the texture stage but lost those live
uniforms and `bumpParametersEnabled`, violating static realization's invariant.
Copy the exact four matrix scalars and luminance scale/bias from the winning
merged state; preserve controller changes, texture identity and UVs. Reject
missing/wrong-type/nonfinite or multiple bump contracts rather than disabling
bump mapping or weakening the backend guard. The original ddcb header fails the
expanded fixture for this condition. The old fatal lacks a drawable identity;
mesh/material/count/enabled fields and evaluated-draw identity are now attached
to the retained fatal to identify another unsupported case.

## Lit previews, reflection and refraction without shadow-map recording

Pinned VSG 1.1.15 `ViewDependentState::traverse` returns immediately when
`RECORD_SHADOW_MAPS` is absent, before packing ambient/directional light data.
Our isolated preview and water reflection/refraction views deliberately use
`RECORD_LIGHTS`. Populate VSG's actual supported light-buffer layout explicitly
for these views without enabling, allocating, or rendering shadow maps. Counts,
ambient, directional, point and spot slots preserve the pinned layout; uploads
are marked dirty only when the values change. Capacity overflow remains an error.
The production pixel fixture renders the repaired view lit; the legacy control
renders it black. It also checks changing lights, removal and ray orientation.

Do not inject main-world local lights into isolated character previews. Their
native lighting snapshot remains authoritative. This fixes a demonstrated cause
of a black preview and black auxiliary water captures, not every orientation,
water, terrain or texture problem in a modded game.

## Independent sun specular

The original OpenMW shader has separate sun diffuse and specular colors. The
compatibility fragment previously ignored the captured specular color, including
an explicit zero. Preserve it in the unused yzw components of the existing ninth
environment vec4. The enchanted 16 Hz index remains in x; no field offsets or
buffer size change. Ordinary and enchanted shader families now share that
explicit declaration. Previews keep their original zero sun-specular value.
A production pixel test verifies zero and independently colored specular updates;
the old-path control fails by rendering a white highlight instead of black.
This does not establish the exact cause of all cyan/green authored materials.

## Bounded CPU work and attribution

One immutable effective OSG state is shared by material and UV capture inside
each evaluated draw. Previously the same inherited state was merged twice. No
snapshot persists across frames, and no animation update, object ordering or
actor-reuse predicate changes. A comparison switch restores the repeated merge.
Both paths pass the same evaluated-capture fixture; no hardware frame-time
improvement is claimed before another comparable runtime capture.

The bounded Standard recorder also separates auxiliary sync, image acquisition,
completion polling, retirement, native sky preparation, local-light sync and VSG
update operations. These are CPU intervals within existing parent stages, NOT
GPU timings or extra waits. They address attribution of the still-large steady
frame cost; they are not themselves a performance repair.

## Same-executable controls

Presence selects the old comparison even when the value is `0`. Keep controls
fixed during one run. Unset selects the repaired implementation.

- `OPENMW_V4_LEGACY_UNSHADOWED_LIGHTS_CONTROL`: restore pinned VSG early return.
- `OPENMW_V4_LEGACY_SUN_SPECULAR_CONTROL`: restore white sun-specular response.
- `OPENMW_V4_REPEAT_CAPTURE_STATE_CONTROL`: repeat the effective-state merge.

No legacy bump-data-loss switch is provided: the retained invariant already
aborts this invalid path. The original-header fixture is its regression control.
All earlier preview/sky/water/normal/retention/terrain/actor/effect controls remain.

## Validation and unresolved work

The real-OSG capture fixture has 24 cases; the ddcb original header passes 21/24
and fails the three targeted bump cases. Native and sanitized repaired capture
must pass all 24. Pixel tests execute actual production shaders, require a real
Vulkan implementation, and explicitly fail rather than skip missing devices.
The independent runner requires the validation layer and zero Vulkan errors;
all four old pixel controls must fail only their specific rendering assertions.
Retain UV, uniform-layout, memory/cache ownership, recorder, launcher, production
translation-unit and full Windows production/link/package checks.

The guard's timing/position, all individual GUI input events, complete preview
orientation, all cyan/green material assets, water/terrain parity, and the large
steady exterior frame cost are not marked repaired by these results. The user
need not rerun the failing old build. Source tests do not access the game or saves.
