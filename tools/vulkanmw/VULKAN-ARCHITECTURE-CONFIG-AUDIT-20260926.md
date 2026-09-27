# Vulkan architecture and configuration audit — 2026-09-26

## Verdict

Keep the persistent native-renderer direction, but revise the sequencing. Removing
OSG is a means, not the performance acceptance criterion. Do not wait for every
OSG importer/controller to disappear before fixing Vulkan command preparation,
lighting, shadows and post-processing. Conversely, do not remove compatibility
semantics just to make the renderer's profile look smaller.

This turn audits source and creates an isolated configuration/tool. It does **not**
implement the architectural repairs below or claim a frame-rate improvement.

Source: `vulkanmw/phase3c-native-runtime-substitution`, base
`336a4cf8668afa1a4f03069785421e9a5e2b74ef`, with pre-existing local changes preserved.
The primary Documents checkout is not the active Vulkan implementation.
Shared archive router/current-state/decision records were read, not modified.
Archive events 139–143 establish the native branch and its P1/P2 OpenGL ancestry;
the current checkout is newer than that archive's Phase 2 checkpoint. Event 144's
OpenGL P7 stack is a separate track, not code automatically present in VulkanMW.

## 1. Measurement and configuration findings

The previous same-executable investigation is in
`FRAME-PROFILE-INVESTIGATION-20260926.md`. Its approximately 68 ms Vulkan frame,
13.08 ms recording, 8.59 ms object capture, 8.97 ms combined actor work, 7.33 ms
population/inventory work, and 4.87 ms OSG update are useful diagnostic evidence.
They do not imply these costs are all removable or fully parallelizable.

**New qualification of the OpenGL comparison:** the archived run manifest hashes
the same normal `settings.cfg` inspected here (SHA256
`bd98f4245c2b8c5ee573f353f188089dbd7fd6e922e9f3f15e64353c82f6e5a1`). It requests
`render scale=0.90`, NIS and an OMWFX chain. Render scale/NIS are consumed by
`postprocessor.cpp::renderWidth/renderHeight` and `pingpongcanvas.cpp`, not by the
Vulkan world render-target setup. The two renderers do not execute the same
post-processing chain either. Thus the reported 68/18.145 ms ratio describes
those configurations, **not a proven equal-quality 3.75x API/architecture gap**.
Verify actual targets and active effects in the next matched comparison; this
qualification does not make the observed Vulkan CPU cost acceptable.

The benchmark helper also writes `vsync=false`, while this branch's registered
setting is `vsync mode`. The inherited normal value was already 0, so this is not
evidence that those runs were vsync-limited. It is a harness correctness defect to
avoid in future comparisons. No historical measurements were altered here.

The half-resolution Vulkan run remained around 70 ms while main GPU span fell
from about 33 to 11.5 ms. Shadows remained about 15.5 ms. This is strong evidence
of a current CPU/critical-path limit **and** meaningful GPU work that will matter
once CPU work is reduced. GPU spans include stalls; they are not shader-only
times, and must not be added to CPU wall-time scopes.

### Configuration contamination, without assuming causation

- The old diagnostic launcher protects writable data but includes the normal
  configuration directory in the live chain. It therefore inherits all normal
  optimization presets. Save isolation is not settings isolation.
- The current normal settings label themselves OpenGL P7 FULL STUTTER. Source
  searches show the later P3–P7 `optimizedmw` keys are absent from this branch.
  The package's actual decoded `defaults.bin` rejects 36 normal keys as unknown.
  This count includes later-branch controls, misplaced keys and typos; it does
  **not** mean 36 harmful features were running.
- Examples: `[Cells] object paging` is misplaced (registered under Terrain),
  `clusterd lighting` is misspelled/unregistered, and `;framerate limit` is a key,
  not a comment. The engine uses `#` comments. No normal file was repaired here.
- The default V3.6 profile is enabled and silently bundles RAM overdrive, Lua
  timer optimization, coarse OSG occlusion and far-shadow pruning. Omitting V3
  lines does not make a clean baseline. Explicitly disable the umbrella and
  select shared mechanisms independently (`components/settings/v36profile.hpp`).
- Many Vulkan environment controls use presence tests: `FLAG=0` still enables
  them. Remove inherited controls rather than zeroing them.

## 2. Existing optimizations: keep, bypass, replace

"Remove" below means remove from the **Vulkan execution path** after equivalent
behavior exists, not delete working OpenGL code from this dual-backend repository.

| Existing mechanism | Current source evidence / status | Vulkan decision |
| --- | --- | --- |
| Lua bytecode/dependency precompile, package-prototype reuse, idle timer fast path | `components/settings/v36profile.hpp`, V3 accessors and Lua consumers; not OpenGL command work | Keep independently selected. Preserve sandbox/event order. The excess synchronized Lua time needs attribution, not blanket Lua disabling. |
| Audio metadata/caches, asynchronous preload, physics/navigation workers | Shared engine services; unrelated to GL state sorting | Keep. Budget concurrency across services; do not give every subsystem twelve busy workers. |
| Focus cadence | Current default is registered `[V3] v3.19 focus cadence=2`; older archive said environment-only | Keep current source truth. Vulkan's deformation-aware intersection path in `renderingmanager.cpp::getIntersectionVisitor` intentionally evaluates rig/morph geometry. Investigate spatial broad phase and revision-bound pick data; do not disable the correction that fixed NPC targeting. |
| Overdrive and very long object/cache retention | `ramcache.hpp` treats presets as minimums; overdrive raises counts even if smaller values are configured | Replace implicit preset coupling with explicit budgets. New baseline uses normal retention, 12/20 preloaded cells and ordinary expiry. This is a diagnostic policy, not a proven best streaming policy; traversal/hot-return regressions must be measured. |
| GL-P1A/P1B host-pressure/speculative ownership | `engine.cpp` explicitly guards these with `!mUseVulkanRenderer` | Already excluded from Vulkan. Do not call disabling them a new speedup. Retain native `HostMemoryPolicy`, enabled on Vulkan unless its legacy-control environment variable is present. Host-pressure thresholds are not a total VRAM budget. |
| GL-P2 readiness split, ObjectPaging optimizer, V3 frontload/premerge/spatial batches | `v4scenerenderlifecycle.cpp` disables legacy terrain preload/frontload unless explicit controls reopen them; `scene.cpp` respects this | Much is already bypassed. Do not spend another rewrite removing an inactive path. Keep any remaining OSG asset/picking compatibility uses until traced. Native paging must schedule asset preparation/uploads, not rebuild legacy merged geometry. |
| V3 ICO compile pacing, completion/fairness governors, groundcover/PostFX warmup | `renderingmanager.cpp` constructs OSG ICO policy; V3 settings govern GL object compilation | Exclude optional knobs. Later replace any residual Vulkan-side setup/admission overhead with bounded native upload/pipeline preparation. Avoid pretending the absence of a GL context eliminates all CPU setup automatically. |
| GL-P3–P7 template prefetch, semantic premerge, display lists, normalized packets, shadow batching, compile scheduler, split terrain VBOs, parallel binding/terrain preparation | Present in the normal config and archive's GL track, absent from current Vulkan source/default definitions | Do not port GL mechanisms wholesale. Reuse the ownership principles where useful: immutable preparation, bounded jobs, cancellation, staged publication. Their config keys cannot activate missing implementation. |
| Old GL VRAM telemetry, residency sweep and far-cascade reuse/pruning | V3 settings/GL resource policies, not Vulkan allocator authority | Keep out of clean Vulkan settings. Use Vulkan memory-budget data, live/in-flight/cache accounting and completion-safe retirement. Any shadow approximation stays an explicit quality choice. |
| OSG CPU occlusion / coarse MSOC | OSG culler created from Camera settings; Vulkan has separate `NativeVisibility` and environment controls | Disable duplicate legacy MSOC in isolated baseline. Preserve native frustum/terrain-occlusion controls. Previous cohort had zero usable occluder triangles, so "enabled" was not evidence of useful occlusion. New LAND experiment remains off pending validation. |
| Native caches, persistent populations/draw streams, revision gates, instance reconciliation | `vsgruntimehost.cpp`, `pipelineinventory.hpp`, `persistentpipelinecache.hpp` | Keep and extend; they are not completed end-to-end change publication. Avoid whole-cell republish and root graph inventory scans for local changes. |
| Static instancing | `staticassetrealizer.cpp` already builds per-instance translation/rotation/scale arrays and sets `instanceCount` | Already implemented for supported populations. Audit actual batching coverage/fragmentation before proposing "add instancing" again. Indirect drawing and broader batching need their own coverage and cost evidence. |
| Parallel actor preparation / view recording | Existing bounded actor jobs; `ParallelRecordTask` has two helpers for independent command graphs | Keep safe ownership. This is not intra-main-view parallel recording. Split substantial opaque batches using per-frame/per-worker resources; preserve transparent order. Do not parallelize live OSG mutation or a tiny task followed by an immediate wait. |
| Frame resource lifetime | `frameresourcepool.hpp` already uses submission/completion watermarks and bounded versions | Keep the completion discipline. Extend accounting and eviction; do not substitute CPU frame age for GPU completion, or cache mutable configurators that retain whole retired scene generations. |
| OSG update-only world | `v4updateonlyviewer.hpp` runs CPU update/particle work and explicitly forbids GL presentation | Retire by semantic coverage, not global deletion. Controllers, attachments, effects, picking and auxiliary views still need correct owners. Saves do not require OSG rendering; loading an OSG-native mod asset still needs a supported importer/adapter. |

Historical constraints still matter: V3.19 static-instancing rejection was about
shader/correctness coverage, not a ban on native instancing. V3.23/24 MSOC parallel
experiments did not justify duplicate raster work or same-frame waits for tiny
jobs. Preserve per-worker ownership, generation checks and fail-open visibility.
Do not repeat those experiments under new names without changed conditions.

## 3. Revised implementation sequence and acceptance gates

User clarification during the audit: do not make repeated OpenGL comparisons a
prerequisite for Vulkan progress. Existing Vulkan attribution is sufficient to
choose substantive repairs. The historical comparison qualification above is
retained for accuracy, not as a request for another OpenGL benchmark.

### A. Establish one reproducible Vulkan baseline

Use the isolated profile and its exact executable/hash. Compare Vulkan repairs
with the preceding Vulkan version at the same effective quality and workload.
Track absolute frame time, low-percentile performance/stutter rate, CPU critical
path, GPU pass times and memory pressure. Verify effective render-target
dimensions, active effects, adapter and swapchain mode, not just config text.
Use OpenGL only when needed as a visual/behavioral reference, not a mandatory
performance gate. No further OpenGL run is requested by this audit.
The existing benchmark cannot simply receive this profile as `--user-config`:
the profile's ordered content directories are in its recorded command. A future
benchmark adapter must use that entire command, not discard the content chain.

Run fixed indoor/outdoor/actor-heavy scenes and a cell-transition/hot-return route.
Normal gameplay timing must not include capture/validation-layer overhead. Keep
instrumented runs separate. No requirement for the user to repeat the old broken
build. Where a repair has an independent switch, the same Vulkan executable can
provide the control. A configuration-policy change is recorded separately from
a renderer implementation improvement.

### B. Complete change-driven publication, not another cache around inspection

Native static objects, actor bindings and resident resources already exist.
Extend them: generational render identities, immutable mesh/material handles,
per-instance transforms/bounds, explicit pose/material/attachment dirty lists.
`staticpopulationproducer.hpp::flush` still calls `makeRecord(cell)` for dirty
cells; `vsgruntimehost.cpp` can reseal the root pipeline inventory. Replace those
with affected-slot/group updates and retained pipeline membership references.

Gate: unchanged buildings cause no evaluated capture, material rediscovery,
cell-wide record copies or global inventory scan. Actor movement changes only
its pose/transform streams; equipment changes rebuild only affected attachments.
Unsupported controllers/assets retain a reason-coded narrow fallback, counted
by time as well as objects. No silent dropped mod semantics.

### C. Fix command preparation and GPU scene work concurrently

- Record from stable draw packets with explicit pipeline/material grouping.
  Measure traversal, state binding, recording, driver calls and waits separately.
  Preserve existing instancing, reduce its fragmentation, then evaluate indirect
  drawing where it actually reduces CPU work. Do not assume bindless is required
  for first improvement; provide device-feature fallbacks.
- Worker-owned command/descriptor pools per in-flight frame; enough work per
  task, persistent bounded workers, no shared live OSG traversal. Reuse command
  buffers only when their recorded resource/state dependencies really permit it.
  Khronos specifically warns that too many tiny secondary buffers can lose
  performance: [command recording sample](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html).
- `legacymaterialshader.cpp` loops through the view's local lights for each lit
  fragment. Introduce conservative spatial light lists, then clustered/tiled
  lists where justified. The measured 90 lights are a reason to investigate,
  not proof that this loop alone explains the 33 ms GPU span. Lights without a
  finite authored influence cutoff must remain in a global list or use an
  explicitly approved approximation; do not silently truncate attenuation.
- Use view-specific shadow caster selection and stable shadow resource reuse.
  Main-camera invisibility cannot remove an object casting a visible shadow.
  Screen-resolution upscaling does not shrink fixed 2048 shadow maps.
- Share geometry and immutable materials across main/refraction/shadow views,
  not their visibility or mutable per-view data. Use conservative current bounds
  and fail open for missing/stale occlusion. GPU HZB is a later measured option,
  not a prerequisite for fixing CPU publication.

### D. GPU actor deformation with shared pose/picking contracts

`dynamicactorpreparation.hpp` still CPU-deforms and copies streams in the ordinary
route. Upload bind geometry once, publish pose palettes/morph weights, and skin/
morph on GPU for all relevant views. Maintain conservative animated bounds and
accurate CPU interaction results; never solve picking with a synchronous GPU
readback. Avoid doing full CPU skinning solely to rebuild bounds/picking every
frame and thereby retaining the original cost. Missing/disconnected body parts
are a release blocker, not an acceptable performance trade.

Gate: correct body/equipment/creature/morph/controller behavior, no duplicate
full-mesh CPU deformation for covered actors, no per-frame full stream upload.

### E. Bring post-processing forward as a parallel deliverable

The old roadmap postponed it too long. Vulkan startup explicitly sets
`mUsePostProcessing=false` in `postprocessor.cpp` when there is no GL context;
F2 consults that processor's enabled state. `nativepostprocess.hpp` currently
offers Copy/EdgeAA/Depth only and explicitly is not an OMWFX implementation.

Split effect parsing/options/Lua/F2 state from GL execution. Keep the OMWFX front
end and version-specific semantics; provide a Vulkan executor with HDR color,
depth conventions, normals, ordered passes, named/intermediate/history targets,
samplers, uniforms and resource transitions. Cache graph topology until effects
or target sizes change; do not rebuild a global pass graph every frame.

Use small reference effects first, then Rafael's real chains individually.
Unsupported effects get actionable diagnostics, not a blank F2 menu or silent
omission. Raw replacement GLSL tied to OpenGL is a separate compatibility class;
preserving NIF/ESP/Lua/save formats does not automatically translate arbitrary GL
shaders. The [OMWFX reference](https://openmw.readthedocs.io/en/openmw-0.49.0/reference/postprocessing/omwfx.html)
documents inputs such as depth/normals/previous-pass data; match the local engine
version rather than assume conventions are identical across OpenMW releases.

### F. Budgeted streaming and retirement

Separate required/live, GPU-in-flight, reusable cache and speculative prefetch
ownership; account CPU copies and GPU allocations separately. Reuse the existing
completion ring and native host-pressure safety. Add a Vulkan budget/headroom
policy for the 8 GB adapter, bounded staging buffers, and cancellation of stale
optional work. Preserve active resources; reclaim cache-only resources safely.
Retain useful raw NIF/texture/animation parsing without also retaining duplicate
render representations indefinitely. More aggressive turnover is not inherently
better: validate stutters, tail latency and repeated traversal as well as FPS.

## 4. DLSS: worthwhile, but not an immediate CPU fix

The local adapter inventory identifies an **RTX 5050 Laptop GPU** alongside the
AMD integrated GPU. Add DLSS Super Resolution as an optional Vulkan capability,
with an actual runtime support check and a non-DLSS path. No SDK was installed
or downloaded in this audit.

NVIDIA's current [Streamline DLSS guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
and [manual Vulkan integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md)
describe Vulkan integration. It needs render-resolution color, depth, motion
vectors, output-resolution color, camera/jitter/history handling and correct
resource states. The current Vulkan path does not have a complete temporal
motion-vector/upscaling integration. Camera motion alone is insufficient for
moving actors, morphs, particles and animated foliage.

Build a common temporal-rendering interface for motion, history reset, exposure
and resize. Compose UI at output resolution. Place effects according to their
input/output contracts; do not blindly run every OMWFX effect before or after
upscaling. Keep raw/native rendering as a correctness control. Check SDK
redistribution/licensing requirements before shipping proprietary components
with this GPL project; an optional loader is not by itself a legal conclusion.

DLSS may lower resolution-dependent GPU cost and enable heavier effects after
the CPU bottleneck is addressed. It does not remove scene capture, Lua/focus
costs or command recording, and does not inherently reduce fixed-resolution
shadow cost. Evaluate Frame Generation separately later; present rate is not
simulation rate or reduced base-frame latency. No FPS multiplier is promised.

## 5. What was created in this turn

- `tools/vulkanmw/vulkan-clean-settings.cfg`: explicit Vulkan baseline, no GL
  umbrella preset; preserves selected shared engine optimizations.
- `tools/vulkanmw/vulkan-profile.py`: prepare/verify/run utility. Reads the
  executable's actual base64 defaults, validates supported keys, snapshots the
  ordered content config without normal settings, resolves asset paths, keeps
  content replacement semantics, refuses ambiguous/unknown content options and
  autoloaded saves. No wholesale mod directory copies.
- `tools/vulkanmw/tests/test-vulkan-profile.py`: parser, selection, paths/order,
  environment isolation, generation, tamper/config-contamination refusal tests.
  Registered in the existing test CMake file without replacing other changes.
- This audit. No engine C++ or existing benchmark logic changed this turn.

Ready profile: `C:/VulkanMW-Clean-Profile/Start-Vulkan-Clean.cmd`.
Editable private settings: `C:/VulkanMW-Clean-Profile/settings.cfg`.
It reuses `C:/VulkanMW-FrameProfile-V2-Test/openmw.exe`, SHA256
`dc1aa17bb4c74ca3cc83311c3212928dfbf8363aa2c15aa61acbfe36a2300275`.
No new game build is being represented as a repaired renderer.

The profile retains 1080p, the existing 73744 view distance, 15000 groundcover
distance, three 2048 shadow maps and water refraction. It does not manufacture an
FPS win by reducing those values. Native visibility is enabled separately; legacy
MSOC is off. Backend-independent Lua compilation/timer optimizations remain
explicit. Optional unvalidated population/actor-stream/LAND/pose experiments
remain off. Native resource retention and retained-renderer fast paths remain.

No normal saves or Lua persistent state were copied. Normal asset directories
are read-only references; engine writes use private data. Input bindings were
copied. The original postfx chain is kept as reference text in `profile.json`,
not secretly activated. The native copy/AA/depth demo is not enabled as a fake
replacement. This is an isolated test profile, not a migration of existing saves.

The launcher strips inherited `OPENMW_`, `OSG`, `VSG`, `VK_`, `MESA_`, `__GL_` and
`__NV_` controls in the child environment, then applies the frozen Vulkan list.
It refuses changed executable/defaults/content snapshots or added package/content
settings. Each run records effective merged settings and stripped variable names.
It does not change registry implicit layers, external overlays or driver profiles;
those remain a separate profiling variable. Settings, not all machine state, are
isolated. Assets/shaders can change externally, so freeze mod versions for timing.

## Validation and limits

Profile unit tests: 11 passed; existing frame-report tests: 4 passed; existing
benchmark-control tests: 11 passed (26 Python tests total). CMake reconfiguration
and all three corresponding CTest registrations passed. `git diff --check`
passed with only existing LF/CRLF conversion warnings. Real-package prepare/
verification checks the actual defaults schema; all seven originally hashed
configuration/state files remain unchanged. Runtime game
launch/performance/mod compatibility are **not** established by those tests.
No engine C++ compilation was necessary for this tooling/configuration-only
change. Existing uncommitted engine work remains unpromoted. No commit or push.
Profile footprint at creation is about 124 KB; no executable/assets duplicated.

Next implementation gate: isolated Vulkan baseline plus local-change publication and
draw-preparation fixes, with the Vulkan postfx executor developed in parallel.
GPU light/shadow work must not wait for complete OSG removal. Promote only after
mechanism counters, image/interaction tests and stable/tail frame-time results
show a real improvement with preserved content/save semantics.
