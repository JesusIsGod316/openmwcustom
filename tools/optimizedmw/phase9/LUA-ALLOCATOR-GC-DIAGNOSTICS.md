# Lua allocator and GC evidence

Deep adds read-only evidence through the existing HitchTrace capture. Normal
does not sample allocator clocks or counters. The production allocator still
uses the original `free`/`realloc`, memory limit, ownership and bookkeeping;
the production GC worker still makes one `LUA_GCSTEP` with the original request.
No Lua VM, instruction hook, JIT mode, step multiplier or GC scheduling changes.

`lua.call` retains its four existing inclusive logical-request counters. Its
nested `lua.alloc.service`, `lua.alloc.request_small` and
`lua.alloc.request_large` summarize the same enclosing callback operation.
These are wall-only envelopes with the existing 0.25 ms threshold; their wall
durations must not be added or interpreted as allocator-only time. Nested
calls also nest inclusive counters, so parent and child counter values must
not be added. An enclosing engine/container/event/timer scope identifies the
callback; allocation rows repeat the available container and configured path.

| Phase | Metric | Meaning |
| --- | --- | --- |
| `lua.alloc.service` | `service_ns` | Sum of steady-clock nanoseconds bracketing actual CRT `free`/`realloc`, including failed CRT requests. Includes clock measurement cost; excludes tracking bookkeeping, policy refusals and logging. Wall time includes any scheduling delay during service. |
| | `service_calls`, `service_failures` | Actual CRT invocations and failed nonzero realloc results. |
| | `policy_blocked` | Refused requests under the existing Lua memory limit, before calling CRT. |
| `lua.alloc.request_small` | `req_le64`, `req_65_256`, `req_257_1024`, `req_1025_16384` | Attempt counts grouped by requested nonzero `nsize` logical bytes. |
| `lua.alloc.request_large` | `req_16385_1048576`, `req_gt1048576` | Remaining attempted nonzero request counts. |
| | `free_requests` | Zero-size requests, including any null-pointer free. |
| | `tracking_available` | Whether this VM actually has the tracking allocator. Zero means unavailable, rather than zero allocation cost. |
| `lua.gc.step_metrics` | `requested_steps` | Nonnegative requested step amount; `asset` preserves the exact signed argument. Engine GC requests are positive. |
| | `cycle_completed` | Exactly whether this explicit `LUA_GCSTEP` returned 1. |
| | `heap_before_bytes`, `heap_after_bytes` | VM-reported heap bytes surrounding that step, not committed/resident OS memory. |
| `lua.gc.observation` | identity | `unavailable_without_vm_sampler`: machine-readable automatic-GC coverage status. One retained record per VM. |
| | `automatic_gc_observed`, `vm_sampler_enabled` | Both zero. Neither heap shrinkage nor free requests prove automatic GC pauses. |

Histogram counts include denied and failed attempts; existing growth/shrink
byte counters include successful requests only. A null-pointer request's
`osize` can be a Lua type tag and is still normalized to zero as before.
Allocator counters cover all VM requests within a callback, including any
automatic VM GC allocation/free activity. They do not distinguish why the VM
requested those operations, or prove the duration of automatic GC inside it.

The pinned Windows SDK identifies LuaJIT 2.1.1767980792, vcpkg 2026-01-09,
source commit `707c12bf00dafdfd3899b1a6c36435dbbf6c7022`. Its DLL exports the
genuine `luaJIT_profile_start/stop/dumpstack` API. The
[official profiler documentation](https://luajit.org/ext_profiler.html)
defines sample state `G` as GC, and warns that callback delivery may follow
the sampled VM state. The
[pinned implementation](https://raw.githubusercontent.com/LuaJIT/LuaJIT/707c12bf00dafdfd3899b1a6c36435dbbf6c7022/src/lj_profile.c)
uses one global profiler: start can replace a profiler in the same VM, public
API lacks an active-owner query, stop resets profiler mode and flushes JIT
traces, and Windows starts a timer thread with `timeBeginPeriod`. These effects
can change the workload being diagnosed, so this release does not start it.
Existing ETW CPU stack samples may provide separate automatic-GC evidence
where matching symbols identify VM GC functions. That coverage requires
actual matching samples/symbols; DLL or PDB presence alone does not establish it.

`verify-lua-allocator-gc.py COMPONENTS_TESTS OUTPUT_DIRECTORY` runs Normal,
Deep and isolated native-allocator fallback processes. Real production tests
exercise allocation bucket boundaries, type-tag handling, grow/shrink data
preservation, memory-limit denial preserving the old pointer, exception paths,
VM-returned cycle completion and a real 2 ms userdata finalizer. The validator
checks metric units/relationships, nesting, coverage status and capture loss.
It neither injects allocator failures nor forces hot trace-row retention.

The compiled tests work with configured Lua or LuaJIT. The build being
qualified determines which VM was actually tested; one LuaJIT run is not
evidence that a separate plain-Lua build passed.
