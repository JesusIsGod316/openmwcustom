# CP4F recovery: source audit, repair order, and acceptance gates

Date: 2026-09-18. Status: proposed execution plan based on archive and source inspection; not a claim that repairs or runtime acceptance have completed.

## Objective and scope

Make the existing Vulkan route load the user's save and a new game reliably, render the correct world and actors, remain responsive, and keep resources bounded on the 8 GB GPU. Preserve the OpenGL control and working Lua threading. CP5 stays blocked until the CP4F correctness, threading, and basic playability gates pass.

This is an engine-level repair, not a list of filename exceptions. The user's mod assets are regression fixtures and a compatibility corpus, not a reason to disable mods or silently omit content. Target the behavior the existing OpenMW renderer actually supports; do not promise every possible NIF feature or third-party shader is automatically supported.

No full build, game launch, code fix, or push was performed for this planning audit. Existing uncommitted engine changes were preserved.

## 1. Verified baseline and authority

- Checkout: `C:\Users\LSCha\Documents\ChatGPT\OpenMW custom Build-cp4f`.
- Branch: `v4.0-cp4f-exterior-closeout`.
- Local HEAD: `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`.
- Fetched remote tip: `e0f460f32ee3e1786c21da786ac5d138cc5fd576`. Its only difference from HEAD is a build-workflow edit, not an engine repair. Fetch did not merge or alter the working tree.
- Real uncommitted work exists in the VSG immediate-effect realizer, static realizer/conformance files, runtime host, and runtime blocker contract. It is experimental, not a qualified recovery baseline. The two Lua worker files appear in status but have no content changes in the ordinary diff summary; do not stage them indiscriminately.
- Shared Project Context Archive checkpoint 111 is the latest recovery handoff reviewed. The structured current/decision/feature documents contain older checkpoints and cannot override the newer source or checkpoint 111.
- Retain: canonical OpenMW animation ownership; first-match skeleton names; winning-VFS identities; neutral render-state boundary; separate logical, content, and GPU lifetimes; runtime budget headroom inside the 8 GB limit; same-build/frozen-cohort comparisons.
- The historical donor guidance is advisory: reuse verified algorithms where useful, not a donor's incompatible ownership model or a second gameplay animation system.

Sources: [shared archive](https://docs.google.com/document/d/1Zk7qcYlLn_3375npd8MU1KV6bozewuVMyl3_l0OPves/edit), [current state](https://docs.google.com/document/d/1asic8LPiHtZ2IqKYnSOjcJa46Wi6MZZC-QpX7tGO7JQ/edit), [decisions](https://docs.google.com/document/d/1fYGGgfL46YmD1SvDdUq68vnIZ7oO4DqjzzBUy2HFOs0/edit), [feature history](https://docs.google.com/document/d/15U_txmwFiis7giyrA5YDSyX-JxzJG-WGZBrgvkH3ov4/edit), and the exact local/fetched Git state.

## 2. Why the earlier validation did not prevent this

The evidence supports a validation-coverage gap, not one mysterious recurring crash:

| Failure family | What the history establishes | Required regression coverage |
| --- | --- | --- |
| Build, ABI, and missing DLLs | Compilation/API/definition and deployment failures happened before gameplay could be tested. | Exact pinned dependencies, both renderer build configurations, and clean-directory launch with verified loaded DLL paths. |
| Menu/video/UI | Texture ownership, UI update, video presentation, and skip transitions needed separate repairs. User later reported menu tabs working. | Real changing UI/video frames, repeated transitions, and Escape at several points; not merely successful shader compilation. |
| Save-load compatibility | Successive unsupported material roles/apply modes, external skeleton parts, and morph cases were discovered one at a time. | Batch capability inventory plus semantic fixtures, not first-error-only save attempts. |
| First gameplay submission | Auxiliary-view/pipeline and camera publication issues occurred after loading/menu success. | Complete main plus auxiliary frame, and first use after every relevant transition. |
| Latest memory failure | Checkpoint 111 records a decoded VSG exception with `VK_ERROR_OUT_OF_DEVICE_MEMORY`. This specific dump was not a Lua or Escape teardown failure. | Resident allocation, retirement, and repeated-load testing with actual GPU budget measurements. |
| Latest visible output | User screenshots show missing/wrong geometry, purple materials, black map output, oversized actor/body geometry, and severe stalls. | Differential geometry/material tests, responsive gameplay, multiview checks, and a sustained resource test. |

`tools/v4/cp4/runtime-blocker-contract.py` largely checks source text and ordering. Those checks can protect an architectural boundary, but cannot establish correct pixels, bone placement, cache invalidation, or GPU lifetime. Existing C++ behavioral tests should be extended rather than replaced. The inspected CP3D Linux invocation does not define `NDEBUG`; this audit does not claim those assertions were disabled. Future Release-mode tests must still verify that checks execute and avoid side effects inside `assert`.

Build success, source-contract success, subsystem runtime success, and full gameplay acceptance must have separate statuses. A milestone name must not imply that its full runtime acceptance occurred.

## 3. Findings from the current source

### A. Material classification mismatch — confirmed; screenshot attribution pending

`apps/openmw/mwrender/v4effectcapture.hpp::textureRole` recognizes `normalMap` but not `normalHeightMap`, and defaults unknown names to Diffuse. Its capture then assigns color interpretation from that role. Existing `components/shader/shadervisitor.cpp` explicitly recognizes `normalHeightMap` and treats it as a normal map with height information.

This is a concrete semantic discrepancy and a strong candidate for purple objects, but the screenshot's exact bed texture has not yet been traced. Also audit texture-type fallback, UV-set selection versus texture unit, TexMat, sampler state, inherited OVERRIDE/PROTECTED state, blend/depth/cull, and all legacy material stages together.

### B. Repeated texture I/O in frame capture — confirmed mechanism; cost unmeasured

`v4effectcapture.hpp::captureMaterial` invokes `resolveTextureVfsIdentity` per texture. `components/nifrender/vfsidentity.hpp` opens the winning VFS stream and computes its content hash. This is reachable during dynamic frame capture, without a cache at that boundary.

Move immutable identity resolution out of the recurring frame path. Key reusable results by canonical winning asset and a reliable content/session generation; invalidate deliberately. Do not substitute raw pointers or an unchecked filename-only cache. Measure time/call counts before assigning a percentage of the current stall to this work.

### C. Residency WIP is not complete — confirmed omissions; safety must be tested

`vsgruntimehost.cpp::synchronizeDynamicActors` still reconstructs/deforms/realizes actor resources each frame and compiles the next dynamic root. The new immediate-effect resident map has lookup and insertion but no removal sweep. The reuse predicate in `immediateeffectrealizer.hpp` does not establish equality of index contents, material scalar/state changes, sampler/UV interpretation, or bounds. The update routine changes vertex streams, not all those properties.

The short archived reuse run is evidence of one improvement, not proof of bounded memory or semantic correctness. The reported 955 draws are immediate-effect-path draws, not proof that the scene contains 955 independent particles. Mutable buffers and transforms also need an explicit frames-in-flight ownership rule.

### D. Evaluated drawable gap — confirmed

`AnimatedObjectCaptureVisitor` handles plain `osg::Geometry` and particle systems, but not direct `SceneUtil::RigGeometry` or `MorphGeometry`. A traversal with no captured draws can become a warning instead of complete rendering.

The proposed shortcut from the previous handoff needs revision: these classes' `getGeometry(frame)` methods are private, and evaluation is tied to OSG cull traversal. The source mesh is not the evaluated animated mesh. Do not impersonate a CullVisitor with a generic visitor or expose a stale buffer as a fix.

Create a supported evaluated-state boundary that reuses canonical CPU deformation semantics at a defined frame stage, then publishes an owned/immutable snapshot or correctly versioned buffers. Preserve the OpenGL execution path. This is not a mandate for a wholesale animation rewrite.

### E. Oversized actor/body — reproducible symptom; exact math defect unproven

Inspect `components/nifrender/actormodelcomposer.hpp`, `components/render/backend/vsg/dynamicactorplan.hpp`, `components/rendercore/deformation.hpp`, and canonical `SceneUtil::RigGeometry`. Check bind, skeleton, attachment, model, world, and camera spaces at each boundary. A multiplication-order change without a reference test would be another guess.

### F. Lua — distinct acceptance requirement, not a universal explanation

The current source snapshots GUI/local-map preparation before Lua overlap. Worker threads remain supported. Vulkan still quarantines package-prototype reuse in `luamanagerimp.cpp`, but configured dependency precompilation is enabled. Older archive language suggesting all dependency precompilation is bypassed is stale.

Audit ownership and worker shutdown across load failure, skip, quit, and exceptions. An `Unexpected destruction of LuaWorker` message following another fatal error is not sufficient evidence that Lua caused the initial failure. Do not permanently set worker count to zero to declare success.

### G. Remaining capability guards and view output — inventory before another save attempt

`VsgRuntimeHost::renderFrame` still rejects dynamic materials, clustered-local-light requests, and unequal render/output extents. The native map bridge currently supplies equal extents. Consequently the previous 0.85/1.0 setting experiment does not establish that render scaling is supported or that it caused a particular crash.

Inventory every reachable fail-closed branch and intentional degradation across translation, publication, realization, submission, and presentation. Classify each as invalid input, implemented compatibility behavior, required missing behavior, or explicitly deferred optional feature. Test the actual active settings and producers before deciding scope. Do not remove guards simply to make a save load.

## 4. Execution plan and order

### Phase 1 — Reconcile source and establish reliable evidence

1. Review/preserve the current WIP as a separately identifiable checkpoint; incorporate the remote workflow edit without losing it when implementation starts. Record source and binary hashes independently.
2. Extend the existing QC runner rather than inventing another launch path. Give every run a unique evidence folder and isolated test profile. Never overwrite the normal build's settings, original saves, or earlier crash evidence.
3. Preflight the executable's transitive DLL dependencies, architecture and exact loaded paths, matching symbols, plugin paths, quoted save/config arguments, requested/effective renderer, effective settings, GPU/driver, and mod/VFS cohort.
4. Record ordered milestones: process start, menu, intro decoding, skip requested/completed, scene publish, auxiliary compilation, first gameplay submission/completion, then progressing gameplay frames. Capture stage duration and last completed milestone.
5. Report launch failure, exception, process exit, unresponsive phase, user stop, diagnostic timeout, and scenario completion distinctly. A timeout or a single visible frame is not a pass. Finalize evidence even after interrupted harness execution; label incomplete runs.

**Gate:** One reproducible command produces an unambiguous report tied to source, executable, dependencies, effective configuration, and scenario. A deliberately bad path/dependency test must fail preflight. No more normal-profile log guessing.

### Phase 2 — Audit and repair the shared data path

Two coupled workstreams, with Lua ownership reviewed alongside both:

**2A: Geometry/material equivalence.** Build deterministic fixtures from the user's winning-VFS assets and small synthetic edge cases. Compare the neutral/Vulkan result with the existing OpenMW semantics. Cover static and evaluated objects, skinned body/equipment, rigid attachments, morphing faces, external skeletons/animations, particles/billboards, instanced transforms, switches/LOD, and hidden/collision-only nodes. For each visible drawable, account for its disposition; zero draws must have a legitimate reason.

Use fixed poses/times to compare vertex positions, normals, bounds, attachment matrices, texture roles, UVs, and material state. Include nonidentity bind/root transforms, mirrored/nonuniform scaling where supported, duplicate bone names, equipment replacement, and both first-/third-person views. Reuse canonical math/evaluation where practical. Make the first-person melee pitch cap an explicit visual test, not a suspected cure for all body placement errors.

Audit the whole supported legacy material table: Diffuse/Dark/Detail/Decal/Emissive/Normal/normal-height/Bump/Environment/Gloss/Specular, apply modes, bump matrix/luma, alpha tests/blends, depth/stencil, inherited properties, and texture coordinates. Unknown semantics need a diagnostic, not accidental Diffuse classification. Trace the purple bed's actual selected textures to close the screenshot issue. Test static MyGUI transfer and video range/colorspace separately; no arbitrary gamma adjustments.

**2B: Persistent resources.** Separate immutable asset/material/pipeline identity from pose/vertex changes and from instance/view placement. Resolve VFS identity once per valid content generation. Retain actor/effect resources and update only dirty data; reuse immutable pipelines without retaining every obsolete generation. Use stable instance/system identities rather than live-particle ordinals where topology can change. Batch particles by compatible material/system where semantically safe.

Define revision handling for topology/index changes, materials, textures/samplers, visibility, bounds, pose and morph state. Validate a complete update before mutating resident state. Retire removed/replaced resources after their final GPU use; handle world reset, cell unload, failed publication, and shutdown. Bound caches and pending retirements against the runtime budget, with reserved headroom. No routine device-wide idle as the solution.

**Gate:** Behavioral fixtures pass, including negative controls. Unchanged warm assets cause no recurring texture-file hashing/decoding or pipeline creation. Moving/animating updates the intended streams; equal-size topology/material changes cannot reuse stale state. Despawn and repeated A-B-A cell cycles settle back to a defined bounded working set after GPU completion. A short no-OOM run alone does not pass.

### Phase 3 — Lua/frame ownership and full-view audit

Audit the chain from animation/world updates through `prepare()` to `presentPrepared()` and every consumer reachable during Lua overlap. Publish immutable or otherwise safely owned inputs before releasing the worker. Check map/UI outputs, animation buffers, object lifetime, mutation during unload, exception paths, and lock/join ordering. Keep dependency precompilation; review the narrower package-prototype quarantine independently and re-enable only after a reproduced failure and validated correction.

At fixed timestamps, use the same executable and frozen save/config for Lua-worker A/B diagnosis. Worker-disabled results isolate causality only. Acceptance requires the intended threaded configuration, no race/deadlock, clean exceptional shutdown, and measured overlap/wait behavior. Never infer threading performance from total CPU utilization alone.

For main, map, shadow, reflection/refraction and UI views, check view-specific pipeline compilation, descriptor layouts, viewport/extent, depth convention, visibility masks, render-target transitions, write/sample dependencies, and target lifetimes. Track a known test marker through local-map render, readback, retained image, fog composition, and MyGUI sampling to localize the black map. Account for the full map producer-to-consumer route, not just a successful readback call.

Audit remaining capability guards with the full configured scene. Render scale must either work through proper separate targets or be explicitly diagnosed as unsupported before gameplay, not silently interpreted as a successful 0.85-scale test.

**Gate:** Correct first and subsequent frames with the full view set; clean threaded execution through load failure/quit/unload; visible local-map reference markers and terrain/room coverage; no stale camera or resource references across transitions.

### Phase 4 — One diagnostic entrypoint, multiple purposeful test modes

The diagnostic should collect many independent failures in one report, but it cannot honestly catch every possible defect in one game run.

1. **Batch asset/config audit:** enumerate active content and winning VFS assets; group by semantic features; test the failing corpus first, then a resumable broader mod inventory. Emit all independent incompatibilities with canonical asset/record/role and source location. Continue across isolated bad fixtures in the offline tool; never submit invalid live GPU work just to collect more errors. Keep proprietary assets local; publish hashes/recipes and redistributable synthetic fixtures.
2. **Semantic regression suite:** fixed poses/material fixtures, state changes, retirement, frame handoff, and negative controls. These catch wrong-but-legal rendering that API validation cannot know is wrong.
3. **Vulkan correctness mode:** core/object-lifetime and synchronization validation, with targeted GPU-assisted runs where shader-access issues warrant them. Verify the requested layers actually loaded. Deduplicate by error signature while preserving first occurrence, counts, phase, object labels, and resource provenance.
4. **Liveness/resource mode:** stage timings, sampled thread stacks on sustained stalls, live/retired bytes and object counts, hash/decode/compile/update counts, device budget, and failure-safe evidence. Include CPU/GPU waits and known object-paging/vertex-optimization stalls as separate lanes from Lua and GPU OOM.
5. **Clean performance mode:** same executable/cohort, expensive validation and capture disabled; record frame cap, warmup, CPU stages, GPU time, budget and frame-time distribution. Diagnostic slowdowns are not performance results.

Khronos describes synchronization validation as checking resource access conflicts and GPU-assisted validation as runtime shader instrumentation: [development tools](https://docs.vulkan.org/guide/latest/development_tools.html). GPU instrumentation has overhead, so use targeted runs and do not benchmark with it enabled: [LunarG GPU validation](https://vulkan.lunarg.com/doc/view/latest/windows/gpu_validation.html).

**Gate:** The suite detects deliberately introduced representative errors; missing instrumentation reports unavailable/incomplete, never pass. Every report separates observed defects from suspected causes and includes reproduction inputs.

### Phase 5 — Consolidated acceptance, then a full publishable build

Use incremental local compilation and targeted tests throughout. Do not publish a full package for every small edit. Once the coupled repairs and fixture gates pass, test the locally staged candidate, then run one consolidated Windows CI/package checkpoint. Verify that the resulting package is the same tested source and has complete dependencies. A package smoke test remains necessary even after local gameplay succeeds.

Required runtime scenarios:

- Main menu tabs and UI updates; intro completion and Escape at early/middle/late playback; repeated menu/new-game transitions.
- The supplied Gnisis save: correct room/bed/materials, proportionate animated actors, working movement/look, responsive menu/map, and normal exit.
- External-cell saves implicated earlier (including Caldera/Khuul where available), interior/exterior transitions, cell revisits, combat, equipment and first-person body motion.
- Map, shadows, weather and water/auxiliary views; resize, minimize/restore; scale 1.0 and 0.85 only after scale behavior is established.
- Repeated load/unload and a bounded traversal soak. Start with a short liveness/resource gate, then a longer soak; abort a runaway allocation/stall early with preserved evidence instead of waiting for an OOM.
- Normal worker configuration plus diagnostic same-binary controls as needed. No permanent disabling of Lua threading, actors, required map views, or mod content to obtain a green result.

Acceptance means no crash/hang, no unexplained drawable omissions, correct fixture output, no unresolved relevant validation errors, responsive movement/camera/UI, bounded post-warmup allocation and retirement, and operation below the measured device budget with predeclared headroom. Establish exact timing/memory tolerances from the frozen reference and hardware before the run. Do not invent an FPS target or call 2.64 seconds per frame playable.

The user monitors launches and CI. Request screenshots only for a specific remaining visual question; automated evidence should already say what code/configuration was tested and whether frames progressed. Do not start ongoing monitoring without a new request.

## 5. Working discipline and handoff

- Maintain a defect register: symptom, last reproduced build, subsystem, confirmed mechanism versus hypothesis, proposed general fix, negative/regression test, and remaining runtime acceptance.
- Every repair should have a test that failed before it and passes afterward. A test asserting that the new source text exists is supplementary, not that regression proof.
- Keep switchable control paths where safe; switches isolate mechanisms, not hide missing compatibility. Source experiments and diagnostic output remain bounded/default-off as appropriate.
- Commit coherent repairs with provenance; do not stage unrelated/line-ending-only files. Keep unqualified WIP separately identifiable.
- Update archive current state, feature history and evidence ledger together at the next authorized checkpoint. Correct stale precompilation claims and unmeasured CPU attribution. Preserve historical results, but clearly supersede them.
- Record old screenshots as observations. The approximately 7649 MB/0 FPS/2641.7 ms overlay is not a controlled benchmark. The assertion that sack warnings consume most CPU time is not proven by timestamps and must be replaced with timing evidence.
- The newest inspected evidence directory contained only a preexisting dump, not a completed run report; do not mistake directory creation or an old dump for successful evidence collection.
- CP4F remains open. Do not move into CP5 based solely on another green build.

## 6. Immediate implementation starting point

Start with Phase 1's trustworthy identity/evidence harness and a complete compatibility-guard inventory. Then implement the coupled Phase 2 fixes: material-role/UV semantics and evaluated geometry, plus bounded resident resources and cached immutable VFS identities. Build their behavioral fixtures first or alongside each change. Review Lua/frame ownership concurrently; complete its acceptance before advancing. Only then ask for the next full gameplay acceptance run.

This plan intentionally allows a structural repair of the runtime adapter if the differential tests show that patching individual cases would preserve the same defects. It does not authorize speculative replacement of the whole renderer or repeated full builds without new evidence.
