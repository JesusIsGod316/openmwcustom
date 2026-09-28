# OptimizedMW P8G3 — vegetation visibility and submission lab

Parent: `optimizedmw/gl-p8g2-groundcover-shader@ea34526461103643ef804b6b8afab11288a82966`.
All new rendering paths are startup-only and default off. No performance or full mod-corpus visual acceptance is claimed.
The P7 FULL-STUTTER stack remains the launcher baseline. VulkanMW branches are not part of this change.

## Mechanisms

* **Adaptive hierarchy:** split eligible static model populations along their longest spatial axis, only for sufficiently
  large and spread-out groups. Default minimum population is 64, maximum leaves 8, extra drawable budget 16/model and
  32/chunk. Sparse/compact groups stay unsplit. A per-tile current-view frustum/distance test and the existing main-view
  MSOC depth reject hidden populations. No new occluder rasterizer, larger MSOC budget, GPU readback, or cull worker join.
* **Bounds:** calculate all eight source-box corners under the literal established GLSL Euler transform, including
  off-origin geometry. OSG must not union the uninstanced source mesh at the chunk origin back into a tile bound. Current-view inherited wind values bound all four harmonics and maximum stomp displacement.
  Expand in world X/Y then transform back. Invalid bounds/transform/wind fail open. New derived graph disables ordinary
  unanimated bound rejection so it cannot override this conservative test. This may increase CPU traversal work and is
  explicitly part of the performance acceptance gate. Mixed unsupported graphs retain their original callbacks. Custom shader prefixes and custom bound callbacks fall back.
* **Density LOD:** protect the near 5000 units; smooth reduction toward 45% density by 13000 units for small projected
  vegetation. Projected prominence restores density for large plants/narrower FOV. Stable reference identity determines
  nested ranks, never a per-frame random number. Immutable 100/77/52% prefix geometries share vertex and instance arrays;
  only one is selected per drawable/view. CPU selection bounds the maximum shader density across the whole tile and
  removes only fully faded ranks. Refinement is immediate; coarsening uses a small conservative hysteresis. Up to four
  camera histories, try-lock only, missing/contended state uses fresh current-view selection. No stale occlusion results.
* **Transition coverage:** only the changing 5% rank band alters alpha before the existing alpha test. There are no
  duplicate full-population draws or temporal random dithers. This is density LOD, not a geometrically identical image;
  cutout/coverage transitions require visual QA, especially without MSAA. The original full near population is retained.
* **Storage:** one private source clone/model/chunk; derived tiles share its immutable vertex/normal/UV arrays. A tile's
  offset and Euler/rank arrays share one instance VBO across material meshes and LODs. Primitive/index objects are private
  per tier (bounded three copies); no cull-time setNumInstances or shared-template mutation. New hierarchy storage is
  bounded by construction limits, not a whole-process memory cap. Normal cache retention/lifetime remains unchanged.
* **Preparation:** use the existing reserved background PrepJobService for large numeric instance-preparation halves
  only when already off the constructor/owner thread and on the compilation/preload path. Busy lane runs serially.
  The background owner joins its own task before publishing. Frustum/MSOC decisions remain current-view caller-owned;
  no live OSG graph, resource cache, shader creation, or OpenGL object work runs on the numeric helper.
* **FAST-WIND:** independent override `-1=inherit P8G2 mode 2, 0=full, 1=two harmonics`. Wind-only can now use original
  GPU path 0 without the unpromoted SAFE-SHADER package. User found P8G2 FAST-WIND visually acceptable, not performance
  promotion of this independent combination.
* **Ordering:** optional grass-only render bin 1 using OSG's native state-grouped front-to-back ordering. Normal opaque
  bin 0 and transparent bins are unchanged. This is not global sorting, a depth prepass, or forced early-fragment tests.

## Configuration / launcher

`[Groundcover] optimizedmw hierarchy`, `optimizedmw density lod`, `optimizedmw front to back`: false by default.
`optimizedmw parallel preparation=true` has no consumer unless a derived path is on.
`optimizedmw minimum batch=64`, `optimizedmw lod near=5000`, `optimizedmw lod far=13000` (world units).
`optimizedmw fast wind=-1` retains old P8G2 mode behavior. Legacy gpu path and shadow-receive keys are preserved.
No new generic attribute slots: offset at 6, Euler at 7; LOD rank uses only unused component 7.w. Fallback geometry's
uniform LOD parameters remain zero and its Vec3 Euler stream is unchanged. Slots 8/9 are forbidden.

`START-OptimizedMW-GL-P8G3-Test.bat` provides CONTROL, HIER-CULL, HIER-LOD, FAST-WIND-ONLY, ORDER-ONLY,
HIER-LOD-WIND and FULL. All keep point lighting, shadow receiving, cascade quality and P8G2 gpu math path 0.
The launcher saves effective settings/executable/config hashes, clears inherited OPENMW experiment flags locally,
restores the original environment and settings, checks the restored settings hash, and auto-packs the profile.
It never modifies saves or content. Do not run overlapping launchers. If forcibly terminated, restore settings-before.cfg
from the profile manually; finally blocks cannot survive OS process termination.

## Validation / acceptance

`policy-tests.cpp`: independent numerical checks of complete partition membership, bounded splitting, nested ranks,
zero-opacity omission, conservative hysteresis, analytical wind bounds, exact z specialization, worker parity/busy fallback.
`batch-tests.cpp`: real OSG object ownership, shared vertex/UV and instance buffers, isolated counts, UV slot exclusion,
full/half geometry counts, real SceneView cull with near/far/camera-cut/alternate-view/frustum/occlusion/fail-open checks.
`shader-tests.cpp`: actual production ShaderManager source expansion and Mesa/OpenGL compile+link of 64 groundcover
program variants across LOD, wind, math path, normals, shadows and clustered/legacy lighting. No stand-in shader functions.
A dedicated OBJECT target compiles production groundcover.cpp, not a copy of the integration.

Low-overhead optional counters require OPENMW_P8G3_STATS. Counts are cumulative **per-drawable/view visits**, not unique
world instances or exact hardware glDraw calls. MSOC counters and p8g3_chunk rows separately show eligibility/extra tiles.
Normal gameplay leaves diagnostics off. Build tests are not proof of user-hardware speedup or visual parity.

Initial user matrix: CONTROL -> HIER-CULL -> HIER-LOD, same binary/save/view/route/cap. Quality first: screen-edge sway,
hill/building reveal, fast turns, teleport/cell return, looking down, water/reflection, large mod plants, normal-mapped
cutouts, torch lighting and night/weather. Then wind-only/order-only or combined modes if useful. Require real GPU/wall
improvement and neutral-or-better p95/p99/transition/memory behavior; higher rejection alone is not a performance win.
The rejected P8G1 basis attributes, SHADOW-LITE, old stronger MSOC and global sorter are not revived.

## Pinned PBR deployment correction

The P8G2 runtime ZIP was inspected directly: its two groundcover shaders are byte-identical to the pinned Rafael overlay,
and contain none of the P8G2 experiment switches. Those shader-only performance interpretations are withdrawn.
P8G3 explicitly adapts the two **deployed PBR** shaders after the pinned overlay, with parent/output hashes in the
package manifest. The remaining overlay files are unchanged. CONTROL with both new switches disabled is token-equivalent
to the original PBR shaders. PBR lighting, subsurface response, edge refinement, distance fade, shadows and fog remain.

The standalone FAST-WIND option now removes only the middle cosine harmonic from the actual three-term PBR wind,
retaining the low-frequency term, gust term and long-distance wind lean. This new effective variant needs runtime QA;
the user's previous no-difference report cannot qualify it because the old switch was not deployed.
LOD opacity is applied AFTER PBR edge refinement, so that filter cannot resurrect omitted instances. Tight culling
bounds cover the actual PBR gust and 40*windSpeed lean, as well as the legacy shader family. The LOD helper is in the
explicit package list. Native CI runs the real CMake configure and incremental staging path, checks unrelated PBR
bytes and CONTROL equivalence, rejects missing/stale payloads, then compiles the **staged PBR** shader matrix.
