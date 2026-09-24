# OpimizedMW

OpenGL-focused OpenMW performance, stability, and optional enhancements.
The project name is intentionally spelled **OpimizedMW**, as requested.
The repository remains `JesusIsGod316/openmwcustom`; executable names, configuration
locations, save format, Lua API and shader interfaces remain OpenMW-compatible.

## GL-P0 foundation

Starting committed source: `be2869dd00a49774ff2642ec238d98f1456b2b8a`.
Accepted SDL3/OpenGL behavior reference: `40227915a9069d74743eb1a344e28cdeb9ee68d6`.
The original V3.25 Mode151 source/materialization provenance remains authoritative
for its accepted optimization history; do not rerun old source patch generators.

Before removal, the named source lineages were preserved in annotated tags and
an independently downloadable Git bundle. The bundle was restored into an empty
repository, checked with `git fsck --full --strict`, and passed the existing
materialized-source verifier. External SDK binaries are not inside the bundle.
Local uncommitted/untracked Windows worktrees were NOT accessed or backed up.
No local game, configuration, mod or save changes are authorized by this stage.

Archive tag: `opimizedmw-gl-p0-precleanup-20260924`.
Bundle SHA256: `26b829e48bb70957cfb0889c5c77d30e54e3712535a8473320f83396abad2762`.
Source archive: https://github.com/JesusIsGod316/openmwcustom/releases/tag/opimizedmw-gl-p0-precleanup-20260924
Additional parked Vulkan tag: `archive/vulkan-retained-20260924-be2869`.
OpenGL-reference tag: `archive/opimizedmw-opengl-reference-20260924`.

The initial cleanup is deliberately restricted to reviewed, literally disabled
shadow diagnostic/experimental blocks. Active alternatives, public settings,
accepted caches/batching, Mode151, platform paths and Vulkan source are retained.
`gl-p0/removals.json` records exact old/new blobs, original ranges, the source
removal commit and restoration instructions after the candidate passes QC.
No runtime speed or RAM reduction is attributed to deleting disabled source.

## Reproduce the OpenGL control

Use the pinned recipe at `.github/workflows/windows.yml` and
`CI/deps_versions.msvc.sh`: Windows 2022/MSVC, Ninja, RelWithDebInfo, LTO ON,
vcpkg bundle tag `2026-02-24`, Qt `6.6.3`, native SDL `3.4.10` with its SHA256
pinned in `CMakeLists.txt`. Capture the exact compiler version and dependency
manifest hashes from the build; those are not inferred from the runner label.
Vulkan runtime and Vulkan-only recovery tests must be OFF.

For a new local build directory, add this initial-cache file to the otherwise
unchanged Windows configure recipe:

```text
cmake -C tools/opimizedmw/gl-p0/CMakeInitialCache.cmake -S . -B build-opimizedmw -G Ninja <the pinned toolchain, Qt and LuaJIT arguments>
```

This is not a command with guessed machine-specific paths. Use the actual
installed paths from the preserved workflow. Validate the resulting CMakeCache,
compile commands, binary hashes, DLL identities and installation contents.
The archive workflow performs an unchanged-production-source Windows control
build before any later performance work. Build status and runtime acceptance
must be reported separately.

## Runtime comparison contract

GL-P0 does not rewrite the player's existing configuration or invent a fresh
benchmark. Preserve a hashed copy of the actual configuration chain, settings,
launcher and process-local feature controls when the next test is prepared.
The current Windows user profile was not read by this remote implementation.
The existing `tools/v3/launchers/V3_Lab.ps1` Mode151 is a historical reference,
not a command to run its full instrumented benchmark by default.

Accepted controller preparation uses `OPENMW_V325_ACTOR_SOURCE_BATCH=1` and
`OPENMW_V325_PARALLEL_ACTOR_BINDING=1`; deep telemetry and optional job-group
statistics stay off for normal performance testing. Preserve the user's actual
paging setting as the control rather than silently re-enabling the freeze path.
Do not change OSG thread mode, mod content, shader stack, save format, or runtime
resource policy in this foundation step. No P1 memory or P2 streaming change is
included here. The paused Vulkan world renderer is not a release prerequisite.

## Next implementation

GL-P1 RAM admission/ownership/reclamation and GL-P2 required-ready streaming
remain separately switchable functional work for the first performance pack.
Archive references and mechanism-specific rejection history remain in the
shared project modules. Keep `07 LEGACY_ARCHIVE` read-only.
