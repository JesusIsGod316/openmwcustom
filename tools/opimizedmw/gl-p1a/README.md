# OpimizedMW GL-P1A: OpenGL host-pressure protection

Scope: implement event `evt.opimizedmw.gl_p1.research_audit.128` P1A only.
Base: `opimizedmw/gl-p0@4286850552993b15594b8cd36942965eb4854f7e`.
No world-renderer migration, P1B byte-accounting rewrite, P2 paging dependency
repair, cache TTL/count reset, prepared-instance enable, or gameplay change.

## Switch and control

Startup-only settings in `[Cells]`:

```ini
opimizedmw host pressure = true
opimizedmw host reserve mb = 0
opimizedmw commit reserve mb = 1024
```

The default is **false** pending Windows/gameplay control comparisons. Off
creates no monitor and does not query the OS or make reservations for OpenGL.
Keep the previous P0 executable as the external control; same-binary OFF vs ON
alone does not prove absence of always-on infrastructure regression. Do not use
`setx` or change normal saves. No toggle resets existing V3 retention settings.
The old Vulkan admission-bypass environment variable cannot disable opted-in
OpenGL protection. Vulkan's original policy and controls remain separate.

## Implemented

- Physical availability, current-process commit headroom, system commit
  headroom and Windows low-memory notification are independently evaluated.
  Private commit is diagnostic only; it is NOT physical RAM usage.
- At 32 GiB usable RAM the proposed auto physical bands are 4 GiB caution,
  2 GiB critical, 5 GiB recovery. Configured reserve overrides scale critical
  to one half and recovery to 1.25 times reserve. Commit defaults independently
  to 1 GiB caution, 256 MiB critical, 1.5 GiB recovery. These are initial test
  values, not measured optimum thresholds or total-process allocation caps.
- One invalid probe cannot hide another valid critical signal. Missing private
  counters cannot block recovery. Missing physical/commit coverage is Degraded:
  deny new optional cell work, but do not invent a destructive trim request.
- Five continuously sampled healthy seconds are required for recovery, then
  admission ramps at one new job/sample for five seconds before allowing four.
- One small sampling worker per enabled ResourceSystem isolates queries from
  long resource sweeps and loading jobs. Initialization seeds one sample before
  gameplay workers start. Runtime reads only try-lock/copy the published struct;
  they never call an OS query or wait behind it. No thread per cache/job.
- Each publication has a generation and sample-start timestamp. Contended reads,
  clock reversal or snapshots older than 3 seconds degrade admission. Queries
  execute outside the publication lock; shutdown wakes and joins the sampler.
- Existing coarse RAII admission is reused with an interim per-sample allowance:
  finished jobs do not refund credits against the same stale OS snapshot. Both
  the outstanding-job cap and the per-sample cap remain four. This deliberately
  conservative estimate can overcharge allocations already reflected in an OS
  sample. Exact peak/retained-output accounting is **P1B, not implemented here**.
- Existing pressure escape hatches stop optional cell preloads between assets,
  release completed optional preload owners and trim cache-only resources. P1A
  does not claim a globally time-bounded destructor/GL-release path or coverage
  of every terrain/derived allocation. Required terrain loading remains intact.

## Preservation

All edited preexisting files are recoverable at
`opimizedmw-gl-p0-precleanup-20260924`, whose bundle was restore-verified in P0.
P0's 228-line disabled shadow cleanup is untouched. Changes are opt-in additions
and a narrowly scoped admission-bypass correction, not mass dead-code deletion.

## Native validation

```sh
cmake -S tools/opimizedmw/gl-p1a -B build-p1a -DCMAKE_BUILD_TYPE=Release
cmake --build build-p1a --config Release
ctest --test-dir build-p1a -C Release --output-on-failure
```

The suite runs 38 P1A cases and 22 existing legacy cases. Windows tests compile
and invoke the real Windows API probe (no stubs); Linux tests exercise policy,
publication, stale reads, recovery and concurrency with synthetic inputs.
Sanitizers are supported with `-DOPIMIZEDMW_P1A_SANITIZERS=ON` on Clang/GCC.

`implementation.patch` and `prepare-source.py` are one-time connector transport:
CI materializes the ten existing-file edits, validates exact before/after Git
blobs, tests that source, and commits it on the P1A branch. Published source is
readable and directly buildable with no generator/harness dependency. The
transport patch is retained only as review/recovery evidence.

The targeted workflow is NOT a full OpenMW Windows build or runtime benchmark.
P1A can share the next full engine build with P1B when authorized. No FPS, RAM
reduction, full mod compatibility, or final-binary performance claim is made.

## API references

- https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/ns-sysinfoapi-memorystatusex
- https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-performance_information
- https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-querymemoryresourcenotification
- https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-creatememoryresourcenotification
