from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
ORIGINAL = HERE / "apply_v325_engine_ownership_bridge_cp2_actorbatch.py"

if not ORIGINAL.is_file():
    raise RuntimeError("V3.25 CP2 actor-batch v2 failure: original actor-batch layer is missing")

# Apply the original actor-batched Mode151 refinement first.
exec(
    compile(ORIGINAL.read_text(encoding="utf-8"), str(ORIGINAL), "exec"),
    {"__file__": str(ORIGINAL), "__name__": "__main__"},
)

animation_cpp = ROOT / "apps/openmw/mwrender/animation.cpp"
text = animation_cpp.read_text(encoding="utf-8")

func_start_marker = "    std::shared_ptr<Animation::AnimSource> Animation::addSingleAnimSource(\n"
func_end_marker = "    void Animation::clearAnimSources()\n"
func_start = text.find(func_start_marker)
func_end = text.find(func_end_marker, func_start if func_start >= 0 else 0)
if func_start < 0 or func_end < 0 or func_end <= func_start:
    raise RuntimeError("V3.25 CP2 actor-batch v2 could not isolate addSingleAnimSource")

func = text[func_start:func_end]

# The original actor-batch replacement starts at the Mode151 `else` branch and
# replaces through the mAnimSources publication marker. The predecessor region
# also contained the closing brace for the surrounding V3.6 ControllerClone
# PhaseScope. Restore that outer scope close explicitly.
bad_tail = "            }\n\n        mAnimSources.push_back(animsrc);"
good_tail = "            }\n        }\n\n        mAnimSources.push_back(animsrc);"
if func.count(bad_tail) != 1:
    raise RuntimeError(
        "V3.25 CP2 actor-batch v2 expected exactly one missing profiling-scope close before mAnimSources"
    )
func = func.replace(bad_tail, good_tail, 1)

# Fail closed on the exact structural condition that escaped the first preflight.
# addSingleAnimSource contains no brace-bearing string literals in this generated
# region, so a direct brace count is a useful cheap syntax guard before MSVC.
if func.count("{") != func.count("}"):
    raise RuntimeError(
        f"V3.25 CP2 actor-batch v2 unbalanced addSingleAnimSource braces: "
        f"open={func.count('{')} close={func.count('}')}"
    )
if func.count(good_tail) != 1:
    raise RuntimeError("V3.25 CP2 actor-batch v2 profiling-scope close was not restored")

text = text[:func_start] + func + text[func_end:]
animation_cpp.write_text(text, encoding="utf-8", newline="\n")

print("V3.25 CP2 actor-batch v2 restored ControllerClone profiling scope and passed brace guard")
