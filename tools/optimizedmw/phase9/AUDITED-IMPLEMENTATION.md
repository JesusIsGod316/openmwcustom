# OptimizedMW Phase 9 audited candidate

This implements the engine work paused at Event 161. Its preparation parent is
`e017315a1b8ce5dce6f7156bf4ac8efe5bbab5af`, whose engine parent is the user-tested
`651edc7bc31be01c5351733f5ab800bb9a012e8d`. The shared archive remains the authority
for history and decisions, the ledger for benchmark evidence, and Git for source.
This candidate makes no performance promotion and does not alter VulkanMW.

## Implemented mechanisms

| Audited requirement | Implementation and regression evidence |
| --- | --- |
| Prove temporal lifetime before relaxing the canvas dependency | Exactly two acquired OSG 3.6.5 SceneView owners, immutable camera/resource inputs and successful-draw-only history; delayed native single/threaded pixels, repeated/skipped IDs, resize/restart and stale consumer tests. [Ownership proof](TEMPORAL-OWNERSHIP.md). |
| Prepare composite producer dependencies | Existing OpenMW ICO prepares diffuse/blendmap textures, the actual producer program variant, destination texture and FBO. Exact revision/context/GL object and program generation are checked again before baking. Actual GL tests invalidate images, programs, targets, contexts and producer passes. |
| Coordinate admission without starving content | One context/frame account charges preparation and baking, repeated traversals cannot replenish it, observed expensive cost informs admission, and one shared age allowance enables bounded progress. At most eight pending maps and 32 MiB; required terrain uses the complete original bake. Pixel tests retain first/subsequent layer blend/depth/order behavior and in-flight promotion. |
| Repair terrain leaf metadata | Ordinary geometry and TerrainDrawable's direct submission both clear and initialize pooled metadata at the actual cull seam. Tests alternate those routes through reused pools across main/refraction/shadow cameras. |
| Attribute resources without inventing upload identity | Cull/CPU producer catalog records effective inherited texture attributes and sampler roles with OVERRIDE/PROTECTED semantics. Callback-free PreparedTerrainTexture applies give compressed-upload hooks exact numeric object/image/revision/unit state. Uninstrumented or callback-managed applies remain unknown. Bounded catalog/drop tests and exact GL hook tests cover that distinction. |
| Repair GPU frame ownership and capture readers | Queries use draw State FrameStamp and camera; query-ring and writer losses are separate. Named-column readers ignore comment footers, reject incoherent history/input/extents and explicit loss, and preserve raw ZIPs before bounded reporting. Both real PowerShell hosts run the behavioral fixtures. |
| Advance DLSS prerequisites independently | Supported opaque rigid/CPU rig/morph surfaces produce immutable deformation motion with depth agreement, conservative fallback and memory bounds. A separate process proves one Windows GL/Vulkan RGBA8 sharing contract on the intended GPU. [Interop contract and evidence](interop/README.md). |

The dynamic geometry fixture also exposed a retained RigGeometry skin-to-skeleton
traversal overrun for a direct child with equal empty root names. The bounded
ordered-iterator fix is exercised by the actual Skeleton/RigGeometry test, with
separate cloned actor identities. This correctness fix applies to the control too.

## Controls and retained boundaries

`OPENMW_P9_TEMPORAL_OWNERSHIP`, `OPENMW_P9_COMPOSITE_PREPARE` and
`OPENMW_P9_DYNAMIC_MOTION` require an exact value of `1`. One public BAT exposes
the retained seven modes and five additional isolated/combined modes. Temporal
ownership versus original temporal mode uses the same diagnostic cost; composite
versus reference uses the common foundation. The shipped README defines the
comparison matrix.

The global actor/dynamic completion barrier, existing OSG threading model, normal
saves and quality policies remain in place. Unsupported ownership paths retain
the DYNAMIC canvas. The rejected actor static-resource prewarm remains unwired.
No glFinish, added production fence or forced download pretends to make admitted
GL calls preemptible. Preparation proves submission and exact resource validity,
not guaranteed residency, completion or absence of later stalls.

The resource catalog describes each effective texture attribute's first source
image. Multiple images, truncated labels/bindings or saturation produce explicit
uncovered counts. It is a diagnostic scope with selected extension hooks; direct
core GL and driver internals are not intercepted. Largest local labels are not
proof of duplicate allocation or of the actual timed upload.

`ConsumerFrame` is valid for its exact draw owner only. Its motion texture can be
rewritten by the next draw; future asynchronous external consumers require
separate slots retained through their retirement. Dynamic coverage still excludes
transparent/cutout/custom vertex effects, instancing, wind/particles and complete
first-person content. `denseDynamicMotion` and `dlss_ready` remain false. Jitter,
NGX/Streamline evaluation and frame generation remain off.

## Reproducible gates

The Windows helpers use the existing private MSVC/native dependency installation;
they do not install a new system SDK or change the normal game profile:

```powershell
./tools/optimizedmw/phase9/build-local.ps1 -Stage Configure -Interop
./tools/optimizedmw/phase9/build-local.ps1 -Stage Build
./tools/optimizedmw/phase9/build-local.ps1 -Stage Test
./tools/optimizedmw/phase9/launcher-tests.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/optimizedmw/phase9/launcher-tests.ps1
./tools/optimizedmw/phase9/build-game-local.ps1 -Stage Configure
./tools/optimizedmw/phase9/build-game-local.ps1 -Stage Build
./tools/optimizedmw/phase9/build-game-local.ps1 -Stage Test
./tools/optimizedmw/phase9/build-game-local.ps1 -Stage Install
```

Commit first, then reconfigure/build/test/install so executable revision metadata
matches the delivered source. Install refuses an uncommitted source or a different
configured revision. The `optimizedmw-phase9.yml` workflow independently runs
production integration compilation, native graphics tests, ASan/UBSan, retained
regressions, both launcher hosts, full Windows compilation and installed shader/
settings/identity checks. The Linux opaque-Win32 hardware gate is an explicit
skip; Windows hardware evidence is recorded separately and checksum-linked to
the precise fixture source. A software skip never proves shared-image pixels.

## Runtime acceptance remains a separate gate

First compare candidate reference against the old 651edc7b control with identical
assets, save, route, cap, resolution, power/thermal conditions and settings. Then
compare the same candidate executable's 2/8 temporal pair and 1/9 composite pair,
followed by combined mode 10. Keep cold resource runs separate from warm routes.
Use mode 11 to inspect supported moving geometry independently. Exercise resize,
fullscreen, cuts, cell transitions, water/shadows and first-person rendering.

Measure whole-frame median/p95/p99, >33/>50 ms tails and hitch clusters, queue
pressure/waits, later required stalls, completion age, RAM/adapter VRAM, latency
and visual content. Deep trace runs explain causes and cannot promote clean FPS.
The old approximately 15 ms dynamic-wait observation is not a recoverable budget.
Earlier nested CPU/GL envelopes are not additive and selected slow hooks cannot
establish how much work is outside all hooks. Wait migration, missing content,
partial maps, races, quality cuts or unbounded retention reject the candidate.
