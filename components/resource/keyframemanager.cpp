#include <components/debug/hitchtrace.hpp>
#include "keyframemanager.hpp"

#include <array>

#include <components/vfs/manager.hpp>

#include <osgAnimation/Animation>
#include <osgAnimation/BasicAnimationManager>
#include <osgAnimation/Channel>

#include <components/debug/debuglog.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/conversion.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/sceneutil/keyframe.hpp>
#include <components/sceneutil/osgacontroller.hpp>
#include <components/vfs/pathutil.hpp>

#include "animation.hpp"
#include "objectcache.hpp"
#include "animationdependencies.hpp"
#include "preloadestimate.hpp"
#include <optional>
#include "scenemanager.hpp"

namespace Resource
{
    namespace
    {
        std::string parseTextKey(const std::string& line)
        {
            const std::size_t spacePos = line.find_last_of(' ');
            if (spacePos != std::string::npos)
                return line.substr(0, spacePos);
            return {};
        }

        double parseTimeSignature(std::string_view line)
        {
            const std::size_t spacePos = line.find_last_of(' ');
            double time = 0.0;
            if (spacePos != std::string_view::npos && spacePos + 1 < line.size())
                time = Misc::StringUtils::toNumeric<double>(line.substr(spacePos + 1), time);
            return time;
        }
    }

    RetrieveAnimationsVisitor::RetrieveAnimationsVisitor(SceneUtil::KeyframeHolder& target,
        osg::ref_ptr<osgAnimation::BasicAnimationManager> animationManager, VFS::Path::NormalizedView path,
        const VFS::Manager& vfs)
        : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        , mTarget(target)
        , mAnimationManager(std::move(animationManager))
        , mPath(path)
        , mVFS(&vfs)
    {
        constexpr VFS::Path::ExtensionView txt("txt");
        mPath.changeExtension(txt);
    }

    bool RetrieveAnimationsVisitor::belongsToLeftUpperExtremity(const std::string& name)
    {
        static const std::array boneNames = { "bip01 l clavicle", "left clavicle", "bip01 l upperarm", "left upper arm",
            "bip01 l forearm", "bip01 l hand", "left hand", "left wrist", "shield bone", "bip01 l pinky1",
            "bip01 l pinky2", "bip01 l pinky3", "bip01 l ring1", "bip01 l ring2", "bip01 l ring3", "bip01 l middle1",
            "bip01 l middle2", "bip01 l middle3", "bip01 l pointer1", "bip01 l pointer2", "bip01 l pointer3",
            "bip01 l thumb1", "bip01 l thumb2", "bip01 l thumb3", "left forearm" };

        if (std::find(boneNames.begin(), boneNames.end(), name) != boneNames.end())
            return true;

        return false;
    }

    bool RetrieveAnimationsVisitor::belongsToRightUpperExtremity(const std::string& name)
    {
        static const std::array boneNames = { "bip01 r clavicle", "right clavicle", "bip01 r upperarm",
            "right upper arm", "bip01 r forearm", "bip01 r hand", "right hand", "right wrist", "bip01 r thumb1",
            "bip01 r thumb2", "bip01 r thumb3", "weapon bone", "bip01 r pinky1", "bip01 r pinky2", "bip01 r pinky3",
            "bip01 r ring1", "bip01 r ring2", "bip01 r ring3", "bip01 r middle1", "bip01 r middle2", "bip01 r middle3",
            "bip01 r pointer1", "bip01 r pointer2", "bip01 r pointer3", "right forearm" };

        if (std::find(boneNames.begin(), boneNames.end(), name) != boneNames.end())
            return true;

        return false;
    }

    bool RetrieveAnimationsVisitor::belongsToTorso(const std::string& name)
    {
        static const std::array boneNames
            = { "bip01 spine1", "bip01 spine2", "bip01 neck", "bip01 head", "head", "neck", "chest", "groin" };

        if (std::find(boneNames.begin(), boneNames.end(), name) != boneNames.end())
            return true;

        return false;
    }

    void RetrieveAnimationsVisitor::addKeyframeController(const std::string& name, const osg::Node& node)
    {
        osg::ref_ptr<SceneUtil::OsgAnimationController> callback = new SceneUtil::OsgAnimationController();

        callback->setName(name);

        std::vector<SceneUtil::EmulatedAnimation> emulatedAnimations;

        for (const auto& animation : mAnimationManager->getAnimationList())
        {
            if (animation)
            {
                //"Default" is osg dae plugin's default naming scheme for unnamed animations
                if (animation->getName() == "Default")
                {
                    animation->setName(std::string("idle"));
                }

                osg::ref_ptr<Resource::Animation> mergedAnimationTrack = new Resource::Animation;
                const std::string animationName = animation->getName();
                mergedAnimationTrack->setName(animationName);

                const osgAnimation::ChannelList& channels = animation->getChannels();
                for (const auto& channel : channels)
                {
                    if (name == "Bip01 R Clavicle")
                    {
                        if (!belongsToRightUpperExtremity(channel->getTargetName()))
                            continue;
                    }
                    else if (name == "Bip01 L Clavicle")
                    {
                        if (!belongsToLeftUpperExtremity(channel->getTargetName()))
                            continue;
                    }
                    else if (name == "Bip01 Spine1")
                    {
                        if (!belongsToTorso(channel->getTargetName()))
                            continue;
                    }
                    else if (belongsToRightUpperExtremity(channel->getTargetName())
                        || belongsToLeftUpperExtremity(channel->getTargetName())
                        || belongsToTorso(channel->getTargetName()))
                        continue;

                    mergedAnimationTrack->addChannel(channel.get()->clone());
                }

                callback->addMergedAnimationTrack(std::move(mergedAnimationTrack));

                float startTime = static_cast<float>(animation->getStartTime());
                float stopTime = static_cast<float>(startTime + animation->getDuration());

                SceneUtil::EmulatedAnimation emulatedAnimation;
                emulatedAnimation.mStartTime = startTime;
                emulatedAnimation.mStopTime = stopTime;
                emulatedAnimation.mName = animationName;
                emulatedAnimations.emplace_back(emulatedAnimation);
            }
        }

        // mTextKeys is a nif-thing, used by OpenMW's animation system
        // Format is likely "AnimationName: [Keyword_optional] [Start OR Stop]"
        // AnimationNames are keywords like idle2, idle3... AiPackages and various mechanics control which
        // animations are played Keywords can be stuff like Loop, Equip, Unequip, Block, InventoryHandtoHand,
        // InventoryWeaponOneHand, PickProbe, Slash, Thrust, Chop... even "Slash Small Follow" osgAnimation formats
        // should have a .txt file with the same name, each line holding a textkey and whitespace separated time
        // value e.g. idle: start 0.0333
        if (const Files::IStreamPtr textKeysFile = mVFS->find(mPath))
        {
            try
            {
                std::string line;
                while (getline(*textKeysFile, line))
                    mTarget.mTextKeys.emplace(static_cast<float>(parseTimeSignature(line)), parseTextKey(line));
            }
            catch (const std::exception& e)
            {
                Log(Debug::Warning) << "Failed to read text key file \"" << mPath << "\": " << e.what();
            }
        }
        else
        {
            Log(Debug::Warning) << "Text key file is not found: " << mPath;
        }

        callback->setEmulatedAnimations(emulatedAnimations);
        mTarget.mKeyframeControllers.emplace(name, callback);
    }

    void RetrieveAnimationsVisitor::apply(osg::Node& node)
    {
        if (node.libraryName() == std::string_view("osgAnimation") && node.className() == std::string_view("Bone")
            && Misc::StringUtils::lowerCase(node.getName()) == std::string_view("bip01"))
        {
            addKeyframeController("bip01", node); /* Character root */
            addKeyframeController("Bip01 Spine1", node); /* Torso */
            addKeyframeController("Bip01 L Clavicle", node); /* Left arm */
            addKeyframeController("Bip01 R Clavicle", node); /* Right arm */
        }

        traverse(node);
    }

    KeyframeManager::KeyframeManager(const VFS::Manager* vfs, SceneManager* sceneManager, double expiryDelay,
        const ToUTF8::StatelessUtf8Encoder* encoder)
        : ResourceManager(vfs, expiryDelay)
        , mSceneManager(sceneManager)
        , mEncoder(encoder)
    {
    }

    osg::ref_ptr<const SceneUtil::KeyframeHolder> KeyframeManager::get(
        VFS::Path::NormalizedView name, std::uint64_t maximumPreloadEstimate)
    {
        Debug::HitchTrace::Scope hitchKeyframes("resource.keyframe_lookup", {}, name.value());
        Debug::ResourceLifecycle::RequestScope lifecycle(Debug::ResourceLifecycle::Kind::Keyframe,
            name.value(), mVFS, SpeculativeScope::active(), mCache.get());
        const auto sourceGeneration = animationDependencyLifetimeEnabled() ? mVFS->getIndexGeneration() : 0;
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(name);

        if (obj != nullptr)
            return osg::ref_ptr<const SceneUtil::KeyframeHolder>(static_cast<SceneUtil::KeyframeHolder*>(obj.get()));

        // Only the candidate's bounded optional worker participates. Demand
        // loading retains the original synchronous path and never joins it.
        const bool boundedPreload = animationDependencyPreloadEnabled() && SpeculativeScope::active()
            && name.extension() == VFS::Path::ExtensionView("kf");
        auto claim = boundedPreload
            ? SpeculativeScope::claim(std::to_string(mVFS->getIndexGeneration()) + "|keyframe|" + std::string(name.value()))
            : SpeculativeBudget::Claim{};
        std::optional<SpeculativeScope::Stage> memoryStage;
        std::uint64_t ownerEstimate = 0;
        osg::ref_ptr<SceneUtil::KeyframeHolder> loaded(new SceneUtil::KeyframeHolder);
        constexpr VFS::Path::ExtensionView kf("kf");
        if (name.extension() == kf)
        {
            auto file = std::make_shared<Nif::NIFFile>(name);
            Nif::Reader reader(*file, mEncoder);
            if (boundedPreload)
            {
                auto stream = mVFS->get(name);
                const auto estimate = estimateStream(*stream, false);
                ownerEstimate = estimate.retained;
                if (estimate.retained > maximumPreloadEstimate) throw SpeculativeDeferred{};
                memoryStage.emplace(estimate.peak, estimate.retained);
                Debug::HitchTrace::Scope detail("resource.keyframe_parse");
                reader.parse(std::move(stream));
            }
            else
            {
                auto stream = mVFS->get(name);
                if (animationWarmLeaseEnabled())
                {
                    try
                    {
                        // Stamp from the ACTUAL parsed source, never reopen a
                        // later filesystem revision on a warm hit. Required
                        // loads are not admitted/denied by optional budgeting.
                        ownerEstimate = estimateStream(*stream, false).retained;
                    }
                    catch (const SpeculativeDeferred&)
                    {
                        // An unsupported seek is only an unknown ownership
                        // estimate. Reopen the original demand stream so its
                        // parser does not inherit a failed metadata seek.
                        stream = mVFS->get(name);
                    }
                }
                Debug::HitchTrace::Scope detail("resource.keyframe_parse");
                reader.parse(std::move(stream));
            }

            {
                Debug::HitchTrace::Scope detail("resource.keyframe_build", Debug::HitchTrace::CpuTiming::Scoped);
                NifOsg::Loader::loadKf(*file, *loaded.get());
            }

        }
        else
        {
            osg::ref_ptr<osg::Node> scene = const_cast<osg::Node*>(mSceneManager->getTemplate(name).get());
            osg::ref_ptr<osgAnimation::BasicAnimationManager> bam
                = dynamic_cast<osgAnimation::BasicAnimationManager*>(scene->getUpdateCallback());
            if (bam)
            {
                Resource::RetrieveAnimationsVisitor rav(*loaded.get(), std::move(bam), name, *mVFS);
                scene->accept(rav);
            }
        }
        mCache->addEntryToObjectCache(name.value(), loaded, 0.0,
            sourceGeneration, ownerEstimate);
        return loaded;
    }

    ObjectCacheLease KeyframeManager::getReadyLease(
        VFS::Path::NormalizedView name, std::uint64_t maximumPreloadEstimate)
    {
        const auto generation = mVFS->getIndexGeneration();
        const auto value = get(name, maximumPreloadEstimate);
        auto lease = mCache->getLeaseFromObjectCache(name, value.get(), generation);
        if (animationWarmLeaseEnabled())
        {
            Debug::HitchTrace::Scope receipt("animation.keyframe_ready_lease", {}, name.value());
            receipt.retain();
            receipt.setMetric(0, "outcome", 3); // missing/stale owner
            receipt.setMetric(1, "estimated_bytes", 0);
            receipt.setMetric(2, "charged_bytes", lease.bytes());
            receipt.setMetric(3, "lifecycle_id", lease.mLifecycle);
            if (!lease || !lease.mCharge || generation != mVFS->getIndexGeneration())
                return {};
            if (lease.bytes() && lease.mOwnerEstimate > lease.bytes())
            {
                receipt.setMetric(0, "outcome", 7); // positive charge is below its immutable estimate
                return {};
            }
            try
            {
                // Even a charged warm hit must admit this NEW optional owner.
                // Already-owned progress imports never call this path.
                auto admission = SpeculativeScope::reserveOwnerEstimate(mCache->speculativeBudget(), 0);
                if (!lease.bytes())
                {
                    if (name.extension() != VFS::Path::ExtensionView("kf"))
                    {
                        receipt.setMetric(0, "outcome", 7); // no immutable KF size contract
                        return {};
                    }
                    // Already stamped under the same cache publication as the
                    // generation-tagged payload; no VFS reopen/hash/parse here.
                    const auto estimate = lease.mOwnerEstimate;
                    receipt.setMetric(1, "estimated_bytes", estimate);
                    if (!estimate)
                    {
                        receipt.setMetric(0, "outcome", 7); // unknown publication footprint
                        return {};
                    }
                    if (estimate > maximumPreloadEstimate)
                    {
                        receipt.setMetric(0, "outcome", 5); // local retention cap
                        return {};
                    }
                    auto reservation = SpeculativeScope::reserveOwnerEstimate(mCache->speculativeBudget(), estimate);
                    if (generation != mVFS->getIndexGeneration()
                        || !mCache->promoteReadyLease(name, lease, estimate, reservation))
                    {
                        receipt.setMetric(0, "outcome", 9); // generation/revision/charge winner changed
                        return {};
                    }
                    receipt.setMetric(0, "outcome", 2); // admitted canonical warm charge
                }
                else
                    receipt.setMetric(0, "outcome", 1); // existing positive immutable charge
                receipt.setMetric(2, "charged_bytes", lease.bytes());
                if (lease.bytes() > maximumPreloadEstimate)
                {
                    receipt.setMetric(0, "outcome", 5);
                    return {};
                }
                if (generation != mVFS->getIndexGeneration() || !isReadyLeaseCurrent(name, lease))
                {
                    receipt.setMetric(0, "outcome", 9);
                    return {};
                }
                return lease;
            }
            catch (const SpeculativeDeferred&)
            {
                receipt.setMetric(0, "outcome", 8); // pressure/headroom/scope admission; retryable caller
                throw;
            }
        }
        if (!lease || !lease.mCharge || !lease.bytes() || lease.bytes() > maximumPreloadEstimate
            || generation != mVFS->getIndexGeneration())
            return {}; // unknown/stale owners are not optional free pins
        return lease;
    }

    void KeyframeManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Keyframe", frameNumber, mCache->getStats(), *stats);
    }

}
