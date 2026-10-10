#ifndef OPENMW_RENDERCORE_GPUDEFORMATIONINPUT_H
#define OPENMW_RENDERCORE_GPUDEFORMATIONINPUT_H

#include "actorprogram.hpp"

namespace RenderCore
{
    // Immutable authored streams + small current native pose. No OSG/VSG
    // object escapes to preparation workers or into the semantic render world.
    struct GpuDeformationInput
    {
        ModelNodeIndex node;
        std::shared_ptr<const MeshPayload> source;
        std::shared_ptr<const SkinPayload> skin;
        std::shared_ptr<const MorphPayload> morphs;
        std::vector<glm::mat4> palette;
        std::vector<float> weights;
    };

    inline std::optional<GpuDeformationInput> gpuDeformationInput(const ActorProgram& program,
        const ActorProgram::MeshBinding& binding, const ActorProgram::Pose& pose, const MorphWeightState* morph)
    {
        GpuDeformationInput result;
        result.node = binding.node;
        result.source = binding.source;
        result.skin = binding.skin;
        result.morphs = binding.morphs;
        if (binding.morphs)
        {
            if (!morph || morph->current.size() != binding.morphs->targets.size()) return {};
            for (const float weight : morph->current) if (!std::isfinite(weight)) return {};
            result.weights = morph->current;
        }
        else if (!program.skinPalette(binding, pose, result.palette)) return {};
        return result;
    }
}
#endif
