#include "objectpaging.hpp"

#include <limits>
#include <array>

#include "occlusionculling.hpp"

#include <components/sceneutil/occlusionculling.hpp>

#include <unordered_map>
#include <set>
#include <vector>

#include <osg/AlphaFunc>
#include <osg/CullFace>
#include <osg/FrontFace>
#include <osg/ComputeBoundsVisitor>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/Sequence>
#include <osg/Switch>
#include <osgAnimation/BasicAnimationManager>
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystemUpdater>
#include <osgUtil/IncrementalCompileOperation>

#include <components/esm/path.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/esm3/readerscache.hpp>
#include <components/esm4/loadacti.hpp>
#include <components/esm4/loadcont.hpp>
#include <components/esm4/loaddoor.hpp>
#include <components/esm4/loadfurn.hpp>
#include <components/esm4/loadstat.hpp>
#include <components/esm4/loadtree.hpp>
#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/rng.hpp>
#include <components/nifosg/autotransform.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/resource/speculativebudget.hpp>
#include <components/resource/v321classifiedcompileset.hpp>
#include <components/sceneutil/boundedtwowaywork.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/pagingwork.hpp>
#include <components/sceneutil/persistentopaquepackets.hpp>
#include <components/sceneutil/gpuindirectpackets.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/shadowproxygroup.hpp>
#include <components/sceneutil/opaqueshadowbatch.hpp>
#include <components/sceneutil/generatedstaticlod.hpp>
#include <components/sceneutil/selectivehlod.hpp>
#include <components/resource/assetpolicy.hpp>
#include <components/resource/derivedassetcache.hpp>
#include <components/resource/assetgpubudget.hpp>
#include <components/resource/speculativebudget.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/riggeometryosgaextension.hpp>
#include <components/sceneutil/util.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v36structuretrace.hpp>
#include <components/settings/ramcache.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

#include "apps/openmw/mwbase/environment.hpp"
#include "apps/openmw/mwbase/world.hpp"
#include "apps/openmw/mwclass/esm4base.hpp"
#include "apps/openmw/mwworld/esmstore.hpp"

#include "vismask.hpp"

namespace MWRender
{

    namespace
    {
        bool typeFilter(int type, bool far)
        {
            switch (type)
            {
                case ESM::REC_STAT:
                case ESM::REC_ACTI:
                case ESM::REC_DOOR:
                case ESM::REC_STAT4:
                case ESM::REC_DOOR4:
                case ESM::REC_TREE4:
                    return true;
                case ESM::REC_CONT:
                case ESM::REC_ACTI4:
                case ESM::REC_CONT4:
                case ESM::REC_FURN4:
                    return !far;

                default:
                    return false;
            }
        }

#ifndef OPENMW_ASSET_PAGING_TEST_HOOKS
        template <typename Record>
        VFS::Path::Normalized getEsm4Model(const Record& record)
        {
            if (MWClass::ESM4Impl::isMarkerModel(record->mModel.getOriginal()))
                return {};
            return record->mModel.getNormalized();
        }

        VFS::Path::Normalized getModel(int type, ESM::RefId id, const MWWorld::ESMStore& store)
        {
            switch (type)
            {
                case ESM::REC_STAT:
                    return store.get<ESM::Static>().searchStatic(id)->mModel.getNormalized();
                case ESM::REC_ACTI:
                    return store.get<ESM::Activator>().searchStatic(id)->mModel.getNormalized();
                case ESM::REC_DOOR:
                    return store.get<ESM::Door>().searchStatic(id)->mModel.getNormalized();
                case ESM::REC_CONT:
                    return store.get<ESM::Container>().searchStatic(id)->mModel.getNormalized();
                case ESM::REC_STAT4:
                    return getEsm4Model(store.get<ESM4::Static>().searchStatic(id));
                case ESM::REC_DOOR4:
                    return getEsm4Model(store.get<ESM4::Door>().searchStatic(id));
                case ESM::REC_TREE4:
                    return getEsm4Model(store.get<ESM4::Tree>().searchStatic(id));
                case ESM::REC_ACTI4:
                    return getEsm4Model(store.get<ESM4::Activator>().searchStatic(id));
                case ESM::REC_CONT4:
                    return getEsm4Model(store.get<ESM4::Container>().searchStatic(id));
                case ESM::REC_FURN4:
                    return getEsm4Model(store.get<ESM4::Furniture>().searchStatic(id));
                default:
                    return {};
            }
        }
#endif
    }

    osg::ref_ptr<osg::Node> ObjectPaging::getChunk(float size, const osg::Vec2f& center, unsigned char /*lod*/,
        unsigned int lodFlags, bool activeGrid, const osg::Vec3f& viewPoint, bool compile)
    {
        if (activeGrid && !mActiveGrid)
            return nullptr;

        const ChunkId id = std::make_tuple(center, size, activeGrid);
        const auto assetGeneration = mAssetGeneration.load(std::memory_order_acquire);
        const auto assetVfsGeneration = mSceneManager->getVFS()->getIndexGeneration();
        const bool assetQualityRepair = Resource::AssetPolicy::enabled(Resource::AssetPolicy::VisualLod)
            || Resource::AssetPolicy::enabled(Resource::AssetPolicy::Hlod);
        const auto assetShaderGeneration = assetQualityRepair
            ? mSceneManager->getShaderManager().assetGeneration() : 0;
        const auto assetSourcesCurrent = [&] {
            return assetGeneration == mAssetGeneration.load(std::memory_order_acquire)
                && assetVfsGeneration == mSceneManager->getVFS()->getIndexGeneration()
                && (!assetQualityRepair || assetShaderGeneration == mSceneManager->getShaderManager().assetGeneration());
        };
        const bool p2RequiredReadiness = SceneUtil::PagingWorkScope::requiredReadiness();
        const unsigned char p3OptionalMask
            = compile && SceneUtil::PagingWorkScope::optionalOptimization()
            ? (!activeGrid ? p3OptionalDistantMask()
                : (Settings::cells().mOptimizedMWActiveShadowBatching
                    ? static_cast<unsigned char>(p3OptionalDistantMask() & (1u << 3u)) : 0)) : 0;
        const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);
        const int v313QualityMode = static_cast<int>(Settings::cells().mV313ChunkQualityMode);

        // Mode0 remains the inherited V3.12/V3.11 first-writer path unless a
        // P3 optional distant upgrade explicitly needs a stronger cached variant.
        if (v313QualityMode == 0 && p3OptionalMask == 0 && !assetQualityRepair)
        {
            if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
            {
                if (v311PrepareMode > 0 && activeGrid && !compile)
                {
                    std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                    if (mV311PreparedActiveChunks.contains(id))
                        mV311PreparedActiveHits.fetch_add(1, std::memory_order_relaxed);
                }
                return static_cast<osg::Node*>(obj.get());
            }

            if (v311PrepareMode > 0 && activeGrid && !compile)
            {
                mV311DemandFallbacks.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                mV311PreparedActiveChunks.erase(id);
            }

            const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
                activeGrid ? "active_grid" : "distant", 0.25);
            SceneUtil::AssetPaging::Outcome outcome;
            osg::ref_ptr<osg::Node> node = createChunkImpl(size, center, activeGrid, viewPoint, compile, lod, outcome);
            mCache->addEntryToObjectCache(id, node.get());

            if (v311PrepareMode > 0 && activeGrid && compile && !p2RequiredReadiness)
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                mV311PreparedActiveChunks.insert(id);
                mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
            }
            return node;
        }

        const unsigned char v313RequestedPrepareMode
            = activeGrid && compile && !p2RequiredReadiness && v311PrepareMode > 0
            ? static_cast<unsigned char>(v311PrepareMode) : 0;
        const unsigned char v313RequestedSpatialMode
            = activeGrid && compile && !p2RequiredReadiness
                && static_cast<int>(Settings::cells().mV312SpatialBatchMode) > 0 ? 1 : 0;
        const V313ChunkQuality v313RequestedQuality{
            v313RequestedPrepareMode, v313RequestedSpatialMode, p3OptionalMask };

        const auto v313QualitySatisfies = [&](const V313ChunkQuality& have, const V313ChunkQuality& need) {
            if (have.mPrepareMode < need.mPrepareMode)
                return false;
            if (v313QualityMode >= 2 && need.mPrepareMode > 0 && have.mSpatialMode != need.mSpatialMode)
                return false;
            constexpr unsigned assetBits = SceneUtil::AssetPaging::Mask << 4;
            const unsigned normalNeed = need.mP3OptionalMask & ~assetBits;
            if ((have.mP3OptionalMask & normalNeed) != normalNeed)
                return false;
            return have.mAssets.satisfies((need.mP3OptionalMask >> 4) & SceneUtil::AssetPaging::Mask,
                SceneUtil::AssetPaging::milliseconds());
        };

        osg::ref_ptr<osg::Object> cached;
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
        // An advisory read lets the fixture hold the historical stale pointer
        // across publication. The authoritative read below always replaces it.
        cached = mCache->getRefFromObjectCache(id);
        if (mFixtureBeforeCache) mFixtureBeforeCache();
#endif
        bool v313RepairBuild = false, v313CachedSatisfies = false;
        SceneUtil::AssetPaging::Quality priorAssets;
        {
            // Node and quality are one publication. A stale caller must not
            // pair an old node with new metadata or invalidate a newer epoch.
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            if (!assetSourcesCurrent())
                throw SceneUtil::PagingWorkCancelled{};
            cached = mCache->getRefFromObjectCache(id);
            auto quality = mV313ChunkQualities.find(id);
            if (cached && assetQualityRepair && (quality == mV313ChunkQualities.end()
                || quality->second.mAssetVfsGeneration != assetVfsGeneration
                || quality->second.mAssetShaderGeneration != assetShaderGeneration))
            {
                // Serialization prevents a new publication between this exact
                // node/quality read and removal. Epoch was checked above.
                mCache->removeFromObjectCache(id); mV313ChunkQualities.erase(id);
                mV313StrongUpgradeInFlight.erase(id); cached = nullptr; quality = mV313ChunkQualities.end();
            }
            if (cached)
            {
                const V313ChunkQuality have = quality != mV313ChunkQualities.end() ? quality->second : V313ChunkQuality{};
                priorAssets = have.mAssets;
                v313CachedSatisfies = !((v313QualityMode > 0 && v313RequestedPrepareMode > 0) || p3OptionalMask != 0)
                    || v313QualitySatisfies(have,v313RequestedQuality);
                const unsigned requestedAssets = (p3OptionalMask >> 4) & SceneUtil::AssetPaging::Mask;
                const unsigned unsettledAssets = requestedAssets & ~have.mAssets.settled();
                if (v313CachedSatisfies && (unsettledAssets & (have.mAssets.transient | have.mAssets.completed)))
                {
                    ++SceneUtil::AssetPaging::Counters::retryDeferred;
                    // A cooldown or submitted-but-unready cache observation is
                    // no attempt, but the terrain owner still needs a later
                    // optional check. Ready/terminal/exhausted quality is settled.
                    SceneUtil::AssetPaging::noteOptionalTransient(unsettledAssets);
                }
                if (!v313CachedSatisfies)
                {
                    mV313WeakCacheHitOnStrongPrepare.fetch_add(1,std::memory_order_relaxed);
                    if (!mV313StrongUpgradeInFlight.insert(id).second)
                    {
                        mV313UpgradeCoalesced.fetch_add(1,std::memory_order_relaxed);
                        SceneUtil::AssetPaging::noteOptionalTransient(unsettledAssets);
                        return static_cast<osg::Node*>(cached.get());
                    }
                    v313RepairBuild = true;
                }
            }
            else
            {
                // Eviction invalidates graph-bound completion, but never the
                // finite transient/backoff/exhaustion history of this source.
                if (quality != mV313ChunkQualities.end() && quality->second.mAssetVfsGeneration == assetVfsGeneration
                    && quality->second.mAssetShaderGeneration == assetShaderGeneration)
                    priorAssets = quality->second.mAssets;
                priorAssets = priorAssets.withoutOwner();
                if (!(priorAssets.transient | priorAssets.exhausted)) mV313ChunkQualities.erase(id);
                if (assetSourcesCurrent())
                    mV313StrongUpgradeInFlight.erase(id);
            }
        }
        if (cached && v313CachedSatisfies)
        {
            if (v311PrepareMode > 0 && activeGrid && !compile)
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                if (mV311PreparedActiveChunks.contains(id)) mV311PreparedActiveHits.fetch_add(1,std::memory_order_relaxed);
            }
            return static_cast<osg::Node*>(cached.get());
        }

        if (v311PrepareMode > 0 && activeGrid && !compile)
        {
            mV311DemandFallbacks.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.erase(id);
        }

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(),
            v313RepairBuild ? "object_chunk_quality_upgrade" : "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);
        osg::ref_ptr<osg::Node> node;
        SceneUtil::AssetPaging::Outcome assetOutcome;
        assetOutcome.allowed &= ~(priorAssets.exhausted | priorAssets.terminal);
        const auto now = SceneUtil::AssetPaging::milliseconds();
        for (unsigned i = 0; i < 2; ++i)
            if ((priorAssets.transient & (1u << i)) && now < priorAssets.retryAfter[i])
                assetOutcome.allowed &= ~(1u << i);
        try
        {
            node = createChunkImpl(size, center, activeGrid, viewPoint, compile, lod, assetOutcome);
            SceneUtil::PagingWorkScope::checkpoint();
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
            if (mFixtureBeforePublish) mFixtureBeforePublish();
#endif
        }
        catch (...)
        {
            if (v313RepairBuild)
            {
                std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
                if (assetSourcesCurrent())
                    mV313StrongUpgradeInFlight.erase(id);
            }
            throw;
        }

        // This reconstruction has a new graph owner. No completion from an
        // unattempted lane may be carried onto its ordinary fallback, even if
        // the previous owner is still alive elsewhere in a view/compile set.
        priorAssets = priorAssets.withoutOwner();
        const V313ChunkQuality v313BuiltQuality{
            static_cast<unsigned char>(activeGrid && compile && !p2RequiredReadiness && v311PrepareMode > 0
                ? v311PrepareMode : 0),
            static_cast<unsigned char>(activeGrid && compile && !p2RequiredReadiness
                && static_cast<int>(Settings::cells().mV312SpatialBatchMode) > 0),
            p3OptionalMask, assetVfsGeneration, assetShaderGeneration,
            assetOutcome.attempted ? priorAssets.after(assetOutcome, SceneUtil::AssetPaging::milliseconds()) : priorAssets };
        if (v313RepairBuild)
            mV313UpgradeBuilt.fetch_add(1, std::memory_order_relaxed);

        if (v313QualityMode > 0 || p3OptionalMask != 0 || assetQualityRepair)
        {
            // Strong-wins installation. A cheap demand miss may have started before a
            // strong worker finished; it must never overwrite the stronger live node.
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            if (!assetSourcesCurrent())
            {
                // This caller no longer owns the current epoch's coalescing
                // flag; the fresh publication/invalidation manages that flag.
                throw SceneUtil::PagingWorkCancelled{};
            }
            const osg::ref_ptr<osg::Object> current = mCache->getRefFromObjectCache(id);
            const auto currentIt = mV313ChunkQualities.find(id);
            const V313ChunkQuality currentQuality
                = current && currentIt != mV313ChunkQualities.end() ? currentIt->second : V313ChunkQuality{};

            const bool currentCoversP3
                = (currentQuality.mP3OptionalMask & v313BuiltQuality.mP3OptionalMask & ~(SceneUtil::AssetPaging::Mask << 4))
                == (v313BuiltQuality.mP3OptionalMask & ~(SceneUtil::AssetPaging::Mask << 4));
            // A deferred cache hit may satisfy demand/backoff; it cannot defeat
            // a completed retry or erase the new finite attempt receipt.
            const bool currentCoversAssets
                = (currentQuality.mAssets.settled() & assetOutcome.attempted) == assetOutcome.attempted
                && (currentQuality.mAssets.completed & assetOutcome.completed) == assetOutcome.completed;
            if (current && v313QualitySatisfies(currentQuality, v313BuiltQuality)
                && currentCoversAssets
                && ((v313BuiltQuality.mP3OptionalMask != 0 && currentCoversP3)
                    || currentQuality.mPrepareMode > v313BuiltQuality.mPrepareMode
                    || (currentQuality.mPrepareMode == v313BuiltQuality.mPrepareMode
                        && (v313QualityMode < 2 || currentQuality.mSpatialMode == v313BuiltQuality.mSpatialMode))))
            {
                // Preserve the already-installed equal-or-stronger node. This is the
                // race that prevents a late compile=false build from downgrading cache quality.
                if (currentQuality.mPrepareMode > 0 && activeGrid)
                {
                    std::lock_guard<std::mutex> preparedLock(mV311PreparedActiveMutex);
                    mV311PreparedActiveChunks.insert(id);
                }
                mV313StrongUpgradeInFlight.erase(id);
                return static_cast<osg::Node*>(current.get());
            }

            mCache->addEntryToObjectCache(id, node.get());
            mV313ChunkQualities[id] = v313BuiltQuality;
            mV313StrongUpgradeInFlight.erase(id);
            if (v313RepairBuild)
                mV313UpgradeInstalled.fetch_add(1, std::memory_order_relaxed);
        }
        else
            mCache->addEntryToObjectCache(id, node.get());

        if (v311PrepareMode > 0 && activeGrid && compile && !p2RequiredReadiness)
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.insert(id);
            mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
        }
        return node;
    }

    namespace
    {
        using P3ShadowBatchResult = SceneUtil::OpaqueShadowBatch::Result;
        P3ShadowBatchResult p3BuildShadowBatch(osg::Group& normal)
        {
            return SceneUtil::OpaqueShadowBatch::build(normal);
        }

        class CanOptimizeCallback : public SceneUtil::Optimizer::IsOperationPermissibleForObjectCallback
        {
        public:
            bool isOperationPermissibleForObjectImplementation(
                const SceneUtil::Optimizer* optimizer, const osg::Drawable* node, unsigned int option) const override
            {
                return true;
            }
            bool isOperationPermissibleForObjectImplementation(
                const SceneUtil::Optimizer* optimizer, const osg::Node* node, unsigned int option) const override
            {
                return (node->getDataVariance() != osg::Object::DYNAMIC);
            }
        };

        using LODRange = osg::LOD::MinMaxPair;

        LODRange intersection(const LODRange& left, const LODRange& right)
        {
            return { std::max(left.first, right.first), std::min(left.second, right.second) };
        }

        bool empty(const LODRange& r)
        {
            return r.first >= r.second;
        }

        LODRange operator/(const LODRange& r, float div)
        {
            return { r.first / div, r.second / div };
        }

        class CopyOp : public osg::CopyOp
        {
        public:
            bool mActiveGrid = false;
            LODRange mDistances = { 0.f, 0.f };
            osg::Node::NodeMask mCopyMask = ~0u;

            CopyOp(bool activeGrid, osg::Node::NodeMask copyMask)
                : mActiveGrid(activeGrid)
                , mCopyMask(copyMask)
            {
            }

            void copy(const osg::Node* toCopy, osg::Group* attachTo)
            {
                const osg::Group* groupToCopy = toCopy->asGroup();
                if (toCopy->getStateSet() || toCopy->asTransform() || !groupToCopy)
                    attachTo->addChild(operator()(toCopy));
                else
                {
                    for (unsigned int i = 0; i < groupToCopy->getNumChildren(); ++i)
                        attachTo->addChild(operator()(groupToCopy->getChild(i)));
                }
            }

            osg::Node* operator()(const osg::Node* node) const override
            {
                if (!(node->getNodeMask() & mCopyMask))
                    return nullptr;

                if (const osg::Drawable* d = node->asDrawable())
                    return operator()(d);

                if (dynamic_cast<const osgParticle::ParticleProcessor*>(node))
                    return nullptr;
                if (dynamic_cast<const osgParticle::ParticleSystemUpdater*>(node))
                    return nullptr;

                if (const osg::Switch* sw = node->asSwitch())
                {
                    osg::Group* n = new osg::Group;
                    for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                        if (sw->getValue(i))
                            n->addChild(operator()(sw->getChild(i)));
                    n->setDataVariance(osg::Object::STATIC);
                    return n;
                }
                if (const osg::LOD* lod = dynamic_cast<const osg::LOD*>(node))
                {
                    std::vector<std::pair<osg::ref_ptr<osg::Node>, LODRange>> children;
                    for (unsigned int i = 0; i < lod->getNumChildren(); ++i)
                        if (const auto r = intersection(lod->getRangeList()[i], mDistances); !empty(r))
                            children.emplace_back(operator()(lod->getChild(i)), lod->getRangeList()[i]);
                    if (children.empty())
                        return nullptr;

                    if (children.size() == 1)
                        return children.front().first.release();
                    else
                    {
                        osg::LOD* n = new osg::LOD;
                        for (const auto& [child, range] : children)
                            n->addChild(child, range.first, range.second);
                        n->setRangeMode(lod->getRangeMode());
                        n->setCenterMode(lod->getCenterMode());
                        n->setCenter(lod->getCenter());
                        n->setRadius(lod->getRadius());
                        n->setDataVariance(osg::Object::STATIC);
                        return n;
                    }
                }
                if (const osg::Sequence* sq = dynamic_cast<const osg::Sequence*>(node))
                {
                    osg::Group* n = new osg::Group;
                    n->addChild(operator()(sq->getChild(sq->getValue() != -1 ? sq->getValue() : 0)));
                    n->setDataVariance(osg::Object::STATIC);
                    return n;
                }

                if (!mActiveGrid)
                {
                    if (const auto* autoTransform = dynamic_cast<const NifOsg::AutoTransform*>(node))
                    {
                        osg::MatrixTransform* n = new osg::MatrixTransform();
                        n->setMatrix(autoTransform->computeMatrix(nullptr));

                        for (unsigned int i = 0; i < autoTransform->getNumChildren(); ++i)
                            if (osg::Node* clonedChild = operator()(autoTransform->getChild(i)))
                                n->addChild(clonedChild);

                        n->setDataVariance(osg::Object::STATIC);

                        handleCallbacks(node, n);

                        return n;
                    }
                }

                osg::Node* cloned = static_cast<osg::Node*>(node->clone(*this));
                if (!mActiveGrid)
                    cloned->setDataVariance(osg::Object::STATIC);
                cloned->setUserDataContainer(nullptr);
                cloned->setName("");

                handleCallbacks(node, cloned);

                return cloned;
            }

            void handleCallbacks(const osg::Node* node, osg::Node* cloned) const
            {
                for (const osg::Callback* callback = node->getCullCallback(); callback != nullptr;
                     callback = callback->getNestedCallback())
                {
                    if (node->getCullCallback()->getNestedCallback())
                    {
                        osg::Callback* clonedCallback = osg::clone(callback, osg::CopyOp::SHALLOW_COPY);
                        clonedCallback->setNestedCallback(nullptr);
                        cloned->addCullCallback(clonedCallback);
                    }
                    else
                        cloned->addCullCallback(const_cast<osg::Callback*>(callback));
                }
            }
            osg::Drawable* operator()(const osg::Drawable* drawable) const override
            {
                if (!(drawable->getNodeMask() & mCopyMask))
                    return nullptr;

                if (dynamic_cast<const osgParticle::ParticleSystem*>(drawable))
                    return nullptr;

                if (dynamic_cast<const SceneUtil::OsgaRigGeometry*>(drawable))
                    return nullptr;
                if (const SceneUtil::RigGeometry* rig = dynamic_cast<const SceneUtil::RigGeometry*>(drawable))
                    return operator()(rig->getSourceGeometry());
                if (const SceneUtil::MorphGeometry* morph = dynamic_cast<const SceneUtil::MorphGeometry*>(drawable))
                    return operator()(morph->getSourceGeometry());

                if (getCopyFlags() & DEEP_COPY_DRAWABLES)
                {
                    osg::Drawable* d = static_cast<osg::Drawable*>(drawable->clone(*this));
                    d->setDataVariance(osg::Object::STATIC);
                    d->setUserDataContainer(nullptr);
                    d->setName("");
                    return d;
                }
                else
                    return const_cast<osg::Drawable*>(drawable);
            }
            osg::Callback* operator()(const osg::Callback* callback) const override { return nullptr; }
        };

        class RefnumSet : public osg::Object
        {
        public:
            RefnumSet() {}
            RefnumSet(const RefnumSet& copy, const osg::CopyOp&)
                : mRefnums(copy.mRefnums)
            {
            }
            META_Object(MWRender, RefnumSet)
            std::vector<ESM::RefNum> mRefnums;
        };

        class AnalyzeVisitor : public osg::NodeVisitor
        {
        public:
            AnalyzeVisitor(osg::Node::NodeMask analyzeMask)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mCurrentStateSet(nullptr)
            {
                setTraversalMask(analyzeMask);
            }

            typedef std::unordered_map<osg::StateSet*, unsigned int> StateSetCounter;
            struct Result
            {
                StateSetCounter mStateSetCounter;
                unsigned int mNumVerts = 0;
            };

            void apply(osg::Node& node) override
            {
                if (node.getStateSet())
                    mCurrentStateSet = node.getStateSet();

                if (osg::Switch* sw = node.asSwitch())
                {
                    for (unsigned int i = 0; i < sw->getNumChildren(); ++i)
                        if (sw->getValue(i))
                            traverse(*sw->getChild(i));
                    return;
                }
                if (osg::LOD* lod = dynamic_cast<osg::LOD*>(&node))
                {
                    for (unsigned int i = 0; i < lod->getNumChildren(); ++i)
                        if (const auto r = intersection(lod->getRangeList()[i], mDistances); !empty(r))
                            traverse(*lod->getChild(i));
                    return;
                }
                if (osg::Sequence* sq = dynamic_cast<osg::Sequence*>(&node))
                {
                    traverse(*sq->getChild(sq->getValue() != -1 ? sq->getValue() : 0));
                    return;
                }

                traverse(node);
            }
            void apply(osg::Geometry& geom) override
            {
                if (osg::Array* array = geom.getVertexArray())
                    mResult.mNumVerts += array->getNumElements();

                ++mResult.mStateSetCounter[mCurrentStateSet];
                ++mGlobalStateSetCounter[mCurrentStateSet];
            }
            Result retrieveResult()
            {
                Result result = mResult;
                mResult = Result();
                mCurrentStateSet = nullptr;
                return result;
            }
            void addInstance(const Result& result)
            {
                for (auto pair : result.mStateSetCounter)
                    mGlobalStateSetCounter[pair.first] += pair.second;
            }
            float getMergeBenefit(const Result& result)
            {
                if (result.mStateSetCounter.empty())
                    return 1;
                float mergeBenefit = 0;
                for (auto pair : result.mStateSetCounter)
                {
                    mergeBenefit += mGlobalStateSetCounter[pair.first];
                }
                mergeBenefit /= result.mStateSetCounter.size();
                return mergeBenefit;
            }

            Result mResult;
            osg::StateSet* mCurrentStateSet;
            StateSetCounter mGlobalStateSetCounter;
            LODRange mDistances = { 0.f, 0.f };
        };

        class DebugVisitor : public osg::NodeVisitor
        {
        public:
            DebugVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }
            void apply(osg::Drawable& node) override
            {
                osg::ref_ptr<SceneUtil::Material> m(new SceneUtil::Material);
                osg::Vec4f color(
                    Misc::Rng::rollProbability(), Misc::Rng::rollProbability(), Misc::Rng::rollProbability(), 0.f);
                color.normalize();
                m->setDiffuse(osg::Vec4f(0.1f, 0.1f, 0.1f, 1.f));
                m->setAmbient(osg::Vec4f(0.1f, 0.1f, 0.1f, 1.f));
                m->setEmission(osg::Vec4f(color));
                m->setVertexColorMode(SceneUtil::VertexColorModes::None);
                osg::ref_ptr<osg::StateSet> stateset = node.getStateSet()
                    ? osg::clone(node.getStateSet(), osg::CopyOp::SHALLOW_COPY)
                    : new osg::StateSet;
                stateset->setAttribute(m);
                m->updateStateSet(stateset);
                node.setStateSet(stateset);
            }
        };

        class AddRefnumMarkerVisitor : public osg::NodeVisitor
        {
        public:
            AddRefnumMarkerVisitor(ESM::RefNum refnum)
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
                , mRefnum(refnum)
            {
            }
            ESM::RefNum mRefnum;
            void apply(osg::Geometry& node) override
            {
                osg::ref_ptr<RefnumMarker> marker(new RefnumMarker);
                marker->mRefnum = mRefnum;
                if (osg::Array* array = node.getVertexArray())
                    marker->mNumVertices = array->getNumElements();
                node.getOrCreateUserDataContainer()->addUserObject(marker);
            }
        };
    }

    ObjectPaging::ObjectPaging(Resource::SceneManager* sceneManager, ESM::RefId worldspace)
        : GenericResourceManager<ChunkId>(nullptr, Settings::RamCache::objectPagingExpiryDelay())
        , Terrain::QuadTreeWorld::ChunkManager(worldspace)
        , mSceneManager(sceneManager)
        , mActiveGrid(Settings::terrain().mObjectPagingActiveGrid)
        , mDebugBatches(Settings::terrain().mDebugChunks)
        , mMergeFactor(Settings::terrain().mObjectPagingMergeFactor)
        , mMinSize(Settings::terrain().mObjectPagingMinSize)
        , mMinSizeMergeFactor(Settings::terrain().mObjectPagingMinSizeMergeFactor)
        , mMinSizeCostMultiplier(Settings::terrain().mObjectPagingMinSizeCostMultiplier)
        , mRefTrackerLocked(false)
    {
    }

    unsigned char ObjectPaging::p3OptionalDistantMask() const
    {
        unsigned char mask = 0;
        if (static_cast<bool>(Settings::cells().mOptimizedMWSemanticPremerge))
            mask |= 1u << 0u;
        if (static_cast<bool>(Settings::cells().mOptimizedMWDistantDisplayLists))
            mask |= 1u << 1u;
        if (static_cast<bool>(Settings::cells().mOptimizedMWNormalizedStaticPackets))
            mask |= 1u << 2u;
        if (static_cast<bool>(Settings::cells().mOptimizedMWShadowStaticBatching))
            mask |= 1u << 3u;
        if (Resource::AssetPolicy::enabled(Resource::AssetPolicy::VisualLod)) mask |= 1u << 4u;
        if (Resource::AssetPolicy::enabled(Resource::AssetPolicy::Hlod)) mask |= 1u << 5u;
        return mask;
    }

    bool ObjectPaging::supportsDistantStrongPagingUpgrade() const
    {
        return p3OptionalDistantMask() != 0;
    }

    void ObjectPaging::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, unsigned int maxTriangles,
        OcclusionCulling::OcclusionStorage* storage, bool coarseChunkOcclusion)
    {
        mOcclusionCuller = culler;
        mMaxTriangles = maxTriangles;
        mOcclusionStorage = storage;
        mV35CoarseChunkOcclusion = coarseChunkOcclusion;
    }

    namespace
    {
        struct PagedCellRef
        {
            ESM::RefId mRefId;
            ESM::RefNum mRefNum;
            osg::Vec3f mPosition;
            osg::Vec3f mRotation;
            float mScale;
        };

        PagedCellRef makePagedCellRef(const ESM::CellRef& value)
        {
            return PagedCellRef{
                .mRefId = value.mRefID,
                .mRefNum = value.mRefNum,
                .mPosition = value.mPos.asVec3(),
                .mRotation = value.mPos.asRotationVec3(),
                .mScale = value.mScale,
            };
        }

        PagedCellRef makePagedCellRef(const ESM4::Reference& value)
        {
            return PagedCellRef{
                .mRefId = value.mBaseObj,
                .mRefNum = value.mId,
                .mPosition = value.mPos.asVec3(),
                .mRotation = value.mPos.asRotationVec3(),
                .mScale = value.mScale,
            };
        }

#ifndef OPENMW_ASSET_PAGING_TEST_HOOKS
        std::map<ESM::RefNum, PagedCellRef> collectESM3References(
            float size, const osg::Vec2i& startCell, const MWWorld::ESMStore& store)
        {
            std::map<ESM::RefNum, PagedCellRef> refs;
            ESM::ReadersCache readers;
            for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
            {
                for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
                {
                    const ESM::Cell* cell = store.get<ESM::Cell>().searchStatic(cellX, cellY);
                    if (!cell)
                        continue;
                    for (size_t i = 0; i < cell->mContextList.size(); ++i)
                    {
                        try
                        {
                            const std::size_t index = static_cast<std::size_t>(cell->mContextList[i].index);
                            const ESM::ReadersCache::BusyItem reader = readers.get(index);
                            cell->restore(*reader, i);
                            ESM::CellRef ref;
                            ESM::MovedCellRef cMRef;
                            bool deleted = false;
                            bool moved = false;
                            while (ESM::Cell::getNextRef(
                                *reader, ref, deleted, cMRef, moved, ESM::Cell::GetNextRefMode::LoadOnlyNotMoved))
                            {
                                if (moved)
                                    continue;

                                if (std::find(cell->mMovedRefs.begin(), cell->mMovedRefs.end(), ref.mRefNum)
                                    != cell->mMovedRefs.end())
                                    continue;

                                int type = store.findStatic(ref.mRefID);
                                if (!typeFilter(type, size >= 2))
                                    continue;
                                if (deleted)
                                {
                                    refs.erase(ref.mRefNum);
                                    continue;
                                }
                                refs.insert_or_assign(ref.mRefNum, makePagedCellRef(ref));
                            }
                        }
                        catch (const std::exception& e)
                        {
                            Log(Debug::Warning) << "Failed to collect references from cell \"" << cell->getDescription()
                                                << "\": " << e.what();
                            continue;
                        }
                    }
                    for (const auto& [ref, deleted] : cell->mLeasedRefs)
                    {
                        if (deleted)
                        {
                            refs.erase(ref.mRefNum);
                            continue;
                        }
                        int type = store.findStatic(ref.mRefID);
                        if (!typeFilter(type, size >= 2))
                            continue;
                        refs.insert_or_assign(ref.mRefNum, makePagedCellRef(ref));
                    }
                }
            }
            return refs;
        }

        std::map<ESM::RefNum, PagedCellRef> collectESM4References(
            float size, const osg::Vec2i& startCell, ESM::RefId worldspace)
        {
            std::map<ESM::RefNum, PagedCellRef> refs;
            const auto& store = MWBase::Environment::get().getWorld()->getStore();
            for (int cellX = startCell.x(); cellX < startCell.x() + size; ++cellX)
            {
                for (int cellY = startCell.y(); cellY < startCell.y() + size; ++cellY)
                {
                    const ESM4::Cell* cell
                        = store.get<ESM4::Cell>().searchExterior(ESM::ExteriorCellLocation(cellX, cellY, worldspace));
                    if (!cell)
                        continue;
                    for (const ESM4::Reference* ref4 : store.get<ESM4::Reference>().getByCell(cell->mId))
                    {
                        if (ref4->mFlags & ESM4::Rec_Disabled)
                            continue;
                        int type = store.findStatic(ref4->mBaseObj);
                        if (!typeFilter(type, size >= 2))
                            continue;
                        if (!ref4->mEsp.parent.isZeroOrUnset())
                        {
                            const ESM4::Reference* parentRef
                                = store.get<ESM4::Reference>().searchStatic(ref4->mEsp.parent);
                            if (parentRef)
                            {
                                bool parentDisabled = parentRef->mFlags & ESM4::Rec_Disabled;
                                bool inversed = ref4->mEsp.flags & ESM4::EnableParent::Flag_Inversed;
                                if (parentDisabled != inversed)
                                    continue;
                            }
                        }
                        refs.insert_or_assign(ref4->mId, makePagedCellRef(*ref4));
                    }
                }
            }
            return refs;
        }
#endif
    }

    osg::ref_ptr<osg::Node> ObjectPaging::createChunk(float size, const osg::Vec2f& center, bool activeGrid,
        const osg::Vec3f& viewPoint, bool compile, unsigned char lod)
    {
        SceneUtil::AssetPaging::Outcome outcome;
        return createChunkImpl(size, center, activeGrid, viewPoint, compile, lod, outcome);
    }

    osg::ref_ptr<osg::Node> ObjectPaging::createChunkImpl(float size, const osg::Vec2f& center, bool activeGrid,
        const osg::Vec3f& viewPoint, bool compile, unsigned char lod, SceneUtil::AssetPaging::Outcome& assetOutcome)
    {
        Debug::V3Diagnostics::TraceScope trace(
            "paging", "object_chunk_create", activeGrid ? "active_grid" : "distant", 0.1);
        const bool v36StructureEnabled = Debug::V36StructureTrace::writer().enabled();
        const auto v36StructureStart
            = v36StructureEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        const osg::Vec2i startCell(static_cast<int>(std::floor(center.x() - size / 2.f)),
            static_cast<int>(std::floor(center.y() - size / 2.f)));
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
        const bool providedPopulation = !mFixturePopulation.empty();
        std::map<ESM::RefNum,const FixtureReference*> providedReferences;
#endif
#ifndef OPENMW_ASSET_PAGING_TEST_HOOKS
        const MWBase::World* world = MWBase::Environment::get().getWorld();
        const MWWorld::ESMStore* store = &world->getStore();
#else
        if (!providedPopulation) throw std::logic_error("chain fixture requires a bounded parsed source population");
#endif
        std::map<ESM::RefNum, PagedCellRef> refs;

        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_collect_refs", activeGrid ? "active_grid" : "distant", 0.1);
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
            if (providedPopulation)
            {
                unsigned index = 0;
                for (const auto& source : mFixturePopulation)
                {
                    ESM::RefNum id; id.mIndex = ++index; id.mContentFile = 0;
                    refs.emplace(id,PagedCellRef{ESM::RefId::stringRefId("asset-chain-" + std::to_string(index)),
                        id,source.position,{},source.scale});
                    providedReferences.emplace(id,&source);
                }
            }
#else
            if (mWorldspace == ESM::Cell::sDefaultWorldspaceId)
            {
                refs = collectESM3References(size, startCell, *store);
            }
            else
            {
                refs = collectESM4References(size, startCell, mWorldspace);
            }
#endif
        }

        const auto sourceType = [&](ESM::RefNum number, const PagedCellRef& ref) {
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
            return providedReferences.at(number)->type;
#else
            return store->findStatic(ref.mRefId);
#endif
        };
        const auto sourceModel = [&](ESM::RefNum number, int type, const PagedCellRef& ref) {
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
            return providedReferences.at(number)->model;
#else
            return getModel(type,ref.mRefId,*store);
#endif
        };
        const auto sourceVersion = [&](ESM::RefNum number) {
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
            return static_cast<unsigned>(ESM::VER_130);
#else
            return world->getESMVersions()[number.mContentFile];
#endif
        };
        if (activeGrid && !refs.empty())
        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            const std::set<ESM::RefNum>& blacklist = getRefTracker().mBlacklist;
            if (blacklist.size() < refs.size())
            {
                for (ESM::RefNum ref : blacklist)
                    refs.erase(ref);
            }
            else
            {
                std::erase_if(refs, [&](const auto& ref) { return blacklist.contains(ref.first); });
            }
        }

        const osg::Vec2f minBound = (center - osg::Vec2f(size / 2.f, size / 2.f));
        const osg::Vec2f maxBound = (center + osg::Vec2f(size / 2.f, size / 2.f));
        const osg::Vec2i floorMinBound(
            static_cast<int>(std::floor(minBound.x())), static_cast<int>(std::floor(minBound.y())));
        const osg::Vec2i ceilMaxBound(
            static_cast<int>(std::ceil(maxBound.x())), static_cast<int>(std::ceil(maxBound.y())));
        struct InstanceList
        {
            std::vector<const PagedCellRef*> mInstances;
            AnalyzeVisitor::Result mAnalyzeResult;
            VFS::Path::Normalized mModel;
            bool mNeedCompile = false;
        };
        typedef std::map<osg::ref_ptr<const osg::Node>, InstanceList> NodeMap;
        NodeMap nodes;
        const unsigned requestedAssets = !activeGrid && compile && SceneUtil::PagingWorkScope::optionalOptimization()
            ? Resource::AssetPolicy::mask() & SceneUtil::AssetPaging::Mask & assetOutcome.allowed : 0;
        const bool assetPreparation = requestedAssets && Resource::SpeculativeScope::active();
        if (requestedAssets && !assetPreparation) assetOutcome.defer(requestedAssets);
        using AssetSource = SceneUtil::AssetPaging::Source;
        std::map<const osg::Node*, AssetSource> assetCertifiedSources;
        std::set<const osg::Node*> assetCheckedTemplates;
        const osg::ref_ptr<RefnumSet> refnumSet = activeGrid ? new RefnumSet : nullptr;

        // Mask_UpdateVisitor is used in such cases in NIF loader:
        // 1. For collision nodes, which is not supposed to be rendered.
        // 2. For nodes masked via Flag_Hidden (VisController can change this flag value at runtime).
        // Since ObjectPaging does not handle VisController, we can just ignore both types of nodes.
        constexpr auto copyMask = ~Mask_UpdateVisitor;

        const int cellSize = getCellSize(mWorldspace);
        const float smallestDistanceToChunk = (size > 1 / 8.f) ? (size * cellSize) : 0.f;
        const float higherDistanceToChunk
            = activeGrid ? ((size < 1) ? 5 : 3) * cellSize * size + 1 : smallestDistanceToChunk + 1;
        const LODRange lodDistances = activeGrid ? LODRange{ 0.f, std::numeric_limits<float>::max() }
                                                 : LODRange{ smallestDistanceToChunk, higherDistanceToChunk };

        AnalyzeVisitor analyzeVisitor(copyMask);
        const float minSize = mMinSizeMergeFactor ? mMinSize * mMinSizeMergeFactor : mMinSize;

        std::size_t p3TemplatePrefetchModels = 0;
        std::size_t p3TemplateReuseHits = 0;
        std::size_t p3TemplatePrefetchHelpers = 0;
        bool p3TemplatePrefetchParallel = false;
        std::map<VFS::Path::Normalized, osg::ref_ptr<const osg::Node>> p3PrefetchedTemplates;
        const bool p3TemplatePrefetchRequired = SceneUtil::PagingWorkScope::requiredReadiness();
        const bool p3TemplatePrefetch = static_cast<bool>(Settings::cells().mOptimizedMWParallelTemplatePrefetch)
            && !activeGrid && compile && !SceneUtil::PagingWorkScope::optionalOptimization();
        if (p3TemplatePrefetch)
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "p3_template_prefetch", p3TemplatePrefetchRequired ? "required_distant" : "distant", 0.1);
            std::set<VFS::Path::Normalized> seenModels;
            std::vector<VFS::Path::Normalized> models;
            models.reserve(refs.size());

            // Reproduce only checks that occur before the authoritative
            // getTemplate() call below. The original loop still owns grouping,
            // radius decisions, analysis, merge and publication.
            for (const auto& [refNum, ref] : refs)
            {
                SceneUtil::PagingWorkScope::checkpoint();
                if (size < 1.f)
                {
                    const osg::Vec3f cellPos = ref.mPosition / static_cast<float>(cellSize);
                    if ((minBound.x() > floorMinBound.x() && cellPos.x() < minBound.x())
                        || (minBound.y() > floorMinBound.y() && cellPos.y() < minBound.y())
                        || (maxBound.x() < ceilMaxBound.x() && cellPos.x() >= maxBound.x())
                        || (maxBound.y() < ceilMaxBound.y() && cellPos.y() >= maxBound.y()))
                        continue;
                }

                const float dSqr = (viewPoint - ref.mPosition).length2();
                {
                    std::lock_guard<std::mutex> lock(mSizeCacheMutex);
                    const SizeCache::iterator found = mSizeCache.find(refNum);
                    if (found != mSizeCache.end() && found->second < dSqr * minSize * minSize)
                        continue;
                }
                if (Misc::ResourceHelpers::isHiddenMarker(ref.mRefId))
                    continue;

                const int type = sourceType(refNum,ref);
                VFS::Path::Normalized model = sourceModel(refNum,type,ref);
                if (model.empty())
                    continue;
                model = Misc::ResourceHelpers::correctMeshPath(model);
                {
                    std::lock_guard<std::mutex> lock(mLODNameCacheMutex);
                    LODNameCacheKey key{ model, lod };
                    LODNameCache::const_iterator found = mLODNameCache.lower_bound(key);
                    if (found != mLODNameCache.end() && found->first == key)
                        model = found->second;
                    else
                        model = mLODNameCache
                                    .emplace_hint(found, std::move(key),
                                        Misc::ResourceHelpers::getLODMeshName(sourceVersion(refNum),
                                            model, *mSceneManager->getVFS(), lod))
                                    ->second;
                }
                {
                    std::lock_guard<std::mutex> lock(mRefTrackerMutex);
                    if (getRefTracker().mDisabled.count(refNum))
                        continue;
                }
                if (seenModels.insert(model).second)
                    models.push_back(std::move(model));
            }

            p3TemplatePrefetchModels = models.size();
            const std::size_t minimumTemplates = static_cast<std::size_t>(
                Settings::cells().mOptimizedMWParallelTemplatePrefetchMinTemplates);
            std::vector<osg::ref_ptr<const osg::Node>> prefetchedNodes(models.size());
            const auto speculativeContext = Resource::SpeculativeScope::capture();
            const auto pagingContext = SceneUtil::PagingWorkScope::capture();
            std::array<std::uint64_t, 2> helperRetainedEstimates{};
            std::atomic_size_t nextModel{ 0 };
            const std::size_t configuredHelpers = static_cast<std::size_t>(
                Settings::cells().mOptimizedMWParallelTemplatePrefetchWorkers);
            const std::size_t requestedHelpers
                = models.size() >= minimumTemplates * 4 ? configuredHelpers : std::size_t{ 1 };

            auto loadWorker = [&](std::size_t workerIndex) {
                auto loadRemaining = [&] {
                    for (;;)
                    {
                        const std::size_t i = nextModel.fetch_add(1, std::memory_order_relaxed);
                        if (i >= models.size())
                            break;
                        SceneUtil::PagingWorkScope::checkpoint();
                        prefetchedNodes[i] = mSceneManager->getTemplate(models[i], false);
                    }
                };

                if (workerIndex == 0)
                {
                    loadRemaining();
                    return;
                }

                Resource::SpeculativeScope helperSpeculation(speculativeContext);
                SceneUtil::PagingWorkScope helperPaging(pagingContext);
                struct Receipt
                {
                    Resource::SpeculativeScope& scope;
                    std::uint64_t& estimate;
                    ~Receipt() { estimate = scope.retainedEstimate(); }
                } receipt{ helperSpeculation, helperRetainedEstimates.at(workerIndex - 1) };
                loadRemaining();
            };

            auto creditHelperRetained = [&] {
                for (const std::uint64_t bytes : helperRetainedEstimates)
                    Resource::SpeculativeScope::creditRetainedEstimate(bytes);
            };

            try
            {
                p3TemplatePrefetchHelpers = SceneUtil::BoundedTwoWayWork::runWorkers(
                    models.size(), minimumTemplates, requestedHelpers, loadWorker);
                p3TemplatePrefetchParallel = p3TemplatePrefetchHelpers != 0;
            }
            catch (...)
            {
                creditHelperRetained();
                throw;
            }
            creditHelperRetained();

            if (!p3TemplatePrefetchParallel)
                loadWorker(0);

            for (std::size_t i = 0; i < models.size(); ++i)
                if (prefetchedNodes[i])
                    p3PrefetchedTemplates.emplace(models[i], prefetchedNodes[i]);
        }

        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_template_analysis", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& [refNum, ref] : refs)
            {
            SceneUtil::PagingWorkScope::checkpoint();
            if (size < 1.f)
            {
                const osg::Vec3f cellPos = ref.mPosition / static_cast<float>(cellSize);
                if ((minBound.x() > floorMinBound.x() && cellPos.x() < minBound.x())
                    || (minBound.y() > floorMinBound.y() && cellPos.y() < minBound.y())
                    || (maxBound.x() < ceilMaxBound.x() && cellPos.x() >= maxBound.x())
                    || (maxBound.y() < ceilMaxBound.y() && cellPos.y() >= maxBound.y()))
                    continue;
            }

            const float dSqr = (viewPoint - ref.mPosition).length2();
            if (!activeGrid)
            {
                std::lock_guard<std::mutex> lock(mSizeCacheMutex);
                SizeCache::iterator found = mSizeCache.find(refNum);
                if (found != mSizeCache.end() && found->second < dSqr * minSize * minSize)
                    continue;
            }

            if (Misc::ResourceHelpers::isHiddenMarker(ref.mRefId))
                continue;

            const int type = sourceType(refNum,ref);
            VFS::Path::Normalized model = sourceModel(refNum,type,ref);
            if (model.empty())
                continue;
            model = Misc::ResourceHelpers::correctMeshPath(model);

            if (activeGrid && type != ESM::REC_STAT && type != ESM::REC_STAT4)
            {
                model = Misc::ResourceHelpers::correctActorModelPath(model, mSceneManager->getVFS());
                constexpr VFS::Path::ExtensionView nif("nif");
                if (model.extension() == nif)
                {
                    VFS::Path::Normalized kfname = model;
                    constexpr VFS::Path::ExtensionView kf("kf");
                    kfname.changeExtension(kf);
                    if (mSceneManager->getVFS()->exists(kfname))
                        continue;
                }
            }

            const VFS::Path::Normalized originalAssetModel = model;
            if (!activeGrid)
            {
                std::lock_guard<std::mutex> lock(mLODNameCacheMutex);
                LODNameCacheKey key{ model, lod };
                LODNameCache::const_iterator found = mLODNameCache.lower_bound(key);
                if (found != mLODNameCache.end() && found->first == key)
                    model = found->second;
                else
                    model = mLODNameCache
                                .emplace_hint(found, std::move(key),
                                    Misc::ResourceHelpers::getLODMeshName(sourceVersion(refNum),
                                        model, *mSceneManager->getVFS(), lod))
                                ->second;
            }

            osg::ref_ptr<const osg::Node> cnode;
            if (p3TemplatePrefetch)
            {
                const auto prefetched = p3PrefetchedTemplates.find(model);
                if (prefetched != p3PrefetchedTemplates.end())
                {
                    cnode = prefetched->second;
                    ++p3TemplateReuseHits;
                }
            }
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
            if (!cnode && providedPopulation && providedReferences.at(refNum)->original)
                cnode = providedReferences.at(refNum)->original;
#endif
            if (!cnode)
                cnode = mSceneManager->getTemplate(model, false);

            // Certify original ancestry BEFORE CopyOp can strip controllers or rig/morph identity.
            // Only the optional worker resolves/hashes a winning source stream.
            if (assetPreparation && model == originalAssetModel && ref.mScale > 0.f
                && (type == ESM::REC_STAT || type == ESM::REC_STAT4)
                && assetCertifiedSources.size() < 64 && assetCheckedTemplates.size() < 256
                && assetCheckedTemplates.insert(cnode.get()).second)
            {
                if (auto source = SceneUtil::AssetPaging::prepareSource(*cnode,model,*mSceneManager->getVFS(),requestedAssets,assetOutcome))
                    assetCertifiedSources.emplace(cnode.get(),std::move(*source));
            }

            if (activeGrid)
            {
                if (cnode->getNumChildrenRequiringUpdateTraversal() > 0
                    || SceneUtil::hasUserDescription(cnode, Constants::NightDayLabel)
                    || SceneUtil::hasUserDescription(cnode, Constants::HerbalismLabel)
                    || (cnode->getName() == "Collada visual scene group"
                        && dynamic_cast<const osgAnimation::BasicAnimationManager*>(cnode->getUpdateCallback())))
                    continue;
                else
                    refnumSet->mRefnums.push_back(refNum);
            }

            {
                std::lock_guard<std::mutex> lock(mRefTrackerMutex);
                if (getRefTracker().mDisabled.count(refNum))
                    continue;
            }

            const float radius2 = cnode->getBound().radius2() * ref.mScale * ref.mScale;
            if (radius2 < dSqr * minSize * minSize && !activeGrid)
            {
                std::lock_guard<std::mutex> lock(mSizeCacheMutex);
                mSizeCache[refNum] = radius2;
                continue;
            }

            const auto emplaced = nodes.emplace(std::move(cnode), InstanceList());
            if (emplaced.second)
            {
                analyzeVisitor.mDistances = lodDistances / ref.mScale;
                const osg::Node* const nodePtr = emplaced.first->first.get();
                // const-trickery required because there is no const version of NodeVisitor
                const_cast<osg::Node*>(nodePtr)->accept(analyzeVisitor);
                emplaced.first->second.mAnalyzeResult = analyzeVisitor.retrieveResult();
                emplaced.first->second.mModel = model;
                emplaced.first->second.mNeedCompile = compile && nodePtr->referenceCount() <= 2;
            }
            else
                analyzeVisitor.addInstance(emplaced.first->second.mAnalyzeResult);
            emplaced.first->second.mInstances.push_back(&ref);
            }
        }

        const osg::Vec3f worldCenter
            = osg::Vec3f(center.x(), center.y(), 0) * static_cast<float>(getCellSize(mWorldspace));
        osg::ref_ptr<osg::Group> group = new osg::Group;
        const int v312SpatialBatchMode = static_cast<int>(Settings::cells().mV312SpatialBatchMode);
        const bool v312SpatialPrepared = v312SpatialBatchMode > 0 && activeGrid && compile
            && static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) >= 2;
        const int v315PacketizedPremergeMode = static_cast<int>(Settings::cells().mV315PacketizedPremergeMode);
        const bool v315PacketizedPrepared = !v312SpatialPrepared && v315PacketizedPremergeMode > 0
            && activeGrid && compile
            && static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) >= 2
            && static_cast<int>(Settings::cells().mV38WorldBatchingMode) >= 2;

        std::vector<osg::ref_ptr<osg::Group>> v312MergeGroups;
        const unsigned int v315PacketCount
            = v315PacketizedPrepared ? (v315PacketizedPremergeMode >= 2 ? 16u : 4u)
                                     : (v312SpatialPrepared ? 4u : 1u);
        v312MergeGroups.reserve(v315PacketCount);
        for (unsigned int i = 0; i < v315PacketCount; ++i)
            v312MergeGroups.emplace_back(new osg::Group);
        osg::Group* mergeGroup = v312MergeGroups.front().get();
        struct AssetPartition
        {
            osg::ref_ptr<osg::Group> control = new osg::Group;
            osg::ref_ptr<osg::Group> source = new osg::Group;
            std::string identity;
            unsigned eligible = 0;
            unsigned materialOwner = 0;
            std::size_t instances = 0;
            std::vector<std::pair<osg::ref_ptr<osg::Node>,osg::Group*>> originalDestinations;
        };
        std::map<std::tuple<int,int,int,unsigned>, AssetPartition> assetPartitions;
        std::map<const osg::Group*, AssetPartition*> assetControls;
        SceneUtil::GeneratedStaticLod::EffectiveMaterialOwners assetMaterialOwners;
        std::size_t assetPrivateBytes = 0, assetInstances = 0;
        std::uint64_t assetDerivedHostBytes = 0;
        struct PrivateBuffers final : osg::NodeVisitor
        {
            PrivateBuffers() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
            void apply(osg::Geometry& geometry) override
            {
                osg::ref_ptr<osg::VertexBufferObject> vertex = new osg::VertexBufferObject;
                for (auto* array : {geometry.getVertexArray(), geometry.getNormalArray(), geometry.getColorArray()})
                    if (array) array->setVertexBufferObject(vertex);
                for (unsigned i = 0; i < geometry.getNumTexCoordArrays(); ++i)
                    if (auto* array = geometry.getTexCoordArray(i)) array->setVertexBufferObject(vertex);
                osg::ref_ptr<osg::ElementBufferObject> elements = new osg::ElementBufferObject;
                for (unsigned i = 0; i < geometry.getNumPrimitiveSets(); ++i)
                    if (auto* draw = geometry.getPrimitiveSet(i)->getDrawElements()) draw->setElementBufferObject(elements);
            }
        } privateBuffers;
        std::unique_ptr<Resource::SpeculativeScope::Stage> assetSourceAdmission;
        if (assetPreparation && !assetCertifiedSources.empty())
        {
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
            // Exact source snapshots have completed and unwound. The native
            // fixture can now stress private-copy admission independently.
            if (mFixtureBeforePrivate) mFixtureBeforePrivate();
#endif
            unsigned eligible = 0;
            for (const auto& [node,source] : assetCertifiedSources) eligible |= source.eligible;
            assetSourceAdmission = SceneUtil::AssetPaging::reservePrivateSources(eligible,assetOutcome);
        }
        auto assetReadiness = std::make_shared<SceneUtil::GeneratedStaticLod::Readiness>();
        osg::ref_ptr<Resource::TemplateMultiRef> templateRefs = new Resource::TemplateMultiRef;
        osgUtil::StateToCompile stateToCompile(0, nullptr);
        CopyOp copyop(activeGrid, copyMask);

        const bool buildOccluders = Settings::camera().mOcclusionCulling && Settings::camera().mOcclusionCullingStatics;
        osg::ref_ptr<PagedOccluderData> pagedOccluderData;
        float occluderMinRadius = 0;
        int occluderMeshRes = 6;
        int occluderMaxMeshRes = 24;
        float occluderShrinkFactor = 0.9f;
        if (buildOccluders || mV35CoarseChunkOcclusion)
            pagedOccluderData = new PagedOccluderData;
        if (buildOccluders)
        {
            occluderMinRadius = Settings::camera().mOcclusionOccluderMinRadius;
            occluderMeshRes = Settings::camera().mOcclusionOccluderMeshResolution;
            occluderMaxMeshRes = Settings::camera().mOcclusionOccluderMaxMeshResolution;
            occluderShrinkFactor = Settings::camera().mOcclusionOccluderShrinkFactor;
        }

        std::size_t v36RepeatedGroups = 0;
        std::size_t v36RepeatedInstances = 0;
        std::size_t v36TotalInstances = 0;
        std::size_t v36MergeCandidateGroups = 0;
        std::size_t p3CompatibleIndexMerges = 0;
        std::size_t p3DisplayListPromotions = 0;
        std::size_t p3NormalizedColorStreams = 0;
        std::size_t p3ShadowCandidateDrawables = 0;
        std::size_t p3ShadowEligibleDrawables = 0;
        std::size_t p3ShadowRejectedState = 0;
        std::size_t p3ShadowRejectedGeometry = 0;
        std::size_t p3ShadowProxyBatches = 0;
        std::size_t p3ShadowSourceIndices = 0;
        bool p3SemanticPremergeUsed = false;
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_build_instances", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& pair : nodes)
            {
                SceneUtil::PagingWorkScope::checkpoint();
                const osg::Node* cnode = pair.first;

            const AnalyzeVisitor::Result& analyzeResult = pair.second.mAnalyzeResult;

            const int v38ConfiguredBatchingMode
                = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const bool v39OnDemandFallback
                = static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !compile;
            const int v38BatchingMode
                = v39OnDemandFallback ? std::min(v38ConfiguredBatchingMode, 1) : v38ConfiguredBatchingMode;
            const unsigned int v38InstanceCount = static_cast<unsigned int>(pair.second.mInstances.size());
            float v38MergeMultiplier = 1.f;
            if (v38BatchingMode > 0)
            {
                const float configuredMultiplier
                    = static_cast<float>(Settings::cells().mV38WorldBatchingMergeMultiplier);
                // Distant chunks are the highest-payoff population and are immutable.
                // Keep active-grid pressure lower because those chunks also carry refnum
                // interaction bookkeeping.
                if (!activeGrid)
                    v38MergeMultiplier = configuredMultiplier
                        * (v38BatchingMode == 1 ? 1.f : (v38BatchingMode == 2 ? 1.75f : 3.f));
                else if (v38BatchingMode >= 2)
                    v38MergeMultiplier = v38BatchingMode == 2 ? 1.15f : 1.5f;
            }

            const float mergeCost = analyzeResult.mNumVerts * size;
            const float mergeBenefit
                = analyzeVisitor.getMergeBenefit(analyzeResult) * mMergeFactor * v38MergeMultiplier;
            const bool v38RepeatedCandidate = v38BatchingMode >= 2
                && v38InstanceCount >= static_cast<unsigned int>(Settings::cells().mV38WorldBatchingMinInstances);
            const bool v38ForceMerge = (v38BatchingMode >= 3 && !activeGrid)
                || (v38RepeatedCandidate && (!activeGrid || v38BatchingMode >= 3));
            const bool merge = mergeBenefit > mergeCost || v38ForceMerge;

            const float factor2
                = mergeBenefit > 0 ? std::min(1.f, mergeCost * mMinSizeCostMultiplier / mergeBenefit) : 1;
            const float minSizeMergeFactor2 = (1 - factor2) * mMinSizeMergeFactor + factor2;
            const float minSizeMerged = minSizeMergeFactor2 > 0 ? mMinSize * minSizeMergeFactor2 : mMinSize;

            unsigned int numinstances = 0;
            for (const PagedCellRef* refPtr : pair.second.mInstances)
            {
                const PagedCellRef& ref = *refPtr;
                if (!activeGrid && minSizeMerged != minSize
                    && cnode->getBound().radius2() * ref.mScale * ref.mScale
                        < (viewPoint - ref.mPosition).length2() * minSizeMerged * minSizeMerged)
                    continue;

                const osg::Vec3f nodePos = ref.mPosition - worldCenter;
                const osg::Quat nodeAttitude = osg::Quat(ref.mRotation.z(), osg::Vec3f(0, 0, -1))
                    * osg::Quat(ref.mRotation.y(), osg::Vec3f(0, -1, 0))
                    * osg::Quat(ref.mRotation.x(), osg::Vec3f(-1, 0, 0));
                const osg::Vec3f nodeScale(ref.mScale, ref.mScale, ref.mScale);
                const auto assetSource = assetCertifiedSources.find(cnode);
                const bool assetHlodEligible = assetSource != assetCertifiedSources.end() && (assetSource->second.eligible & SceneUtil::AssetPaging::Hlod);
                const auto assetKey = std::make_tuple(static_cast<int>(std::floor(nodePos.x() / 2048.f)),
                    static_cast<int>(std::floor(nodePos.y() / 2048.f)),
                    static_cast<int>(std::floor(nodePos.z() / 2048.f)),
                    assetSource == assetCertifiedSources.end() ? 0u : assetSource->second.eligible);
                const std::size_t assetCopyBytes = assetSource == assetCertifiedSources.end() ? 0
                    : assetSource->second.hostBytes * (assetHlodEligible ? 2 : 1);
                bool providedOccluderOwner = false;
#ifdef OPENMW_ASSET_PAGING_TEST_HOOKS
                if (providedPopulation) providedOccluderOwner = providedReferences.at(ref.mRefNum)->occluderOwner;
#endif
                bool assetCandidate = !providedOccluderOwner && assetSourceAdmission && ref.mScale > 0.f && assetCopyBytes > 0
                    && assetInstances < 64 && (assetPartitions.contains(assetKey) || assetPartitions.size() < 16)
                    && assetCopyBytes <= 32 * Resource::SpeculativeBudget::MiB - assetPrivateBytes
                    // Retain the original owner and MSOC representation intact.
                    && !(buildOccluders && cnode->getBound().radius() * ref.mScale >= occluderMinRadius);
                if (assetSource != assetCertifiedSources.end() && assetSourceAdmission && !assetCandidate)
                    SceneUtil::AssetPaging::refuse(assetSource->second.eligible,
                        providedOccluderOwner || (buildOccluders && cnode->getBound().radius() * ref.mScale >= occluderMinRadius)
                            ? SceneUtil::AssetPaging::Gate::Occluder : SceneUtil::AssetPaging::Gate::Population);
                if (assetCandidate)
                    assetCandidate = SceneUtil::AssetPaging::admitPrivateSource(assetSource->second,assetOutcome,
                        *assetSourceAdmission,assetCopyBytes);

                osg::ref_ptr<osg::Group> trans;
                if (merge || assetCandidate)
                {
                    // Optimizer currently supports only MatrixTransforms.
                    osg::Matrixf matrix;
                    matrix.preMultTranslate(nodePos);
                    matrix.preMultRotate(nodeAttitude);
                    matrix.preMultScale(nodeScale);
                    trans = new osg::MatrixTransform(matrix);
                    trans->setDataVariance(osg::Object::STATIC);
                }
                else
                {
                    trans = new SceneUtil::PositionAttitudeTransform;
                    SceneUtil::PositionAttitudeTransform* pat
                        = static_cast<SceneUtil::PositionAttitudeTransform*>(trans.get());
                    pat->setPosition(nodePos);
                    pat->setScale(nodeScale);
                    pat->setAttitude(nodeAttitude);
                }

                // DO NOT COPY AND PASTE THIS CODE. Cloning osg::Geometry without also cloning its contained Arrays is
                // generally unsafe. In this specific case the operation is safe under the following two assumptions:
                // - When Arrays are removed or replaced in the cloned geometry, the original Arrays in their place must
                // outlive the cloned geometry regardless. (ensured by TemplateMultiRef)
                // - Arrays that we add or replace in the cloned geometry must be explicitely forbidden from reusing
                // BufferObjects of the original geometry. (ensured by needvbo() in optimizer.cpp)
                copyop.setCopyFlags(merge ? osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES
                                          : osg::CopyOp::DEEP_COPY_NODES);
                if (assetCandidate)
                    copyop.setCopyFlags(osg::CopyOp::DEEP_COPY_NODES | osg::CopyOp::DEEP_COPY_DRAWABLES
                        | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES);
                copyop.mDistances = lodDistances / ref.mScale;
                copyop.copy(cnode, trans);
                if (assetCandidate) trans->accept(privateBuffers);

                // Build occluder mesh for building-sized objects. Cache the simplified proxy
                // in model-local space so repeated instances/chunks and future sessions can reuse it.
                if (buildOccluders && !assetCandidate)
                {
                    float scaledRadius = cnode->getBound().radius() * ref.mScale;
                    if (scaledRadius >= occluderMinRadius)
                    {
                        // Scale grid resolution with object size so grid cell size stays ~constant.
                        // A small building uses base resolution; very large structures get more detail.
                        int adaptiveRes = occluderMeshRes;
                        if (scaledRadius > occluderMinRadius)
                        {
                            float scale = scaledRadius / occluderMinRadius;
                            adaptiveRes = std::clamp(
                                static_cast<int>(occluderMeshRes * scale), occluderMeshRes, occluderMaxMeshRes);
                        }

                        const int shrinkKey = OcclusionStorage::makeShrinkKey(occluderShrinkFactor);
                        const int scaleKey = static_cast<int>(ref.mScale * 1000.f + (ref.mScale >= 0.f ? 0.5f : -0.5f));
                        std::string cacheKey = pair.second.mModel.value();
                        cacheKey += "|paged|";
                        cacheKey += activeGrid ? "active" : "distant";
                        cacheKey += "|lod=" + std::to_string(static_cast<unsigned int>(lod));
                        cacheKey += "|scale=" + std::to_string(scaleKey);

                        OccluderMesh localMesh;
                        bool cacheHit = false;
                        if (mOcclusionStorage && mOcclusionStorage->isOpen() && !cacheKey.empty())
                            cacheHit = mOcclusionStorage->get(cacheKey, adaptiveRes, shrinkKey, localMesh);

                        if (!cacheHit)
                        {
                            if (mOcclusionStorage && mOcclusionStorage->isOpen())
                                mOcclusionStorage->recordMiss();

                            // Reproduce the same CopyOp filtering/LOD selection as the rendered paged object,
                            // but omit this particular instance transform so the result is reusable.
                            osg::ref_ptr<osg::Group> localProxySource = new osg::Group;
                            copyop.copy(cnode, localProxySource);
                            localMesh = buildSimplifiedMesh(localProxySource, adaptiveRes, occluderShrinkFactor);

                            if (mOcclusionStorage && mOcclusionStorage->isOpen() && !cacheKey.empty())
                                mOcclusionStorage->put(cacheKey, adaptiveRes, shrinkKey, localMesh);
                        }

                        if (!localMesh.indices.empty())
                        {
                            // Apply this reference's transform to the cached local-space proxy, then
                            // offset the chunk-relative position into world space.
                            osg::Matrixf localToChunk = osg::Matrixf::identity();
                            localToChunk.preMultTranslate(nodePos);
                            localToChunk.preMultRotate(nodeAttitude);
                            localToChunk.preMultScale(nodeScale);

                            OccluderMesh occMesh;
                            occMesh.indices = localMesh.indices;
                            occMesh.vertices.reserve(localMesh.vertices.size());
                            for (const auto& v : localMesh.vertices)
                            {
                                osg::Vec3f worldVertex = v * localToChunk;
                                worldVertex += worldCenter;
                                occMesh.vertices.push_back(worldVertex);
                                occMesh.aabb.expandBy(worldVertex);
                            }
                            pagedOccluderData->mOccluderMeshes.push_back(std::move(occMesh));
                        }
                    }
                }

                if (activeGrid)
                {
                    if (merge)
                    {
                        AddRefnumMarkerVisitor visitor(ref.mRefNum);
                        trans->accept(visitor);
                    }
                    else
                    {
                        osg::ref_ptr<RefnumMarker> marker = new RefnumMarker;
                        marker->mRefnum = ref.mRefNum;
                        trans->getOrCreateUserDataContainer()->addUserObject(marker);
                    }
                }

                osg::Group* attachTo = group;
                if (merge)
                {
                    if (v312SpatialPrepared)
                    {
                        const unsigned int cluster = (nodePos.x() >= 0.f ? 1u : 0u)
                            | (nodePos.y() >= 0.f ? 2u : 0u);
                        attachTo = v312MergeGroups[cluster].get();
                    }
                    else if (v315PacketizedPrepared)
                    {
                        unsigned int cluster = 0;
                        if (v315PacketizedPremergeMode <= 1)
                        {
                            cluster = (nodePos.x() >= 0.f ? 1u : 0u)
                                | (nodePos.y() >= 0.f ? 2u : 0u);
                        }
                        else
                        {
                            const float chunkWorldSize = size * static_cast<float>(cellSize);
                            const float halfChunkWorldSize = chunkWorldSize * 0.5f;
                            const float normalizedX = chunkWorldSize > 0.f
                                ? std::clamp((nodePos.x() + halfChunkWorldSize) / chunkWorldSize, 0.f, 0.999999f)
                                : 0.f;
                            const float normalizedY = chunkWorldSize > 0.f
                                ? std::clamp((nodePos.y() + halfChunkWorldSize) / chunkWorldSize, 0.f, 0.999999f)
                                : 0.f;
                            const unsigned int x = static_cast<unsigned int>(normalizedX * 4.f);
                            const unsigned int y = static_cast<unsigned int>(normalizedY * 4.f);
                            cluster = x | (y << 2u);
                        }
                        attachTo = v312MergeGroups[cluster].get();
                    }
                    else
                        attachTo = mergeGroup;
                }
                if (assetCandidate)
                {
                    // Bounded spatial partitions preserve original membership and avoid a
                    // chunk-wide atlas bound. No game node is reparented.
                    auto& partition = assetPartitions[assetKey];
                    if (!partition.materialOwner) partition.materialOwner = 1u << assetPartitions.size();
                    partition.control->addChild(trans);
                    partition.originalDestinations.emplace_back(trans,attachTo);
                    ++partition.instances;
                    partition.identity += assetSource->second.identity + "|";
                    assetPrivateBytes += assetCopyBytes; ++assetInstances;
                    partition.eligible = assetSource->second.eligible;
                    if (assetHlodEligible)
                        partition.source->addChild(osg::clone(trans.get(), osg::CopyOp::DEEP_COPY_NODES
                            | osg::CopyOp::DEEP_COPY_DRAWABLES | osg::CopyOp::DEEP_COPY_ARRAYS | osg::CopyOp::DEEP_COPY_PRIMITIVES));
                }
                else attachTo->addChild(trans);
                if (assetSourceAdmission)
                {
                    const unsigned owner = assetCandidate ? assetPartitions.at(assetKey).materialOwner : 1u;
                    // Read the exact copied population, including the leaf's
                    // own material and branch-local inherited state. The
                    // historical AnalyzeVisitor remains only a merge heuristic.
                    assetMaterialOwners.observe(*trans,owner,copyMask);
                }
                ++numinstances;
            }
            if (numinstances > 0)
            {
                v36TotalInstances += numinstances;
                if (numinstances > 1)
                {
                    ++v36RepeatedGroups;
                    v36RepeatedInstances += numinstances;
                }
                if (merge)
                    ++v36MergeCandidateGroups;
                // add a ref to the original template to help verify the safety of shallow cloning operations
                // in addition, we hint to the cache that it's still being used and should be kept in cache
                templateRefs->addRef(cnode);

                if (pair.second.mNeedCompile)
                {
                    int mode = osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES;
                    if (!merge)
                        mode |= osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                    stateToCompile._mode = mode;
                    const_cast<osg::Node*>(cnode)->accept(stateToCompile);
                }
                }
            }
        }

        // Preserve existing strong same-material merging. L1 may reunite a
        // complete candidate material family across spatial buckets, but only
        // inside one exact original merge destination. All cross-destination,
        // mixed-lane, H1 and ordinary-owner conflicts retain control topology.
        // This avoids turning repeated rocks into extra control submissions.
        unsigned conflictingAssetOwners = 0;
        if (!assetPartitions.empty())
        {
            using OwnershipCounters = SceneUtil::GeneratedStaticLod::Counters;
            if (!assetMaterialOwners.report().conflicting || assetMaterialOwners.report().shared)
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& material : assetMaterialOwners.materials())
                    {
                        if ((material.owners & 1u) || !(material.owners & (material.owners - 1))) continue;
                        std::vector<decltype(assetPartitions.begin())> family;
                        osg::Group* destination = nullptr;
                        bool eligible = true;
                        for (auto partition = assetPartitions.begin(); partition != assetPartitions.end(); ++partition)
                            if (partition->second.materialOwner & material.owners)
                            {
                                const auto& value = partition->second;
                                if (value.eligible != SceneUtil::AssetPaging::Lod || value.originalDestinations.empty())
                                { eligible = false; break; }
                                for (const auto& original : value.originalDestinations)
                                {
                                    if (!destination) destination = original.second;
                                    if (destination != original.second) eligible = false;
                                }
                                family.push_back(partition);
                            }
                        if (!eligible || family.size() < 2) continue;
                        auto& retained = family.front()->second;
                        for (std::size_t i = 1; i < family.size(); ++i)
                        {
                            auto& joined = family[i]->second;
                            while (joined.control->getNumChildren())
                            {
                                osg::ref_ptr<osg::Node> child = joined.control->getChild(0);
                                joined.control->removeChild(0u,1u); retained.control->addChild(child);
                            }
                            retained.originalDestinations.insert(retained.originalDestinations.end(),
                                joined.originalDestinations.begin(),joined.originalDestinations.end());
                            retained.instances += joined.instances; retained.identity += joined.identity;
                            assetMaterialOwners.replaceOwner(joined.materialOwner,retained.materialOwner);
                            ++OwnershipCounters::materialOwnerCohesions;
                            OwnershipCounters::materialOwnerCohesiveInstances += joined.instances;
                            assetPartitions.erase(family[i]);
                        }
                        changed = true;
                        break; // Ownership masks changed; restart bounded graph connectivity.
                    }
                }
            }
            const auto ownership = assetMaterialOwners.report();
            conflictingAssetOwners = ownership.conflicting;
            OwnershipCounters::materialOwnerUnknown += ownership.unknown;
            OwnershipCounters::materialOwnerNull += ownership.nullMaterial;
            OwnershipCounters::materialOwnerCapacity += ownership.capacity;
            OwnershipCounters::materialOwnerShared += ownership.shared;
            if (conflictingAssetOwners)
            {
                static std::atomic_uint samples{0};
                if (samples.fetch_add(1,std::memory_order_relaxed) < 8)
                    Log(Debug::Info) << "Asset material ownership refusal unknown=" << ownership.unknown
                        << " null=" << ownership.nullMaterial << " capacity=" << ownership.capacity
                        << " shared_family=" << ownership.shared << " first_owners=" << ownership.firstOwners
                        << " second_owners=" << ownership.secondOwners << " refused_owners=" << ownership.conflicting;
            }
            for (auto partition = assetPartitions.begin(); partition != assetPartitions.end();)
            {
                if (partition->second.materialOwner & conflictingAssetOwners)
                {
                    SceneUtil::AssetPaging::each(partition->second.eligible,[&](auto& counters) {
                        counters.materialConflictInstances += partition->second.instances;
                    });
                    SceneUtil::AssetPaging::refuse(partition->second.eligible,SceneUtil::AssetPaging::Gate::Material);
                    for (const auto& [instance,destination] : partition->second.originalDestinations)
                        destination->addChild(instance);
                    partition = assetPartitions.erase(partition);
                }
                else
                {
                    // Restoration is settled before flattening. Retaining a
                    // replaced transform here keeps its obsolete child-parent
                    // links alive: the optimizer then rediscovers that orphan
                    // and reports a removal again without a live parent.
                    // Keep only population counts for retained host accounting.
                    partition->second.originalDestinations.clear();
                    ++partition;
                }
            }
        }

        if (v315PacketizedPrepared)
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "v315_packet_premerge", activeGrid ? "active_grid" : "distant", 0.1);
            const osg::Vec3f v315RelativeViewPoint = viewPoint - worldCenter;
            const bool v315CanonicalizePackets
                = static_cast<bool>(Settings::cells().mV315PremergeStateCanonicalization);

            for (const osg::ref_ptr<osg::Group>& packetRef : v312MergeGroups)
            {
                osg::Group* const packet = packetRef.get();
                if (!packet->getNumChildren())
                    continue;

                if (v315CanonicalizePackets)
                    mSceneManager->shareState(packet);

                SceneUtil::Optimizer packetOptimizer;
                if (size > 1 / 8.f)
                {
                    packetOptimizer.setViewPoint(v315RelativeViewPoint);
                    packetOptimizer.setMergeAlphaBlending(true);
                }
                packetOptimizer.setIsOperationPermissibleForObjectCallback(new CanOptimizeCallback);
                constexpr unsigned int v315PacketOptions = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
                    | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;
                packetOptimizer.optimize(packet, v315PacketOptions);
            }

            // Recombine every temporary packet before the final Mode79-quality
            // optimizer. No packet survives as final render topology.
            osg::ref_ptr<osg::Group> recombined = new osg::Group;
            for (const osg::ref_ptr<osg::Group>& packetRef : v312MergeGroups)
            {
                osg::Group* const packet = packetRef.get();
                while (packet->getNumChildren() > 0)
                {
                    osg::ref_ptr<osg::Node> child = packet->getChild(0);
                    packet->removeChild(0u, 1u);
                    recombined->addChild(child);
                }
            }
            v312MergeGroups.clear();
            v312MergeGroups.emplace_back(recombined);
            mergeGroup = recombined.get();
        }

        Debug::V36StructureTrace::StructureStats v36BeforeStats;
        if (v36StructureEnabled)
        {
            v36BeforeStats += Debug::V36StructureTrace::inspect(*group);
            for (const osg::ref_ptr<osg::Group>& v312MergeGroupRef : v312MergeGroups)
                v36BeforeStats += Debug::V36StructureTrace::inspect(*v312MergeGroupRef);
        }

        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;
        for (auto& [key, partition] : assetPartitions)
        {
            v312MergeGroups.push_back(partition.control);
            assetControls.emplace(partition.control.get(), &partition);
        }

        for (const osg::ref_ptr<osg::Group>& v312MergeGroupRef : v312MergeGroups)
        {
            osg::Group* const mergeGroup = v312MergeGroupRef.get();
            if (!mergeGroup->getNumChildren())
                continue;
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_merge_optimize", activeGrid ? "active_grid" : "distant", 0.1);
            SceneUtil::Optimizer optimizer;
            if (size > 1 / 8.f)
            {
                optimizer.setViewPoint(relativeViewPoint);
                optimizer.setMergeAlphaBlending(true);
            }
            optimizer.setIsOperationPermissibleForObjectCallback(new CanOptimizeCallback);

            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const bool p2RequiredReadiness = SceneUtil::PagingWorkScope::requiredReadiness();
            const bool p3OptionalDistant
                = !activeGrid && compile && SceneUtil::PagingWorkScope::optionalOptimization();
            const bool p3SubmissionCompaction
                = static_cast<bool>(Settings::cells().mOptimizedMWSubmissionCompaction)
                && compile && !p2RequiredReadiness && v38BatchingMode >= 2;
            const bool p3DistantDisplayLists
                = static_cast<bool>(Settings::cells().mOptimizedMWDistantDisplayLists)
                && p3OptionalDistant && !static_cast<bool>(Settings::stereo().mMultiview);
            const bool p3NormalizedStaticPackets
                = static_cast<bool>(Settings::cells().mOptimizedMWNormalizedStaticPackets)
                && p3OptionalDistant && v38BatchingMode >= 2;
            optimizer.setMergeCompatibleIndexTypes(p3SubmissionCompaction);
            optimizer.setPreferDisplayListsForMergedGeometry(p3DistantDisplayLists);
            optimizer.setNormalizeIgnoredVertexColors(p3NormalizedStaticPackets);
            unsigned int options = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
                | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;

            const int v39ConfiguredBatchOptimizerMode
                = static_cast<int>(Settings::cells().mV39BatchOptimizerMode);
            const int v39BatchOptimizerMode
                = (static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !compile)
                ? 1
                : v39ConfiguredBatchOptimizerMode;
            const bool v310PreloadPostTransform
                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)
                && compile && !p2RequiredReadiness && mV310InitialFrontloadActive.load(std::memory_order_acquire)
                && v38BatchingMode >= 2;
            const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);
            const bool v311PreparedActive
                = v311PrepareMode > 0 && compile && !p2RequiredReadiness && activeGrid && v38BatchingMode >= 2;
            const bool v311PreparedPostTransform = v311PrepareMode >= 2 && v311PreparedActive;

            if (v39BatchOptimizerMode == 0 && !v310PreloadPostTransform && !v311PreparedActive)
            {
                // Exact V3.8 behavior for rollback/A-B comparison.
                if (v38BatchingMode >= 2)
                    options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
                if (v38BatchingMode >= 3)
                    options |= SceneUtil::Optimizer::VERTEX_PRETRANSFORM;
            }
            else if ((v39BatchOptimizerMode >= 3 || v310PreloadPostTransform || v311PreparedPostTransform)
                && v38BatchingMode >= 2)
            {
                // V3.10 may promote startup work; V3.11 may promote only exact
                // compile=true active-grid preparation. Neither path requests
                // VERTEX_PRETRANSFORM.
                options |= SceneUtil::Optimizer::VERTEX_POSTTRANSFORM;
            }

            // Required terrain readiness must still flatten/merge and queue GL
            // compilation, but must not depend on expensive vertex reordering.
            // The optional second pass requests the normal strong quality.
            if (p2RequiredReadiness)
                options &= ~(SceneUtil::Optimizer::VERTEX_POSTTRANSFORM | SceneUtil::Optimizer::VERTEX_PRETRANSFORM);

            const bool v315CanonicalizeBeforeMerge
                = static_cast<bool>(Settings::cells().mV315PremergeStateCanonicalization)
                && compile && v38BatchingMode >= 2;
            const bool p3SemanticPremerge
                = static_cast<bool>(Settings::cells().mOptimizedMWSemanticPremerge)
                && p3OptionalDistant && v38BatchingMode >= 2;
            if (v315CanonicalizeBeforeMerge || p3SemanticPremerge)
            {
                mSceneManager->shareState(mergeGroup);
                p3SemanticPremergeUsed = p3SemanticPremerge;
            }

            SceneUtil::PagingWorkScope::checkpoint();
            optimizer.optimize(mergeGroup, options);
            p3CompatibleIndexMerges += optimizer.getCompatibleIndexMergeCount();
            p3DisplayListPromotions += optimizer.getDisplayListPromotionCount();
            p3NormalizedColorStreams += optimizer.getNormalizedColorStreamCount();
            SceneUtil::PagingWorkScope::checkpoint();

            const bool v39ShareState
                = v39BatchOptimizerMode >= 2 || v310PreloadPostTransform || v311PreparedActive;
            if ((v39BatchOptimizerMode == 0 && v38BatchingMode >= 2) || v39ShareState)
                mSceneManager->shareState(mergeGroup);

            if (SceneUtil::GpuIndirectPackets::indirectEnabled())
                for (unsigned i = 0; i < mergeGroup->getNumChildren(); ++i)
                    if (auto* geometry = dynamic_cast<osg::Geometry*>(mergeGroup->getChild(i)))
                        SceneUtil::GpuIndirectPackets::install(*geometry);
            osg::ref_ptr<osg::Node> wrappedMergeNode = assetControls.contains(mergeGroup)
                ? osg::ref_ptr<osg::Node>(mergeGroup) : SceneUtil::PersistentOpaquePackets::build(*mergeGroup);
            SceneUtil::GeneratedStaticLod::Result assetLod;
            SceneUtil::HlodBuildResult assetHlod;
            if (const auto asset = assetControls.find(mergeGroup); asset != assetControls.end())
            {
                auto& partition = *asset->second;
                auto tiers = SceneUtil::AssetPaging::buildTiers(*partition.source,*mergeGroup,partition.identity,
                    partition.eligible,mAssetMainCamera.get(),assetOutcome);
                assetLod = std::move(tiers.lod);
                assetHlod = std::move(tiers.hlod);
                if (assetLod.node) assetDerivedHostBytes += assetLod.derivedIndices * sizeof(unsigned)
                    + 8192ull * partition.instances + 8192;
                if (assetHlod) assetDerivedHostBytes += assetHlod.atlasBytes
                    + 2ull * (assetHlod.sourceVertices * 64ull + assetHlod.sourceTriangles * 3ull * sizeof(unsigned))
                    + 8192ull * partition.instances + 8192;
            }

            osg::ref_ptr<osg::Group> p3ShadowRoot;
            const bool p3ShadowStaticBatching
                = static_cast<bool>(Settings::cells().mOptimizedMWShadowStaticBatching)
                && (p3OptionalDistant || (activeGrid && Settings::cells().mOptimizedMWActiveShadowBatching
                    && compile && !p2RequiredReadiness && SceneUtil::PagingWorkScope::optionalOptimization()));
            if (p3ShadowStaticBatching)
            {
                P3ShadowBatchResult shadowBatch = p3BuildShadowBatch(*mergeGroup);
                p3ShadowCandidateDrawables += shadowBatch.mCandidateDrawables;
                p3ShadowEligibleDrawables += shadowBatch.mEligibleDrawables;
                p3ShadowRejectedState += shadowBatch.mRejectedState;
                p3ShadowRejectedGeometry += shadowBatch.mRejectedGeometry;
                p3ShadowSourceIndices += shadowBatch.mSourceIndices;
                if (shadowBatch.mShadowRoot)
                {
                    ++p3ShadowProxyBatches;
                    p3ShadowRoot = shadowBatch.mShadowRoot;
                }
            }

            // Shadow certification consumed the ordinary optimized control above.
            // Only the original-before-CopyOp certified private asset partitions
            // may recursively attach P1 across retained material-bearing Geodes.
            if (assetControls.contains(mergeGroup))
            {
                wrappedMergeNode = SceneUtil::PersistentOpaquePackets::buildCertifiedSubtrees(*mergeGroup);
                if (assetLod.node)
                {
                    auto derived = SceneUtil::PersistentOpaquePackets::buildCertifiedSubtrees(*assetLod.node->derived());
                    wrappedMergeNode = new SceneUtil::GeneratedStaticLod::Group(*wrappedMergeNode,
                        *derived, assetLod.node->error(), assetReadiness, mAssetMainCamera.get());
                }
                if (assetHlod)
                {
                    auto derived = SceneUtil::PersistentOpaquePackets::buildCertifiedSubtrees(*assetHlod.representation);
                    wrappedMergeNode = new SceneUtil::GeneratedStaticLod::Group(*wrappedMergeNode,
                        *derived, mergeGroup->getBound().radius() * .02f, assetReadiness, mAssetMainCamera.get(), true);
                }
            }
            if (p3ShadowRoot) wrappedMergeNode = new SceneUtil::ShadowProxyGroup(wrappedMergeNode, p3ShadowRoot);

            group->addChild(wrappedMergeNode);

            if (mDebugBatches)
            {
                DebugVisitor dv;
                mergeGroup->accept(dv);
            }
            if (compile)
            {
                stateToCompile._mode = osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                if (assetControls.contains(mergeGroup))
                    stateToCompile._mode |= osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES;
                wrappedMergeNode->accept(stateToCompile);
                if (p3ShadowRoot)
                    p3ShadowRoot->accept(stateToCompile);
            }
        }
        }

        SceneUtil::PagingWorkScope::checkpoint();
        // Pending compile sets and losing candidates still own these arrays.
        // Attach their retained host receipt before exposing the graph to ICO.
        SceneUtil::AssetPaging::retainHostOwner(*group,assetPrivateBytes + assetDerivedHostBytes);
        osgUtil::IncrementalCompileOperation* const ico = mSceneManager->getIncrementalCompileOperation();
        if (!stateToCompile.empty() && ico && !ico->getContextSet().empty())
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_compile_map", activeGrid ? "active_grid" : "distant", 0.1);
            // Classification is metadata only unless a scheduler/fairness policy
            // consumes it. Always retain the producer identity so GL-P4 can
            // prioritize without enabling the older CP2 completion policy.
            auto compileSet = new Resource::V321ClassifiedCompileSet(
                group, Resource::V321CompileClass::ObjectPaging,
                activeGrid ? Resource::V321CompileUrgency::NearFuture
                           : Resource::V321CompileUrgency::Background);
            compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
            if (!assetPartitions.empty())
            {
                assetReadiness->assetMask = assetOutcome.completed;
                assetOutcome.readiness = assetReadiness;
                compileSet->_compileCompletedCallback = new SceneUtil::GeneratedStaticLod::CompileCompleted(assetReadiness);
            }
            ico->add(compileSet, false);
        }

        else if (assetOutcome.completed) assetOutcome.defer(assetOutcome.completed);
        assetOutcome.finish(requestedAssets);

        if (Debug::V3Diagnostics::renderWriter().enabled())
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                << ',' << Debug::V3Diagnostics::csvQuote("object_chunk_summary") << ',' << Debug::V3Diagnostics::csvQuote(
                    std::string(activeGrid ? "active" : "distant") + " refs=" + std::to_string(refs.size())
                    + " templates=" + std::to_string(nodes.size())
                    + " p3_index_merges=" + std::to_string(p3CompatibleIndexMerges)
                    + " p3_semantic_premerge=" + std::to_string(p3SemanticPremergeUsed ? 1 : 0)
                    + " p3_display_lists=" + std::to_string(p3DisplayListPromotions)
                    + " p3_normalized_colors=" + std::to_string(p3NormalizedColorStreams)
                    + " p3_shadow_candidates=" + std::to_string(p3ShadowCandidateDrawables)
                    + " p3_shadow_eligible=" + std::to_string(p3ShadowEligibleDrawables)
                    + " p3_shadow_rejected_state=" + std::to_string(p3ShadowRejectedState)
                    + " p3_shadow_rejected_geometry=" + std::to_string(p3ShadowRejectedGeometry)
                    + " p3_shadow_batches=" + std::to_string(p3ShadowProxyBatches)
                    + " p3_shadow_indices=" + std::to_string(p3ShadowSourceIndices)
                    + " p3_prefetch_models=" + std::to_string(p3TemplatePrefetchModels)
                    + " p3_prefetch_reuse_hits=" + std::to_string(p3TemplateReuseHits)
                    + " p3_prefetch_parallel=" + std::to_string(p3TemplatePrefetchParallel ? 1 : 0)
                    + " p3_prefetch_helpers=" + std::to_string(p3TemplatePrefetchHelpers)
                    + " p3_prefetch_required=" + std::to_string(p3TemplatePrefetchRequired ? 1 : 0))
                << ",0";
            Debug::V3Diagnostics::renderWriter().writeLine(row.str());
        }

        if (v36StructureEnabled)
        {
            const Debug::V36StructureTrace::StructureStats v36AfterStats
                = Debug::V36StructureTrace::inspect(*group);
            Debug::V36StructureTrace::writeChunk(activeGrid, size, lod, refs.size(), nodes.size(),
                v36RepeatedGroups, v36RepeatedInstances, v36TotalInstances, v36MergeCandidateGroups,
                v36BeforeStats, v36AfterStats, Debug::V3Diagnostics::elapsedMs(v36StructureStart));
        }

        group->getBound();
        if (mV35CoarseChunkOcclusion && pagedOccluderData)
        {
            osg::ComputeBoundsVisitor v35BoundsVisitor;
            group->accept(v35BoundsVisitor);
            pagedOccluderData->mChunkBounds = v35BoundsVisitor.getBoundingBox();
            pagedOccluderData->mEstimatedChildren = v36TotalInstances;
        }
        group->setNodeMask(Mask_Static);
        osg::UserDataContainer* udc = group->getOrCreateUserDataContainer();
        if (activeGrid)
        {
            std::sort(refnumSet->mRefnums.begin(), refnumSet->mRefnums.end());
            refnumSet->mRefnums.erase(
                std::unique(refnumSet->mRefnums.begin(), refnumSet->mRefnums.end()), refnumSet->mRefnums.end());
            udc->addUserObject(refnumSet);
            group->addCullCallback(new SceneUtil::LightListCallback);
        }
        udc->addUserObject(templateRefs);
        if (pagedOccluderData
            && (!pagedOccluderData->mOccluderMeshes.empty()
                || (mV35CoarseChunkOcclusion && pagedOccluderData->mChunkBounds.valid())))
        {
            udc->addUserObject(pagedOccluderData);
            if (mOcclusionCuller)
            {
                const float maxDist = Settings::camera().mOcclusionOccluderMaxDistance;
                group->addCullCallback(new PagedOccluderCallback(mOcclusionCuller, maxDist, mMaxTriangles));
            }
        }

        return group;
    }

    unsigned int ObjectPaging::getNodeMask()
    {
        return Mask_Static;
    }

    namespace
    {
        osg::Vec2f clampToCell(const osg::Vec3f& cellPos, const osg::Vec2i& cell)
        {
            return osg::Vec2f(std::clamp(cellPos.x(), static_cast<float>(cell.x()), cell.x() + 1.f),
                std::clamp(cellPos.y(), static_cast<float>(cell.y()), cell.y() + 1.f));
        }

        class CollectIntersecting
        {
        public:
            explicit CollectIntersecting(
                bool activeGridOnly, const osg::Vec3f& position, const osg::Vec2i& cell, ESM::RefId worldspace)
                : mActiveGridOnly(activeGridOnly)
                , mPosition(clampToCell(position / static_cast<float>(getCellSize(worldspace)), cell))
            {
            }

            void operator()(const ChunkId& id, osg::Object* /*obj*/)
            {
                if (mActiveGridOnly && !std::get<2>(id))
                    return;
                if (intersects(id))
                    mCollected.push_back(id);
            }

            const std::vector<ChunkId>& getCollected() const { return mCollected; }

        private:
            bool intersects(ChunkId id) const
            {
                const osg::Vec2f center = std::get<0>(id);
                const float halfSize = std::get<1>(id) / 2;
                return mPosition.x() >= center.x() - halfSize && mPosition.y() >= center.y() - halfSize
                    && mPosition.x() <= center.x() + halfSize && mPosition.y() <= center.y() + halfSize;
            }

            bool mActiveGridOnly;
            osg::Vec2f mPosition;
            std::vector<ChunkId> mCollected;
        };
    }

    bool ObjectPaging::enableObject(
        int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell, bool enabled)
    {
        if (!typeFilter(type, false))
            return false;

        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            if (enabled && !getWritableRefTracker().mDisabled.erase(refnum))
                return false;
            if (!enabled && !getWritableRefTracker().mDisabled.insert(refnum).second)
                return false;
            if (mRefTrackerLocked)
                return false;
        }

        CollectIntersecting ccf(false, pos, cell, mWorldspace);
        {
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            mAssetGeneration.fetch_add(1, std::memory_order_acq_rel);
            mCache->call(ccf);
            if (ccf.getCollected().empty()) return false;
            for (const ChunkId& chunk : ccf.getCollected()) mCache->removeFromObjectCache(chunk);
            for (const ChunkId& chunk : ccf.getCollected())
            {
                mV313ChunkQualities.erase(chunk);
                mV313StrongUpgradeInFlight.erase(chunk);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            for (const ChunkId& chunk : ccf.getCollected())
                mV311PreparedActiveChunks.erase(chunk);
        }
        return true;
    }

    bool ObjectPaging::blacklistObject(int type, ESM::RefNum refnum, const osg::Vec3f& pos, const osg::Vec2i& cell)
    {
        if (!typeFilter(type, false))
            return false;

        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            if (!getWritableRefTracker().mBlacklist.insert(refnum).second)
                return false;
            if (mRefTrackerLocked)
                return false;
        }

        CollectIntersecting ccf(true, pos, cell, mWorldspace);
        {
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            mAssetGeneration.fetch_add(1, std::memory_order_acq_rel);
            mCache->call(ccf);
            if (ccf.getCollected().empty()) return false;
            for (const ChunkId& chunk : ccf.getCollected()) mCache->removeFromObjectCache(chunk);
            for (const ChunkId& chunk : ccf.getCollected())
            {
                mV313ChunkQualities.erase(chunk);
                mV313StrongUpgradeInFlight.erase(chunk);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            for (const ChunkId& chunk : ccf.getCollected())
                mV311PreparedActiveChunks.erase(chunk);
        }
        return true;
    }

    void ObjectPaging::clear()
    {
        std::lock_guard<std::mutex> lock(mRefTrackerMutex);
        mRefTrackerNew.mDisabled.clear();
        mRefTrackerNew.mBlacklist.clear();
        mRefTrackerLocked = true;
        mAssetGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    void ObjectPaging::clearCache()
    {
        {
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            mAssetGeneration.fetch_add(1, std::memory_order_acq_rel);
            mCache->clear();
            mV313ChunkQualities.clear();
            mV313StrongUpgradeInFlight.clear();
        }
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.clear();
        }
    }

    bool ObjectPaging::unlockCache()
    {
        if (!mRefTrackerLocked)
            return false;
        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            mRefTrackerLocked = false;
            if (mRefTracker == mRefTrackerNew)
                return false;
            else
                mRefTracker = mRefTrackerNew;
        }
        clearCache();
        return true;
    }

    namespace
    {
        struct GetRefnumsFunctor
        {
            GetRefnumsFunctor(std::vector<ESM::RefNum>& output)
                : mOutput(output)
            {
            }
            void operator()(MWRender::ChunkId chunkId, osg::Object* obj)
            {
                if (!std::get<2>(chunkId))
                    return;
                const osg::Vec2f& center = std::get<0>(chunkId);
                const bool activeGrid = (center.x() > mActiveGrid.x() || center.y() > mActiveGrid.y()
                    || center.x() < mActiveGrid.z() || center.y() < mActiveGrid.w());
                if (!activeGrid)
                    return;

                osg::UserDataContainer* udc = obj->getUserDataContainer();
                if (udc && udc->getNumUserObjects())
                {
                    RefnumSet* refnums = dynamic_cast<RefnumSet*>(udc->getUserObject(0));
                    if (!refnums)
                        return;
                    mOutput.insert(mOutput.end(), refnums->mRefnums.begin(), refnums->mRefnums.end());
                }
            }
            osg::Vec4i mActiveGrid;
            std::vector<ESM::RefNum>& mOutput;
        };
    }

    void ObjectPaging::getPagedRefnums(const osg::Vec4i& activeGrid, std::vector<ESM::RefNum>& out)
    {
        GetRefnumsFunctor grf(out);
        grf.mActiveGrid = activeGrid;
        mCache->call(grf);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    void ObjectPaging::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        if (Resource::AssetPolicy::enabled(Resource::AssetPolicy::VisualLod)
            || Resource::AssetPolicy::enabled(Resource::AssetPolicy::Hlod))
        {
            using C = SceneUtil::GeneratedStaticLod::Counters;
            const auto report = [&](const std::string& name, const std::atomic_uint64_t& value) {
                stats->setAttribute(frameNumber, name, static_cast<double>(value.load(std::memory_order_relaxed)));
            };
            report("L1 Source Requests", C::requests); report("L1 Source Certified", C::certified);
            report("L1 Built Bundles", C::built); report("L1 Disk Hits", C::diskHits);
            report("L1 Original Indices", C::originalIndices); report("L1 Tier Indices", C::derivedIndices);
            report("Asset Main Derived Views", C::coarseViews); report("Asset Original Views", C::originalViews);
            report("L1 Consumed Draws", C::derivedDraws); report("L1 Consumed Indices", C::submittedIndices);
            for (unsigned i = 1; i < static_cast<unsigned>(SceneUtil::GeneratedStaticLod::Refusal::Count); ++i)
                report("L1 Refusal " + std::to_string(i), C::refused[i]);
            using H = SceneUtil::HlodCounters;
            report("H1 Requests", H::requests); report("H1 Certified", H::certified);
            report("H1 Built", H::built); report("H1 Refused", H::refused);
            report("H1 Control Submissions", H::controlSubmissions); report("H1 Proxy Submissions", H::proxySubmissions);
            report("H1 Atlas Bytes", H::atlasBytes); report("H1 Consumed Draws", H::consumedDraws);
            report("H1 Consumed Indices", H::consumedIndices);
        }
        if (SceneUtil::ShadowBatchCounters::enabled())
        {
            namespace C = SceneUtil::ShadowBatchCounters;
            const auto report = [&](const std::string& name, const std::atomic_uint64_t& value) {
                stats->setAttribute(frameNumber, name, static_cast<double>(value.load(std::memory_order_relaxed)));
            };
            report("P8G4 Shadow Built", C::built);
            report("P8G4 Shadow Wrapped", C::wrapped);
            report("P8G4 Shadow Built Indices", C::builtIndices);
            for (unsigned i = 0; i < C::proxyVisits.size(); ++i)
            {
                report("P8G4 Shadow Proxy Visits " + std::to_string(i), C::proxyVisits[i]);
                report("P8G4 Shadow Proxy Indices " + std::to_string(i), C::proxyIndices[i]);
                report("P8G4 Shadow Original Fallback " + std::to_string(i), C::normalFallback[i]);
            }
        }

        Resource::reportStats("Object Chunk", frameNumber, mCache->getStats(), *stats);
        stats->setAttribute(frameNumber, "V3.11 Prepared Active Built",
            static_cast<double>(mV311PreparedActiveBuilt.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.11 Prepared Active Hit",
            static_cast<double>(mV311PreparedActiveHits.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.11 Demand Fallback",
            static_cast<double>(mV311DemandFallbacks.load(std::memory_order_relaxed)));
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            stats->setAttribute(frameNumber, "V3.11 Prepared Active Resident",
                static_cast<double>(mV311PreparedActiveChunks.size()));
        }
        stats->setAttribute(frameNumber, "V3.13 Weak Cache Hit On Strong Prepare",
            static_cast<double>(mV313WeakCacheHitOnStrongPrepare.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.13 Upgrade Built",
            static_cast<double>(mV313UpgradeBuilt.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.13 Upgrade Installed",
            static_cast<double>(mV313UpgradeInstalled.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.13 Upgrade Coalesced",
            static_cast<double>(mV313UpgradeCoalesced.load(std::memory_order_relaxed)));
        {
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            stats->setAttribute(frameNumber, "V3.13 Quality Entries",
                static_cast<double>(mV313ChunkQualities.size()));
            stats->setAttribute(frameNumber, "V3.13 Upgrade In Flight",
                static_cast<double>(mV313StrongUpgradeInFlight.size()));
        }
    }

}
