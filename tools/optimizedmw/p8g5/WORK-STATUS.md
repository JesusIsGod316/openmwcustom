# P8G5 reconstructed temporal/DLSS track — local source checkpoint

Base: 8ed1f63beed5fa2d7b3e46e272cbaa2f9b27f228 (P8G4).
This branch is separate from p8u1-resource-donors-local. Neither is pushed.
No P8G5 workflow, game binary, launcher mode or NVIDIA DLL is added.

## Recovery gap

Earlier chat reported local-only commits da5cac7 and 79b6ecb on
p8g5-dlss-temporal-local. Their actual source/Git objects were not recoverable
from the current runtime, Project/Library search, Drive search or remote refs.
This is RECONSTRUCTED source with a new identity, not recovery of those commits.
The previous claims about integrated camera/PostProcessor/RigGeometry and
MorphGeometry edits cannot be carried forward as existing code. They must be
reimplemented and verified before being marked present.

## Implemented and tested here

- Render-owner temporal history with prepare/commit/abort tickets. An aborted or
  skipped evaluation cannot become the previous rendered history. Duplicate and
  stale commits are rejected; invalid matrices/inputs invalidate history.
- Independent view/world/camera/projection epochs, extents, explicit cuts,
  discontinuous frame sequence and depth convention trigger history reset.
  Ordinary cell crossings do not inherently reset history.
- Halton jitter in render pixels, separately retained unjittered matrices,
  current/previous view-projection and clip-to-previous-clip transforms.
- Current-to-previous motion in top-left render pixels; CPU static-depth
  reconstruction and explicit current/previous-world-position helpers.
- Input metadata gate for frame/view/device/revision/extents/format/alias,
  HUD-free color, exposure policy and complete dynamic motion. Camera-only
  motion cannot pass the complete-input gate.
- Candidate camera/static motion fragment shader tested against a real RG16F
  OpenGL render target. It is not yet installed or called by PostProcessor.
- Three native tests pass, including 5 x 256 GPU pixels compared to the CPU
  motion contract. The same three pass under Clang ASan/UBSan, with only Mesa
  GLX process-lifetime allocations excluded from leak checking.

## Still required before real DLSS

1. Reintegrate final rendered per-view camera state and low-resolution depth/
   color into the current PostProcessor/Canvas without leaking jitter into
   culling, shadows, UI or alternate views. The final camera after any OSG
   near/far adjustments must match the actual depth buffer.
2. Implement previous world transforms and previous deformation for actors,
   skinned/morphed objects, nested NIF transforms, first-person/body views and
   vertex-animated grass. Static-depth reprojection alone is not enough.
3. Define translucent/particle/water handling and disocclusion policy; enforce
   history reset for genuinely discontinuous identities/cameras/resources.
4. Build and test the narrow same-GPU OpenGL/Vulkan resource-sharing bridge,
   explicit layouts, semaphore/fence ownership, bounded in-flight lifetime,
   device matching, cancellation and device-loss/resize handling. No CPU image
   readback in the intended runtime path. No shared-context correctness claim
   is established by the current same-context pixel test.
5. Pin/validate the actual supported NVIDIA SDK/runtime, initialization,
   application identity and distribution requirements. Query optimal render
   dimensions from the runtime. Keep native/NIS fallback intact.
6. Verify installed shader/runtime identity and test on NVIDIA hardware. Measure
   total frame latency and GPU time, including bridge cost, at matched quality.
   No speedup, visual compatibility or working DLSS is claimed yet.

Reference checked during reconstruction:
https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md
Use the current official SDK at integration time; do not assume the requirements
or binary version from a prior chat remain current. The row-major utility is a
storage adapter, not a tested Streamline binding.

DLSS Super Resolution is the first required working consumer. Frame generation
is out of scope. Extensible temporal inputs are not a promise of DLSS 5 API
compatibility; a separate future neural-rendering SDK needs its own audit.
