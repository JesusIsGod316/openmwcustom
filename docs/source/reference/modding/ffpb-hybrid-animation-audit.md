# Full-body first-person hybrid animation: source audit and composition contract

Status: source and reference audit, 2026-09-24. A default-off engine prototype
is being implemented on the feature branch. It is not runtime validated.
Feature branch
`codex/ffpb-hybrid-animation` starts at `opimizedmw/gl-p2` commit
`17e6e5e6bb1f5ca297236f75507e515375007199`.

## Authority and control

The physical camera remains `Camera::Mode::FirstPerson`. The existing
`NpcAnimation::VM_FirstPersonFullBody` uses the normal skeleton, body parts,
equipment and world depth. Its owner-view head, hair and helmet cull must
continue to leave those parts in shadows and secondary views. The existing
third-person and full-body first-person animation paths are the control. The
new composition must be explicitly enabled, local-player-only, and must fall
back to that control on missing or incompatible source data. No mechanics,
attack events, text-key dispatch, or movement accumulation may be duplicated.

The historical V3.21 CP4 mode 131 was accepted for ordinary play with the
unmodified Dynamic Animations mod and a complete player head shadow. Preserve
its animation-consumer Lua context: physical camera queries still report first
person, while marked animation consumers can receive third-person animation
semantics. This is separate from the proposed pose composition.

## Current engine path

* `NpcAnimation::updateNpcBase` selects the normal skeleton and
  `Settings::models().mXbaseanim` in `VM_FirstPersonFullBody`; only
  `VM_FirstPerson` selects the first-person base. It calls
  `Animation::addAnimSource` in source priority order and optionally loads
  additional `.kf` files from the matching `animations/` directory.
* `Animation::addSingleAnimSource` binds each controller to a node of the
  single actor skeleton. `detectBlendMask` partitions controllers at
  `Bip01 Spine1`, left clavicle and right clavicle into lower body, torso,
  left arm and right arm. Root and pelvis belong to the lower body group.
* `Animation::play` chooses one highest-priority source with the requested
  group. `resetActiveGroups` chooses one active state per bone group and
  installs its controllers. The present blend controllers smooth **changes
  between tracks**; they do not continuously mix two source poses per bone.
* `Animation::runAnimation` advances that one state's time, dispatches its
  text keys and uses its lower-body accumulation controller for movement.
  A hybrid visual source must have its own time and must never dispatch keys
  or feed `mAccumCtrl`.
* `WeaponAnimation::configureControllers` rotates spine1 and spine2 for
  ranged aim and the existing procedural melee correction. Hybrid authored
  melee needs a local-player FFPB exclusion for the additional melee factor;
  ordinary third person, NPCs and ranged aim retain their current behavior.

This suggests adding a **visual-only companion source** to the existing
`AnimState`, not a second actor or a second gameplay animation state. The
normal state remains authoritative for group selection, movement, event
dispatch, save/load and Lua-observable animation state. Companion controllers
can be bound to selected nodes of that same skeleton with a separately mapped
visual time. Binding and mask lookup should occur when the actor/source is
rebuilt, not on every frame.

## Supplied reference audit

The four archives were read as reference data; none is an engine dependency.
The ReAnimation v3 archive includes `Sources/Tools/FBACompat` with the
converter and checks. The separate compatibility archive has 95 `.kf` files
under `Animations/xbase_anim.1st` plus a marker text file. The Better Bodies
FBA archive supplies base full-body `.kf` files. The First Person Hands archive
supplies retargeted base `.kf` files and an optional plugin.

`fba_merge.py` uses this lower set: `Bip01`, pelvis, spine, both thighs,
calves and feet. Locomotion aligns corresponding footfall markers with a
monotone time warp and retains third-person root movement. Attack/equip
sections align matching text-key phases; horizontal attack travel is removed
from the accumulated root and represented as limited pelvis motion. Its
spine counter-rotation holds the authored FP upper orientation while allowing
some locomotion sway. `fba_posture.py` applies a configurable chest lean and
counter-rotates the neck; its default 15 degrees is reference data, not an
engine constant. `fba_fingers.py` explicitly solves third-person fingers onto
the different first-person finger rig using mesh correspondence.

Representative files parsed with the included `nifkf.py` show:

| File | Tracks | Relevant observation |
| --- | ---: | --- |
| Better Bodies FBA `xbase_anim.kf` | 34 | Root, pelvis, spine, torso, both arms, weapon, legs; three two-joint finger chains per hand. |
| Better Bodies FBA `xbase_anim.1st.kf` | 34 | Same listed track names as its full-body base. |
| ReAnimation `x1hIdle.kf` | 55 | Adds toes, shield and five three-joint finger chains per hand. |
| ReAnimation `x1hAltAttacks.kf` | 55 | Includes FP upper and lower tracks plus `WeaponOneHand` and `WeaponOneHand1` attack sections. |
| Compatibility `x1hIdle.kf` | 55 | Merged output has six text keys versus three in the original FP idle. |
| First Person Hands `xbase_anim.1st.kf` | 52 | Adds FP finger tracks to the FBA base, showing that hand mapping depends on mesh and skeleton choice. |

The original FP `x1hAltAttacks.kf` places `WeaponOneHand: Chop Start` near
4.0333 s. The merged compatibility file places that same key near 15.5333 s.
This is direct evidence that absolute time reuse or a simple whole-clip
normalization will select the wrong attack pose. Group and phase mapping is
required. The original FP idle combines `IdleShield` and `Idle1h` text in
three compound keys; the compatibility output separates them into six keys.

## Bone/source contract for the first prototype

| Bone/role | Authoritative source | Rule |
| --- | --- | --- |
| Root `Bip01`, pelvis, spine, thighs, calves, feet and toes | Normal full-body | Keep movement accumulation and gameplay position unchanged. Never attach a second root controller. |
| Spine1 and spine2 | Composed boundary | Blend locally with explicit weight and chest compensation; do not transfer raw FP torso pitch to the camera. |
| Neck and head | Full-body with optional counter-correction | Keep camera anchor and owner-view cull behavior; measure clipping before choosing correction. |
| Left/right clavicle, upper arm, forearm, hand | FP visual companion when compatible | Bind to the same actor skeleton. Fall back per group on missing controllers or unsafe hierarchy. |
| Weapon bone and shield bone | Follow the corresponding authored hand source | One rendered weapon and one attack event; verify attachment and shield motion against body pose. |
| Fingers | Rig-dependent | Use only matching tracks initially. Retargeting needs mesh-aware mapping; do not assume the optional vanilla-hands plugin fits Better Bodies. |
| Bow/arrow/other special attachments | Deferred | Keep current ranged pitch and attachment control until explicitly validated. |

The source pair must be selected by semantic group and attack section, not
filename alone. Key anchors should include start, min/max attack, hit and
follow-through for melee; footfall and loop boundaries matter for locomotion.
Build a monotone, bounded mapping at source-bind time and reject malformed or
missing anchors to the control path. Only the visual companion reads this
mapped time. Smooth weight transitions must cover draw, attack, recovery,
interruptions and FP/TP switches, including actor rebuild after load.

## Prototype implementation and validation gates

The prototype loads the winning native first-person base KF and additional
first-person animation directory sources through OpenMW's VFS. It pairs a
matching visual group with the normal full-body state. The normal source
remains the only source of text keys and root motion. Matching animation
phase keys produce a monotone visual time map, and a callback samples both
NIF tracks on the same actor bone. Upper arm/weapon tracks can receive the
authored FP pose; torso is initially weighted to 0.45, while root, pelvis,
legs, neck, head and fingers remain on the full-body track. Visual transitions
and interruptions are weight-blended. Unsupported/missing NIF tracks fall
back per bone. The new setting defaults off.

This first slice does not retarget different rest poses or finger rigs, and
its torso weight is a test setting rather than a promoted camera-clipping
solution. OSG animation controllers remain on the normal source. Runtime
testing must check regular first-person assets and a representative modded
stack, especially hand/weapon framing, chest clipping, draw/sheath, and
attack follow-through. The same binary with the setting off is the control.

1. Verify disabled-path parity for native FP, existing FFPB, ordinary TP and
   NPCs in a compiled build.
2. Verify one actor/skeleton, cached compatible visual bindings, mapped time,
   and no additional text-key dispatch or root accumulation.
3. Test authored upper-body/weapon motion and smooth attack, draw and recovery
   transitions with regular and modded first-person animations.
4. Verify extra FFPB melee pitch is excluded only while authored FP melee is
   active. Keep ranged pitch and third-person/NPC correction.
5. Test missing-bone/source fallback,
   root/pelvis single ownership, attack-event single dispatch, FP/TP/load
   rebuild, equipment and complete shadow geometry. Add test vectors for
   mismatched ReAnimation attack timelines and finger rig differences.

The first Windows build should follow cheap source checks. Its result is a
compile gate only; user runtime validation determines whether the visual
presentation or performance can be promoted.
