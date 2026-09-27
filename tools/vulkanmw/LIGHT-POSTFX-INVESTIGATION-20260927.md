# Vulkan lighting and post-processing checkpoint — 2026-09-27

## Authority and status

Worktree: `C:/Users/LSCha/.codex/worktrees/vulkanmw-phase3c-metadata/OpenMW custom Build`.
Branch: `vulkanmw/phase3c-native-runtime-substitution` at base HEAD
`336a4cf8668afa1a4f03069785421e9a5e2b74ef`. Source changes remain
uncommitted alongside earlier dirty work; no push or runtime promotion.
The Shared Project Context Archive control/current-state/decision material was
consulted earlier in this ongoing implementation. The exact results below are
from the local benchmark manifests, not inferred from screenshots.

The user's larger goal (Vulkan at least as fast as OpenGL with mod/save and
post-processing compatibility) is **not achieved**. The experiments below
separate shader cost, visual correctness, and the still-large base-renderer cost.

## Screenshot and post-processing visual result

The user's near-white screenshot was taken on 2026-09-26 at 23:28, before the
native OMWFX camera-data repair documented in
`OMWFX-CORRECTNESS-CHECKPOINT-20260927.md`. In the later direct GPU capture
`C:/VulkanMW-OMWFX-Shadow-Test/Benchmarks/rafael-visual-1/images`, terrain and
buildings retain textures. This establishes that the severe earlier whiteout is
absent in that later capture, **not** that every weather or shader combination is
correct.

Two subsequent same-binary, visual-only runs in `haze-isolation-a` and
`haze-isolation-b` held the scene and HBAO effect constant. Adding Rafael's VAIO
created a heavy grey/blue haze; HBAO alone was clear. VAIO intentionally has
fog/atmosphere controls, so this isolates the effect but does not prove a
Vulkan-vs-OpenGL parity defect. Projection convention, authored fog settings,
and depth/texture coordinates need a same-content visual reference before
changing the engine or the mod. These GPU readback runs are **not** performance
measurements.

## Fixed-scene cost of the native effect chain

Package: `C:/VulkanMW-OMWFX-Shadow-Test`, executable SHA256
`dd25df15b88d29f0022c22fbf89817a223251f46444d658cb2f5eee71fbf87b1`.
The benchmark uses private content/settings/user data, 1920x1080, fixed new-game
scene and seed, 15 seconds warmup and approximately 30 seconds measured, with
the same binary and opt-in stable-shadow route for every arm. Each run exited 0,
published the requested number of techniques, and reported the original
configuration chain unchanged. The one empty-chain arm is a diagnostic, not a
fully post-processing-disabled or pixel-equivalent control.

| Run under `Benchmarks` | Chain | Frames | Mean frame ms |
| --- | --- | ---: | ---: |
| `postfx-cost-full-a` | HBAO, VAIO, godrays, DIVE, wetworld, tonemap, SMAA, SMB | 422 | 71.0131 |
| `postfx-cost-hbao-a` | HBAO | 441 | 68.1447 |
| `postfx-cost-hbao-b` | HBAO | 436 | 68.8093 |
| `postfx-cost-full-b` | same full chain | 426 | 70.4265 |
| `postfx-cost-empty-a` | empty native chain | 442 | 67.8454 |

The two-run means are 70.720 ms full versus 68.477 ms HBAO-only: about 2.24 ms
additional whole-frame time. The single empty-chain arm is about 2.87 ms faster
than the full-chain mean. This fixed scene remains about 67.8 ms even with zero
effects; moving bloom/clouds from OMWFX to another engine pass cannot explain
or eliminate that base cost. These are diagnostic short runs, not a claim for
other scenes, weather, live movement, visual equivalence, or F2 interaction.
The screenshot's instantaneous 7 FPS is not contradicted by a different fixed
scene's mean; stalls and camera/scene differences can matter.

## Local lighting result and quality boundary

The native renderer's classic-falloff control supplies approximately 90 active
local lights to the fragment path. Clustered lighting uses radius fade and is a
**visual change**, so the large classic-to-clustered GPU reduction is not an
equal-quality optimization. The new `OPENMW_VK_TILED_LIGHTS=1` path builds
per-tile masks only where radius-faded lighting is already in force; classic
falloff retains its control route. Within the clustered mode, a same-binary
ABBA cohort in `C:/VulkanMW-Particle-Split-Test/Benchmarks` found:

| Arm | Frame mean ms | Main GPU interval ms | Shadows GPU interval ms |
| --- | ---: | ---: | ---: |
| `tilevalue-0-...-clustered` | 60.1625 | 16.6600 | 17.8744 |
| `tilevalue-1-...-fastlights` | 61.1585 | 13.86 | 17.90 |
| `tilevalue-2-...-fastlights` | 60.0120 | 13.90 | 17.87 |
| `tilevalue-3-...-clustered` | 60.4546 | 16.68 | 17.91 |

The tile mask saves about 2.8 ms inside the main GPU interval but does not
produce a repeatable whole-frame gain here. CPU command recording remains about
12–13 ms and object capture about 8 ms. GPU intervals can overlap CPU work and
must not be subtracted from whole-frame time. All four profile summaries report
no health issues. The broader classic-to-clustered experiment's frame means
were 61.98/60.92/60.91/62.23 ms, but again it changes attenuation quality.
Keep lighting controls independently switchable and default-off; do not claim
this achieves the requested FPS.

## F2 access and test package

The Vulkan OMWFX frontend could already execute an effect chain, but
`A_TogglePostProcessorHUD` still required an OpenGL graphics context. The
opt-in native frontend now allows the existing F2 HUD to open without one:

- `apps/openmw/mwrender/postprocessor.hpp`: exposes whether the native frontend
  was initialized.
- `apps/openmw/mwinput/actionmanager.cpp`: accepts that frontend for the F2 HUD;
  the old OpenGL gate remains intact.

No effect source, normal saves, or normal user settings were changed. This is
frontend access, not a claim that every F2 control or live reload path has been
user-interaction-tested. Unsupported normal/distortion targets and VAIO haze
remain open issues.

Private package: `C:/VulkanMW-F2-Test`, executable SHA256
`8c2b6c88db2341de7bad759340d9dbb4b711b745bbea3f6326ad50d0d51f74da`.
Its source manifest is `source-changes.json` (SHA256
`f99e253ff7494367819dab403a5fdb55d23062c5b8c6c846497b5cdf9715dc9b`).
The `Benchmarks/Play-F2` profile has an isolated menu launcher
`Start-Vulkan-Clean.cmd`, the eight-effect chain, and stable sun shadows. Its
automatic new-game smoke exited 0 with eight native techniques and original
config-chain hashes unchanged. The standalone profile verifier also passed.
Do not start `openmw.exe` directly if testing this private setup; use the launcher
so the ordinary OpenMW configuration and saves stay outside writable paths.
The package carries all earlier uncommitted renderer changes, not just this F2
gate. The performance measurements above used the earlier package/binary, not
this new executable; its one smoke mean of 73.874 ms is not an A/B speed claim.

## Validation and next boundary

After the F2 source edit, MSVC `RelWithDebInfo` built `openmw` and
`openmw-v4-rendering-capture-tests`; capture tests passed **47/47**. Python
publication-cohort tests passed **5/5** when invoked by their hyphenated file
name. The native OMWFX GPU suite ran against the F2 package's defaults and
passed **90 pixel checks** (45 chain/depth/history, 18 imported image, 27 camera
API). `git diff --check` passed. An initial FX-test invocation used a build-tree
defaults file in place of the package defaults and exited 1 without output;
rerunning with the package defaults and runtime DLL path passed. These checks
do not certify live F2 keyboard interaction, shader parity, full mod/save
compatibility, or acceptable performance.

The next performance mechanism should attack the measured CPU producers and
recording path while preserving the existing compatibility fallback. Repeating
screen-space shader rearrangements or GPU-only tile work will not close a
roughly 60–70 ms frame by itself. Any new speed claim needs a same-executable,
equal-quality controlled run plus visual and compatibility checks.

## Follow-up shader and occlusion probes (not promoted)

An exact-zero-specular guard was prototyped in the legacy Vulkan shader and
tested with a same-binary, full-resolution ABBA cohort under
`C:/VulkanMW-Specular-Test/Benchmarks`. The binary SHA256 was
`04e7768b7747e4a15a7689eb30112c28ee5030a3fd21148e7c57d902fcbc3468`.
Mean frame times in control/guard/guard/control order were
67.7924 / 67.6411 / 67.1914 / 66.8244 ms. Mean main-GPU intervals were
32.016 / 32.675 / 32.646 / 32.138 ms. The guard did not improve throughput
and increased the sampled main-GPU interval, so its source and benchmark
switch were removed. This package preserves evidence of the rejected prototype;
its executable is not the current source's executable. All four arms exited 0
and verified the normal configuration-chain hashes unchanged. This test does
not establish pixel equivalence across arbitrary mods.

The same package's separate `land-depth-on` run enabled the already-existing,
conservative weighted LAND software occluder. It admitted roughly 8,700
terrain triangles and rejected about 16 of roughly 600 candidates in the
steady exterior. Its frame mean was 67.7125 ms against the above control
means, while sampled CPU visibility grew from about 0.226 to 1.098 ms and
the main-GPU interval fell from about 32.138 to 31.544 ms compared with the
nearest control. This was one on-arm, not a full ABBA promotion test. The
visibility option stays default-off: in this scene its CPU cost outweighed
the GPU saving, and it does not help the shadow or water views.
