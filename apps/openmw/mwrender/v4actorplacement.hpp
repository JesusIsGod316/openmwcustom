#ifndef OPENMW_MWRENDER_V4ACTORPLACEMENT_H
#define OPENMW_MWRENDER_V4ACTORPLACEMENT_H

#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/rendercore/records.hpp>

namespace MWRender
{
    // Preserve the live root's translation/attitude/nonuniform scale exactly.
    // Bone matrices remain skeleton-local; the backend applies this only once.
    inline RenderCore::WorldTransform captureV4ActorPlacement(const SceneUtil::PositionAttitudeTransform& root)
    {
        const auto& p = root.getPosition();
        const auto& q = root.getAttitude();
        const auto& s = root.getScale();
        RenderCore::WorldTransform result;
        result.translation = { p.x(), p.y(), p.z() };
        result.rotation = { static_cast<float>(q.w()), static_cast<float>(q.x()),
            static_cast<float>(q.y()), static_cast<float>(q.z()) };
        result.scale = { s.x(), s.y(), s.z() };
        return result;
    }
}
#endif
