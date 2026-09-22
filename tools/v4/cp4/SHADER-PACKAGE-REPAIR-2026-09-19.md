# CP4F deployed shader package repair — 2026-09-19

## Failure and QC gap

Run `20260919-165807-gameplay-78908` exited 1 during initialization with
`failed initializing shader: debug`. The log identified missing
`lib/core/vertex.h.glsl` and `lib/core/fragment.h.glsl`.
Inspection found all 18 files in the pinned Rafael overlay absent from the
executable's shader directory, while the archive and base shaders remained.
The exact earlier operation that removed these files is not established.

Compilation/native tests did not audit the deployed resource directory. The old
overlay extraction ran only at configure time; an incremental executable build
had no shader-package restoration/verification dependency. A green code build
was incorrectly handed off as ready for a runtime test.

Read the cached authoritative archive control block and relevant V4 events
107–111 before implementation; current checkout supersedes their older state.
Preserve the pinned V3/PBR overlay, existing renderer controls, mods and saves.
No shader math, renderer, performance mechanism, or save serialization changed.

## Repair

- Stage the declared base shader files followed by the checksum-pinned overlay.
  The expected manifest hashes come from source/archive bytes, not from trusting
  whatever files happen to exist in the runtime directory.
- Add an always-run dependency of the `openmw` build target. Missing or altered
  engine shader files are restored even when C++ does not need relinking.
- Audit the deployed files, literal includes, conditional `@link` dependencies,
  and include cycles. Preserve unrelated files; no broad directory deletion.
- Register independent deployed-package and negative-fixture CTest gates.
- Both diagnostic launch routes reject an incomplete/altered package before
  spawning the game. Manual captures record the package manifest checksum.
- The staging/verification tool uses Python 3, now explicitly discovered by
  the shader CMake configuration. The tested interpreter is Python 3.13.15.
- This validates the bundled engine package, not every mod-provided shader,
  GLSL variant, GPU compilation, visual output, or gameplay compatibility.

## Exact source files changed in this repair

- `files/shaders/CMakeLists.txt`
- `apps/openmw/CMakeLists.txt` (shader-resource target dependency only;
  other dirty changes predate this repair)
- `tools/v4/cp4/shader_resources.py` (new)
- `tools/v4/cp4/test-shader-resources.py` (new)
- `tools/v4/cp4/gameplay-diagnostics.py`
- `tools/v4/cp4/test-gameplay-diagnostics.py`
- `tools/v4/cp4/run-runtime-qc.ps1`
- `tools/v4/cp4/SHADER-PACKAGE-REPAIR-2026-09-19.md` (this record)

## Validation and provenance

- Branch `v4.0-cp4f-exterior-closeout`, HEAD
  `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`, dirty WIP retained.
  No commit, push, or CI run.
- Normal Windows/MSVC RelWithDebInfo `openmw` target: PASS. No C++ changes or
  recompilation required for this repair; existing binary hash unchanged:
  `a577fb901200bbbff41f9a45132184104a3234cf046378b54c8b6214cc5bf56a`.
- Actual deployed package: 82 files, hash/include/link audit PASS.
- Package manifest SHA256:
  `bc4c6b6493a7f229a5ed99b109031328ba882cde8e7ca40dd252546ee903e411`.
- Pinned overlay SHA256 remains:
  `6f42a686e2a6a9038bbd4a9e2d0d1be6d8812b9b3e681d8b569560c2fe110255`.
- All 13 selected CTest cases PASS (two package gates plus the previous eleven
  native recovery tests). Package fixture suite includes 10 tests; diagnostic
  Python suite includes 13 tests, all PASS. PowerShell parser: no errors.
- Fresh diagnostic launch `20260919-171639-gameplay-62516`, game PID 77908:
  log confirms VSG/Vulkan, passes the former shader failure, loads Imperial
  Prison Ship at 17:16:56, and emits completed gameplay frames. No shader fatal
  in the inspected log. Game left running for the user's test, no input injected.
- Original `_zZz__Wake_up.omwsave` remains SHA256
  `d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`.
- Startup blocker repaired; exterior completion, sustained FPS, material
  correctness, existing-save runtime acceptance and CP4F promotion remain open.

## Readiness distinction

Source checks, native tests, deployed-package checks, startup smoke, and gameplay
acceptance are separate gates. A package audit is not full runtime acceptance.
Never hand off a failed package/startup as ready merely because compilation or
native unit tests passed.
