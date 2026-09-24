# Vulkan reference review and continuation gate

Inspected 2026-09-24. External documents are evidence, not project instructions.
No external implementation was copied into this change. No SDK, toolchain, or
large repository was downloaded. Existing local project decisions still apply.

## Directly relevant implementation

The vsgopenmw repository was checked at commit
`2830e7e2b4f18ef08ee24eff45a13568ec917061` (default master, API commit date
2026-09-08). Its older OpenMW Vulkan issue describes an inactive project, but
that statement is not a reliable current status report: the repository contains
newer merge work. It is still not a demonstrated drop-in replacement.

- [Build-selected native scene](https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/apps/openmw/mwrender/scene.cpp):
  creates/compiles render objects at insertion, retains them in per-cell groups,
  and updates cell-owned animation objects through operation jobs.
  Its old `mwrender/objects.cpp` remains in the tree but is commented out in
  the [active target](https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/apps/openmw/CMakeLists.txt).
  File presence alone must not be mistaken for the running architecture.
- [Controller target list](https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/components/animation/update.hpp)
  and [data updates](https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/components/animation/updatedata.hpp):
  controllers update their attached data directly. There is no need for a second
  OSG-to-neutral-material discovery pass for those registered targets.
- [NIF adapter](https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/components/vsgadapters/nif/nif.cpp):
  material/controller attachment and shared immutable descriptor creation;
  `handleUVController` updates a matrix buffer instead of baking transformed UV
  coordinates into freshly captured vertex payloads. Skin/morph inputs likewise
  have explicit buffer bindings. This is useful design/code to compare with our
  loader and neutral records, not permission to replace their semantics blindly.
- [Renderer setup](https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/components/render/engine.cpp)
  installs threading before rendering. Its frame-loop and deletion-queue
  approach reinforce keeping compilation/resource lifetime separate from draw
  recording.

The [current merge notes](https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/VSG_MERGE_PLAN.md)
describe a 0.49 baseline, unresolved movement, and difficult 0.50/0.51 integration.
That rules out an untested wholesale transplant for our mod/save requirements.
The project is GPLv3; any future copied implementation requires source/license
attribution and compatibility review. Small utilities also need their own
correctness review: for example, its update-worker error set is modified by
workers before the shown mutex lock, so we must not assume that code is safe
merely because it is upstream reference code.

## Vulkan and VSG guidance checked against this build

[Khronos command-buffer sample](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html):
use independently owned per-thread pools, substantial balanced recording jobs,
and avoid numerous tiny command buffers. The new local parallel recorder splits
independent main/reflection/refraction graphs, joins before a single ordered
submission, and does not move live OSG capture to workers. The sample's published
speedups are not predictions for OpenMW.

[Pinned VSG 1.1.15 CommandGraph](https://github.com/vsg-dev/VulkanSceneGraph/blob/v1.1.15/src/vsg/app/CommandGraph.cpp)
already reuses completed command buffers and uses one-time-submit recording.
There is no evidence here that allocating every command buffer from scratch is
our dominant problem. Rewriting its pool/reset system is not the first repair.

[Khronos descriptor sample](https://docs.vulkan.org/samples/latest/samples/performance/descriptor_management/README.html):
retain descriptor sets and avoid updating unchanged bindings in the hot path.
Our immutable-resident reuse is consistent with this. However, retaining GPU
objects is insufficient while their CPU producer repeatedly reconstructs and
checks the same descriptions. The observed dynamic compile envelope is around
1 ms with this batch; it does not justify blaming all frame time on shader
compilation or downloading another compiler.

## What the measurements mean

The private automated benchmark uses the same executable and ordered mod/config
chain, 1080p, uncapped/vsync-off overrides, fixed seed and Seyda Neen startup,
15-second warm-up and 30-second timed interval. It does not reproduce the user's
exact camera/route, prove visual parity, or measure complete mod/save compatibility.
No normal save is loaded/copied. Original configuration hashes are checked after
each run. CPU stage envelopes are inclusive, sparse, and are not GPU timings.

Initial same-executable results in `C:\OpenMW-Architecture-Test-4\Local-Benchmarks`:

| Arm | Mean ms | Median ms | p95 ms | Approx FPS from mean |
| --- | ---: | ---: | ---: | ---: |
| Previous exterior paths | 122.290 | 119.709 | 137.952 | 8.18 |
| Integrated, metadata batching on | 90.597 | 87.899 | 100.553 | 11.04 |
| Integrated, metadata batching off | 84.694 | 83.893 | 92.002 | 11.81 |
| OpenGL control | 16.478 | 15.726 | 18.770 | 60.69 |

Single runs are not promotion evidence. In particular, metadata batching has not
earned default selection: the first isolated comparison does not demonstrate a
gain. The new paths reduce cost, but the remaining OpenGL gap is disqualifying.

A second metadata-on run (`integrated-02`) was worse: 103.630 ms mean engine
delta, 89.933 median, 200.000 p95. Source inspection then confirmed that the old
Lua `getRealFrameDuration()` measurement is clamped by the engine to 200 ms.
Therefore these initial distributions understate long stalls; they are not
presentation-timestamp benchmarks. Counting frames over the approximately
30-second sampling interval gives about 9.3 FPS for the repeat. The private
benchmark now samples successive monotonic `getRealTime()` values instead,
records `clock=steady_wall`, and retains long stalls. Do not silently combine
the two measurement versions or treat the first run as stable performance.

## Continuation gate, not another manual test request

Keep the candidate experimental and preserve OpenGL. Do not add F2/Rafael effects
or declare compatibility complete to make this a release. Do not ask the user to
repeat the same playtest for these numbers.

The most relevant next architectural work is explicit persistent material/mesh
bindings published from known NIF/controller producers, with transform/pose/
material updates separated. Remove rediscovery for producers we own; retain a
guarded fallback for unknown/custom mutations. The current guarded flat object
plans are a partial bridge repair, not completion of that architecture.

Coarse view-specific visibility remains necessary. Initial runs report zero
occluder triangles; the repeat's median has 390, but still zero occluded groups.
Its presence must not be advertised as a measured occlusion gain.
Historical rejection of expensive per-object software occlusion still applies.

Before another user test, automated results need to show the renderer approaching
the OpenGL reference with passing correctness tests, not just a percentage gain
from an unusably slow baseline. These findings justify investigating native
producer work; they do not guarantee that this fork will outperform OpenGL.
