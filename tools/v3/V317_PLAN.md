# OpenMW Custom Build V3.17 — Lua/runtime hitch consolidation

Baseline: V3.16 Mode 88 behavior, branched from full-build commit `f31ea980fbf0b03278fc427ccb9c987fc70dde00`.

## Goals
- Reduce recurring ordinary-play Lua/runtime stalls without sacrificing script semantics.
- Integrate a deterministic Rubic0n-derived LuaJIT option and measure it against stock LuaJIT.
- Reduce OpenMW↔Lua boundary overhead and first-materialization work.
- Remove remaining diagnostic I/O periodicity from gameplay.
- Audit and cache safe work in the Lua→sound boundary without delaying normal sound start.

## Planned modes
- 90: V3.16 Mode 88-equivalent control, stock LuaJIT.
- 91: Rubic0n runtime only.
- 92: engine-side Lua/materialization optimizations, stock LuaJIT.
- 93: combined balanced candidate.
- 94: Mode 93 + aggressive V3.16 Mode 89 SFX predecode.

## Rubic0n policy
- Pin an exact Rubic0n source revision and identify it in package/CI metadata.
- Keep current OpenMW `content.lua` unless a real runtime compatibility requirement is proven.
- Start with conservative Lua-compatible GC/finalizer semantics. Do not enable Rubic0n's aggressive direct-finalizer/direct-free contracts until OpenMW/Sol userdata finalizers have been audited.
- Prefer source-built/pinned runtime integration over an opaque manually copied DLL.

## Engine-side Lua lane
- Cache immutable handler/subscriber/script metadata where semantics permit.
- Avoid constructing Lua userdata/event arguments for recipients that cannot consume the event.
- Pre-resolve immutable first-use materialization work (script VFS paths, module/package lookup metadata, dependency/config lookup) without executing mutable script state early.
- Preserve every legitimate callback and ordering guarantee.

## Diagnostics lane
- Consolidate V3 diagnostic output behind one writer queue/thread.
- No periodic gameplay-cadence disk flushes; flush at shutdown/explicit checkpoints.
- Lab diagnostics remain disabled for normal gaming modes.

## Audio/Lua boundary lane
- Audit `playAmbientSound`, `playSoundFile`, and related lookup/normalization paths.
- Cache immutable filename/VFS/record metadata where safe.
- Never add a blocking predecode/wait gate to the normal sound-start path.
- Ambient-start latency is a specific QA metric.

## Success criteria
- Lower p95/p99 and fewer >33.3 ms / >50 ms ordinary-play frames.
- Lower Lua timer/local-update/event/materialization tails.
- Reduced Dynamic Sounds/actor-event spikes where engine-side work is responsible.
- No Lua-mod compatibility regressions, no audio delay/regression, no renderer regression, no material VRAM increase.

## Deferred to V3.18
- Internal render scaling architecture, Native/Bilinear/NIS, native-resolution HUD/UI, mip-bias controls.
- Distant ObjectPaging meshoptimizer path with startup/idle cache construction and asynchronous persistence.
