# CP4F NPC attachment and loading evidence repair

## Source and authority

Branch `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Changes remain uncommitted atop
preserved recovery WIP. No push or new GitHub CI run. CP4F remains unaccepted;
CP5 remains blocked by runtime correctness/performance gates.

Read the previously retrieved Shared Archive AI_CONTEXT_CONTROL_BLOCK and
relevant history through 111, then local resource-lifetime and skin-space
repair records. Fresh Drive retrieval failed; no claim of a fresh remote
archive snapshot. Source state and runtime evidence were checked locally.
Retain canonical attachment semantics and fail-closed invalid content; no
asset-name special cases, Lua threading disablement, GL fallback, save-format
change, or mod/config change.

## Trigger and established evidence

User-monitored run `20260919-091307-gameplay-71728`, original executable SHA256
`a4d0353acef0a655931d6ea243050e74295a32d0ca0634d17e9d663d95f88600`:
user reports connected animated bodies and working hatch activation, but
roughly 13 FPS and minutes of exterior loading. The log progresses through
Seyda Neen and surrounding cells before the fatal:
`NPC part cannot resolve its published model or attachment bone`.
Process eventually exited 1 after the fatal modal. Collector records all five
tracked normal configuration/storage files unchanged. Existing save hash is
still `d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`.
This New Game test is not existing-save compatibility acceptance.

The old fatal merges missing model/payload and missing bone without actor or
part identity. We cannot retrospectively name the failed NPC or prove the exact
asset triggered the defect below. The LuaWorker destruction message occurs
during exception unwind; it does not attribute the initiating error to Lua.

## Reproduced compatibility defect and repair

`NifTranslator::buildSkeleton` intentionally selects only `mRequiredBones`
referenced by skin. Runtime previously chose this reduced palette whenever
present, building a full actor skeleton only if translation produced none.
The actor composer retains only those bones and ancestors. A valid equipment
attachment node not influencing the base skin is consequently removed.
Canonical ActorAnimation attachment lookup uses the full base actor node map.

NPC runtime now builds/caches the existing full canonical actor skeleton even
when a translated skin skeleton is present. Evaluated OpenMW poses provide the
additional attachment transforms. Non-NPC translated-skeleton behavior is
unchanged. Skinning remains name-bound, and the prior geometry-local skin-space
repair remains in place. No missing-part checks are suppressed.

Added fixture characterizes rejection of equipment attached to an existing
non-skin node under the old reduced-skeleton choice. The full skeleton composes
the equipment and weapon-local ArrowBone ammunition correctly and follows two
different attachment poses. Case-insensitive lookup and real missing-bone/model
rejection are covered. Existing 32 skin-space parity cases remain green.

Missing-model and missing-bone failures now have distinct messages. Source
asset identity is carried even if the model handle cannot resolve. Bone errors
include requested bone, part model, base model and skeleton; runtime adds NPC
reference identity, source base path and current cell. Coordinator failures
are logged before the fatal modal, rather than waiting for dialog dismissal.

## Loading/report coverage

Opt-in Operation scopes span loadCell, changeCellGrid and interior/exterior
transitions independently of frame sampling. Begin/end records include IDs,
cell identity when available, wall time and exception-unwind status. Normal
frame sampling is not broadened. The existing file cap still applies; reaching
it is a coverage warning. These scopes do not diagnose every possible deadlock
or native GPU fault, and do not record every per-object substage.

Report reads operation/failure records, game-log fatal entries, error-line
counts, and manifest exit state. Nested loading timings are inclusive, not
additive. Incomplete scopes while running are not automatically called crashes.
The old run has been re-reported: fatal attachment failure, 21 error-level log
lines (not all causal), absent loading-scope coverage and exit 1 are now explicit
findings even though sampled gameplay invariants were clean.

## Validation

- Initial regression executable preserved 32/32 skin cases and failed the new
  diagnostic-distinction assertion, confirming the old conflated error.
- Final MSVC RelWithDebInfo production `openmw` compile/link PASS. Two local
  diagnostic-format compilation issues (CellStore include and string_view
  concatenation) were corrected. Existing conversion/dependency warnings remain.
- 7/7 CTest cases PASS: effects, update-only, actor skin/attachments, diagnostic
  emission enabled/disabled, dynamic actor plan, deformation.
- Native emission tests exercise actual output outside sampled frames,
  exception unwind, JSON escaping, normal completion and no file when disabled.
- 12/12 Python diagnostic tests PASS, all 7 CP4 source contracts PASS.
- Final executable SHA256:
  `5e771ba828cd3ff222766acb485ab697f4daa1c4918630c63427e3dd2acf3e09`.
- Executable: `C:\Users\LSCha\AppData\Local\Temp\openmw-cp4-local-deps\openmw-build-cp4f-qc\openmw.exe`.
- No new game launch or runtime promotion in this batch. No FPS/loading-speed
  improvement claim. The exact exterior failure still needs a monitored retest.

## Exact files changed in this batch

- apps/openmw/CMakeLists.txt
- apps/openmw/mwrender/v4enginerenderbridge.cpp
- apps/openmw/mwrender/v4engineframecoordinator.cpp
- apps/openmw/mwworld/scene.cpp
- components/nifrender/actormodelcomposer.hpp
- components/debug/gameplaydiagnostics.hpp
- tools/v4/cp4/actor-skin-space-tests.cpp
- tools/v4/cp4/diagnostic-emission-tests.cpp (new)
- tools/v4/cp4/gameplay-diagnostics.py
- tools/v4/cp4/test-gameplay-diagnostics.py
- tools/v4/cp4/runtime-blocker-contract.py
- tools/v4/cp4/GAMEPLAY-DIAGNOSTICS.md
- tools/v4/cp4/ATTACHMENT-LOADING-REPAIR-2026-09-19.md (this record)

Next: same-config user-monitored hatch-to-exterior test with diagnostic capture,
followed by an existing-save test. Separately address measured dynamic capture
and presentation costs, material artifacts and map/profiler compatibility.
