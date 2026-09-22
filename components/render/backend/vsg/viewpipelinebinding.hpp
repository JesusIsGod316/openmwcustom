#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VIEWPIPELINEBINDING_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VIEWPIPELINEBINDING_H

#include <vsg/app/View.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/vk/Context.h>
#include <cstdlib>
#include <map>
#include <string_view>

namespace RenderVsg
{
    // VSG 1.1.15 shares implementations across views by pipeline states alone.
    // Render-pass compatibility and the shader-stage traversal mask must also
    // participate. This inert state extends that cache key without changing
    // Vulkan rasterization state or requiring a patched dependency DLL.
    class ViewPipelineKey final : public vsg::Inherit<vsg::GraphicsPipelineState, ViewPipelineKey>
    {
    public:
        vsg::ref_ptr<vsg::RenderPass> renderPass;
        vsg::Mask traversalMask;
        explicit ViewPipelineKey(vsg::Context& context)
            : renderPass(context.renderPass), traversalMask(context.mask) {}
        int compare(const vsg::Object& object) const override
        {
            if (const int result = vsg::GraphicsPipelineState::compare(object)) return result;
            const auto& rhs = static_cast<const ViewPipelineKey&>(object);
            if (renderPass.get() != rhs.renderPass.get())
                return std::less<const vsg::RenderPass*>{}(renderPass.get(), rhs.renderPass.get()) ? -1 : 1;
            return vsg::compare_value(traversalMask, rhs.traversalMask);
        }
        void apply(vsg::Context&, VkGraphicsPipelineCreateInfo&) const override {}
    };

    class ViewPipelineCache final : public vsg::Inherit<vsg::Object, ViewPipelineCache>
    {
    public:
        struct Entry
        {
            vsg::observer_ptr<vsg::View> view;
            vsg::ref_ptr<ViewPipelineKey> key;
        };
        std::map<std::uint32_t, Entry> views;
    };

    class ViewPipelineBinding final : public vsg::Inherit<vsg::BindGraphicsPipeline, ViewPipelineBinding>
    {
    public:
        explicit ViewPipelineBinding(vsg::ref_ptr<vsg::GraphicsPipeline> pipeline) : Inherit(std::move(pipeline)) {}
        static void prepareCache(vsg::GraphicsPipeline& pipeline)
        {
            // Establish the auxiliary comparison key BEFORE SharedObjects
            // interns this pipeline. The cache contents are not compared.
            if (!pipeline.getObject<ViewPipelineCache>("openmw.viewPipelineCache"))
                pipeline.setObject("openmw.viewPipelineCache", ViewPipelineCache::create());
        }
        void compile(vsg::Context& context) override
        {
            // Unsafe historical control is for isolated validation tests only.
            const char* control = std::getenv("OPENMW_V4_UNSAFE_PIPELINE_CACHE_CONTROL");
            if (control && std::string_view(control) == "1")
            {
                vsg::BindGraphicsPipeline::compile(context);
                return;
            }
            constexpr const char* CacheKey = "openmw.viewPipelineCache";
            vsg::ref_ptr<ViewPipelineCache> cache(pipeline->getObject<ViewPipelineCache>(CacheKey));
            if (!cache)
            {
                cache = ViewPipelineCache::create();
                pipeline->setObject(CacheKey, cache);
            }
            auto key = ViewPipelineKey::create(context);
            auto& entry = cache->views[context.viewID];
            auto view = context.view.ref_ptr();
            if (!entry.key || entry.view.ref_ptr() != view || entry.key->compare(*key) != 0)
            {
                // A retired map's numeric view ID can be reused. An existing
                // non-null implementation is not proof of compatibility.
                pipeline->release(context.viewID);
                entry.view = vsg::observer_ptr<vsg::View>(view);
                entry.key = key;
            }
            context.overridePipelineStates.push_back(entry.key);
            try { vsg::BindGraphicsPipeline::compile(context); }
            catch (...) { context.overridePipelineStates.pop_back(); throw; }
            context.overridePipelineStates.pop_back();
        }
    };
}
#endif
