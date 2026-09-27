#ifndef OPENMW_COMPONENTS_FX_NATIVEPLAN_HPP
#define OPENMW_COMPONENTS_FX_NATIVEPLAN_HPP

#include "technique.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>

namespace Fx
{
    struct NativeParameter
    {
        std::size_t uniformIndex = 0;
        std::size_t offset = 0;
        std::size_t stride = 0;
        std::size_t count = 0;
    };

    struct NativePass
    {
        std::string name;
        std::string target;
        VulkanShaderSources shaders;
        std::optional<osg::BlendFunc::BlendFuncMode> blendSource;
        std::optional<osg::BlendFunc::BlendFuncMode> blendDestination;
        std::optional<osg::BlendEquation::Equation> blendEquation;
    };

    // An immutable load/reload-time plan, not an evaluated scene capture.
    // Texture/image references are importer products, used only when Vulkan
    // uploads the asset. No OSG programs, state sets or render traversal survive.
    struct NativeTechnique
    {
        std::string name;
        std::size_t flags = 0;
        bool hdr = false;
        bool lights = false;
        std::vector<NativePass> passes;
        std::vector<osg::ref_ptr<osg::Texture>> textures;
        std::map<std::string, Types::RenderTarget> targets;
        std::vector<NativeParameter> parameters;
        std::size_t parameterBytes = 16;
    };

    struct NativeChain
    {
        std::uint64_t generation = 0;
        std::vector<NativeTechnique> techniques;
    };

    struct NativeFrame
    {
        std::shared_ptr<const NativeChain> chain;
        std::vector<char> state;
        std::vector<std::vector<char>> parameters;
        bool enabled = false;
        bool interior = false;
        bool underwater = false;
        float exposureSpeed = 1.0f;
    };

    inline NativeTechnique makeNativeTechnique(Technique& technique)
    {
        NativeTechnique result;
        result.name = technique.getName();
        result.flags = technique.getFlags();
        result.hdr = technique.getHDR();
        result.lights = technique.getLights();
        result.textures = technique.getTextures();
        for (const auto& [name, target] : technique.getRenderTargetsMap())
            result.targets.emplace(name, target);
        for (const auto& pass : technique.getPasses())
            result.passes.push_back({pass->getName(), pass->getTarget(), pass->getVulkanSources(technique),
                pass->getBlendSource(), pass->getBlendDest(), pass->getBlendEquation()});
        std::size_t cursor = 0;
        const auto& uniforms = technique.getUniformMap();
        for (std::size_t i = 0; i < uniforms.size(); ++i)
        {
            const auto& uniform = *uniforms[i];
            if (uniform.mSamplerType) continue;
            std::visit([&](const auto& value)
            {
                using T = typename std::decay_t<decltype(value)>::value_type;
                if (!value.isArray() && uniform.mStatic
                    && Settings::ShaderManager::get().getMode() != Settings::ShaderManager::Mode::Debug) return;
                constexpr std::size_t bytes = std::is_same_v<T, bool> ? 4 : sizeof(T);
                const std::size_t alignment = value.isArray() || bytes >= 12 ? 16 : bytes;
                cursor = (cursor + alignment - 1) / alignment * alignment;
                const auto stride = value.isArray() ? (bytes + 15) / 16 * 16 : bytes;
                const auto count = uniform.getNumElements();
                result.parameters.push_back({i, cursor, stride, count});
                cursor += stride * count;
            }, uniform.mData);
        }
        result.parameterBytes = std::max<std::size_t>(16, (cursor + 15) / 16 * 16);
        return result;
    }

    inline std::vector<char> packNativeParameters(const NativeTechnique& plan, Technique& technique)
    {
        std::vector<char> result(plan.parameterBytes, 0);
        for (const auto& field : plan.parameters)
        {
            const auto& uniform = technique.getUniformMap().at(field.uniformIndex);
            std::visit([&](const auto& value)
            {
                using T = typename std::decay_t<decltype(value)>::value_type;
                const auto copy = [&](std::size_t index, T item)
                {
                    const auto offset = field.offset + index * field.stride;
                    if constexpr (std::is_same_v<T, bool>)
                    {
                        const std::int32_t integer = item ? 1 : 0;
                        if (offset + sizeof(integer) > result.size()) throw std::logic_error("Stale FX parameter layout");
                        std::memcpy(result.data() + offset, &integer, sizeof(integer));
                    }
                    else
                    {
                        if (offset + sizeof(T) > result.size()) throw std::logic_error("Stale FX parameter layout");
                        std::memcpy(result.data() + offset, &item, sizeof(T));
                    }
                };
                if (value.isArray())
                {
                    if (value.getArray().size() != field.count) throw std::logic_error("FX array size changed without reload");
                    for (std::size_t i = 0; i < field.count; ++i) copy(i, value.getArray()[i]);
                }
                else copy(0, value.getValue());
            }, uniform->mData);
        }
        return result;
    }
}
#endif
