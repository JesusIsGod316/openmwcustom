# Intro transition regression: nested shadow state slots

## Status

Partial correctness repair, NOT gameplay acceptance or performance promotion.
The user reached the exterior on the final executable, but reported white LAND,
flat water, cyan wood highlights, partially obscured menu text and repeated freezes.
No further game was launched after that session exited.

Checkout: `C:/Users/LSCha/Documents/ChatGPT/OpenMW custom Build-cp4f`.
Branch: `v4.0-cp4f-exterior-closeout`.
HEAD: `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b` plus existing uncommitted work.
No commit, push, CI run or performance promotion. Existing work preserved.

## Causal evidence and correction

The water/sky parameter change removed fragment push constants from state slot 2
and replaced them with descriptor-set-0 UBO bindings in slot 1. This exposed an
existing incremental-publication defect: VSG 1.1.15 updates top-level command
graph maxSlots, but not ViewDependentState's nested shadow preRenderCommandGraph.
Startup reserved slots 0/1; later material descriptor set 1 required slot 2.
The shadow recorder therefore omitted material bindings after startup.

The ordered API trace in `runtime-qc-evidence/20260920-ordered-api-trace/api.txt`
shows pipeline 0x1ed40000001ed4 followed by a set-0 binding and an indexed draw
without the required set-1 binding (around lines 1308204-1308254). Validation
reported VUID-vkCmdDrawIndexed-None-08600, including incompatible material layouts.
The later RGBA readback failure reported an already-lost device; it was not
proof the readback copy caused the failure.

Successful incremental compile results now monotonically grow the affected
views' shadow graph maxSlots even when top-level viewer resources need no update.
Failed compile results do not mutate these limits. The UBO correction stays in
place; invalid overlapping fragment/vertex push constants are not restored.
`OPENMW_V4_STALE_SHADOW_SLOTS=1` is an isolated unsafe causal control, default off.

## Exact files changed in the nested-slot repair step

- `components/render/backend/vsg/vsgsubmission.hpp`: shared post-compile helper;
  both compile entry points propagate requirements into nested shadow graphs.
- `components/render/backend/vsg/viewpipelinebinding.hpp`: remove speculative
  every-bind descriptor dirtying and the invalid order-sensitive layout check;
  retain the earlier render-pass/view-mask-aware pipeline cache repair.
- `components/render/backend/vsg/staticassetrealizer.cpp`: intern replacement
  ViewPipelineBinding objects; keep configurator copyTo pipeline deduplication.
- `tools/v4/cp4/water-transition-tests.cpp`: CPU regression for late slot growth,
  no shrink on GUI publication, and no mutation after failed compilation.
- `tools/v4/cp4/runtime-blocker-contract.py`: reflect real configurator/wrapper
  sharing and require shadow-slot propagation plus the causal-control fixture.
- This repair record.

Earlier changes in the same dirty checkout (view compatibility, auxiliary
readback diagnostics, texture fixture expansion, etc.) remain present. This
list is not a claim that the entire checkout contains only the changes above.

## Build and validation

MSVC 2022 RelWithDebInfo production build succeeded. Logs under
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc`:

- `shadow-slot-final-build.log`: production and GPU fixtures built successfully.
- `shadow-slot-unit-build.log`: registered compiled test targets rebuilt.
- `shadow-slot-ctest.log`: 14/14 passed.
- Seven CP4 source contract scripts passed, including runtime blocker contract.
- Python diagnostic tests: 16/16 passed. `git diff --check` passed.
- `shadow-slot-regression-control.log`: expected exit 1 with control enabled,
  reporting that late material slot did not reach the shadow graph.
- `shadow-slot-transition.log`: first repaired water/map/late-object fixture
  completed with zero validation errors.
- `shadow-slot-final-transition.log` and `shadow-slot-final-gui.log`: behavioral
  assertions exited 0, but final fixture validation is NOT a clean pass because
  separate swapchain/image-view storage-usage errors were logged.
- `shadow-slot-final-texture.log`: stopped only owned fixture PID 96628 after
  prolonged runtime and existing validation failures. Its 150-frame residency
  assertions did not complete; result INCOMPLETE, not passed.

Final openmw.exe SHA256:
`6F3A1AAA8B73968D0027B6C12277520E38BC18DE16988CBE438256A67FF7BECA`.

## Actual new-game runs

Evidence root: `C:/Users/LSCha/Documents/My Games/OpenMW/runtime-qc-evidence`.
Both runs used private writable profile folders and `--skip-menu --new-game`;
neither loaded the regular save. Strict QC and Khronos validation were enabled.

1. `20260920-shadow-slot-repair-newgame`, first repaired hash
   `98849544AA3DEFC6856E210FBF287E7CFF3C2B6FAED3ED52598505979BFDD1F0`:
   entered ship, last sampled frame 1404 completed, zero validation errors,
   normal exit. Not an exterior/performance test.
2. `20260920-shadow-slot-final-newgame`, final hash above: user played through
   the ship to exterior, reported serious visual/performance failures, exited
   normally at 08:27:13.972. No draw-binding validation errors or device loss
   recorded. Ten swapchain/image-view validation errors remain:
   VUID-VkSwapchainCreateInfoKHR-imageFormat-01778 and
   VUID-VkImageViewCreateInfo-usage-02275 (sRGB storage usage).

Implicit-layer exclusion did not eliminate these latter errors. RTSS was running
again, but this record does not attribute the remaining errors or freezes to it
without a controlled comparison. No global overlay setting was changed.

The user screenshots around 08:25 show multi-second frames and roughly 30 GB
system RAM use. Sampled frame 2313 took 6051.5929 ms, with static_sync
4816.3319 ms and dynamic_realization 659.7778 ms. Frame 2343 took 545.0904 ms,
with static_sync 0.3505 ms, dynamic_realization 86.5365 ms, vulkan_capture
85.7802 ms and submit_present 262.8611 ms. Stages overlap; DO NOT SUM THEM.
The capture reached its detail limit, and strict validation adds overhead.
These are diagnostic observations, not comparable production benchmarks.

Normal openmw.cfg and the protected Leon_Valerius/_zZz__Wake_up.omwsave were
rehashed and match the pre-repair identities. The test window loaded no save.

## Remaining acceptance blockers

Investigate long static synchronization and continuing steady-state frame cost;
repair white terrain, water appearance, wood material interpretation and GUI
occlusion. Resolve the remaining swapchain validation errors. Repeat targeted
fixtures with clean validation, then test the exact new-game route without
heavy validation for runtime performance. Do not claim this build is ready or
the exterior is fixed based on the intro transition passing.
