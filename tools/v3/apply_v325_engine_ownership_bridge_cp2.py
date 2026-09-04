import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.25 CP2 match(es), found {count}: {old[:120]!r}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.25 CP2 patched {rel} ({count} match(es))")


# CP2 / Mode151 keeps the CP1 batching win and moves only the controller-clone
# preparation kernel to a dedicated, queue-less frame-critical fork/join group.
# Live actor node traversal, node-map creation, blend-mask discovery, resource-cache
# lookup, animation-source publication, controller-map publication and final actor-wide
# source assignment stay on the main thread.
#
# KeyframeManager can also produce OsgAnimationController for non-KF animation formats.
# Those and any other controller subclasses fail closed to the exact historical serial
# osg::clone path. Worker cloning is enabled only when every valid controller in the
# source is a NifOsg::KeyframeController produced by the KF loader.

animation_cpp = "apps/openmw/mwrender/animation.cpp"

replace_exact(
    animation_cpp,
    "#include <algorithm>\n#include <limits>\n",
    "#include <algorithm>\n#include <cstdlib>\n#include <limits>\n#include <vector>\n",
)

replace_exact(
    animation_cpp,
    "#include <components/debug/v3deeptelemetry.hpp>\n",
    "#include <components/debug/v3deeptelemetry.hpp>\n\n"
    "#include <components/nifosg/controller.hpp>\n"
    "#include <components/sceneutil/framecriticaljobgroup.hpp>\n",
)

path = ROOT / animation_cpp
text = path.read_text(encoding="utf-8")
func_start_marker = "    std::shared_ptr<Animation::AnimSource> Animation::addSingleAnimSource(\n"
func_end_marker = "    void Animation::clearAnimSources()\n"
func_start = text.find(func_start_marker)
func_end = text.find(func_end_marker, func_start if func_start >= 0 else 0)
if func_start < 0 or func_end < 0 or func_end <= func_start:
    raise RuntimeError(f"{animation_cpp}: V3.25 CP2 could not isolate addSingleAnimSource")
func = text[func_start:func_end]

old_loop = '''        const NodeMap& nodeMap = getNodeMap();
        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;
        for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
             it != controllerMap.end(); ++it)
        {
            std::string bonename = Misc::StringUtils::lowerCase(it->first);
            NodeMap::const_iterator found = nodeMap.find(bonename);
            if (found == nodeMap.end())
            {
                Log(Debug::Warning) << "Warning: addAnimSource: can't find bone '" + bonename << "' in " << baseModel
                                    << " (referenced by " << kfname << ")";
                continue;
            }

            osg::Node* node = found->second;

            size_t blendMask = detectBlendMask(node, it->second->getName());

            // clone the controller, because each Animation needs its own ControllerSource
            osg::ref_ptr<SceneUtil::KeyframeController> cloned
                = osg::clone(it->second.get(), osg::CopyOp::SHALLOW_COPY);
            cloned->setSource(mAnimationTimePtr[blendMask]);

            animsrc->mControllerMap[blendMask].insert(std::make_pair(bonename, cloned));
        }
'''
if func.count(old_loop) != 1:
    raise RuntimeError(
        f"{animation_cpp}: V3.25 CP2 expected one historical controller loop in addSingleAnimSource, "
        f"found {func.count(old_loop)}"
    )

new_loop = '''        const NodeMap& nodeMap = getNodeMap();
        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;

        static const bool v325ParallelActorBinding = [] {
            const char* value = std::getenv("OPENMW_V325_PARALLEL_ACTOR_BINDING");
            return value && value[0] == '1' && value[1] == '\\0';
        }();

        if (!v325ParallelActorBinding)
        {
            for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
                 it != controllerMap.end(); ++it)
            {
                std::string bonename = Misc::StringUtils::lowerCase(it->first);
                NodeMap::const_iterator found = nodeMap.find(bonename);
                if (found == nodeMap.end())
                {
                    Log(Debug::Warning) << "Warning: addAnimSource: can't find bone '" + bonename << "' in " << baseModel
                                        << " (referenced by " << kfname << ")";
                    continue;
                }

                osg::Node* node = found->second;
                size_t blendMask = detectBlendMask(node, it->second->getName());

                osg::ref_ptr<SceneUtil::KeyframeController> cloned
                    = osg::clone(it->second.get(), osg::CopyOp::SHALLOW_COPY);
                cloned->setSource(mAnimationTimePtr[blendMask]);
                animsrc->mControllerMap[blendMask].insert(std::make_pair(bonename, cloned));
            }
        }
        else
        {
            struct V325BindingInput
            {
                std::string mBoneName;
                size_t mBlendMask = 0;
                const SceneUtil::KeyframeController* mController = nullptr;
            };

            std::vector<V325BindingInput> inputs;
            inputs.reserve(controllerMap.size());
            bool workerCloneSafe = true;
            for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
                 it != controllerMap.end(); ++it)
            {
                std::string bonename = Misc::StringUtils::lowerCase(it->first);
                NodeMap::const_iterator found = nodeMap.find(bonename);
                if (found == nodeMap.end())
                {
                    Log(Debug::Warning) << "Warning: addAnimSource: can't find bone '" + bonename << "' in " << baseModel
                                        << " (referenced by " << kfname << ")";
                    continue;
                }

                osg::Node* node = found->second;
                const size_t blendMask = detectBlendMask(node, it->second->getName());
                const SceneUtil::KeyframeController* controller = it->second.get();
                if (dynamic_cast<const NifOsg::KeyframeController*>(controller) == nullptr)
                    workerCloneSafe = false;
                inputs.push_back(V325BindingInput{ std::move(bonename), blendMask, controller });
            }

            std::vector<osg::ref_ptr<SceneUtil::KeyframeController>> prepared(inputs.size());
            bool parallelPrepared = false;
            constexpr std::size_t minParallelControllers = 16;
            constexpr std::size_t cloneChunkSize = 8;
            constexpr std::size_t reservedWorkers = 2;

            if (workerCloneSafe && inputs.size() >= minParallelControllers)
            {
                auto cloneRange = [&](std::size_t begin, std::size_t end) {
                    for (std::size_t i = begin; i < end; ++i)
                    {
                        const auto* source = static_cast<const NifOsg::KeyframeController*>(inputs[i].mController);
                        osg::ref_ptr<SceneUtil::KeyframeController> cloned
                            = new NifOsg::KeyframeController(*source, osg::CopyOp::SHALLOW_COPY);
                        cloned->setSource(mAnimationTimePtr[inputs[i].mBlendMask]);
                        prepared[i] = std::move(cloned);
                    }
                };
                parallelPrepared = SceneUtil::FrameCriticalJobGroup::instance().parallelFor(
                    inputs.size(), reservedWorkers, cloneChunkSize, cloneRange);
            }

            if (!parallelPrepared)
            {
                // Fail closed: reconstruct every unpublished controller through the
                // exact historical clone operation. No partial worker result is published.
                for (std::size_t i = 0; i < inputs.size(); ++i)
                {
                    osg::ref_ptr<SceneUtil::KeyframeController> cloned
                        = osg::clone(inputs[i].mController, osg::CopyOp::SHALLOW_COPY);
                    cloned->setSource(mAnimationTimePtr[inputs[i].mBlendMask]);
                    prepared[i] = std::move(cloned);
                }
            }

            // Deterministic main-thread publication. Worker completion order never
            // affects source/controller priority or the live animation graph.
            for (std::size_t i = 0; i < inputs.size(); ++i)
                animsrc->mControllerMap[inputs[i].mBlendMask].insert(
                    std::make_pair(inputs[i].mBoneName, prepared[i]));
        }
'''

func = func.replace(old_loop, new_loop, 1)
text = text[:func_start] + func + text[func_end:]
path.write_text(text, encoding="utf-8", newline="\n")
print("V3.25 CP2 patched parallel unpublished controller-clone preparation")

launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")

menu150 = next((line for line in launcher.splitlines() if "150 = V3.25" in line), None)
if not menu150:
    raise RuntimeError("V3.25 CP2 launcher lost Mode150 menu anchor")
launcher = launcher.replace(
    menu150 + "\n",
    menu150 + "\n" + "Write-Host '151 = V3.25 CP2 batched + parallel NIF controller-clone preparation'\n",
    1,
)

choice_line = next((line for line in launcher.splitlines() if "135-146, 149-150" in line), None)
if not choice_line:
    raise RuntimeError("V3.25 CP2 launcher choice prompt anchor drifted")
new_choice = choice_line.replace("135-146, 149-150", "135-146, 149-151", 1)
if ",'150'))" not in new_choice:
    raise RuntimeError("V3.25 CP2 launcher allowlist anchor drifted")
new_choice = new_choice.replace(",'150'))", ",'150','151'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)

batch_var = "$V325ActorSourceBatch = '0'"
if launcher.count(batch_var) != 1:
    raise RuntimeError("V3.25 CP2 launcher batching variable anchor drifted")
launcher = launcher.replace(batch_var, batch_var + "\n$V325ParallelActorBinding = '0'", 1)

line150 = next((line for line in launcher.splitlines() if line.lstrip().startswith("'150'")), None)
if not line150:
    raise RuntimeError("V3.25 CP2 launcher could not recover Mode150 body")
mode150_body = line150[line150.index("{") + 1:line150.rindex("}")].strip()
mode151 = f"        '151' {{ {mode150_body}; $V325ParallelActorBinding = '1' }}"
launcher = launcher.replace(line150 + "\n", line150 + "\n" + mode151 + "\n", 1)

manifest_anchor = '    "v325_actor_source_batch=$V325ActorSourceBatch",'
if launcher.count(manifest_anchor) != 1:
    raise RuntimeError("V3.25 CP2 launcher manifest anchor drifted")
launcher = launcher.replace(
    manifest_anchor,
    manifest_anchor + '\n    "v325_parallel_actor_binding=$V325ParallelActorBinding",',
    1,
)

env_anchor = "    $env:OPENMW_V325_ACTOR_SOURCE_BATCH = $V325ActorSourceBatch"
if launcher.count(env_anchor) != 1:
    raise RuntimeError("V3.25 CP2 launcher actor-source environment anchor drifted")
launcher = launcher.replace(
    env_anchor,
    env_anchor
    + "\n    $env:OPENMW_V325_PARALLEL_ACTOR_BINDING = $V325ParallelActorBinding\n"
      "    if ($V325ParallelActorBinding -eq '1') {\n"
      "        $env:OPENMW_V325_JOBGROUP_STATS_FILE = Join-Path $ProfileDir 'v325-jobgroup-summary.csv'\n"
      "    }",
    1,
)

cleanup_anchor = "    Remove-Item Env:OPENMW_V325_ACTOR_SOURCE_BATCH -ErrorAction SilentlyContinue"
if launcher.count(cleanup_anchor) != 1:
    raise RuntimeError("V3.25 CP2 launcher cleanup anchor drifted")
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V325_JOBGROUP_STATS_FILE -ErrorAction SilentlyContinue\n"
    "    Remove-Item Env:OPENMW_V325_PARALLEL_ACTOR_BINDING -ErrorAction SilentlyContinue\n"
    + cleanup_anchor,
    1,
)

launcher_path.write_text(launcher, encoding="utf-8", newline="\n")
print("V3.25 CP2 patched Mode151 launcher and job-group summary output")

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as f:
    f.write('''

V3.25 ENGINE OWNERSHIP BRIDGE - CP2 / MODE151
===============================================
Mode151 inherits Mode150 actor-source batching, then enables only the safe NIF
keyframe-controller clone-preparation kernel on the V3.25 FrameCriticalJobGroup.
The group is queue-less, lazily creates up to two reserved workers, and makes the
caller participate through a shared coarse range cursor (16-controller threshold,
8-controller chunks). Worker results remain unpublished until the main thread
commits controller maps in deterministic source order.

The main thread still owns KeyframeManager/VFS resolution, NodeMap construction,
live node association, detectBlendMask traversal, missing-bone logging, AnimSource
publication, accumulation-root/blend-rule work and final AssignControllerSourcesVisitor.
Any OsgAnimationController or other non-NIF controller type forces that entire
source back through the historical serial osg::clone path. A worker exception also
discards partial unpublished results and retries the full clone set serially.

Mode151 writes one bounded v325-jobgroup-summary.csv at shutdown. It records group,
item, caller/worker chunk, fallback/failure and peak-worker totals without per-job
runtime file I/O. Deep telemetry remains OFF for performance A/B.
''')

patch_text = subprocess.run(
    ["git", "diff", "--binary"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source.patch").write_text(patch_text, encoding="utf-8", newline="\n")
stat_text = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, capture_output=True, text=True
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat_text, encoding="utf-8", newline="\n")

checks = {
    animation_cpp: [
        "OPENMW_V325_PARALLEL_ACTOR_BINDING",
        "FrameCriticalJobGroup::instance().parallelFor",
        "minParallelControllers = 16",
        "reservedWorkers = 2",
        "dynamic_cast<const NifOsg::KeyframeController*>",
        "Deterministic main-thread publication",
    ],
    "tools/v3/launchers/V3_Lab.ps1": [
        "151 = V3.25 CP2 batched + parallel NIF controller-clone preparation",
        "OPENMW_V325_PARALLEL_ACTOR_BINDING",
        "v325-jobgroup-summary.csv",
    ],
    "components/sceneutil/framecriticaljobgroup.hpp": [
        "class FrameCriticalJobGroup",
        "std::try_to_lock",
        "fallbackSerialGroups",
    ],
}
for rel, markers in checks.items():
    data = (ROOT / rel).read_text(encoding="utf-8")
    for marker in markers:
        if marker not in data:
            raise RuntimeError(f"V3.25 CP2 generated-source identity missing {marker!r} in {rel}")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
print("V3.25 CP2 Mode151 parallel actor-binding layer passed")
