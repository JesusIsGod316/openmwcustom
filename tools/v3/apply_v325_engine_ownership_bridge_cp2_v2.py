from pathlib import Path

HERE = Path(__file__).resolve().parent
ORIGINAL = HERE / "apply_v325_engine_ownership_bridge_cp2.py"

if not ORIGINAL.is_file():
    raise RuntimeError("V3.25 CP2 v2 failure: original CP2 layer is missing")

source = ORIGINAL.read_text(encoding="utf-8")
start_marker = "old_loop = '''"
end_marker = '\nlauncher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"'
start = source.find(start_marker)
end = source.find(end_marker, start if start >= 0 else 0)
if start < 0 or end < 0 or end <= start:
    raise RuntimeError("V3.25 CP2 v2 failure: could not isolate original controller-loop patch block")

# V3.6 wraps the controller loop in a PhaseScope and adds per-controller counting.
# Patch by semantic function boundaries instead of pretending the historical loop
# is still textually pristine. Keep the V3.6 clone phase around both paths and keep
# controller() accounting on the deterministic main-thread publish path.
replacement = r'''region_start_marker = "        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;\n"
region_end_marker = "\n        mAnimSources.push_back(animsrc);"
region_start = func.find(region_start_marker)
region_end = func.find(region_end_marker, region_start if region_start >= 0 else 0)
if region_start < 0 or region_end < 0 or region_end <= region_start:
    raise RuntimeError(f"{animation_cpp}: V3.25 CP2 could not isolate profiled controller clone region")
old_region = func[region_start:region_end]
for required in (
    "Debug::V36ControllerTrace::PhaseScope v36ControllerClone",
    "Debug::V36ControllerTrace::Phase::ControllerClone",
    "osg::clone(it->second.get(), osg::CopyOp::SHALLOW_COPY)",
    "v36ControllerTrace.controller(it->second->className())",
):
    if old_region.count(required) != 1:
        raise RuntimeError(
            f"{animation_cpp}: V3.25 CP2 expected one profiled controller marker {required!r}, "
            f"found {old_region.count(required)}"
        )

new_region = r"""        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;
        {
            Debug::V36ControllerTrace::PhaseScope v36ControllerClone(
                v36ControllerTrace, Debug::V36ControllerTrace::Phase::ControllerClone);

            static const bool v325ParallelActorBinding = [] {
                const char* value = std::getenv("OPENMW_V325_PARALLEL_ACTOR_BINDING");
                return value && value[0] == '1' && value[1] == '\0';
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
                    const size_t blendMask = detectBlendMask(node, it->second->getName());

                    osg::ref_ptr<SceneUtil::KeyframeController> cloned
                        = osg::clone(it->second.get(), osg::CopyOp::SHALLOW_COPY);
                    cloned->setSource(mAnimationTimePtr[blendMask]);
                    animsrc->mControllerMap[blendMask].insert(std::make_pair(bonename, cloned));
                    v36ControllerTrace.controller(it->second->className());
                }
            }
            else
            {
                struct V325BindingInput
                {
                    std::string mBoneName;
                    size_t mBlendMask = 0;
                    const SceneUtil::KeyframeController* mController = nullptr;
                    std::shared_ptr<AnimationTime> mSource;
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
                    inputs.push_back(
                        V325BindingInput{ std::move(bonename), blendMask, controller, mAnimationTimePtr[blendMask] });
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
                            const auto* source
                                = static_cast<const NifOsg::KeyframeController*>(inputs[i].mController);
                            osg::ref_ptr<SceneUtil::KeyframeController> cloned
                                = new NifOsg::KeyframeController(*source, osg::CopyOp::SHALLOW_COPY);
                            cloned->setSource(inputs[i].mSource);
                            prepared[i] = std::move(cloned);
                        }
                    };
                    parallelPrepared = SceneUtil::FrameCriticalJobGroup::instance().parallelFor(
                        inputs.size(), reservedWorkers, cloneChunkSize, cloneRange);
                }

                if (!parallelPrepared)
                {
                    // Fail closed: no worker result has been published. Rebuild the
                    // complete source through the historical clone operation.
                    for (std::size_t i = 0; i < inputs.size(); ++i)
                    {
                        osg::ref_ptr<SceneUtil::KeyframeController> cloned
                            = osg::clone(inputs[i].mController, osg::CopyOp::SHALLOW_COPY);
                        cloned->setSource(inputs[i].mSource);
                        prepared[i] = std::move(cloned);
                    }
                }

                // Deterministic main-thread publication. Worker completion order never
                // affects source/controller priority or the live animation graph.
                for (std::size_t i = 0; i < inputs.size(); ++i)
                {
                    animsrc->mControllerMap[inputs[i].mBlendMask].insert(
                        std::make_pair(inputs[i].mBoneName, prepared[i]));
                    v36ControllerTrace.controller(inputs[i].mController->className());
                }
            }
        }
"""

func = func[:region_start] + new_region + func[region_end:]
text = text[:func_start] + func + text[func_end:]
path.write_text(text, encoding="utf-8", newline="\n")
print("V3.25 CP2 structurally patched profiled controller-clone preparation region")
'''

source = source[:start] + replacement + source[end:]

exec(
    compile(source, str(ORIGINAL), "exec"),
    {"__file__": str(ORIGINAL), "__name__": "__main__"},
)
