#ifndef OPENMW_MWRENDER_V4OBJECTCAPTUREPLAN_H
#define OPENMW_MWRENDER_V4OBJECTCAPTUREPLAN_H

#include "v4effectcapture.hpp"
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <osg/MatrixTransform>
#include <osg/PositionAttitudeTransform>

namespace MWRender
{
    // Owning-thread binding plans, never live OSG traversal on workers. An
    // exact topology check preserves unannounced attachments/reparenting; live
    // transforms and switch/mask values update the flat plan without rebuilding
    // it. Geometry/material guards still detect unmarked content mutations.
    class V4ObjectCapturePlans
    {
    public:
        std::optional<V4EffectCaptureResult> capture(osg::Node& root, const std::string& prefix,
            const VFS::Manager& vfs, NifRender::TextureIdentityCache& identities)
        {
            if (++mClock % 256 == 0)
                for (auto it = mPlans.begin(); it != mPlans.end();)
                    if (!it->second.root.valid()) { mNodeCount -= it->second.nodes.size(); it = mPlans.erase(it); }
                    else ++it;
            auto found = mPlans.find(&root);
            if (found == mPlans.end() || !found->second.current())
            {
                if (found != mPlans.end()) { mNodeCount -= found->second.nodes.size(); mPlans.erase(found); }
                Plan plan; plan.root = &root;
                osg::NodePath path;
                plan.supported = plan.append(root, NoParent, 0, path);
                while (mPlans.size() >= 1024 || mNodeCount + plan.nodes.size() > 32768)
                {
                    const auto oldest = std::min_element(mPlans.begin(), mPlans.end(),
                        [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
                    mNodeCount -= oldest->second.nodes.size(); mPlans.erase(oldest);
                }
                mNodeCount += plan.nodes.size();
                found = mPlans.emplace(&root, std::move(plan)).first;
                ++rebuilds;
                if (Debug::GameplayDiagnostics::sampling()) ++Debug::GameplayDiagnostics::context.objectPlanRebuilds;
            }
            else { ++reuses; if (Debug::GameplayDiagnostics::sampling()) ++Debug::GameplayDiagnostics::context.objectPlanReuses; }
            auto& plan = found->second;
            plan.used = mClock;
            if (!plan.supported) {
                ++fallbacks;
                if (Debug::GameplayDiagnostics::sampling()) ++Debug::GameplayDiagnostics::context.objectPlanFallbacks;
                return {};
            }
            V4EffectCaptureResult result;
            result.draws.reserve(plan.draws);
            std::size_t ordinal = 0;
            for (auto& binding : plan.nodes)
            {
                auto* node = binding.node.get(); // current() checked every observer/edge first
                binding.active = node->getNodeMask() != 0 && !binding.effectRoot;
                binding.matrix.makeIdentity();
                if (binding.parent != NoParent)
                {
                    const auto& parent = plan.nodes[binding.parent];
                    binding.active = binding.active && parent.active;
                    if (const auto* selection = parent.selection)
                        binding.active = binding.active && selection->getValue(binding.childIndex);
                    binding.matrix = parent.matrix;
                }
                if (!binding.active) continue;
                if (binding.transform) binding.transform->computeLocalToWorldMatrix(binding.matrix, nullptr);
                if (binding.geometry)
                {
                    if (Debug::GameplayDiagnostics::sampling()) ++Debug::GameplayDiagnostics::context.ordinaryGeometries;
                    RenderCore::ImmediateEffectDraw draw;
                    if (!v4_effect_detail::captureGeometry(*binding.geometry, binding.path, vfs,
                        prefix + ":geometry:" + std::to_string(ordinal++), draw, result.diagnostic,
                        &identities, false, &binding.matrix)) return result;
                    result.draws.push_back(std::move(draw));
                }
            }
            return result;
        }
        void clear() { mPlans.clear(); mNodeCount = 0; }
        std::uint64_t rebuilds = 0, reuses = 0, fallbacks = 0;

    private:
        inline static constexpr std::size_t NoParent = std::numeric_limits<std::size_t>::max();
        struct Binding
        {
            osg::observer_ptr<osg::Node> node;
            osg::Group* group = nullptr;
            osg::Transform* transform = nullptr;
            osg::Switch* selection = nullptr;
            osg::Geometry* geometry = nullptr;
            std::vector<const osg::Node*> children;
            osg::NodePath path;
            std::size_t parent;
            unsigned childIndex;
            bool effectRoot = false, active = false;
            osg::Matrix matrix;
        };
        struct Plan
        {
            osg::observer_ptr<osg::Node> root;
            std::vector<Binding> nodes;
            std::size_t draws = 0;
            std::uint64_t used = 0;
            bool supported = false;

            bool current() const
            {
                if (!root.valid()) return false;
                for (const auto& binding : nodes)
                {
                    if (!binding.node.valid()) return false;
                    if (binding.effectRoot != (binding.parent != NoParent
                        && v4_effect_detail::isEffectRoot(*binding.node))) return false;
                    if (binding.group && !binding.effectRoot)
                    {
                        if (binding.group->getNumChildren() != binding.children.size()) return false;
                        for (unsigned i = 0; i < binding.children.size(); ++i)
                            if (binding.group->getChild(i) != binding.children[i]) return false;
                    }
                }
                return true;
            }

            bool append(osg::Node& node, std::size_t parent, unsigned childIndex, osg::NodePath& path)
            {
                // Bound plan memory and reject cycles. Unsupported/custom node
                // traversal, particles, rigs and morphs use the original visitor.
                if (nodes.size() >= 512 || path.size() >= 64
                    || std::find(path.begin(), path.end(), &node) != path.end()) return false;
                const auto& type = typeid(node);
                const bool effect = parent != NoParent && v4_effect_detail::isEffectRoot(node);
                const bool known = type == typeid(osg::Group) || type == typeid(osg::Node)
                    || type == typeid(osg::Geode) || type == typeid(osg::Geometry)
                    || type == typeid(osg::Switch) || type == typeid(osg::MatrixTransform)
                    || type == typeid(osg::PositionAttitudeTransform)
                    || type == typeid(SceneUtil::PositionAttitudeTransform)
                    || type == typeid(SceneUtil::Skeleton);
                Binding binding;
                binding.node = &node; binding.group = node.asGroup();
                binding.parent = parent; binding.childIndex = childIndex; binding.effectRoot = effect;
                binding.transform = dynamic_cast<osg::Transform*>(&node);
                binding.selection = dynamic_cast<osg::Switch*>(&node);
                binding.geometry = type == typeid(osg::Geometry) ? static_cast<osg::Geometry*>(&node) : nullptr;
                path.push_back(&node); binding.path = path;
                if (binding.geometry) ++draws;
                if (binding.group && !effect)
                    for (unsigned i = 0; i < binding.group->getNumChildren(); ++i)
                        binding.children.push_back(binding.group->getChild(i));
                const auto index = nodes.size();
                nodes.push_back(std::move(binding));
                bool valid = effect || known;
                if (!effect && valid && nodes[index].group)
                {
                    // append() can move nodes; never retain a Binding reference.
                    auto* group = nodes[index].group;
                    for (unsigned i = 0; valid && i < group->getNumChildren(); ++i)
                        valid = append(*group->getChild(i), index, i, path);
                }
                path.pop_back();
                return valid;
            }
        };
        std::uint64_t mClock = 0;
        std::size_t mNodeCount = 0;
        std::unordered_map<const osg::Node*, Plan> mPlans;
    };
}
#endif
