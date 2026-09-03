import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
path = ROOT / "apps/openmw/mwrender/occlusionculling.cpp"
text = path.read_text(encoding="utf-8")
old = "std::stable_sort(v322Candidates.begin(), v322Candidates.end(),"
count = text.count(old)
if count != 2:
    raise RuntimeError(f"V3.22 CP2 rank refinement expected 2 stable_sort sites, found {count}")
text = text.replace(old, "std::sort(v322Candidates.begin(), v322Candidates.end(),")
path.write_text(text, encoding="utf-8", newline="\n")

if "std::stable_sort(v322Candidates" in text:
    raise RuntimeError("V3.22 CP2 rank refinement left a candidate stable_sort site")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary", "--", ":!V3-applied-source.patch", ":!V3-applied-source-stat.txt"],
    cwd=ROOT,
    check=True,
    stdout=(ROOT / "V3-applied-source.patch").open("w", encoding="utf-8", newline="\n"),
)
subprocess.run(
    ["git", "diff", "--stat", "--", ":!V3-applied-source.patch", ":!V3-applied-source-stat.txt"],
    cwd=ROOT,
    check=True,
    stdout=(ROOT / "V3-applied-source-stat.txt").open("w", encoding="utf-8", newline="\n"),
)
print("V3.22 CP2 ranking hot-path refinement applied: stable_sort -> in-place sort")
