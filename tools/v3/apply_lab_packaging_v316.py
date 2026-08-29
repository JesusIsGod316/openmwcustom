from pathlib import Path
import subprocess

# Apply the complete validated V3.15 stack first, then V3.16 general-play hitch work.
v315 = Path(__file__).with_name("apply_lab_packaging_v315.py")
exec(
    compile(v315.read_text(encoding="utf-8"), str(v315), "exec"),
    {"__file__": str(v315), "__name__": "__main__"},
)

v316 = Path(__file__).with_name("apply_v316_general_play_hitch.py")
exec(
    compile(v316.read_text(encoding="utf-8"), str(v316), "exec"),
    {"__file__": str(v316), "__name__": "__main__"},
)

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.16 general-play hitch suppression
====================================

Primary objective
-----------------
V3.16 freezes the promoted V3.15 Mode84 renderer/paging architecture and attacks
ordinary gameplay frametime spikes that occur while standing still, traversing,
or approaching actors. V3.15 logs showed repeated Dynamic Sounds / streamed
sound initialization stalls plus actor-local Lua bursts, smaller periodic resource
cleanup pulses, and a separate render-traversal synchronization lane.

First integrated mechanism
--------------------------
V3.16 backports official OpenMW upstream commit
9ec49cfb4709cbfd8f14e97f5b9a558b71b8184f (sound-file head cache). Streamed
music/voice initialization can reuse the exact prefix/suffix bytes FFmpeg needed
on the first open, moving subsequent initialization reads to RAM. The feature is
configured through [Sound] 'head cache size'. V3.16 keeps the engine default at
0 MB so the control remains exact; experiment modes explicitly select 64/128 MB.
The upstream per-file safety ceiling remains 256 KiB.

Runtime matrix
--------------
86 = exact V3.15 Mode84 control, sound head cache 0 MB
87 = Mode86 + 64 MB streamed-audio head cache
88 = balanced V3.16 hitch candidate, currently Mode84 + 64 MB audio cache and
     reserved for additional validated hitch suppressors in this build line
89 = aggressive V3.16 hitch candidate, currently Mode84 + 128 MB audio cache and
     reserved for aggressive validated hitch suppressors

Development policy
------------------
Do not weaken Lua/script semantics to hide expensive mods. Do not defer required
scene construction or V3.13 strong-wins cache installation. Any resource cleanup
or render-thread changes added later in V3.16 must be bounded and independently
switchable. Diagnostic I/O must not be allowed to become a gameplay-thread hitch
source.
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
readme_text = (ROOT / "V3-LAB-README.txt").read_text(encoding="utf-8")
for marker in (
    "head cache size",
    "mHeadCacheSize",
    "class HeadCache",
    "getStreamDecoder",
    "v316-mode84-control",
    "v316-audio64",
    "v316-balanced-hitch",
    "v316-aggressive-hitch",
):
    if marker not in patch_text and marker not in launcher_text:
        raise RuntimeError(f"V3.16 generated source snapshot missing marker: {marker}")

if "V3.16 general-play hitch suppression" not in readme_text:
    raise RuntimeError("V3.16 README identity marker missing")

print("V3.16 exact generated-source snapshot refreshed after complete V3.16 layer.")
