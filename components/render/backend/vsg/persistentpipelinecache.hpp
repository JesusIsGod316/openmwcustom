#ifndef OPENMW_RENDER_VSG_PERSISTENTPIPELINECACHE_H
#define OPENMW_RENDER_VSG_PERSISTENTPIPELINECACHE_H

#include "enchantedmaterialshader.hpp"
#include "legacybumpmaterialshader.hpp"
#include <algorithm>
#include <array>
#include <deque>

namespace RenderVsg
{
    // These caches deliberately cannot accept a GraphicsPipelineConfigurator:
    // that object owns frame arrays, descriptors and material data. Retaining
    // configurators across frames used to pin entire retired scene generations.
    class PersistentPipelineCache
    {
    public:
        explicit PersistentPipelineCache(std::size_t capacity = 256) : mCapacity(capacity) {}

        vsg::ref_ptr<vsg::GraphicsPipeline> get(vsg::ref_ptr<vsg::GraphicsPipeline> pipeline)
        {
            if (!pipeline || !mCapacity) return pipeline;
            // No ordered keys: compiling a ShaderModule populates its SPIR-V.
            // A sorted container would risk changing a key after insertion.
            const auto found = std::find_if(mPipelines.begin(), mPipelines.end(),
                [&](const auto& existing) { return existing->compare(*pipeline) == 0; });
            if (found != mPipelines.end())
            {
                auto result = *found;
                mPipelines.erase(found);
                mPipelines.push_back(result);
                return result;
            }
            if (mPipelines.size() == mCapacity) mPipelines.pop_front();
            mPipelines.push_back(pipeline);
            return pipeline;
        }

        std::size_t size() const { return mPipelines.size(); }

    private:
        const std::size_t mCapacity;
        std::deque<vsg::ref_ptr<vsg::GraphicsPipeline>> mPipelines;
    };

    class PersistentShaderFamilies
    {
    public:
        vsg::ref_ptr<vsg::ShaderSet> get(bool enchanted, bool sphere, bool bump)
        {
            const std::size_t family = sphere ? 4 : enchanted ? 2 : 0;
            auto& base = mSets[family];
            if (!base)
                base = family ? createEnchantedLegacyCompatibilityShaderSet({}, sphere)
                              : createLegacyCompatibilityShaderSet();
            if (!bump) return base;
            auto& variant = mSets[family + 1];
            if (!variant) variant = createLegacyBumpCompatibilityShaderSet(base);
            return variant;
        }

    private:
        // Six fixed built-in families; no material identities or mesh payloads.
        std::array<vsg::ref_ptr<vsg::ShaderSet>, 6> mSets;
    };
}
#endif
