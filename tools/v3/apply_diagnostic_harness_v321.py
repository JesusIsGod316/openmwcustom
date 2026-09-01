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
    "apply_v321_adaptive_governor.py",
    "apply_v321_runtime_modes.py",
):
    layer = HERE / layer_name
    exec(
        compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
        {"__file__": str(layer), "__name__": "__main__"},
    )

# The final V3.20 generated engine rewrites traversal into a helper scope where
# stats/frameNumber remain available but frame()'s cached reportResource local
# does not. Preserve the same resource-stat gating by querying viewer stats at
# the V3.21 insertion point. Repair both the fixed and adaptive blocks and fail
# closed if either expected generated-source anchor has drifted.
engine_path = ROOT / "apps/openmw/engine.cpp"
engine_text = engine_path.read_text(encoding="utf-8")
old_fixed_guard = '''                if (reportResource)
                {
                    stats->setAttribute(frameNumber, "V321 Completion Seen", counters.mCompletedSeen);'''
new_fixed_guard = '''                if (stats->collectStats("resource"))
                {
                    stats->setAttribute(frameNumber, "V321 Completion Seen", counters.mCompletedSeen);'''
old_adaptive_guard = "                    if (reportResource && v321CompletionGovernorMode == 2)"
new_adaptive_guard = '                    if (stats->collectStats("resource") && v321CompletionGovernorMode == 2)'
if engine_text.count(old_fixed_guard) != 1:
    raise RuntimeError(
        "V3.21 CP1 adaptive generated engine scope repair expected exactly one fixed completion block"
    )
if engine_text.count(old_adaptive_guard) != 1:
    raise RuntimeError(
        "V3.21 CP1 adaptive generated engine scope repair expected exactly one adaptive stats block"
    )
engine_text = engine_text.replace(old_fixed_guard, new_fixed_guard, 1)
engine_text = engine_text.replace(old_adaptive_guard, new_adaptive_guard, 1)
engine_path.write_text(engine_text, encoding="utf-8", newline="\n")
print("V3.21 CP1 repaired generated resource-stats scope for fixed + adaptive governors")

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(
        r'''

V3.21 CP1 — exterior completed-work admission pacing
=====================================================

V3.21 begins from the final normal V3.20 foundation. Mode 125 is an exact
behavioral control cloned from V3.20 Mode 123: stock LuaJIT, promoted focus
cadence 2, and the retained V3.20 gameplay stack. Mode 126 changes only the
new V3.21 fixed completed-work governor. Mode 127 retains the same fixed ICO
compile cap but adapts completed CompileSet merge admission from the previous
completed frame's wall time.

The governors do not throttle WorkQueue threads, terrain preload jobs, object
preparation, or prediction. Async producers continue to prepare useful work at
normal rate. The mechanism acts downstream at OSG's IncrementalCompileOperation:
it restores the configured target frame rate instead of V3.8's aggressive 36 Hz
compile target, caps GL compile objects per frame, and holds fully compiled
CompileSets in a FIFO before Viewer::updateTraversal().

Mode 126 admits a fixed bounded number of completed CompileSets per frame. Mode
127 changes only that merge budget. It uses the previously completed frame plus
a bounded EMA, guarantees a nonzero minimum service rate, accrues only bounded
service debt when pressure suppresses the fixed budget, and repays at most a
small configured amount on slack frames. The existing bounded-age supplement
still provides mandatory oldest-item escape. It never reacts to partial timing
from the frame currently being serviced.

Resource stats expose total completions/admissions/forced progress, deferred
depth and age, plus MODE 127 previous-frame time, EMA, adaptive merge budget,
debt, and debt repayment. This is substantive pacing plus attribution, not a
telemetry-only cycle.

Default settings keep the governor off. CP1 validation must compare Modes 125,
126, and 127 on the same save/mod/settings/route with the frame pacer off or
nonbinding. Promotion requires lower p95/p99 and fewer >33.3 ms / >50 ms frames
and render spikes without persistent deferred-queue growth, visible paging or
pop-in regression, or meaningful steady-state performance loss.
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
    "openmw-custom-v3.21-cp1-adaptive-governor",
    "V321 Completion Deferred",
    "V321 Completion OldestAge",
    "V321 Adaptive PreviousFrameMs",
    "V321 Adaptive MergeBudget",
    "V321 Adaptive Debt",
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
    "parsed <= 2",
):
    if marker not in render_text:
        raise RuntimeError(f"V3.21 CP1 renderer source missing marker: {marker}")

for marker in (
    "mV321CompletionGovernorMode",
    "mV321MergeSetsPerFrame",
    "mV321MaxDeferredFrames",
    "mV321ForcedMergeSets",
    "mV321AdaptiveTargetMilliseconds",
    "mV321AdaptiveFrameEmaAlpha",
    "mV321AdaptiveMergeMin",
    "mV321AdaptiveMergeMax",
    "mV321AdaptiveDebtCap",
    "mV321AdaptiveDebtRepayPerFrame",
):
    if marker not in cells_text:
        raise RuntimeError(f"V3.21 CP1 setting registration missing marker: {marker}")

for marker in (
    "v3.21 completion governor mode = 0",
    "v3.21 compile objects per frame = 4",
    "v3.21 merge sets per frame = 2",
    "v3.21 max deferred frames = 4",
    "v3.21 adaptive target milliseconds = 24.0",
    "v3.21 adaptive merge minimum = 1",
    "v3.21 adaptive merge maximum = 4",
    "v3.21 adaptive debt cap = 8",
):
    if marker not in settings_text:
        raise RuntimeError(f"V3.21 CP1 default settings missing marker: {marker}")

for marker in (
    "Enter 1 through 127",
    "v321-cp1-v320-control",
    "v321-cp1-fixed-completion-governor",
    "v321-cp1-adaptive-completion-governor",
    "OPENMW_V321_COMPLETION_GOVERNOR",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.21 CP1 launcher missing marker: {marker}")

line123 = next(line for line in launcher_text.splitlines() if line.lstrip().startswith("'123'"))
line125 = next(line for line in launcher_text.splitlines() if line.lstrip().startswith("'125'"))
line126 = next(line for line in launcher_text.splitlines() if line.lstrip().startswith("'126'"))
line127 = next(line for line in launcher_text.splitlines() if line.lstrip().startswith("'127'"))
for line in (line123, line125, line126, line127):
    if "$V317LuaRuntime = 'stock'" not in line:
        raise RuntimeError("V3.21 CP1 causal modes lost the stock-LuaJIT V3.20 foundation")
    if "$V319FocusCadence = '2'" not in line:
        raise RuntimeError("V3.21 CP1 causal modes lost promoted focus cadence 2")
if "$V321CompletionGovernor = '0'" not in line125:
    raise RuntimeError("V3.21 Mode125 is not an explicit governor-off control")
if "$V321CompletionGovernor = '1'" not in line126:
    raise RuntimeError("V3.21 Mode126 does not enable the fixed governor")
if "$V321CompletionGovernor = '2'" not in line127:
    raise RuntimeError("V3.21 Mode127 does not enable the adaptive governor")
if any("safejit" in line.lower() for line in (line125, line126, line127)):
    raise RuntimeError("V3.21 CP1 causal modes unexpectedly selected experimental Safe-JIT")

for marker in (
    "do not throttle WorkQueue",
    "previously completed frame",
    "nonzero minimum service rate",
    "bounded",
    "persistent deferred-queue growth",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.21 CP1 README missing marker: {marker}")

if "v3.21" not in patch_text.lower():
    raise RuntimeError("V3.21 generated patch has no V3.21 source identity marker")

print("V3.21 CP1 fixed + adaptive completion-governor generated-source invariants passed")
