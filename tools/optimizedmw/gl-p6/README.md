# OptimizedMW GL-P6 stutter residency pack

Parent: optimizedmw/gl-p4r-scheduler-repair@4f0598cf558554bf1d94820013a4ec41670cfd5e

P6 treats the remaining 30-60 ms OpenGL calls as a residency problem rather than another scheduler-tuning problem.

This first switchable pack establishes safe prerequisites before any shared-context GL ownership transfer:
- producer urgency/deadline metadata;
- heavy background quarantine;
- byte-tier texture/drawable prediction;
- cold terrain prior independent of the old P5 heavy lane;
- immutable terrain vertex/VBO reuse across stitching variants;
- optional OSG PBO texture staging;
- benchmark-only all-frame/hitch/swap telemetry.

Normal gameplay telemetry is off. The P6 launcher explicitly enables benchmark telemetry, while a normal openmw.exe launch sets none of those environment variables.

The shared-context worker remains a follow-up after this matrix tells us which quarantined calls are genuinely required before visibility. It will require explicit publication/fence ownership so background GL work cannot race a live drawable.

## First Windows gate repair

Run 36252863827 failed at components/terrain/chunkmanager.cpp because P6 urgency selection referenced activeGrid inside createChunk without carrying that value through the private createChunk boundary. The repair passes activeGrid from getChunk into createChunk and the P6 source contract now asserts declaration, definition, and call-site wiring.


## P6 hardware matrix and PBO rejection

Hardware test of 4a53dcfda5fd0515317f0cb37d8a3c8b90d2163e isolated a runtime crash to the PBO experiment:
- modes 1-4 exited 0;
- P6-QUARANTINE-PBO and P6-FULL both exited 1;
- PBO staging was the only experimental switch common to both crashes and absent from the four clean runs.

The PBO path is rejected. It mutated shared osg::Image BufferObject ownership during paging through StateToCompile::_assignPBOToImages, which is not an acceptable ownership model for OpenMW's shared image/cache workload. P6R removes that path rather than hiding it behind a default-off switch.

Clean-route evidence also showed:
- Stage-2: p95 23.62 ms, >33.3 ms 1.89%, >50 ms 0.53%, queue end 4;
- P6 quarantine: p95 19.42 ms, >33.3 ms 1.54%, >50 ms 0.79%, queue end 4;
- strict quarantine: p95 19.79 ms but queue end 122, proving postponement alone is not a residency solution;
- terrain vertex reuse did not improve the extreme tail enough to promote.

P6R replaces the two PBO launcher modes with:
- P6-TERRAIN-RESOURCE-PHASES: TerrainDrawable pass textures/programs become native CompileTextureOp/CompileProgramOp entries so the size-tier model can see them instead of hiding them in generic StateAttribute work.
- P6-TERRAIN-VBO-SPLIT: additionally gives position, normal, and color streams independent VBOs and explicit buffer compile operations before final geometry realization.

The benchmark launcher now copies any fresh openmw-crash*.dmp into the profile ZIP when OpenMW exits non-zero.


## Direct renderingTraversals attribution

P6R benchmarks now instantiate a benchmark-only P6InstrumentedViewer when OPENMW_P6_TRAVERSAL_FILE is present. Normal gameplay still constructs the stock osgViewer::Viewer.

The instrumented viewer preserves the OpenSceneGraph 3.6.5 renderingTraversals control flow and records one compact row per benchmark frame with:
- context query and window status;
- optional scene-stat traversal;
- pager begin/end signaling;
- scene bound recomputation;
- camera query;
- start-render barrier;
- main-thread cull;
- no-graphics-thread context operations;
- end-render-dispatch barrier wait;
- main-thread swap path;
- dynamic-draw completion wait;
- context release;
- residual/other time, context/camera counts, and active OSG threading model.

This is attribution, not a threading-model experiment. No OSG threading mode, barrier policy, or runtime synchronization semantics are intentionally changed. The existing graphics-context swap callback remains a separate event stream.
