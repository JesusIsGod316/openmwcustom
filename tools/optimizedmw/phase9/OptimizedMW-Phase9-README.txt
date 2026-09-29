OptimizedMW Phase 9 - root-cause capture and shared-static preparation

Extract into a NEW directory and use the one START-OptimizedMW-Test.bat.
Your normal saves/mods remain in their existing locations.

PRIMARY MENU
1 REFERENCE: same P8U1 donor/resource foundation; old HITCH1 refresh off.
2 OPTIMIZED: reference plus shared static rig/morph attribute/index precompile.
3 ROOT-CAUSE-TRACE: reference plus draw/state/selected GL-call attribution.
4 OPTIMIZED-TRACE: identical attribution plus static precompile.
Use 3 first for the recurring hitch investigation, then 4 on the same route.
Use untraced 1/2 for performance claims. Instrumentation has nonzero overhead.
The static-prewarm mechanism has NOT been promoted by gameplay measurements.

Type A for retained advanced controls: 5 TEMPORAL, 6 OPTIMIZED-TEMPORAL,
7 MOTION-VIEW, 8 old HITCH, 9 old HITCH-TRACE, 10 HITCH-TEMPORAL.
Temporal is still camera/static motion only, not correct dense actor/wind motion.
Jitter stays off. DLSS/DLAA/frame generation are NOT implemented or selectable.
No temporal input/rendered image behavior changed in this checkpoint.

ROOT-CAUSE EVIDENCE
p9-draw-phases.csv: slow leaf envelopes; matrices/state/draw/retire, camera,
StateSet, drawable identity, names and steady-clock start time.
*.frames.csv: ALL measured leaf contributions per frame, including cheap calls.
*.gl.csv: selected slow OSG extension-dispatch calls, phase, arguments and time.
*.gl-frames.csv and *.gl-totals.csv: sums/counts/maxima of selected calls.
*.renderer.csv: renderer-entry CPU envelope (can include queue wait). The last
observed leaf frame is not an assertion all outer work belongs to that frame.
Check all status files, ROOT-CAUSE-CAPTURE.json, TEST_MODE.txt and exit status.
Coverage/overflow failure is explicit. Direct core GL calls, renderstage work
outside leaves, swap and driver internals are not magically split by these timers.
Scopes overlap: do not sum outer/leaf/API times or subtract async GPU timings.
Trace attachment now occurs before Viewer::realize; unsupported startup fails
loudly instead of silently collecting zero visitors. Normal modes are unchanged.

OPTIMIZATION SCOPE
Static prewarm touches only shared STATIC_DRAW buffers using the existing ICO
context owner. No live pose evaluation, private VBO creation, display-list draw,
new context, synchronization-barrier removal or first-use completeness claim.
Per-backing 4 MiB / per-call 8 MiB admission bounds are not hard GL time limits.
Counters in p9-static-prewarm.txt identify engagement, not GPU completion.

CULLING AND QUALITY
Audit found existing whole-cell, per-object, paged-chunk and HIER-CULL coverage.
No duplicate hierarchy or speculative actor culling has been installed. Existing
main/shadow/reflection visibility rules, grass density and 3 x 2048 shadows at
4096 distance remain. LOD2 and opaque shadow proxies remain off, not deleted.

PACKAGING
On exit, normal settings/environment are restored, coverage status is recorded,
and a SHA256-verified RAW ZIP is created BEFORE the optional 30-second report.
Report failure cannot remove that raw ZIP. ZIP appears beside openmw.exe; a
protected folder falls back to the profile parent under Documents/My Games/OpenMW.
Partial/crashed/invalid traces are still preserved and clearly marked. Keep the
launcher open until it reports the final ZIP and selects it in Explorer.
