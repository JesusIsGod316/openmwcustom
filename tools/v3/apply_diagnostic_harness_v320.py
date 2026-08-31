from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

v319 = HERE / "apply_diagnostic_harness_v319.py"
exec(compile(v319.read_text(encoding="utf-8"), str(v319), "exec"), {"__file__": str(v319), "__name__": "__main__"})

for layer_name in (
    "apply_v320_focus_cadence.py",
    "apply_v320_runtime_modes.py",
    "apply_v320_lua_fastpaths.py",
    "apply_v320_lua_runtime_modes.py",
    "apply_v320_sound_query_coalescing.py",
    "apply_v320_cp3_runtime_modes.py",
    "apply_v320_lua_profiler_recorder.py",
    "apply_v320_cp6_runtime_modes.py",
):
    layer = HERE / layer_name
    exec(compile(layer.read_text(encoding="utf-8"), str(layer), "exec"), {"__file__": str(layer), "__name__": "__main__"})

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.20 CP1 — focus cadence refinement
====================================

V3.20 is layered over the exact clean V3.19 P0 stable gaming source. CP1 keeps
[V3] v3.19 focus cadence at the promoted default 2 and adds the disabled-by-
default [V3] v3.20 adaptive focus cadence option. Fixed mode is the exact P0
decision path. Adaptive mode forces a refresh when the main camera view or
projection matrix changes, while the fixed cadence remains a hard maximum
staleness bound for moving objects observed by a stationary camera. GUI mode
still refreshes every frame and activation/input queries remain untouched.

Modes 109-113 provide exact P0/off, fixed cadence2, fixed cadence3, adaptive2,
and adaptive3 causal selections. Aggregate counters are published only through
the existing resource-stat collection path; CP1 adds no per-frame file logger.

V3.20 CP2 — engine/Lua and pure sound-conversion fast paths
===========================================================

CP2 promotes the mature V3.17 handler-presence and pure ID/path-conversion
prototype into independent native settings. Handler checks remain unload-safe:
an unloaded container is always treated as a possible recipient. The sound cache
stores only deterministic ESM::RefId and normalized-path values in bounded TLS
maps; it never stores SoundBuffer, OpenAL, mutable world, or missing-resource
state. Exact P0 control modes force both mechanisms off. Aggregate checks, skips,
dispatches, hits, misses, and evictions use the existing resource-stat channel.

V3.20 CP3 — same-frame sound-query coalescing
================================================

CP3 optionally coalesces identical isSoundPlaying queries only within one engine
frame. Lua play/stop calls immediately invalidate cached query results. Listener
updates, playback mutations, one-shots, and frame-to-frame state remain normal-
rate. SceneManager already performs positive object-cache lookup before a real
scene_template_miss, so CP3 deliberately adds no redundant template cache and no
negative cache.

V3.20 Lua profiler recorder checkpoint
=======================================

The engine-side recorder is disabled until the in-game console command
`luaProfilerRecord start` is issued. `luaProfilerRecord stop` closes and flushes
the current CSV and `luaProfilerRecord status` reports its state/path. Recording
uses the existing LuaState count hook and per-script allocator attribution. It
adds an exact per-frame instruction accumulator beside the unchanged 30-frame
display average, so bursts are retained instead of averaged away.

The CSV is sparse: one `[frame_total]` row is written every frame and per-script
rows are written when raw operations are nonzero or active/inactive memory
changes. Omitted script rows therefore mean zero operations and unchanged
memory. The existing bounded nonblocking writer records dropped rows rather than
stalling gameplay. Normal V3.20 gameplay keeps the tracking allocator available
so a session can start at runtime; the instruction hook remains off unless the
stock profiler setting or the recorder enables it. Inherited and exact-P0 lab
modes force the recorder capability off, preserving stock allocator identity.

V3.20 CP6 — stock-semantic safe LuaJIT optimizer runtime
=========================================================

CP6 packages a second LuaJIT DLL built from the exact stable-P0 LuaJIT source
plus the checked-in metatable specialization, global-environment specialization,
and improved sinking series. It imports no Rubic0n allocator, GC/finalizer,
sandbox, builtin, ABI, or content.lua change. Root lua51.dll remains stock; the
lab launcher swaps the safe runtime only for modes 122 and 124 and restores
stock after exit. Modes 121/122 and 123/124 are same-settings causal pairs.
''')

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

patch_text = patch.decode("utf-8", errors="replace")
engine_text = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
settings_text = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
cells_text = (ROOT / "components/settings/categories/cells.hpp").read_text(encoding="utf-8")
launcher_text = (ROOT / "tools/v3/launchers/V3_Lab.ps1").read_text(encoding="utf-8")
readme_text = readme_path.read_text(encoding="utf-8")

for marker in (
    "openmw-custom-v3.20-cp1-focus",
    "mV320AdaptiveFocusCadence",
    "OPENMW_V320_FOCUS_ADAPTIVE",
    "V320 Focus Attempted",
    "V320 Focus CadenceSkipped",
    "V320 Focus DirtyForced",
    "currentView != previousView",
):
    if marker not in engine_text and marker not in patch_text:
        raise RuntimeError(f"V3.20 CP1 generated source missing marker: {marker}")

if "v3.19 focus cadence = 2" not in settings_text:
    raise RuntimeError("V3.20 CP1 lost the promoted P0 focus default")
if "v3.20 adaptive focus cadence = false" not in settings_text:
    raise RuntimeError("V3.20 CP1 adaptive focus default is not fail-closed")
if "mV320AdaptiveFocusCadence" not in cells_text:
    raise RuntimeError("V3.20 CP1 adaptive focus setting is not registered")
for marker in ("v320-cp1-p0-control", "v320-cp1-adaptive2", "v320-cp1-adaptive3"):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.20 CP1 launcher missing marker: {marker}")
for marker in ("V3.20 CP1", "staleness bound", "activation/input queries remain untouched"):
    if marker not in readme_text:
        raise RuntimeError(f"V3.20 CP1 README missing marker: {marker}")
for forbidden in ("v319StaticInstance", "OPENMW_V319_STATIC_INSTANCING"):
    if forbidden in patch_text:
        raise RuntimeError(f"V3.20 CP1 contaminated by rejected P1/P1b marker: {forbidden}")

for rel, markers in {
    "components/settings/categories/lua.hpp": ("mV320EngineLuaFastPaths", "mV320SoundConversionCache"),
    "files/settings-default.cfg": (
        "v3.20 engine event fast paths = true",
        "v3.20 sound conversion cache = true",
    ),
    "apps/openmw/mwlua/engineevents.cpp": ("OPENMW_V320_ENGINE_LUA_FASTPATHS", "recordEventCheck"),
    "apps/openmw/mwlua/soundbindings.cpp": ("OPENMW_V320_SOUND_CONVERSION_CACHE", "recordSoundEviction"),
    "apps/openmw/mwlua/luamanagerimp.cpp": ("V320 Lua EventChecks", "V320 Lua SoundConversionEvictions"),
    "tools/v3/launchers/V3_Lab.ps1": ("v320-cp2-combined",),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.20 CP2 generated source missing {marker!r} in {rel}")

for rel, markers in {
    "components/settings/categories/lua.hpp": (
        "mV320SoundQueryCoalescing",
        "mV320LuaProfilerRecorderCapable",
    ),
    "files/settings-default.cfg": (
        "v3.20 sound query coalescing = false",
        "v3.20 lua profiler recorder capability = true",
    ),
    "apps/openmw/mwlua/soundbindings.cpp": ("OPENMW_V320_SOUND_QUERY_COALESCING", "mSoundQueryCoalesced"),
    "tools/v3/launchers/V3_Lab.ps1": ("Enter 1 through 124", "v320-cp3-combined"),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.20 CP3 generated source missing {marker!r} in {rel}")

for rel, markers in {
    "components/lua/luastate.hpp": ("setInstructionProfilerEnabled", "mInstructionProfilerEnabled"),
    "components/lua/scriptscontainer.hpp": ("mFrameInstructionCount",),
    "components/debug/v3diagnostics.hpp": ("closeChannel", "mCloseQueued"),
    "apps/openmw/mwlua/luamanagerimp.cpp": (
        "class V320LuaProfilerRecorder",
        "luaProfilerRecord start|stop|status",
        "[frame_total]",
        "OPENMW_V320_LUA_PROFILE_DIR",
    ),
    "tools/v3/launchers/V3_Lab.ps1": (
        "OPENMW_V320_LUA_PROFILE_DIR",
        "OPENMW_V320_LUA_PROFILER_CAPABLE",
        "$choice -notin @('114','118','121')",
    ),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.20 Lua recorder generated source missing {marker!r} in {rel}")

for exact_control in ("109", "114", "118", "121"):
    mode_line = next(line for line in launcher_text.splitlines() if f"'{exact_control}' {{" in line)
    if "V320LuaProfilerRecorderCapable" in mode_line:
        raise RuntimeError(f"Exact-control mode {exact_control} directly enables the Lua recorder capability")

for marker in (
    "Enter 1 through 124",
    "v320-cp6-safejit-only",
    "v320-cp6-combined-stock",
    "v320-cp6-combined-safejit",
    "safejit\\lua51.dll",
    "V320-SAFE-LUAJIT-RUNTIME.txt",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.20 CP6 generated launcher missing marker: {marker}")

print("V3.20 CP1+CP2+CP3+Lua-recorder+CP6 generated-source policy invariants passed")
