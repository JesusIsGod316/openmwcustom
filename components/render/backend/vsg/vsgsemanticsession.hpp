#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGSEMANTICSESSION_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGSEMANTICSESSION_H

#include "vsgruntimebootstrap.hpp"

#include <components/nifrender/staticmodelcache.hpp>
#include <components/rendercore/activecellproducer.hpp>
#include <components/rendercore/frameproducer.hpp>

#include <memory>
#include <string>

namespace RenderVsg
{
    // Build-gated aggregate for the engine's distinct Vulkan route. The source
    // side publishes through the neutral services exposed here; backend/window
    // objects remain private to VsgRuntimeBootstrap. Member order guarantees
    // the runtime and its in-flight GPU resources are destroyed before logical
    // world bindings and source caches.
    class VsgSemanticSession final
    {
    public:
        [[nodiscard]] static std::unique_ptr<VsgSemanticSession> create(
            StaticTextureResolver textureResolver, VsgRuntimeBootstrapOptions options = {});

        ~VsgSemanticSession();
        VsgSemanticSession(const VsgSemanticSession&) = delete;
        VsgSemanticSession& operator=(const VsgSemanticSession&) = delete;

        [[nodiscard]] RenderCore::RenderWorld& world() noexcept { return mWorld; }
        [[nodiscard]] const RenderCore::RenderWorld& world() const noexcept { return mWorld; }
        [[nodiscard]] RenderCore::RenderWorldPublisher& publisher() noexcept { return mPublisher; }
        [[nodiscard]] NifRender::StaticModelCache& models() noexcept { return mModels; }
        [[nodiscard]] RenderCore::ActiveCellProducer& cells() noexcept { return mCells; }
        [[nodiscard]] VsgRuntimeBootstrap& bootstrap() noexcept { return *mBootstrap; }
        [[nodiscard]] const VsgRuntimeBootstrap& bootstrap() const noexcept { return *mBootstrap; }

        RenderCore::RenderFrameResult renderFrame(const RenderCore::SingleViewFrameInput& input);
        [[nodiscard]] bool resetWorld();
        void waitIdle();

        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        VsgSemanticSession(StaticTextureResolver textureResolver, VsgRuntimeBootstrapOptions options);

        RenderCore::RenderWorld mWorld;
        RenderCore::RenderWorldPublisher mPublisher;
        NifRender::StaticModelCache mModels;
        RenderCore::ActiveCellProducer mCells;
        RenderCore::SingleViewFrameProducer mFrames;
        std::string mLastDiagnostic;
        std::unique_ptr<VsgRuntimeBootstrap> mBootstrap;
    };
}

#endif
