# V3.20 effective-source map and checkpoint contract

## Foundation identity

- Repository: `JesusIsGod316/openmwcustom`
- Branch point: `v3.19-p0-stable-gaming`
- Foundation commit: `253fbec5358fb2b677fbebf68ed8746040420458`
- V3.20 integration branch: `v3.20-cpu-gameplay-pack`
- Stable rollback: the foundation commit above; V3.20 must not rewrite or force-push the stable branch.

The checked-in repository is a patch-generator tree. The authoritative P0 runtime
source is produced by `tools/v3/apply_diagnostic_harness_v319.py`, which executes
the V3.18 harness and then, in order:

1. `apply_v319_focus_cadence.py`
2. `apply_v319_runtime_modes.py`
3. `apply_v319_binary_identity.py`
4. `apply_v319_stable_gaming.py`

A clean preflight generation from the foundation commit modified or generated 92
paths and completed every V3.19 generated-source invariant. Its source-oracle
artifacts were:

- `V3-applied-source.patch` SHA-256:
  `9b636f1d5f3f3f7d6a4b175662f6de7b89dc438d28e072cfae442e6f2dab966b`
- `V3-applied-source-stat.txt` SHA-256:
  `b156e2eed10728ebb845563922b7929267a9abbe18b5d1b62d64f56b13889863`

These hashes identify the local effective-source oracle only. They are not a
runtime artifact or a substitute for a Windows build checksum.

## Existing promoted behavior

### Focus cadence

- Generated runtime hook: `apps/openmw/engine.cpp`, immediately before
  `mWorld->updateFocusObject()`.
- Registered setting: `components/settings/categories/cells.hpp`,
  `[V3] v3.19 focus cadence`, clamped to 1..3.
- Default: `files/settings-default.cfg`, cadence 2.
- Lab override: `OPENMW_V319_FOCUS_CADENCE`.
- Safety contract: GUI mode always refreshes each frame; activation/input object
  queries are untouched.

### Engine-to-Lua fast paths

- `components/lua/scriptscontainer.hpp` provides
  `mightHaveEngineHandlers()`. An unloaded container returns true so lazy
  materialization and handler-registration semantics are preserved.
- `apps/openmw/mwlua/globalscripts.hpp` and `localscripts.hpp` expose handler
  presence checks.
- `apps/openmw/mwlua/engineevents.cpp` gates the V3.17 fast paths with
  `OPENMW_V317_LUA_OPT`.
- `apps/openmw/mwlua/soundbindings.cpp` uses bounded thread-local identifier/path
  caches only: 4,096 entries and 512-byte maximum keys. It does not retain
  `SoundBuffer` or OpenAL objects.

### Scene templates

`components/resource/scenemanager.cpp::SceneManager::getTemplate()` already
checks the native object cache before loading and records `scene_template_miss`
only on a real cache miss. V3.20 must not add a redundant or persistent negative
cache. Lane D remains conditional until a distinct repeated positive lookup cost
is proven.

### Per-frame world invariants

`apps/openmw/mwworld/worldimp.cpp::World::update()` owns weather, navigator,
player, world-scene, rendering, sound-listener, spell-preload, and post-load
navigation work. `updateSoundListener()` must remain normal-rate. Object-update
suppression or traversal pruning must use authoritative dirty/generation state
and must not defer input, activation, focus correctness, streaming, audio-listener
updates, or cell-transition work.

## V3.20 lane map

| Lane | Intended hook | Initial disposition | Required proof |
|---|---|---|---|
| A: focus/cadence | `engine.cpp`; cells settings/defaults; launcher | First implementation checkpoint | Exact P0 control, bounded latency, semantic forcing, attempted/executed/skipped/forced counters |
| B: engine-to-Lua | scripts container, global/local scripts, engine events | Extend validated V3.17 path | No lost lazy registration or handler dispatch; per-event hit/skip counters |
| C: Dynamic Sounds | Lua sound bindings and engine-owned context queries | Conditional | Reduce repeated boundary/query work without editing the user mod or changing sound timing |
| D: scene-template cache | resource scene manager | Dormant unless a distinct positive-lookup cost is proven | Positive-only, bounded, generation-safe; never negative-cache unknown/unloaded assets |
| E: object updates | world/scene update scheduling | Experimental | Authoritative dirtiness, bounded deferral, normal-rate player/input/audio/streaming behavior |
| F: traversal pruning | renderer/scene traversal | Experimental | Provably irrelevant nodes only; no shader/material/shadow semantic changes |
| G: state sorting | existing render-bin/state ordering | Disabled by default | Stable ordering and visual parity before timing claims |
| H: Rubic0n-derived JIT | exact stable LuaJIT baseline, after source audit | Late conditional lane | Stock ABI/GC/finalizers/content.lua; isolated optimizer commits and same-build A/B |

## Hard rejection boundaries

V3.20 must not introduce static instancing, shader substitution, material or PBR
state merging, lighting/shadow-state sharing, arbitrary `StateSet` equivalence or
deduplication, persistent negative caches, the full Rubic0n runtime, Rubic0n's
paged allocator, altered GC/finalizer/direct-free semantics, or Rubic0n
`content.lua`.

## Checkpoint protocol

Each checkpoint must be a coherent, reviewable commit on the V3.20 branch. Before
moving to the next checkpoint it must pass its local invariants, `git diff
--check`, and any relevant generation/build checks. Then append an EVENT to the
Shared Project Context Archive and publish an exact `resume_action`. Runtime or
benchmark evidence belongs in the Benchmark & Log Ledger only after a real build
or gameplay run exists.

If work stops partway through a lane, create a clearly labeled partial checkpoint
only when the tree is coherent and its defaults remain stable-safe. Never leave
an enabled half-implementation as the documented resume point.

## CP0 conclusion and next action

The effective P0 source and generator ownership are resolved. CP1 should add a
V3.20 harness layered over V3.19 and implement Lane A through generator scripts,
with P0 fixed-cadence behavior as the exact control. Adaptive focus work must not
be enabled until authoritative forcing signals and bounded maximum staleness are
defined and tested.
