#include <components/render/native/skeletalobjectprogram.hpp>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace RenderCore;
using namespace RenderNative;
namespace
{
    unsigned checks = 0;
    void check(bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }
    bool near(const glm::mat4& a, const glm::mat4& b)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(a[c][r] - b[c][r]) > 1e-4f) return false;
        return true;
    }
    struct Fixture
    {
        RenderWorld world;
        std::shared_ptr<ModelPayload> nodes = std::make_shared<ModelPayload>();
        std::shared_ptr<SkeletonPayload> bones = std::make_shared<SkeletonPayload>();
        ModelRecord model;
        SkeletonRecord skeleton;
        NifControllerProgram controllers;
        Fixture()
        {
            auto geometry = std::make_shared<MeshPayload>();
            geometry->positions = {{0,0,0},{1,0,0},{0,1,0}};
            geometry->indices = {0,1,2};
            geometry->surfaces = {{PrimitiveTopology::Triangles,0,3,0}};
            auto skin = std::make_shared<SkinPayload>();
            skin->bones.push_back({"bone", glm::mat4(1)});
            skin->vertexInfluences.resize(3, {SkinInfluence{0,1}});
            MeshRecord mesh; mesh.payload = geometry; mesh.surfaceCount = 1; mesh.skinned = true; mesh.skin = skin;
            auto handle = *world.reserveMesh(); check(world.commit(handle, mesh), "fixture skin");
            ModelNodeRecord root; root.name = "collapsed";
            root.localTransform = glm::translate(glm::mat4(1), glm::vec3(4,0,0));
            root.controllerFlags = modelControllerFlag(ModelControllerFlag::Transform);
            ModelNodeRecord bone; bone.name = "Bone"; bone.parent = ModelNodeIndex{0};
            bone.localTransform = glm::translate(glm::mat4(1), glm::vec3(0,3,0));
            ModelNodeRecord body; body.name = "body"; body.parent = ModelNodeIndex{0};
            body.mesh = handle; body.kind = ModelNodeKind::Geometry;
            nodes->nodes = {root,bone,body}; nodes->roots = {ModelNodeIndex{0}};
            BoneRecord b; b.name = "bone"; b.bindLocal = root.localTransform * bone.localTransform;
            bones->bones = {b}; skeleton.payload = bones; model.payload = nodes;
            TransformControllerProgram channel; channel.node = ModelNodeIndex{0}; channel.autoPlay = true;
            channel.timing.stop = 2; channel.timing.extrapolation = ControllerExtrapolation::Cycle;
            channel.track.translations.keys = {{0, {4,0,0}}, {2, {8,0,0}}};
            controllers.transforms = {channel};
        }
        SkeletalObjectProgram bind() { return SkeletalObjectProgram::bind(world, model, skeleton, controllers); }
    };
}
int main()
{
    try
    {
        Fixture f;
        const auto program = f.bind();
        check(program.valid() && program.channelCount() == 1, "native bind rejected");
        std::vector<glm::mat4> pose;
        for (unsigned i = 0; i < 90; ++i)
        {
            const float time = static_cast<float>(i) / 10;
            const float x = 4 + f.controllers.transforms[0].timing.map(time) * 2;
            check(program.evaluate(time, pose), "native sample rejected");
            check(pose.size() == 1 && near(pose[0], glm::translate(glm::mat4(1), glm::vec3(x,3,0))),
                "collapsed animated ancestor sampled in wrong space");
        }
        const auto before = pose;
        check(!program.evaluate(std::numeric_limits<float>::quiet_NaN(), pose) && pose == before,
            "invalid time changed the published pose");
        // Immutable program owns source data, not a mutable scenegraph alias.
        f.nodes->nodes[0].localTransform[3][0] = 900;
        check(program.evaluate(1, pose) && pose[0][3].x == 6, "program retained mutable source alias");
        {
            Fixture q; q.controllers.transforms[0].autoPlay = false;
            const auto controlled = q.bind(); auto state = controlled.initialState();
            check(controlled.valid(), "controlled clock binding rejected");
            check(controlled.evaluate(100, 1.0f, {}, {}, {}, state, pose) && pose[0][3].x == 6,
                "gameplay clock replaced by autoplay clock");
            check(controlled.evaluate(200, std::nullopt, {}, {}, {}, state, pose) && pose[0][3].x == 6,
                "unassigned controller changed retained translation");
        }
        {
            Fixture q;
            const auto p = q.bind(); auto state = p.initialState();
            NamedTransformTrack track; track.timing.stop = 2;
            track.track.translations.keys = {{0,{20,0,0}}, {2,{24,0,0}}};
            std::vector<SkeletalObjectProgram::ExternalChannel> channels{{"collapsed", &track, 1}};
            check(p.evaluate(1, std::nullopt, channels, {}, {}, state, pose) && pose[0][3].x == 22,
                "external KF failed to override embedded controller");
            track.track.translations.keys.clear();
            track.track.scales.keys = {{0, 2}};
            check(p.evaluate(1, std::nullopt, channels, {}, {}, state, pose)
                && pose[0][3].x == 6 && pose[0][3].y == 6,
                "partial KF did not preserve embedded result and collapsed scale");
            check(p.evaluate(1, std::nullopt, channels, "COLLAPSED", {1,0,0}, state, pose) && pose[0][3].x == 0,
                "accumulation reset differs from gameplay axes");
            const auto saved = state.locals; const auto savedPose = pose;
            channels.push_back({"absent", &track, 1});
            check(!p.evaluate(1, std::nullopt, channels, {}, {}, state, pose)
                && state.locals == saved && pose == savedPose, "failed playback partially committed history");
            channels.pop_back(); channels.push_back(channels.front());
            check(!p.evaluate(1, std::nullopt, channels, {}, {}, state, pose), "duplicate external binding accepted");
            channels.resize(1); track.timing.frequency = std::numeric_limits<float>::max();
            channels[0].time = 100;
            check(!p.evaluate(1, std::nullopt, channels, {}, {}, state, pose), "overflow clock accepted");
        }
        {
            Fixture q; q.controllers.transforms[0].autoPlay = false;
            const auto p = q.bind(); auto state = p.initialState();
            auto seed = state.locals; seed[0][3][0] = 99;
            check(p.seed(seed, state), "valid pose seed rejected");
            check(p.evaluate(1, std::nullopt, {}, {}, {}, state, pose) && pose[0][3].x == 99,
                "stopped controller lost seeded channels");
            const auto saved = state.locals;
            seed[0][0][0] = std::numeric_limits<float>::quiet_NaN();
            check(!p.seed(seed, state) && state.locals == saved, "invalid seed changed history");
        }
        {
            Fixture q; q.controllers.transforms.push_back(q.controllers.transforms[0]);
            check(!q.bind().valid(), "duplicate controllers admitted");
        }
        {
            Fixture q; q.controllers.transforms.clear();
            q.nodes->nodes[0].controllerFlags = 0;
            q.nodes->nodes[0].localTransform = glm::mat4(1);
            q.bones->bones[0].bindLocal = q.nodes->nodes[1].localTransform;
            const auto p = q.bind();
            check(p.valid() && !p.requiredSeedNodes()[0] && p.requiredSeedNodes()[1],
                "immutable identity group incorrectly requires an evaluated matrix");
            check(p.evaluate(100, pose) && pose[0][3].y == 3,
                "immutable group lost native authored transform");
        }
        {
            Fixture q; q.controllers.transforms.clear();
            check(!q.bind().valid(), "uncompiled source controller admitted");
        }
        {
            Fixture q; q.controllers.visibility.push_back({});
            check(!q.bind().valid(), "visibility controller ignored");
        }
        {
            Fixture q; q.controllers.diagnostics.push_back("unsupported interpolator");
            check(!q.bind().valid(), "failed controller compilation ignored");
        }
        {
            Fixture q; q.nodes->nodes[0].name = "BONE";
            check(!q.bind().valid(), "ambiguous names admitted");
        }
        {
            Fixture q; q.bones->bones[0].bindLocal[3][0] = 12;
            check(!q.bind().valid(), "mismatched skin space admitted");
        }
        {
            Fixture q; q.model.dynamicRequirements = modelDynamicRequirement(ModelDynamicRequirement::ParticleSystem);
            check(!q.bind().valid(), "particles silently dropped");
        }
        {
            Fixture q; q.nodes->nodes[1].billboard = ModelBillboardMode::AlwaysFaceCamera;
            check(!q.bind().valid(), "view-dependent transform ignored");
        }
        {
            Fixture q; q.nodes->nodes[0].localTransform[0][0] = 2;
            q.bones->bones[0].bindLocal = q.nodes->nodes[0].localTransform * q.nodes->nodes[1].localTransform;
            check(!q.bind().valid(), "non-uniform animated source admitted");
        }
        std::cout << checks << " native skeletal object checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
