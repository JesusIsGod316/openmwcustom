# VulkanMW work and plan audit — 2026-09-27

## Verdict

The user is right to reject the current outcome. Recent work contains a real,
scene-specific publication improvement and useful correctness infrastructure,
but it has not delivered acceptable Vulkan performance or demonstrated the
requested rendering/mod/save compatibility. The execution process has failed
to turn individual repairs into a consistently tested, progressively faster
candidate. More phase completion, passing fixture counts, or shader compilation
must not be presented as evidence that the performance goal is approaching.

This is a focused source/evidence/plan audit, not a claim to have inspected every
line of OpenMW. No game was launched, renderer code changed, or new performance
measurement made during this audit.

## State and authorities

- Active source root: `C:/Users/LSCha/.codex/worktrees/vulkanmw-phase3c-metadata/OpenMW custom Build`.
- Branch: `vulkanmw/phase3c-native-runtime-substitution`.
- HEAD: `336a4cf8668afa1a4f03069785421e9a5e2b74ef`.
- At audit start: 42 modified tracked files and 39 untracked files, including
  runtime code, fixtures, benchmark tools and reports. HEAD alone identifies
  none of the recent experimental binaries. All existing work was preserved.
- Current local executable: `build/phase3c-engine/RelWithDebInfo/openmw.exe`,
  SHA256 `a8b6a380239ae80c13a917bce4467138f1d480c993cb49c4b8c7aad6d6260737`.
  Its project configuration uses MaxSpeed optimization and the non-debug CRT.
  No dirty C/C++/shader source file inspected had a newer modification time than
  this executable; timestamps alone do not establish reproducible provenance.
  The measured publication/tile results below belong to other binaries.
- Shared archive router, current-state, decisions and benchmark ledger were
  consulted read-only. Events 139–143 preserve native producer, compatibility,
  control-path and promotion boundaries. Their Phase 2 state predates this dirty
  Phase 3C checkout. The fetched router did not expose a literal
  `AI_CONTEXT_CONTROL_BLOCK`; linked current-state/decision records supplied
  the relevant controls. Old Vulkan-pause records are superseded sequencing,
  not a reason to stop this user-authorized project.
- Local manifests, console result lines and current source take precedence over
  recollections. No shared archive or memory file was edited.

## 1. High priority: successful changes were not a consistent baseline

The publication cohort used settings-isolated profiles and enabled three repairs
together. The later admission and specular/LAND cohorts used the older diagnostic
launcher, inherited the normal OpenMW config directory, and omitted all three
publication switches. This is directly recorded in their `manifest.json`
commands and controls. The particle/tile cohorts **did** retain those switches;
it would be inaccurate to say every subsequent test discarded the improvement.

Within-cohort comparisons remain useful. Cross-cohort numbers cannot be summed
or treated as a progression of the same candidate. In particular, the measured
admission gain cannot be added to the publication gain without a combined run.
Returning to an older control can be appropriate for attribution, but that must
be paired with an integration test and labelled clearly to the user.

Verified examples (same executable within each row; arithmetic mean of run means):

| Work | Control mean ms | Candidate mean ms | Conclusion |
| --- | ---: | ---: | --- |
| Group publication + chunk transactions + resident inventories | 68.5090 | 61.7107 | Real 6.7982 ms / 9.92% fixed-scene gain; insufficient overall |
| Cached object admission, without publication trio | 68.0331 | 66.7840 | 1.2491 ms signal; smaller capture reduction; combined result untested |
| Tile masks within radius-faded lighting, with publication trio | 60.3086 | 60.5853 | Main GPU span improves ~2.8 ms; no whole-frame gain demonstrated |
| Exact-zero-specular guard, without publication trio | 67.3084 | 67.4163 | No gain; prototype already removed from source |
| LAND occluders | Nearest control 66.8244 | Single on-run 67.7125 | No promotion evidence; increased CPU visibility cost |

Primary evidence directories:

- `C:/VulkanMW-Publication-V2-Test/Benchmarks/publication-{0-control,1-combined,2-combined,3-control}`;
  binary `fb312a5cdbed5e6a8494ab8487dc4b7bfc02d24c94dbd5acee31ee5e9bd64aa9`.
  All four console-result means were reread and match their manifests.
  Their current settings files also hash identically to one another. Runtime
  serialization changed their bytes from the recorded creation hash; do not
  confuse the post-run file with the original configuration snapshot.
- `C:/VulkanMW-Admission-Test/Benchmarks/admission-*`;
  binary `2d928d6aba72c38aa9a32c1e1ac5f6d2348cacf145ccdd8a485ab10c15542770`.
- `C:/VulkanMW-Particle-Split-Test/Benchmarks/tilevalue-*`;
  binary `60bcbf790e3881fd35e6a0ab3b1f2f33995072f8d7e5e95900ba204746cc9728`.
- `C:/VulkanMW-Specular-Test/Benchmarks/specular-*` and `land-depth-on`;
  binary `04e7768b7747e4a15a7689eb30112c28ee5030a3fd21148e7c57d902fcbc3468`.

Action: one versioned, settings-isolated candidate profile must carry the selected
repairs. Keep independent controls, but report candidate-level results after
integration. Preserve the old harness/results as historical evidence; stop using
that harness as the default progression test.

## 2. High priority: the persistent producer remains an inspection path

`apps/openmw/mwrender/v4enginerenderbridge.cpp:557` still calls
`rendering.forEachAnimation`. Cached admission skips some work only after
entering this per-object scan. Supported animated objects then reach
`V4PersistentObject::publish` at line 729.

`apps/openmw/mwrender/v4persistentobject.hpp:62` checks every retained node,
child edge and controller chain; line 83 starts another all-binding loop.
Known revision changes avoid some expensive recapture, but line 163 reconstructs
effective inherited state for changed material/UV bindings. Unsupported cases
still enter capture visitors (`v4enginerenderbridge.cpp:765–778`).

These are useful transitional caches. They are not the promised update-driven
native producer. Renaming them or adding another cache around admission cannot
remove the measured ~8 ms non-actor capture region.

Action: classify/register objects at load time into static, directly updated,
and reason-coded fallback sets. Engine movement/controller/equipment/unload
operations must enqueue changes to persistent slots. Known supported content
must leave the broad inspection loop. Keep unknown mutation guards on the
fallback objects, with correctness-preserving invalidation when ownership or
topology changes. Mod authors must not supply new notifications.

## 3. High priority: native animation is still followed by CPU mesh work

`components/render/backend/vsg/dynamicactorpreparation.hpp:54–78` looks up the
transform, evaluates the draw plan and calls `RenderCore::deformMesh`; the
ordinary route also copies the mesh payload before replacing its deformed
streams. `vsgruntimehost.cpp:1129–1135` then updates resident CPU vertex arrays.
Parallel preparation still joins before publication. Native skeleton sampling
does not remove this CPU deformation/upload path or all OSG update obligations.

The reference profile attributed ~9 ms to actor capture + preparation + stream
updates combined. Those measurements describe the old cohort, not a guaranteed
9 ms saving in a new one. The recent admission repair does not address this work.

Action: build one complete actor route: retained bind geometry, native pose/morph
streams, GPU deformation across main/shadow/water views, attachments and bounds.
Retire the corresponding CPU rendering work for covered actors. Preserve
animation events and gameplay. Picking needs a spatial broad phase and exact
candidate evaluation where needed; recreating every deformed mesh for picking
would retain much of the original cost. Disconnected body parts must have a
reproducing integration fixture before this route is presented as compatible.

## 4. High priority: recording was measured but insufficiently acted upon

`parallelrecordtask.hpp:34–57` parallelizes independent command graphs and joins
them. It does not divide a large main graph into substantial parallel batches.
In publication combined run 1, existing profile evidence gives graph order 0
13.3165 ms and order -3 4.8301 ms. The critical graph remains expensive; adding
workers to short side graphs cannot remove it. The recording envelope includes
the join and driver/recording work; it is not a pure parallel CPU-work budget.

After the ~13 ms recording bottleneck was known, the exact-zero shader probe and
additional occluder probe had low demonstrated leverage on total throughput.
Their controls and rejection records were sound, but they should not have
dominated the next performance iteration.

Action: use the existing timing split plus a bounded CPU/driver trace to locate
the dominant graph's traversal, binding, command API, allocation and wait costs.
Then record from stable, view-specific draw packets with retained resource
dependencies. Use substantial opaque batches and worker-owned frame pools where
the trace justifies it. Preserve transparency order and view ownership. Reuse
command buffers only while their recorded dependencies remain valid. This is a
specific missing measurement before a high-impact mechanism, not justification
for another telemetry-only milestone.

[Khronos's command-buffer sample](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)
supports per-thread/per-frame pool ownership and warns that many small secondary
buffers can worsen performance. It does not establish a speedup for this engine.

## 5. High priority: occlusion currently arrives too late for major CPU savings

`vsgruntimehost.cpp:2489–2497` synchronizes static resources, population visibility
and dynamic actors before the main native occlusion update. Producer capture
has already happened before entering this renderer. Therefore this occlusion
stage cannot retroactively save that CPU work. Some other frustum checks already
exist; this is not a claim that every visibility test is late or absent.

The LAND probe added ~0.87 ms CPU visibility while reducing the sampled main GPU
span by ~0.59 ms against the nearest control. It rejected roughly 16 of ~600
candidates and does not establish a useful shadow/water saving. Do not promote it
just because it demonstrably culls objects.

Action: use current conservative bounds and per-view render eligibility ahead of
render-only preparation. Continue gameplay and necessary animation semantics.
Use separate caster/reflection/refraction eligibility; main-camera occlusion
must never globally disable an object. Keep the present costly LAND option off
unless representative scenes demonstrate net benefit. HZB is an implementation
candidate after the scheduling contract is correct, not a guaranteed fix.

## 6. High priority: the plan incorrectly exempted the neutral data layer

The previous plan said RenderWorld/FrameRenderState was not the source of the
observed gap. That statement was too categorical. The publication repair itself
removed repeated neutral-world copying/publication and consumer inventory work.
`components/rendercore/updatebatch.hpp:181–190` still copies and validates the
world on the general transaction route; fast chunk/light paths cover only some
operation classes. This may matter during mixed updates and streaming, even
where stationary chunk updates are now cheap. Its current transition cost has
not been quantified, so it is a risk to measure, not a newly proven bottleneck.

Keep stable handles, semantic separation and atomic publication. Audit the cost
of their implementations: touched-record transactions, shared immutable payloads,
change lists, generation-safe consumers and bounded retirement. Avoid another
whole-world representation or snapshot copy merely to cross an interface.
No subsystem earns a performance exemption from being labelled native.

## 7. Release blocker: post-processing support is incomplete at the data contract

Current code provides real Vulkan effect execution and an F2 entry path, but:

- `omwfx.cpp:379` supplies black for scene normals and distortion. Effects may
  compile and execute with invalid inputs. OpenMW's documented normal sampler
  represents normalized world-space normals, not a placeholder.
- `omwfx.cpp:247–248` rejects authored blending.
- `omwfx.cpp:432–435` exports the current Vulkan projection to the shader API.
  The existing checkpoint records a negative-Y versus authored shader convention
  concern. Reconstruction tests alone do not establish convention equivalence.
- `vsgruntimehost.cpp:1955–1972` catches an executor failure, logs it, and bypasses
  the whole chain. This protects gameplay but is degraded rendering, not successful
  compatibility. Frontend rejection can also omit individual techniques.
- Chain startup tests do not certify F2 interaction, reload, resize, weather,
  underwater transitions, or the previously reported whiteout in all conditions.

Action: define and test the complete input contract: color/HDR, normal encoding,
depth/projection/origin, history, samplers and blending. Surface unsupported
features in the effect UI; do not silently treat placeholders as support. Test
real Rafael effects individually and in a chain, with reference images and
camera movement. Keep this as a correctness workstream with its own cost budget.
The measured ~68 ms empty-chain frame means it cannot be the principal cure for
the base performance problem.

Reference: [OpenMW 0.49 OMWFX language contract](https://openmw.readthedocs.io/en/openmw-0.49.0/reference/postprocessing/omwfx.html).
The local OpenMW implementation remains the version-specific reference.

## 8. Lighting needs semantic parity as well as GPU optimization

In classic mode the local OpenGL `LightListCallback` intersects light/object
bounds and caps a proximity-ranked list (`sceneutil/lightmanager.cpp:714–755`).
The Vulkan non-tiled shader loops the supplied local-light list
(`legacymaterialshader.cpp:559–563`), approximately 90 lights in the reference
scene. This is a concrete difference in work selection and potentially output.
Changing to radius-faded clustered lighting is not an equal-quality repair of
that difference, even when the GPU becomes faster.

Audit ignored lights, range/radius scaling, max-light selection and per-object
scope before choosing a native list builder. Preserve the selected semantics
without importing OSG traversal. Separately reduce shadow caster work with correct
per-view bounds. GPU work remains substantial, but tile-mask results show why
GPU-only savings cannot be counted as whole-frame gains under the current CPU
limit. DLSS cannot remove capture or recording costs and is not the first repair.

## 9. Validation and reporting discipline need to change

- The legacy benchmark still writes `vsync = false` at
  `tools/v4/cp4/architecture-benchmark.py:73`; the registered key is `vsync mode`.
  This was identified in the prior audit and left in active tooling. Inherited
  mode 0 means this does not prove earlier runs were capped. The current tests
  all pass without catching this schema error.
- Historical GL/Vulkan runs did not prove equal output: normal settings requested
  0.90 render scale/NIS and a different postfx chain. Preserve the measurements
  with that qualification; stop using the ratio as pure API overhead. No new
  OpenGL benchmark is required to proceed with Vulkan repairs.
- Thirty-second fixed-camera samples, with ~15 sparse CPU-profile samples, are
  useful diagnostics. They do not cover camera-turn stutter, streaming, combat,
  animated clothing, repeated cell entry or loaded saves. Treat run-to-run noise
  honestly; do not select the fastest run as the expected user experience.
- A 60.012018 ms recent run is not an established all-time minimum or an accepted
  build. The historical archive ledger records an older 59.552607 ms retained
  run (two-run mean 60.278948 ms). The old package is no longer present at its
  recorded local path, so this is archive evidence, not a rerun or comparable
  current candidate. A smaller historical outlier also does not prove progress.
- Publication run logs contain Lua initialization errors and a GUI DDS placeholder
  despite exit 0. Their causes are not established by this audit. They invalidate
  an inference of full mod compatibility from normal exit, not the timing record.
- Existing tests are useful, not fake: baseline assert fixtures explicitly undo
  NDEBUG; inspected new fixtures use throwing checks. The missing layer is
  representative integrated behavioral/rendering acceptance.

## Disposition and revised execution plan

Keep the native semantic architecture, known working importers, persistent
resource/geometry/pipeline ownership, existing instancing, completion-safe
retirement, the OpenGL control, and the measured publication trio. Keep admission
as a candidate pending integration. Keep native FX infrastructure as incomplete.
Park further zero-specular work, costly LAND promotion, expanded tile experiments
as an FPS cure, and whole-cache redesign without a measured memory bottleneck.
Preserve useful failed-experiment evidence. Do not delete source wholesale.

1. **Consolidate the candidate.** Extend the isolated-profile harness to express
   the chosen cumulative controls, validate effective settings/targets/effects,
   and freeze binary/source identities. Compare against the same candidate with
   each investigated mechanism disabled. Run a normal, profiler-off confirmation
   separately. This integrates already measured work; no FPS gain is assumed.
2. **Deliver a coordinated CPU-path change.** Replace known-object discovery with
   producer-owned dirty queues; introduce complete native actor deformation and
   view-specific preparation; address the dominant recording graph based on its
   CPU/driver trace. Keep these independently switchable so regressions can be
   localized. Do not wait for complete OSG migration before changing recording.
   Each mechanism needs an explicit list of old work it removes and where its new
   cost appears. A silt-strider fix is relevant only as shared coverage or a
   correctness fixture, not the main performance milestone.
3. **Finish correctness beside that work.** Fix FX input contracts and reproduce
   body/equipment, camera-dependent shadow and clipping reports. Bound shadow/
   lighting cost while preserving semantics. A visual regression disqualifies a
   candidate even if its timing improves.
4. **Gate the next user-facing build.** Automated indoor, exterior, actor-heavy,
   camera-sweep and streaming/hot-return routes; consistent scene/quality;
   median/mean/p95/p99 and memory peaks; baseline and combined candidate repeats;
   explicit log-error review. Add private-copy save-load-save checks, equipment/
   morph/picking tests, cell unload/reload, and F2/FX interaction. Never write the
   normal save/config directories. User acceptance remains required for promotion.

The next delivery should show a repeatable material reduction in total frame
time outside measured run variation, plus correct output, before requesting
another manual playtest. It must state the actual gain if it is small and stop
presenting that build as the broad performance repair. A 60–70 ms frame is still
unacceptable. Reaching 33.3 ms and then 16.7 ms would be useful engineering
milestones, not promises or substitutes for the user's OpenGL-or-better goal.

## Checks performed in this audit

- Reconciled branch/HEAD, dirty files, executable hashes, optimized build settings,
  selected raw manifests, publication console results, sampled graph/GPU timings,
  source call order, existing test configuration and prior reports.
- Reran `test-benchmark-control.py`: 11 passed; `test-publication-cohort.py`:
  5 passed; `test-vulkan-profile.py`: 11 passed. These 27 checks do not establish
  runtime performance or compatibility. No C++ rebuild was needed for this
  documentation-only change. Historical compiler/GPU test logs were reviewed,
  not represented as fresh executions.
- Changed only this report and the architecture plan's audit guidance/unsupported
  attribution claim. No renderer repair, new game test, commit, push, download,
  cleanup or runtime promotion occurred. Existing dirty work remains intact.
