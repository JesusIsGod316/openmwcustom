# CP4F runtime ownership diagnostics

This is an optional, observational facility compiled into the optimized engine.
It does not disable RAM overdrive, trim caches, change expiry, change renderer
selection, force GPU completion, alter actor/effect reuse, or load a save.
The existing actor-plan, effect-bounds, full-publication and water controls remain
available. Keep this distinct from a future pressure-aware cache policy.

## Manual use (Windows)

Use the helper beside the packaged `openmw.exe`:

```powershell
.\run-memory-qc.ps1 -UserConfig 'C:\Users\LSCha\Documents\My Games\OpenMW'
```

`-Executable`, `-EvidenceRoot`, `-PythonExecutable`, `-SourceHead` can override
locations/provenance. Python 3.11+ is required for reporting/packaging, not for the
engine's recorder. The helper waits for natural exit and never kills the game.
Normal content configuration is inherited, but writable config/logs **and
user-data/saves/screenshots** are isolated. It never copies or loads regular saves.
The command line explicitly disables inherited save autoload, skip-menu/new-game
autostart and startup console scripts. Choose **New Game** manually. Keep the same content, settings, 1920x1080 windowed
profile and New Game -> ship -> Seyda Neen -> Excise Office route for comparison.
The helper does not silently change resolution, renderer or cache settings.
Check the game's logged configuration paths and renderer before interpreting it.

Modes are `-Diagnostics standard` (default), `focused`, and `off`. These are
same-executable observation controls, **not cache-policy controls**. Off also
disables this helper's gameplay recorder, but unrelated inherited profiler
variables remain explicit in the manifest. Do not call an off run a clean
benchmark when another profiler or frame cap is active.

Direct engine use sets `OPENMW_RUNTIME_DIAGNOSTICS=standard` or `focused` and
`OPENMW_RUNTIME_DIAGNOSTICS_FILE=<writable path>/runtime.jsonl`. Set existing
`OPENMW_GAMEPLAY_DIAGNOSTICS=1` and its FILE variable to retain stage records too.
With no runtime diagnostic variable, normal gameplay is unchanged. Focused mode
adds existing actor/geometry fingerprints and full allocation graph inspection;
standard omits those expensive checks. Focused is NOT GPU readback or a per-asset
filter. Baseline old gameplay-only diagnostic behavior remains available.

After exit, the helper writes `report.md/json` and `memory-report.md/json`, then
a ZIP of the logs, reports and manifest. Paths and mod identities can appear in
these files; review before sharing. **No saves, asset files, Lua storage, original
config copies or crash dumps are included in the automatic ZIP.** An exact crash
dump may still be needed separately. A running/interrupted capture can be parsed:

```powershell
python .\gameplay-diagnostics.py report '<evidence directory>'
python .\runtime-diagnostics.py '<evidence directory>'
```

## Implemented measurements and meaning

- Windows process private commit, working set, peak working set, cumulative page
  faults; available/total physical memory; system commit and commit limit. API
  availability flags are separate from numeric zero. Page faults are not labelled
  hard faults. The writer samples these about once per second even while the
  engine's main thread is blocked. Non-Windows builds report unavailability.
- Configured cache inputs and effective V3.6/overdrive policy, preload limits,
  expiry, parsed-NIF retention, shape/prepared-instance limits. This shows profile
  overrides; it does not reconstruct per-setting file-origin history.
- Existing resource-manager cache counters and bounded, borrowed payload census
  during normal resource sweeps (not a newly forced sweep). Images are measured
  by their existing CPU mip payload. NIF capacity covers the file/record-pointer
  vectors and selected geometry/UV/triangle/strip arrays, NOT all records,
  strings, keyframes or allocator overhead. Other opaque caches are explicitly
  unmeasured. Shared payloads count once **within each cache census**, not once
  per alias; different caches/layers are not globally additive.
- Hits/lookups/expiry and expirations without a post-insertion cache hit; lifetime
  and interval hit ratios. Refcount classification includes other cache aliases
  and cannot prove active gameplay use or unique ownership. Cache expiry age is
  based on the owner's existing last-usage policy, not instrumented last draw.
  Cache misses have inclusive read/parse/decode wall times, not measured savings
  per hit. Cache clearing/removal is not misreported as expiry.
- Collision/prepared-instance pools and cell preload counts. Terrain job IDs,
  queue/work times and main-thread wait dependencies. Job addresses are local to
  a capture and may be reused; use timestamps too. Inclusive timings overlap.
- Neutral mesh vector capacities, frame/effect-contract mesh capacities,
  actor/effect version counts, writable/in-flight versions, graph-root retirement
  counts and oldest last-use frame. Snapshots occur **before** normal completion
  collection; completed roots awaiting that collection can be normal. Writable
  versions retained for reuse are not automatically leaked. These are not full
  byte-accurate lifetimes for every Vulkan/allocator allocation.
- Driver-provided Vulkan per-heap budget/usage estimates when supported, heap
  flags/size, and VSG device pool reserved/available bytes. No additional waits.
  CPU payload sizes are not physical GPU allocation sizes. Pool reservation and
  heap usage overlap; budgets can change with other processes.
- The existing exact effect reuse predicate now returns its first mismatch
  reason (material, texture identity/binding, topology, layouts, etc.). The
  boolean decision is unchanged. Cumulative reason counts and bounded examples
  expose why rebuilding happens. Float material alpha deltas use IEEE754 float32
  bit patterns; color/source/binding change flags are not full material dumps.
- Rate-limited preview redraw, auxiliary target presence, external MyGUI texture
  publication and unresolved alias observations. An image existing or a target
  being active does NOT prove rendered pixels or full producer/consumer parity.

## Bounds and observer cost

Eight thread-assigned shards, 128 fixed records each, occupy approximately
5.3 MiB total. More than eight producer threads share shards. Producers use
try-lock/copy only; full/busy shards drop records and increment a counter.
Thread assignment is lazy; recorder initialization allocates the ring and starts
one writer once. That one-time cost is included in recorded producer time.

Each record has at most 16 fields and bounded names/values. Clipping is explicit.
Each output sink is capped at 64 MiB plus at most one record and final markers.
The analyzer bounds record size, owner cardinality, open operations and examples;
it reads incrementally rather than materializing the whole trace. Ordinary game
logs are read line-by-line. Existing gameplay data is constrained by this sink
when the new mode is enabled; its older parser is not a general unlimited-trace
processor.

OS/census probes are sampled; cache census stops after 32,768 entries and emits
partial coverage. It allocates a bounded temporary identity set while holding a
cache lock and therefore can delay a competing lookup. Its cost is reported,
not claimed free. Fixed diagnostic fields (including an eight-byte reuse counter
per generic cache entry) remain in the binary even with recording off.

Periodic and shutdown records expose dropped/clipped counts and producer/writer
wall time. These times overlap across threads and with census probes; do not add
them to frame time. Producer time covers the recorder, not all caller-side legacy
string formatting or the one-time NIF capacity scan. Writer time excludes idle sleep and the final footer/flush.
Validate overhead with same-binary Off/Standard controls. A synthetic queue test
is not a measured gameplay overhead result.

The writer flushes completed batches, not every gameplay hook. This trades
bounded recent-record loss on abrupt failure for lower disturbance. No report,
ZIP, heap allocation or forced synchronization is attempted by a crash handler.
The normal fatal log path remains the immediate failure breadcrumb.

## Explicit remaining coverage

This is the first useful ownership/pressure slice, not a whole-process allocator
profiler. Complete model/scene/collision/controller/Lua allocation bytes and all
third-party/driver ownership, exact cache-value saved-work estimates, a pressure-
aware eviction controller, GPU timestamps, pipeline/texture pixel parity and
asset-filtered deep tracing remain future work. Do not call an unexplained
private-commit delta a proven leak. Do not add RAM/VRAM/payload/pool layers.

## Verification

`run-runtime-diagnostics-tests.sh` compiles real-OSG CPU fixtures under strict
warnings, runs Off/Standard/Focused controls, and parses their output. Optional
`OPENMW_UV_TEST_SANITIZERS=1` adds AddressSanitizer/UndefinedBehaviorSanitizer.
Tests verify read-only cache/pool observation, exact prior effect reuse decisions,
bounded capture/JSON, inactive mode and report coverage. Production CMake adds
three headless CTest cases under `OPENMW_V4_BUILD_RECOVERY_TESTS`.

Related build repair: the real-NIF compile-only semantic adapter target now gets
`OPENMW_ENABLE_V4_VULKAN_RUNTIME=1`, matching its Vulkan source list and the
production declarations. This repairs the verified missing `GlobalMap` native
API compile failure in run 35696837972 without altering OpenGL-only targets.

Platform references: Microsoft PROCESS_MEMORY_COUNTERS_EX, GlobalMemoryStatusEx,
GetPerformanceInfo; Khronos VkPhysicalDeviceMemoryBudgetPropertiesEXT. These APIs
report distinct scopes, not a universal heap ownership breakdown.
