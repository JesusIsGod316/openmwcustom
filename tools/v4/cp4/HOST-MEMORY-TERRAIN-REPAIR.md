# CP4F combined host-memory and terrain repair

Base: `3de01e9b4804bcec940a9aef3f71b96035c5b25f` (includes the evaluated UV-sharing repair).
This is a source/test checkpoint. Performance improvement, memory convergence,
visual parity and the Excise Office transition require a new hardware capture.

## Substantive changes

### Useful caching with a pressure escape hatch

V3 overdrive/profile inputs, normal expiry, parsed-NIF retention, and ordinary
cache hits remain available. Vulkan additionally samples Windows physical
availability, process private commit, and process commit headroom at most once
per second. The monitor is independent of the diagnostic recorder; switching
recording off does not change resource policy. No extra thread or GPU wait is
created. Missing counters are invalid, not zero-byte memory emergencies.

For an ideal 32 GiB usable system, candidate soft thresholds are:

| Signal | Trim | Critical | Recovery condition |
| --- | --- | --- | --- |
| Available physical memory | below 4 GiB | below 1 GiB | at least 5 GiB |
| Process private commit | at least 24 GiB | at least 28 GiB | below 23 GiB |
| Available process commit | below 4 GiB | below 1 GiB | at least 5 GiB |

Thresholds scale with reported usable physical memory, not a hard-coded machine
name or installed-RAM label. Recovery requires five continuous seconds above
all margins. These are conservative starting values, not measured optimal
settings and **not a hard ceiling on total process allocation**. Required live
content is not dropped to enforce a number.

Under pressure, speculative cell/model preloads stop between assets and new
optional preloads/prepared clones are not admitted. In-progress work is asked
to abort without a new main-thread wait. Completed optional preload owners are
transferred to the existing cache-maintenance worker for destruction. A partial
aborted preload is not kept as a complete warm cell after pressure recovers.

Maintenance drops optional retaining owners before templates/shared state and
images. Generic cached objects must have no external strong references and must
not have been requested since the preceding pressure scan. Prepared instances
are unpublished optional clones; active objects, checked-out collision shapes,
CPU image storage referenced elsewhere and in-flight Vulkan resources are not
stripped. Scans/removals are count-limited (4096 entries and 128/512 removals per
generic cache per maintenance pass). Oldest completed cell owners have a 4/16
removal budget, plus up to 16 incomplete completed jobs. Destructor cost and the
existing shared-state prune are not wall-time bounded; no zero-hitch guarantee.
Pressure eviction counters remain distinct from ordinary expiry counters.

The disabled control avoids OS queries/clock sampling and preserves the original
retention rules. The new fixed cache metadata, including a recent-use bit, still
exists in that executable; this is not a claim of zero instruction/size overhead.

### Do not prepare unused legacy terrain on the Vulkan route

The old frontload flag only skipped extra future grids. Current-grid
`Terrain::World::preload` and its loading-screen wait still ran, as did terrain
caching by speculative cell preloads. Vulkan already has an independent native
LAND-to-terrain producer and groundcover publication route.

A renderer lifecycle capability now suppresses those **legacy render-preload**
requests by default on Vulkan. OpenGL retains them. Required `Scene::loadCell`
LAND/heightfield physics, navigation, scripts, cell activation, native current
terrain preparation, native surrounding-terrain publication and groundcover
selection remain unchanged. Legacy paged-ref omission is not used without the
legacy paging render preparation; canonical gameplay objects remain present.

This removes the legacy job which dominated the recorded transition, but does
not establish the resulting transition time: native work, required object
insertion and memory pressure remain and may shift the critical path.

## Independent same-executable controls

Variables use presence semantics, matching existing CP4 controls (unset enables
the new policy; even a string value `0` requests the legacy control).

- `OPENMW_V4_LEGACY_HOST_RETENTION_CONTROL`: original host-retention behavior.
- `OPENMW_V4_LEGACY_TERRAIN_PRELOAD_CONTROL`: original current-grid legacy preload.
- Existing `OPENMW_V4_LEGACY_TERRAIN_FRONTLOAD`: legacy preload plus the existing
  configured extra-grid frontload behavior.

All existing actor, effect, static-publication, water-composition, and renderer
selection controls remain. No Lua-worker, shader, texture-quality, draw-distance,
actor-plan-reuse or simulation policy is changed. Do not combine control changes
in the middle of a capture. Do not treat missing measurement as performance zero.

## Packaged launcher repair

`Start-CP4F-Test.cmd` is included beside the EXE. It invokes the same corrected
Python launch path as `run-memory-qc.ps1`, without requiring a PowerShell policy
change. It uses the package's identity and EXE hash manifest rather than a stale
hard-coded e26 hash. Normal configuration is included once with
`--replace=config`. No empty `--load-savegame` value is passed. Any inherited
autoload setting is refused before launch. The private final layer explicitly
requests Vulkan, 1920x1080 windowed, and the chosen diagnostic mode; other
configuration and engine comparison switches remain inherited.

Normal saves are never copied or selected. Reports distinguish an early config
abort from a successful exit code, and preserve evidence if report generation
fails. The archive contains logs/reports/manifest, not assets, original configs,
Lua storage or saves. The separate e26-pinned starter is not needed for this
package and should not be used with a different executable.

## Focused validation and remaining work

Authored tests cover pressure thresholds, invalid counters, commit-vs-physical
accounting, recovery hysteresis, concurrent one-sample monitoring, disabled
controls, cache refcount protection, recently used entries, bounded scan cursors
(including duplicate pool keys), owner-to-image reclamation, out-of-lock
reentrant destruction, concurrent cache use, and separation from ordinary expiry.

Source guards pin required LAND/physics and native terrain/groundcover producer
bodies. These are source guards, not substitutes for real engine compilation or
pixel/gameplay tests. The early native Windows/Qt header project includes the
real OS probe and policy tests, retaining the prior macro-boundary protection.

**Not implemented in this batch:** particle/effect draw batching or elimination
of the large remaining dynamic-capture/submission cost; a full byte-attributed
cache-utility optimizer; native character-preview, water, sky or material-pixel
repairs. Most exterior effects were already reused in the observed capture;
naive particle batching would risk transparency ordering and view-dependent
billboards. The UV repair and its failure provenance remain intact.

One combined Windows production gate follows focused checks. No additional
user test of the old crash-only executable is needed as a prerequisite.
