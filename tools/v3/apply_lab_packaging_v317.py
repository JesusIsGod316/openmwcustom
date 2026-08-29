from pathlib import Path
import subprocess

# V3.17 is a strict additive layer over the validated V3.16 stack.
v316 = Path(__file__).with_name("apply_lab_packaging_v316.py")
exec(
    compile(v316.read_text(encoding="utf-8"), str(v316), "exec"),
    {"__file__": str(v316), "__name__": "__main__"},
)

# First substantive V3.17 change: consolidate all V3 diagnostic CSV transport
# behind one bounded writer thread and eliminate periodic gameplay-cadence flushes.
v317_diag = Path(__file__).with_name("apply_v317_diagnostic_hub.py")
exec(
    compile(v317_diag.read_text(encoding="utf-8"), str(v317_diag), "exec"),
    {"__file__": str(v317_diag), "__name__": "__main__"},
)

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.17 Lua/runtime hitch consolidation
=====================================

Primary objective
-----------------
V3.17 keeps the V3.16 balanced hitch architecture as its gameplay foundation and
attacks recurring Lua/runtime/event tails. The planned runtime matrix separates a
stock-LuaJIT control, Rubic0n runtime attribution, engine-side Lua/materialization
work, a combined candidate, and an aggressive SFX-predecode variant.

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
readme_text = readme_path.read_text(encoding="utf-8")
for marker in (
    "class DiagnosticWriterHub",
    "sMaxQueuedItems = 16384",
    "mQueue.push_back({ channel, {}, true })",
    "No gameplay-cadence flushes",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.17 generated source snapshot missing marker: {marker}")

for forbidden in (
    "linesSinceFlush >= 240",
    "static constexpr std::size_t sMaxQueuedLines = 4096",
):
    if forbidden in patch_text:
        raise RuntimeError(f"V3.17 generated source still contains superseded diagnostic path: {forbidden}")

if "V3.17 Lua/runtime hitch consolidation" not in readme_text:
    raise RuntimeError("V3.17 README identity marker missing")
if "Consolidated diagnostic writer" not in readme_text:
    raise RuntimeError("V3.17 diagnostics README marker missing")

print("V3.17 generated-source snapshot refreshed after V3.17 additive layers.")
