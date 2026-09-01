from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

v320 = HERE / "apply_diagnostic_harness_v320.py"
exec(
    compile(v320.read_text(encoding="utf-8"), str(v320), "exec"),
    {"__file__": str(v320), "__name__": "__main__"},
)

for layer_name in (
    "apply_v321_completion_governor.py",
    "apply_v321_compile_header_fix.py",
    "apply_v321_runtime_modes.py",
):
    layer = HERE / layer_name
    exec(
        compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
        {"__file__": str(layer), "__name__": "__main__"},
    )

# The final V3.20 generated engine rewrites traversal into a helper scope where
# stats/frameNumber remain available but frame()'s cached reportResource local
# does not. Keep resource-stat emission semantically equivalent by querying the
# same viewer-stats collection flag at the V3.21 insertion point. Fail closed if
# the expected CP1 block is not present so preflight cannot hide source drift.
engine_path = ROOT / "apps/openmw/engine.cpp"
engine_text = engine_path.read_text(encoding="utf-8")
old_resource_guard = '''                if (reportResource)
                {
                    stats->setAttribute(frameNumber, "V321 Completion Seen", counters.mCompletedSeen);'''
new_resource_guard = '''                if (stats->collectStats("resource"))
                {
                    stats->setAttribute(frameNumber, "V321 Completion Seen", counters.mCompletedSeen);'''
if engine_text.count(old_resource_guard) != 1:
    raise RuntimeError(
        "V3.21 CP1 generated engine resource-stats scope repair expected exactly one completion block"
    )
engine_path.write_text(
    engine_text.replace(old_resource_guard, new_resource_guard, 1),
    encoding="utf-8",
    newline="\n",
)
print("V3.21 CP1 repaired generated resource-stats scope")

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(
        r'''

V3.21 CP1 — exterior completed-work admission pacing
=====================================================

V3.21 begins from the final normal V3.20 foundation. Mode 125 is an exact
behavioral control cloned from V3.20 Mode 123: stock LuaJIT, promoted focus
cadence 2, and the retained V3.20 gameplay stack. Mode 126 changes only the
new V3.21 completed-work governor.

The fixed governor does not throttle WorkQueue threads, terrain preload jobs,
object preparation, or prediction. Async producers continue to prepare useful
work at normal rate. The governor acts downstream at OSG's IncrementalCompile-
Operation: it restores the configured target frame rate instead of V3.8's
aggressive 36 Hz compile target, caps GL compile objects per frame, and holds
fully compiled CompileSets in a FIFO before Viewer::updateTraversal().

Only a bounded number of completed CompileSets are exposed for main-thread
merge/install each frame. A bounded-age supplement admits additional oldest
sets after the configured maximum deferral so queue entries cannot starve.
Resource stats expose total completions/admissions/forced progress, per-frame
admission, deferred depth, oldest age, and peak deferred depth. This is
substantive pacing plus attribution, not a telemetry-only cycle.

Default settings keep the governor off. CP1 validation must use Modes 125 and
126 on the same save/mod/settings/route with the frame pacer off or nonbinding.
Promotion requires lower p95/p99 and fewer >33.3 ms / >50 ms frames and render
spikes without persistent deferred-queue growth, visible paging/pop-in
regression, or a meaningful steady-state performance loss.
'''
    )

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"],
    cwd=ROOT,
    check=True,
    stdout=subprocess.PIPE,
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"],
    cwd=ROOT,
    check=True,
    text=True,
    stdout=subprocess.PIPE,
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

patch_text = patch.decode("utf-8", errors="replace")
engine_text = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
render_text = (ROOT / "apps/openmw/mwrender/renderingmanager.cpp").read_text(encoding="utf-8")
settings_text = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
cells_text = (ROOT / "components/settings/categories/cells.hpp").read_text(encoding="utf-8")
launcher_text = (ROOT / "tools/v3/launchers/V3_Lab.ps1").read_text(encoding="utf-8")
readme_text = readme_path.read_text(encoding="utf-8")

for marker in (
    "openmw-custom-v3.21-cp1-completion-governor",
    "V321 Completion Deferred",
    "V321 Completion OldestAge",
    "getCompiledMutex",
    "getCompiled()",
    "#include <osgUtil/IncrementalCompileOperation>",
    'stats->collectStats("resource")',
):
    if marker not in engine_text:
        raise RuntimeError(f"V3.21 CP1 engine source missing marker: {marker}")

for marker in (
    "OPENMW_V321_COMPLETION_GOVERNOR",
    "mV321CompileObjectsPerFrame",
    "mV321CompileMinimumMilliseconds",
    "compileTarget = configuredTarget",
):
    if marker not in render_text:
        raise RuntimeError(f"V3.21 CP1 renderer source missing marker: {marker}")

for marker in (
    "mV321CompletionGovernorMode",
    "mV321MergeSetsPerFrame",
    "mV321MaxDeferredFrames",
    "mV321ForcedMergeSets",
):
    if marker not in cells_text:
        raise RuntimeError(f"V3.21 CP1 setting registration missing marker: {marker}")

for marker in (
    "v3.21 completion governor mode = 0",
    "v3.21 compile objects per frame = 4",
    "v3.21 merge sets per frame = 2",
    "v3.21 max deferred frames = 4",
):
    if marker not in settings_text:
        raise RuntimeError(f"V3.21 CP1 default settings missing marker: {marker}")

for marker in (
    "Enter 1 through 126",
    "v321-cp1-v320-control",
    "v321-cp1-fixed-completion-governor",
    "OPENMW_V321_COMPLETION_GOVERNOR",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.21 CP1 launcher missing marker: {marker}")

line123 = next(line for line in launcher_text.splitlines() if line.lstrip().startswith("'123'"))
line125 = next(line for line in launcher_text.splitlines() if line.lstrip().startswith("'125'"))
line126 = next(line for line in launcher_text.splitlines() if line.lstrip().startswith("'126'"))
if "$V317LuaRuntime = 'stock'" not in line123 or "$V317LuaRuntime = 'stock'" not in line125 or "$V317LuaRuntime = 'stock'" not in line126:
    raise RuntimeError("V3.21 CP1 causal pair lost the stock-LuaJIT V3.20 foundation")
if "$V319FocusCadence = '2'" not in line123 or "$V319FocusCadence = '2'" not in line125 or "$V319FocusCadence = '2'" not in line126:
    raise RuntimeError("V3.21 CP1 causal pair lost the promoted focus cadence 2")
if "$V321CompletionGovernor = '0'" not in line125:
    raise RuntimeError("V3.21 Mode125 is not an explicit governor-off control")
if "$V321CompletionGovernor = '1'" not in line126:
    raise RuntimeError("V3.21 Mode126 does not enable the fixed governor")
if "safejit" in line125.lower() or "safejit" in line126.lower():
    raise RuntimeError("V3.21 CP1 control/governor modes unexpectedly selected experimental Safe-JIT")

for marker in (
    "V3.21 CP1",
    "does not throttle WorkQueue",
    "bounded-age supplement",
    "persistent deferred-queue growth",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.21 CP1 README missing marker: {marker}")

if "v3.21" not in patch_text.lower():
    raise RuntimeError("V3.21 generated patch has no V3.21 source identity marker")

print("V3.21 CP1 fixed completion-governor generated-source invariants passed")
