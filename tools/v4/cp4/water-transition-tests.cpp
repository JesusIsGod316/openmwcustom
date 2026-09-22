#include <components/render/backend/vsg/vsgsemanticsession.hpp>
#include <components/debug/gameplaydiagnostics.hpp>
#include <vsg/app/RenderGraph.h>
#include <SDL3/SDL.h>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    void checkLateShadowStateSlots()
    {
        auto viewer = vsg::Viewer::create();
        auto view = vsg::View::create();
        auto shadows = vsg::CommandGraph::create();
        shadows->maxSlots.state = 1; // GUI/uniform-only startup.
        view->viewDependentState->preRenderCommandGraph = shadows;
        vsg::CompileResult result;
        result.result = VK_SUCCESS;
        result.maxSlots.state = 2; // Material descriptor set 1.
        result.views[view.get()];
        // Empty task list also checks propagation when the top-level viewer
        // has no resource update to perform. Nested graphs are independent.
        RenderVsg::updateViewerAfterCompile(*viewer, result);
        require(shadows->maxSlots.state == 2, "late material slot did not reach shadow command graph");
        result.maxSlots.state = 1;
        RenderVsg::updateViewerAfterCompile(*viewer, result);
        require(shadows->maxSlots.state == 2, "GUI publication shrank shadow state slots");
        result.result = VK_ERROR_INITIALIZATION_FAILED;
        result.maxSlots.state = 5;
        RenderVsg::updateViewerAfterCompile(*viewer, result);
        require(shadows->maxSlots.state == 2, "failed compilation changed shadow state slots");
    }

    class RegistrationFailureState final : public vsg::Inherit<vsg::ViewDependentState, RegistrationFailureState>
    {
    public:
        explicit RegistrationFailureState(vsg::View* view) : Inherit(view) {}
        void compile(vsg::Context&) override { throw std::runtime_error("injected registration failure"); }
    };

    void checkRegistrationLifetime(vsg::Device* device)
    {
        auto viewer = vsg::Viewer::create();
        auto manager = RenderVsg::ViewCompileManager::create(*viewer, vsg::ref_ptr<vsg::ResourceHints>{});
        auto target = RenderVsg::createOffscreenRenderTarget(device, {32, 32});
        require(static_cast<bool>(target), "registration fixture framebuffer");
        auto parent = vsg::View::create();
        auto shadow = vsg::View::create();
        parent->viewDependentState->shadowMaps.push_back({target.renderGraph, shadow});
        auto registration = manager->registerFramebufferView(*target.renderGraph->framebuffer, parent);
        require(manager->contextCount() == 2, "parent/shadow contexts not both registered");
        auto survivor = vsg::View::create();
        auto survivorRegistration = manager->registerFramebufferView(*target.renderGraph->framebuffer, survivor);
        require(manager->contextCount() == 3, "second view registration missing");
        vsg::observer_ptr<vsg::View> parentObserver(parent);
        vsg::observer_ptr<vsg::View> shadowObserver(shadow);
        parent = {}; shadow = {};
        require(static_cast<bool>(parentObserver.ref_ptr()), "registration did not retain its view");
        registration.reset();
        require(!parentObserver.ref_ptr() && !shadowObserver.ref_ptr(), "retired view/shadow lifetime leaked");
        require(manager->contextCount() == 1, "retirement did not preserve only the unrelated view");
        survivorRegistration.reset();
        require(manager->contextCount() == 0, "registration contexts leaked");

        bool caught = false;
        try
        {
            auto pending = manager->registerFramebufferView(*target.renderGraph->framebuffer, survivor);
            throw std::runtime_error("injected publication failure");
        }
        catch (const std::runtime_error&) { caught = true; }
        require(caught && manager->contextCount() == 0, "failed publication did not roll back contexts");

        auto failingView = vsg::View::create();
        auto failingState = RegistrationFailureState::create(failingView.get());
        failingView->viewDependentState = failingState;
        failingState->shadowMaps.push_back({vsg::RenderGraph::create(), vsg::View::create()});
        caught = false;
        try
        {
            auto pending = manager->registerFramebufferView(*target.renderGraph->framebuffer, failingView);
        }
        catch (const std::runtime_error& e) { caught = std::string_view(e.what()) == "injected registration failure"; }
        require(caught && manager->contextCount() == 0, "partial registration did not roll back contexts");
        // Acquiring the queue again also verifies the exception returned every
        // traversal instead of leaving a subsequent registration deadlocked.
        auto retry = manager->registerFramebufferView(*target.renderGraph->framebuffer, survivor);
        require(manager->contextCount() == 1, "registration retry after rollback failed");
    }
}

#include "water-pixel-tests.hpp"

int main(int argc, char** argv)
{
    try
    {
        checkLateShadowStateSlots();
        using namespace RenderCore;
        require(SDL_Init(SDL_INIT_VIDEO), "SDL initialization failed");
        RenderVsg::VsgRuntimeBootstrapOptions options;
        options.title = "OpenMW active water transition regression";
        options.width = 320; options.height = 240;
        options.presentMode = RenderVsg::VsgPresentMode::Immediate;
        options.host.shadows.enabled = true;
        options.host.shadows.mapResolution = 256;
        options.host.water.enabled = true;
        options.host.water.reflection = true;
        options.host.water.refraction = true;
        options.host.water.targetSize = 64;
        auto session = RenderVsg::VsgSemanticSession::create(
            [](const TextureRecord&, const TextureRealizationKey&) { return vsg::ref_ptr<vsg::Data>{}; }, options);
        checkRegistrationLifetime(session->bootstrap().vsgWindow()->getOrCreateDevice());
        checkWaterPixels(session->bootstrap().vsgWindow()->getOrCreateDevice());
        // Isolate framebuffer rendering from swapchain presentation validation.
        // The default still runs all gameplay-style transition checks below.
        if (argc == 2 && std::string_view(argv[1]) == "--pixels-only")
        {
            session.reset();
            SDL_Quit();
            std::cout << "PASS offscreen-only water/sky/GUI and view lifetime checks\n";
            return 0;
        }
        auto& world = session->world();
        auto addObject = [&](float alpha) {
            auto material = world.reserveMaterial();
            MaterialRecord materialRecord;
            materialRecord.sourceIdentity = "water-transition:material";
            materialRecord.alpha = alpha;
            require(material && world.commit(*material, materialRecord), "material publication");
            auto mesh = world.reserveMesh();
            auto geometry = std::make_shared<MeshPayload>();
            geometry->positions = {{-1, -1, -5}, {1, -1, -5}, {0, 1, -5}};
            geometry->normals.resize(3, {0, 0, 1});
            geometry->indices = {0, 1, 2};
            geometry->surfaces = {{PrimitiveTopology::Triangles, 0, 3, 0}};
            MeshRecord meshRecord;
            meshRecord.sourceIdentity = "water-transition:mesh";
            meshRecord.payload = geometry; meshRecord.surfaceCount = 1;
            require(mesh && world.commit(*mesh, meshRecord), "mesh publication");
            auto model = world.reserveModel();
            auto payload = std::make_shared<ModelPayload>();
            ModelNodeRecord node;
            node.name = "water-transition:triangle"; node.kind = ModelNodeKind::Geometry;
            node.mesh = *mesh; node.materials = {*material};
            payload->nodes.push_back(node); payload->roots.push_back(ModelNodeIndex{0});
            ModelRecord modelRecord;
            modelRecord.sourceIdentity = "water-transition:model"; modelRecord.payload = payload;
            require(model && world.commit(*model, modelRecord), "model publication");
            auto instance = world.reserveInstance();
            InstanceRecord record; record.model = *model;
            require(instance && world.commit(*instance, record), "instance publication");
        };
        SingleViewFrameInput input;
        input.renderExtent = {320, 240}; input.outputExtent = input.renderExtent;
        input.environment.skyEnabled = false; input.environment.sunVisible = false;
        input.environment.sunLightEnabled = false;
        auto present = [&](const char* phase, bool gui = false) {
            static std::uint64_t frame = 0;
            Debug::GameplayDiagnostics::Frame diagnostic(++frame, true, false);
            const auto result = gui ? session->renderGuiFrame(input) : session->renderFrame(input);
            if (result != RenderFrameResult::Presented)
                throw std::runtime_error(std::string(phase) + ": " + session->lastDiagnostic());
            diagnostic.completed = true;
        };
        addObject(1.f);
        present("interior");
        // The actual hatch path retires the interior map before admitting new
        // exterior scenery. Do not replace this with direct exterior startup.
        auto& host = session->bootstrap().renderer();
        const auto initialContexts = host.compilationContextCount();
        for (int cycle = 0; cycle < 8; ++cycle)
        {
            SingleViewFrameInput::AuxiliaryView map;
            map.kind = ViewKind::Map;
            map.extent = {64, 64};
            map.colorFormat = RenderTargetFormat::Rgba8Srgb;
            map.sampledByMain = true;
            map.stableSlot = 0;
            input.auxiliaryViews = {map};
            present("create interior map");
            const auto mapTarget = RenderTargetHandle::fromParts(4, 1);
            require(static_cast<bool>(host.auxiliaryColorImage(mapTarget)), "map image was not published");
            require(host.compilationContextCount() > initialContexts, "map compilation context not registered");
            // Match the production local-map bridge, including its synchronous
            // GPU-to-CPU copy. Creating a map image alone does not test readback.
            for (int redraw = 0; redraw < 3; ++redraw)
            {
                present("redraw interior map");
                const auto images = host.readbackAuxiliaryRgba8(std::span(&mapTarget, 1));
                if (!images)
                    throw std::runtime_error("production map readback: " + host.lastDiagnostic());
                require(images->size() == 1 && images->front().rgba.size() == 64 * 64 * 4,
                    "map readback did not return the complete image");
            }
            input.auxiliaryViews.clear();
            require(host.retireAuxiliarySurface(mapTarget), "map retirement failed");
            require(!host.auxiliaryColorImage(mapTarget), "retired map image remains published");
            require(host.retireAuxiliarySurface(mapTarget), "repeated map retirement is not idempotent");
            addObject(0.8f + cycle * 0.02f);
            present("new scenery after map retirement");
            require(host.compilationContextCount() == initialContexts, "retired map compilation contexts accumulated");
        }
        input.environment.waterEnabled = true;
        input.environment.waterHeight = -2;
        // Cave policy: water present, no sky or outdoor sun/shadow policy.
        present("first active cave reflection/refraction");
        input.environment.underwater = true;
        present("underwater cave");
        input.environment.underwater = false;
        input.environment.skyEnabled = true;
        input.environment.sunLightEnabled = true;
        input.environment.sunVisible = true;
        input.environment.shadowsEnabled = true;
        present("outdoor water with sky and shadows");
        addObject(0.7f);
        present("new exterior pipeline while water active");
        // Exercise a multi-object publication, not just one-object updates.
        for (int object = 0; object < 64; ++object)
            addObject(0.7f + object * 0.001f);
        const auto batchStart = std::chrono::steady_clock::now();
        present("64-object exterior publication");
        std::cout << "64-object publication ms=" << std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - batchStart).count() << '\n';
        for (int cycle = 0; cycle < 3; ++cycle)
        {
            input.environment.waterEnabled = false;
            input.environment.skyEnabled = false;
            input.environment.sunVisible = false;
            input.environment.sunLightEnabled = false;
            input.environment.shadowsEnabled = false;
            present("interior return");
            addObject(0.5f + cycle * 0.05f);
            present("loading GUI", true);
            input.environment.waterEnabled = true;
            present("water reactivation with new pipeline");
            input.environment.underwater = true;
            present("underwater");
            input.environment.underwater = false;
            present("above water");
        }
        session->waitIdle();
        session.reset();
        SDL_Quit();
        std::cout << "PASS active water: eight map create/retire/new-scenery cycles without context growth, cave water and underwater, outdoor sky/shadows, late objects, GUI loading, repeated transitions\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL active water: " << e.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
