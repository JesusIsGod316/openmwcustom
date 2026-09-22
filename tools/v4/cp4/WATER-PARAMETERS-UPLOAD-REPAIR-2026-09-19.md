# Water parameter repair and static upload candidate — 2026-09-19

## Scope and source identity

Checkout: `C:/Users/LSCha/Documents/ChatGPT/OpenMW custom Build-cp4f`.
Branch: `v4.0-cp4f-exterior-closeout`.
HEAD: `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`, with existing uncommitted work preserved.
No commit, push, CI run, gameplay launch, or save load was performed in this repair turn.
The context archive control block and relevant history were consulted before implementation.

The prior map-context lifetime repair remains intact. This is NOT CP4F acceptance or CP6 optimization promotion.

## Latest gameplay evidence

Evidence directory: `C:/Users/LSCha/Documents/My Games/OpenMW/runtime-qc-evidence/20260919-231314-gameplay-88640`.
User tested executable SHA256: `1371694F780FA0430C573AEF26C747F98EE4CB1D6E1C1CA995E1C8BACC74309A`.
The user reached the exterior through New Game and the ship hatch. This run exited normally but failed visual/performance acceptance.
Screenshots show a large pale-blue overlay, white terrain, bright wood artifacts and roughly 1 FPS.

Raw exterior samples recorded static synchronization at 702.1566 ms and 640.7892 ms on frames 5021 and 5051.
Later sampled static synchronization was about 0.35 ms, while dynamic capture remained about 73–76 ms and dynamic realization about 63–68 ms.
Exterior transition was 17022.9975 ms, including terrain wait 13448.5008 ms.
These are inclusive, overlapping stage timings; do not sum them or infer GPU time from the overlay's utilization percentage.

## Active correctness repair

- Water and sky previously declared fragment push-constant ranges at offset 0, overlapping VSG's vertex matrix range 0–127. They then issued fragment-only writes, while VSG issued vertex-only matrix writes. This violates the overlapping-range stage-mask requirement in Vulkan `vkCmdPushConstants`, VUID-offset-01796: <https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushConstants.html>.
- Move water parameters to set 0/binding 2 and sky parameters to set 0/binding 0 uniform buffers. Keep the standard 128-byte vertex transform range, with no assumption that hardware supports more than 128 push-constant bytes.
- Retain dynamic data registration and dirty notifications so changes reach the viewer's transfer task.
- Water samples reflection/refraction using fragment screen coordinates and inverse render extent. Remove corner UVs computed with `abs(clip.w)`, which do not remain valid across near-plane clipping of the large water plane.
- These are concrete faults, but attributing the entire user's overlay to them still requires the actual hatch route to be retested.

## Default-off static upload candidate

VSG 1.1.15's CompileManager records uploads and waits for completion per compile call. The existing path compiles each new static placement/population separately.
`OPENMW_V4_BATCH_STATIC_UPLOADS=1` instead collects pending roots and compiles them once before residency/scene publication. Failed batch compilation cannot publish its pending roots. Existing view ownership guards and retirement remain unchanged.
Unset or any value other than `1` retains individual compilation. Nothing in the launcher or normal profile enables the candidate.
`OPENMW_V4_REBUILD_STATIC_PLANS=1` continues to force the earlier full-planning control.
Add bounded `static_plan`, `static_compile`, and `static_mutation` diagnostics to distinguish planning from uploads during the next real load.

The synthetic 64-object publication is a correctness fixture, not a gameplay benchmark. Preliminary timings varied: batch 272.045 ms versus individual 263.043 ms; final executable observations were individual 270.681 ms, batch 244.364 ms, forced full planning 267.742 ms. There is no controlled repeat-series or demonstrated representative improvement. Therefore batching remains OFF by default.

## Validation

Build directory: `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc`.

- MSVC RelWithDebInfo production executable and all linked recovery targets: PASS. Final incremental build log: `water-batch-verified-build.log`.
- CTest: 14/14 PASS (`water-final-ctest.log`), including shader package checks.
- Seven CP4 source contracts: PASS. The prior sky contract explicitly required the invalid fragment push-constant implementation; it now requires the separate uniform descriptor. Water contract also rejects overlapping fragment push constants.
- Python diagnostic tests: 16/16 PASS.
- Production water/sky offscreen pixels: PASS. Checks sample pixels above/below the horizon, both reflection/refraction inputs, screen-coordinate sampling, live sky/water uniform updates, cave water, changed water height crossing the camera, disabled water, and reversed-depth occlusion. This is not full native-water visual parity.
- Vulkan transition fixture: PASS on default, batch, and full-plan controls. Includes 64-object publication, eight map create/retire/new-object cycles with bounded compile-context count, cave/underwater/outdoor transitions, late objects and GUI loading (`water-final-*-test.log`).
- GUI isolation GPU fixture: PASS (`water-final-gui-test.log`).
- Texture residency GPU fixture: PASS, 150 frames; warm and final tracked pool bytes both 85551600; one decoded payload and one GPU image across 96 independent graphs (`water-final-texture-test.log`).
- `git diff --check`: PASS.
- No full Vulkan validation-layer run was available; do not call these results validation-layer clean.

During test development, an explicit VSG ref_ptr constructor mismatch caused the first test build to fail and was fixed before the final build. The new pixel readback initially reused a signalled fence, making one sky readback stale; each readback now owns a fresh fence. Final runs use that corrected harness. Earlier logs are retained rather than overwritten.

Executable SHA256: `CA242311770FF4771C76902460DBCC163FCACD67900E59D9D013B21E1F28326E`.
Shader manifest SHA256: `bc4c6b6493a7f229a5ed99b109031328ba882cde8e7ca40dd252546ee903e411` (82 files).

## Exact files changed this turn

1. `components/render/backend/vsg/watersurface.cpp` — uniform descriptor and screen-coordinate sampling.
2. `components/render/backend/vsg/skybackdrop.cpp` — uniform descriptor.
3. `components/render/backend/vsg/vsgruntimehost.cpp` — default-off upload batch and static timing separation.
4. `tools/v4/cp4/water-pixel-tests.hpp` — new offscreen production-shader pixel test.
5. `tools/v4/cp4/water-transition-tests.cpp` — invoke pixel checks and test multi-object publication.
6. `tools/v4/cp4/water-environment-contract.py` — non-overlapping parameter and candidate-control checks.
7. `tools/v4/cp4/population-environment-contract.py` — replace obsolete sky push-constant requirement.
8. `tools/v4/cp4/runtime-blocker-contract.py` — preserve static early-out assertion after timing-scope refactor.
9. This repair record.

## Remaining defects / acceptance boundary

White terrain still lacks native LAND texture layers/blend weights/UV publication in `v4terrainsource.cpp`. Wood PBR artifacts are not fixed here. Water still has basic procedural ripples rather than native normal-map texture detail. The CPU actor/effect capture, realization costs, terrain preparation time, and overall exterior FPS remain unresolved.

No settings, mods or saves were changed. Normal `openmw.cfg` hash is unchanged at `70257CFB9851831D850D772596A66DBFA6920295D6E9A2CE141D424EB83373C0`; settings/input/shaders/global/player storage hashes match the last-run manifest. Protected `Leon_Valerius/_zZz__Wake_up.omwsave` remains `D34685C838BD1B4318DB34B1E0C1F00A7A2FE9A990FD21A120973297083AF99B`.

Next visual acceptance route remains normal menu -> New Game -> prison ship -> hatch -> Seyda Neen. Do not substitute an existing save or direct exterior startup, and do not infer performance acceptance from the GPU fixtures.
