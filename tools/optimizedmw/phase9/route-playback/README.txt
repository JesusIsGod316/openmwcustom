PRIVATE BALMORA INPUT PLAYBACK PREVIEW

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

This preview supports the current exterior first-person Balmora recording only.
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
