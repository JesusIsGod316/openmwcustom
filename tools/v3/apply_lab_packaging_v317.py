from pathlib import Path
import subprocess

# V3.17 is a strict additive layer over the validated V3.16 stack.
v316 = Path(__file__).with_name("apply_lab_packaging_v316.py")
exec(
    compile(v316.read_text(encoding="utf-8"), str(v316), "exec"),
    {"__file__": str(v316), "__name__": "__main__"},
)

# Substantive engine-side V3.17 lane. This is deliberately applied only after
# the complete V3.16/V3.7 generated stack exists because it upgrades the mature
# loaded-container handler fast path and the final Lua/sound binding source.
# The fixed wrapper changes only two engineevents.cpp anchors so all pre-existing
# V3.3 event attribution remains intact.
v317_engine_lua = Path(__file__).with_name("apply_v317_engine_lua_fastpaths_fixed.py")
exec(
    compile(v317_engine_lua.read_text(encoding="utf-8"), str(v317_engine_lua), "exec"),
    {"__file__": str(v317_engine_lua), "__name__": "__main__"},
)

# Consolidate all V3 diagnostic CSV transport behind one bounded writer thread
# and eliminate periodic gameplay-cadence flushes.
v317_diag = Path(__file__).with_name("apply_v317_diagnostic_hub.py")
exec(
    compile(v317_diag.read_text(encoding="utf-8"), str(v317_diag), "exec"),
    {"__file__": str(v317_diag), "__name__": "__main__"},
)

# Add the full V3.17 attribution matrix only after all gameplay layers have
# settled so Mode90 can copy the final Mode88 body and Mode94 the final Mode89 body.
v317_modes = Path(__file__).with_name("apply_v317_runtime_modes.py")
exec(
    compile(v317_modes.read_text(encoding="utf-8"), str(v317_modes), "exec"),
    {"__file__": str(v317_modes), "__name__": "__main__"},
)

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.17 Lua/runtime hitch consolidation
=====================================

Primary objective
-----------------
V3.17 keeps the V3.16 balanced hitch architecture as its gameplay foundation and
attacks recurring Lua/runtime/event tails. The runtime matrix separates a stock-
LuaJIT control, Rubic0n runtime attribution, engine-side Lua/materialization work,
a combined candidate, and an aggressive SFX-predecode variant.

Engine Lua/event fast paths
---------------------------
Mode92+ adds conservative handler-presence checks before engine events construct
Lua wrapper arguments or resolve secondary RefNums that no callback can consume.
A loaded container whose current handler list is empty is a known negative and can
be skipped. An unloaded container is ALWAYS treated as "may have handlers" because
its top-level Lua code may legally choose a different handler table when it is
materialized again. V3.17 therefore does not persist negative handler-interest
state across unload/reload and does not suppress legitimate first materialization.
Lua-debug missing-object lookups are retained on otherwise-skipped event paths.

Lua -> sound conversion cache
-----------------------------
Mode92+ also keeps a small per-thread cache for the immutable conversions performed
at the Lua sound API boundary: textual sound IDs to ESM::RefId and file-name text
to normalized VFS paths. Each cache is capped at 4096 entries and keys larger than
512 bytes bypass it. Once full, new keys are parsed normally rather than clearing
the cache in gameplay. No SoundBuffer, OpenAL handle, decoded PCM, or mutable sound
state is cached here, and the normal play path never waits on predecode work.

Consolidated diagnostic writer
------------------------------
V3.16 correctly removed synchronous producer-thread CSV writes, but each enabled
CsvWriter still owned its own disk thread and flushed every 240 rows. V3.17 routes
all V3 CsvWriter instances through one shared bounded queue and one shared writer
thread. Producers use a nonblocking try-lock and drop diagnostic rows rather than
waiting. File open/header work is also queued to the writer so enabled diagnostics
do not open files on a gameplay thread. There are no periodic CSV flushes during
ordinary gameplay; streams drain and flush once at orderly shutdown. The shared
queue is bounded at 16384 items and dropped-row accounting remains per stream.

Runtime attribution matrix
--------------------------
90 = exact final V3.16 Mode88 gameplay settings + stock packaged LuaJIT.
91 = Mode90 + pinned sandboxed Rubic0n runtime only.
92 = Mode90 + V3.17 engine-side Lua/materialization optimizations, stock LuaJIT.
93 = Mode90 + Rubic0n + V3.17 engine-side Lua/materialization optimizations.
94 = Mode93 but inherits V3.16 Mode89 aggressive SFX retention/predecode budgets.

The launcher selects lua51.dll before OpenMW starts and restores the staged stock
runtime after the process exits. Direct/non-lab launches therefore stay stock by
default. Every profile records the selected DLL SHA256 and the packaged runtime
identity manifest.

Runtime safety policy
---------------------
The Rubic0n lane is source/revision pinned and sandboxed. Current OpenMW content.lua
is retained unless a real compatibility dependency is demonstrated. Experimental
Rubic0n allocator/finalizer behavior is not promoted merely because it exists;
unsafe semantic changes require explicit OpenMW/Sol userdata auditing first.
''')

ROOT = Path(__file__).resolve().parents[2]
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
launcher_text = (ROOT / "tools/v3/launchers/V3_Lab.ps1").read_text(encoding="utf-8")
readme_text = readme_path.read_text(encoding="utf-8")
for marker in (
    "class DiagnosticWriterHub",
    "sMaxQueuedItems = 16384",
    "mQueue.push_back({ channel, {}, true })",
    "No gameplay-cadence flushes",
    "mightHaveEngineHandlers",
    "v317EngineFastPathEnabled",
    "class V317SoundBindingCache",
    "v317SoundBindingCache().soundId(soundId)",
    "v317SoundBindingCache().path(fileName)",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.17 generated source snapshot missing marker: {marker}")

for marker in (
    "90 = V3.17 control",
    "v317-rubicon-only",
    "v317-engine-lua-only",
    "v317-combined-balanced",
    "v317-combined-aggressive-sfx",
    "OPENMW_V317_LUA_OPT",
    "v317-runtime",
    "Enter 1 through 94",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.17 generated launcher missing marker: {marker}")

for forbidden in (
    "linesSinceFlush >= 240",
    "static constexpr std::size_t sMaxQueuedLines = 4096",
):
    if forbidden in patch_text:
        raise RuntimeError(f"V3.17 generated source still contains superseded diagnostic path: {forbidden}")

for marker in (
    "V3.17 Lua/runtime hitch consolidation",
    "Engine Lua/event fast paths",
    "Lua -> sound conversion cache",
    "Consolidated diagnostic writer",
    "Runtime attribution matrix",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.17 README marker missing: {marker}")

print("V3.17 generated-source snapshot refreshed after substantive V3.17 layers.")
