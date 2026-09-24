# CP4F authored-material / frame-handoff repair

## Checkpoint and scope

Base: `efa4f3694fad904b3546e7c846c756403950f0d2`.
Worktree branch: `codex/cp4f-material-frame-repair`.
This is an uncommitted repair batch, not a promoted gameplay build. No mod asset,
normal save, user configuration, archive entry, remote branch, or release is changed.
Local headless graphics fixtures are not gameplay acceptance.

The reference capture is `20260923-110936-gameplay-75864`, Standard diagnostics,
isolated New Game, displayed 1920x1080, Vulkan, exit 1. Its sampled ship median is
33.6737 ms (51 frames); exterior median 268.30875 ms (8 frames). These sparse CPU
envelopes overlap and are not GPU timings, p95/p99, or a controlled comparison.
Preserve the later lit appearance preview, cloud detail, correct guard placement,
and Event 123 real shadow-view/uniform checks.

## Implemented mechanisms

### A. Complete the demonstrated authored environment material

The failing evaluated draw is `animated-object:ref:0xa8000418:geometry:2`.
The winning reference override is in `LB_SeydaNeen_BCOM.esp`, load index 168;
the base record is `T_Imp_Drink_CherryBrandy_01` from Tamriel Data.
Winning NIF: `meshes/pc/m/pc_misc_kurst.nif` in the installed Tamriel Data HD
`00 Data Files` directory. Its SHA-256 is
`27c14853b5f8018eab20a3a42feff07ceb3538a4c601d168e50973e0700d877c`.

The third mesh is `Cylinder.003` (NiTriShape 14), inheriting NiTextureEffect 12:
one generated sphere environment map, `pc_reflect_redware.dds`. It combines
`pc_kurst.dds`, `pc_kurst_bumpmap.dds`, auto normal `pc_kurst_n.dds` (DXT1 RGB,
not BC5), and `pc_kurst_spec.dds` (DXT5). Authored bump matrix is identity,
luminance scale/bias 1/0, material specular 0.5 and gloss 12.5. Effective
pre-light environment blending was enabled. This is a legitimate single map,
not a corrupt 32-frame enchanted sequence.

The neutral material now distinguishes None, SphereMap, and EnchantedSequence.
Capture preserves the effective environment color, generated-coordinate
semantics, bump matrix/luminance, normal/specular stages, and pre/post-light order.
The realizer admits the exact one-map sphere case or exact 32-map enchanted case.
Unsupported generators, nonidentity environment TexMat, malformed counts, and
missing required streams still fail explicitly. There is no reference whitelist,
UV fabrication, map removal, or untextured fallback. Bump descriptors were moved
away from the existing gloss/environment descriptor slots.

The new end-to-end fixture passes inherited OSG state through production capture,
an owned neutral frame, and the production Vulkan shader. Synthetic pixel images
are deterministic fixture inputs, not replacements for user assets. It covers the
combined diffuse/normal/specular/bump/sphere material and pre/post-light order;
existing enchanted and malformed-sequence cases remain separate. The fixture does
not prove visual equivalence for every mod material or nonidentity bump transform.

### B. Own and validate effect payloads once

`OwnedImmediateEffects` deep-copies the producer payload once, including procedural
texture pixels. It has no mutable accessors or copy/move assignment. A retained
producer alias cannot invalidate its cached result. Sorted full-string uniqueness
checking is collision-safe and does not reorder the rendering vector.

Session routing, preparation, presentation, and history share that immutable
payload. World epoch, resource revision, view and history checks remain live.
One duplicate host validation was removed; compatibility still validates the
frame. The legacy vector route remains available as a same-executable control.

Dynamic capture itself is not newly cached. New aggregate counters separate actor
and non-actor capture, use-animation objects, rig/morph evaluation, and ordinary
geometry visits. Safe finer-grained invalidation still needs actual controller,
active-child, deformation and particle-generation evidence. Do not freeze these
by treating a parent animation callback or pointer identity as a dirty flag.

### C. Avoid unchanged population replans; admit speculative work before allocation

Static population planning can reuse a resident plan only with identical options
and current placement/model/material/texture dependencies. A chunk-only revision
change is acknowledged after revalidation; graph identity, packing origin and
fence retirement remain intact. Exact stale dependency identity, owning chunk and
model identities, and old/new revision are logged for bounded examples. Chunk
mismatch counts are still not unique cells or compiled population counts.

Optional cell preloading now reserves admission before owner allocation or cache
eviction. At most four queued/running optional jobs are admitted, each with a
256 MiB headroom allowance. These are estimates, not allocated or released bytes;
an individual asset can exceed the allowance. Independently valid physical,
process and commit counters constrain admission. A move-only reservation refunds
exactly once on completion, exception, cancellation/destruction. Existing
between-asset pressure checks, completed-owner pressure release, useful 1800-second
retention, overdrive, required loading/collision, and GPU fence ownership remain.

### D. Observe actual visual inputs before changing composition

Opt-in water probes copy an 8x8 sparse sample of reflection RGBA16F and refraction
RGBA16F/D32 targets after the render passes, on the normal queue. Buffers, images
and commands stay owned until the ordinary frame-completion fence permits reading.
There is no added device-idle wait or independent readback submission. Layouts are
restored before later consumers. At most 12 batches are issued, at least five
seconds apart, only on sampled water frames.

Records include the source frame, clear color, linear color range/nonclear count,
reversed-depth range/coverage, nonfinite count, checksum, target extent, clip plane,
water height, interior/underwater state, projection conventions and normal-map
presence/format/dimensions. Clear-like sparse samples do not prove an entire target
is empty. A fresh process is needed after the bounded probe budget is exhausted.

Focused material probes include evaluated capture and static realization, winning
VFS content identity, stage role, source format/color space, UV/TexMat information,
environment mode/order, specular/gloss, and bump matrix/luminance. Logging is
bounded and can be narrowed by a case-sensitive path/identity substring.
No cyan-material tint correction or water-composition heuristic is introduced.

## Independent controls and diagnostics

Presence of these environment variables selects the old mechanism; **unset** them
for the repair path (setting `0` still counts as present). Change one per comparison.

| Variable | Control |
| --- | --- |
| `OPENMW_V4_LEGACY_FRAME_HANDOFF_CONTROL` | Mutable-vector copy/validation route |
| `OPENMW_V4_LEGACY_POPULATION_PLAN_CONTROL` | Full population-plan rebuilding route |
| `OPENMW_V4_LEGACY_PRELOAD_ADMISSION_CONTROL` | No new optional-job reservations |
| `OPENMW_V4_LEGACY_AUTHORED_ENVIRONMENT_CONTROL` | Reject the one-map authored material; intended negative fixture control, expected Office failure |

`gameplay-diagnostics.py launch` retains Standard, Focused and Off modes,
1920x1080 borderless output, separate user-data and no copied regular saves.
Do not use Focused/probe captures as a timing benchmark.

- `--water-probes`: opt-in Vulkan probes; rejects diagnostics Off.
- `--material-probe pc_kurst`: narrow example, requires Vulkan + `--diagnostics focused`.
- No probe flag enables extra GPU readback by default.
- The manifest records executable hash, shader package, source-head/diff arguments,
  inherited controls, configuration fingerprints, and changed source-file hashes
  when launched from a source checkout. A source checkout fingerprint alone does
  not prove the executable was built from it; match the build/executable manifest.
- Reports retain the last 64 repair records per category (eight printed examples),
  inclusive stage timings, preload reservation estimates and runtime memory data.
  The raw logs remain in the evidence ZIP. No saves or mod assets are bundled.

For an authorized test, point the helper at the **verified staged executable**,
not an older `C:\Openmw-V4Test` binary just because the path already exists.
The packaged starter is `Start-CP4F-Test.cmd`; do not repeat the obsolete
`Start-CP4F-Fixed.cmd` instruction.

## Local verification, 2026-09-23

- Focused MSVC Release build: passed, including the changed production translation
  units compiled in both OpenGL-control and Vulkan configurations. This is not a
  substitute for a complete engine link.
- CTest: 7/7 passed (handoff, static population, static sync, view features,
  uniforms, capture, host memory). Host-memory executable: 22/22 cases, including
  concurrent admission/refunds and partial valid counters.
- Native diagnostic-header build: passed with MSVC `/W4 /WX /permissive-`, the
  Windows SDK and PSAPI; CTest 4/4 passed (clean includes, keyword macros before
  and after inclusion, host-memory policy). These include-order fixtures do not
  replace real Qt builds; the local Qt variants were not run.
- Production-function handoff/scaling: 256/1000/2000/4000 draws, malformed and
  duplicate payloads, alias safety, generation/coherence, skipped/retried frames,
  history and shared lifetime passed. Timing printouts are microbenchmarks, not FPS.
- Full pixel suite: passed on NVIDIA GeForce RTX 5050 Laptop GPU with Vulkan
  validation enabled. Includes actual authored capture-to-shader fixture, exact
  enchanted sequence, bump/normal/specular combination, sparse water/depth
  readback, real shadow views, unchanged preview buffers, sky/water/LAND and GUI.
- Authored-environment legacy negative control: exits 1 with the expected one-map
  material failure; repaired default passes.
- Python diagnostic tests: 20 gameplay, 14 configuration, 10 shader-resource,
  17 runtime cases (one platform-specific skip). Header/linkage and host-memory
  integration guards passed. Streaming, population/environment, water, startup
  video/UI, evaluated animation, first-person and runtime-blocker source guards
  passed. Source guards alone do not establish runtime correctness.
- Full engine MSVC Release OpenGL/Vulkan production builds: both passed through
  `openmw.exe` linking, exit 0 and zero errors. OpenGL logged 459 warnings; Vulkan
  logged 506 warnings (this is not a warning-free claim). Initial attempts
  exposed incomplete cached Boost, MyGUI and Bullet include trees. Only these
  isolated build configurations now select the existing complete package
  (`boost-repair-20260918/package/installed/x64-windows`); shared dependencies
  were not edited. MyGUI and all four Bullet import/static libraries were
  verified byte-identical between the cached packages. Both Bullet header
  trees report version 325. The builds use bounded MSVC `/MP3` compilation.
- The generated Vulkan build's 82-file shader package passed hash verification;
  shader manifest SHA-256 is
  `bc4c6b6493a7f229a5ed99b109031328ba882cde8e7ca40dd252546ee903e411`.

The local engine build directories are `build/cp4f-engine-vulkan` and
`build/cp4f-engine-opengl`; each keeps its effective `CMakeCache.txt` and
`production-build-complete-deps.log`. The focused rendering and diagnostic-header
directories are `build/cp4f-rendering` and `build/cp4f-diagnostic-headers`.
The engine target gate is not a build of the Qt launcher/editor or every optional
application/component-test target. A linked executable alone is not a staged,
runtime-complete test package. Do not substitute it into an older installation
without verifying the accompanying dependencies and resources.

Exact local executable SHA-256 values:

- Vulkan: `fade37e7da0a3924bb9e423f2b0ac80214126d7159251dd0d96b39ac75ff4e5b`
- OpenGL control: `54943dd25d4abfd63d7f9ec48c5296adeb0ffeceec15192319a6122e72c76489`

`build/cp4f-local-build-manifest.json` records the base commit, dirty-worktree
status, hashes of all 43 changed source/test files (this Markdown note excluded),
both executable hashes, build caches and validation state. The source inventory
hash is `a0db665446689633e489e50a75fe94f875ad9d5a5d2ee173d7f25d3131d97364`;
it is a file-hash inventory, not a Git commit or a patch/diff hash. The original
checkout remains clean at `073a00dd00ca8026ca3a4734120aad09d42ef677` on
`codex/cp4e-water-recovery`. No commit, push, CI dispatch, package deployment,
archive update or gameplay launch was performed for this batch.

## Next authorized runtime validation

1. Standard diagnostics: New Game, ship, exterior, appearance preview (first
   publication, reopen, race/sex/face/hair controls), Office, return/revisit.
   Check guard placement, clouds, shadows, animated objects and effects.
2. Short Focused visual pass with water probes; include screenshots of the actual
   water/shoreline and cyan material. Use a selected winning material substring
   where needed. Identify whether failure is input coverage, depth/projection,
   stage semantics or composition; do not assume one common cause.
3. Fresh-process fixed-location same-executable comparisons, identical settings,
   view, time and diagnostics mode, one control at a time. Use a separate dense
   frame-time capture for median/p95/p99/hitches; sparse diagnostic samples cannot
   supply reliable tail statistics. Keep diagnostic/probe overhead separate.
4. Bounded memory soak with queued/running reservation counts, completed optional
   owners, external image/template references, staging and fence retirements.
   Do not add overlapping payload/CPU/driver accounting scopes.

Remaining work is explicit: gameplay acceptance of the Office material; actual
exterior performance measurements; safe finer-grained dynamic-capture invalidation;
canonical comparison of cyan materials and water/LAND coverage; preview-controls
and cave-water validation. No promise of FPS gain follows from these local tests.

## Exact changed-file inventory

Paths relative to the isolated worktree, including this note:

```text
components/render/backend/vsg/waterinputprobe.hpp
components/resource/preloadadmission.hpp
tools/v4/cp4/CP4F-MATERIAL-FRAME-REPAIR.md
tools/v4/cp4/authored-material-fixture.cpp
tools/v4/cp4/frame-handoff-tests.cpp
apps/openmw/mwrender/v4effectcapture.hpp
apps/openmw/mwrender/v4effectuv.hpp
apps/openmw/mwrender/v4engineframecoordinator.cpp
apps/openmw/mwrender/v4enginerenderbridge.cpp
apps/openmw/mwrender/v4localmapbridge.cpp
apps/openmw/mwrender/v4previewbridge.cpp
apps/openmw/mwworld/cellpreloader.cpp
components/debug/gameplaydiagnostics.hpp
components/misc/resourcehelpers.cpp
components/misc/resourcehelpers.hpp
components/nifrender/enchantedglow.hpp
components/render/backend/vsg/enchantedmaterialshader.hpp
components/render/backend/vsg/immediateeffectrealizer.hpp
components/render/backend/vsg/legacybumpmaterialshader.hpp
components/render/backend/vsg/offscreenrendertarget.cpp
components/render/backend/vsg/staticassetrealizer.cpp
components/render/backend/vsg/staticpopulationresidency.hpp
components/render/backend/vsg/staticworldplan.hpp
components/render/backend/vsg/vsgruntimehost.cpp
components/render/backend/vsg/vsgruntimehost.hpp
components/render/backend/vsg/vsgsemanticsession.cpp
components/rendercore/effectframe.hpp
components/rendercore/frameproducer.hpp
components/rendercore/framerenderstate.hpp
components/rendercore/realizationkeys.hpp
components/rendercore/records.hpp
components/resource/hostmemorybudget.hpp
components/resource/resourcesystem.hpp
components/sceneutil/texturetype.hpp
components/sceneutil/util.cpp
components/sceneutil/util.hpp
tools/v4/cp4/gameplay-diagnostics.py
tools/v4/cp4/host-memory-tests.cpp
tools/v4/cp4/rendering-capture-tests.cpp
tools/v4/cp4/rendering-pixel-tests.cpp
tools/v4/cp4/rendering-tests/CMakeLists.txt
tools/v4/cp4/runtime-diagnostics.py
tools/v4/cp4/static-population-smoke.cpp
tools/v4/cp4/test-gameplay-diagnostics.py
```
