# VulkanMW Phase 3 — Native Controller and Animation Runtime

Status: Phase 3A implementation active; focused source gate pending
Branch: vulkanmw/phase3-native-controller-animation
Parent: vulkanmw/phase2-native-static-world@93e94bc6b9bc778e65c8969cea5a27363dcd17b6

## Accepted Phase 2 handoff

The user hardware run completed cleanly on the exact Phase 2 package. Phase 2 is accepted as an
architecture/runtime checkpoint, not as a performance improvement. Existing Vulkan rendering defects
remain visible but the user observed no new Phase-2-specific visual regression.

Phase 3 therefore starts from the exact native static-world source head rather than from the packaging
commit.

## Phase 3 objective

Move dynamic authored animation semantics away from legacy scene mutation and into immutable native
programs plus explicit per-frame semantic outputs.

The complete Phase 3 target remains:

- local transform controllers
- external .kf animation tracks and OpenMW group selection
- visibility/switch state
- morph weights
- texture transforms
- material color/alpha state
- controller timing and extrapolation
- text-key/event compatibility
- first/third-person and attachment-compatible pose publication

The OpenGL path remains the compatibility oracle and is not replaced by this work.

## Phase 3A — embedded NIF controller program

Phase 3A introduces `RenderNative::NifControllerProgram`.

The source compiler now converts source-embedded:

- `NiKeyframeController` / `BSKeyframeController` transform tracks
- `NiVisController` visibility tracks
- `NiGeomMorpherController` morph-weight tracks

into immutable backend-neutral controller tracks keyed by the translated model's stable
`ModelNodeIndex`.

Timing follows the current compatibility implementation for frequency, phase, start/stop and
Cycle/Reverse/Constant extrapolation. Float/vector Hermite, constant and linear interpolation and
quaternion slerp are represented directly. Legacy visibility step behavior is represented separately
rather than approximated.

The controller compiler uses the same parsed `Nif::FileView` already consumed by
`NifSemanticCompiler`, so normal native model resolution still performs one winning-VFS parse.

`NifAssetService` retains the immutable controller program beside the stable published model
metadata, making later dynamic instances able to reuse compiled tracks without reparsing the model.

### Important Phase 3A boundary

This checkpoint does **not** yet replace the live actor pose capture path.

External `.kf` group priority/blend-mask selection, animation-state timing, text-key dispatch,
material/texture controllers and final FrameRenderState/VSG application are Phase 3B/3C work.

Phase 3A is the native data/evaluation foundation required to remove those live scene dependencies
without changing gameplay semantics.

## Phase 3A acceptance gate

Before production routing:

1. Phase 0 architecture guard stays green.
2. Phase 1 NIF semantic compiler contract stays green.
3. Phase 2 static-world/terrain/groundcover contract stays green.
4. Phase 3 native controller source has no legacy scenegraph dependency.
5. Native timing/interpolation smoke passes.
6. New controller compiler source compiles on the focused Linux gate.
7. Full Windows Vulkan build/link gate passes before runtime promotion.

## Next implementation slices

### Phase 3B — native .kf source and OpenMW animation-state binding

Compile winning-VFS `.kf` data directly into named native transform tracks while preserving:

- first duplicate-name controller wins
- last-added animation source priority
- group start/loop/stop text keys
- speed multiplier and loop count
- OpenMW blend masks and animation priority
- accumulation/root-motion semantics
- full-body first-person and hybrid-animation compatibility gates

The existing gameplay `Animation` state machine remains authoritative initially, but it will publish
native selected-track/time state instead of attaching controller callbacks for Vulkan consumption.

### Phase 3C — direct dynamic-frame publication

Publish native controller outputs into RenderCore/FrameRenderState so Vulkan no longer reconstructs
normal controller results from evaluated legacy nodes.

The first production substitutions should target the already-existing semantic lanes:

- skeleton pose publication
- morph weights
- dynamic local transforms

Visibility/switch and material/texture state receive explicit RenderCore lanes before their old
capture paths are removed.

## Non-goals for this checkpoint

- no Phase 4 GPU skinning claim
- no effects/particles migration
- no visual parity claim
- no performance improvement claim until matched hardware data
- no removal of the OpenGL control path
