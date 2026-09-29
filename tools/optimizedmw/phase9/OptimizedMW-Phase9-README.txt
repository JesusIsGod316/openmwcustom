OptimizedMW Phase 9 - first draw-side/temporal test build

Extract the complete ZIP into a NEW folder. Use START-OptimizedMW-Test.bat.
Do not copy it over an older build. Your mods/saves stay in their normal locations.

PRIMARY TEST
1 REFERENCE: P8U1 COMBINED foundation, Phase 9 buffer/motion work off.
2 HITCH: identical foundation plus private rig/morph vertex-buffer refresh.
Use the same save, scene, route, shadows, grass, frame cap and power state.
The experiment is NOT promoted; report visual defects or worse frame pacing.

TEMPORAL TEST
3 TEMPORAL: camera/static RG16F motion inputs without replacing normal scene color.
4 COMBINED: HITCH plus those temporal inputs.
5 MOTION-VIEW: visualize camera/static motion. Moving actors/grass are NOT complete.
Jitter, DLSS, DLAA and frame generation are NOT implemented/selectable here.
Native rendering, bilinear/NIS and existing PostFX remain the ordinary outputs.

ATTRIBUTION ONLY
6 HITCH-TRACE and 7 REFERENCE-TRACE add bounded state/draw/private-buffer records.
Compare these with their untraced counterparts to quantify observer overhead.
CPU scopes can include driver waits; they are not GPU timers and must not be summed
with overlapping cull/draw/frame counters. Missing data never means zero time.

The shared foundation keeps P1/P2/P7, HIER-CULL/CULL-CPU, resource repair, Lua cache,
audio warming and shadow-setting consistency. LOD2 and inactive shadow proxies
remain off for these comparisons, not deleted. Shadows remain 3 x 2048 at 4096.
There are no lowered-quality shadow modes or global OSG threading changes.

PACKAGING
After the game exits, settings/environment are restored and a SHA256-verified RAW
ZIP is created BEFORE the optional offline report. The report has a 30-second
limit and cannot remove the raw ZIP. A verified enriched archive replaces it only
if successful. The ZIP is beside openmw.exe; a protected install folder falls back
to the profile parent under Documents/My Games/OpenMW/OptimizedMW-Phase9-Profiles.
The launcher selects the final ZIP in Explorer. Raw folders are retained.
Crashes can lose deferred engine CSVs; partial evidence is still zipped and clearly
marked in PROFILE-CAPTURE.json. Check TEST_MODE.txt and all capture-status files.
Do not assume that a successful ZIP or build proves complete/correct telemetry.

SCOPE
The VBO experiment refreshes existing eligible PRIVATE position/normal/tangent
storage, preserving the GL name, index buffers, CPU double buffering and OSG's
safe-point barrier. It does not use a fence-owned ring or prove GPU completion.
Requests are bounded at 4 MiB per buffer / 32 MiB per frame-context; driver memory
retention is a separate measurement. Cold/unsupported/over-budget data use OSG.
The temporal pass initially supports the normal mono view. Unsupported contexts,
invalid histories or missing resources retain normal rendering. Dense actor,
morph, wind and effect motion and the OpenGL/Vulkan-to-DLSS bridge are still future
work. There is no performance or gameplay-compatibility promotion from CI alone.
