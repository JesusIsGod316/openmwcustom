# VulkanMW Phase 0 — Initial Producer Inventory

Status: initial source audit; not yet Phase 0 closeout
Reference source: codex/cp4f-material-frame-repair@be2869dd00a49774ff2642ec238d98f1456b2b8a

Classification:
- NATIVE-FOUNDATION: already appropriate for VulkanMW ownership.
- NATIVE-ADAPTER: semantic code worth keeping, but may need OSG math/helper removal.
- MIXED: contains reusable orchestration plus transitional OSG-derived work; split it.
- TRANSITIONAL: compatibility/capture code to remove from the normal VulkanMW path.
- REFERENCE: preserve for semantic forensics, not as the native implementation.

## RenderCore and backend

| Area | Class | Disposition | Notes |
| --- | --- | --- | --- |
| components/rendercore/renderworld.hpp | NATIVE-FOUNDATION | KEEP | Stable semantic resource/instance tables, revisions and retirement. |
| components/rendercore/records.hpp | NATIVE-FOUNDATION | KEEP/EXTEND | Already models geometry, models, material/texture semantics, skins, morphs, skeletons, switches/LOD/billboards and lights. |
| components/rendercore/frameproducer.hpp | NATIVE-FOUNDATION | KEEP | Already consumes transform deltas, skeleton poses, morph weights, views and history. |
| components/rendercore/framerenderstate.hpp | NATIVE-FOUNDATION | KEEP | Correct immutable/versioned frame boundary. |
| components/render/backend/vsg/staticassetrealizer.* | NATIVE-FOUNDATION | KEEP/REFINE | Direct RenderCore -> VSG realization. |
| components/render/backend/vsg/persistentdrawscene.hpp | MIXED-REFERENCE | REUSE PATTERNS | Strong residency/retirement pattern, but PersistentDraw originated as compatibility representation. |
| components/render/backend/vsg/vsgruntimehost.* | NATIVE-FOUNDATION | KEEP/REFINE | Backend ownership is correct; remove assumptions driven by compatibility capture over time. |
| components/render/backend/vsg/vsgsubmission.hpp | NATIVE-FOUNDATION | KEEP/REFINE | Submission, completion and compile lifecycle remain useful. |
| components/render/backend/vsg/dynamicactorplan.hpp | NATIVE-FOUNDATION | KEEP/REFINE | Neutral actor planning already consumes RenderWorld and frame pose data. |
| components/render/backend/vsg/dynamicactorpreparation.hpp | MIXED | REPLACE CPU DEFORMATION | Planning is useful; per-frame deformed MeshPayload generation should become GPU skin/morph. |

## OpenMW-side producers

| File/area | Class | Disposition | Native replacement |
| --- | --- | --- | --- |
| v4scenerenderlifecycle.cpp | MIXED / mostly NATIVE-ADAPTER | SPLIT/KEEP | Retain cell/object lifecycle publication. Remove actor/model paths that wait for OSG assembly. |
| v4semanticsource.cpp/.hpp | MIXED | SPLIT | Keep direct MWWorld/cell/light/environment semantics. Replace actor placement and other live OSG-derived values with native authoritative state. |
| v4terrainsource.cpp | MIXED | REWRITE PRODUCER | Semantic output is useful, but it currently relies on osg arrays/images/matrices and legacy terrain helpers. Produce RenderCore terrain buffers directly. |
| v4localmapbridge.cpp | NATIVE-ADAPTER | KEEP/REFINE | Native auxiliary view/readback/UI route is useful. Ensure map scene consumes native RenderWorld only. |
| v4previewbridge.cpp | MIXED | SPLIT | Keep native target/UI publication; replace CaptureVisitor and OSG sky/preview source with native assets/views. |
| v4enginerenderbridge.cpp | MIXED | SPLIT HEAVILY | Keep session/runtime orchestration. Remove MorphCollector, AnimatedObjectCaptureVisitor and OSG-derived dynamic publication. |
| v4engineframecoordinator.cpp | MIXED | SPLIT | Keep immutable frame orchestration. Replace magic/effect subtree capture with native effect state. |
| v4actorplacement.hpp | NATIVE-ADAPTER / LEGACY-SEAM | REPLACE INPUT | Small adapter today; actor placement should come from authoritative animation/world state without scene-graph dependency. |
| v4persistentobject.hpp | TRANSITIONAL | REMOVE FROM NORMAL PATH | Replace by canonical asset + instance/controller delta publication. |
| v4effectcapture.hpp | TRANSITIONAL / REFERENCE | REMOVE FROM NORMAL PATH | Preserve as semantic reference and debug compatibility path only. |
| v4objectcaptureplan.hpp | TRANSITIONAL | REMOVE FROM NORMAL PATH | Replace by persistent model/instance/controller records. |
| v4rigidactorpose.hpp | TRANSITIONAL | REMOVE | Replace with native pose publisher. |
| v4skycapture.cpp | TRANSITIONAL | REPLACE | Publish sky/weather semantics directly; keep VSG sky realization. |
| v4updateonlyviewer.hpp | TRANSITIONAL | REMOVE BY PHASE 7 | Native animation/controller evaluation must eliminate the Vulkan OSG CPU scene. |

## Highest-value Phase 1 seam

The first native conversion target is the asset pipeline:

Current compatibility path:
NIF -> NifOsg/OSG -> capture/translation -> RenderCore -> VSG

VulkanMW path:
NIF -> current OpenMW NIF parser -> NifSemanticCompiler -> RenderCore -> VSG

This removes a large class of compatibility failures and prevents static content from entering the live OSG capture world.

## vsgopenmw donor areas verified

Current donor master reviewed at 2830e7e2b4f18ef08ee24eff45a13568ec917061.

High-value donor/reference directories:
- components/vsgadapters/nif
- components/animation
- components/mwanimation
- components/pipeline
- components/resource
- components/render
- components/terrain
- components/view
- components/vsgutil

Verified useful native semantics include:
- NIF geometry/strips/lines
- material and texture slots
- skin weights/bone indices/bone matrices
- morph weights
- transforms/keyframes
- switches
- depth sorting
- particles
- VSG render/device/compile structure
- actor/object animation and attachments

Do not rebase or wholesale merge the donor. Port/adapt semantics into the current custom OpenMW + RenderCore architecture.

## Next source work

1. Add native producer directories and interfaces.
2. Define NifSemanticCompiler result/error contracts.
3. Reuse existing RenderCore records rather than creating a second canonical scene format.
4. Start with static NIF corpus conversion.
5. Keep OpenGL/NifOsg untouched as the reference path.
6. Do not expand v4effectcapture/v4persistentobject to solve newly discovered content unless needed only as a temporary forensic control.
