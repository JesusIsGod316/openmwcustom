#include <apps/openmw/mwrender/v4effectcapture.hpp>
#include <apps/openmw/mwrender/v4actorplacement.hpp>
#include <components/render/backend/vsg/staticworldplan.hpp>

#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/render/backend/vsg/immediateeffectrealizer.hpp>
#include <components/render/backend/vsg/frameresourcepool.hpp>
#include <components/render/backend/vsg/livetexturecache.hpp>
#include <components/render/backend/vsg/livetextureimages.hpp>

#include <components/sceneutil/skeleton.hpp>
#include <components/debug/gameplaydiagnostics.hpp>
#include <components/nifrender/texturepass.hpp>
#include <components/nifrender/shadermaterialpass.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/nifrender/actormodelcomposer.hpp>
#include <components/nifrender/translationpublish.hpp>
#include <components/render/backend/vsg/dynamicactorplan.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/sceneutil/controller.hpp>
#include <components/sceneutil/updateonlyvisitor.hpp>
#include <components/sceneutil/particleplayback.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/bsaarchive.hpp>
#include <osg/MatrixTransform>
#include <osg/Switch>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace
{
    // captureMaterial needs image identity, not a decoded DDS payload. Keep this
    // fixture entirely in memory and exercise the real winning-VFS lookup/hash.
    class MemoryFile final : public VFS::File
    {
    public:
        std::string bytes = "test texture bytes";
        unsigned int opens = 0;
        mutable unsigned int metadataReads = 0;
        std::filesystem::file_time_type modified{};
        Files::IStreamPtr open() override { ++opens; return std::make_unique<std::istringstream>(bytes); }
        std::filesystem::file_time_type getLastModified() const override { ++metadataReads; return modified; }
        std::string getStem() const override { return "capture"; }
    };

    class MemoryArchive final : public VFS::Archive
    {
    public:
        void listResources(VFS::FileMap& out) override
        {
            out.insert_or_assign(VFS::Path::Normalized(path), &mFile);
        }
        bool contains(VFS::Path::NormalizedView file) const override
        {
            return file.value() == path;
        }
        std::string getDescription() const override { return "capture-test-memory-archive"; }

    public:
        MemoryFile mFile;
        std::string path = "textures/capture.dds";
    };

    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    osg::ref_ptr<osg::StateSet> textureState(
        unsigned int unit, const std::string& name, const std::string& explicitType = {})
    {
        auto image = osg::ref_ptr<osg::Image>(new osg::Image);
        image->allocateImage(1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        image->setFileName("textures/capture.dds");
        auto texture = osg::ref_ptr<osg::Texture2D>(new osg::Texture2D(image));
        texture->setName(name);
        auto state = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        state->setTextureAttribute(unit, texture);
        if (!explicitType.empty())
            state->setTextureAttribute(unit, new SceneUtil::TextureType(explicitType));
        return state;
    }
}

int main(int argc, char** argv)
{
    VFS::Manager vfs;
    vfs.addArchive(std::make_unique<MemoryArchive>());
    vfs.buildIndex();
    int failures = 0;
    auto test = [&](const char* name, auto&& run) {
        try
        {
            run();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    };
    auto check = [&](unsigned int unit, const std::string& name, const std::string& explicitType,
                     RenderCore::TextureRole role, RenderCore::TextureColorSpace colorSpace) {
        auto state = textureState(unit, name, explicitType);
        MWRender::v4_effect_detail::CapturedMaterial material;
        std::string diagnostic;
        require(MWRender::v4_effect_detail::captureMaterial({}, state, vfs, material, diagnostic),
            diagnostic.c_str());
        require(material.textures.size() == 1, "expected one texture");
        const auto& binding = material.textures.front().binding;
        require(binding.role == role, "wrong texture role");
        require(binding.colorSpace == colorSpace, "wrong texture color interpretation");
        require(binding.transform.uvSet == unit, "authored texture unit lost");
    };
    using RenderCore::TextureColorSpace;
    using RenderCore::TextureRole;
    test("unnamed unit zero remains diffuse", [&] { check(0, "", "", TextureRole::Diffuse, TextureColorSpace::Srgb); });
    test("named normal texture uses data interpretation", [&] {
        check(1, "normalMap", "", TextureRole::Normal, TextureColorSpace::Data);
    });
    test("normal-height type is not a diffuse texture", [&] {
        check(1, "", "normalHeightMap", TextureRole::Normal, TextureColorSpace::Data);
    });
    test("named specular texture is not a diffuse texture", [&] {
        check(2, "specularMap", "", TextureRole::Specular, TextureColorSpace::Srgb);
    });
    test("explicit type overrides texture name", [&] {
        check(1, "normalMap", "emissiveMap", TextureRole::Emissive, TextureColorSpace::Srgb);
    });
    test("unknown secondary texture is diagnosed", [&] {
        auto state = textureState(2, "unknown-custom-stage");
        MWRender::v4_effect_detail::CapturedMaterial material;
        std::string diagnostic;
        require(!MWRender::v4_effect_detail::captureMaterial({}, state, vfs, material, diagnostic),
            "unknown secondary texture was silently treated as diffuse");
        require(diagnostic.find("unknown-custom-stage") != std::string::npos,
            "diagnostic must identify the unsupported texture semantic");
    });
    test("texture identity is hashed once across repeated captures", [&] {
        VFS::Manager local;
        auto archive = std::make_unique<MemoryArchive>();
        auto* file = &archive->mFile;
        local.addArchive(std::move(archive));
        local.buildIndex();
        NifRender::TextureIdentityCache cache(local);
        const auto first = cache.resolve(VFS::Path::Normalized("textures/capture.dds"));
        for (int i = 0; i < 100; ++i)
            require(cache.resolve(VFS::Path::Normalized("textures/capture.dds")).contentIdentity == first.contentIdentity,
                "unstable cached identity");
        require(first.valid() && file->opens == 1, "cache reread unchanged texture bytes");
        file->bytes = "changed bytes";
        file->modified += std::filesystem::file_time_type::duration(1);
        require(cache.resolve(VFS::Path::Normalized("textures/capture.dds")).contentIdentity != first.contentIdentity,
            "timestamp change failed to invalidate hash");
        require(file->opens == 2, "modified texture was not rehashed exactly once");
        cache.clear();
        (void)cache.resolve(VFS::Path::Normalized("textures/capture.dds"));
        require(file->opens == 3, "explicit invalidation did not rehash");
    });
    test("same timestamp winning VFS replacement invalidates identity", [&] {
        VFS::Manager local;
        local.addArchive(std::make_unique<MemoryArchive>());
        local.buildIndex();
        NifRender::TextureIdentityCache cache(local);
        const auto first = cache.resolve(VFS::Path::Normalized("textures/capture.dds"));
        auto replacement = std::make_unique<MemoryArchive>();
        replacement->mFile.bytes = "winning replacement at identical timestamp";
        local.addArchive(std::move(replacement));
        local.buildIndex();
        require(cache.resolve(VFS::Path::Normalized("textures/capture.dds")).contentIdentity != first.contentIdentity,
            "cache retained old winner");
        local.reset();
        require(!cache.resolve(VFS::Path::Normalized("textures/capture.dds")).valid() && cache.size() == 0,
            "reset retained deleted texture");
    });
    test("zero-capacity cache preserves uncached control", [&] {
        VFS::Manager local;
        auto archive = std::make_unique<MemoryArchive>();
        auto* file = &archive->mFile;
        local.addArchive(std::move(archive));
        local.buildIndex();
        NifRender::TextureIdentityCache cache(local, 0);
        (void)cache.resolve(VFS::Path::Normalized("textures/capture.dds"));
        (void)cache.resolve(VFS::Path::Normalized("textures/capture.dds"));
        require(file->opens == 2 && cache.size() == 0, "uncached control unexpectedly retained entries");
    });

    test("bounded texture cache evicts least recently used identity", [&] {
        VFS::Manager local;
        auto first = std::make_unique<MemoryArchive>();
        auto second = std::make_unique<MemoryArchive>();
        auto* firstFile = &first->mFile;
        auto* secondFile = &second->mFile;
        second->path = "textures/other.dds";
        local.addArchive(std::move(first));
        local.addArchive(std::move(second));
        local.buildIndex();
        NifRender::TextureIdentityCache cache(local, 1);
        (void)cache.resolve(VFS::Path::Normalized("textures/capture.dds"));
        (void)cache.resolve(VFS::Path::Normalized("textures/other.dds"));
        (void)cache.resolve(VFS::Path::Normalized("textures/capture.dds"));
        require(cache.size() == 1 && firstFile->opens == 2 && secondFile->opens == 1,
            "cache grew beyond capacity or retained evicted identity");
    });

    test("frame snapshot checks texture metadata once and invalidates next frame", [&] {
        VFS::Manager local;
        auto archive = std::make_unique<MemoryArchive>();
        auto* file = &archive->mFile;
        local.addArchive(std::move(archive));
        local.buildIndex();
        NifRender::TextureIdentityCache cache(local);
        const VFS::Path::Normalized path("textures/capture.dds");
        const auto first = cache.resolve(path);
        const auto before = file->metadataReads;
        {
            NifRender::TextureIdentityCache::CaptureScope frame(cache);
            for (int i = 0; i < 100; ++i) (void)cache.resolve(path);
            require(file->metadataReads == before + 1, "frame repeated filesystem metadata calls");
            file->bytes = "next frame contents";
            file->modified += std::filesystem::file_time_type::duration(1);
        }
        {
            NifRender::TextureIdentityCache::CaptureScope frame(cache);
            require(cache.resolve(path).contentIdentity != first.contentIdentity, "next frame ignored file edit");
            local.reset();
            require(!cache.resolve(path).valid(), "VFS reset retained frame snapshot");
        }
    });
    test("legacy and shader translation share texture hashes across models", [&] {
        VFS::Manager local;
        auto archive = std::make_unique<MemoryArchive>();
        auto* file = &archive->mFile;
        local.addArchive(std::move(archive));
        local.buildIndex();
        NifRender::TextureIdentityCache cache(local);
        Nif::NiSourceTexture texture;
        texture.mExternal = true;
        texture.mFile = "textures/capture.dds";
        for (int i = 0; i < 10; ++i)
        {
            NifRender::TranslationBundle legacy, shader;
            require(NifRender::texture_pass_detail::stageSourceTexture(texture, &local, legacy, &cache).has_value(),
                "legacy translation failed");
            require(NifRender::stageShaderTexture(texture.mFile, &local, shader, texture, &cache).valid(),
                "shader translation failed");
            require(legacy.textures[0].record.contentIdentity == shader.textures[0].record.contentIdentity,
                "cached translation changed content semantics");
        }
        require(file->opens == 1, "translation repeatedly hashed shared texture");
    });

    auto triangle = [] {
        auto geometry = osg::ref_ptr<osg::Geometry>(new osg::Geometry);
        auto vertices = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array);
        vertices->push_back({ 1, 0, 0 });
        vertices->push_back({ 0, 1, 0 });
        vertices->push_back({ 0, 0, 1 });
        geometry->setVertexArray(vertices);
        geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES, 0, 3));
        return geometry;
    };
    test("particle actor retains body, particles, placement and active children without duplicate attachments", [&] {
        auto root = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        root->setMatrix(osg::Matrix::translate(10, 20, 30));
        auto body = osg::ref_ptr<osg::Geode>(new osg::Geode);
        body->addDrawable(triangle());
        root->addChild(body);
        auto particles = osg::ref_ptr<osgParticle::ParticleSystem>(new osgParticle::ParticleSystem);
        auto* particle = particles->createParticle(nullptr);
        particle->setPosition(osg::Vec3(3, 4, 5));
        particle->setLifeTime(10);
        auto particleNode = osg::ref_ptr<osg::Geode>(new osg::Geode);
        particleNode->addDrawable(particles);
        root->addChild(particleNode);
        auto hidden = osg::ref_ptr<osg::Switch>(new osg::Switch);
        hidden->addChild(body, false);
        root->addChild(hidden);
        MWRender::EffectParams params;
        auto attached = osg::ref_ptr<osg::Group>(new osg::Group);
        attached->setUpdateCallback(new MWRender::UpdateVfxCallback(params));
        attached->addChild(body);
        root->addChild(attached);
        auto captured = MWRender::captureV4ParticleActor(*root, "actor", vfs);
        require(captured.valid(), captured.diagnostic.c_str());
        require(captured.draws.size() == 2, "body/particle omitted or inactive/attached effect duplicated");
        const auto& bodyDraw = captured.draws[0];
        const auto& particleDraw = captured.draws[1];
        require((bodyDraw.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster)) != 0,
            "actor body lost shadow eligibility");
        require((particleDraw.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::Effect)) != 0,
            "particle lost effect semantics");
        require(bodyDraw.worldTransform[3] == glm::vec4(10, 20, 30, 1), "body placement lost");
        require(particleDraw.worldTransform[3] == glm::vec4(13, 24, 35, 1), "particle placement lost");
        particle->setPosition(osg::Vec3(6, 7, 8));
        auto next = MWRender::captureV4ParticleActor(*root, "actor", vfs, nullptr, 1);
        require(next.valid() && next.draws.size() == 2, "next particle frame failed");
        require(next.draws[1].worldTransform[3] == glm::vec4(16, 27, 38, 1), "particle state froze");
        require(next.draws[1].identity == particleDraw.identity, "particle identity changed each frame");
        auto effects = MWRender::captureV4AttachedEffects(*root, "effect", vfs);
        require(effects.valid() && effects.draws.size() == 1, "separate attached effect disappeared");
    });
    test("particle actor keeps body with no living particles and rejects unsupported particle state", [&] {
        auto root = osg::ref_ptr<osg::Geode>(new osg::Geode);
        root->addDrawable(triangle());
        auto particles = osg::ref_ptr<osgParticle::ParticleSystem>(new osgParticle::ParticleSystem);
        root->addDrawable(particles);
        auto captured = MWRender::captureV4ParticleActor(*root, "empty", vfs);
        require(captured.valid() && captured.draws.size() == 1, "empty emitter removed its actor body");
        particles->setUseShaders(true);
        captured = MWRender::captureV4ParticleActor(*root, "unsupported", vfs);
        require(!captured.valid() && captured.diagnostic.find("shader-evaluated") != std::string::npos,
            "unsupported state was silently discarded");
    });
    test("morph capture uses evaluated targets and leaves source unchanged", [&] {
        auto source = triangle();
        auto morph = osg::ref_ptr<SceneUtil::MorphGeometry>(new SceneUtil::MorphGeometry);
        morph->setSourceGeometry(source);
        auto base = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array(
            *static_cast<osg::Vec3Array*>(source->getVertexArray())));
        auto offsets = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array(3));
        (*offsets)[0] = { 4, 0, 0 };
        morph->addMorphTarget(base, 0.f); // target zero is base, not a weighted delta
        morph->addMorphTarget(offsets, 0.5f);
        auto captured = MWRender::captureV4WholeEffectSubtree(*morph, "morph", vfs, nullptr, 0);
        require(captured.valid() && captured.draws.size() == 1, "morph drawable disappeared from capture");
        require(captured.draws[0].mesh.positions[0].x == 3.f, "morph delta not evaluated");
        require((*static_cast<osg::Vec3Array*>(source->getVertexArray()))[0].x() == 1.f,
            "capture mutated immutable source");
        morph->getMorphTarget(1).setWeight(1.f);
        morph->dirty();
        captured = MWRender::captureV4WholeEffectSubtree(*morph, "morph", vfs, nullptr, 1);
        require(captured.draws[0].mesh.positions[0].x == 5.f, "next traversal reused stale morph");
    });
    test("malformed active morph target is diagnosed before evaluation", [&] {
        auto morph = osg::ref_ptr<SceneUtil::MorphGeometry>(new SceneUtil::MorphGeometry);
        auto source = triangle();
        morph->setSourceGeometry(source);
        morph->addMorphTarget(static_cast<osg::Vec3Array*>(source->getVertexArray()), 1.f);
        morph->addMorphTarget(new osg::Vec3Array(1), 1.f);
        const auto captured = MWRender::captureV4WholeEffectSubtree(*morph, "bad-morph", vfs, nullptr, 1);
        require(!captured.valid() && captured.diagnostic.find("topology") != std::string::npos,
            "bad morph topology was silently accepted");
    });
    test("rig evaluation cancels attachment transform without moving skeleton space", [&] {
        auto source = triangle();
        auto rig = osg::ref_ptr<SceneUtil::RigGeometry>(new SceneUtil::RigGeometry);
        rig->setSourceGeometry(source);
        rig->setBoneInfo({ { "Bone", {}, osg::Matrixf::identity() } });
        rig->setInfluences({ { { 0, 1.f } }, { { 0, 1.f } }, { { 0, 1.f } } });
        rig->setTransform(osg::Matrixf::identity());
        auto skeleton = osg::ref_ptr<SceneUtil::Skeleton>(new SceneUtil::Skeleton);
        auto bone = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        bone->setName("Bone");
        bone->setMatrix(osg::Matrix::translate(3, 0, 0));
        skeleton->addChild(bone);
        auto attachment = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        attachment->setName("attachment");
        rig->setName("Tri Mesh");
        attachment->setMatrix(osg::Matrix::translate(10, 0, 0));
        attachment->addChild(rig);
        skeleton->addChild(attachment);
        osg::NodePath path{ skeleton, attachment, rig };
        auto* evaluated = rig->evaluateGeometry(1, path);
        require(evaluated != nullptr, "rig evaluator could not resolve parent skeleton");
        const auto local = (*static_cast<osg::Vec3Array*>(evaluated->getVertexArray()))[0];
        const auto placed = local * osg::computeLocalToWorld(path);
        require(std::abs(placed.x() - 4.f) < 0.0001f, "attachment was double-applied or bone pose lost");
        require((*static_cast<osg::Vec3Array*>(source->getVertexArray()))[0].x() == 1.f,
            "skinning overwrote source");
        bone->setMatrix(osg::Matrix::translate(6, 0, 0));
        evaluated = rig->evaluateGeometry(2, path);
        require(std::abs(((*static_cast<osg::Vec3Array*>(evaluated->getVertexArray()))[0]
                    * osg::computeLocalToWorld(path)).x() - 7.f) < 0.0001f, "rig pose did not advance");
    });
    test("rig without parent skeleton fails visibly rather than publishing rest pose", [&] {
        auto rig = osg::ref_ptr<SceneUtil::RigGeometry>(new SceneUtil::RigGeometry);
        rig->setSourceGeometry(triangle());
        const auto captured = MWRender::captureV4WholeEffectSubtree(*rig, "missing-skeleton", vfs, nullptr, 1);
        require(!captured.valid(), "unbound rig silently disappeared");
    });
    RenderCore::ImmediateEffectDraw resident;
    resident.identity = "fixture";
    resident.mesh.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
    resident.mesh.indices = { 0, 1, 2 };
    resident.mesh.surfaces = { { RenderCore::PrimitiveTopology::Triangles, 0, 3, 0 } };
    resident.bounds = { { 0, 0, 0 }, { 1, 1, 0 } };
    RenderCore::EffectTextureSnapshot texture;
    texture.texture.sourceIdentity = "textures/capture.dds";
    texture.texture.contentIdentity = "fixture-content";
    texture.texture.width = texture.texture.height = 1;
    resident.textures.push_back(texture);
    auto rejectsChange = [&](auto&& change) {
        auto current = resident;
        change(current);
        require(!RenderVsg::immediateEffectLayoutMatches(resident, current),
            "resident reuse accepted changed immutable state");
    };
    test("equal-sized index changes invalidate reuse", [&] {
        rejectsChange([](auto& draw) { draw.mesh.indices = { 0, 2, 1 }; });
    });
    test("numeric material changes invalidate reuse", [&] {
        rejectsChange([](auto& draw) { draw.material.alpha = 0.25f; });
    });
    test("UV transform changes invalidate reuse", [&] {
        rejectsChange([](auto& draw) { draw.textures[0].binding.transform.offset.x = 0.5f; });
    });
    test("sampler changes invalidate reuse", [&] {
        rejectsChange([](auto& draw) { draw.textures[0].binding.sampler.wrapU = RenderCore::TextureWrap::Clamp; });
    });
    test("interpretation changes invalidate reuse", [&] {
        rejectsChange([](auto& draw) { draw.textures[0].binding.colorSpace = TextureColorSpace::Data; });
    });
    test("animated bounds retain layout but update sort bounds", [&] {
        auto current = resident;
        current.bounds.maximum.x = 2;
        current.mesh.positions[1].x = 2;
        require(RenderVsg::immediateEffectLayoutMatches(resident, current), "bounds forced a resource rebuild");
        require(!RenderVsg::immediateEffectLayoutMatches(resident, current, true), "legacy bounds control was lost");
        std::vector<RenderVsg::StaticRealizationResult::MutableDrawStreams> streams(1);
        streams[0].positions = vsg::vec3Array::create(3);
        streams[0].normals = vsg::vec3Array::create(3);
        streams[0].colors = vsg::vec4Array::create(3);
        streams[0].sorted = vsg::DepthSorted::create();
        require(RenderVsg::updateImmediateEffectRealization(current, streams), "bounds update failed");
        require(std::abs(streams[0].sorted->bound.center.x - 1.0) < 1e-6, "ordinary sort center is stale");
        current.billboard = RenderCore::ModelBillboardMode::AlwaysFaceCamera;
        auto billboardResident = current;
        const auto* positions = streams[0].positions.get();
        for (unsigned frame = 1; frame <= 120; ++frame)
        {
            current.mesh.positions[1].x = float(frame);
            current.bounds.maximum.x = float(frame);
            require(RenderVsg::immediateEffectLayoutMatches(billboardResident, current), "particle size rebuilt layout");
            require(RenderVsg::updateImmediateEffectRealization(current, streams), "particle stream update failed");
            require(streams[0].positions.get() == positions, "particle array was replaced");
            require(streams[0].sorted->bound.center == vsg::dvec3(0.0, 0.0, 0.0), "billboard sort center moved off pivot");
            require(std::abs(streams[0].sorted->bound.radius - double(frame)) < 1e-6, "billboard sort radius is stale");
        }
    });
    test("visibility changes invalidate reuse", [&] {
        rejectsChange([](auto& draw) { draw.semanticFlags = 0; });
    });
    test("resized effect sort bounds match fresh conformance realization", [&] {
        for (bool billboard : {false, true})
        {
            auto draw = resident;
            draw.textures.clear();
            draw.material.alphaMode = RenderCore::AlphaMode::Blend;
            draw.material.alphaBlendEnabled = true;
            draw.material.transparentSort = RenderCore::TransparentSortPolicy::Sorted;
            if (billboard) draw.billboard = RenderCore::ModelBillboardMode::AlwaysFaceCamera;
            const RenderVsg::StaticTextureResolver resolver = [](const auto&, const auto&) {
                return vsg::ref_ptr<vsg::Data>{}; // no textures in this fixture
            };
            auto realized = RenderVsg::realizeImmediateEffectDraw(draw, resolver, vsg::SharedObjects::create());
            require(realized.valid(), realized.diagnostic.c_str());
            require(realized.mutableDraws.size() == 1 && realized.mutableDraws[0].sorted,
                "real effect did not expose its conformance sort wrapper");
            const auto* root = realized.root.get();
            draw.mesh.positions[1].x = 7;
            draw.mesh.positions[0].x = -3;
            draw.bounds.minimum.x = -3;
            draw.bounds.maximum.x = 7;
            require(RenderVsg::updateImmediateEffectRealization(draw, realized.mutableDraws), "real effect update failed");
            auto fresh = RenderVsg::realizeImmediateEffectDraw(draw, resolver, vsg::SharedObjects::create());
            require(fresh.valid(), fresh.diagnostic.c_str());
            const auto& actual = realized.mutableDraws[0].sorted->bound;
            const auto& expected = fresh.mutableDraws[0].sorted->bound;
            require(actual.center == expected.center && std::abs(actual.radius - expected.radius) < 1e-6,
                "updated sort bound differs from fresh conformance graph");
            require(realized.root.get() == root, "ordinary size update replaced the scene root");
        }
    });
    test("unchanged state and mutable positions permit reuse", [&] {
        auto current = resident;
        current.mesh.positions[0].x = 0.1f;
        current.worldTransform[3][0] = 4;
        require(RenderVsg::immediateEffectLayoutMatches(resident, current),
            "ordinary position/placement updates should not invalidate static state");
    });
    test("rejected stream update does not partially overwrite residents", [&] {
        auto current = resident;
        current.mesh.texCoordSets = { { { 0, 0 }, { 1, 0 }, { 0, 1 } } };
        current.mesh.surfaces.push_back(current.mesh.surfaces[0]);
        std::vector<RenderVsg::StaticRealizationResult::MutableDrawStreams> streams(2);
        for (auto& stream : streams)
        {
            stream.positions = vsg::vec3Array::create(3);
            stream.normals = vsg::vec3Array::create(3);
            stream.colors = vsg::vec4Array::create(3);
            stream.texCoords.push_back(vsg::vec2Array::create(3));
            stream.positions->set(0, { 99, 99, 99 });
        }
        streams[1].texCoords[0] = {}; // malformed later stream
        require(!RenderVsg::updateImmediateEffectRealization(current, streams), "invalid streams accepted");
        require((*streams[0].positions)[0] == vsg::vec3(99, 99, 99), "first stream was partially overwritten");
    });
    auto makeStreams = [] {
        std::vector<RenderVsg::StaticRealizationResult::MutableDrawStreams> streams(1);
        streams[0].positions = vsg::vec3Array::create(3);
        streams[0].normals = vsg::vec3Array::create(3);
        streams[0].colors = vsg::vec4Array::create(3);
        streams[0].texCoords.push_back(vsg::vec2Array::create(3));
        return streams;
    };
    auto streamDraw = resident;
    streamDraw.mesh.texCoordSets = { { { 0, 0 }, { 1, 0 }, { 0, 1 } } };
    const auto streamVersions = [](const auto& streams) {
        std::array<vsg::ModifiedCount, 4> result;
        streams[0].positions->getModifiedCount(result[0]);
        streams[0].normals->getModifiedCount(result[1]);
        streams[0].colors->getModifiedCount(result[2]);
        streams[0].texCoords[0]->getModifiedCount(result[3]);
        return result;
    };
    test("unchanged and placement-only updates do not dirty GPU streams", [&] {
        auto streams = makeStreams();
        auto draw = streamDraw;
        require(RenderVsg::updateImmediateEffectRealization(draw, streams), "initial update rejected");
        const auto before = streamVersions(streams);
        require(RenderVsg::updateImmediateEffectRealization(draw, streams), "unchanged update rejected");
        require(before == streamVersions(streams), "unchanged streams scheduled redundant uploads");
        draw.worldTransform[3][0] = 10;
        require(RenderVsg::updateImmediateEffectRealization(draw, streams), "placement-only update rejected");
        require(before == streamVersions(streams), "placement-only update scheduled redundant uploads");
    });
    test("only changed position normal color and UV streams are dirtied", [&] {
        auto streams = makeStreams();
        auto draw = streamDraw;
        require(RenderVsg::updateImmediateEffectRealization(draw, streams), "initial update rejected");
        for (std::size_t changed = 0; changed != 4; ++changed)
        {
            const auto before = streamVersions(streams);
            switch (changed)
            {
                case 0: draw.mesh.positions[0].x = 0.125f; break;
                case 1: draw.mesh.normals.assign(3, glm::vec3(0, 1, 0)); break;
                case 2: draw.mesh.colors.assign(3, glm::vec4(0.5f)); break;
                case 3: draw.mesh.texCoordSets[0][0].x = 0.5f; break;
            }
            require(RenderVsg::updateImmediateEffectRealization(draw, streams), "changed update rejected");
            const auto after = streamVersions(streams);
            for (std::size_t i = 0; i != 4; ++i)
                require((before[i] != after[i]) == (i == changed), "wrong GPU stream invalidation");
        }
        require((*streams[0].positions)[0].x == 0.125f, "position data was not updated");
        require((*streams[0].normals)[0] == vsg::vec3(0, 1, 0), "normal data was not updated");
        require((*streams[0].colors)[0] == vsg::vec4(0.5f, 0.5f, 0.5f, 0.5f), "color data was not updated");
        require((*streams[0].texCoords[0])[0].x == 0.5f, "UV data was not updated");
    });
    test("live actor placement matches OpenMW translation attitude and nonuniform scale", [&] {
        auto root = osg::ref_ptr<SceneUtil::PositionAttitudeTransform>(new SceneUtil::PositionAttitudeTransform);
        root->setPosition({ 123, -45, 6 });
        root->setAttitude(osg::Quat(0.7, osg::Vec3(0, 0, 1)));
        root->setScale({ 0.8f, 1.2f, 1.1f });
        osg::Matrix reference;
        root->computeLocalToWorldMatrix(reference, nullptr);
        const auto matrix = RenderVsg::staticInstancePlacementMatrix(MWRender::captureV4ActorPlacement(*root));
        const osg::Vec3d expected = osg::Vec3d(1, 2, 3) * reference;
        const auto actual = matrix * glm::dvec4(1, 2, 3, 1);
        require(glm::length(glm::dvec3(actual) - glm::dvec3(expected.x(), expected.y(), expected.z())) < 0.0001,
            "live placement lost scale, attitude, or translation");
    });
    test("actor stream reuse updates pose placement and sort bounds without dirtying unchanged arrays", [&] {
        RenderCore::RenderWorld world;
        RenderVsg::StaticAssetPlan plan;
        plan.draws.resize(1);
        auto payload = resident.mesh;
        RenderVsg::MeshPayloadResolver resolve = [&](auto, auto) { return &payload; };
        auto streams = makeStreams();
        streams[0].transform = vsg::MatrixTransform::create();
        streams[0].sorted = vsg::DepthSorted::create();
        require(RenderVsg::updateDeformedAssetRealization(world, plan, resolve, streams), "initial actor update failed");
        const auto versions = streamVersions(streams);
        plan.draws[0].worldTransform[3][0] = 5.f;
        require(RenderVsg::updateDeformedAssetRealization(world, plan, resolve, streams), "placement update failed");
        require(versions == streamVersions(streams), "placement-only change dirtied vertex data");
        require(streams[0].transform->matrix[3][0] == 5.0, "rigid draw placement not updated");
        require(std::abs(streams[0].sorted->bound.center.x - 5.5) < 0.0001, "sorted bound used bind pose");
        payload.positions[0].x = -3;
        require(RenderVsg::updateDeformedAssetRealization(world, plan, resolve, streams), "pose update failed");
        const auto after = streamVersions(streams);
        require(after[0] != versions[0] && after[1] == versions[1], "pose dirtied wrong streams");
        require(std::abs(streams[0].sorted->bound.center.x - 4.0) < 0.0001, "sort bound did not follow deformation");
    });
    test("actor stream update validates all draws before changing any", [&] {
        RenderCore::RenderWorld world;
        RenderVsg::StaticAssetPlan plan;
        plan.draws.resize(2);
        auto payload = resident.mesh;
        RenderVsg::MeshPayloadResolver resolve = [&](auto, auto) { return &payload; };
        auto streams = makeStreams();
        streams[0].transform = vsg::MatrixTransform::create();
        streams.push_back({}); // invalid second destination
        streams[0].positions->set(0, { 42, 0, 0 });
        require(!RenderVsg::updateDeformedAssetRealization(world, plan, resolve, streams), "bad destination accepted");
        require((*streams[0].positions)[0].x == 42, "first draw partially overwritten");
    });
    using RenderCore::FrameId;
    using Pool = RenderVsg::FrameResourcePool<std::shared_ptr<int>>;
    test("in-flight effect buffers are never acquired for writing", [&] {
        Pool pool(3);
        pool.beginFrame(FrameId{ 1 }, {});
        auto first = pool.acquire("effect") = std::make_shared<int>(11);
        require(pool.markSubmitted(FrameId{ 1 }), "first submission rejected");
        pool.beginFrame(FrameId{ 2 }, {});
        auto& second = pool.acquire("effect");
        require(!second, "in-flight version returned for mutation");
        second = std::make_shared<int>(22);
        require(*first == 11, "earlier frame was overwritten");
        require(pool.markSubmitted(FrameId{ 2 }), "second submission rejected");
        pool.beginFrame(FrameId{ 3 }, FrameId{ 1 });
        require(pool.acquire("effect") == first, "completed version was not reused");
    });
    test("resource pool refuses unbounded versions without GPU completion", [&] {
        Pool pool(3);
        for (std::uint64_t frame = 1; frame <= 3; ++frame)
        {
            pool.beginFrame(FrameId{ frame }, {});
            pool.acquire("effect") = std::make_shared<int>(1);
            require(pool.markSubmitted(FrameId{ frame }), "submission rejected");
        }
        pool.beginFrame(FrameId{ 4 }, {});
        bool rejected = false;
        try { (void)pool.acquire("effect"); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected && pool.size() == 3, "pool exceeded ring capacity or reused in-flight data");
    });
    test("disappeared effects survive until their final GPU use completes", [&] {
        Pool pool(3);
        pool.beginFrame(FrameId{ 1 }, {});
        std::weak_ptr<int> lifetime = pool.acquire("removed") = std::make_shared<int>(1);
        require(pool.markSubmitted(FrameId{ 1 }), "submission rejected");
        pool.beginFrame(FrameId{ 2 }, {});
        pool.collectUnused();
        require(!lifetime.expired(), "in-flight removed resource destroyed too early");
        require(pool.markSubmitted(FrameId{ 2 }), "empty submission rejected");
        pool.beginFrame(FrameId{ 3 }, FrameId{ 1 });
        pool.collectUnused();
        require(lifetime.expired() && pool.size() == 0, "removed resource retained after completion");
    });
    test("published graphs retain resources independently of cache eviction", [&] {
        Pool pool(3);
        pool.beginFrame(FrameId{ 1 }, {});
        auto published = pool.acquire("removed") = std::make_shared<int>(7);
        require(pool.markSubmitted(FrameId{ 1 }), "submission rejected");
        pool.beginFrame(FrameId{ 2 }, FrameId{ 1 });
        pool.collectUnused();
        require(pool.size() == 0 && *published == 7, "eviction invalidated a retained graph");
    });
    test("repeated effect spawn and despawn has bounded residency", [&] {
        Pool pool(3);
        for (std::uint64_t frame = 1; frame <= 500; ++frame)
        {
            pool.beginFrame(FrameId{ frame }, frame > 3 ? std::optional(FrameId{ frame - 3 }) : std::nullopt);
            pool.acquire("spawn-" + std::to_string(frame)) = std::make_shared<int>(1);
            pool.collectUnused();
            require(pool.size() <= 3, "removed identities accumulated in the resident cache");
            require(pool.markSubmitted(FrameId{ frame }), "submission rejected");
        }
        pool.beginFrame(FrameId{ 501 }, FrameId{ 500 });
        pool.collectUnused();
        require(pool.size() == 0, "world unload failed to drain completed residents");
    });
    test("steady effects reuse a bounded ring after warmup", [&] {
        Pool pool(3);
        int created = 0;
        for (std::uint64_t frame = 1; frame <= 500; ++frame)
        {
            pool.beginFrame(FrameId{ frame }, frame > 3 ? std::optional(FrameId{ frame - 3 }) : std::nullopt);
            auto& value = pool.acquire("steady");
            if (!value)
            {
                value = std::make_shared<int>(0);
                ++created;
            }
            ++*value;
            pool.collectUnused();
            require(pool.markSubmitted(FrameId{ frame }), "submission rejected");
        }
        require(created == 3 && pool.size() == 3, "steady effect kept allocating after warmup");
    });
    test("failed unsubmitted preparation does not pin a new version", [&] {
        Pool pool(3);
        pool.beginFrame(FrameId{ 1 }, {});
        auto prepared = pool.acquire("effect") = std::make_shared<int>(9);
        pool.beginFrame(FrameId{ 2 }, {}); // no submission of frame 1
        require(pool.acquire("effect") == prepared, "unsubmitted version was treated as in-flight");
        require(!pool.markSubmitted(FrameId{ 1 }), "stale prepared frame accepted");
        require(pool.markSubmitted(FrameId{ 2 }), "current prepared frame rejected");
        require(!pool.markSubmitted(FrameId{ 2 }), "duplicate submission accepted");
    });
    test("duplicate effect identities cannot alias mutable frame data", [&] {
        Pool pool(3);
        pool.beginFrame(FrameId{ 1 }, {});
        (void)pool.acquire("duplicate");
        bool rejected = false;
        try { (void)pool.acquire("duplicate"); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "duplicate identity reused the same mutable resource");
    });
    test("diagnostic JSON escapes control characters and Windows paths", [&] {
        require(Debug::GameplayDiagnostics::quote("a\n\"\\") == "\"a\\u000a\\\"\\\\\"", "invalid JSON escape");
    });
    test("diagnostic camera parity distinguishes changed and invalid matrices", [&] {
        osg::Matrixf a, b;
        require(Debug::GameplayDiagnostics::matrixDifference(a, b) == 0, "identity parity failed");
        b(3, 0) = 50;
        require(Debug::GameplayDiagnostics::matrixDifference(a, b) == 50, "translation discrepancy missed");
        b(1, 1) = std::numeric_limits<float>::quiet_NaN();
        require(Debug::GameplayDiagnostics::matrixDifference(a, b) == -1, "NaN reported as match");
    });
    test("diagnostic skeleton counters observe suppression without changing policy", [&] {
        using namespace Debug::GameplayDiagnostics;
        auto skeleton = osg::ref_ptr<SceneUtil::Skeleton>(new SceneUtil::Skeleton);
        osg::NodeVisitor update(osg::NodeVisitor::UPDATE_VISITOR, osg::NodeVisitor::TRAVERSE_ALL_CHILDREN);
        update.setTraversalNumber(10);
        skeleton->updateBoneMatrices(1);
        skeleton->setActive(SceneUtil::Skeleton::SemiActive);
        context = {}; context.sample = true;
        skeleton->traverse(update);
        require(context.skeletonSkippedCull == 1 && context.skeletonUpdates == 0, "cull suppression unobserved");
        skeleton->setActive(SceneUtil::Skeleton::Active);
        skeleton->traverse(update);
        require(context.skeletonUpdates == 1, "active traversal unobserved");
        context = {};
    });
    test("live texture decode cache shares payloads without retaining them", [&] {
        using namespace RenderCore;
        TextureRecord record;
        record.sourceIdentity = "textures/shared.dds";
        record.contentIdentity = "content-a";
        TextureRealizationKey key;
        RenderWorld textureWorld;
        key.view.texture = textureWorld.reserveTexture().value();
        unsigned decodes = 0;
        auto cached = RenderVsg::cacheLiveTextures([&](const TextureRecord&, const TextureRealizationKey&) {
            ++decodes;
            return vsg::vec4Array::create(1);
        });
        auto first = cached(record, key);
        auto second = cached(record, key);
        require(first && first == second && decodes == 1, "same live texture decoded repeatedly");
        auto aliasKey = key;
        aliasKey.view.texture = textureWorld.reserveTexture().value();
        require(cached(record, aliasKey) == first && decodes == 1, "same content under another handle was duplicated");
        vsg::observer_ptr<vsg::Data> weak(first);
        first = {}; second = {};
        require(!weak.valid(), "weak texture cache retained unused payload");
        auto replacement = cached(record, key);
        require(replacement && decodes == 2, "expired payload did not decode again");
        key.view.colorSpace = TextureColorSpace::Data;
        auto dataView = cached(record, key);
        require(dataView != replacement && decodes == 3, "sRGB/data views aliased");
        record.contentIdentity = "content-b";
        auto changed = cached(record, key);
        require(changed != dataView && decodes == 4, "world reset/winning-content change reused old data");
        key.revision = ResourceRevision(2);
        require(!cached(record, key) && decodes == 4, "stale revision used cache or decoder");
    });
    test("texture cache capacity eviction cannot invalidate scene-owned payload", [&] {
        using namespace RenderCore;
        TextureRecord record;
        record.contentIdentity = "content";
        TextureRealizationKey key;
        RenderWorld textureWorld;
        key.view.texture = textureWorld.reserveTexture().value();
        unsigned decodes = 0;
        auto cached = RenderVsg::cacheLiveTextures([&](const TextureRecord&, const TextureRealizationKey&) {
            ++decodes;
            return vsg::vec4Array::create(1);
        }, 1);
        auto live = cached(record, key);
        auto otherKey = key;
        otherKey.view.texture = textureWorld.reserveTexture().value();
        auto otherRecord = record;
        otherRecord.contentIdentity = "different-content";
        auto other = cached(otherRecord, otherKey);
        require(live && other && live != other, "eviction invalidated a live payload");
        auto again = cached(record, key);
        require(again != live && decodes == 3, "bounded cache did not evict its weak index entry");
    });
    test("failed texture decodes remain retryable and uncached control stays independent", [&] {
        using namespace RenderCore;
        TextureRecord record;
        record.contentIdentity = "content";
        TextureRealizationKey key;
        RenderWorld textureWorld;
        key.view.texture = textureWorld.reserveTexture().value();
        unsigned decodes = 0;
        auto cached = RenderVsg::cacheLiveTextures(
            [&](const TextureRecord&, const TextureRealizationKey&) -> vsg::ref_ptr<vsg::Data> {
                ++decodes;
                if (decodes == 1) return {};
                if (decodes == 2) throw std::runtime_error("decode fixture failure");
                return vsg::vec4Array::create(1);
            });
        require(!cached(record, key), "null decoder result was accepted");
        bool threw = false;
        try { (void)cached(record, key); }
        catch (const std::runtime_error&) { threw = true; }
        require(threw, "decoder error was hidden");
        auto live = cached(record, key);
        require(live && cached(record, key) == live && decodes == 3, "failed decode poisoned retry/cache");
        auto uncached = RenderVsg::cacheLiveTextures(
            [](const TextureRecord&, const TextureRealizationKey&) { return vsg::vec4Array::create(1); }, 0);
        auto first = uncached(record, key);
        auto second = uncached(record, key);
        require(first && second && first != second, "uncached control unexpectedly reused payloads");
    });
    test("GPU texture sharing is weak, sampler-exact, bounded and excludes dynamic data", [&] {
        RenderVsg::LiveTextureImages images(2);
        auto data = vsg::ubvec4Array2D::create(4, 4);
        data->properties.format = VK_FORMAT_R8G8B8A8_UNORM;
        auto sampler = vsg::Sampler::create();
        sampler->maxLod = VK_LOD_CLAMP_NONE;
        auto first = images.get(data, sampler);
        auto equivalent = vsg::Sampler::create();
        equivalent->maxLod = VK_LOD_CLAMP_NONE;
        require(first == images.get(data, equivalent), "equivalent samplers duplicated the GPU image");
        auto changed = vsg::Sampler::create();
        changed->maxLod = 0;
        auto different = images.get(data, changed);
        require(first != different, "incompatible mip policies shared an image");
        auto replacement = vsg::ubvec4Array2D::create(4, 4);
        auto replacementImage = images.get(replacement, equivalent);
        require(replacementImage != first, "changed pixels shared an image");
        vsg::observer_ptr<vsg::ImageInfo> weak(first);
        first = {};
        require(!weak.valid(), "image index kept an unused GPU image alive");
        data->properties.dataVariance = vsg::DYNAMIC_DATA;
        require(images.get(data, changed) != images.get(data, changed), "mutable images shared unexpectedly");
        RenderVsg::LiveTextureImages control(0);
        require(control.get(replacement, equivalent) != control.get(replacement, equivalent), "control shared images");
    });
    test("static sharing prune retains in-flight ownership then releases unused graph", [&] {
        auto cache = vsg::SharedObjects::create();
        auto child = vsg::Group::create();
        auto parent = vsg::Group::create();
        parent->addChild(child);
        cache->share(child);
        cache->share(parent);
        vsg::observer_ptr<vsg::Group> weakChild(child), weakParent(parent);
        auto inFlight = parent;
        parent = {}; child = {};
        cache->prune();
        require(weakChild.valid() && weakParent.valid(), "prune released fence-owned graph");
        inFlight = {}; // completion tracker drops its last ownership
        cache->prune();
        require(!weakChild.valid() && !weakParent.valid(), "prune retained orphan graph");
    });
    test("diagnostics retain sparse coverage after a long manual session", [&] {
        using Debug::GameplayDiagnostics::sampleSequence;
        require(sampleSequence(12) && !sampleSequence(13) && sampleSequence(30), "initial cadence changed");
        require(sampleSequence(36000) && !sampleSequence(36030) && sampleSequence(36300)
            && sampleSequence(100200), "late-session coverage silently stopped");
    });
    // Optional local-asset integration gate. Assets stay outside the repository:
    // effect-capture-tests.exe <loose-data-root> <archive> <relative-nif>
    if (argc == 4)
        test("real actor production translation, planning or particle playback", [&] {
            VFS::Manager assets;
            assets.addArchive(VFS::makeBsaArchive(argv[2], nullptr));
            assets.addArchive(std::make_unique<VFS::FileSystemArchive>(argv[1]));
            assets.buildIndex();
            Resource::ResourceSystem resources(&assets, 0, nullptr);
            const VFS::Path::Normalized model(argv[3]);
            const auto nif = resources.getNifFileManager()->get(model);
            const auto translated = NifRender::translateNif(Nif::FileView(*nif));
            if (translated.model.dynamicRequirements == 0)
            {
                RenderCore::RenderWorld world;
                RenderCore::RenderWorldPublisher publisher(world);
                const auto complete = NifRender::translateStaticNif(Nif::FileView(*nif), assets);
                const auto published = NifRender::publishTranslation(world, publisher, complete);
                require(published.applied(), "real rigid actor translation publication failed");
                const auto* record = world.get(published.binding.model);
                auto forced = NifRender::buildForcedActorSkeleton(*record, "integration:forced-skeleton");
                require(forced.valid(), forced.diagnostic.c_str());
                const auto skeleton = world.reserveSkeleton();
                require(skeleton && world.commit(*skeleton, std::move(forced.record)), "real rigid skeleton publication failed");
                const auto instance = world.reserveInstance();
                RenderCore::InstanceRecord actorInstance;
                actorInstance.model = published.binding.model;
                actorInstance.skeleton = *skeleton;
                require(instance && world.commit(*instance, std::move(actorInstance)), "real rigid instance publication failed");
                std::string diagnostic;
                const auto plan = RenderVsg::buildDynamicActorPlan(world, *instance, {}, &diagnostic);
                require(plan.has_value(), diagnostic.c_str());
                unsigned rigid = 0;
                for (const auto& draw : plan->asset.draws)
                {
                    const auto* mesh = world.get(draw.mesh);
                    if (!mesh->skinned && !mesh->morphed) ++rigid;
                }
                require(!plan->asset.draws.empty(), "real actor lost all drawable meshes");
                RenderCore::SingleViewFrameProducer producer;
                RenderCore::SingleViewFrameInput input;
                input.renderExtent = {800u, 600u}; input.outputExtent = input.renderExtent;
                input.environment.skyEnabled = false;
                RenderCore::SkeletonPoseInput pose;
                pose.instance = *instance; pose.skeleton = *skeleton;
                for (const auto& bone : world.get(*skeleton)->payload->bones)
                    pose.localTransforms.push_back(bone.bindLocal);
                input.skeletonPoses.push_back(std::move(pose));
                for (const auto& draw : plan->asset.draws)
                {
                    const auto* mesh = world.get(draw.mesh);
                    if (!mesh->morphed) continue;
                    RenderCore::MorphWeightInput weights;
                    weights.instance = *instance; weights.mesh = draw.mesh; weights.modelNode = draw.node;
                    weights.weights.resize(mesh->morphs->targets.size(), 0.f);
                    input.morphWeights.push_back(std::move(weights));
                }
                const auto frame = producer.produce(world, input);
                require(frame.has_value(), "real rigid frame publication failed");
                const auto evaluated = RenderVsg::evaluateDynamicActorAssetPlan(world, *frame, *plan);
                require(evaluated && evaluated->draws.size() == plan->asset.draws.size(), "real actor draw evaluation failed");
                std::cout << "Real actor planned and evaluated draws=" << plan->asset.draws.size()
                          << " rigid=" << rigid << " morphs=" << input.morphWeights.size() << '\n';
                for (const auto& node : record->payload->nodes)
                    if (!node.mesh && node.name.find("Shadow") != std::string::npos)
                    {
                        require((node.controllerFlags & RenderCore::modelControllerFlag(
                            RenderCore::ModelControllerFlag::Morph)) == 0, "omitted shadow retains drawable morph requirement");
                        std::cout << "Preserved non-drawable node: " << node.name << '\n';
                    }
                return;
            }
            require(translated.model.dynamicRequirements
                == RenderCore::modelDynamicRequirement(RenderCore::ModelDynamicRequirement::ParticleSystem),
                "asset does not select the production particle-actor compatibility route");
            NifOsg::Loader::setHiddenNodeMask(MWRender::Mask_UpdateVisitor);
            auto actor = NifOsg::Loader::load(Nif::FileView(*nif), resources.getImageManager(), resources.getBgsmFileManager());
            SceneUtil::AssignControllerSourcesVisitor assign(std::make_shared<SceneUtil::FrameTimeSource>());
            actor->accept(assign);
            SceneUtil::FindMaxControllerLengthVisitor duration;
            actor->accept(duration);
            const float seconds = std::max(4.f, duration.getMaxLength());
            require(std::isfinite(seconds) && seconds <= 600.f, "integration animation duration exceeds test budget");
            const unsigned frames = static_cast<unsigned>(std::ceil(seconds * 30.f)) + 2;
            std::cout << "Real actor animation seconds=" << seconds << " frames=" << frames << '\n';
            SceneUtil::UpdateOnlyVisitor update;
            SceneUtil::ParticlePlaybackVisitor playback;
            playback.setTraversalMask(~MWRender::Mask_UpdateVisitor);
            auto stamp = osg::ref_ptr<osg::FrameStamp>(new osg::FrameStamp);
            update.setFrameStamp(stamp);
            playback.setFrameStamp(stamp);
            std::size_t bodyDraws = 0, particleDraws = 0;
            for (unsigned frame = 1; frame <= frames; ++frame)
            {
                stamp->setFrameNumber(frame); stamp->setSimulationTime(frame / 30.0);
                update.setTraversalNumber(frame); actor->accept(update);
                playback.setTraversalNumber(frame); actor->accept(playback);
                auto captured = MWRender::captureV4ParticleActor(*actor, "real-actor", assets, nullptr, frame);
                require(captured.valid(), captured.diagnostic.c_str());
                for (const auto& draw : captured.draws)
                    if (draw.semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::Effect))
                        ++particleDraws;
                    else
                        ++bodyDraws;
            }
            std::cout << "Real actor captured body draws=" << bodyDraws << " particle draws=" << particleDraws << '\n';
            require(bodyDraws > 0 && particleDraws > 0, "real actor body or particle playback missing");
        });
    else if (argc != 1)
    {
        std::cerr << "Expected zero arguments or: <loose-data-root> <archive> <relative-nif>\n";
        ++failures;
    }
    std::cout << "Effect recovery failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
