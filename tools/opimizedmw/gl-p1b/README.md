# OpimizedMW GL-P1B: speculative memory and owner reclamation

This is an opt-in, OpenGL-only source implementation on GL-P1A
`8a5310014b32f6dfa8f80c14a0e5d1aaed26216e`. It does not reset accepted
retention durations, pool counts, shader behavior, save formats, or Mode151.
The Vulkan world renderer and its legacy policy remain parked and unchanged.

## Controls (startup only)

```ini
[Cells]
opimizedmw host pressure = true
opimizedmw speculative budget = true
opimizedmw transient budget mb = 1024
```

Both booleans default false. P1B requires P1A; a contradictory startup setting
is diagnosed rather than silently enabling a feature. The transient limit is
an optional *future reservation* budget, not a cap on process memory. P1A's
physical and independent commit headroom reserves still apply. P1A alone
retains its prior conservative per-sample admission behavior.

## Implemented

- A shared ledger with move-only concurrent-job and stage reservations. DDS,
  PNG and TGA headers inform image peak estimates; NIF stream length informs
  parsing estimates. Nested template/instance/terrain preparation uses explicit
  fallback estimates. Sizes are inspected only on optional workers.
- Registered image payload bytes are known; wrapper/template output sizes can
  be estimated or unknown. Shared image backing and cache aliases use one
  charge. Tickets live with retaining cache entries, not with an OSG node's
  user data. Job completion does not release surviving output tickets.
- Every OS sample carries the cumulative registered-growth watermark read
  BEFORE its query. Later registered growth is charged against the older
  sample; releasing an owner does not fabricate immediate OS free capacity.
  Fresh samples reconcile registered growth. This is not exact accounting of
  an allocator, driver, or partially materialized third-party decoder.
- Normal jobs use concurrency and byte admission rather than four new jobs
  per second. The one-per-sample recovery ramp is preserved. Optional
  duplicate immutable requests defer instead of waiting; demand does not wait
  behind the optional claim. VFS generation and path distinguish claims.
- Cache hits distinguish demand from optional preparation. These counters are
  resource-private, and do not add per-scene metadata or a scenegraph census.
- Budget deferrals are not asset errors. Image callbacks, template loading,
  and error-marker fallback rethrow the dedicated deferral. Incomplete cell
  jobs retry normally; non-NIF plugin model prefetch is conservatively skipped
  because arbitrary third-party exception handling is not guaranteed. Required
  loading continues through the existing path.
- Completed retired cell owners transfer into a bounded maintenance queue.
  Transfer clears the caller reference while publication is locked, so a fast
  consumer cannot hand final destruction back to the frame thread. Running
  tasks retain ownership until done; cancellation is cooperative.
- Cache clear/remove/replace releases occur outside cache locks. Normal expiry,
  pressure pruning, instance pools, and shared state participate in one
  cooperative maintenance budget, with cursors and rotating manager selection.
  Retiring owners precede templates/images within each maintenance cycle.
- The release queue retains both owner-count and estimated-byte charges while
  a destructor is executing; it permits one oversized owner when empty to
  avoid starvation. A full queue stops new speculation instead of freeing
  graphs on the main thread. Retired map entries do not defeat the healthy
  minimum-cell-retention floor.

## Boundaries and acceptance

The current implementation is **not a hard allocation/OOM guarantee**. A
third-party decoder can exceed an estimate; ordinary demand allocation, Lua,
allocator slack, driver memory and live scene references are not globally
intercepted. Nested fallback estimates deliberately can overreserve. The byte
ledger labels known, estimated and unknown ownership separately. Removed
cache ownership is not proof of physical or GPU memory release. Shared
payload mutation outside these cache lifecycles is not remeasured on every
frame. No CPU image arrays are stripped and no global GPU wait is introduced.

The separate `TerrainPreloadItem` can become a required dependency of
`syncTerrainLoad`. It is deliberately NOT aborted by optional byte admission;
P1B covers cell-owned optional terrain preparation, while this mixed
required/optional terrain route remains the P2 readiness/dependency boundary.
Its cache results participate in configured resource accounting, but its
entire in-flight allocation is not covered by an optional job reservation.

Maintenance limits are cooperative between safe units (2 ms, 4096 scans,
64 releases and 64 MiB charged release bytes per ordinary pass). A single
large destructor or GL driver call is not preemptible. Shared payload byte
estimates and the 16-owner/512-MiB retirement queue are not total GPU budgets.
CPU destruction continues to use OSG's existing context-owned GL lifetimes.

Targeted tests exercise the actual production ledger/policy and real OSG
cache code, including reentrant destruction, concurrent transfer, byte and
sample reconciliation, aliasing, expiry cursors and pressure recovery. They
are not gameplay performance or mod-compatibility acceptance. Full Windows
compile/link/install and runtime comparison are distinct gates.

Required runtime comparison: previous P0 executable versus new P1-disabled
control, then same executable P1 enabled in healthy and bounded-pressure
conditions. P2 remains fixed for RAM attribution. Use normal diagnostics-off
performance runs, isolated save copies and matching content/settings. No new
FPS or measured RAM reduction is claimed by source publication.

## Build and archive provenance

The native QC workflow uses an allowlisted, hash-checked, one-time source
transport (`implementation.patch.gz` and `source-manifest.json`). It publishes
an ordinary Git commit containing the materialized C++ source. This is NOT a
runtime/build patch harness; final engine builds use the published source as
is. The transport remains readable by gzip decompression for archival review.
The full-engine workflow runs on a separately triggered build branch at the
same published SHA, never on the unmaterialized staging tree.

The pre-P1B source is recoverable from the preserved P1A Git branch and P0
bundle plus P1A source-and-validation archive. The small old shared-state
wrapper is relocated into `sharedstatecache.hpp`, not discarded as a feature.
No other dead-code cleanup is bundled into this functional change.
