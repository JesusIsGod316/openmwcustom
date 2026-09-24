// Actual OSG state/geometry capture and neutral frame publication. No GPU or game.
#include <apps/openmw/mwrender/v4skycapture.hpp>
#include <apps/openmw/mwrender/v4effectcapture.hpp>
#include <apps/openmw/mwrender/v4objectcaptureplan.hpp>
#include <apps/openmw/mwrender/v4persistentobject.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <osgUtil/UpdateVisitor>
#include <components/rendercore/frameproducer.hpp>
#include <components/rendercore/realizationkeys.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/sceneutil/disabledshadowtexture.hpp>
#include <cstring>
#include <chrono>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Switch>
#include <functional>
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <limits>

namespace
{
    using namespace RenderCore;
    class MemoryFile final : public VFS::File
    {
    public:
        Files::IStreamPtr open() override { return std::make_unique<std::istringstream>("sky image bytes"); }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "sky"; }
    };
    class Archive final : public VFS::Archive
    {
    public:
        MemoryFile file;
        void listResources(VFS::FileMap& out) override { out.insert_or_assign(VFS::Path::Normalized("textures/sky.dds"), &file); }
        bool contains(VFS::Path::NormalizedView path) const override { return path.value()=="textures/sky.dds"; }
        std::string getDescription() const override { return "native-sky-fixture"; }
    };
    void require(bool value,const char* why) { if(!value)throw std::runtime_error(why); }
    osg::ref_ptr<osg::Geometry> geometry()
    {
        auto g=osg::ref_ptr<osg::Geometry>(new osg::Geometry);
        auto v = osg::ref_ptr<osg::Vec3Array>(new osg::Vec3Array);
        v->push_back({0,0,0}); v->push_back({1,0,0}); v->push_back({0,1,0});
        auto uv = osg::ref_ptr<osg::Vec2Array>(new osg::Vec2Array);
        uv->push_back({0,0}); uv->push_back({1,0}); uv->push_back({0,1});
        auto colors = osg::ref_ptr<osg::Vec4Array>(new osg::Vec4Array);
        colors->push_back({1,1,1,1});
        g->setVertexArray(v); g->setTexCoordArray(0,uv);
        g->setColorArray(colors,osg::Array::BIND_OVERALL);
        g->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
        g->getOrCreateStateSet()->addUniform(new osg::Uniform("pass",0));
        return g;
    }
    osg::ref_ptr<osg::Texture2D> texture()
    {
        auto image=osg::ref_ptr<osg::Image>(new osg::Image);
        image->allocateImage(1,1,1,GL_RGBA,GL_UNSIGNED_BYTE);image->setFileName("textures/sky.dds");
        auto tex=osg::ref_ptr<osg::Texture2D>(new osg::Texture2D(image));tex->setName("diffuseMap");return tex;
    }
    struct Callback final:osg::NodeCallback
    {
        int calls=0;
        void operator()(osg::Node* node,osg::NodeVisitor* visitor) override {++calls;traverse(node,visitor);}
    };
    struct Scene
    {
        VFS::Manager vfs; NifRender::TextureIdentityCache identities; MWRender::V4SkyCapture capture;
        osg::ref_ptr<osg::Group> root=new osg::Group;
        Scene():identities(vfs){vfs.addArchive(std::make_unique<Archive>());vfs.buildIndex();}
        MWRender::V4SkyCapture::Result run(){return capture.capture(*root,identities);}
    };
    struct TrackedCallback final : SceneUtil::NodeCallback<TrackedCallback>
    {
        unsigned mask = SceneUtil::RenderMaterial | SceneUtil::RenderTexCoords;
        unsigned renderMutationMask() const noexcept override { return mask; }
        void changed() { publishRenderMutation(); }
        void operator()(osg::Node* node, osg::NodeVisitor* visitor) { traverse(node, visitor); }
    };
}
int main()
{
    unsigned tests=0,failures=0;
    auto test=[&](const char* name,auto body){++tests;try{body();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}};
    test("load-time native bindings bypass repeat geometry and material capture", [] {
        Scene s; auto g=geometry();s.root->addChild(g);
        MWRender::V4PersistentObject producer(*s.root);
        auto first=producer.publish("object",s.vfs,s.identities);
        require(first && first->valid() && first->draws.size()==1,"initial native publication");
        auto again=producer.publish("object",s.vfs,s.identities);
        require(again && again->draws[0].meshSnapshot==first->draws[0].meshSnapshot,"immutable geometry retained");
        require(producer.geometryBuilds==1 && producer.materialUpdates==0 && producer.reusedDraws==1,"capture not bypassed");
        MWRender::v4_effect_detail::CaptureVisitor visitor("object",true,s.vfs,&s.identities);
        s.root->accept(visitor); auto control=visitor.take();
        require(control.valid() && control.draws.size()==1,"control capture");
        require(control.draws[0].meshData().positions==again->draws[0].meshData().positions,"position parity");
        require(control.draws[0].worldTransform==again->draws[0].worldTransform,"transform parity");
    });
    test("native movement and inherited transforms do not rebuild geometry", [] {
        Scene s;osg::ref_ptr<NifOsg::MatrixTransform> parent=new NifOsg::MatrixTransform;
        parent->addChild(geometry());s.root->addChild(parent);
        MWRender::V4PersistentObject producer(*s.root);auto before=producer.publish("moving",s.vfs,s.identities);
        parent->setMatrix(osg::Matrix::translate(4,5,6));auto after=producer.publish("moving",s.vfs,s.identities);
        require(after && after->valid() && after->draws[0].worldTransform[3]==glm::vec4(4,5,6,1),"current transform");
        require(before->draws[0].worldTransform[3]==glm::vec4(0,0,0,1),"old frame mutated");
        require(producer.geometryBuilds==1 && producer.materialUpdates==0,"moving recaptured geometry/material");
    });
    test("movement setter notifications preserve parent propagation and absolute transforms", [] {
        Scene s;osg::ref_ptr<SceneUtil::PositionAttitudeTransform> parent=new SceneUtil::PositionAttitudeTransform;
        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> child=new SceneUtil::PositionAttitudeTransform;
        child->addChild(geometry());parent->addChild(child);s.root->addChild(parent);
        MWRender::V4PersistentObject producer(*s.root);auto before=producer.publish("movement",s.vfs,s.identities);
        parent->setPosition({10,0,0});child->setPosition({2,3,4});
        auto moved=producer.publish("movement",s.vfs,s.identities);
        require(moved && moved->draws[0].worldTransform[3]==glm::vec4(12,3,4,1),"parent transform not propagated");
        const auto revision=child->renderMutationRevision();child->setPosition({2,3,4});
        require(revision==child->renderMutationRevision(),"unchanged position published a mutation");
        child->setReferenceFrame(osg::Transform::ABSOLUTE_RF);
        auto absolute=producer.publish("movement",s.vfs,s.identities);
        require(absolute && absolute->draws[0].worldTransform[3]==glm::vec4(2,3,4,1),"absolute transform inherited parent");
        require(before->draws[0].meshSnapshot==absolute->draws[0].meshSnapshot,"movement rebuilt mesh");
        require(!producer.publish("replacement",s.vfs,s.identities),"replaced identity reused stale draw names");
    });
    test("controller contract changes are conservative and nested chains cannot hide mutations", [] {
        Scene s;s.root->addChild(geometry());osg::ref_ptr<TrackedCallback> callback=new TrackedCallback;
        s.root->setUpdateCallback(callback);MWRender::V4PersistentObject producer(*s.root);
        require(producer.publish("controller",s.vfs,s.identities).has_value(),"known callback rejected");
        callback->mask=SceneUtil::RenderUntracked;
        require(!producer.publish("controller",s.vfs,s.identities),"new unsupported controller ignored");
        callback->mask=SceneUtil::RenderTransform;MWRender::V4PersistentObject nested(*s.root);
        callback->setNestedCallback(new Callback);
        require(!nested.publish("controller",s.vfs,s.identities),"new nested callback ignored");
    });
    test("engine callback revision updates only affected UVs and retains old frames", [] {
        Scene s;auto g=geometry();s.root->addChild(g);s.root->addChild(geometry());
        auto* state=g->getOrCreateStateSet();state->setTextureAttributeAndModes(0,texture());
        osg::ref_ptr<osg::TexMat> texmat=new osg::TexMat;state->setTextureAttribute(0,texmat);
        osg::ref_ptr<TrackedCallback> callback=new TrackedCallback;g->setUpdateCallback(callback);
        MWRender::V4PersistentObject producer(*s.root);auto before=producer.publish("uv",s.vfs,s.identities);
        texmat->setMatrix(osg::Matrix::translate(0.5,0.25,0));callback->changed();
        auto after=producer.publish("uv",s.vfs,s.identities);
        require(after && after->valid(),"UV mutation publication");
        require(after->draws[0].meshData().texCoordSets[0][0]==glm::vec2(0.5f,0.25f),"UV controller ignored");
        require(before->draws[0].meshData().texCoordSets[0][0]==glm::vec2(0,0),"retained frame changed");
        require(before->draws[1].meshSnapshot==after->draws[1].meshSnapshot,"unaffected sibling rebuilt");
        require(producer.geometryBuilds==2 && producer.uvUpdates==1 && producer.materialUpdates==1,"unexpected mutation scope");
        const auto revision=callback->renderMutationRevision();osgUtil::UpdateVisitor update;s.root->accept(update);
        require(callback->renderMutationRevision()==revision+1,"real update callback did not publish");
    });
    test("native hidden draw retains dirty material and switch selection", [] {
        Scene s;osg::ref_ptr<osg::Switch> selection=new osg::Switch;auto g=geometry();
        selection->addChild(g,true);s.root->addChild(selection);
        osg::ref_ptr<TrackedCallback> callback=new TrackedCallback;selection->setUpdateCallback(callback);
        MWRender::V4PersistentObject producer(*s.root);auto first=producer.publish("hidden",s.vfs,s.identities);
        selection->setValue(0,false);callback->changed();auto hidden=producer.publish("hidden",s.vfs,s.identities);
        require(hidden && hidden->draws.empty(),"hidden draw submitted");
        selection->setValue(0,true);auto visible=producer.publish("hidden",s.vfs,s.identities);
        require(visible && visible->draws.size()==1 && producer.materialUpdates==1,"hidden dirtiness lost");
        require(first->draws[0].identity==visible->draws[0].identity,"instance identity unstable");
    });
    test("native UV layout leases grow shrink and fall back without corrupting old frames", [] {
        Scene s;auto g=geometry();s.root->addChild(g);
        auto* state=g->getOrCreateStateSet();state->setTextureAttribute(0,texture());
        state->setTextureAttribute(1,texture());
        state->setTextureAttribute(1,new SceneUtil::TextureType("emissiveMap"));
        g->setTexCoordArray(1,g->getTexCoordArray(0));
        osg::ref_ptr<osg::TexMat> texmat=new osg::TexMat;state->setTextureAttribute(1,texmat);
        osg::ref_ptr<TrackedCallback> callback=new TrackedCallback;g->setUpdateCallback(callback);
        const auto baseline=MWRender::V4PersistentObject::retainedBytes();
        std::size_t oneStreamBudget=0;
        {
            MWRender::V4PersistentObject producer(*s.root);
            auto first=producer.publish("uv-budget",s.vfs,s.identities);
            require(first && first->valid() && first->draws[0].meshData().texCoordSets.size()==1,"shared UV fixture");
            oneStreamBudget=MWRender::V4PersistentObject::retainedBytes();
            texmat->setMatrix(osg::Matrix::translate(0.5,0.25,0));callback->changed();
            auto expanded=producer.publish("uv-budget",s.vfs,s.identities);
            require(expanded && expanded->valid() && expanded->draws[0].meshData().texCoordSets.size()==2,"UV layout did not grow");
            require(MWRender::V4PersistentObject::retainedBytes()>oneStreamBudget,"UV growth was not charged");
            require(first->draws[0].meshData().texCoordSets.size()==1,"retained frame layout changed");
            texmat->setMatrix(osg::Matrix::identity());callback->changed();
            auto reduced=producer.publish("uv-budget",s.vfs,s.identities);
            require(reduced && reduced->valid() && reduced->draws[0].meshData().texCoordSets.size()==1,"UV layout did not shrink");
            require(MWRender::V4PersistentObject::retainedBytes()==oneStreamBudget,"unused UV storage lease retained");
        }
        require(MWRender::V4PersistentObject::retainedBytes()==baseline,"UV producer accounting leaked");
        MWRender::V4PersistentObject bounded(*s.root,oneStreamBudget);
        auto retained=bounded.publish("uv-budget",s.vfs,s.identities);
        require(retained && retained->valid(),"initial budgeted publication");
        texmat->setMatrix(osg::Matrix::translate(0.5,0.25,0));callback->changed();
        require(!bounded.publish("uv-budget",s.vfs,s.identities),"over-budget UV growth did not use fallback");
        require(MWRender::V4PersistentObject::retainedBytes()==baseline,"fallback retained producer lease");
        require(retained->valid() && retained->draws[0].meshData().texCoordSets.size()==1,"fallback invalidated old frame");
    });
    test("untracked custom controllers and replaced bindings use compatibility fallback", [] {
        Scene s;s.root->addChild(geometry());s.root->setUpdateCallback(new Callback);
        MWRender::V4PersistentObject unknown(*s.root);
        require(!unknown.supported() && !unknown.publish("custom",s.vfs,s.identities),"custom callback accepted");
        s.root->setUpdateCallback(nullptr);MWRender::V4PersistentObject known(*s.root);
        require(known.supported(),"known graph rejected");s.root->addChild(geometry());
        require(!known.publish("changed",s.vfs,s.identities),"new attachment silently omitted");
        MWRender::V4PersistentObject replaced(*s.root);s.root->setUpdateCallback(new Callback);
        require(!replaced.publish("changed",s.vfs,s.identities),"callback replacement ignored");
    });
    test("producer budget and unload release bindings without invalidating submitted data", [] {
        Scene s;s.root->addChild(geometry());const auto bytes=MWRender::V4PersistentObject::retainedBytes();
        MWRender::V4PersistentObject tiny(*s.root,0);require(!tiny.supported(),"budget ignored");
        std::optional<MWRender::V4EffectCaptureResult> frame;
        { MWRender::V4PersistentObject producer(*s.root);
          const auto beforeMesh=MWRender::V4PersistentObject::retainedBytes();
          frame=producer.publish("unload",s.vfs,s.identities);
          require(frame && frame->valid(),"initial publication");
          const auto minimumMeshCharge=sizeof(RenderCore::FrozenEffectMesh)
              +frame->draws[0].meshData().positions.size()*sizeof(glm::vec3)
              +frame->draws[0].meshData().indices.size()*sizeof(std::uint32_t);
          require(MWRender::V4PersistentObject::retainedBytes()-beforeMesh>=minimumMeshCharge,
              "producer budget omitted retained mesh storage");
          s.root=nullptr;
          require(!producer.publish("unload",s.vfs,s.identities),"expired binding accepted"); }
        require(MWRender::V4PersistentObject::retainedBytes()==bytes,"producer accounting leak");
        require(frame->valid() && frame->draws[0].meshSnapshot->valid(),"submitted mesh retired prematurely");
    });
    test("load-bound identities share a scope and survive cache replacement and generation invalidation", [] {
        Scene s;auto a=s.identities.bind(VFS::Path::Normalized("textures/sky.dds"));
        auto b=s.identities.bind(VFS::Path::Normalized("textures/sky.dds"));require(a==b,"duplicate binding slots");
        std::string identity;
        { NifRender::TextureIdentityCache::CaptureScope scope(s.identities);
          const auto& one=s.identities.resolveBound(*a);identity=one.contentIdentity;
          require(one.valid() && &one==&s.identities.resolveBound(*b),"bound scope did not share checked identity");
          s.vfs.buildIndex();require(s.identities.resolveBound(*a).valid(),"generation invalidation failed");
          s.identities.clear();require(s.identities.resolveBound(*a).contentIdentity==identity,"explicit invalidation failed"); }
        NifRender::TextureIdentityCache replacement(s.vfs,1);
        { NifRender::TextureIdentityCache::CaptureScope scope(replacement);
          require(replacement.resolveBound(*a).contentIdentity==identity,"cache replacement reused foreign state"); }
        { auto expired=replacement.bind(VFS::Path::Normalized("expired.dds")); }
        require(replacement.bind(VFS::Path::Normalized("textures/sky.dds"))!=nullptr,"expired binding slot not reclaimed");
    });
    test("loaded texture identity follows engine rebinding rather than polling unchanged assets", [] {
        struct File final : VFS::File {
            std::string bytes="first";int revision=1;unsigned metadataReads=0;
            Files::IStreamPtr open() override {return std::make_unique<std::istringstream>(bytes);}
            std::filesystem::file_time_type getLastModified() const override {
                ++const_cast<File*>(this)->metadataReads;
                return std::filesystem::file_time_type(std::filesystem::file_time_type::duration(revision));}
            std::string getStem() const override {return "loaded";}
        };
        struct Provider final : VFS::Archive {
            File file;
            void listResources(VFS::FileMap& out) override {out.insert_or_assign(VFS::Path::Normalized("textures/loaded.dds"),&file);}
            bool contains(VFS::Path::NormalizedView p) const override {return p.value()=="textures/loaded.dds";}
            std::string getDescription() const override {return "loaded fixture";}
        };
        auto provider=std::make_unique<Provider>();auto* file=&provider->file;VFS::Manager vfs;
        vfs.addArchive(std::move(provider));vfs.buildIndex();NifRender::TextureIdentityCache identities(vfs);
        auto binding=identities.bindLoaded(VFS::Path::Normalized("textures/loaded.dds"));std::string first;
        {NifRender::TextureIdentityCache::CaptureScope scope(identities);
         require(identities.resolveLoaded(*binding).valid(),"fixture texture identity missing");
         first=identities.resolveLoaded(*binding).contentIdentity;}
        const auto reads=file->metadataReads;file->bytes="second";++file->revision;
        {NifRender::TextureIdentityCache::CaptureScope scope(identities);
         require(identities.resolveLoaded(*binding).contentIdentity==first,"resident asset changed without reload");}
        require(file->metadataReads==reads,"unchanged loaded binding polled filesystem");
        identities.bindLoaded(VFS::Path::Normalized("textures/loaded.dds"));
        {NifRender::TextureIdentityCache::CaptureScope scope(identities);
         require(identities.resolveLoaded(*binding).contentIdentity!=first,"engine rebind did not reload identity");}
        file->bytes="third";++file->revision;identities.clear();
        auto third=identities.resolveLoaded(*binding).contentIdentity;
        file->bytes="fourth";++file->revision;vfs.buildIndex();
        require(identities.resolveLoaded(*binding).contentIdentity!=third,"VFS rebuild did not invalidate resident identity");
    });
    test("batched texture metadata preserves edits replacement and unknown provider fallback", [] {
        struct NativeFile : VFS::File {
            std::filesystem::path path; mutable unsigned reads=0;
            Files::IStreamPtr open() override { return std::make_unique<std::ifstream>(path,std::ios::binary); }
            std::filesystem::file_time_type getLastModified() const override { ++reads;return std::filesystem::last_write_time(path); }
            std::optional<std::filesystem::path> getMetadataPath() const override { return path; }
            std::string getStem() const override {return "metadata";}
        };
        struct NativeArchive : VFS::Archive {
            std::vector<NativeFile> files;
            void listResources(VFS::FileMap& out) override {
                for(unsigned i=0;i<files.size();++i)out.insert_or_assign(VFS::Path::Normalized("textures/"+std::to_string(i)+".dds"),&files[i]);
            }
            bool contains(VFS::Path::NormalizedView) const override {return true;}
            std::string getDescription() const override {return "metadata test";}
        };
        // Dedicated fixture owns exactly these files; no recursive cleanup.
        const auto directory=std::filesystem::temp_directory_path()/(
            "openmw-metadata-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory),"fixture directory collision");
        struct Cleanup {std::filesystem::path path;~Cleanup(){std::error_code ec;for(unsigned i=0;i<40;++i)std::filesystem::remove(path/std::to_string(i),ec);std::filesystem::remove(path,ec);}} cleanup{directory};
        auto archive=std::make_unique<NativeArchive>();archive->files.resize(80);
        for(unsigned i=0;i<40;++i){std::ofstream(directory/std::to_string(i))<<"original"<<i;}
        for(unsigned i=0;i<80;++i)archive->files[i].path=directory/std::to_string(i%40);
        auto* source=archive.get();VFS::Manager vfs;vfs.addArchive(std::move(archive));vfs.buildIndex();
        NifRender::TextureIdentityCache cache(vfs);
        std::vector<std::string> first;
        {NifRender::TextureIdentityCache::CaptureScope scope(cache);for(unsigned i=0;i<80;++i)first.push_back(cache.resolve(VFS::Path::Normalized("textures/"+std::to_string(i)+".dds")).contentIdentity);}
        unsigned before=0;for(auto& file:source->files)before+=file.reads;
        {NifRender::TextureIdentityCache::CaptureScope scope(cache);for(unsigned i=0;i<80;++i)require(first[i]==cache.resolve(VFS::Path::Normalized("textures/"+std::to_string(i)+".dds")).contentIdentity,"warm metadata changed identity");}
        unsigned after=0;for(auto& file:source->files)after+=file.reads;
        require(after-before==(Misc::environmentFlag<"OPENMW_V4_BATCH_TEXTURE_METADATA">()?0u:80u),"metadata batch failed to avoid duplicate provider reads");
        const auto oldTime=std::filesystem::last_write_time(directory/"0");
        {std::ofstream(directory/"0")<<"changed payload";}
        std::filesystem::last_write_time(directory/"0",oldTime+std::chrono::seconds(2));
        {NifRender::TextureIdentityCache::CaptureScope scope(cache);require(cache.resolve(VFS::Path::Normalized("textures/0.dds")).contentIdentity!=first[0],"next capture froze modified file");}
        auto replacement=std::make_unique<Archive>();vfs.addArchive(std::move(replacement));vfs.buildIndex();
        {NifRender::TextureIdentityCache::CaptureScope scope(cache);require(cache.resolve(VFS::Path::Normalized("textures/sky.dds")).valid(),"unknown metadata provider lost compatibility path");}
        // A predicted path can disappear after a cell unload. Don't fail until
        // requested, then preserve the ordinary missing-file error.
        {NifRender::TextureIdentityCache::CaptureScope scope(cache);(void)cache.resolve(VFS::Path::Normalized("textures/0.dds"));}
        std::filesystem::remove(directory/"0");
        {NifRender::TextureIdentityCache::CaptureScope scope(cache);require(cache.resolve(VFS::Path::Normalized("textures/sky.dds")).valid(),"unused missing predicted file failed capture");
         bool threw=false;try{(void)cache.resolve(VFS::Path::Normalized("textures/0.dds"));}catch(const std::filesystem::filesystem_error&){threw=true;}require(threw,"requested missing file hidden");}
    });
    test("material value cache observes unmarked edits and agrees with full capture", [] {
        using namespace MWRender::v4_effect_detail;
        Scene s; auto state = osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        auto material = osg::ref_ptr<SceneUtil::Material>(new SceneUtil::Material);
        auto alpha = osg::ref_ptr<osg::Uniform>(new osg::Uniform("alpha", 1.f));
        auto tex = texture();
        state->setAttribute(material); state->addUniform(alpha); state->setTextureAttribute(0, tex);
        MaterialValueCache cache;
        auto check = [&](bool expectHit) {
            CapturedMaterial full; std::string why;
            require(captureMaterialUncached({}, nullptr, s.vfs, full, why, &s.identities, false, state), why.c_str());
            const auto* stored = cache.find(*state, full.material.textureApply, full.material.unlit, s.identities);
            require(bool(stored) == expectHit, "material cache hit/miss contract");
            if (stored) require(stored->material == full.material && std::equal(stored->textures.begin(),
                stored->textures.end(), full.textures.begin(), full.textures.end(), [](const auto& a, const auto& b) {
                    return a.texture == b.texture && a.binding == b.binding;
                }),
                "material cache differs from full translation");
            else cache.remember(*state, full);
        };
        check(false); check(true);
        (*alpha->getFloatArray())[0] = .37f; check(false); check(true);
        material->setEmission({.1f,.2f,.3f,1.f}); check(false); check(true);
        tex->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_BORDER); check(false); check(true);
        tex->setName("normalMap"); check(false); check(true);
        auto type = osg::ref_ptr<SceneUtil::TextureType>(new SceneUtil::TextureType("diffuseMap"));
        state->setTextureAttribute(0,type); check(false); check(true);
        type->setName("normalMap"); check(false); check(true);
        auto image = osg::ref_ptr<osg::Image>(new osg::Image);
        image->allocateImage(2,3,1,GL_RGBA,GL_UNSIGNED_BYTE); image->setFileName("textures/sky.dds");
        tex->setImage(image); check(false); check(true);
        state->setDefine("FORCE_OPAQUE","1"); check(false); check(true);
        state->setMode(GL_BLEND,osg::StateAttribute::ON); check(false); check(true);
        auto blend = osg::ref_ptr<osg::BlendFunc>(new osg::BlendFunc(GL_SRC_ALPHA, GL_ONE));
        state->setAttribute(blend); check(false); check(true);
        blend->setFunction(GL_ONE, GL_ONE); check(false); check(true);
        state->addUniform(new osg::Uniform("alpha", .9f)); check(false); check(true);
        state->removeUniform("alpha"); check(false); check(true);
        require(!cache.find(*state, RenderCore::TextureApplyMode::Replace, false, s.identities), "path apply mode ignored");
        require(!cache.find(*state, RenderCore::TextureApplyMode::Modulate, true, s.identities), "path shader prefix ignored");
        for (const bool cached : {false,true})
        {
            NifRender::TextureIdentityCache::CaptureScope scope(s.identities);
            const auto begin = std::chrono::steady_clock::now();
            for (unsigned i=0; i<20000; ++i)
            {
                if (cached) require(cache.find(*state, TextureApplyMode::Modulate, false, s.identities), "warm material miss");
                else { CapturedMaterial result; std::string why;
                    require(captureMaterialUncached({},nullptr,s.vfs,result,why,&s.identities,false,state),why.c_str()); }
            }
            std::cout << "MATERIAL repeats=20000 cache=" << cached << " ms="
                << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count() << '\n';
        }
    });
    test("effect capture respects live switches without ticking controllers", [] {
        Scene s; auto selection = osg::ref_ptr<osg::Switch>(new osg::Switch);
        auto a = geometry(), b = geometry(); a->setName("A"); b->setName("B");
        selection->addChild(a, true); selection->addChild(b, false);
        auto cb = osg::ref_ptr<Callback>(new Callback); selection->setUpdateCallback(cb);
        const bool active = std::getenv("OPENMW_V4_ACTIVE_SWITCH_CAPTURE") != nullptr;
        auto run = [&] { return MWRender::captureV4WholeEffectSubtree(*selection, "switch", s.vfs, &s.identities); };
        auto first = run(); require(first.valid() && first.draws.size() == (active ? 1u : 2u), "switch capture count");
        selection->setValue(0, false); selection->setValue(1, true);
        auto second = run(); require(second.valid() && second.draws.size() == (active ? 1u : 2u), "live switch update");
        if (active) require(second.draws[0].material.sourceIdentity.ends_with(":B"), "switched branch frozen");
        require(cb->calls == 0, "capture ran gameplay update callback");
        selection->setAllChildrenOff(); require(!active || run().draws.empty(), "disabled branches captured");
    });
    test("persistent object plans preserve topology transforms switches and unmarked edits", [] {
        Scene s; MWRender::V4ObjectCapturePlans plans;
        auto branch = osg::ref_ptr<osg::Switch>(new osg::Switch);
        auto transform = osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform);
        auto a = geometry(), b = geometry();
        transform->addChild(a); branch->addChild(transform); branch->addChild(b); s.root->addChild(branch);
        auto cb = osg::ref_ptr<Callback>(new Callback); transform->setUpdateCallback(cb);
        auto compare = [&] {
            auto actual = plans.capture(*s.root,"binding",s.vfs,s.identities);
            require(actual && actual->valid(), "known graph did not produce a valid binding plan");
            MWRender::v4_effect_detail::CaptureVisitor reference("binding",true,s.vfs,&s.identities);
            reference.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
            s.root->accept(reference); auto expected = reference.take();
            require(actual->draws.size() == expected.draws.size(), "binding active topology differs");
            for (std::size_t i=0;i<actual->draws.size();++i)
            {
                const auto& x=actual->draws[i]; const auto& y=expected.draws[i];
                require(x.identity==y.identity && x.worldTransform==y.worldTransform && x.material==y.material
                    && x.meshData().positions==y.meshData().positions && x.meshData().indices==y.meshData().indices,
                    "binding plan changed capture semantics");
            }
            require(cb->calls==0,"binding plan advanced gameplay callbacks");
        };
        compare(); compare(); require(plans.rebuilds==1 && plans.reuses==1,"unchanged plan rebuilt");
        transform->setMatrix(osg::Matrix::translate(3,4,5)); compare();
        branch->setValue(0,false); compare(); branch->setValue(0,true); compare();
        transform->setNodeMask(0); compare(); transform->setNodeMask(~0u); compare();
        static_cast<osg::Vec3Array*>(a->getVertexArray())->at(0).x() = 2.f; compare();
        a->getStateSet()->addUniform(new osg::Uniform("alpha",.25f)); compare();
        require(plans.rebuilds==1,"value mutation rebuilt topology");
        transform->setReferenceFrame(osg::Transform::ABSOLUTE_RF); compare();
        transform->removeChild(a); transform->addChild(b); compare();
        require(plans.rebuilds==2,"attachment edit was not rebuilt");
        auto pat=osg::ref_ptr<SceneUtil::PositionAttitudeTransform>(new SceneUtil::PositionAttitudeTransform);
        pat->setPosition({10,20,30}); pat->setScale({-2,3,4}); pat->addChild(a); s.root->addChild(pat); compare();
        pat->setAttitude(osg::Quat(.5,osg::Vec3(0,0,1))); compare();
        struct Unknown : osg::Group { void traverse(osg::NodeVisitor&) override {} };
        auto unknown=osg::ref_ptr<Unknown>(new Unknown); unknown->addChild(geometry());s.root->addChild(unknown);
        require(!plans.capture(*s.root,"binding",s.vfs,s.identities),"custom traversal silently bypassed");
        s.root->removeChild(unknown); compare();
        plans.clear(); compare();
    });
    test("persistent producer publishes only mutations and withdraws on fallback",[]{
        Scene s; auto g=geometry(); s.root->addChild(g);
        MWRender::V4PersistentObject producer(*s.root);
        RenderCore::PersistentDrawWorld world;
        world.begin(1); auto first=producer.publish("retained",s.vfs,s.identities,&world,true);
        require(first && first->valid() && first->draws.empty(),"persistent producer used immediate stream");
        auto initial=world.finish(); require(initial->size()==1,"persistent draw missing");
        const auto* entry=initial->get(0); require(entry && entry->resource->draw().material.environmentMapPreLight,
            "engine environment-map ordering setting lost");
        world.begin(1); producer.publish("retained",s.vfs,s.identities,&world,true);
        require(world.finish()==initial && producer.geometryBuilds==1,"unchanged producer copied/rebuilt");
        s.root->setNodeMask(0); world.begin(1); producer.publish("retained",s.vfs,s.identities,&world,true);
        auto hidden=world.finish(); require(!hidden->get(0)->visible && initial->get(0)->visible,"visibility mutation lost");
        s.root->setNodeMask(~0u); world.begin(1); producer.publish("retained",s.vfs,s.identities,&world,false);
        auto rebound=world.finish(); require(rebound->get(0)->visible
            && !rebound->get(0)->resource->draw().material.environmentMapPreLight,"unhide/rebind lost");
        s.root->addChild(geometry()); world.begin(1);
        require(!producer.publish("retained",s.vfs,s.identities,&world),"unknown topology skipped fallback");
        require(world.finish()->size()==0 && initial->get(0),"fallback left duplicate or invalidated old frame");
    });
    test("sky capture preserves native pass, colour and transform",[]{
        Scene s;auto g=geometry();auto p=osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform(osg::Matrix::translate(1,2,3)));
        p->addChild(g);s.root->addChild(p);g->getStateSet()->addUniform(new osg::Uniform("diffuseColor",osg::Vec4(.2,.3,.4,.5)));
        auto r=s.run();require(bool(r),r.diagnostic.c_str());auto& d=r.snapshot->draws.at(0);
        require(d.pass==SkyPass::Atmosphere && d.diffuseColor==glm::vec4(.2,.3,.4,.5),"native colour lost");
        require(glm::vec3(d.transform[3])==glm::vec3(1,2,3),"local transform lost");
    });
    test("sky geometry sharing excludes mutable per-frame uniforms",[]{
        Scene s;auto g=geometry();s.root->addChild(g);auto a=s.run();require(bool(a),a.diagnostic.c_str());
        g->getStateSet()->addUniform(new osg::Uniform("diffuseColor",osg::Vec4(.5,.4,.3,1)));
        auto b=s.run();require(bool(b),b.diagnostic.c_str());require(a.snapshot->draws[0].mesh==b.snapshot->draws[0].mesh,"uniform change copied mesh");
        require(a.snapshot->draws[0].identity==b.snapshot->draws[0].identity,"sky identity unstable");
    });
    test("dirty authored array publishes new immutable geometry",[]{
        Scene s;auto g=geometry();s.root->addChild(g);auto a=s.run();auto* v=static_cast<osg::Vec3Array*>(g->getVertexArray());
        (*v)[0].x()=3;v->dirty();auto b=s.run();require(bool(a)&&bool(b),"capture failed");
        require(a.snapshot->draws[0].mesh!=b.snapshot->draws[0].mesh,"dirty mesh reused");
        require(a.snapshot->draws[0].mesh->positions[0].x==0 && b.snapshot->draws[0].mesh->positions[0].x==3,"previous geometry mutated");
    });
    test("sky capture follows active switches without advancing updates",[]{
        Scene s;auto p=osg::ref_ptr<osg::Switch>(new osg::Switch);auto a=geometry(),b=geometry();
        auto cb=osg::ref_ptr<Callback>(new Callback);p->setUpdateCallback(cb);p->addChild(a,true);p->addChild(b,false);s.root->addChild(p);
        auto r=s.run();require(bool(r)&&r.snapshot->draws.size()==1,"inactive sky branch captured");require(cb->calls==0,"capture advanced update callback");
    });
    test("cloud pass preserves original TexMat and winning VFS identity",[]{
        Scene s;auto g=geometry();s.root->addChild(g);auto* st=g->getStateSet();st->getUniform("pass")->set(2);
        st->setTextureAttribute(0,texture());st->setTextureAttribute(0,new osg::TexMat(osg::Matrix::translate(2,3,0)));
        auto r=s.run();require(bool(r),r.diagnostic.c_str());const auto& d=r.snapshot->draws[0];
        require(d.pass==SkyPass::Clouds && glm::vec3(d.uvTransform[3])==glm::vec3(2,3,0),"cloud scroll lost");
        require(d.textures[0].texture.sourceIdentity=="textures/sky.dds" && !d.textures[0].texture.contentIdentity.empty(),"winning texture identity missing");
    });
    test("moon pass keeps phase and mask textures and separate uniforms",[]{
        Scene s;auto g=geometry();s.root->addChild(g);auto* st=g->getStateSet();st->getUniform("pass")->set(3);
        st->setTextureAttribute(0,texture());st->setTextureAttribute(1,texture());
        st->addUniform(new osg::Uniform("moonBlend",osg::Vec4(1,.5,.25,0)));st->addUniform(new osg::Uniform("atmosphereFade",osg::Vec4(.1,.2,.3,.4)));
        auto r=s.run();require(bool(r),r.diagnostic.c_str());const auto& d=r.snapshot->draws[0];
        require(d.textures.size()==2 && d.moonBlend==glm::vec4(1,.5,.25,0) && d.atmosphereFade==glm::vec4(.1,.2,.3,.4),"moon masks lost");
    });
    test("authored textured sky without UVs fails closed",[]{
        Scene s;auto g=geometry();g->getStateSet()->getUniform("pass")->set(1);g->getStateSet()->setTextureAttribute(0,texture());g->setTexCoordArray(0,nullptr);s.root->addChild(g);
        require(!s.run(),"authored sky UVs fabricated");
    });
    test("occlusion-dependent sky work is counted rather than treated as ordinary geometry",[]{
        Scene s;auto a=geometry(),b=geometry();a->getStateSet()->getUniform("pass")->set(5);b->getStateSet()->getUniform("pass")->set(6);s.root->addChild(a);s.root->addChild(b);
        auto r=s.run();require(bool(r)&&r.snapshot->draws.empty()&&r.snapshot->deferredOcclusionDraws==2,"unimplemented occlusion not explicit");
    });
    test("unknown native sky pass fails closed",[]{Scene s;auto g=geometry();g->getStateSet()->getUniform("pass")->set(9);s.root->addChild(g);require(!s.run(),"unknown pass accepted");});
    test("preview captures independent alpha blend factors and force opaque",[]{
        Scene s;auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);st->setMode(GL_BLEND,osg::StateAttribute::ON);
        st->setAttribute(new osg::BlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA));st->setDefine("FORCE_OPAQUE","1");
        MWRender::v4_effect_detail::CapturedMaterial m;std::string diagnostic;
        require(MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,diagnostic,&s.identities,true),diagnostic.c_str());
        require(m.material.separateAlphaBlend && m.material.sourceAlphaBlend==BlendFactor::One && m.material.forceOpaqueAlpha,"preview alpha contract lost");
    });
    test("only named engine-owned preview depth sentinel bypasses authored-image capture",[]{
        Scene s;auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);auto t=osg::ref_ptr<osg::Texture2D>(new osg::Texture2D);
        t->setName("openmw.character-preview.depth-sentinel");t->setInternalFormat(GL_DEPTH_COMPONENT);t->setShadowComparison(true);t->setShadowCompareFunc(osg::Texture::ALWAYS);st->setTextureAttribute(7,t);
        MWRender::v4_effect_detail::CapturedMaterial m;std::string d;
        require(MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities,true)&&m.textures.empty(),"preview sentinel treated as authored image");
        require(!MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities,false),"world texture guard weakened");
        t->setName("mod texture");require(!MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities,true),"user image silently omitted");
    });
    test("disabled-shadow factory preserves legacy depth comparison without a VFS filename",[]{
        const auto t=SceneUtil::makeDisabledShadowTexture();
        require(SceneUtil::isDisabledShadowTexture(*t),"factory and capture contract disagree");
        require(t->getWrap(osg::Texture::WRAP_S)==osg::Texture::CLAMP_TO_EDGE
            && t->getWrap(osg::Texture::WRAP_T)==osg::Texture::CLAMP_TO_EDGE,"legacy shadow wraps changed");
        require(t->getImage() && t->getImage()->getFileName().empty(),"generated image given a false VFS identity");
    });
    test("preview excludes image-backed disabled shadows but retains authored diffuse and UVs",[]{
        Scene s;auto root=osg::ref_ptr<osg::Group>(new osg::Group);auto g=geometry();root->addChild(g);
        auto* state=root->getOrCreateStateSet();const auto shadow=SceneUtil::makeDisabledShadowTexture();
        for(unsigned unit: {5u,6u,8u}) {
            state->setTextureAttribute(unit,shadow,osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE|osg::StateAttribute::PROTECTED);
            state->addUniform(new osg::Uniform(("shadowTexture"+std::to_string(unit)).c_str(),static_cast<int>(unit)));
        }
        g->getOrCreateStateSet()->setTextureAttribute(0,texture());
        RenderCore::ImmediateEffectDraw draw;std::string d;
        const bool ok=MWRender::v4_effect_detail::captureGeometry(*g,{root.get()},s.vfs,"shadow-preview",draw,d,&s.identities,true);
        require(ok,d.c_str());require(draw.textures.size()==1 && draw.textures[0].texture.sourceIdentity=="textures/sky.dds",
            "authored image removed or generated shadow uploaded as material");
        require(draw.meshData().texCoordSets.size()==1 && draw.meshData().texCoordSets[0].size()==3,"shadow units consumed authored UVs");
        require(state->getTextureAttribute(6,osg::StateAttribute::TEXTURE)==shadow.get(),"source state mutated during capture");
    });
    test("world capture still rejects the disabled-shadow image as an authored material",[]{
        Scene s;auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        st->setTextureAttribute(6,SceneUtil::makeDisabledShadowTexture());
        MWRender::v4_effect_detail::CapturedMaterial m;std::string d;
        require(!MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities,false),"world image guard weakened");
    });
    test("preview rejects unnamed depth images and modified engine sentinels",[]{
        Scene s;
        for(int change=0;change<5;++change) {
            auto t=SceneUtil::makeDisabledShadowTexture();
            switch(change) {
                case 0:t->setName("");break;
                case 1:t->setShadowCompareFunc(osg::Texture::LESS);break;
                case 2:t->setShadowComparison(false);break;
                case 3:{const float depth=1;std::memcpy(t->getImage()->data(),&depth,sizeof(depth));break;}
                case 4:t->getImage()->allocateImage(1,1,1,GL_RGBA,GL_UNSIGNED_BYTE);break;
            }
            require(!SceneUtil::isDisabledShadowTexture(*t),"modified or unmarked texture accepted as sentinel");
            auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);st->setTextureAttribute(6,t);
            MWRender::v4_effect_detail::CapturedMaterial m;std::string d;
            require(!MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities,true),"unknown generated image silently dropped");
        }
    });
    test("marker name does not hide an authored texture",[]{
        Scene s;auto t=texture();t->setName(SceneUtil::DisabledShadowTextureName);
        auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);st->setTextureAttribute(0,t);
        MWRender::v4_effect_detail::CapturedMaterial m;std::string d;
        require(!SceneUtil::isDisabledShadowTexture(*t),"authored file accepted as engine sentinel");
        require(MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities,true),d.c_str());
        require(m.textures.size()==1 && m.textures[0].texture.sourceIdentity=="textures/sky.dds","authored texture lost");
    });
    test("unknown preview texture errors identify unit image state and drawable",[]{
        Scene s;auto g=geometry();g->setName("preview-head-fixture");
        auto t=osg::ref_ptr<osg::Texture2D>(new osg::Texture2D);t->setName("unknown-generated-texture");
        g->getStateSet()->setTextureAttribute(6,t);
        RenderCore::ImmediateEffectDraw draw;std::string d;
        require(!MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"preview-test",draw,d,&s.identities,true),
            "unknown preview texture accepted");
        require(d.find("texture_unit=6")!=std::string::npos && d.find("image_present=0")!=std::string::npos
            && d.find("unknown-generated-texture")!=std::string::npos && d.find("preview-head-fixture")!=std::string::npos,
            "texture failure lacks actionable provenance");
    });
    test("evaluated BumpTexture preserves authored matrix and luminance parameters",[]{
        Scene s;auto g=geometry();auto* state=g->getStateSet();
        state->setTextureAttribute(0,texture());state->setTextureAttribute(0,new SceneUtil::TextureType("bumpMap"));
        state->addUniform(new osg::Uniform("bumpMapMatrix",osg::Matrix2(2.f,3.f,5.f,7.f)));
        state->addUniform(new osg::Uniform("envMapLumaBias",osg::Vec2f(.4f,.6f)));
        RenderCore::ImmediateEffectDraw draw;std::string d;
        require(MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"bump",draw,d,&s.identities),d.c_str());
        require(draw.material.bumpParametersEnabled,"captured BumpTexture lost its shader parameters");
        require(draw.material.bumpMapMatrix==glm::vec4(2.f,3.f,5.f,7.f),"bump matrix transposed or changed");
        require(draw.material.environmentMapLumaBias==glm::vec2(.4f,.6f),"bump luminance scale/bias lost");
        require(draw.textures.size()==1 && draw.textures[0].binding.role==TextureRole::Bump
            && draw.meshData().texCoordSets.size()==1,"authored bump stage/coordinates lost");
        require(draw.textures[0].binding.colorSpace==TextureColorSpace::Data,"bump treated as colour data");
    });
    test("evaluated bump follows live controller uniforms and winning inherited state",[]{
        Scene s;auto parent=osg::ref_ptr<osg::Group>(new osg::Group);auto* ps=parent->getOrCreateStateSet();
        auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        st->setTextureAttribute(0,texture());st->setTextureAttribute(0,new SceneUtil::TextureType("bumpMap"));
        ps->addUniform(new osg::Uniform("bumpMapMatrix",osg::Matrix2(2.f,3.f,5.f,7.f)),osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        ps->addUniform(new osg::Uniform("envMapLumaBias",osg::Vec2f(.4f,.6f)));
        st->addUniform(new osg::Uniform("bumpMapMatrix",osg::Matrix2(1.f,0.f,0.f,1.f)));
        MWRender::v4_effect_detail::CapturedMaterial first,second;std::string d;
        require(MWRender::v4_effect_detail::captureMaterial({parent.get()},st,s.vfs,first,d,&s.identities),d.c_str());
        require(first.material.bumpMapMatrix==glm::vec4(2,3,5,7),"inherited override not respected");
        ps->getUniform("bumpMapMatrix")->set(osg::Matrix2(11.f,13.f,17.f,19.f));
        ps->getUniform("envMapLumaBias")->set(osg::Vec2f(.8f,.1f));
        require(MWRender::v4_effect_detail::captureMaterial({parent.get()},st,s.vfs,second,d,&s.identities),d.c_str());
        require(second.material.bumpMapMatrix==glm::vec4(11,13,17,19)
            && second.material.environmentMapLumaBias==glm::vec2(.8f,.1f),"controller values frozen");
        require(first.material.bumpMapMatrix!=second.material.bumpMapMatrix,"material reuse cannot see a bump change");
    });
    test("bump uniforms without a bump texture do not enable a phantom stage",[]{
        Scene s;auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        st->addUniform(new osg::Uniform("bumpMapMatrix",osg::Matrix2(2.f,3.f,5.f,7.f)));
        st->addUniform(new osg::Uniform("envMapLumaBias",osg::Vec2f(.4f,.6f)));
        MWRender::v4_effect_detail::CapturedMaterial m;std::string d;
        require(MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities),d.c_str());
        require(!m.material.bumpParametersEnabled && m.textures.empty(),"orphan uniform creates a bump stage");
    });
    test("missing wrong-type nonfinite and multiple bump contracts fail explicitly",[]{
        Scene s;
        for(int change=0;change<5;++change){
            auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);
            st->setTextureAttribute(0,texture());st->setTextureAttribute(0,new SceneUtil::TextureType("bumpMap"));
            st->addUniform(new osg::Uniform("bumpMapMatrix",osg::Matrix2(2.f,3.f,5.f,7.f)));
            st->addUniform(new osg::Uniform("envMapLumaBias",osg::Vec2f(.4f,.6f)));
            switch(change){
            case 0:st->removeUniform("bumpMapMatrix");break;
            case 1:st->removeUniform("envMapLumaBias");st->addUniform(new osg::Uniform("envMapLumaBias",1.f));break;
            case 2:st->getUniform("bumpMapMatrix")->set(osg::Matrix2(1.f,0.f,0.f,std::numeric_limits<float>::infinity()));break;
            case 3:st->getUniform("envMapLumaBias")->set(osg::Vec2f(std::numeric_limits<float>::quiet_NaN(),0.f));break;
            case 4:st->setTextureAttribute(1,texture());st->setTextureAttribute(1,new SceneUtil::TextureType("bumpMap"));break;
            }
            MWRender::v4_effect_detail::CapturedMaterial m;std::string d;
            require(!MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities),"unsupported bump contract accepted");
            require(d.find("BumpTexture")!=std::string::npos,"bump error is not actionable");
        }
    });
    test("normal and glow material roles remain distinct",[]{
        Scene s;auto st=osg::ref_ptr<osg::StateSet>(new osg::StateSet);st->setTextureAttribute(1,texture());st->setTextureAttribute(1,new SceneUtil::TextureType("normalMap"));
        st->setTextureAttribute(2,texture());st->setTextureAttribute(2,new SceneUtil::TextureType("emissiveMap"));
        MWRender::v4_effect_detail::CapturedMaterial m;std::string d;
        require(MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,m,d,&s.identities),d.c_str());
        require(m.textures.size()==2 && m.textures[0].binding.role==TextureRole::Normal && m.textures[0].binding.colorSpace==TextureColorSpace::Data
            && m.textures[1].binding.role==TextureRole::Emissive && m.textures[1].binding.colorSpace==TextureColorSpace::Srgb,"normal was used as emissive colour");
    });
    test("neutral preview payload is isolated and stable-slot publication retains dependency",[]{
        RenderWorld world;SingleViewFrameProducer producer;SingleViewFrameInput input;input.renderExtent=input.outputExtent={128,128};
        auto scene=std::make_shared<IsolatedSceneSnapshot>();scene->identity=1;scene->revision=1;scene->viewportExtent={64,64};
        SingleViewFrameInput::AuxiliaryView view;view.extent={128,128};view.stableSlot=(1u<<30)+1;view.sampledByMain=true;view.isolatedScene=scene;input.auxiliaryViews={view};
        auto frame=producer.prepare(world,input);require(frame && frame->valid(),"isolated frame rejected");
        require(frame->views().back().isolatedScene==scene && !frame->views().front().isolatedScene,"preview payload attached to main scene");
        input.auxiliaryViews[0].kind=ViewKind::Map;require(!producer.prepare(world,input),"preview attached to map");
        input.auxiliaryViews[0].kind=ViewKind::Preview;scene->viewportExtent={256,64};require(!producer.prepare(world,input),"oversize viewport accepted");
    });
    test("nonfinite isolated lighting and sky state are rejected",[]{
        RenderWorld world;SingleViewFrameProducer p;SingleViewFrameInput i;i.renderExtent=i.outputExtent={32,32};
        auto scene=std::make_shared<IsolatedSceneSnapshot>();scene->identity=1;scene->revision=1;scene->viewportExtent={32,32};scene->ambient.r=std::numeric_limits<float>::quiet_NaN();
        SingleViewFrameInput::AuxiliaryView v;v.extent={32,32};v.isolatedScene=scene;i.auxiliaryViews={v};require(!p.prepare(world,i),"NaN preview accepted");
        Scene capture;auto g=geometry();capture.root->addChild(g);auto r=capture.run();require(bool(r),"capture failed");auto sky=std::make_shared<NativeSkySnapshot>(*r.snapshot);sky->draws[0].opacity=scene->ambient.r;
        i.auxiliaryViews.clear();i.nativeSky=sky;require(!p.prepare(world,i),"NaN sky accepted");
    });
    test("authored sphere map captures colour and needs no fabricated UV stream",[]{
        Scene s; auto g=geometry(); g->setName("synthetic-kurst-sphere");
        g->setTexCoordArray(0,nullptr);
        auto st=g->getOrCreateStateSet(); auto t=texture(); t->setName("envMap");
        st->setTextureAttribute(4,t);
        st->addUniform(new osg::Uniform("envMapColor",osg::Vec4(.2f,.3f,.4f,1.f)));
        ImmediateEffectDraw draw; std::string why;
        require(MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"sphere",draw,why,&s.identities),why.c_str());
        require(draw.material.environmentMapMode==EnvironmentMapMode::SphereMap
            && draw.material.environmentMapStrength==1.f
            && draw.material.environmentMapColor==glm::vec4(.2f,.3f,.4f,1.f), "sphere parameters lost");
        require(draw.meshData().texCoordSets.empty(),"generated sphere coordinates consumed a vertex stream");
        st->setTextureAttribute(4,new osg::TexMat(osg::Matrix::translate(1,0,0)));
        require(!MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"sphere",draw,why,&s.identities),
            "unsupported generated-coordinate TexMat silently ignored");
        st->removeTextureAttribute(4,osg::StateAttribute::TEXMAT);
        st->getUniform("envMapColor")->set(osg::Vec4(std::numeric_limits<float>::quiet_NaN(),0,0,1));
        MWRender::v4_effect_detail::CapturedMaterial material;
        require(!MWRender::v4_effect_detail::captureMaterial({},st,s.vfs,material,why,&s.identities),
            "nonfinite environment colour accepted");
    });
    test("persistent geometry detects unmarked edits and preserves live material and transforms",[]{
        if (!std::getenv("OPENMW_V4_PERSISTENT_CAPTURE")) return;
        Scene s; auto g=geometry(); auto* state=g->getOrCreateStateSet();
        state->setTextureAttribute(0,texture());
        ImmediateEffectDraw a,b; std::string why;
        auto capture=[&](ImmediateEffectDraw& draw) {
            require(MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"cached",draw,why,&s.identities),why.c_str());
        };
        capture(a); capture(b);
        require(a.meshSnapshot && a.meshSnapshot==b.meshSnapshot,"unchanged source not shared");
        MWRender::v4_effect_detail::GeometrySnapshotCache exactCache;
        require(!exactCache.find(*g,*state,b),"empty stamp cache hit");
        auto seed=b; seed.mesh=b.meshData(); seed.meshSnapshot.reset();
        exactCache.insert(*g,seed);
        const auto copied=exactCache.stampBytesCopied;
        require(exactCache.find(*g,*state,b),"seeded stamp cache missed");
        if (std::getenv("OPENMW_V4_INCREMENTAL_CAPTURE"))
            require(exactCache.stampBytesCopied==copied,"warm exact comparison copied input arrays");
        state->addUniform(new osg::Uniform("alpha", .25f));
        capture(b);
        require(a.meshSnapshot==b.meshSnapshot && b.material.alpha==.25f, "material controller frozen by geometry cache");
        auto parent=osg::ref_ptr<osg::MatrixTransform>(new osg::MatrixTransform(osg::Matrix::translate(7,0,0)));
        require(MWRender::v4_effect_detail::captureGeometry(*g,{parent.get()},s.vfs,"cached",b,why,&s.identities),
            why.c_str());
        require(a.meshSnapshot==b.meshSnapshot && b.worldTransform[3].x==7, "placement frozen by geometry cache");
        auto* vertices=static_cast<osg::Vec3Array*>(g->getVertexArray());
        (*vertices)[0].x()=2; // deliberately no dirty()
        capture(b);
        require(a.meshSnapshot!=b.meshSnapshot && a.meshData().positions[0].x==0
            && b.meshData().positions[0].x==2,"unmarked vertex edit hidden or old owner mutated");
        a=b;
        auto* uv=static_cast<osg::Vec2Array*>(g->getTexCoordArray(0));
        (*uv)[0].x()=.75f;
        capture(b);
        require(a.meshSnapshot!=b.meshSnapshot && b.meshData().texCoordSets[0][0].x==.75f,"unmarked UV edit hidden");
        a=b;
        state->setTextureAttribute(0,new osg::TexMat(osg::Matrix::translate(.25,0,0)));
        capture(b);
        require(a.meshSnapshot!=b.meshSnapshot && b.meshData().texCoordSets[0][0].x==1,"TexMat frozen");
        auto indices=osg::ref_ptr<osg::DrawElementsUInt>(new osg::DrawElementsUInt(GL_TRIANGLES));
        indices->push_back(0);indices->push_back(1);indices->push_back(2);g->setPrimitiveSet(0,indices);
        capture(a);(*indices)[0]=2;capture(b);
        require(a.meshSnapshot!=b.meshSnapshot && b.meshData().indices[0]==2,"unmarked index edit hidden");
        (*vertices)[0].x()=std::numeric_limits<float>::quiet_NaN();
        require(!MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"cached",b,why,&s.identities),
            "cached mesh hid invalid geometry");
        MWRender::v4_effect_detail::GeometrySnapshotCache tiny(1);
        require(!tiny.find(*g,*state,a),"over-budget entry admitted");
    });
    test("structural state cache preserves unmarked live values and exact override changes",[]{
        MWRender::v4_effect_detail::EffectStateCache cache(4);
        auto parent=osg::ref_ptr<osg::Group>(new osg::Group);
        auto child=osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        auto* ps=parent->getOrCreateStateSet();
        ps->addUniform(new osg::Uniform("alpha",.5f),osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        child->addUniform(new osg::Uniform("alpha",.9f));
        auto a=cache.get({parent.get()},child);
        auto b=cache.get({parent.get()},child);
        require(a==b && cache.hits==1,"unchanged state did not persist");
        ps->getUniform("alpha")->set(.25f);
        b=cache.get({parent.get()},child);
        float alpha=0; b->getUniform("alpha")->get(alpha);
        require(a==b && alpha==.25f,"cached merge froze a live uniform");
        child->addUniform(new osg::Uniform("alpha",.7f),osg::StateAttribute::ON|osg::StateAttribute::PROTECTED);
        b=cache.get({parent.get()},child); b->getUniform("alpha")->get(alpha);
        require(a!=b && alpha==.7f,"protected replacement not propagated");
        a=b; child->removeUniform("alpha");
        b=cache.get({parent.get()},child); b->getUniform("alpha")->get(alpha);
        require(a!=b && alpha==.25f,"removal not propagated");
        auto t=texture(); child->setTextureAttribute(0,t);
        a=cache.get({parent.get()},child);
        t->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
        b=cache.get({parent.get()},child);
        require(a==b && static_cast<const osg::Texture2D*>(b->getTextureAttribute(0,osg::StateAttribute::TEXTURE))
            ->getFilter(osg::Texture::MIN_FILTER)==osg::Texture::NEAREST,"sampler edit frozen");
        child->setMode(GL_BLEND,osg::StateAttribute::ON);
        child->setDefine("FORCE_OPAQUE","1");child->setRenderBinDetails(3,"TraversalOrderBin");
        b=cache.get({parent.get()},child);
        require(a!=b && b->getMode(GL_BLEND)==osg::StateAttribute::ON
            && b->getDefinePair("FORCE_OPAQUE") && b->getBinName()=="TraversalOrderBin","mode/define/bin edit frozen");
        parent->setStateSet(new osg::StateSet);
        b=cache.get({parent.get()},child);
        require(!b->getUniform("alpha"),"replacement parent retained inherited state");
        MWRender::v4_effect_detail::EffectStateCache control(0);
        require(control.get({},child)!=control.get({},child),"zero capacity retained a state");
        MWRender::v4_effect_detail::EffectStateCache lifecycle(2);
        osg::observer_ptr<osg::Texture2D> released;
        {
            auto owned=osg::ref_ptr<osg::StateSet>(new osg::StateSet);auto tex=texture();released=tex;
            owned->setTextureAttribute(0,tex);lifecycle.get({},owned);
        }
        for (int i=0;i<1024;++i) lifecycle.get({},nullptr);
        require(!released.valid(),"expired source retained texture backing after maintenance");
    });
    test("state merge workload observes reuse without freezing material controllers",[]{
        std::vector<osg::ref_ptr<osg::Group>> owners;
        osg::NodePath path;
        for (int i=0;i<8;++i)
        {
            owners.push_back(new osg::Group);path.push_back(owners.back());
            auto* state=owners.back()->getOrCreateStateSet();
            for (int j=0;j<8;++j)
                state->addUniform(new osg::Uniform(("fixture"+std::to_string(i)+":"+std::to_string(j)).c_str(),float(j)));
        }
        auto drawable=osg::ref_ptr<osg::StateSet>(new osg::StateSet);
        drawable->addUniform(new osg::Uniform("alpha",1.f));
        for (const std::size_t capacity : {0u,128u})
        {
            MWRender::v4_effect_detail::EffectStateCache cache(capacity);
            cache.get(path,drawable);
            const auto begin=std::chrono::steady_clock::now();
            for (int i=0;i<2000;++i)
            {
                drawable->getUniform("alpha")->set(float(i));
                const auto state=cache.get(path,drawable);float alpha=0;state->getUniform("alpha")->get(alpha);
                require(alpha==float(i),"warm cache lost live value");
            }
            require(cache.hits==(capacity ? 2000u : 0u),"state cache missed stable structure");
            std::cout<<"STATE merge depth=8 uniforms=65 repeats=2000 cache="<<bool(capacity)<<" ms="
                <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count()<<'\n';
        }
    });
    test("capture scaling observes production path without timing acceptance thresholds",[]{
        Scene s; auto g=geometry();
        static_cast<osg::Vec3Array*>(g->getVertexArray())->resize(12000, osg::Vec3(1,1,1));
        static_cast<osg::Vec2Array*>(g->getTexCoordArray(0))->resize(12000, osg::Vec2(0,0));
        g->setPrimitiveSet(0,new osg::DrawArrays(GL_TRIANGLES,0,12000));
        g->getStateSet()->setTextureAttribute(0,texture());
        ImmediateEffectDraw first; std::string why;
        require(MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"scaling",first,why,&s.identities),why.c_str());
        const auto start=std::chrono::steady_clock::now();
        std::size_t shared=0;
        for (int i=0;i<200;++i)
        {
            ImmediateEffectDraw current;
            require(MWRender::v4_effect_detail::captureGeometry(*g,{},s.vfs,"scaling",current,why,&s.identities),why.c_str());
            shared += current.meshSnapshot && current.meshSnapshot==first.meshSnapshot;
            require(current.meshData().positions.size()==12000, "scaling capture lost vertices");
        }
        require(shared==(std::getenv("OPENMW_V4_PERSISTENT_CAPTURE") ? 200u : 0u), "warm capture did not reuse ownership");
        std::cout<<"CAPTURE scaling vertices=12000 repeats=200 shared="<<shared<<" ms="
            <<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()<<'\n';
    });
    std::cout<<(tests-failures)<<'/'<<tests<<" rendering capture tests passed\n";return failures?1:0;
}
