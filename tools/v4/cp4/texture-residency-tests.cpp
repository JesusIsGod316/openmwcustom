#include <components/render/backend/vsg/vsgsemanticsession.hpp>
#include <components/render/backend/vsg/immediateeffectrealizer.hpp>
#include <components/render/backend/vsg/livetexturecache.hpp>
#include <components/render/backend/vsg/allocationdiagnostics.hpp>
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
}

int main(int argc, char** argv)
{
    try
    {
        using namespace RenderCore;
        const bool stable = argc == 2 && std::string_view(argv[1]) == "--stable";
        unsigned decodes = 0;
        auto resolver = RenderVsg::cacheLiveTextures([&](const TextureRecord&, const TextureRealizationKey&) {
            ++decodes;
            auto pixels = vsg::ubvec4Array2D::create(256, 256);
            pixels->properties.format = VK_FORMAT_R8G8B8A8_SRGB;
            for (auto& pixel : *pixels) pixel = vsg::ubvec4(160, 100, 50, 255);
            return pixels;
        });
        ImmediateEffectDraw draw;
        draw.identity = "residency:effect";
        draw.mesh.positions = {{-1, -1, -5}, {1, -1, -5}, {0, 1, -5}};
        draw.mesh.normals.resize(3, {0, 0, 1});
        draw.mesh.texCoordSets = {{{0, 0}, {1, 0}, {0, 1}}};
        draw.mesh.indices = {0, 1, 2};
        draw.mesh.surfaces = {{PrimitiveTopology::Triangles, 0, 3, 0}};
        draw.bounds = {{-1, -1, -5}, {1, 1, -5}};
        EffectTextureSnapshot texture;
        texture.texture.sourceIdentity = "textures/residency.dds";
        texture.texture.contentIdentity = "residency:pixels";
        texture.texture.width = texture.texture.height = 256;
        draw.textures.push_back(texture);

        // Independent frame/actor configurators: global SharedObjects must not
        // retain their mutable geometry in order to share an immutable texture.
        auto graphs = vsg::Group::create();
        for (unsigned i = 0; i < 96; ++i)
        {
            draw.material.alpha = 0.8f + float(i) * 0.001f;
            auto realized = RenderVsg::realizeImmediateEffectDraw(draw, resolver, vsg::SharedObjects::create());
            require(realized.valid(), realized.diagnostic.c_str());
            graphs->addChild(realized.root);
        }
        vsg::CollectResourceRequirements requirements;
        graphs->accept(requirements);
        std::unordered_set<const vsg::Image*> images;
        for (const auto& info : requirements.requirements.imageInfos) images.insert(info->imageView->image.get());
        std::cout << "Independent graphs=96 decoded_payloads=" << decodes << " GPU_images=" << images.size() << '\n';
        require(decodes == 1, "production realizer repeatedly decoded the same immutable content");

        require(SDL_Init(SDL_INIT_VIDEO), "SDL initialization failed");
        RenderVsg::VsgRuntimeBootstrapOptions options;
        options.title = "OpenMW texture residency regression";
        options.width = 320; options.height = 240;
        options.presentMode = RenderVsg::VsgPresentMode::Immediate;
        options.host.shadows.enabled = true;
        options.host.shadows.mapResolution = 256;
        options.host.water.enabled = true;
        options.host.water.targetSize = 64;
        auto session = RenderVsg::VsgSemanticSession::create(resolver, options);
        auto device = session->bootstrap().vsgWindow()->getOrCreateDevice();
        SingleViewFrameInput input;
        input.renderExtent = {320, 240}; input.outputExtent = input.renderExtent;
        input.environment.skyEnabled = false; input.environment.sunLightEnabled = false;
        input.environment.sunVisible = false;
        for (unsigned i = 0; i < 96; ++i)
        {
            draw.identity = "residency:" + std::to_string(i);
            draw.textures.resize(1);
            // Interleave layouts with different texture counts, like the
            // prison ship's plain, normal-mapped and glow-mapped materials.
            if (i % 3 != 0)
            {
                auto normal = texture;
                normal.binding.role = TextureRole::Normal;
                draw.textures.push_back(normal);
            }
            if (i % 3 == 2)
            {
                auto glow = texture;
                glow.binding.role = TextureRole::Emissive;
                draw.textures.push_back(glow);
            }
            input.immediateEffectDraws.push_back(draw);
        }
        auto pools = device->deviceMemoryBufferPools.ref_ptr();
        std::uint64_t warmBytes = 0, finalBytes = 0;
        const unsigned frameCount = stable ? 12 : 150;
        for (unsigned frame = 0; frame < frameCount; ++frame)
        {
            // Force new material/layout realizations as particle systems do,
            // while retaining exactly the same immutable sampled image.
            for (unsigned i = 0; i < input.immediateEffectDraws.size(); ++i)
            {
                auto& effect = input.immediateEffectDraws[i];
                if (!stable)
                    effect.material.alpha = (frame % 2 ? 0.7f : 0.8f) + float(i) * 0.001f;
                // Change mutable geometry even on reuse-only frames.
                effect.mesh.positions[0].x = frame % 2 ? -0.9f : -1.f;
            }
            if (stable && frame == 5)
                input.immediateEffectDraws[0].material.alpha = 0.42f;
            require(session->renderFrame(input) == RenderFrameResult::Presented,
                session->lastDiagnostic().c_str());
            finalBytes = pools->computeMemoryTotalReserved() + pools->computeMemoryTotalAvailable();
            if (frame == (stable ? 3u : 29u)) warmBytes = finalBytes;
            if (stable)
            {
                const auto compiled = session->bootstrap().renderer().lastCompiledDynamicRootCount();
                require(compiled == (frame == 0 ? 96u : (frame == 5 ? 1u : 0u)),
                    "dynamic compile revisited residents or omitted changed material");
                // Deterministic completion makes reuse of the same resident
                // version testable; this is not a performance benchmark.
                session->waitIdle();
            }
            if (frame % 10 == 0)
                std::cout << "completed frame " << frame + 1 << '/' << frameCount << std::endl;
        }
        session->bootstrap().renderer().waitIdle();
        std::cout << frameCount << " real Vulkan frames, warm_pool_bytes=" << warmBytes
                  << " final_pool_bytes=" << finalBytes << '\n';
        session.reset(); pools = {}; device = {};
        SDL_Quit();
        require(images.size() == 1, "independent production realizations duplicated GPU texture images");
        require(finalBytes <= warmBytes + 8 * 1024 * 1024, "texture/layout churn caused unbounded pool growth");
        std::cout << "PASS production texture residency and GPU churn\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
