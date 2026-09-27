# Native OMWFX and sun-shadow repair checkpoint — 2026-09-27

## Status and authority

Worktree: `C:/Users/LSCha/.codex/worktrees/vulkanmw-phase3c-metadata/OpenMW custom Build`.
Branch: `vulkanmw/phase3c-native-runtime-substitution`.
Base HEAD: `336a4cf8668afa1a4f03069785421e9a5e2b74ef`.
Local uncommitted changes; no new commit or push. Preserve the other native
animation/publication experiments already in this worktree. The Shared Context
Archive/control block was consulted earlier in this ongoing implementation.
Source/build state is authoritative; this report does not promote a benchmark.

The user's larger performance goal remains **unachieved**. These are rendering
correctness repairs, not proof of near-OpenGL frame times or complete mod support.

## Reproduced defects and repairs

1. **Fatal `snowTexture` upload rejection.** The native executor rejected all
   compressed OSG images. It now decodes supported S3TC DXT1/DXT3/DXT5 data once
   when publishing the graph, using OSG's existing decoder. Images shared by
   different samplers share a GPU allocation. This is not per-frame decoding.
   Unsupported formats still report a diagnostic; the runtime bypasses a failed
   effect chain rather than terminating the game.
2. **White, textureless-looking world after enabling Rafael's chain.** The native
   bridge copied camera matrices but omitted eye position/direction formerly
   supplied by the OpenGL cull callback. Fog/weather effects consequently used
   an incorrect world-space ray. The native path now updates all camera fields
   through the authoritative typed std140 layout, including position, direction,
   near/far planes and resolution. Parsed effect GPU tests exercise translated
   and rotated cameras. The combined runtime capture retains visible textures;
   remaining haze/reflection issues are not declared fixed.
3. **Camera-dependent sun shadow fitting.** The VSG 1.1.15 fitter uses a projected
   `vec3` position operation to derive a direction. At an origin-centered camera,
   the regression test produces non-finite cascade matrices. The new opt-in
   `OPENMW_VK_STABLE_SUN_SHADOWS=1` route uses a world-fixed light basis, enclosing
   cascade spheres and texel-snapped centers. It includes upstream, off-camera
   casters. It retains VSG's shadow resources/descriptors/recording, and falls back
   for unsupported shadow modes. No library binary was replaced. The original
   path remains available by omitting the flag. This changes shadow fitting and
   is not an equal-quality performance comparison.

The source used to confirm the library calculation is the pinned
[VSG 1.1.15 implementation](https://raw.githubusercontent.com/vsg-dev/VulkanSceneGraph/v1.1.15/src/vsg/state/ViewDependentState.cpp).
The local `vsg/maths/mat4.h` confirms that the relevant vec3 operation divides by
homogeneous w. The same-binary control fails the finite-matrix assertion; the
new route passes, including at large world coordinates.

## Files implementing these repairs

- `components/render/backend/vsg/omwfx.cpp`: compressed imports, image sharing,
  complete native camera upload.
- `components/fx/stateupdater.hpp`, `components/std140/ubo.hpp`: typed camera
  snapshot patching; avoids duplicating byte offsets in the backend.
- `components/render/backend/vsg/openmwviewdependentstate.cpp` and `.hpp`:
  opt-in world-anchored hard directional-shadow fitting and buffer publication.
- `components/render/backend/vsg/fximagecapture.hpp` (new), `vsgruntimehost.cpp`
  and `.hpp`: explicit one-shot before/after GPU capture for visual diagnosis.
  `OPENMW_VK_FX_CAPTURE_DIR` enables a wait/readback on frame 120 only. It is
  disabled normally. Never qualify performance runs with this control enabled.
- `tools/vulkanmw/tests/fx-native-render-tests.cpp` and
  `tools/vulkanmw/tests/data/shaders/native-camera-probe.omwfx`: import and
  parsed camera API pixel regressions.
- `tools/v4/cp4/rendering-pixel-tests.cpp`: anchored-shadow pixel regression,
  16 camera/world cases, finite matrices and light-ray alignment assertions.
- `apps/openmw/CMakeLists.txt`: links the existing authored-material fixture
  needed by the rendering pixel test target.
- `tools/vulkanmw/run-omwfx-smoke.py`: private-config visual capture and
  independently selectable stable-shadow option; manifests explicitly mark
  these smoke runs performance-unqualified.

The broader native OMWFX parser/executor/frontend implementation is also still
uncommitted. The package's source manifest enumerates and hashes every dirty
tracked/untracked source file; the list above isolates the recent repairs.

## Build and validation

MSVC RelWithDebInfo build succeeded for `openmw`,
`openmw-vulkan-fx-render-tests` and `openmw-v4-rendering-pixel-tests`.
Evidence: `build/omwfx-shadow-build.log` and `build/stable-shadow-build3.log`.
Existing unrelated conversion/unused-variable warnings remain.

- Native OMWFX GPU checks: **90 passed** on RTX 5050 Laptop GPU. Evidence:
  `build/omwfx-shadow-fx-test.log`. Includes parsed camera API (27), compressed
  textures/1D/3D formats (18), and existing chain/depth/history/parameter tests (45).
- Rendering pixel suite, `all`, with stable shadows enabled: **passed**.
  Evidence: `build/stable-shadow-all-test2.log`. Includes 16 anchored-shadow
  frames at origin and `(30000,-60000,500)`, with camera turns/translations.
- Same test binary, `shadow-anchor`, stable flag absent: **expected failure**,
  `non-finite shadow projection`. Evidence: `build/stable-shadow-control.log`.
- Independent-view recording plus stable shadows, `shadow-light-routing`:
  **passed**, `build/stable-shadow-parallel-test.log`.
- The 18 metadata CTests passed again, `build/omwfx-shadow-metadata-test.log`,
  with the runtime DLL directory and VSG bin directory on PATH. An initial rerun
  without that environment had three missing-DLL loader failures (0xc0000135),
  corrected by restoring the test environment, not by changing those tests.
  These do not replace runtime GPU testing or establish visual compatibility.

One initial new-build link error used a VSG helper not exported by the Windows
library. It was corrected to call the exported instrumentation object's virtual
method. A large-coordinate fixture initially omitted instance placement; the
test now mirrors the runtime's outer placement transform and passes. Earlier
failing logs are retained rather than rewritten as passes.

## Private runtime package and visual evidence

Package: `C:/VulkanMW-OMWFX-Shadow-Test`.
Executable SHA256:
`dd25df15b88d29f0022c22fbf89817a223251f46444d658cb2f5eee71fbf87b1`.
Run: `Benchmarks/rafael-visual-1`.
Eight effects: HBAO, VAIO, godrays, DIVE, wetworld, tonemap, SMAA, SMB.
Native graph published eight techniques; normal exit 0, no compressed-upload
failure. Original OpenMW config/settings/input/shader/Lua-storage hashes all
unchanged. Any mod-triggered autosave belongs to the private test configuration.

Images `images/scene-before-fx.png` and `images/scene-after-fx.png` are direct
GPU readbacks, before UI composition, converted from linear HDR for viewing.
They are not photographic edits and are not lossless HDR exports. Earlier
`C:/VulkanMW-OMWFX-Camera-Test` PNGs had a capture-writer orientation error;
the new package fixes the PNG row order without changing game rendering.

**Do not use this run's frame times as a speedup claim.** Capture readback stalls
and visual inspection make it a correctness run. No OpenGL comparison was run.
The last qualified publication speedup remains the separate approximately 9.9%
same-binary cohort in `PUBLICATION-REPAIR-CHECKPOINT-20260926.md`, not this binary.

## Remaining work / cautions

- Main frame times still far above the goal; continue profiling the remaining
  preparation/capture/record/submit and GPU work. Correctness repairs are not a
  substitute for that architectural/performance work.
- Native OMWFX currently exposes the Vulkan negative-Y projection consistently
  with its current input images. Rafael uses projection `[1][1]` for quantities
  such as positive depth tolerances, so this remains an API-convention concern.
  Audit projection, texture origin, derivatives, authored textures/RTs and final
  presentation together. Do not change one matrix sign or patch mod sources
  in isolation. The current camera regression verifies reconstruction but is
  not a full positive-Y/OpenGL shader API equivalence test.
- Haze and screen-space reflections require further visual verification. Scene
  normal/distortion targets are unavailable, authored blending is still rejected,
  arbitrary compressed formats and original imported mip chains are not covered.
- F2 uses the existing frontend, but end-to-end user interaction/hot reload is
  not certified by these automated chain-start tests.
- Stable shadow fitting is independently opt-in and has no performance promotion.
  Its larger conservative caster coverage may cost work; test before promotion.
- Normal saves/mod files are not modified. This does not certify every mod/save.
- SSD free space was approximately 87.9 GiB before this small runtime package.
  No additional SDK/app download or destructive cleanup was performed.
