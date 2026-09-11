#include <components/render/backend/vsg/enchantedmaterialshader.hpp>

#include <vsg/all.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <vsg/utils/ShaderCompiler.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
            throw std::runtime_error(std::string(message));
    }

    [[nodiscard]] const vsg::ShaderStage* findStage(
        const vsg::ShaderSet& shaderSet, VkShaderStageFlagBits stage)
    {
        for (const auto& candidate : shaderSet.stages)
        {
            if (candidate && candidate->stage == stage)
                return candidate.get();
        }
        return nullptr;
    }
}

int main()
{
    using namespace RenderCore;

    MaterialRecord material;
    material.environmentMapColor = { 0.25f, 0.5f, 0.75f, 1.0f };
    material.environmentMapStrength = 0.5f;
    material.environmentMapPreLight = true;
    auto environmentUniform = RenderVsg::makeEnchantedEnvironmentMaterial(material);
    require(static_cast<bool>(environmentUniform), "failed to create enchanted environment uniform");
    const auto& color = environmentUniform->value().colorStrength;
    require(color.x == 0.125f && color.y == 0.25f && color.z == 0.375f && color.w == 0.5f,
        "enchanted environment color/strength packing changed");
    require(environmentUniform->value().effects.x == 1.0f,
        "enchanted pre-light material semantic was not packed into the backend uniform");

    auto shaderSet = RenderVsg::createEnchantedLegacyCompatibilityShaderSet();
    require(static_cast<bool>(shaderSet),
        "enchanted ShaderSet could not patch the pinned VSG 1.1.15 Phong vertex contract");
    require(shaderSet->optionalDefines.contains("OPENMW_ENCHANTED_ENVIRONMENT"),
        "enchanted ShaderSet lost its explicit feature define");

    const auto& mapsBinding = shaderSet->getDescriptorBinding("openmwEnvironmentMaps");
    require(static_cast<bool>(mapsBinding)
            && mapsBinding.set == 1u
            && mapsBinding.binding == RenderVsg::EnchantedEnvironmentTextureBinding
            && mapsBinding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            && mapsBinding.descriptorCount == RenderVsg::EnchantedEnvironmentFrameCount
            && mapsBinding.define == "OPENMW_ENCHANTED_ENVIRONMENT",
        "enchanted 32-frame descriptor-array contract changed");

    const auto& effectBinding = shaderSet->getDescriptorBinding("openmwEnvironmentEffect");
    require(static_cast<bool>(effectBinding)
            && effectBinding.set == 1u
            && effectBinding.binding == RenderVsg::EnchantedEnvironmentUniformBinding
            && effectBinding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
            && effectBinding.descriptorCount == 1u
            && effectBinding.define == "OPENMW_ENCHANTED_ENVIRONMENT",
        "enchanted environment-effect descriptor contract changed");

    const vsg::ShaderStage* vertex = findStage(*shaderSet, VK_SHADER_STAGE_VERTEX_BIT);
    const vsg::ShaderStage* fragment = findStage(*shaderSet, VK_SHADER_STAGE_FRAGMENT_BIT);
    require(vertex && vertex->module && fragment && fragment->module,
        "enchanted ShaderSet is missing its patched vertex or fragment stage");
    require(vertex->module->source.find("layout(location = 8) out vec2 openmwEnvironmentUv") != std::string::npos
            && vertex->module->source.find("openmwEnvironmentUv = vec2") != std::string::npos,
        "enchanted vertex sphere-map realization is absent");
    require(fragment->module->source.find("binding = 14) uniform sampler2D openmwEnvironmentMaps[32]")
                != std::string::npos
            && fragment->module->source.find("binding = 15) uniform OpenMwEnvironmentEffectData")
                != std::string::npos
            && fragment->module->source.find("openmwEnvironment.temporalEffects.x") != std::string::npos
            && fragment->module->source.find("surfaceColor.rgb += openmwEnvEffect") != std::string::npos
            && fragment->module->source.find("outColor.rgb += openmwEnvEffect") != std::string::npos,
        "enchanted descriptor, clock, or pre/post-light realization is absent");

    vsg::ShaderCompiler compiler;
    require(compiler.supported(), "VSG was built without GLSL compiler support");

    auto basicHints = vsg::ShaderCompileSettings::create();
    basicHints->defines = { "OPENMW_ENCHANTED_ENVIRONMENT", "VSG_TEXTURECOORD_0" };
    auto basicStages = shaderSet->getShaderStages(basicHints);
    require(compiler.compile(basicStages),
        "enchanted compatibility shader failed GLSL compilation without a normal map");

    auto normalHints = vsg::ShaderCompileSettings::create();
    normalHints->defines = { "OPENMW_ENCHANTED_ENVIRONMENT", "VSG_TEXTURECOORD_0", "VSG_DIFFUSE_MAP",
        "VSG_NORMAL_MAP" };
    auto normalStages = shaderSet->getShaderStages(normalHints);
    require(compiler.compile(normalStages),
        "enchanted compatibility shader failed GLSL compilation with the per-fragment normal-map path");

    auto configurator = vsg::GraphicsPipelineConfigurator::create(shaderSet);
    require(static_cast<bool>(configurator), "failed to allocate enchanted pipeline configurator");
    require(configurator->assignDescriptor("openmwEnvironmentEffect", environmentUniform),
        "enchanted pipeline rejected its color/strength descriptor");

    auto sampler = vsg::Sampler::create();
    auto image = vsg::ubvec4Array2D::create(1u, 1u);
    image->set(0u, 0u, vsg::ubvec4(255u, 255u, 255u, 255u));
    image->properties.format = VK_FORMAT_R8G8B8A8_SRGB;
    image->properties.mipLevels = 1u;
    vsg::ImageInfoList frames;
    frames.reserve(RenderVsg::EnchantedEnvironmentFrameCount);
    for (std::size_t i = 0; i < RenderVsg::EnchantedEnvironmentFrameCount; ++i)
        frames.push_back(vsg::ImageInfo::create(sampler, image));
    require(configurator->assignTexture("openmwEnvironmentMaps", frames),
        "enchanted pipeline rejected its exact 32-frame image array");
    configurator->init();
    require(configurator->shaderHints
            && configurator->shaderHints->defines.contains("OPENMW_ENCHANTED_ENVIRONMENT"),
        "descriptor assignment did not activate the enchanted shader define");
    require(configurator->descriptorConfigurator
            && configurator->descriptorConfigurator->descriptorSets.size() > 1u
            && configurator->descriptorConfigurator->descriptorSets[1]
            && configurator->descriptorConfigurator->descriptorSets[1]->setLayout,
        "enchanted material descriptor set was not constructed");

    const auto& bindings = configurator->descriptorConfigurator->descriptorSets[1]->setLayout->bindings;
    const auto mapsLayout = std::find_if(bindings.begin(), bindings.end(), [](const VkDescriptorSetLayoutBinding& binding) {
        return binding.binding == RenderVsg::EnchantedEnvironmentTextureBinding;
    });
    const auto effectLayout = std::find_if(bindings.begin(), bindings.end(), [](const VkDescriptorSetLayoutBinding& binding) {
        return binding.binding == RenderVsg::EnchantedEnvironmentUniformBinding;
    });
    require(mapsLayout != bindings.end()
            && mapsLayout->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
            && mapsLayout->descriptorCount == RenderVsg::EnchantedEnvironmentFrameCount,
        "VSG configurator did not preserve the 32-image descriptor layout");
    require(effectLayout != bindings.end()
            && effectLayout->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
            && effectLayout->descriptorCount == 1u,
        "VSG configurator did not preserve the enchanted effect uniform layout");

    return 0;
}
