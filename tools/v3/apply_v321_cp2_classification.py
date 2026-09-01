import os
from pathlib import Path
import subprocess

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 CP2 classification match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 CP2 classification patched {rel} ({count} match(es))")


def require_exact(rel: str, marker: str, expected: int = 1) -> None:
    text = (ROOT / rel).read_text(encoding="utf-8")
    count = text.count(marker)
    if count != expected:
        raise RuntimeError(
            f"{rel}: expected {expected} V3.21 CP2 semantic anchor(s) {marker!r}, found {count}"
        )


# CP2 preparation deliberately does not change queue service policy. It only
# attaches an OpenMW-owned class marker to the subgraph already held by OSG's
# CompileSet. OSG 3.6.5 overwrites CompileSet::_markerObject in ico->add(), so
# that field must remain reserved for StateToCompile's own visited-object marker.
helper_path = ROOT / "components/resource/v321compileclass.hpp"
if helper_path.exists():
    raise RuntimeError("components/resource/v321compileclass.hpp unexpectedly already exists")
helper_path.write_text(
    r'''#pragma once

#include <osg/Object>
#include <osg/UserDataContainer>
#include <osg/ref_ptr>

namespace Resource
{
    enum class V321CompileClass : unsigned char
    {
        Unknown = 0,
        ObjectPaging,
        Terrain,
        GenericModel,
    };

    inline constexpr const char* v321CompileClassMarkerName(V321CompileClass value)
    {
        switch (value)
        {
            case V321CompileClass::ObjectPaging:
                return "OpenMW.V321CompileClass.ObjectPaging";
            case V321CompileClass::Terrain:
                return "OpenMW.V321CompileClass.Terrain";
            case V321CompileClass::GenericModel:
                return "OpenMW.V321CompileClass.GenericModel";
            case V321CompileClass::Unknown:
            default:
                return nullptr;
        }
    }

    inline void markV321CompileClass(osg::Object& object, V321CompileClass value)
    {
        const char* const markerName = v321CompileClassMarkerName(value);
        if (!markerName)
            return;

        osg::UserDataContainer* const udc = object.getOrCreateUserDataContainer();
        if (udc->getUserObject(markerName))
            return;

        osg::ref_ptr<osg::DummyObject> marker = new osg::DummyObject;
        marker->setName(markerName);
        udc->addUserObject(marker.get());
    }

    inline V321CompileClass getV321CompileClass(const osg::Object* object)
    {
        if (!object)
            return V321CompileClass::Unknown;
        const osg::UserDataContainer* const udc = object->getUserDataContainer();
        if (!udc)
            return V321CompileClass::Unknown;
        if (udc->getUserObject(v321CompileClassMarkerName(V321CompileClass::ObjectPaging)))
            return V321CompileClass::ObjectPaging;
        if (udc->getUserObject(v321CompileClassMarkerName(V321CompileClass::Terrain)))
            return V321CompileClass::Terrain;
        if (udc->getUserObject(v321CompileClassMarkerName(V321CompileClass::GenericModel)))
            return V321CompileClass::GenericModel;
        return V321CompileClass::Unknown;
    }
}
''',
    encoding="utf-8",
    newline="\n",
)
print("V3.21 CP2 generated components/resource/v321compileclass.hpp")

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    "#include <components/resource/scenemanager.hpp>\n",
    "#include <components/resource/scenemanager.hpp>\n#include <components/resource/v321compileclass.hpp>\n",
)
replace_exact(
    "components/terrain/chunkmanager.cpp",
    "#include <components/resource/scenemanager.hpp>\n",
    "#include <components/resource/scenemanager.hpp>\n#include <components/resource/v321compileclass.hpp>\n",
)

# Earlier generated V3 layers may legitimately change statements adjacent to
# these submissions. Validate the complete semantic path with independent exact
# anchors, but mutate only the stable unique submission statement. This remains
# fail-closed without coupling CP2 to unrelated formatting/adjacent changes.
object_rel = "apps/openmw/mwrender/objectpaging.cpp"
object_compile_line = "            auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);"
require_exact(
    object_rel,
    "        osgUtil::IncrementalCompileOperation* const ico = mSceneManager->getIncrementalCompileOperation();",
)
require_exact(object_rel, object_compile_line)
require_exact(object_rel, "            compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);")
require_exact(object_rel, "            ico->add(compileSet, false);")
replace_exact(
    object_rel,
    object_compile_line,
    '''            // CP2 classification only: preserve the existing CompileSet/buildCompileMap/add path.
            // The class marker lives on the subgraph because ICO owns CompileSet::_markerObject.
            Resource::markV321CompileClass(*group, Resource::V321CompileClass::ObjectPaging);
            auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);''',
)

terrain_rel = "components/terrain/chunkmanager.cpp"
terrain_guard = "        if (!templateGeometry && compile && mSceneManager->getIncrementalCompileOperation())"
terrain_submit_line = "            mSceneManager->getIncrementalCompileOperation()->add(geometry);"
require_exact(terrain_rel, terrain_guard)
require_exact(terrain_rel, terrain_submit_line)
replace_exact(
    terrain_rel,
    terrain_submit_line,
    '''            // CP2 classification only: preserve SceneManager/ICO submission semantics.
            Resource::markV321CompileClass(*geometry, Resource::V321CompileClass::Terrain);
            mSceneManager->getIncrementalCompileOperation()->add(geometry);''',
)

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(
        r'''

V3.21 CP2 preparation — compile-source classification
=====================================================

This branch adds classification metadata only. Object-paging and terrain ICO
submissions attach OpenMW-owned user-object markers to the CompileSet subgraph.
It does not change WorkQueue cadence, producer/preload cadence, compile budgets,
completed-set admission budgets, FIFO/max-age escape, or MODE 125-127 behavior.
No fairness scheduler is enabled yet.

Generic SceneManager model preloads remain intentionally unclassified until a
category-propagation API can carry source identity without guessing after the
fact. Groundcover remains outside the ICO completion queue and is not routed
through ICO merely for scheduler symmetry.
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

helper_text = helper_path.read_text(encoding="utf-8")
object_text = (ROOT / object_rel).read_text(encoding="utf-8")
terrain_text = (ROOT / terrain_rel).read_text(encoding="utf-8")
readme_text = readme_path.read_text(encoding="utf-8")
patch_text = patch.decode("utf-8", errors="replace")

for marker in (
    "OpenMW.V321CompileClass.ObjectPaging",
    "OpenMW.V321CompileClass.Terrain",
    "OpenMW.V321CompileClass.GenericModel",
    "getV321CompileClass",
):
    if marker not in helper_text:
        raise RuntimeError(f"V3.21 CP2 helper missing marker: {marker}")
if object_text.count("Resource::markV321CompileClass(*group, Resource::V321CompileClass::ObjectPaging)") != 1:
    raise RuntimeError("V3.21 CP2 object-paging classification marker missing or duplicated")
if object_text.count("auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);") != 1:
    raise RuntimeError("V3.21 CP2 object-paging CompileSet path changed unexpectedly")
if object_text.count("compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);") != 1:
    raise RuntimeError("V3.21 CP2 object-paging buildCompileMap path changed unexpectedly")
if object_text.count("ico->add(compileSet, false);") != 1:
    raise RuntimeError("V3.21 CP2 object-paging ICO add path changed unexpectedly")
if terrain_text.count("Resource::markV321CompileClass(*geometry, Resource::V321CompileClass::Terrain)") != 1:
    raise RuntimeError("V3.21 CP2 terrain classification marker missing or duplicated")
if terrain_text.count("mSceneManager->getIncrementalCompileOperation()->add(geometry);") != 1:
    raise RuntimeError("V3.21 CP2 terrain ICO submission path changed unexpectedly")
for marker in (
    "classification metadata only",
    "does not change WorkQueue cadence",
    "No fairness scheduler is enabled yet",
    "Groundcover remains outside the ICO completion queue",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.21 CP2 README missing marker: {marker}")
if "v321compileclass.hpp" not in patch_text.lower():
    raise RuntimeError("V3.21 CP2 generated patch does not contain classification helper")

print("V3.21 CP2 object-paging + terrain source classification prep applied; scheduler remains disabled")
