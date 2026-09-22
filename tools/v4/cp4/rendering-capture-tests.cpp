// Actual OSG state/geometry capture and neutral frame publication. No GPU or game.
#include <apps/openmw/mwrender/v4skycapture.hpp>
#include <apps/openmw/mwrender/v4effectcapture.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <components/rendercore/realizationkeys.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/Switch>
#include <functional>
#include <iostream>
#include <sstream>
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
}
int main()
{
    unsigned tests=0,failures=0;
    auto test=[&](const char* name,auto body){++tests;try{body();std::cout<<"PASS "<<name<<'\n';}
        catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}};
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
    std::cout<<(tests-failures)<<'/'<<tests<<" rendering capture tests passed\n";return failures?1:0;
}
