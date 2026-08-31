# V3.20 CP6 safe LuaJIT optimizer runtime

## Source identity

- Stable-P0 dependency chain: `OpenMW/openmw-deps@2026-02-24` ->
  `OpenMW/openmw-deps-build@8306cb2556b92c9bb891b13f4769889838fe3c10` ->
  `vcpkg@6d332a018c433fad20822ff4b536e4ccdc3413bd`.
- Exact stock LuaJIT source: `LuaJIT/LuaJIT@707c12bf00dafdfd3899b1a6c36435dbbf6c7022`.
- Audited Rubic0n source: `DreamWeave-MP/rubic0n@f3ee18afcc8c029dc7e13c8c69fe119dbcbc4c50`.

## Whitelist

The checked-in patch series contains only:

1. guarded exact-metatable specialization derived from Rubic0n `6a297e50`;
2. global-environment specialization derived from `3a90fa111`;
3. improved allocation sinking plus hardening from `b5a1981e`, `94435a0`,
   `1eb6111`, and `cb1449f`;
4. the corresponding metatable, global-environment, and sinking regression tests.

The metatable commit's only conflict was an unrelated Rubic0n/OpenResty
`compile-unpack` function absent from exact stock. It was deliberately not
imported. The other whitelisted source patches applied without semantic edits.

## Blacklist

The series contains no Rubic0n paged/small-object allocator, GC pacing change,
direct C finalizer, sweep-time finalizer discovery, non-resurrection contract,
batched finalizer, sandbox bypass, expanded builtin, userdata C-index
specialization, `content.lua`, or full-runtime merge.

## Local proof

On Linux, the exact stock baseline plus the series completed a clean LuaJIT
build. All extracted Lua bodies from `t/mtspec.t` (5), `t/globalspec.t` (8), and
`t/sink.t` (6) passed, followed by a hot-loop smoke test. The Perl TAP wrapper
was unavailable because its external Test::Base/IPC::Run3 dependencies are not
part of stock LuaJIT; the Lua test payloads themselves were executed directly.

This proves source composition and focused semantics, not OpenMW runtime
performance or long-gameplay compatibility. The Windows packaging job must
build this exact series beside the stock DLL, retain stock at the game root,
record hashes, and expose same-settings stock/safe-JIT A/B launcher modes.
