from pathlib import Path
import subprocess

# Apply the complete validated V3.14 stack first, then V3.15 renderer/tail work.
v314 = Path(__file__).with_name("apply_lab_packaging_v314_fixed.py")
exec(compile(v314.read_text(encoding="utf-8"), str(v314), "exec"),
    {"__file__": str(v314), "__name__": "__main__"})

v315 = Path(__file__).with_name("apply_v315_render_submission_tail.py")
exec(compile(v315.read_text(encoding="utf-8"), str(v315), "exec"),
    {"__file__": str(v315), "__name__": "__main__"})

# MSVC/OSG exposes both removeChild(Node*) and removeChild(unsigned int, unsigned int).
# The literal 0 in the V3.15 packet recombination path is therefore ambiguous on
# Windows. Make the indexed-removal overload explicit before snapshotting/building.
objectpaging_path = Path(__file__).resolve().parents[2] / "apps/openmw/mwrender/objectpaging.cpp"
objectpaging_text = objectpaging_path.read_text(encoding="utf-8")
old_remove = "                    packet->removeChild(0);"
new_remove = "                    packet->removeChild(0u, 1u);"
if objectpaging_text.count(old_remove) != 1:
    raise RuntimeError(
        f"V3.15 packet removeChild hotfix expected 1 match, found {objectpaging_text.count(old_remove)}"
    )
objectpaging_path.write_text(objectpaging_text.replace(old_remove, new_remove, 1), encoding="utf-8", newline="\n")
print("V3.15 fixed ambiguous osg::Group::removeChild packet recombination call for MSVC.")

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.15 render submission / traversal-tail layer
==============================================

Foundation
----------
Mode80 is an exact runtime copy of promoted V3.14 Mode79. V3.15 does not change
V3.13 strong-wins cache-quality behavior, Lua semantics, groundcover density,
shadow quality, or the external Rafael PBR shader algorithms.

New mechanisms
--------------
1. Premerge shared-state canonicalization
   Strong compile=true ObjectPaging merge groups can be passed through OpenMW's
   existing SharedStateManager before MERGE_GEOMETRY. OpenMW's merge comparator
   groups geometry by StateSet pointer identity, so canonicalizing structurally
   equal immutable StateSets before the merger exposes more render-equivalent PBR
   geometry without inventing a new material signature or weakening compatibility.
2. Temporary hierarchical packet premerge
   Strong prepared active-grid merge candidates may be distributed into 4 or 16
   spatial packets. Each packet receives only flatten/remove/merge work, then every
   result is recombined into ONE group before the existing Mode79-quality final
   optimizer and VERTEX_POSTTRANSFORM pass. No packet survives as final topology;
   this explicitly avoids the V3.12/Mode76 submission regression.
3. Adaptive ICO compile governor
   IncrementalCompileOperation's speculative per-frame budget is adjusted from the
   actual frame duration. Expensive frames collapse background GL compilation to
   one object and a conservative budget; cheap frames restore aggressive prewarm.
   Required/demand scene work and V3.13 cache-quality repair are never deferred.

Runtime matrix
--------------
80 = exact V3.14 Mode79 control
81 = Mode80 + premerge state canonicalization
82 = Mode81 + 4-packet temporary hierarchical premerge
83 = Mode81 + balanced adaptive ICO governor
84 = full balanced candidate: canonicalization + 4 packets + balanced governor
85 = aggressive candidate: canonicalization + 16 packets + aggressive governor

Deferred-but-promising upstream dev backport
--------------------------------------------
Current OpenMW master has a post-0.52 water path that removes the dedicated
refraction RTT and resolves opaque color/depth from the main render pipeline.
It is potentially a material GPU win for refraction-enabled water, but upstream
required follow-up fixes for underwater sorting and texture-unit handling. V3.15
tracks it as a separate future switch/backport rather than partially mixing it
into the first renderer-submission binary.
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
for marker in (
    "v3.15 premerge state canonicalization",
    "mV315PacketizedPremergeMode",
    "v315_packet_premerge",
    "v315CanonicalizeBeforeMerge",
    "mV315AdaptiveCompileGovernor",
    "v315-mode79-control",
    "v315-balanced-full",
    "v315-aggressive-full",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.15 exact generated-source snapshot missing marker: {marker}")

print("V3.15 exact generated-source snapshot refreshed after complete V3.15 layer.")
