OptimizedMW Phase 9 - root-cause telemetry + DLSS temporal-input continuation

Extract into a NEW directory and use the one START-OptimizedMW-Test.bat.
Your normal saves/mods remain in their existing locations.

SAFE TEST MATRIX
1 REFERENCE: unchanged P8U1/Phase 9 foundation.
2 TEMPORAL-INPUTS: camera/static RG16F motion plus consumer-input telemetry.
3 ROOT-CAUSE-TRACE: reference plus draw/state/selected-GL/composite attribution.
4 TEMPORAL-TRACE: modes 2 and 3 together for one diagnostic capture.
5 MOTION-VIEW: visualize the generated camera/static motion field.
Type A for retained old HITCH1 diagnostic controls: 6 HITCH, 7 HITCH-TRACE.

The static actor-resource prewarm experiment that crashed OPTIMIZED-TRACE is not
selectable and is no longer wired into RigGeometry/MorphGeometry compile paths.
Do not compare traced-mode FPS against normal gameplay: tracing has overhead.

ROOT-CAUSE EVIDENCE
p9-draw-phases.csv records slow leaf matrices/state/draw/retire timing plus
camera bucket, drawable/StateSet identity, node-path hash, nearest named owner,
geometry size and the two largest Texture2D image identities for slow state work.
Its status file also aggregates SceneCam, RefractionCamera, ReflectionCamera,
ShadowCamera and TerrainCompositeMapCamera CPU envelopes across all leaves.

p9-terrain-composite.csv directly measures TerrainCompositeMapRenderer work:
queue state, required maps, composite drawables, FBO setup, State::apply, draw
submission and cooperative yields. TerrainCompositeMapRenderer and its camera
now have explicit identities. p9-gpu-passes.csv uses existing nonblocking GPU
timestamp queries for named camera/pass timing.

The selected GL trace now records up to eight integral call arguments. Compressed
texture uploads therefore retain width/height/image-size fields where supplied by
the OSG extension dispatch. p9-draw-phases.csv.gl-last.csv records each context's
last normal-exit hook breadcrumb. An in-flight breadcrumb also remains in process
memory if the driver crashes inside a wrapped call, for crash-dump inspection.
This is NOT interception of every core OpenGL call or NVIDIA driver internals.

DLSS INPUT PROGRESS
Phase 9 now exposes a consumer-ready temporal frame containing the RG16F motion
texture, current/previous/inverse view-projection matrices, reset/history state,
jitter metadata and render/output extents. p9-temporal-inputs.csv records whether
scene color, depth, motion, extents and this matrix contract are present.

Dense dynamic motion for actors/morphs/grass is still false, so dlss_ready remains
false by design. Scene jitter remains OFF and no NGX/Streamline evaluation is
performed yet. Normal PostFX/NIS/presentation are unchanged. This checkpoint is
progress toward DLSS/DLAA input correctness, not a working DLSS release. Frame
generation remains out of scope.

PACKAGING
On exit the launcher restores settings/environment, records capture validity and
creates a SHA256-verified RAW ZIP before the bounded offline report. Crashed or
partial runs are still archived and explicitly marked rather than discarded.
