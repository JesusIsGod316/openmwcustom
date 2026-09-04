from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

animation_hpp = ROOT / "apps/openmw/mwrender/animation.hpp"
animation_cpp = ROOT / "apps/openmw/mwrender/animation.cpp"


def replace_once(path: Path, old: str, new: str, label: str):
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"V3.25 CP2 actor-batch {label}: expected one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"V3.25 CP2 actor-batch patched {label}")


# The first Mode151 implementation proved the safe clone boundary and the job-group
# primitive, but fork/joined once per successful source. Refine the same mechanism to
# aggregate all eligible KF controller clones across the explicit NPC source batch.
# This makes the scheduling unit actor-sized: one fork/join at the outer batch close.

replace_once(
    animation_hpp,
    "namespace SceneUtil\n{\n    class KeyframeHolder;\n",
    "namespace NifOsg\n{\n    class KeyframeController;\n}\n\nnamespace SceneUtil\n{\n    class KeyframeHolder;\n",
    "NifOsg forward declaration",
)

replace_once(
    animation_hpp,
    "        void beginAnimSourceBatch();\n        void endAnimSourceBatch();\n",
    "        void beginAnimSourceBatch();\n        void endAnimSourceBatch();\n        void flushV325PendingControllerClones();\n",
    "batch flush declaration",
)

replace_once(
    animation_hpp,
    "        unsigned int mAnimSourceBatchDepth = 0;\n"
    "        bool mAnimSourceBatchNeedsControllerAssignment = false;\n",
    "        unsigned int mAnimSourceBatchDepth = 0;\n"
    "        bool mAnimSourceBatchNeedsControllerAssignment = false;\n\n"
    "        struct V325PendingControllerClone\n"
    "        {\n"
    "            std::shared_ptr<AnimSource> mAnimSource;\n"
    "            std::string mBoneName;\n"
    "            size_t mBlendMask = 0;\n"
    "            const NifOsg::KeyframeController* mController = nullptr;\n"
    "            std::shared_ptr<AnimationTime> mSource;\n"
    "        };\n"
    "        std::vector<V325PendingControllerClone> mV325PendingControllerClones;\n",
    "pending actor clone storage",
)

text = animation_cpp.read_text(encoding="utf-8")

begin_old = '''    void Animation::beginAnimSourceBatch()
    {
        if (mAnimSourceBatchDepth++ == 0)
            mAnimSourceBatchNeedsControllerAssignment = false;
    }

    void Animation::endAnimSourceBatch()
    {
        if (mAnimSourceBatchDepth == 0)
            return;

        --mAnimSourceBatchDepth;
        if (mAnimSourceBatchDepth != 0 || !mAnimSourceBatchNeedsControllerAssignment)
            return;

        mAnimSourceBatchNeedsControllerAssignment = false;
        if (mObjectRoot)
        {
            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);
            mObjectRoot->accept(assignVisitor);
        }
    }

'''
if text.count(begin_old) != 1:
    raise RuntimeError(
        f"V3.25 CP2 actor-batch batch-method block expected once, found {text.count(begin_old)}"
    )

begin_new = '''    void Animation::flushV325PendingControllerClones()
    {
        if (mV325PendingControllerClones.empty())
            return;

        // Move the complete actor-local batch out before execution so no worker can
        // observe a vector that is subsequently appended to by source discovery.
        std::vector<V325PendingControllerClone> pending = std::move(mV325PendingControllerClones);
        mV325PendingControllerClones.clear();

        std::vector<osg::ref_ptr<SceneUtil::KeyframeController>> prepared(pending.size());
        constexpr std::size_t minParallelControllers = 16;
        constexpr std::size_t cloneChunkSize = 8;
        constexpr std::size_t reservedWorkers = 2;

        bool parallelPrepared = false;
        if (pending.size() >= minParallelControllers)
        {
            auto cloneRange = [&](std::size_t begin, std::size_t end) {
                for (std::size_t i = begin; i < end; ++i)
                {
                    osg::ref_ptr<SceneUtil::KeyframeController> cloned
                        = new NifOsg::KeyframeController(*pending[i].mController, osg::CopyOp::SHALLOW_COPY);
                    cloned->setSource(pending[i].mSource);
                    prepared[i] = std::move(cloned);
                }
            };
            parallelPrepared = SceneUtil::FrameCriticalJobGroup::instance().parallelFor(
                pending.size(), reservedWorkers, cloneChunkSize, cloneRange);
        }

        if (!parallelPrepared)
        {
            // Nothing from this actor batch has been published yet. Reconstruct the
            // complete pending set with the historical clone operation on main.
            for (std::size_t i = 0; i < pending.size(); ++i)
            {
                osg::ref_ptr<SceneUtil::KeyframeController> cloned
                    = osg::clone(pending[i].mController, osg::CopyOp::SHALLOW_COPY);
                cloned->setSource(pending[i].mSource);
                prepared[i] = std::move(cloned);
            }
        }

        // Main-thread deterministic publication in exact discovery/source order.
        for (std::size_t i = 0; i < pending.size(); ++i)
            pending[i].mAnimSource->mControllerMap[pending[i].mBlendMask].insert(
                std::make_pair(pending[i].mBoneName, prepared[i]));
    }

    void Animation::beginAnimSourceBatch()
    {
        if (mAnimSourceBatchDepth++ == 0)
        {
            mAnimSourceBatchNeedsControllerAssignment = false;
            mV325PendingControllerClones.clear();
        }
    }

    void Animation::endAnimSourceBatch()
    {
        if (mAnimSourceBatchDepth == 0)
            return;

        --mAnimSourceBatchDepth;
        if (mAnimSourceBatchDepth != 0)
            return;

        // Publish all deferred controller clones before the final actor-wide source
        // assignment visitor. This preserves the historical fully-built state at batch exit.
        flushV325PendingControllerClones();

        if (!mAnimSourceBatchNeedsControllerAssignment)
            return;

        mAnimSourceBatchNeedsControllerAssignment = false;
        if (mObjectRoot)
        {
            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);
            mObjectRoot->accept(assignVisitor);
        }
    }

'''
text = text.replace(begin_old, begin_new, 1)

func_start_marker = "    std::shared_ptr<Animation::AnimSource> Animation::addSingleAnimSource(\n"
func_end_marker = "    void Animation::clearAnimSources()\n"
func_start = text.find(func_start_marker)
func_end = text.find(func_end_marker, func_start if func_start >= 0 else 0)
if func_start < 0 or func_end < 0 or func_end <= func_start:
    raise RuntimeError("V3.25 CP2 actor-batch could not isolate addSingleAnimSource")
func = text[func_start:func_end]

parallel_anchor = "            else\n            {\n                struct V325BindingInput\n"
parallel_start = func.find(parallel_anchor)
publish_marker = "\n        mAnimSources.push_back(animsrc);"
parallel_end = func.find(publish_marker, parallel_start if parallel_start >= 0 else 0)
if parallel_start < 0 or parallel_end < 0:
    raise RuntimeError("V3.25 CP2 actor-batch could not isolate Mode151 per-source branch")
old_parallel = func[parallel_start:parallel_end]
for required in (
    "FrameCriticalJobGroup::instance().parallelFor",
    "dynamic_cast<const NifOsg::KeyframeController*>",
    "Deterministic main-thread publication",
):
    if required not in old_parallel:
        raise RuntimeError(f"V3.25 CP2 actor-batch missing predecessor marker {required!r}")

new_parallel = '''            else
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

                if (workerCloneSafe && mAnimSourceBatchDepth != 0)
                {
                    // Actor-sized scheduling: gather all eligible source controllers and
                    // fork/join once when the outer NPC source batch closes.
                    for (V325BindingInput& input : inputs)
                    {
                        mV325PendingControllerClones.push_back(V325PendingControllerClone{
                            animsrc,
                            std::move(input.mBoneName),
                            input.mBlendMask,
                            static_cast<const NifOsg::KeyframeController*>(input.mController),
                            std::move(input.mSource),
                        });
                        v36ControllerTrace.controller(input.mController->className());
                    }
                }
                else
                {
                    // Generic callers and non-NIF animation formats remain on the exact
                    // historical serial clone/publish path.
                    for (const V325BindingInput& input : inputs)
                    {
                        osg::ref_ptr<SceneUtil::KeyframeController> cloned
                            = osg::clone(input.mController, osg::CopyOp::SHALLOW_COPY);
                        cloned->setSource(input.mSource);
                        animsrc->mControllerMap[input.mBlendMask].insert(
                            std::make_pair(input.mBoneName, cloned));
                        v36ControllerTrace.controller(input.mController->className());
                    }
                }
            }
'''
func = func[:parallel_start] + new_parallel + func[parallel_end:]
text = text[:func_start] + func + text[func_end:]

# A defensive clear prevents any stale unpublished clone record from surviving a
# source reset even if a future caller violates the normal begin/end batch protocol.
clear_marker = "    void Animation::clearAnimSources()\n    {\n        mStates.clear();\n"
if text.count(clear_marker) != 1:
    raise RuntimeError("V3.25 CP2 actor-batch clearAnimSources anchor drifted")
text = text.replace(
    clear_marker,
    "    void Animation::clearAnimSources()\n    {\n"
    "        mV325PendingControllerClones.clear();\n"
    "        mAnimSourceBatchDepth = 0;\n"
    "        mAnimSourceBatchNeedsControllerAssignment = false;\n"
    "        mStates.clear();\n",
    1,
)

animation_cpp.write_text(text, encoding="utf-8", newline="\n")

# Structural fail-closed checks.
hpp = animation_hpp.read_text(encoding="utf-8")
cpp = animation_cpp.read_text(encoding="utf-8")
for marker in (
    "struct V325PendingControllerClone",
    "mV325PendingControllerClones",
    "flushV325PendingControllerClones",
):
    if marker not in hpp:
        raise RuntimeError(f"V3.25 CP2 actor-batch missing header marker {marker}")

flush_start = cpp.index("    void Animation::flushV325PendingControllerClones()")
flush_end = cpp.index("    void Animation::beginAnimSourceBatch()", flush_start)
flush = cpp[flush_start:flush_end]
for marker in (
    "FrameCriticalJobGroup::instance().parallelFor",
    "pending.size() >= minParallelControllers",
    "reservedWorkers = 2",
    "cloneChunkSize = 8",
    "Main-thread deterministic publication",
):
    if marker not in flush:
        raise RuntimeError(f"V3.25 CP2 actor-batch flush missing {marker}")
for forbidden in ("nodeMap", "detectBlendMask", "mObjectRoot", "getKeyframeManager"):
    clone_start = flush.index("auto cloneRange =")
    clone_end = flush.index("parallelPrepared =", clone_start)
    if forbidden in flush[clone_start:clone_end]:
        raise RuntimeError(f"V3.25 CP2 actor-batch worker clone illegally touches {forbidden}")

add_start = cpp.index(func_start_marker)
add_end = cpp.index(func_end_marker, add_start)
add = cpp[add_start:add_end]
if "FrameCriticalJobGroup::instance().parallelFor" in add:
    raise RuntimeError("V3.25 CP2 actor-batch still fork/joins per source")
if "mV325PendingControllerClones.push_back" not in add:
    raise RuntimeError("V3.25 CP2 actor-batch addSingleAnimSource does not gather actor work")

print("V3.25 CP2 actor-batched Mode151 refinement passed")
