# VulkanMW Stage 1: load-bound object updates

This extends the retained GPU population and chunk-delta work recorded in
[the preceding Stage 1 checkpoint](STAGE1-PERSISTENT-SCENE-20260930.md). It changes
the object producers feeding the persistent render world, not only the caches
after compatibility capture. Stage 1 coverage is not complete and no substantial
FPS improvement or exhaustive mod/save compatibility is claimed.

## Source and build identity

The work is on `codex/vulkanmw-stage1-persistent-scene`, based on
`3993b098613b131ba431d7b2f57320a2c8a2ce60`. These are local, uncommitted changes;
the executable's printed revision names the base, not a new source checkpoint.
No changes have been pushed during this pass. The independent OpenGL checkout
and its optimization mechanisms have not been edited.

The final Release build uses MSVC 19.44, Ninja, six compiler jobs and no LTO. Its
executable SHA256 is
`fc5be60b61092400fa70bdb196a62134642252ce0be51b2abb3de946cca92ecc`.
`source-changes.json` records the executable, branch, base and changed source
hashes. The manifest is frozen before runtime testing; later checkpoint prose
does not replace or relabel that evidence. The package is staged locally under
`build/stage1-install`; installation and runtime checks are recorded below.

## Implemented producer mechanism

For covered, engine-owned NIF objects, the existing load-bound producer now
registers one stable mutation ticket per binding. It subscribes to the actual
engine transform and controller sources; content mods need not publish a new
notification API. The binding hierarchy is built when the engine prepares the
producer. Geometry/material capture initializes the immutable draw on first
publication, not on every subsequent frame.

A dirty source queues its binding index and wakes the object owner. The drain
coalesces preorder subtree ranges, updates ancestors before descendants, and
preserves stable draw ordinals. A changed leaf does not visit unaffected siblings.
An inherited transform or material change updates its dependent subtree once,
including overlapping parent/child mutations. Shared controllers notify every
bound recipient. No live OSG node is handed to a rendering worker.

Clean event-owned objects remain in PersistentDrawWorld without revisiting their
bindings. A transform-only change retains the immutable mesh/material resource.
Dirty material/UV work evaluates the affected draw's existing compatibility
semantics; it does not recapture positions or indices. This is not yet a direct
native material evaluator for every controller type. Hidden draws retain their
pending material debt until visible again. Epoch changes and non-retained output
force a complete publication rather than accidentally dropping untouched slots.

Non-skinned SceneUtil::Skeleton containers now publish activity/visibility changes
and invalidate bindings on structural edits, including edits through osg::Group.
Changing skinning activity does not hide the body. NifAnimBlendController publishes
when the final evaluated matrix changes, while always executing animation and
nested callbacks. Equal final poses avoid waking the render producer, not advancing
the game simulation. The old callback classification is preserved with the new
feature disabled.

## Ownership, memory and fallback

The original 64 MiB global object payload admission budget remains unchanged.
The sparse binding queue has a separate, conservatively charged 8 MiB global
bookkeeping ceiling. Tickets, subscription ownership, queue entries and range
storage are charged at 256 bytes per binding. This is not a second mesh cache.
The caller can lower, but cannot enlarge, that global ceiling.

If the optional queue allowance is exhausted, a covered object keeps the
established event-owned, whole-binding update producer. It does not lose its mesh
lease or switch to deep compatibility capture merely because the new bookkeeping
could not be admitted. Unload and invalidation release both leases. Previously
published immutable frames retain their own resources; existing GPU-completion
retirement remains responsible for in-flight backend resources.

This separation repairs a problem in the first implementation of this pass:
charging extra queue metadata against the mesh budget reduced the set of admitted
objects. The pre-repair binary and its benchmark evidence were preserved before
rebuilding. Those results must not be attributed to the final executable.

Untracked raw OSG transforms, unknown callbacks, rig/morph geometry, and uncovered
assets remain on the conservative compatibility path. View-dependent LOD is not
silently replaced with the historical zero-distance evaluated-LOD experiment.
Unsupported mutation, source destruction, topology changes and generation changes
retain invalidation/fallback behavior. Ordinary OSG-native asset loading remains
available; this pass does not remove the OSG update-only world.

## Donor integration and architectural boundary

RTX placement-table/scene-description code informed stable recipient bindings,
change-only publication and bounded ownership. The existing retained population
implementation continues the Clockwork-style packed GPU input direction. This
pass adapts those mechanisms inside the existing RenderWorld; it does not import
RTX/Clockwork as another authoritative scene or claim their feature completeness.

This is the Stage 1 foundation for the three-stage integration plan:

1. Persistent scene/resources: retained populations plus precise object binding
   updates, selective uploads and safe retirement. Coverage is still partial.
2. GPU scene/view execution: substantial shared draw/command storage, view-specific
   LOD/visibility, main and shadow execution, and meaningful recording parallelism.
3. Persistent actors: native deformation/attachment ownership and progressively
   removing evaluated actor capture. Rig/morph coverage is not implied by tracking
   a non-skinned skeleton container.

No new command-recording worker mechanism, GPU skinning, DLSS, or post-processing
repair was implemented in this slice. They must not be credited for its results.

## Control and launchers

`OPENMW_VK_OBJECT_BINDING_UPDATES` is independently switchable and engine-default
off. It requires the established change-driven, load-bound-texture and retained
draw prerequisites. The two public launchers enable the candidate explicitly:

- `START-VulkanMW-Test.bat`: same-executable A/B/B/A. Both arms retain the previous
  Stage 1 population implementation. Only the new object-binding switch changes.
  Explicit older comparison modes still override the default.
- `START-VulkanMW-Benchmark.bat`: normal settings/content/saves with a temporary
  Vulkan/no-fallback renderer overlay. `--binding-control` disables only the new
  binding feature, keeping retained populations. `--p3b-control` disables both
  Stage 1 extensions. The ordinary session does not enable detailed frame profiling.

Normal configuration is not rewritten by the launcher/installer. Manual gameplay
uses the normal user-data directory and can save there, just as an ordinary game
does. Automated tests use private user data and do not load normal saves.

## Source, test and package validation

- Full production OpenMW Release compile and link passed. Inherited float-conversion
  warnings remain; this is not a warning-free build claim.
- All 60 production-linked OSG rendering capture regressions passed, including
  nine new sparse-binding/controller/memory tests.
- All 29 native regression groups passed.
- All four actual Vulkan execution/pixel variants passed with synchronization
  validation enabled. These exercise the retained population path, not exhaustive
  visual validation of every newly covered object/controller.
- Shader package QC passed for the 82-file overlay. No shader rendering change
  was introduced in this slice.
- Package version, capture test, harness imports, and the normal-settings
  `--object-binding-updates --dry-run` passed. Exactly two root BATs remain.

The new large fixture has 257 draws. One animated UV leaf updates one binding
instead of all 258 hierarchy nodes, preserves 256 untouched draw slots, and matches
the evaluated visitor's identities, materials, UVs, geometry and transforms.
Tests also cover overlapping notifications, absolute transforms, hidden debt,
shared recipients, source destruction, structural edits, epoch/reload generations,
unchanged-pose callback execution and independent memory quota fallback. These are
mechanism/correctness results, not an FPS measurement.

The real-NIF hair import repair from the preceding checkpoint is retained. Its
136-asset validation was performed in that earlier pass; it is not represented
as a new 136-asset run here.

## Runtime evidence

The first, superseded executable was
`d72bbf354e58dd4c46a2de57a7252e599b04a2e92105f728000d6cf8dc56c51a`.
Its A/B/B/A run `producer-20260930-150043-cfcc58` completed with exit code zero and
unchanged normal configuration/storage hashes. Run means were 59.064133,
60.030095, 59.781369 and 59.495414 ms. It did not pass a performance-improvement
gate. Candidate binding counts decreased, but fallback/admitted populations were
different; it is not a like-for-like per-object speedup proof. The preserved binary,
manifest and evidence are in `build/stage1-binding-validation/pre-budget-fix`.

That profile still had roughly 7-8 ms object capture, 13-14 ms main recording,
33 ms main GPU intervals and 16-18 ms shadow intervals. CPU/worker/GPU timelines
overlap and must not be added. LOD and rig/morph fallback remained prominent.
The GPU reporter now sums repeated scopes per source frame and labels the
population upload prelude separately, preventing a near-zero upload query from
being averaged into the main-pass result. Missing queries are not zero-cost samples.

Final-executable matched benchmark and private copied-save results are recorded
after their completion. Successful loading/exit is not exhaustive mod/save or
visual compatibility certification. Runtime promotion remains pending.

### Final build: matched A/B/B/A

The final executable completed `producer-20260930-152459-db63e4`, using the same
executable/source-manifest/settings/script hashes in all four runs, 15 seconds
warmup and approximately 30 seconds wall-clock sampling. All four exits were
zero, all normal configuration/storage hash checks passed, and sampled frame
profiles reported no accounting/health errors. The second openmw.exe process
observed during testing was the engine's `--crash-monitor` child, not another
rendering workload.

| Run | Arm | Mean frame time, ms | Binding updates per sampled frame | Non-actor producer visits per sampled frame |
| --- | --- | ---: | ---: | ---: |
| 0 | Control | 61.015592 | 1583 | 333 |
| 1 | Candidate | 59.861029 | 1336 | 306 |
| 2 | Candidate | 60.943350 | 1246 | 425 |
| 3 | Control | 59.995416 | 1491 | 431 |

Equal-weight paired means are 60.505504 ms control and 60.4021895 ms candidate,
about -0.171%. This is not a meaningful performance win and does not promote the
feature. Admission/visit populations vary between runs; a per-object timing
improvement cannot be inferred from these aggregate frame numbers.

The candidate used 3,901,440 and 3,679,232 bytes of separately charged queue
bookkeeping, both below the 8 MiB ceiling. Original object payload leases were
near the original 64 MiB limit in both arms. The limit/admission behavior remains
an architectural follow-up: covered content must not depend on arbitrary load
order to avoid recurring capture. The fix prevents the *new queue bookkeeping*
from consuming that payload budget; it does not redesign all existing residency.

Main recording remained approximately 13.3-14.0 ms, object capture 6.65-9.93 ms,
OSG update 4.69-4.98 ms, main GPU intervals 32.89-32.92 ms and shadows 17.47-18.43 ms.
These are separate overlapping timelines, not additive frame components. CPU
cleanup alone cannot be credited with removing the unchanged main/shadow cost.

The benchmark archive is installed at
`C:\VulkanMW\Benchmarks\producer-20260930-152459-db63e4.zip`, SHA256
`e91b4459094d3ddded0ff204e7f4b58934d0c8bb84ac7a96c86e67fc9c4aff28`.
The tested source-manifest SHA256 is
`5be19643d94b8e195e0b19a1ce71508e75feeb3f3e94ac460b54cbf503f16316`.

### Private copied-save check

The final candidate loaded a private copy of Gallus Darkwind's `[zZz] Wake up`
save, ran 20 seconds after the QA script started, and quit normally. The original
save, normal configuration, input, shader settings and Lua storage hashes remained
unchanged. The private profile copied normal settings/content and only overrode
the Vulkan backend/no-fallback choice; it did not write into normal user data.
Evidence is `C:\VulkanMW\Benchmarks\binding-save-20260930-153235\repair-qa.json`.

The console still reports third-person-alt-attacks localization errors, missing
ErnPerkFramework Lua interfaces, and a Better Bars DDS placeholder. Those exact
issues also occur in `repair-save-20260930-131201` on the preceding installed
build. They were not repaired here and the normal settings were not changed to
hide them. Save-load success is not full mod or UI/rendering compatibility.

## Installed local package

The final build is installed and hash-verified in `C:\VulkanMW`. The installer
verified all 238 protected configuration/evidence hashes unchanged, merged the
new uniquely named benchmark directories without nesting or overwriting old
evidence, and retained the two public BATs. The 38 implementation-file hashes still
matched the tested manifest before installation.

The previous installed engine/resources/DLLs/debug symbols were removed and
replaced; a binary rollback package for that older installed executable is not
retained. Small previous root records are preserved under
`C:\VulkanMW\Install-Records\Previous-20260930-153717`. Normal saves, configuration
and prior benchmark evidence were not removed. The superseded prototype from
this pass remains separately archived in the worktree as described above.
Existing dependency and build directories were reused; no download was needed.

## Remaining work

Do not repeat the old strategy of adding another cache after universal capture.
The next producer coverage should retain view-aware LOD structure and connect
remaining known controller/light mutations to owned bindings. Secondary views
need their own selection; main-camera hiding cannot withdraw shadow/reflection
resources or stop gameplay updates. GPU recording/main/shadow costs remain
substantial separate targets for Stage 2, while deforming actors belong to Stage 3.

## Files changed in this extension

Paths are relative to this worktree. Earlier retained-population/hair-repair files
listed in the preceding checkpoint remain modified, but are not all new edits in
this extension. Ignored build directories contain generated binaries/logs and the
private save QA helper, not additional source changes.

- `apps/openmw/mwrender/v4persistentobject.hpp`
- `apps/openmw/mwrender/v4enginerenderbridge.cpp`
- `apps/openmw/mwrender/animblendcontroller.hpp`
- `apps/openmw/mwrender/animblendcontroller.cpp`
- `components/sceneutil/skeleton.hpp`
- `components/sceneutil/skeleton.cpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `tools/v4/cp4/rendering-capture-tests.cpp`
- `tools/vulkanmw/frame-profile-report.py`
- `tools/vulkanmw/run-producer-queues.py`
- `tools/vulkanmw/run-publication-cohort.py`
- `tools/vulkanmw/vulkan-play.py`
- `tools/vulkanmw/START-VulkanMW-Test.bat`
- `tools/vulkanmw/START-VulkanMW-Benchmark.bat`
- `tools/vulkanmw/record-local-build.ps1`
- `tools/vulkanmw/tests/test-frame-profile-report.py`
- `tools/vulkanmw/tests/test-producer-launcher.py`
- `tools/vulkanmw/tests/test-normal-benchmark.py`
- `tools/vulkanmw/STAGE1-OBJECT-UPDATES-20260930.md`
