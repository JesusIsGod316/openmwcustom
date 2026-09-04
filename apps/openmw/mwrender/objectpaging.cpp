#include "objectpaging.hpp"

#include <limits>

#include "occlusionculling.hpp"

#include <components/sceneutil/occlusionculling.hpp>

#include <unordered_map>
#include <vector>

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
#include <components/resource/v321classifiedcompileset.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
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
    }

    osg::ref_ptr<osg::Node> ObjectPaging::getChunk(float size, const osg::Vec2f& center, unsigned char /*lod*/,
        unsigned int lodFlags, bool activeGrid, const osg::Vec3f& viewPoint, bool compile)
    {
        if (activeGrid && !mActiveGrid)
            return nullptr;

        const ChunkId id = std::make_tuple(center, size, activeGrid);
        const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);
        const int v313QualityMode = static_cast<int>(Settings::cells().mV313ChunkQualityMode);

        // Mode0 is the inherited V3.12/V3.11 first-writer path. Keep it isolated
        // so all historical modes remain a valid behavioral/performance control.
        if (v313QualityMode == 0)
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
            osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
            mCache->addEntryToObjectCache(id, node.get());

            if (v311PrepareMode > 0 && activeGrid && compile)
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                mV311PreparedActiveChunks.insert(id);
                mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
            }
            return node;
        }

        const unsigned char v313RequestedPrepareMode
            = activeGrid && compile && v311PrepareMode > 0 ? static_cast<unsigned char>(v311PrepareMode) : 0;
        const unsigned char v313RequestedSpatialMode
            = activeGrid && compile && static_cast<int>(Settings::cells().mV312SpatialBatchMode) > 0 ? 1 : 0;
        const V313ChunkQuality v313RequestedQuality{ v313RequestedPrepareMode, v313RequestedSpatialMode };

        const auto v313QualitySatisfies = [&](const V313ChunkQuality& have, const V313ChunkQuality& need) {
            if (have.mPrepareMode < need.mPrepareMode)
                return false;
            if (v313QualityMode >= 2 && need.mPrepareMode > 0 && have.mSpatialMode != need.mSpatialMode)
                return false;
            return true;
        };

        osg::ref_ptr<osg::Object> cached = mCache->getRefFromObjectCache(id);
        bool v313RepairBuild = false;
        if (cached)
        {
            bool v313CachedSatisfies = true;
            if (v313QualityMode > 0 && v313RequestedPrepareMode > 0)
            {
                std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
                const auto it = mV313ChunkQualities.find(id);
                const V313ChunkQuality have = it != mV313ChunkQualities.end() ? it->second : V313ChunkQuality{};
                v313CachedSatisfies = v313QualitySatisfies(have, v313RequestedQuality);
                if (!v313CachedSatisfies)
                {
                    mV313WeakCacheHitOnStrongPrepare.fetch_add(1, std::memory_order_relaxed);
                    if (!mV313StrongUpgradeInFlight.insert(id).second)
                    {
                        mV313UpgradeCoalesced.fetch_add(1, std::memory_order_relaxed);
                        return static_cast<osg::Node*>(cached.get());
                    }
                    v313RepairBuild = true;
                }
            }

            if (v313CachedSatisfies)
            {
                if (v311PrepareMode > 0 && activeGrid && !compile)
                {
                    std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                    if (mV311PreparedActiveChunks.contains(id))
                        mV311PreparedActiveHits.fetch_add(1, std::memory_order_relaxed);
                }
                return static_cast<osg::Node*>(cached.get());
            }
        }
        else if (v313QualityMode > 0)
        {
            // Generic cache expiry/removal does not know about V3.13's side table.
            // A real cache miss is authoritative and makes any old quality record stale.
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            mV313ChunkQualities.erase(id);
            mV313StrongUpgradeInFlight.erase(id);
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
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);

        const V313ChunkQuality v313BuiltQuality{
            activeGrid && compile && v311PrepareMode > 0 ? static_cast<unsigned char>(v311PrepareMode) : 0,
            activeGrid && compile && static_cast<int>(Settings::cells().mV312SpatialBatchMode) > 0 ? 1 : 0 };
        if (v313RepairBuild)
            mV313UpgradeBuilt.fetch_add(1, std::memory_order_relaxed);

        if (v313QualityMode > 0)
        {
            // Strong-wins installation. A cheap demand miss may have started before a
            // strong worker finished; it must never overwrite the stronger live node.
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            const osg::ref_ptr<osg::Object> current = mCache->getRefFromObjectCache(id);
            const auto currentIt = mV313ChunkQualities.find(id);
            const V313ChunkQuality currentQuality
                = current && currentIt != mV313ChunkQualities.end() ? currentIt->second : V313ChunkQuality{};

            if (current && v313QualitySatisfies(currentQuality, v313BuiltQuality)
                && (currentQuality.mPrepareMode > v313BuiltQuality.mPrepareMode
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

        if (v311PrepareMode > 0 && activeGrid && compile)
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.insert(id);
            mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
        }
        return node;
    }

    namespace
    {
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
    }

    osg::ref_ptr<osg::Node> ObjectPaging::createChunk(float size, const osg::Vec2f& center, bool activeGrid,
        const osg::Vec3f& viewPoint, bool compile, unsigned char lod)
    {
        Debug::V3Diagnostics::TraceScope trace(
            "paging", "object_chunk_create", activeGrid ? "active_grid" : "distant", 0.1);
        const bool v36StructureEnabled = Debug::V36StructureTrace::writer().enabled();
        const auto v36StructureStart
            = v36StructureEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        const osg::Vec2i startCell(static_cast<int>(std::floor(center.x() - size / 2.f)),
            static_cast<int>(std::floor(center.y() - size / 2.f)));
        const MWBase::World& world = *MWBase::Environment::get().getWorld();
        const MWWorld::ESMStore& store = world.getStore();

        std::map<ESM::RefNum, PagedCellRef> refs;

        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_collect_refs", activeGrid ? "active_grid" : "distant", 0.1);
            if (mWorldspace == ESM::Cell::sDefaultWorldspaceId)
            {
                refs = collectESM3References(size, startCell, store);
            }
            else
            {
                refs = collectESM4References(size, startCell, mWorldspace);
            }
        }

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
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_template_analysis", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& [refNum, ref] : refs)
            {
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

            const int type = store.findStatic(ref.mRefId);
            VFS::Path::Normalized model = getModel(type, ref.mRefId, store);
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
                                    Misc::ResourceHelpers::getLODMeshName(world.getESMVersions()[refNum.mContentFile],
                                        model, *mSceneManager->getVFS(), lod))
                                ->second;
            }

            osg::ref_ptr<const osg::Node> cnode = mSceneManager->getTemplate(model, false);

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
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_build_instances", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& pair : nodes)
            {
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

                osg::ref_ptr<osg::Group> trans;
                if (merge)
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
                copyop.mDistances = lodDistances / ref.mScale;
                copyop.copy(cnode, trans);

                // Build occluder mesh for building-sized objects. Cache the simplified proxy
                // in model-local space so repeated instances/chunks and future sessions can reuse it.
                if (buildOccluders)
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
                attachTo->addChild(trans);
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
            unsigned int options = SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
                | SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES | SceneUtil::Optimizer::MERGE_GEOMETRY;

            const int v38BatchingMode = static_cast<int>(Settings::cells().mV38WorldBatchingMode);
            const int v39ConfiguredBatchOptimizerMode
                = static_cast<int>(Settings::cells().mV39BatchOptimizerMode);
            const int v39BatchOptimizerMode
                = (static_cast<int>(Settings::cells().mV39FrontloadMode) > 0 && !compile)
                ? 1
                : v39ConfiguredBatchOptimizerMode;
            const bool v310PreloadPostTransform
                = static_cast<bool>(Settings::cells().mV310PreloadPostTransform)
                && compile && mV310InitialFrontloadActive.load(std::memory_order_acquire)
                && v38BatchingMode >= 2;
            const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);
            const bool v311PreparedActive = v311PrepareMode > 0 && compile && activeGrid && v38BatchingMode >= 2;
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

            const bool v315CanonicalizeBeforeMerge
                = static_cast<bool>(Settings::cells().mV315PremergeStateCanonicalization)
                && compile && v38BatchingMode >= 2;
            if (v315CanonicalizeBeforeMerge)
                mSceneManager->shareState(mergeGroup);

            optimizer.optimize(mergeGroup, options);

            const bool v39ShareState
                = v39BatchOptimizerMode >= 2 || v310PreloadPostTransform || v311PreparedActive;
            if ((v39BatchOptimizerMode == 0 && v38BatchingMode >= 2) || v39ShareState)
                mSceneManager->shareState(mergeGroup);

            group->addChild(mergeGroup);

            if (mDebugBatches)
            {
                DebugVisitor dv;
                mergeGroup->accept(dv);
            }
            if (compile)
            {
                stateToCompile._mode = osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                mergeGroup->accept(stateToCompile);
            }
        }
        }

        osgUtil::IncrementalCompileOperation* const ico = mSceneManager->getIncrementalCompileOperation();
        if (!stateToCompile.empty() && ico)
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_compile_map", activeGrid ? "active_grid" : "distant", 0.1);
            if (Resource::v321CP2FairnessEnabled())
            {
                auto compileSet = new Resource::V321ClassifiedCompileSet(
                    group, Resource::V321CompileClass::ObjectPaging);
                compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                ico->add(compileSet, false);
            }
            else
            {
                auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);
                compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                ico->add(compileSet, false);
            }
        }

        if (Debug::V3Diagnostics::renderWriter().enabled())
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                << ',' << Debug::V3Diagnostics::csvQuote("object_chunk_summary") << ',' << Debug::V3Diagnostics::csvQuote(
                    std::string(activeGrid ? "active" : "distant") + " refs=" + std::to_string(refs.size())
                    + " templates=" + std::to_string(nodes.size()))
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
        mCache->call(ccf);
        if (ccf.getCollected().empty())
            return false;
        for (const ChunkId& chunk : ccf.getCollected())
            mCache->removeFromObjectCache(chunk);
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
        mCache->call(ccf);
        if (ccf.getCollected().empty())
            return false;
        for (const ChunkId& chunk : ccf.getCollected())
            mCache->removeFromObjectCache(chunk);
        return true;
    }

    void ObjectPaging::clear()
    {
        std::lock_guard<std::mutex> lock(mRefTrackerMutex);
        mRefTrackerNew.mDisabled.clear();
        mRefTrackerNew.mBlacklist.clear();
        mRefTrackerLocked = true;
    }

    void ObjectPaging::clearCache()
    {
        mCache->clear();
        if (static_cast<int>(Settings::cells().mV313ChunkQualityMode) > 0)
        {
            {
                std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
                mV313ChunkQualities.clear();
                mV313StrongUpgradeInFlight.clear();
            }
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                mV311PreparedActiveChunks.clear();
            }
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
