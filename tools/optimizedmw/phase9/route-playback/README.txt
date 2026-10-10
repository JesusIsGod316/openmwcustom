PRIVATE OUTDOOR PHYSICAL ROUTE PREVIEW

The guided host profile selects [Game] player movement ignores animation = true
only in its private settings copy. Visual animation evaluation remains enabled;
native controls determine player displacement. This isolates the observed
reversal during side-dominant correction from animation-dependent locomotion.
It is a separate movement cohort, not proof of a defect in a particular mod.
The host records movement_model, private_movement_override and settings hash.
Fresh outdoor route recording and default playback use the same native setting.
-AnimationMovement explicitly disables the private native override for a guided
diagnostic control. -RawInputs preserves the recorded movement cohort when it
is recorded, or the frozen setting for a legacy source. The legacy complete
Balmora source remains explicitly a different movement cohort. Normal gameplay
and source captures are unchanged.

The host generates options.lua and timeline.lua under scripts/optimizedmw_route_playback.
Append this PLAYER script last in the private content order. The existing private
encounter policy must use the same fresh session identifier and remain enabled;
the user-driven recorder remains disabled during playback.

Playback loads a copied Start.omwsave, waits for its exterior starting cell,
confirmed encounter-policy readiness and three unpaused seconds, then follows
recorded movement/run/sneak controls in simulation time. The engine performs
ordinary movement and collision. Camera yaw/pitch are interpolated between
recorded samples; actor heading uses native controls toward the same angle.
The acquisition samples precede physics rotation. Heading commands therefore
sample elapsed simulation time plus the current frame delta, using a separate
pose-only sampler. Movement input and position comparisons remain at current
elapsed time. This phase repair is a preview mechanism, not exact native input
or deterministic traversal proof.
No position teleport or corrective position write is present.

options.guidance_enabled=true explicitly selects physical_xy_follower_v1. The
Lua package defaults to raw_physics_input when the flag is absent/false; the
host may select guided preview by default and expose its -RawInputs control.
Guidance is ordinary actor-local movement only. A separate setup phase aligns
XY within six units, with input norm <=0.2 and five simulation seconds maximum;
starting Z must remain within 48 units. Recorded camera headings and gait flags
remain the reference. No collision, stat, position, or console-mode writes occur.
Player collision must be confirmed enabled by the read-only debug API.

During guided travel, gain-two world XY feedback is projected into actor-local
controls using the heading mechanics consumes this frame. Correction norm is
<=0.35; total input norm is <=1. Reference time holds when current XY error
exceeds 32 units and resumes below 16. While held, nominal movement is zeroed
and bounded feedback catches up to the held target. Reference time is monotone,
and the next heading uses its allowed frame advance. Fixed-view/stop inputs stay
stationary; an out-of-corridor stationary pose fails explicitly. Hard XY/Z drift
limits are 96 units each. Command norm >0.25 with less than eight XY units of
progress for two simulation seconds fails as a stall. Clock lag >15 seconds or
runtime >195 seconds also fails (180 seconds of source plus at most 15 of lag). End XY <=24 and Z <=48 are required for finish.
These bounds cannot make collision-obstructed or physically unreachable paths
reachable. The live stairs/path check is required before accepting this preview.

Guided checks include source elapsed time, reference time/advance/hold/lag,
separate acquired and applied controls, corrections and gait-speed estimate,
XY/Z errors, and read-only collision/ground/swimming/current-speed observations.
Hold/resume and initial alignment events are explicit. getRunSpeed/getWalkSpeed
are estimates used to scale feedback; getCurrentSpeed is separately observed.
Neither speed read proves achieved physical velocity. Successful guided finish
sets route_playback_finished=true and input_replay_finished=false. Successful
raw finish sets both true. Both remain PREVIEW_NOT_BENCHMARK_VALIDATED.

This preview supports explicit exterior first-person existing (Seyda Neen) and
balmora route identities. Canonical Esm3ExteriorCell:x:y IDs and matching integer
grids identify named or unnamed outdoor cells. New existing acquisition must
prove the native movement cohort at start and finish. Legacy Balmora without
that metadata remains a scoped preview control.
It rejects jump/attack inputs and lacks activation, doors or menu replay. Bare
F11 aborts. Unexpected pause, policy failure, observed combat, mismatched starting
cell/position, or excessive path drift aborts. The host must preserve the recorder's
private native F11 conflict parking and configuration isolation.

OMWPLAY JSON observations compare actual and expected positions, cells, and
camera angles at 5 Hz. Completion drains the encounter observer, stops movement,
releases built-in input overrides, and quits if configured. Successful input
completion is still PREVIEW_NOT_BENCHMARK_VALIDATED. Normal physics, NPC movement,
script timing, and differing frame cadence can produce route drift. The comparison
is from PLAYER onFrame before physics. Check rows distinguish camera/actor angles
before commands, expected current heading derived from camera, commanded future
heading/time, and camera setter readback after writing. Postwrite camera readback
does not prove actor rotation has been consumed or the frame has rendered.
No performance conclusion or exact replay guarantee follows from
unit/mock checks, startup, or merely reaching the end.

Test source sampling and actual PLAYER lifecycle with:
python tools/optimizedmw/phase9/route-playback/tests/run_lua_tests.py
The runner uses the installed engine's Lua DLL. Source checks are separate from
live replay validation. User saves and the frozen baseline must remain unchanged.
