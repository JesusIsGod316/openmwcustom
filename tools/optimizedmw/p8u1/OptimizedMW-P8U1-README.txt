OptimizedMW P8U1 - switchable resource repair and upstream donors

Extract this complete build into a NEW folder. Use only
START-OptimizedMW-Test.bat. The inherited P8G4 shader preflight is intentional:
this package keeps the same deployed PBR groundcover shader basis.

Start with 1 REFERENCE, then 2 COMBINED on the same save and route.
REFERENCE is the P8G4 CULL-CPU rendering configuration with all new features OFF.
Every mode uses a common 96 MiB sound head-cache budget, including REFERENCE.
Advanced isolation: 3 RESOURCE-REPAIR, 4 LUA-CACHE, 5 SOUND-WARM,
6 SHADOW-SETTINGS. One launcher, no separate BAT for each experiment.

RESOURCE-REPAIR: canonical file-backed terrain textures, exact revision/context
precompile reuse, and cooperative background composite-map preparation.
Required composites still complete. Normal drawing and the OSG synchronization
barrier remain intact. No hard GL-call deadline, no shared-context upload worker,
no guarantee that every resource is GPU-ready before use, no missing-terrain trick.

LUA-CACHE: weak object/cell userdata reuse with separate local/global/state keys
and reset on world teardown. Scripts and save formats are not rewritten.

SOUND-WARM: bounded loading-time discovery and background cached-byte warming.
The current predecoded-SFX path remains. Warming does not retain a playback
FFmpeg decoder or add an active-cell scan every frame. Shutdown logs report a
queue snapshot; pending work may be cancelled. No performance benefit is assumed.

SHADOW-SETTINGS: opt-in runtime distance/fade/resolution consistency for shadows
that were enabled at startup. Existing captured uniforms are not mutated; changed
map dimensions create new camera/texture attachments. Does not add a new settings
GUI or implement runtime cascade-count/caster-policy changes. It is not a proven
flicker or walking-stutter fix. Normal test quality stays 3 cascades / 2048 / 4096,
far mode 3 / 5 px pruning, with no resolution cut or temporal reuse.

LOD2 and static shadow batching remain off in the primary comparisons. Their
existing source is preserved; LOD2 tail performance remains workload-confounded.

DLSS: reconstructed temporal source and its CPU/RG16F shader tests are preserved
in this branch. The GAME does NOT yet have DLSS, DLAA, dense dynamic motion or a
Vulkan/NGX integration. No frame-generation work is included. No fake DLSS mode.

Quit normally so deferred frame records and post-run reports are written. Upload
the resulting profile ZIP. The launcher backs up/restores settings.cfg and the
inherited OPENMW environment. An abrupt process termination can lose in-memory
records. Inspect capture-status files before treating any result as complete.
The report includes all frames and separately ordinary frames under 100 ms;
it does not automatically select the historically matched exterior route.

The resource counters count preparation calls, not transfer bytes, physical
unique resources, GPU completion, or proven causes of a stutter. A shorter CPU
stage can expose a longer wait elsewhere; judge complete frames and queue progress.
This is an experimental build, not a performance-promoted release.
