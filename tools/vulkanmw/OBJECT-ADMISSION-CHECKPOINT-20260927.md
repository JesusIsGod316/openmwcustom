# Vulkan object-admission checkpoint — 2026-09-27

## Scope and source state

Worktree: `C:/Users/LSCha/.codex/worktrees/vulkanmw-phase3c-metadata/OpenMW custom Build`.
Branch: `vulkanmw/phase3c-native-runtime-substitution`; base HEAD
`336a4cf8668afa1a4f03069785421e9a5e2b74ef`. The worktree remains dirty
with earlier Vulkan changes. This checkpoint is not a commit, push, release, or
runtime promotion. Git/source and the benchmark manifests are the authorities
for the exact state and measurements.

## Mechanism

`OPENMW_VK_CACHED_OBJECT_ADMISSION=1` caches the corrected-model and
model-playback admission decision on each engine-owned `Animation`. The entry
is valid only for the current RenderWorld epoch and object-root revision;
`setObjectRoot()` also clears it. Thus ordinary mod assets do not need to emit
new notifications. Enabled objects still pass the engine state check, and
admitted objects still use the native producer or the existing evaluated-OSG
fallback. The switch defaults off; disabling it restores the prior per-frame
lookup. This does not remove OSG animation updates, object capture for admitted
objects, shadow recording, or GPU lighting.

The private benchmark driver has an explicit `--cached-object-admission` arm
and a four-run ABBA cohort. It rejects using that arm for OpenGL. No normal
configuration or saves were changed.

## Fixed-scene A/B evidence

Package: `C:/VulkanMW-Admission-Test`; executable SHA256
`2d928d6aba72c38aa9a32c1e1ac5f6d2348cacf145ccdd8a485ab10c15542770`. Packaged
`source-changes.json` SHA256:
`dfb860a4c05a4cddaa66473bf3b8abe4c4055b446e6c8c1e78999719b97bee5e`.
All arms used that executable, full 1920x1080 resolution, the same content,
fresh private new-game scene and random seed, 15-second warmup, and about
30-second steady wall-clock sampling. All exited 0, had 15–16 healthy sampled
frame profiles, and reported the original configuration-chain hashes unchanged.
The sole planned control difference was the admission-cache switch.

| Arm | Mean frame ms | Object-capture CPU ms | Recording CPU ms |
| --- | ---: | ---: | ---: |
| control A | 67.5859 | 8.4282 | 12.7243 |
| cached A | 66.4308 | 7.9438 | 13.1590 |
| cached B | 67.1372 | 7.7860 | 12.8530 |
| control B | 68.4803 | 8.4520 | 12.2670 |

Arithmetic means of run means: control **68.0331 ms**, cached **66.7840 ms**,
a scene-specific **1.2491 ms (1.84%)** reduction. Object-capture CPU fell
from **8.4401** to **7.8649 ms** on average. During the cached steady run,
sampled frames reused admission for about 2,638 objects and skipped capture
admission for about 2,007. Main/shadow GPU intervals were broadly unchanged.
This is useful but not a major FPS repair; it does not approach the user's
OpenGL-or-better goal. The fixed scene and sparse CPU samples do not establish
all-content, save round-trip, visual, or live-play compatibility. Logs also
contain existing nonfatal script/configuration errors, so normal exit is not a
mod-compatibility certification.

## Validation and next work

MSVC `RelWithDebInfo` target `openmw` built successfully from the modified
source; the existing cells-settings numeric-conversion warnings remain.
`test-benchmark-control.py` passed 11/11. The four game runs provide an
additional exercised control/fallback check. The source diff passed
`git diff --check` aside from Git's pre-existing Windows line-ending notices.

This cache can be offered as an optional Vulkan fast path, but should not be
mistaken for completion. The measured recording path remains about 12–13 ms,
main GPU about 31–32 ms, shadow GPU about 18 ms, and the end-to-end frame about
67 ms in this cohort. The previously validated model-group publication,
update-only chunk transactions, and resident pipeline inventories are separate
switches enabled in the private playable profile, not in this baseline `retained`
cohort. Next major work should reduce shadow/main recording and GPU lighting
cost without dropping mod/save semantics or relying on post-processing changes.
