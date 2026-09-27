# VulkanMW Phase 3C — Native Runtime Substitution

Status: implementation active; source/build/runtime acceptance pending
Parent: vulkanmw/phase3-native-controller-animation@87962b7d1fda6ca1d5333ee005530ce1ed2731c9

## Purpose

Phase 3A and 3B intentionally built the native controller and external-KF representations first.
Phase 3C is the first production runtime substitution step: stop doing a second evaluated-OSG
skeleton walk for actor poses when the existing OpenMW animation state can be represented by the
native KF path.

This is migration progress toward the final renderer architecture, not a promotion contest against
Phase 2. Performance is retested only after the coherent 3C substitution is build-green and usable.

## Runtime ownership

OpenMW's existing Animation state machine remains authoritative for:

- animation group selection and source priority
- blend-mask ownership
- current animation time, loops and text-key events
- gameplay state and movement accumulation

The Phase 3C adapter snapshots only selected source path, current time, blend-mask bone membership,
and accumulation semantics. It does not read evaluated bone matrices.

V4NativeAnimationRuntime then:

1. compiles/caches the winning-VFS KF source through the Phase 3B compiler;
2. samples the active native transform tracks at the authoritative OpenMW state time;
3. reconstructs skeleton-local transforms from authored bone-local semantics;
4. publishes them directly as SkeletonPoseInput;
5. skips the bridge's evaluated Skeleton::updateBoneMatrices/name-walk/global-to-local copy for
   successful native actors.

## Fail-closed compatibility boundary

The existing OSG evaluated-pose path remains the exact fallback. Native substitution is skipped while
any of these semantics are active or unsupported:

- smooth animation interpolation is currently active;
- hybrid first-person visual blending is active;
- a procedural RotateController (head/body/weapon pitch) is enabled;
- the active source is not a native .kf clip;
- an active compatibility-bound bone is absent from the compiled native clip;
- an authored bone-local transform cannot be represented safely as the NIF
  translation/quaternion/uniform-scale controller contract;
- native evaluation produces non-finite data.

OPENMW_V4_LEGACY_ANIMATION_CAPTURE_CONTROL forces the previous actor pose capture path for matched
same-executable A/B testing.

## Neutral skeleton semantic addition

RenderCore::BoneRecord now retains:

- sourceParentPath — the exact static transform chain between skeleton bones;
- sourceLocal — the exact authored local transform of the controlled bone node.
- sourceAnimationBoundary — whether the authored replacement boundary is known;
- sourceControllerFlags — controllers attached to that exact source node;
- sourceParentControllerFlags — controllers anywhere in the collapsed ancestor path;
- sourceParentPathNodes — ordered, normalized source names along that ancestor path.

This allows native animation to replace only the authored bone transform while preserving static
non-bone ancestors. bindLocal and inverseBind remain authoritative existing skeleton fields.

The forced actor skeleton builder must retain the same metadata as the direct NIF translator.
See [runtime skeleton provenance audit](PHASE3C-SKELETON-PROVENANCE.md) for the NPC/composition
publication defect, its repair, and the scoped compatibility boundaries.

## Phase 3C first-slice boundary

This slice removes duplicate actor **skeleton-pose capture** on eligible steady KF frames. It does
not yet remove the OSG update-only world itself; that remains a later roadmap phase.

MorphCollector capture remains as the compatibility source in this first 3C slice. Once skeleton
substitution is proven build/runtime correct, the next 3C slice can use the already-native morph
controller programs to remove the duplicate morph readback where source/mesh correspondence is
unambiguous.

## Measurement

The gameplay diagnostics expose:

- native_animation_runtime.native_actor_poses
- native_animation_runtime.legacy_actor_poses
- native_animation_runtime.sampled_tracks
- native_animation_runtime.seed_failures
- bounded native_animation_fallback reasons

A useful 3C runtime test must show non-zero native actor pose substitution on the real workload.
No performance claim is made from source/build success alone.

The permanent test launcher accepts `--legacy-animation-capture` for the matched evaluated-pose
control. The benchmark deliberately clears inherited `OPENMW_` settings; merely setting the
environment variable outside this launcher is insufficient. The explicit switch is applied after
that cleanup and recorded in the run manifest. Omit it for native substitution. Both arms use the
same executable, mods, private profile, seed, warmup, and measured interval.
