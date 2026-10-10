#ifndef OPENMW_SCENEUTIL_PERSISTENTOPAQUEPACKETS_H
#define OPENMW_SCENEUTIL_PERSISTENTOPAQUEPACKETS_H

#include "drawphasetrace.hpp"
#include <components/nifosg/matrixtransform.hpp>

#include <osg/Geometry>
#include <osg/AlphaFunc>
#include <osg/Group>
#include <osg/Geode>
#include <osg/MatrixTransform>
#include <osg/observer_ptr>
#include <osg/Version>
#include <osgUtil/CullVisitor>
#include <osgUtil/StateGraph>

#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <typeinfo>
#include <vector>

namespace SceneUtil::PersistentOpaquePackets
{
    inline bool enabled()
    {
        static const bool value = [] {
            const char* p = std::getenv("OPENMW_P9_PERSISTENT_PACKETS");
            return p && std::strcmp(p, "1") == 0;
        }();
        return value;
    }

    struct Counters
    {
        inline static std::atomic_uint64_t built{ 0 }, rows{ 0 }, eligible{ 0 }, liveBytes{ 0 };
        inline static std::atomic_uint64_t views{ 0 }, submitted{ 0 }, culled{ 0 }, fallbacks{ 0 };
        inline static std::atomic_uint64_t statePushes{ 0 }, statePushesAvoided{ 0 }, changedProducers{ 0 };
        inline static std::atomic_uint64_t capacityDeclines{ 0 };
        inline static std::atomic_uint64_t sourceCertified{0}, sourceRejected{0}, ordinaryBranches{0};
    };

    // Only our retained opaque command callback may reuse this cull contract.
    // Ordinary application callbacks still take the original visitor path.
    struct RetainedOpaqueDrawCallback : osg::Drawable::DrawCallback {};

    // Nothing is merged or reordered. The exact inherited state and original
    // material/program still reach the established RenderLeaf draw owner.
    inline bool opaque(const osg::StateSet* state)
    {
        if (state)
        {
            if (state->getAttribute(osg::StateAttribute::STENCIL)) return false;
            if (const auto* alpha = dynamic_cast<const osg::AlphaFunc*>(state->getAttribute(osg::StateAttribute::ALPHAFUNC)))
                if (alpha->getFunction() != GL_ALWAYS) return false;
        }
        return !state || (state->getDataVariance() != osg::Object::DYNAMIC
            && !state->getUpdateCallback()
            && state->getRenderingHint() != osg::StateSet::TRANSPARENT_BIN
            && !(state->getMode(GL_BLEND) & osg::StateAttribute::ON)
            && (!state->useRenderBinDetails() || state->getBinName().empty()
                || state->getBinName() == "RenderBin"));
    }

    inline bool eligible(const osg::Geometry& geometry)
    {
        // A base-Geometry certificate deliberately excludes rig/morph, terrain,
        // billboard and custom drawable accept/draw implementations.
        return typeid(geometry) == typeid(osg::Geometry)
            && geometry.getDataVariance() != osg::Object::DYNAMIC
            && !geometry.getUpdateCallback() && !geometry.getEventCallback()
            && !geometry.getCullCallback() && (!geometry.getDrawCallback()
                || dynamic_cast<const RetainedOpaqueDrawCallback*>(geometry.getDrawCallback()))
            && !geometry.getComputeBoundingBoxCallback() && !geometry.getComputeBoundingSphereCallback()
            && geometry.getVertexArray() && geometry.getNumPrimitiveSets() != 0
            && opaque(geometry.getStateSet());
    }

    struct PrimitiveRevision
    {
        const osg::PrimitiveSet* owner = nullptr;
        unsigned generation = 0, indices = 0, mode = 0;
        int instances = 0;
        explicit PrimitiveRevision(const osg::PrimitiveSet* primitive = nullptr) : owner(primitive)
        {
            if (primitive)
            {
                generation = primitive->getModifiedCount();
                indices = primitive->getNumIndices();
                mode = primitive->getMode();
                instances = primitive->getNumInstances();
            }
        }
        bool matches(const osg::PrimitiveSet* primitive) const
        {
            return primitive == owner && primitive && generation == primitive->getModifiedCount()
                && indices == primitive->getNumIndices() && mode == primitive->getMode()
                && instances == primitive->getNumInstances();
        }
    };

    struct Row
    {
        // The retained original node prevents address reuse after a producer
        // replacement and owns its arrays/material through the consuming view.
        osg::ref_ptr<osg::Node> owner;
        const osg::Array* vertices = nullptr;
        const osg::StateSet* material = nullptr;
        osg::BoundingBox bounds;
        std::array<PrimitiveRevision, 8> primitives;
        std::array<osg::ref_ptr<const osg::PrimitiveSet>, 8> primitiveOwners;
        osg::ref_ptr<const osg::Drawable::DrawCallback> drawOwner;
        unsigned primitiveCount = 0;
        unsigned vertexGeneration = 0;
        bool usable = false;

        explicit Row(osg::Node* node) : owner(node)
        {
            auto* geometry = node ? node->asGeometry() : nullptr;
            usable = geometry && eligible(*geometry);
            if (!usable) return;
            vertices = geometry->getVertexArray();
            vertexGeneration = vertices->getModifiedCount();
            material = geometry->getStateSet();
            drawOwner = geometry->getDrawCallback();
            bounds = geometry->getBoundingBox();
            primitiveCount = geometry->getNumPrimitiveSets();
            usable = bounds.valid() && std::isfinite(bounds.radius()) && primitiveCount <= primitives.size();
            if (usable)
                for (unsigned i = 0; i < primitiveCount; ++i)
                {
                    primitiveOwners[i] = geometry->getPrimitiveSet(i);
                    primitives[i] = PrimitiveRevision(geometry->getPrimitiveSet(i));
                }
        }

        bool matches(osg::Node* current) const
        {
            if (!usable || current != owner.get()) return false;
            auto* geometry = current->asGeometry();
            if (!geometry || !eligible(*geometry) || geometry->getStateSet() != material
                || geometry->getDrawCallback() != drawOwner.get()
                || geometry->getVertexArray() != vertices || vertices->getModifiedCount() != vertexGeneration
                || geometry->getNumPrimitiveSets() != primitiveCount) return false;
            for (unsigned i = 0; i < primitiveCount; ++i)
                if (!primitives[i].matches(geometry->getPrimitiveSet(i))) return false;
            const auto& currentBounds = geometry->getBoundingBox();
            return currentBounds._min == bounds._min && currentBounds._max == bounds._max;
        }
    };

    struct FrameInput
    {
        // Numeric inputs belong to this one synchronous view publication. There
        // is no previous-frame visibility cache or live world/actor pointer.
        const osg::Matrix modelView;
        const osg::Matrix projection;
        const osg::Node::NodeMask traversalMask;
        const unsigned frame;

        explicit FrameInput(osgUtil::CullVisitor& visitor)
            : modelView(*visitor.getModelViewMatrix()), projection(*visitor.getProjectionMatrix())
            , traversalMask(visitor.getTraversalMask())
            , frame(visitor.getFrameStamp() ? visitor.getFrameStamp()->getFrameNumber() : 0) {}
    };

    class Group final : public osg::Group
    {
    public:
        Group() = default;
        Group(const Group& copy, const osg::CopyOp& op = osg::CopyOp::SHALLOW_COPY)
            : osg::Group(copy, op) {} // Cloned topology has no producer certificate: normal traversal.
        META_Node(SceneUtil, Group)

        explicit Group(osg::Group& original, std::vector<Row>&& rows, std::uint64_t charge)
            : mRows(std::move(rows)), mCharge(charge)
        {
            addChild(&original);
            setName("OptimizedMW-PersistentOpaquePackets");
        }

        void traverse(osg::NodeVisitor& visitor) override
        {
            auto* normal = getNumChildren() == 1 ? getChild(0)->asGroup() : nullptr;
            auto* cv = visitor.asCullVisitor();
            const bool supportedVisitor = cv && (typeid(*cv) == typeid(osgUtil::CullVisitor)
                || typeid(*cv) == typeid(DrawPhaseTrace::CullVisitor));
            if (!normal || mRows.empty() || !supportedVisitor || !cv->getModelViewMatrix()
                || !cv->getProjectionMatrix() || !cv->getCurrentStateGraph()
                || normal->getNumChildren() != mRows.size() || !safeRoot(*normal)
                || !certifiedAncestryMatches(visitor)
                || !cv->getCurrentCullingSet().getStateFrustumList().empty()
                || !opaqueInherited(*cv))
            {
                Counters::fallbacks.fetch_add(1, std::memory_order_relaxed);
                osg::Group::traverse(visitor);
                return;
            }
            if (!visitor.validNodeMask(*normal) || cv->isCulled(*normal)) return;
            const FrameInput input(*cv);
            std::uint64_t submissions = 0, rejected = 0, fallbackRows = 0, pushes = 0, avoided = 0, changes = 0;
            visitor.pushOntoNodePath(normal);
            cv->pushCurrentMask();
            const osg::StateSet* rootState = normal->getStateSet();
            if (rootState) cv->pushStateSet(rootState);
            const osg::StateSet* pushedMaterial = nullptr;
            auto closeMaterial = [&] {
                if (pushedMaterial) cv->popStateSet();
                pushedMaterial = nullptr;
            };
            for (unsigned i = 0; i < normal->getNumChildren(); ++i)
            {
                osg::Node* child = normal->getChild(i);
                const Row& row = mRows[i];
                if (!row.matches(child))
                {
                    closeMaterial();
                    if (row.usable) ++changes;
                    ++fallbackRows;
                    child->accept(visitor);
                    continue;
                }
                if (!visitor.validNodeMask(*child)) { ++rejected; continue; }
                auto* geometry = child->asGeometry();
                if (geometry->isCullingActive() && cv->isCulled(row.bounds)) { ++rejected; continue; }
                if (cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR
                    && !cv->updateCalculatedNearFar(input.modelView, *geometry, false)) { ++rejected; continue; }
                const auto center = row.bounds.center();
                const float depth = static_cast<float>(-(static_cast<osgUtil::CullVisitor::value_type>(center.x())
                    * input.modelView(0, 2) + static_cast<osgUtil::CullVisitor::value_type>(center.y())
                    * input.modelView(1, 2) + static_cast<osgUtil::CullVisitor::value_type>(center.z())
                    * input.modelView(2, 2) + input.modelView(3, 2)));
                if (osg::isNaN(depth)) { ++rejected; continue; }
                if (pushedMaterial != row.material)
                {
                    closeMaterial();
                    pushedMaterial = row.material;
                    if (pushedMaterial) { cv->pushStateSet(pushedMaterial); ++pushes; }
                }
                else if (pushedMaterial) ++avoided;
                visitor.pushOntoNodePath(geometry);
                {
                    DrawPhaseTrace::SubmissionScope submission(*cv, *geometry);
                    // OSG owns these immutable per-view matrix leases through
                    // RenderLeaf consumption. The numeric snapshot above is only
                    // used for current-view calculations; no barrier is removed.
                    cv->addDrawableAndDepth(geometry, cv->getModelViewMatrix(), depth);
                }
                visitor.popFromNodePath();
                ++submissions;
            }
            closeMaterial();
            if (rootState) cv->popStateSet();
            cv->popCurrentMask();
            visitor.popFromNodePath();
            Counters::views.fetch_add(1, std::memory_order_relaxed);
            Counters::submitted.fetch_add(submissions, std::memory_order_relaxed);
            Counters::culled.fetch_add(rejected, std::memory_order_relaxed);
            Counters::fallbacks.fetch_add(fallbackRows, std::memory_order_relaxed);
            Counters::statePushes.fetch_add(pushes, std::memory_order_relaxed);
            Counters::statePushesAvoided.fetch_add(avoided, std::memory_order_relaxed);
            Counters::changedProducers.fetch_add(changes, std::memory_order_relaxed);
        }

        static bool safeRoot(const osg::Group& original)
        {
            return (typeid(original) == typeid(osg::Group) || typeid(original) == typeid(osg::Geode))
                && original.getDataVariance() != osg::Object::DYNAMIC
                && !original.getUpdateCallback() && !original.getEventCallback() && !original.getCullCallback()
                && !original.getComputeBoundingSphereCallback() && opaque(original.getStateSet());
        }

        static bool opaqueInherited(osgUtil::CullVisitor& visitor)
        {
            for (auto* graph = visitor.getCurrentStateGraph(); graph; graph = graph->_parent)
                if (const auto* state = graph->getStateSet())
                    if (state->getRenderingHint() == osg::StateSet::TRANSPARENT_BIN
                        || (state->getMode(GL_BLEND) & osg::StateAttribute::ON)
                        || (state->useRenderBinDetails() && !state->getBinName().empty()
                            && state->getBinName() != "RenderBin")) return false;
            return true;
        }

        std::size_t rowCount() const { return mRows.size(); }
        void setCertifiedAncestors(const std::array<osg::observer_ptr<osg::Node>,16>& ancestors, unsigned count)
        { mCertifiedAncestors = ancestors; mAncestorCount = count; }
        osg::BoundingSphere computeBound() const override
        {
            return getNumChildren() == 1 ? getChild(0)->getBound() : osg::Group::computeBound();
        }

    protected:
        ~Group() override { Counters::liveBytes.fetch_sub(mCharge, std::memory_order_relaxed); }

    private:
        bool certifiedAncestryMatches(const osg::NodeVisitor& visitor) const
        {
            const auto& path = visitor.getNodePath();
            std::size_t position = 0;
            for (unsigned i = 0; i < mAncestorCount; ++i)
            {
                const auto* ancestor = mCertifiedAncestors[i].get();
                if (!ancestor || ancestor->getDataVariance() == osg::Object::DYNAMIC
                    || ancestor->getUpdateCallback() || ancestor->getEventCallback() || ancestor->getCullCallback()
                    || ancestor->getComputeBoundingSphereCallback() || !opaque(ancestor->getStateSet())) return false;
                while (position < path.size() && path[position] != ancestor) ++position;
                if (position == path.size()) return false;
                ++position;
            }
            return true;
        }
        std::array<osg::observer_ptr<osg::Node>,16> mCertifiedAncestors{};
        unsigned mAncestorCount = 0;
        const std::vector<Row> mRows;
        const std::uint64_t mCharge = 0;
    };

    inline osg::ref_ptr<osg::Node> build(osg::Group& original, bool requested = enabled())
    {
        if (!requested || std::strcmp(osgGetVersion(), "3.6.5") != 0 || !Group::safeRoot(original))
            return &original;
        constexpr std::size_t MaximumRows = 8192;
        constexpr std::uint64_t MaximumLiveBytes = 64ull * 1024 * 1024;
        if (!original.getNumChildren() || original.getNumChildren() > MaximumRows) return &original;
        std::vector<Row> rows;
        std::size_t usable = 0;
        try
        {
            rows.reserve(original.getNumChildren());
            for (unsigned i = 0; i < original.getNumChildren(); ++i)
            {
                rows.emplace_back(original.getChild(i));
                usable += rows.back().usable;
            }
        }
        catch (const std::bad_alloc&) { return &original; }
        if (!usable) return &original;
        const std::uint64_t charge = rows.capacity() * sizeof(Row) + sizeof(Group);
        auto current = Counters::liveBytes.load(std::memory_order_relaxed);
        do
        {
            if (charge > MaximumLiveBytes || current > MaximumLiveBytes - charge)
            {
                Counters::capacityDeclines.fetch_add(1, std::memory_order_relaxed);
                return &original;
            }
        } while (!Counters::liveBytes.compare_exchange_weak(current, current + charge, std::memory_order_relaxed));
        osg::ref_ptr<osg::Node> result;
        try { result = new Group(original, std::move(rows), charge); }
        catch (const std::bad_alloc&)
        {
            Counters::liveBytes.fetch_sub(charge, std::memory_order_relaxed);
            return &original;
        }
        Counters::built.fetch_add(1, std::memory_order_relaxed);
        Counters::rows.fetch_add(original.getNumChildren(), std::memory_order_relaxed);
        Counters::eligible.fetch_add(usable, std::memory_order_relaxed);
        return result;
    }
    // Certify controller/type identity before paging CopyOp can erase it.
    // Material eligibility is checked later per branch, so transparent sibling
    // materials do not invalidate an otherwise immutable opaque population.
    inline bool sourceAncestryCertified(const osg::Node& source, unsigned maximumNodes = 1024)
    {
        unsigned nodes = 0;
        const auto inspect = [&](const auto& self, const osg::Node& node, unsigned depth) -> bool {
            if (++nodes > maximumNodes || depth > 32 || node.getDataVariance() == osg::Object::DYNAMIC
                || node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback()
                || node.getComputeBoundingSphereCallback()) return false;
            const auto* state = node.getStateSet();
            if (state && (state->getDataVariance() == osg::Object::DYNAMIC
                    || state->getUpdateCallback() || state->getEventCallback())) return false;
            if (const auto* geometry = node.asGeometry())
                return typeid(node) == typeid(osg::Geometry) && !geometry->getDrawCallback()
                    && !geometry->getComputeBoundingBoxCallback();
            const auto* group = node.asGroup();
            if (!group || (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode)
                    && typeid(node) != typeid(osg::MatrixTransform) && typeid(node) != typeid(NifOsg::MatrixTransform))) return false;
            if (const auto* transform = dynamic_cast<const osg::MatrixTransform*>(&node))
            {
                if (transform->getReferenceFrame() != osg::Transform::RELATIVE_RF) return false;
                for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
                    if (!std::isfinite(transform->getMatrix()(r,c))) return false;
            }
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
                if (!self(self, *group->getChild(i), depth + 1)) return false;
            return true;
        };
        return inspect(inspect, source, 0);
    }

    // Caller must provide a privately cloned asset partition whose ORIGINAL
    // ancestry was certified before paging CopyOp. This deliberately does not
    // turn arbitrary controller-stripped scene graphs into packet candidates.
    // Preserve every material and transform ancestor, replacing only exact
    // immutable Group/Geode containers with their ordinary-path-owning wrapper.
    inline osg::ref_ptr<osg::Node> buildCertifiedSubtrees(osg::Node& original, unsigned maximumNodes = 256,
        bool mode = enabled(), bool selective = false)
    {
        if (!mode) return &original;
        unsigned nodes = 0;
        const auto certify = [&](const auto& self, const osg::Node& node, unsigned depth) -> bool {
            if (++nodes > maximumNodes || depth > 16 || node.getDataVariance() == osg::Object::DYNAMIC
                || node.getUpdateCallback() || node.getEventCallback() || node.getCullCallback()
                || node.getComputeBoundingSphereCallback() || !opaque(node.getStateSet())) return false;
            if (const auto* geometry = node.asGeometry()) return eligible(*geometry);
            const auto* group = node.asGroup();
            if (!group || (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode)
                && typeid(node) != typeid(osg::MatrixTransform))) return false;
            if (const auto* transform = dynamic_cast<const osg::MatrixTransform*>(&node))
                for (unsigned r = 0; r < 4; ++r) for (unsigned c = 0; c < 4; ++c)
                    if (!std::isfinite(transform->getMatrix()(r,c))) return false;
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
                if (!self(self, *group->getChild(i), depth + 1)) return false;
            return true;
        };
        if (!selective && !certify(certify, original, 0)) return &original;
        nodes = 0;
        std::array<osg::observer_ptr<osg::Node>,16> ancestors;
        const auto wrap = [&](const auto& self, osg::Node& node, unsigned depth) -> osg::ref_ptr<osg::Node> {
            if (++nodes > maximumNodes || depth >= ancestors.size()
                || node.getDataVariance() == osg::Object::DYNAMIC || node.getUpdateCallback()
                || node.getEventCallback() || node.getCullCallback() || node.getComputeBoundingSphereCallback()
                || !opaque(node.getStateSet())) return &node;
            auto* group = node.asGroup();
            if (!group) return &node;
            if (typeid(node) != typeid(osg::Group) && typeid(node) != typeid(osg::Geode)
                && typeid(node) != typeid(osg::MatrixTransform)) return &node;
            for (unsigned i = 0; i < group->getNumChildren(); ++i)
            {
                osg::ref_ptr<osg::Node> child = group->getChild(i);
                ancestors[depth] = &node;
                osg::ref_ptr<osg::Node> wrapped = self(self, *child, depth + 1);
                if (wrapped != child) group->setChild(i, wrapped);
            }
            if (typeid(node) == typeid(osg::Group) || typeid(node) == typeid(osg::Geode))
            {
                auto result = build(*group, mode);
                if (auto* packet = dynamic_cast<Group*>(result.get()))
                { packet->setCertifiedAncestors(ancestors, depth); if (selective) ++Counters::ordinaryBranches; }
                return result;
            }
            return &node; // exact transform and its state ancestry remain intact
        };
        return wrap(wrap, original, 0);
    }
}
#endif
