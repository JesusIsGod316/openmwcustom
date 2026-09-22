#ifndef OPENMW_MWRENDER_V4RIGIDACTORPOSE_H
#define OPENMW_MWRENDER_V4RIGIDACTORPOSE_H

#include <components/misc/strings/lower.hpp>
#include <components/rendercore/records.hpp>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <unordered_map>

namespace MWRender
{
    // CreatureAnimation intentionally does not force a SceneUtil::Skeleton for
    // rigid animated creatures. Read the evaluated transform hierarchy using
    // the same MatrixTransform-only, case-insensitive first-match contract as
    // Skeleton::InitBoneCacheVisitor, without reparenting or changing animation.
    inline bool captureV4RigidActorPose(osg::Node& root, const RenderCore::SkeletonPayload& skeleton,
        std::vector<glm::mat4>& global, std::string& diagnostic)
    {
        class Visitor final : public osg::NodeVisitor
        {
        public:
            Visitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
            void apply(osg::MatrixTransform& node) override
            {
                const osg::Matrixf parent = matrix;
                matrix = node.getMatrix() * parent;
                transforms.emplace(Misc::StringUtils::lowerCase(node.getName()), matrix);
                traverse(node);
                matrix = parent;
            }
            osg::Matrixf matrix;
            std::unordered_map<std::string, osg::Matrixf> transforms;
        } visitor;
        root.accept(visitor);
        global.clear();
        global.reserve(skeleton.bones.size());
        for (const auto& bone : skeleton.bones)
        {
            const auto found = visitor.transforms.find(Misc::StringUtils::lowerCase(bone.name));
            if (found == visitor.transforms.end())
            {
                diagnostic = "rigid actor evaluated hierarchy is missing transform " + bone.name;
                global.clear();
                return false;
            }
            glm::mat4 transform;
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r) transform[c][r] = found->second(c, r);
            if (!RenderCore::semantic_detail::finite(transform))
            {
                diagnostic = "rigid actor evaluated hierarchy has a non-finite transform " + bone.name;
                global.clear();
                return false;
            }
            global.push_back(transform);
        }
        return true;
    }
}
#endif
