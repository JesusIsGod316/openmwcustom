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

# The inherited V3.6 controller profiler creates more than one textually identical
# source-assignment pair in the generated translation unit. Never patch globally.
# Restrict the transformation to Animation::addSingleAnimSource() and fail closed
# if that function does not contain exactly one historical immediate visitor.
replacement = r'''path = ROOT / animation_cpp
text = path.read_text(encoding="utf-8")
func_start_marker = "    std::shared_ptr<Animation::AnimSource> Animation::addSingleAnimSource(\n"
func_end_marker = "    void Animation::clearAnimSources()\n"
func_start = text.find(func_start_marker)
func_end = text.find(func_end_marker, func_start if func_start >= 0 else 0)
if func_start < 0 or func_end < 0 or func_end <= func_start:
    raise RuntimeError(f"{animation_cpp}: V3.25 could not isolate addSingleAnimSource generated body")
func = text[func_start:func_end]
old_assign = (
    "            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\n"
    "            mObjectRoot->accept(assignVisitor);\n"
)
if func.count(old_assign) != 1:
    raise RuntimeError(
        f"{animation_cpp}: V3.25 expected one controller-source assignment inside addSingleAnimSource, "
        f"found {func.count(old_assign)}"
    )
new_assign = (
    "            if (mAnimSourceBatchDepth != 0)\n"
    "                mAnimSourceBatchNeedsControllerAssignment = true;\n"
    "            else\n"
    "            {\n"
    "                SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);\n"
    "                mObjectRoot->accept(assignVisitor);\n"
    "            }\n"
)
func = func.replace(old_assign, new_assign, 1)
text = text[:func_start] + func + text[func_end:]
path.write_text(text, encoding="utf-8", newline="\n")
print("V3.25 patched addSingleAnimSource controller assignment in function scope")
'''
source = source[:start] + replacement + source[end:]

exec(
    compile(source, str(ORIGINAL), "exec"),
    {"__file__": str(ORIGINAL), "__name__": "__main__"},
)
