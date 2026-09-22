#include <components/render/backend/vsg/vsgsemanticsession.hpp>
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
        require(SDL_Init(SDL_INIT_VIDEO), "SDL video initialization failed");
        RenderVsg::VsgRuntimeBootstrapOptions options;
        options.title = "OpenMW GUI isolation regression test";
        options.width = 320; options.height = 240;
        options.presentMode = RenderVsg::VsgPresentMode::Immediate;
        options.host.shadows.enabled = true;
        options.host.shadows.mapResolution = 256;
        options.host.water.enabled = true;
        options.host.water.targetSize = 64;
        const bool worldControl = argc == 2 && std::string_view(argv[1]) == "--world-control";
        auto session = RenderVsg::VsgSemanticSession::create(
            [](const RenderCore::TextureRecord&, const RenderCore::TextureRealizationKey&) {
                return vsg::ref_ptr<vsg::Data>{}; // This fixture has no textures.
            }, options);
        auto& world = session->world();
        auto& host = session->bootstrap().renderer();
        using namespace RenderCore;
        const auto material = world.reserveMaterial();
        MaterialRecord materialRecord;
        materialRecord.sourceIdentity = "gui-test:material";
        require(material && world.commit(*material, materialRecord), "material publication");
        const auto mesh = world.reserveMesh();
        auto vertices = std::make_shared<MeshPayload>();
        vertices->positions = {{-1.f, -1.f, -5.f}, {1.f, -1.f, -5.f}, {0.f, 1.f, -5.f}};
        vertices->normals.resize(3, {0.f, 0.f, 1.f});
        vertices->indices = {0, 1, 2};
        vertices->surfaces.push_back({PrimitiveTopology::Triangles, 0, 3, 0});
        MeshRecord meshRecord;
        meshRecord.sourceIdentity = "gui-test:triangle";
        meshRecord.payload = vertices; meshRecord.surfaceCount = 1;
        require(mesh && world.commit(*mesh, meshRecord), "mesh publication");
        const auto model = world.reserveModel();
        auto payload = std::make_shared<ModelPayload>();
        ModelNodeRecord node;
        node.name = "body"; node.kind = ModelNodeKind::Geometry;
        node.mesh = *mesh; node.materials = {*material};
        payload->nodes.push_back(node); payload->roots.push_back(ModelNodeIndex{0});
        ModelRecord modelRecord;
        modelRecord.sourceIdentity = "gui-test:model"; modelRecord.payload = payload;
        require(model && world.commit(*model, modelRecord), "model publication");
        auto addStatic = [&] {
            auto instance = world.reserveInstance();
            InstanceRecord record; record.model = *model;
            require(instance && world.commit(*instance, record), "static instance publication");
        };
        addStatic();
        SingleViewFrameInput input;
        input.renderExtent = {320, 240}; input.outputExtent = input.renderExtent;
        input.environment.skyEnabled = false; input.environment.sunLightEnabled = false;
        input.environment.sunVisible = false;
        auto present = [&](bool gui) {
            const auto result = gui && !worldControl ? session->renderGuiFrame(input) : session->renderFrame(input);
            if (result != RenderFrameResult::Presented)
                throw std::runtime_error("presentation failed: " + session->lastDiagnostic());
        };
        for (int i = 0; i < 8; ++i) present(true);
        require(host.residentStaticInstanceCount() == 0, "GUI realized pending world objects");
        present(false);
        require(host.residentStaticInstanceCount() == 1, "gameplay did not realize pending object");
        addStatic();
        for (int i = 0; i < 8; ++i) present(true);
        require(host.residentStaticInstanceCount() == 1, "GUI rebuilt or retired the existing world");
        present(false);
        require(host.residentStaticInstanceCount() == 2, "gameplay did not resume world synchronization");

        // A cell under construction can have an actor but no evaluated pose.
        const auto skeleton = world.reserveSkeleton();
        auto bones = std::make_shared<SkeletonPayload>();
        BoneRecord bone; bone.name = "body"; bones->bones.push_back(bone);
        SkeletonRecord skeletonRecord; skeletonRecord.payload = bones;
        skeletonRecord.sourceIdentity = "gui-test:skeleton";
        require(skeleton && world.commit(*skeleton, skeletonRecord), "skeleton publication");
        auto actor = world.reserveInstance();
        InstanceRecord actorRecord; actorRecord.model = *model; actorRecord.skeleton = *skeleton;
        require(actor && world.commit(*actor, actorRecord), "actor publication");
        for (int i = 0; i < 8; ++i) present(true);
        require(session->renderFrame(input) == RenderFrameResult::Failed,
            "world frame accepted actor without evaluated pose");
        session->waitIdle();
        session.reset();
        SDL_Quit();
        std::cout << "PASS GUI isolation: pending objects untouched, residents retained, world resumes, missing pose still rejected\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "FAIL GUI isolation: " << e.what() << '\n';
        SDL_Quit();
        return 1;
    }
}
