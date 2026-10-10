OptimizedMW private route recorder, schema 1

Purpose
-------
Acquire the existing user-guided route and a separate modded Balmora route.
This package records routes and a private starting-world save. It does not replay
routes, select optimization candidates, or establish performance results.

The PowerShell host copies this package into a unique private profile, generates
scripts/optimizedmw_route_recorder/options.lua, and adds the .omwscripts last.
The checked-in options are disabled. Never add this content to ordinary gameplay.
The installed engine/API is the Phase 9 engine at source 1b9aa66f.

Controls
--------
F6: capture a new private starting save, then begin recording.
F9: add a numbered stage marker.
F10: close acquisition, drain final actor observations, export, exit.
F11: abort, including while waiting for READY; export incomplete evidence and exit.

Use these keys on their own. Modified versions with Ctrl, Shift, Alt, or Windows
are ignored. Ctrl and Shift remain the normal Sneak and Run controls; recording
commands do not require them. A legacy explicit require_ctrl_shift=true options
file is supported for old fixtures, but the generated launcher defaults to false.

The host parks these native actions on unused keys in the copied bindings. An empty
native binding would be restored to its default key by the engine. The player
script removes up to eight matching keyboard bindings from copied OMWInputBindings
and reports their IDs; ordinary movement and all other bindings remain available.
Shift+F8 would toggle S3maphore music, so F8 is deliberately not used.

Stand still with menus closed when starting. The setup briefly holds movement and
the camera while the MENU script saves a NEW slot; no existing slot name is supplied.
A save failure prevents acquisition. Setup writes end after the save acknowledgement.
During recording, all movement, camera, actions, and controls remain user driven.
Before READY and again before the private starting save, setup disables only the
automatic idle Vanity camera switch in this private process and verifies its
readback. This keeps the fixed-view hold from becoming a camera orbit. Looking,
movement, jumping, combat, magic and manual first/third-person switching stay
available. If already in Vanity, setup restores the selected primary first/third-
person view. Normal first/third-person views are not rewritten. Failed readback
prevents READY or START and exports incomplete evidence. READY, START and FINISH
include private_camera_setup metadata. No normal gameplay profile is modified.
The first recorded pose and exact private save directory/filename appear in START.
The host copies and hashes this starting save as the route's future starting state.

Recorded data
-------------
20 Hz maximum observed player/camera poses; exterior grid and interior cell changes;
real, simulation and game clocks; actual Lua frame duration; movement/control/action
changes and raw mouse deltas per engine onFrame; activation/jump/run/UI triggers;
numbered stage markers; pause transitions; exact engine activation request targets,
door IDs and teleport destinations; hostile suppression identities and combat observations.

Engine onFrame handlers execute in content script order; the appended recorder reads
controls after earlier built-in/content handlers. Native yaw/pitch values are an API
readout at that phase, not a promise of a final post-physics camera/input snapshot.
Lua event handlers have a separate reverse dispatch order. Observers return nil.

The appended GLOBAL onActivate engine handler observes the actual activation request
target even when a custom standard-Activation-interface handler consumes an action.
It returns nil and never changes the action. A target invalidated by an earlier
handler or unreadable door destination explicitly invalidates capture coverage.
An Activate input trigger also records a synchronous center-screen rendering ray
as a visual candidate (or unknown/no hit), separately from the exact engine target.
An input trigger or exact request does not prove the action succeeded. Door receipts
and subsequent cell changes remain distinct evidence; custom actions still need
review before eventual automatic replay.

Samples are never invented to fill a long frame. These route-acquisition records are
not the independent benchmark frame-time measurements, and this recording run is
not a candidate performance comparison. The later replay/benchmark layer must use
the existing frame telemetry and verify candidate engagement.

Output and completeness
-----------------------
Each line is OMWROUTE| followed by compact JSON with schema/session/event/seq/t.
READY and START are immediate. Other rows are buffered during acquisition and only
printed after it closes, up to 32 lines per export frame. FINISH is the final row;
expected_records equals its sequence number. The host rejects missing/duplicate rows,
invalid clocks/poses, absent private starting saves, and unsuccessful finish flags.

Acquisition ends before final observation draining. STOP sends a timestamped global
policy request, waits for policy_stopped plus at least four onFrame calls, and times
out after five seconds. acquisition_end_wall excludes that drain, while the footer's
duration_wall includes it. Actor detections carry their observation clocks so late
deliveries can be classified against the recorded interval.

Limits: 600 seconds, 60,000 total records, 16 MiB encoded records, 8 KiB per line.
Budget exhaustion, reload, save failure, malformed data, combat, or failed policy
settling exports incomplete evidence. Reloading after READY does not continue a route.
Quitting before export finishes leaves a partial capture, which the host rejects.

Hostile policy
--------------
The separate policy scripts retain ordinary NPCs/Mercy and remove explicit hostile
references/records plus opt-in high-fight creatures. The latter is a discovery
heuristic, not a complete hostile classifier. Each queued disable needs confirmation.
The host reads encounter-policy.json, a bounded explicit list of 32 creature
records from the effective all-level encounter closure checked for the frozen
mod configuration after user Seyda capture b9a378d8bd244b6fb0afc735f847de34.
Rats, foragers, scribs, parasol spiders, netches and listed variants therefore
bypass Fight discovery. Both route profiles use the same list; those species
are suppressed throughout the private world, including passive individuals.
NPC list descendants are excluded. The policy/config hash is bound in SESSION,
and all 139 consumed ESM/ESP/omwaddon file hashes are checked before preparation.
Changing the frozen openmw.cfg or these files requires a new audited policy. This is a bounded
encounter audit, not complete scripted-spawn or neighboring-grid coverage.
The original rat's modified Fight was not recorded and is not inferred from its
base definition. Actor activation still queues the disable, so the change needs
runtime verification. Old captures are never requalified by it.
Combat/Pursue targets are watched per local update; any combat observed during the
recorded interval invalidates the route's combat qualification. Queued removal does
not guarantee that no mod callback ran before the actor was removed.

Smoke mode
----------
Generated smoke options can automatically start after two seconds in a stable,
unpaused exterior whose name matches smoke_expected_cell, acquire 1-60 seconds,
save/export, then quit. Host console COC setup happens before START and is excluded.
This validates recorder execution and artifact binding, not route repeatability,
combat preemption, candidate performance, or automatic replay.

Verification
------------
python tests/run_lua_tests.py --dll C:/OptimizedMW_Test/lua51.dll

The tests execute pure Lua and mocked installed API behavior using the installed
Lua 5.1 DLL: bounded JSON/records, preservation of user controls during acquisition,
ordered export, save acknowledgement/failure, exact new-slot identification, input
conflicts, stage/door/pause distinctions, and absence of fabricated catch-up samples.
Actual game execution and packaged-output validation are separate required checks.
