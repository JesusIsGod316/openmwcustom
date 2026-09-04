from pathlib import Path

HERE = Path(__file__).resolve().parent
ORIGINAL = HERE / "apply_v325_engine_ownership_bridge.py"

if not ORIGINAL.is_file():
    raise RuntimeError("V3.25 CP1 v2 failure: original ownership bridge layer is missing")

source = ORIGINAL.read_text(encoding="utf-8")
start_marker = '''replace_exact(
    animation_cpp,
    "        SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\\n"'''
end_marker = ''')

replace_exact(
    npc_cpp,
'''
start = source.find(start_marker)
end = source.find(end_marker, start if start >= 0 else 0)
if start < 0 or end < 0:
    raise RuntimeError("V3.25 CP1 v2 failure: could not locate original visitor replacement")
end += len(")\n")

# The inherited V3.6 controller profiler wraps the visitor in its own PhaseScope,
# which changes only indentation around the two mutation lines. Patch those lines
# inside the existing scope instead of matching the whole profiled block. This
# preserves historical diagnostics while making only the expensive visitor itself
# conditional on the explicit V3.25 batch.
replacement = r'''replace_exact(
    animation_cpp,
    "            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\\n"
    "            mObjectRoot->accept(assignVisitor);\\n",
    "            if (mAnimSourceBatchDepth != 0)\\n"
    "                mAnimSourceBatchNeedsControllerAssignment = true;\\n"
    "            else\\n"
    "            {\\n"
    "                SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\\n"
    "                mObjectRoot->accept(assignVisitor);\\n"
    "            }\\n",
)
'''
source = source[:start] + replacement + source[end:]

exec(
    compile(source, str(ORIGINAL), "exec"),
    {"__file__": str(ORIGINAL), "__name__": "__main__"},
)
