import os
import subprocess
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 CP2 generic-class match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 CP2 generic classification patched {rel} ({count} match(es))")


# Carry source identity explicitly from CellPreloader into SceneManager. Existing
# callers keep the Unknown default and existing ICO submission semantics.
replace_exact(
    "components/resource/scenemanager.hpp",
    '#include "resourcemanager.hpp"\n',
    '#include "resourcemanager.hpp"\n#include "v321compileclass.hpp"\n',
)
replace_exact(
    "components/resource/scenemanager.hpp",
    '        osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path, bool compile = true);',
    '''        osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path, bool compile = true,
            V321CompileClass compileClass = V321CompileClass::Unknown);''',
)
replace_exact(
    "components/resource/scenemanager.cpp",
    '    osg::ref_ptr<const osg::Node> SceneManager::getTemplate(VFS::Path::NormalizedView path, bool compile)',
    '''    osg::ref_ptr<const osg::Node> SceneManager::getTemplate(
        VFS::Path::NormalizedView path, bool compile, V321CompileClass compileClass)''',
)
replace_exact(
    "components/resource/scenemanager.cpp",
    '''            if (compile && mIncrementalCompileOperation)
                mIncrementalCompileOperation->add(loaded);
            else
                loaded->getBound();''',
    '''            if (compile && mIncrementalCompileOperation)
            {
                // CP2 source identity is attached to the exact subgraph already submitted to ICO.
                // Unknown preserves all non-preloader callers without guessing after completion.
                markV321CompileClass(*loaded, compileClass);
                mIncrementalCompileOperation->add(loaded);
            }
            else
                loaded->getBound();''',
)
replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '                    mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));',
    '''                    mPreloadedObjects.insert(mSceneManager->getTemplate(
                        mesh, true, Resource::V321CompileClass::GenericModel));''',
)

# Supersede the preparation-only README wording after the original classifier's
# own fail-closed validation has already run.
replace_exact(
    "V3-LAB-README.txt",
    '''Generic SceneManager model preloads remain intentionally unclassified until a
category-propagation API can carry source identity without guessing after the
fact. Groundcover remains outside the ICO completion queue and is not routed
through ICO merely for scheduler symmetry.''',
    '''Generic SceneManager model preloads now propagate an explicit GenericModel class
through SceneManager::getTemplate and tag the exact loaded subgraph immediately
before the existing ICO add call. All other callers default to Unknown. Groundcover
remains outside the ICO completion queue and is not routed through ICO merely for
scheduler symmetry.''',
)

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

hpp = (ROOT / "components/resource/scenemanager.hpp").read_text(encoding="utf-8")
cpp = (ROOT / "components/resource/scenemanager.cpp").read_text(encoding="utf-8")
preloader = (ROOT / "apps/openmw/mwworld/cellpreloader.cpp").read_text(encoding="utf-8")
readme = (ROOT / "V3-LAB-README.txt").read_text(encoding="utf-8")
for marker in (
    'V321CompileClass compileClass = V321CompileClass::Unknown',
    '#include "v321compileclass.hpp"',
):
    if marker not in hpp:
        raise RuntimeError(f"V3.21 CP2 SceneManager header missing marker: {marker}")
for marker in (
    'V321CompileClass compileClass)',
    'markV321CompileClass(*loaded, compileClass);',
    'mIncrementalCompileOperation->add(loaded);',
):
    if marker not in cpp:
        raise RuntimeError(f"V3.21 CP2 SceneManager implementation missing marker: {marker}")
if preloader.count('Resource::V321CompileClass::GenericModel') != 1:
    raise RuntimeError("V3.21 CP2 GenericModel preload classification missing or duplicated")
if "All other callers default to Unknown" not in readme:
    raise RuntimeError("V3.21 CP2 README did not record GenericModel propagation")

print("V3.21 CP2 GenericModel source classification applied with original ICO path preserved")
