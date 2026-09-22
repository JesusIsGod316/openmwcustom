# Map compilation-context lifetime repair — 2026-09-19

## Scope and provenance

Branch `v4.0-cp4f-exterior-closeout`, HEAD `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`, existing dirty work preserved. No commit, push, CI run, normal-profile edits, or full-game launch in this repair turn.

Failed user run: `C:/Users/LSCha/Documents/My Games/OpenMW/runtime-qc-evidence/20260919-222214-gameplay-85816`. This was the normal New Game prison-ship route, not a direct exterior start or a regular save load. The failed executable SHA256 was `1A94A1DD654F7F3ED52D92A7B4694029C3634FF7501FA0F6F694D83CC0C3AC44`.

The native dump's exception context resolves to `vsg::BindViewDescriptorSets::compile+0x29`, `ViewDependentState.cpp:145`, where VSG dereferences `Context::viewDependentState`. Nearby symbolized stack-memory candidates include `CompileManager::compile`, `compileForViewer`, and `VsgRuntimeHost::synchronizeStaticWorld`; these candidates are not a formally unwound stack.

VSG 1.1.15 stores an observer for the View but a raw pointer to its dependent state. Auxiliary map creation registered persistent contexts. Map retirement removed the image/graph/view without removing those contexts. Subsequent new-scenery compilation could therefore dereference freed state. A focused real-Vulkan test reproduced the stale registration after map retirement before the ownership repair, using a safety guard to report the defect instead of deliberately causing an access violation. This confirms the lifetime defect and strongly matches the dump; actual user-route acceptance remains pending.

## Repair

- `ViewCompileManager` retains VSG's normal compiler, traversal queue, descriptor pools, and resource behavior. Each transient framebuffer-view registration owns its View and the exact contexts appended to every leased traversal, including nested shadow contexts.
- Registration destruction unregisters those contexts before releasing the View. The auxiliary runtime owns the registration, so normal retirement, failed publication, and host destruction all use the same cleanup.
- Partial registration exceptions restore each traversal's original context list. Traversal leases return the queue entries on success and failure.
- The lifetime-aware manager is installed after initial `Viewer::compile` has initialized shadow resources. Instrumentation and resource scavenging are preserved, and any matching pager reference is updated. Existing water views are then registered as before.
- General incremental compilation pins live Views while compiling and rejects stale state pointers before dereference. This is a safety guard, not the lifetime repair itself, and does not silently publish incomplete scenes.
- Same-executable causal control: `OPENMW_V4_RETAIN_AUXILIARY_CONTEXTS=1` restores the old auxiliary-registration lifetime. Leave it unset for normal operation. The safety guard remains enabled in the control.

GPU completion ordering is unchanged: auxiliary retirement already waits for idle before removing the command-graph child and erasing its runtime. No graphics features or mods were disabled. No whole-scene eviction was added.

## Exact files changed in this repair turn

1. `components/render/backend/vsg/viewcompilemanager.hpp` — new registration ownership, transactional rollback, traversal lease, QC count.
2. `components/render/backend/vsg/vsgruntimehost.hpp` — auxiliary registration ownership and QC count accessor.
3. `components/render/backend/vsg/vsgruntimehost.cpp` — install lifetime-aware manager after shadow initialization; register auxiliary contexts with owned lifetime; retain explicit control.
4. `components/render/backend/vsg/vsgsubmission.hpp` — stale-context guard and live-view pins during general incremental compilation.
5. `tools/v4/cp4/water-transition-tests.cpp` — map-retirement and registration-lifetime regressions added to the existing water GPU fixture.
6. `tools/v4/cp4/runtime-blocker-contract.py` — supplemental source checks for ownership and regression coverage.
7. This repair record.

## Validation

Build directory: `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc`.

- Pre-repair guarded reproduction: expected failure on `new scenery after map retirement`, with `Stale VSG view compilation context after view retirement`.
- Initial implementation QC caught premature manager creation before shadow initialization; corrected before final build. A missing VSG header include path also failed compilation and was corrected. These intermediate failures are retained in logs.
- Final MSVC RelWithDebInfo production executable and regression-target compile/link: PASS. Existing `engine.cpp` and `settings/categories/cells.hpp` warnings remain; not a warning-free build.
- CTest, including shader-package checks: **14/14 PASS**.
- Final real-Vulkan fixture: PASS for eight create-map → retire-map → admit-new-scenery cycles; context count returns to the initial count each time. Idempotent retirement also passes.
- Registration tests: PASS for View retention until unregister, parent and nested-shadow cleanup, preserving unrelated contexts, failed-publication rollback, partial-registration exception rollback, and successful retry afterward.
- Cave water/underwater, outdoor water with sky/shadows, late objects, GUI loading, repeated water transitions: PASS in the GPU fixture. Visual cave-water parity is not established by this test.
- Same final fixture with `OPENMW_V4_RETAIN_AUXILIARY_CONTEXTS=1`: expected exit 1 and the exact stale-context diagnostic. Default repair: PASS.
- Same final fixture with `OPENMW_V4_REBUILD_STATIC_PLANS=1`: PASS.
- Rebuilt GUI isolation GPU fixture: PASS.
- Python diagnostic tests: **16/16 PASS**; seven CP4 source-contract scripts: PASS; `git diff --check`: PASS.
- VSG's two existing informational `addViewDependentState ... no framebuffer` lines remain in GPU logs; they were not hidden.

Final executable: `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc/openmw.exe`.

SHA256: `1371694F780FA0430C573AEF26C747F98EE4CB1D6E1C1CA995E1C8BACC74309A`.

Evidence logs in the build directory:

- `map-retirement-red-build.log`, `map-retirement-red-test.log`
- `map-retirement-green-build.log`, `map-retirement-green-test.log` (caught initialization-order regression)
- `map-retirement-verified-build.log` (caught include-path error)
- `map-retirement-final-fixture-build.log`, `map-retirement-fixed-test.log`
- `map-retirement-release-build.log`, `map-retirement-ctest.log`
- `map-retirement-final-water.log`, `map-retirement-final-retained-control.log`
- `map-retirement-final-static-control.log`, `map-retirement-final-gui.log`

Normal settings, input, shader settings, global/player storage hashes still match the failed-run manifest. Normal `openmw.cfg` is unchanged (`70257CFB9851831D850D772596A66DBFA6920295D6E9A2CE141D424EB83373C0`). Protected `_zZz__Wake_up.omwsave` is unchanged (`D34685C838BD1B4318DB34B1E0C1F00A7A2FE9A990FD21A120973297083AF99B`). No game or GPU fixture process remains running.

## Acceptance and remaining work

Implementation/build/focused regression gates passed. **The rebuilt executable has not yet completed user testing of New Game → prison ship → hatch → Seyda Neen.** No gameplay FPS, exterior loading-time, visual parity, or CP4F promotion claim is made.

The roughly 17-second terrain preparation seen in the previous run, missing landscape texture-layer publication/white terrain, cyan/green material artifacts, and low gameplay FPS remain unresolved. This repair does not claim to fix them. See `WATER-STATIC-RUNTIME-REPAIR-2026-09-19.md` for the prior terrain findings. The next manual acceptance run must preserve the New Game route, not substitute a regular save or direct exterior startup.
