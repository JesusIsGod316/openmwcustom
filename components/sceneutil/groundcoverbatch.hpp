#ifndef OPENMW_SCENEUTIL_GROUNDCOVERBATCH_H
#define OPENMW_SCENEUTIL_GROUNDCOVERBATCH_H

#include "groundcoverpolicy.hpp"

#include <osg/AutoTransform>
#include <osg/Billboard>
#include <osg/BufferObject>
#include <osg/ComputeBoundsVisitor>
#include <osg/Geometry>
#include <osg/LOD>
#include <osg/MatrixTransform>
#include <osg/NodeCallback>
#include <osg/Switch>
#include <osg/Uniform>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderBin>
#include <osgUtil/StateGraph>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace SceneUtil::GroundcoverBatch
{
    using Instance = GroundcoverPolicy::Instance;
    using Options = GroundcoverPolicy::Options;
    using OcclusionTest = std::function<bool(osgUtil::CullVisitor&, const osg::BoundingBox&, std::uint64_t)>;

    struct Counters
    {
        std::atomic_uint64_t groupsTested{ 0 }, groupsRejected{ 0 }, instancesRejected{ 0 };
        std::atomic_uint64_t fullInstances{ 0 }, submittedInstances{ 0 }, drawableVisits{ 0 };
        std::array<std::atomic_uint64_t, 3> tiers{};
    };

    inline osg::BoundingBox transformBox(const osg::BoundingBox& box, const osg::Matrix& matrix)
    {
        osg::BoundingBox result;
        if (box.valid())
            for (unsigned int i = 0; i < 8; ++i)
                result.expandBy(box.corner(i) * matrix);
        return result;
    }

    inline bool finiteBox(const osg::BoundingBox& box)
    {
        if (!box.valid())
            return false;
        for (unsigned int i = 0; i < 3; ++i)
            if (!std::isfinite(box._min[i]) || !std::isfinite(box._max[i]))
                return false;
        return true;
    }

    inline float linearScaleUpper(const osg::Matrix& matrix)
    {
        double sum = 0.0;
        for (unsigned int r = 0; r < 3; ++r)
            for (unsigned int c = 0; c < 3; ++c)
                sum += matrix(r, c) * matrix(r, c);
        return static_cast<float>(std::sqrt(sum)); // Frobenius norm bounds every singular value.
    }

    inline float currentMargin(osgUtil::CullVisitor& cv)
    {
        // The maximum across inherited candidates is conservative even with OVERRIDE /
        // PROTECTED uniform state. Also cover the precomputed P8G2 coefficient path.
        float result = GroundcoverPolicy::windMargin(0.f);
        for (auto* graph = cv.getCurrentStateGraph(); graph; graph = graph->_parent)
            if (const auto* state = graph->getStateSet())
            {
                if (const auto* uniform = state->getUniform("windSpeed"))
                {
                    float value = 0.f;
                    if (uniform->get(value))
                        result = std::max(result, GroundcoverPolicy::windMargin(std::abs(value)));
                }
                if (const auto* uniform = state->getUniform("groundcoverWindCoefficients"))
                {
                    osg::Vec4f value;
                    if (uniform->get(value))
                    {
                        float bound = 61.f;
                        for (unsigned int i = 0; i < 4; ++i)
                        {
                            if (!std::isfinite(value[i])) return std::numeric_limits<float>::infinity();
                            bound += std::abs(value[i]);
                        }
                        result = std::max(result, bound);
                    }
                }
            }
        return result;
    }

    inline float projectionScaleUpper(osgUtil::CullVisitor& cv)
    {
        float result = static_cast<float>(std::abs((*cv.getProjectionMatrix())(1, 1)));
        for (auto* graph = cv.getCurrentStateGraph(); graph; graph = graph->_parent)
            if (const auto* state = graph->getStateSet())
                if (const auto* uniform = state->getUniform("projectionMatrix"))
                {
                    osg::Matrixf value;
                    if (uniform->get(value)) result = std::max(result, std::abs(value(1, 1)));
                }
        return result;
    }

    struct AnimatedBounds
    {
        osg::BoundingBox local;
        osg::BoundingBox world;
    };

    inline std::optional<AnimatedBounds> animatedBounds(const osg::BoundingBox& rest, osgUtil::CullVisitor& cv)
    {
        if (!finiteBox(rest) || !cv.getCurrentCamera() || !cv.getModelViewMatrix())
            return std::nullopt;
        const float margin = currentMargin(cv);
        if (!std::isfinite(margin))
            return std::nullopt;
        osg::Matrix inverseView;
        if (!inverseView.invert(cv.getCurrentCamera()->getViewMatrix()))
            return std::nullopt;
        const osg::Matrix localToWorld = *cv.getModelViewMatrix() * inverseView;
        osg::Matrix worldToLocal;
        if (!worldToLocal.invert(localToWorld))
            return std::nullopt;
        auto world = transformBox(rest, localToWorld);
        // The production shader displaces world X/Y only. Add a numeric guard in Z.
        world._min -= osg::Vec3f(margin, margin, 1.f);
        world._max += osg::Vec3f(margin, margin, 1.f);
        auto local = transformBox(world, worldToLocal);
        if (!finiteBox(world) || !finiteBox(local))
            return std::nullopt;
        return AnimatedBounds{ local, world };
    }

    class VisibilityCallback final : public osg::NodeCallback
    {
    public:
        VisibilityCallback(osg::BoundingBox rest, float distance, std::uint64_t count,
            OcclusionTest test, std::shared_ptr<Counters> counters)
            : mRest(rest), mDistance(distance), mCount(count), mTest(std::move(test)), mCounters(std::move(counters))
        {
        }
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            auto* cv = nv->asCullVisitor();
            if (!cv)
            {
                traverse(node, nv);
                return;
            }
            if (mCounters)
                mCounters->groupsTested.fetch_add(1, std::memory_order_relaxed);
            bool visible = true;
            if (auto bounds = animatedBounds(mRest, *cv))
            {
                // TestRect and the frustum stack are caller-owned; no worker touches either.
                // These tests must not modify the inherited frustum result-mask for siblings.
                cv->pushCurrentMask();
                visible = !cv->isCulled(bounds->local);
                cv->popCurrentMask();
                if (visible && mDistance > 0.f)
                {
                    const auto eye = cv->getEyePoint();
                    float distance2 = 0.f;
                    for (unsigned int i = 0; i < 3; ++i)
                    {
                        const float d = std::max({ bounds->local._min[i] - eye[i],
                            eye[i] - bounds->local._max[i], 0.f });
                        distance2 += d * d;
                    }
                    visible = distance2 <= mDistance * mDistance;
                }
                if (visible && mTest)
                    visible = mTest(*cv, bounds->world, mCount);
            }
            // Invalid transforms/wind always fail open. Ordinary OSG bound rejection is
            // disabled on this derived graph; it must not reapply stale unanimated bounds.
            if (visible)
                traverse(node, nv);
            else if (mCounters)
            {
                mCounters->groupsRejected.fetch_add(1, std::memory_order_relaxed);
                mCounters->instancesRejected.fetch_add(mCount, std::memory_order_relaxed);
            }
        }
    private:
        osg::BoundingBox mRest;
        float mDistance;
        std::uint64_t mCount;
        OcclusionTest mTest;
        std::shared_ptr<Counters> mCounters;
    };

    class NearFarCallback final : public osg::DrawableCullCallback
    {
    public:
        NearFarCallback(osg::BoundingBox rest, std::uint64_t count, std::shared_ptr<Counters> counters)
            : mRest(rest), mCount(count), mCounters(std::move(counters)) {}
        bool cull(osg::NodeVisitor* nv, osg::Drawable*, osg::RenderInfo*) const override
        {
            auto* cv = nv->asCullVisitor();
            if (cv && mCounters)
            {
                mCounters->fullInstances.fetch_add(mCount, std::memory_order_relaxed);
                mCounters->submittedInstances.fetch_add(mCount, std::memory_order_relaxed);
                mCounters->drawableVisits.fetch_add(1, std::memory_order_relaxed);
            }
            if (cv && cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR)
                if (auto bounds = animatedBounds(mRest, *cv))
                    cv->updateCalculatedNearFar(*cv->getModelViewMatrix(), bounds->local);
            return false;
        }
    private:
        osg::BoundingBox mRest;
        std::uint64_t mCount;
        std::shared_ptr<Counters> mCounters;
    };

    class LodCallback final : public osg::NodeCallback
    {
    public:
        LodCallback(osg::BoundingBox bases, float meshRadius, Options options,
            std::array<unsigned int, 3> counts, std::shared_ptr<Counters> counters)
            : mBases(bases), mMeshRadius(meshRadius), mOptions(options), mCounts(counts), mCounters(std::move(counters))
        {
        }
        void operator()(osg::Node* node, osg::NodeVisitor* nv) override
        {
            auto* cv = nv->asCullVisitor();
            auto* group = node->asGroup();
            if (!cv || !group || group->getNumChildren() != 3)
            {
                traverse(node, nv); // Compile/resource visitors must see every prepared tier.
                return;
            }
            const auto viewBox = transformBox(mBases, *cv->getModelViewMatrix());
            float maximumDensity = 1.f;
            if (finiteBox(viewBox))
            {
                const float minimumDistance = std::max(0.f, viewBox.center().length() - viewBox.radius());
                const float radius = mMeshRadius * linearScaleUpper(*cv->getModelViewMatrix());
                const float projectionY = projectionScaleUpper(*cv);
                const float projected = radius * projectionY / std::max(1.f, minimumDistance);
                maximumDensity = GroundcoverPolicy::density(minimumDistance, projected, mOptions);
            }
            unsigned int tier = GroundcoverPolicy::desiredTier(maximumDensity);
            // Bounded per-camera hysteresis. No wait on contention, no shared geometry mutation,
            // no previous-camera result ever allowed to suppress required detail.
            std::unique_lock lock(mMutex, std::try_to_lock);
            if (lock.owns_lock())
            {
                const osg::Camera* camera = cv->getCurrentCamera();
                auto it = std::find_if(mViews.begin(), mViews.end(),
                    [camera](const View& view) { return view.camera == camera; });
                if (it == mViews.end())
                {
                    it = mViews.begin() + mNextView;
                    mNextView = (mNextView + 1) % mViews.size();
                    *it = View{ camera, tier };
                }
                else
                    tier = GroundcoverPolicy::hystereticTier(it->tier, tier, maximumDensity);
                it->tier = tier;
                lock.unlock();
            }
            if (mCounters)
            {
                mCounters->fullInstances.fetch_add(mCounts[0], std::memory_order_relaxed);
                mCounters->submittedInstances.fetch_add(mCounts[tier], std::memory_order_relaxed);
                mCounters->drawableVisits.fetch_add(1, std::memory_order_relaxed);
                mCounters->tiers[tier].fetch_add(1, std::memory_order_relaxed);
            }
            if (mCounts[tier] != 0)
                group->getChild(tier)->accept(*nv);
        }
    private:
        struct View { const osg::Camera* camera = nullptr; unsigned int tier = 0; };
        osg::BoundingBox mBases;
        float mMeshRadius;
        Options mOptions;
        std::array<unsigned int, 3> mCounts;
        std::shared_ptr<Counters> mCounters;
        std::mutex mMutex;
        std::array<View, 4> mViews{};
        std::size_t mNextView = 0;
    };

    // Eligibility is checked on a private template, before publication. Unknown dynamic
    // graphs keep the established renderer; no broad update suppression or animation stripping.
    class EligibilityVisitor final : public osg::NodeVisitor
    {
    public:
        EligibilityVisitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        bool eligible = true;
        std::size_t drawables = 0;
        void apply(osg::Node& node) override
        {
            std::string shaderPrefix;
            if (node.getUserValue("shaderPrefix", shaderPrefix) && shaderPrefix != "groundcover")
                eligible = false; // A custom program need not implement the LOD coverage contract.
            if (node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback()
                || node.getDataVariance() == osg::Object::DYNAMIC
                || dynamic_cast<osg::LOD*>(&node) || dynamic_cast<osg::Switch*>(&node)
                || dynamic_cast<osg::AutoTransform*>(&node) || dynamic_cast<osg::Billboard*>(&node))
                eligible = false;
            if (auto* transform = node.asTransform())
                if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF)
                    eligible = false;
            if (node.getStateSet() && node.getStateSet()->getDataVariance() == osg::Object::DYNAMIC)
                eligible = false;
            traverse(node);
        }
        void apply(osg::Geometry& geometry) override
        {
            ++drawables;
            if (!geometry.getVertexArray() || geometry.getVertexArray()->getType() != osg::Array::Vec3ArrayType
                || geometry.getDrawCallback() || geometry.getCullCallback() || geometry.getUpdateCallback()
                || geometry.getComputeBoundingBoxCallback()
                || geometry.getDataVariance() == osg::Object::DYNAMIC || geometry.getNumPrimitiveSets() == 0)
                eligible = false;
            apply(static_cast<osg::Node&>(geometry));
        }
        void apply(osg::Drawable& drawable) override
        {
            if (!drawable.asGeometry())
                eligible = false;
            apply(static_cast<osg::Node&>(drawable));
        }
    };

    inline osg::BoundingBox instanceBounds(const osg::BoundingBox& source, std::span<const Instance> instances)
    {
        osg::BoundingBox result;
        for (const auto& instance : instances)
            for (unsigned int i = 0; i < 8; ++i)
            {
                const auto p = source.corner(i);
                const auto r = GroundcoverPolicy::rotate({ p.x(), p.y(), p.z() }, instance.rotation);
                result.expandBy(osg::Vec3f(r[0] * instance.scale + instance.position[0],
                    r[1] * instance.scale + instance.position[1], r[2] * instance.scale + instance.position[2]));
            }
        return result;
    }

    class InstanceVisitor final : public osg::NodeVisitor
    {
    public:
        InstanceVisitor(std::span<const Instance> instances, bool lod, Options options, std::shared_ptr<Counters> counters)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN), mInstances(instances), mLod(lod), mOptions(options),
              mCounters(std::move(counters)), mOffsets(new osg::Vec4Array), mRotations(new osg::Vec4Array),
              mLegacyRotations(new osg::Vec3Array)
        {
            mOffsets->reserve(instances.size());
            mRotations->reserve(lod ? instances.size() : 0);
            mLegacyRotations->reserve(lod ? 0 : instances.size());
            for (const auto& value : instances)
            {
                mOffsets->push_back(osg::Vec4f(value.position[0], value.position[1], value.position[2], value.scale));
                if (lod)
                    mRotations->push_back(osg::Vec4f(value.rotation[0], value.rotation[1], value.rotation[2], value.rank));
                else
                    mLegacyRotations->push_back(osg::Vec3f(value.rotation[0], value.rotation[1], value.rotation[2]));
                mBases.expandBy(osg::Vec3f(value.position[0], value.position[1], value.position[2]));
                mMaxScale = std::max(mMaxScale, value.scale);
            }
            osg::ref_ptr<osg::VertexBufferObject> vbo = new osg::VertexBufferObject;
            mOffsets->setVertexBufferObject(vbo);
            if (lod) mRotations->setVertexBufferObject(vbo);
            else mLegacyRotations->setVertexBufferObject(vbo);
        }
        void apply(osg::Node& node) override
        {
            node.setCullingActive(false);
            traverse(node);
        }
        void apply(osg::Geometry& geometry) override
        {
            const auto sourceBox = geometry.getBoundingBox();
            const auto rest = instanceBounds(sourceBox, mInstances);
            geometry.setInitialBound(rest);
            // InitialBound alone is unioned with the uninstanced mesh at the chunk
            // origin by OSG. Suppress that extra bound: only the transformed
            // population exists on screen. Animated expansion remains per view.
            geometry.setComputeBoundingBoxCallback(new osg::Drawable::ComputeBoundingBoxCallback);
            geometry.setUseDisplayList(false);
            geometry.setUseVertexBufferObjects(true);
            geometry.setCullingActive(false);
            geometry.setVertexAttribArray(6, mOffsets, osg::Array::BIND_PER_VERTEX);
            geometry.setVertexAttribArray(7, mLod ? static_cast<osg::Array*>(mRotations.get())
                : static_cast<osg::Array*>(mLegacyRotations.get()), osg::Array::BIND_PER_VERTEX);
            geometry.setCullCallback(new NearFarCallback(rest, mInstances.size(), mLod ? nullptr : mCounters));
            for (auto& primitive : geometry.getPrimitiveSetList())
                primitive->setNumInstances(static_cast<int>(mInstances.size()));
            if (!mLod)
                return;
            auto state = geometry.getStateSet()
                ? osg::ref_ptr<osg::StateSet>(new osg::StateSet(*geometry.getStateSet(), osg::CopyOp::SHALLOW_COPY))
                : osg::ref_ptr<osg::StateSet>(new osg::StateSet);
            const float meshRadius = sourceBox.radius() * mMaxScale;
            state->addUniform(new osg::Uniform("p8g3LodParams", osg::Vec4f(mOptions.nearDistance,
                mOptions.farDistance, mOptions.minimumDensity, meshRadius)), osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            geometry.setStateSet(state);
            osg::ref_ptr<osg::Group> tiers = new osg::Group;
            tiers->setCullingActive(false);
            std::array<unsigned int, 3> counts{};
            for (unsigned int tier = 0; tier < 3; ++tier)
            {
                counts[tier] = static_cast<unsigned int>(GroundcoverPolicy::tierCount(mInstances, tier));
                osg::ref_ptr<osg::Geometry> copy = new osg::Geometry(geometry, osg::CopyOp::DEEP_COPY_PRIMITIVES);
                for (auto& primitive : copy->getPrimitiveSetList())
                    primitive->setNumInstances(static_cast<int>(counts[tier]));
                // Vertex data and instance VBO arrays are shared. Primitive counts/indices
                // are private and bounded to three variants; never mutated during cull.
                tiers->addChild(copy);
            }
            tiers->setCullCallback(new LodCallback(mBases, meshRadius, mOptions, counts, mCounters));
            mReplacements.emplace_back(&geometry, tiers);
        }
        void finish()
        {
            // Replace after traversal so child iterators are not invalidated during visitation.
            for (const auto& [geometry, replacement] : mReplacements)
            {
                const auto parents = geometry->getParents();
                for (auto* parent : parents)
                    parent->replaceChild(geometry, replacement);
            }
            mReplacements.clear();
        }
    private:
        std::span<const Instance> mInstances;
        bool mLod;
        Options mOptions;
        std::shared_ptr<Counters> mCounters;
        osg::ref_ptr<osg::Vec4Array> mOffsets, mRotations;
        osg::ref_ptr<osg::Vec3Array> mLegacyRotations;
        osg::BoundingBox mBases;
        float mMaxScale = 0.f;
        std::vector<std::pair<osg::ref_ptr<osg::Geometry>, osg::ref_ptr<osg::Group>>> mReplacements;
    };

    inline osg::ref_ptr<osg::Group> buildTile(const osg::Node& preparedTemplate, std::span<const Instance> instances,
        bool lod, const Options& options, float viewDistance, OcclusionTest occlusion, std::shared_ptr<Counters> counters)
    {
        osg::ref_ptr<osg::Group> root = new osg::Group;
        root->setCullingActive(false);
        root->addChild(static_cast<osg::Node*>(preparedTemplate.clone(osg::CopyOp::DEEP_COPY_NODES
            | osg::CopyOp::DEEP_COPY_DRAWABLES | osg::CopyOp::DEEP_COPY_PRIMITIVES)));
        InstanceVisitor visitor(instances, lod, options, counters);
        root->accept(visitor);
        visitor.finish();
        osg::ComputeBoundsVisitor bounds;
        root->accept(bounds);
        root->setCullCallback(new VisibilityCallback(bounds.getBoundingBox(), viewDistance,
            instances.size(), std::move(occlusion), std::move(counters)));
        return root;
    }

    inline void enableFrontToBack(osg::StateSet& state)
    {
        static std::once_flag once;
        std::call_once(once, [] {
            osgUtil::RenderBin::addRenderBinPrototype("OptimizedMWGroundcoverFrontToBack",
                new osgUtil::RenderBin(osgUtil::RenderBin::SORT_BY_STATE_THEN_FRONT_TO_BACK));
        });
        // Grass only; after ordinary opaque bin 0 and before ordinary transparent bin 10.
        state.setRenderBinDetails(1, "OptimizedMWGroundcoverFrontToBack", osg::StateSet::OVERRIDE_RENDERBIN_DETAILS);
    }
}
#endif
