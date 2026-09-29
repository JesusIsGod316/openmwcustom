# Phase 9 root-cause checkpoint: culling audit

Source audited: bb4876a6ea287745c8f535f5525c5381a631a24c.
This checkpoint does not change visibility, culling thresholds, scene membership,
shadow quality, or update traversal. It does not transplant a Vulkan renderer.

## Existing coverage (production code, not a proposed optimization)

* `SceneOcclusionCallback` initializes terrain MSOC for the main scene traversal
  and ends its active frame afterward. Interior/secondary camera rules remain.
* `CellOcclusionCallback` already rejects a whole cell against terrain depth,
  then separates large occluder owners from small objects. The second pass tests
  individual remaining objects against terrain plus building occluders. Doors
  retain `skipOcclusion`; admitted occluders are not tested against their own
  inserted proxy. Ordinary world objects are not an uncullled population.
* `PagedOccluderCallback` already rejects whole prepared object-paging chunks
  before descending. Its meshes also contribute occlusion for other content.
  Hiding a group is not equivalent to removing its proxy contribution.
* `GroundcoverBatch`/Groundcover HIER-CULL already provide a retained spatial
  subdivision, conservative animated bounds, per-view rejection and existing
  coarse MSOC callbacks. Adding another hierarchy would retest the same grass.
* `MWShadowTechnique` has cascade-specific selection and the retained far-caster
  pruning policy. Main-camera invisibility is NOT a valid reason to remove a
  caster from a shadow view, or an object from a reflection/refraction view.

## Disposition of the proposed extra opaque hierarchy

Do not insert it in this build. Cell roots already have a two-pass occluder-owner
protocol; interposing arbitrary groups changes which nodes are classified and
which bounds are rasterized. Much eligible static content is already paged and
merged. There is no new capture establishing a large uncovered static population
or a net win that exceeds another hierarchy's traversal/maintenance cost.

A later hierarchy must replace a measured part of this work, retain per-view
semantics and conservative bounds, and handle create/move/disable/retire events.
It must not reparent gameplay nodes, reinterpret an animated group as a static
opaque occluder, reuse old visibility to omit newly visible content, or drop
visible objects under a draw budget. The same considerations rule out a quick
actor-bin transplant. No global sorter, rejected instancing mode, or reduced
shadow-resolution/reuse mode has been enabled.

## Substantive optimization in this checkpoint

Both RigGeometry and MorphGeometry previously had empty compileGLObjects hooks.
The independent static-prewarm switch now prepares only shared STATIC_DRAW
attribute and element-buffer backing found on their internal geometry. It does
not compile private pose buffers, evaluate animation, generate display lists, or
change index/vertex data. Existing ICO state/program/texture preparation is not
replaced or claimed newly implemented. This is not complete actor readiness.

The helper deduplicates backing pointers per request, skips already-clean GL
objects, restricts one backing to 4 MiB and a call to 8 MiB, and retains stock
fallback for mutable/unsupported/oversized backing. These are admission bounds,
not a driver-allocation budget or a hard execution deadline. It is an experiment
requiring hardware confirmation, particularly that counters show useful work.

## Evidence and interpretation limits

The prior native-only draw test installed its cull visitor after explicitly
stopping threading. The real game instead attempted installation in PostProcessor
after Engine's Viewer::realize, explaining installed=0. The new production
startup test deliberately attaches scene data AFTER realize, with the tracer
already installed, runs DrawThreadPerContext, requires live leaves and actual
OSG GL-dispatch activity, and rejects late installation.

Tracing now retains all-leaf per-frame totals, selected slow leaf records with
camera/state/object identity, selected GL call timings plus per-frame aggregates,
and renderer-entry CPU envelopes. A renderer envelope can include its queue wait;
it is not the OSG Draw traversal timer. Nested scopes overlap and cannot be added.
CPU timings around GL APIs identify a blocked call, not the driver-internal cause.
A fast upload does not disprove a deferred stall elsewhere. Likewise CPU/GPU timers
cannot simply be subtracted to establish driver CPU cost. Direct core GL dispatch
is not intercepted by the extension hooks and remains an explicitly unknown
subpart of its measured leaf/state envelope. These captures do not prove that
all hitches have the same mechanism.
