#include <components/render/backend/vsg/persistentpipelinecache.hpp>
#include <components/render/backend/vsg/livetextureimages.hpp>
#include <components/render/backend/vsg/vsgsubmission.hpp>
#include <components/render/backend/vsg/frameresourcepool.hpp>
#include <vsg/nodes/StateGroup.h>
#include <vsg/utils/GraphicsPipelineConfigurator.h>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <components/misc/environmentflag.hpp>
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main()
{
    try
    {
        const auto flagBegin = std::chrono::steady_clock::now();
        unsigned selected = 0;
        for (unsigned i=0;i<100000;++i) selected += Misc::environmentFlag<"OPENMW_V4_STARTUP_FLAG_CACHE">();
        require(selected == (std::getenv("OPENMW_V4_STARTUP_FLAG_CACHE") ? 100000u : 0u), "flag selection changed");
        std::cout << "FLAG lookups=100000 ms=" << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-flagBegin).count() << '\n';
        RenderVsg::FrameResourcePool<int> pool(3);
        pool.beginFrame(RenderCore::FrameId(1), {});
        pool.acquire("static") = 7;
        require(pool.markSubmitted(RenderCore::FrameId(1)), "first pool submission");
        for (unsigned frame = 2; frame < 8; ++frame)
        {
            pool.beginFrame(RenderCore::FrameId(frame), {});
            auto value = pool.selectUnchanged("static", [](int v) { return v == 7; });
            require(value && *value == 7 && pool.size() == 1, "immutable in-flight version duplicated");
            bool duplicate = false;
            try { pool.acquire("static"); } catch (const std::invalid_argument&) { duplicate = true; }
            require(duplicate, "read-sharing allowed a duplicate writable acquisition");
            require(pool.markSubmitted(RenderCore::FrameId(frame)), "shared submission");
        }
        pool.beginFrame(RenderCore::FrameId(8), {});
        require(!pool.selectUnchanged("static", [](int v) { return v == 8; }), "changed value shared");
        pool.acquire("static") = 8;
        require(pool.size() == 2 && pool.markSubmitted(RenderCore::FrameId(8)), "copy-on-write ring lost");
        pool.beginFrame(RenderCore::FrameId(9), RenderCore::FrameId(1)); pool.collectUnused();
        require(pool.size() == 2, "read-shared version retired using first rather than last use");
        pool.beginFrame(RenderCore::FrameId(10), RenderCore::FrameId(7)); pool.collectUnused();
        require(pool.size() == 1, "completed disappeared version retained");
        pool.beginFrame(RenderCore::FrameId(11), RenderCore::FrameId(8)); pool.collectUnused();
        require(pool.size() == 0, "final disappeared version retained");
        auto root = vsg::Group::create(), sharedGraph = vsg::Group::create();
        auto state = vsg::StateGroup::create();
        state->add(vsg::BindGraphicsPipeline::create()); sharedGraph->addChild(state);
        root->addChild(sharedGraph); root->addChild(sharedGraph);
        auto view = vsg::View::create();
        auto audit = RenderVsg::auditGraphicsPipelinesForView(*root, *view);
        const bool dedup = std::getenv("OPENMW_V4_DEDUP_PIPELINE_AUDIT")
            || std::getenv("OPENMW_V4_FLAT_PIPELINE_AUDIT");
        require(!audit.valid() && audit.bindings == (dedup ? 1 : 2), "DAG audit omitted failure or repeated shared work");
        auto added = vsg::StateGroup::create(); added->add(vsg::BindGraphicsPipeline::create());
        sharedGraph->addChild(added);
        audit = RenderVsg::auditGraphicsPipelinesForView(*root, *view);
        require(!audit.valid() && audit.bindings == (std::getenv("OPENMW_V4_PIPELINE_INVENTORIES") ? 1 : dedup ? 2 : 4), "fresh audit missed graph mutation");
        auto otherView = vsg::View::create();
        require(!RenderVsg::auditGraphicsPipelinesForView(*root, *otherView).valid(), "audit cached across views");
        // Sealed topology must preserve failures, including null bindings and
        // later view identities, while avoiding deep mesh/descriptor traversal.
        auto sealed = RenderVsg::sealPipelineInventory(root);
        require(!RenderVsg::auditGraphicsPipelinesForView(*sealed, *view).valid(), "inventory hid missing pipeline");
        require(!RenderVsg::auditGraphicsPipelinesForView(*sealed, *otherView).valid(), "inventory trusted another view");
        // Many mostly-unshared nodes expose allocation overhead that the
        // two-placement DAG fixture cannot. Report workload cost, never FPS.
        auto wide = vsg::Group::create();
        for (unsigned i = 0; i < 12000; ++i)
        {
            auto group = vsg::Group::create();
            for (unsigned j = 0; j < 5; ++j) group->addChild(vsg::Group::create());
            group->addChild(sharedGraph); wide->addChild(group);
        }
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 20; ++i)
            require(!RenderVsg::auditGraphicsPipelinesForView(*wide, *view).valid(), "large audit omitted null pipeline");
        std::cout << "AUDIT nodes=72000 repetitions=20 ms="
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() << '\n';
        RenderVsg::PersistentShaderFamilies families;
        for (const bool bump : {false,true})
            for (unsigned family = 0; family < 3; ++family)
            {
                auto shader = families.get(family == 1, family == 2, bump);
                require(shader && shader == families.get(family == 1, family == 2, bump), "shader family not reused");
                auto other = families.get(family != 1, false, !bump);
                require(shader != other, "different shader facets merged");
            }
        RenderVsg::PersistentPipelineCache pipelines(2);
        auto shader = families.get(false, false, false);
        auto first = vsg::GraphicsPipelineConfigurator::create(shader); first->init();
        auto second = vsg::GraphicsPipelineConfigurator::create(shader); second->init();
        auto shared = pipelines.get(first->graphicsPipeline);
        require(shared == pipelines.get(second->graphicsPipeline), "identical pipelines not reused");
        auto different = vsg::GraphicsPipelineConfigurator::create(families.get(false,true,false)); different->init();
        require(shared != pipelines.get(different->graphicsPipeline), "different shader pipeline merged");
        auto third = vsg::GraphicsPipelineConfigurator::create(families.get(false,false,true)); third->init();
        pipelines.get(third->graphicsPipeline);
        require(pipelines.size() == 2 && shared, "bounded eviction invalidated a live pipeline");
        RenderVsg::LiveTextureImages images(2);
        auto pixels = vsg::ubvec4Array2D::create(2,2);
        pixels->properties.format = VK_FORMAT_R8G8B8A8_UNORM;
        auto sampler = vsg::Sampler::create();
        auto image = images.get(pixels, sampler);
        require(image == images.get(pixels, sampler), "immutable UI texture allocated twice");
        auto replacement = vsg::ubvec4Array2D::create(2,2);
        replacement->properties.format = VK_FORMAT_R8G8B8A8_UNORM;
        require(image != images.get(replacement, sampler), "updated texture backing reused old pixels");
        auto otherSampler = vsg::Sampler::create(); otherSampler->maxLod = 5;
        require(image != images.get(pixels, otherSampler), "sampler allocation contract ignored");
        pixels->properties.dataVariance = vsg::DYNAMIC_DATA;
        require(images.get(pixels,sampler) != images.get(pixels,sampler), "mutable image entered immutable cache");
        std::cout << "PASS persistent shader families, pipeline identity/eviction, immutable images and replacement\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
