# Phase 3C: runtime skeleton provenance repair

Source baseline: `805d5eb277987c3357affdd85698ed51c781bb30` on
`vulkanmw/phase3c-native-runtime-substitution`. This is a Phase 3C coverage repair,
not Phase 4 and not evidence that Vulkan already outperforms OpenGL.

## Audit finding

There are two production skeleton builders. `niftranslator.cpp` publishes the
authored animation boundary for skin-required bones. `buildForcedActorSkeleton`
in `actormodelcomposer.hpp` previously populated only name, parent, bind-local,
and inverse-bind. Its new BoneRecords defaulted all six source-boundary fields.

The runtime does not necessarily use the translator's skeleton. In
`V4EngineRenderBridge`'s actor publication path, NPCs explicitly select a forced skeleton
even when the translated base model already has a skin skeleton. Consequently,
fixing the direct translator alone could not fix normal NPC runtime coverage.

## Runtime routes

| Actor route | Authoritative skeleton lineage |
| --- | --- |
| Normal NPC | `NpcAnimation::updateNpcBase` selects the winning base model by race, sex, beast/werewolf and view state. `setObjectRoot` records `mV4SourceModel`. The bridge publishes that model and creates/caches its forced skeleton. |
| Equipment/composed NPC | Same base skeleton. `composeActorModel` remaps part skin bone names to the master skeleton. It publishes a ModelRecord, not a new SkeletonPayload. Donor part bind transforms must not overwrite master provenance. |
| Creature | `CreatureAnimation` / `CreatureWeaponAnimation` select their own base model. Use the translated skin skeleton when present; otherwise use the forced builder from that same base ModelRecord. |
| First-person / full-body first-person | NPC base selection still determines the authoritative source. The same forced builder applies; native hybrid/procedural eligibility gates are unchanged. A different first-person source gets its own cache identity. |
| Rebuilt model / changed equipment | Base-source changes select the corresponding published skeleton/cache key. Attachment changes may reuse `mComposedActors`, which stores models, not replacement skeletons. The instance retains the chosen master skeleton handle. |

The final instance handle is resolved from RenderWorld and passed to
`V4NativeAnimationRuntime::captureSkeletonPose`. No later field-by-field skeleton
clone or merge was found in this route. `mForcedActorSkeletons` caches immutable
payloads by source model, while `mComposedActors` caches only composed ModelRecords.

`Animation::injectCustomBones` adds donor subtrees to the compatibility graph.
This patch does not claim native support for arbitrary extra injected bones.
Unmapped/unsupported cases retain existing compatibility rejection/fallback.

## Repair

For each selected forced bone, walk the already-validated authored ModelRecord
parent chain to the nearest selected parent bone. Accumulate intervening nodes
in parent-to-child order, excluding the leaf bone itself. Retain:

- `sourceParentPath`: exact accumulated ancestor transforms;
- `sourceLocal`: the leaf's own authored transform;
- `sourceControllerFlags`: the leaf's actual flags;
- `sourceParentControllerFlags`: OR of the actual collapsed ancestor flags;
- `sourceParentPathNodes`: ordered normalized ancestor names;
- `sourceAnimationBoundary`: true only for this constructed, valid source boundary.

`bindLocal = sourceParentPath * sourceLocal` is the same product as before.
Bone selection, first duplicate name wins, parent indices and inverse-bind
construction are unchanged. No evaluated OSG bones are read for this metadata.
No controller guard is relaxed and missing metadata remains unsafe.

## Substitution and control

On native success, the bridge publishes the native SkeletonPoseInput. Its
evaluated bone-name lookup, `updateBoneMatrices`, global matrix collection and
global-to-local reconstruction remain exclusively inside the fallback branch.
The strengthened source contract checks all four operations, not just call order.

OpenMW Animation remains authoritative for times, blend masks, priority, events,
loops and movement. Active blends, procedural rotations, hybrid first-person and
unsupported sources retain the prior evaluated path. One exact compatibility
seed frame remains required. This does not remove the OSG update-only world or
morph readback.

`OPENMW_V4_LEGACY_ANIMATION_CAPTURE_CONTROL=1` retains the same-executable control.
For the private benchmark, use the new explicit `--legacy-animation-capture`
argument: inherited engine environment variables are intentionally scrubbed.
The effective flag is recorded in the manifest. The summary now also records
sampled native/legacy pose, track and seed-failure counters.

Control follow-up: the forced-legacy arm does not seed native pose history,
because it never resumes native posing. Previously it still decomposed every
legacy pose for the unused native cache; repairing metadata would make that
unnecessary work succeed for many more actors and bias the comparison. Ordinary
blend/procedural/unsupported fallback continues seeding exactly as before.

## Validation at source checkpoint

- New CPU regression reproduced the original defect before the repair:
  `forced skeleton lost authored source-animation boundary`.
- After repair: 54 checks pass in Release with MSVC 19.44, `/W4 /WX`.
- Tests cover ordered collapsed ancestors, duplicate names, controller flags,
  structural roots, equipment donor isolation, publication, changed source,
  multiple roots, and invalid/singular/non-finite hierarchies.
- Five Python tests cover explicit benchmark control and pre-profile validation.
- Phase 0, 1, 2, 3A, 3B and 3C source contracts pass locally.
- Both CI workflows gate on the C++ regression; full Windows CI also repeats it
  under MSVC and retains production link, real-NIF conformance build, packaged
  version and permanent launcher checks.

Full build/package and same-route hardware coverage/performance are separate
acceptance gates. Record their exact source/executable identities and results
after execution; passing these source checks does not establish runtime visual
equivalence, broad mod compatibility, or an FPS improvement.
