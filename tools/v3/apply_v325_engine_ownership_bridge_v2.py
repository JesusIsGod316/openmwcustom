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
    raise RuntimeError("V3.25 CP1 v2 failure: could not locate inherited controller-trace visitor replacement")
end += len(")\n")

replacement = r'''replace_exact(
    animation_cpp,
    "        {\\n"
    "            Debug::V36ControllerTrace::PhaseScope v36SourceAssign(\\n"
    "                v36ControllerTrace, Debug::V36ControllerTrace::Phase::SourceAssign);\\n"
    "            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\\n"
    "            mObjectRoot->accept(assignVisitor);\\n"
    "        }\\n",
    "        if (mAnimSourceBatchDepth != 0)\\n"
    "            mAnimSourceBatchNeedsControllerAssignment = true;\\n"
    "        else\\n"
    "        {\\n"
    "            Debug::V36ControllerTrace::PhaseScope v36SourceAssign(\\n"
    "                v36ControllerTrace, Debug::V36ControllerTrace::Phase::SourceAssign);\\n"
    "            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\\n"
    "            mObjectRoot->accept(assignVisitor);\\n"
    "        }\\n",
)
'''
source = source[:start] + replacement + source[end:]

exec(
    compile(source, str(ORIGINAL), "exec"),
    {"__file__": str(ORIGINAL), "__name__": "__main__"},
)
