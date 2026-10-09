#include <components/sceneutil/occlusionculling.hpp>
#include <apps/openmw/mwrender/objectpaging.hpp>
#include <apps/openmw/mwrender/vismask.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/files/collections.hpp>
#include <components/fx/technique.hpp>
#include <components/resource/assetgpubudget.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/assetpaging.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/stateupdater.hpp>
#include <components/misc/constants.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/shader/assetgeometrycontract.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>
#include <osg/NodeCallback>
#include <osg/Program>
#include <osg/ClipControl>
#include <osgViewer/Viewer>
#include <osgViewer/Renderer>
#include <osgUtil/RenderStage>
#include <osgUtil/SceneView>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

// This target compiles ObjectPaging itself with one consistent class definition.
// The provider substitutes collected references only; getChunk quality/cache,
// original SceneManager loading, CopyOp, private admission, strong optimizer,
// real helpers, ICO completion and main-camera selectors are production code.
namespace A = SceneUtil::AssetPaging;
namespace L = SceneUtil::GeneratedStaticLod;
namespace R = Resource;
namespace
{
    void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
    void phase(const char* name,const char* event) { std::cout<<"CHAIN_PHASE name="<<name<<" event="<<event<<'\n'; }
    struct Inputs
    {
        std::filesystem::path package,resources,shaders,settings;
        std::vector<std::string> models;
        std::string ordinaryModel;
        bool retry=false,sourceProbe=false,mixed=false,textureCombination=false,warmControl=false,experimentBudget=false;
        Inputs(int argc,char** argv) : package(std::filesystem::absolute(std::filesystem::u8path(argv[3])))
        {
            for (int i=5;i<argc;++i)
            {
                const std::string option=argv[i];
                if (option=="--retry") {retry=true;continue;}
                if (option=="--source-probe") {sourceProbe=true;continue;}
                if (option=="--mixed") {mixed=true;continue;}
                if (option=="--texture-combination") {textureCombination=true;continue;}
                if (option=="--warm-control") {warmControl=true;continue;}
                if (option=="--experiment-budget") {experimentBudget=true;continue;}
                if (option=="--ordinary-model") { require(++i<argc,"missing actual ordinary STAT model"); ordinaryModel=argv[i]; continue; }
                if (option=="--resources" || option=="--shaders" || option=="--settings")
                {
                    require(++i<argc,"missing explicit package qualification argument");
                    auto path=std::filesystem::absolute(std::filesystem::u8path(argv[i]));
                    if (option=="--resources") resources=std::move(path);
                    else if (option=="--shaders") shaders=std::move(path);
                    else settings=std::move(path);
                }
                else { require(!option.starts_with("--"),"unknown package qualification option"); models.push_back(option); }
            }
            require(!models.empty() && models.size()<=16,"bounded explicit real model population required");
            require(!mixed || (!sourceProbe && !ordinaryModel.empty()),"mixed qualification requires an explicit actual ordinary model");
            require(!resources.empty() && !shaders.empty() && !settings.empty(),"package chain requires --resources --shaders --settings");
            require(std::filesystem::equivalent(resources,package/"resources"),"resource tree must belong to the assembled package");
            require(std::filesystem::is_regular_file(package/"defaults.bin") && std::filesystem::is_regular_file(resources/"version")
                && std::filesystem::is_regular_file(settings),"actual packaged defaults/version and captured settings required");
            require(std::filesystem::is_regular_file(shaders/"compatibility/objects.vert")
                && std::filesystem::is_regular_file(shaders/"lib/core/vertex.glsl"),"complete package shader tree required");
            require(!std::filesystem::equivalent(shaders,resources/"shaders"),"shader mutation requires a private exact package copy");
            require(!std::filesystem::equivalent(shaders/"lib/core/vertex.glsl",resources/"shaders/lib/core/vertex.glsl"),
                "private mutation dependency must not hardlink the packaged source");
        }
    };
    Shader::ShaderManager::DefineMap rendererDefines(SceneUtil::LightManager& lighting,SceneUtil::ShadowManager& shadows)
    {
        auto defines=Shader::getDefaultDefines();
        for (const auto& [name,value]:shadows.getShadowDefines(Settings::shadows())) defines[name]=value;
        for (const auto& [name,value]:lighting.getLightDefines()) defines[name]=value;
        defines["forcePPL"]=Settings::shaders().mForcePerPixelLighting ? "1" : "0";
        defines["clamp"]=Settings::shaders().mClampLighting ? "1" : "0";
        defines["preLightEnv"]=Settings::shaders().mApplyLightingToEnvironmentMaps ? "1" : "0";
        defines["classicFalloff"]=Settings::shaders().mClassicFalloff ? "1" : "0";
        const bool exponentialFog=Settings::fog().mExponentialFog;
        defines["radialFog"]=(exponentialFog || Settings::fog().mRadialFog) ? "1" : "0";
        defines["exponentialFog"]=exponentialFog ? "1" : "0";
        defines["skyBlending"]=Settings::fog().mSkyBlending ? "1" : "0";
        defines["particlePointLighting"]=Settings::shaders().mParticlePointLighting ? "1" : "0";
        const float groundcoverDistance=Settings::groundcover().mRenderingDistance;
        defines["groundcoverFadeStart"]=std::to_string(groundcoverDistance*.9f);
        defines["groundcoverFadeEnd"]=std::to_string(groundcoverDistance);
        defines["groundcoverStompMode"]=std::to_string(Settings::groundcover().mStompMode);
        defines["groundcoverStompIntensity"]=std::to_string(Settings::groundcover().mStompIntensity);
        defines["optimizedmwGroundcoverGpuPath"]=std::to_string(static_cast<int>(Settings::groundcover().mOptimizedMWGpuPath));
        defines["optimizedmwGroundcoverShadowReceive"]=Settings::groundcover().mOptimizedMWShadowReceive ? "1" : "0";
        const int fastWind=Settings::groundcover().mOptimizedMWFastWind;
        defines["optimizedmwGroundcoverFastWind"]=(fastWind>=0 ? fastWind!=0 : static_cast<int>(Settings::groundcover().mOptimizedMWGpuPath)>=2) ? "1" : "0";
        defines["optimizedmwGroundcoverLod"]=Settings::groundcover().mOptimizedMWDensityLod ? "1" : "0";
        defines["reverseZ"]=SceneUtil::AutoDepth::isReversed() ? "1" : "0";
        return defines;
    }
    std::uint64_t consumed(unsigned mask);
    std::uint64_t controlDraws() { return L::Counters::controlDraws.load(); }
    struct Callback final : osg::NodeCallback {};
    struct Registered
    {
        R::ResourceSystem& resources;
        R::BaseResourceManager* manager;
        Registered(R::ResourceSystem& owner,R::BaseResourceManager* value) : resources(owner),manager(value) { resources.addResourceManager(manager); }
        ~Registered() { resources.removeResourceManager(manager); }
    };
    struct Pause
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered=false,released=false;
        std::atomic_uint calls{0};
        void operator()()
        { if (calls.fetch_add(1)!=0) return; std::unique_lock lock(mutex); entered=true; changed.notify_all(); changed.wait(lock,[&]{return released;}); }
        void wait()
        { std::unique_lock lock(mutex); require(changed.wait_for(lock,std::chrono::seconds(30),[&]{return entered;}),"deterministic production checkpoint did not arrive"); }
        void resume()
        { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
    };
    struct Join
    {
        std::thread& thread;
        Pause& pause;
        ~Join() { pause.resume(); if (thread.joinable()) thread.join(); }
    };
    std::uint64_t hostOwners(const R::SpeculativeBudget::Stats& stats)
    { return stats.knownOwnerBytes+stats.estimatedOwnerBytes+stats.pendingOwnerBytes; }
    struct Selectors final : osg::NodeVisitor
    {
        Selectors() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::vector<L::Group*> values;
        void apply(osg::Node& node) override
        { if (auto* selector = dynamic_cast<L::Group*>(&node)) values.push_back(selector); traverse(node); }
    };
    struct Programs final : osg::NodeVisitor
    {
        Programs() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::set<osg::Program*> values;
        void apply(osg::Node& node) override
        { if (auto* state=node.getStateSet()) if (auto* program=dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM))) values.insert(program); traverse(node); }
    };
    struct Features final : osg::NodeVisitor
    {
        Features() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        unsigned geometries=0,normal=0,specular=0,tangent7=0,pbr=0;
        void apply(osg::Geometry& geometry) override
        {
            ++geometries; tangent7+=geometry.getTexCoordArray(7)!=nullptr;
            const osg::Program* program=nullptr;
            std::vector<const osg::StateSet*> path;
            for (const auto* node:getNodePath()) if (const auto* state=node->getStateSet())
            { path.push_back(state); if (auto* value=dynamic_cast<const osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM))) program=value; }
            if (program)
                if (auto proof=Shader::AssetGeometry::capture(*program,path))
                { normal+=proof->define("normalMap")=="1"; specular+=proof->define("specularMap")=="1"; pbr+=proof->family()==Shader::AssetGeometry::Family::Pbr; }
        }
    };
    struct Geometries final : osg::NodeVisitor
    {
        Geometries() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::vector<osg::Geometry*> values;
        void apply(osg::Geometry& geometry) override {values.push_back(&geometry);}
    };
    struct ControlTopology final : osg::NodeVisitor
    {
        ControlTopology() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::uint64_t submissions=0,indices=0,geometries=0;
        void apply(osg::Node& node) override
        {
            if (auto* selector=dynamic_cast<L::Group*>(&node))
            { if (selector->original()) selector->original()->accept(*this); }
            else traverse(node);
        }
        void apply(osg::Geometry& geometry) override
        {
            ++geometries;
            submissions+=geometry.getNumPrimitiveSets();
            for (unsigned i=0;i<geometry.getNumPrimitiveSets();++i) indices+=geometry.getPrimitiveSet(i)->getNumIndices();
        }
    };
    unsigned indexOnlyStreams(const Selectors& selectors)
    {
        unsigned matched=0;
        for (auto* selector:selectors.values)
        {
            Geometries original,derived; selector->original()->accept(original); selector->derived()->accept(derived);
            for (auto* tier:derived.values)
            {
                auto base=std::find_if(original.values.begin(),original.values.end(),[&](const auto* value){return value->getVertexArray()==tier->getVertexArray();});
                require(base!=original.values.end(),"index-only tier must retain its exact optimized ordinary control vertex array");
                require((*base)->getUseVertexBufferObjects() && !(*base)->getUseDisplayList()
                    && tier->getUseVertexBufferObjects() && !tier->getUseDisplayList(),"index-only production control and tier must already use VBOs");
                const auto sameBuffer=[](const osg::Array* left,const osg::Array* right) {
                    return left==right && (!left || (left->getVertexBufferObject()
                        && left->getVertexBufferObject()==right->getVertexBufferObject()));
                };
                require(sameBuffer((*base)->getVertexArray(),tier->getVertexArray())
                    && sameBuffer((*base)->getNormalArray(),tier->getNormalArray())
                    && sameBuffer((*base)->getColorArray(),tier->getColorArray()),"index-only tier must share actual established position/normal/color VBOs");
                require((*base)->getNormalArray()==tier->getNormalArray() && (*base)->getColorArray()==tier->getColorArray()
                    && (*base)->getNumTexCoordArrays()==tier->getNumTexCoordArrays(),"index-only tier must retain normal/color/UV layout");
                for (unsigned unit=0;unit<tier->getNumTexCoordArrays();++unit)
                    require(sameBuffer((*base)->getTexCoordArray(unit),tier->getTexCoordArray(unit)),"index-only tier must share every actual UV and tangent7 VBO");
                if ((*base)->getPrimitiveSet(0)->getNumIndices()==tier->getPrimitiveSet(0)->getNumIndices())
                    require((*base)->getPrimitiveSet(0)==tier->getPrimitiveSet(0),"unchanged leaf must share actual PrimitiveSet/EBO identity");
                require((*base)->getStateSet()==tier->getStateSet(),"index-only tier must retain immutable material/normal/specular state identity");
                ++matched;
            }
        }
        return matched;
    }
    struct GeometrySnapshot
    {
        struct State
        {
            osg::Geometry* geometry;
            osg::Drawable::DrawCallback* callback;
            osg::UserDataContainer* data;
            bool useVbo,displayList;
            std::vector<std::pair<osg::Array*,unsigned>> arrays;
            std::vector<osg::VertexBufferObject*> vbos;
            std::vector<std::pair<osg::PrimitiveSet*,unsigned>> primitives;
            std::uint64_t indexHash=1469598103934665603ull;
        };
        std::vector<State> values;
        explicit GeometrySnapshot(osg::Node& root)
        {
            Geometries geometries; root.accept(geometries);
            for (auto* geometry:geometries.values)
            {
                State state{geometry,geometry->getDrawCallback(),geometry->getUserDataContainer(),
                    geometry->getUseVertexBufferObjects(),geometry->getUseDisplayList()};
                const auto remember=[&](osg::Array* array) {state.arrays.push_back({array,array ? array->getModifiedCount() : 0});state.vbos.push_back(array ? array->getVertexBufferObject() : nullptr);};
                remember(geometry->getVertexArray());remember(geometry->getNormalArray());remember(geometry->getColorArray());
                for (unsigned unit=0;unit<geometry->getNumTexCoordArrays();++unit) remember(geometry->getTexCoordArray(unit));
                for (unsigned i=0;i<geometry->getNumPrimitiveSets();++i)
                {
                    auto* primitive=geometry->getPrimitiveSet(i);state.primitives.push_back({primitive,primitive->getModifiedCount()});
                    for (unsigned n=0;n<primitive->getNumIndices();++n) state.indexHash=(state.indexHash^primitive->index(n))*1099511628211ull;
                }
                values.push_back(std::move(state));
            }
        }
        void unchanged(osg::Node& root,const char* message) const
        {
            const GeometrySnapshot current(root);require(values.size()==current.values.size(),message);
            for (unsigned i=0;i<values.size();++i)
            {
                const auto& a=values[i];const auto& b=current.values[i];
                require(a.geometry==b.geometry && a.callback==b.callback && a.data==b.data
                    && a.useVbo==b.useVbo && a.displayList==b.displayList && a.arrays==b.arrays
                    && a.vbos==b.vbos && a.primitives==b.primitives && a.indexHash==b.indexHash,message);
            }
        }
    };
    struct ProgramState final : osg::NodeVisitor
    {
        ProgramState() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        osg::Node* owner=nullptr;
        osg::ref_ptr<osg::StateSet> state;
        osg::ref_ptr<osg::Program> program;
        void apply(osg::Node& node) override
        {
            if (owner) return;
            if (auto* value=node.getStateSet())
                if (auto* shader=dynamic_cast<osg::Program*>(value->getAttribute(osg::StateAttribute::PROGRAM)))
                { owner=&node; state=value; program=shader; return; }
            traverse(node);
        }
    };
    void parentStateOwnership(Resource::SceneManager& scenes,const osg::Node& original)
    {
        // Real SceneManager/ShaderVisitor output is the geometry/material input.
        // This fixture adds one immutable inherited uniform to make losing the
        // parent ancestry observable; it does not claim natural-game engagement.
        osg::ref_ptr<osg::Node> copy=osg::clone(&original,osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES);
        ProgramState shader;copy->accept(shader);
        require(shader.program && Shader::AssetGeometry::validate(*shader.program)==Shader::AssetGeometry::Refusal::None,
            "parent ancestry proof requires an exact real ShaderVisitor producer");
        osg::ref_ptr<osg::Group> parent=new osg::Group;
        parent->setDataVariance(osg::Object::STATIC);
        auto inherited=new osg::StateSet(*shader.state,osg::CopyOp::SHALLOW_COPY);
        inherited->addUniform(new osg::Uniform("p9InheritedOwnerProof",1.f));
        parent->setStateSet(inherited);parent->addChild(copy);
        L::EffectiveMaterialOwners owners;
        owners.observe(*parent,2);
        require(!owners.report().unknown && !owners.report().nullMaterial && !owners.report().capacity,
            "bounded complete real source ancestry must be known before optimization");
        ControlTopology before;parent->accept(before);
        scenes.shareState(parent);
        SceneUtil::Optimizer optimizer;
        optimizer.optimize(parent,SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS
            |SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES|SceneUtil::Optimizer::MERGE_GEOMETRY);
        scenes.shareState(parent);
        ControlTopology after;parent->accept(after);
        require(before.indices==after.indices && after.indices && parent->getStateSet() && parent->getNumChildren(),
            "ordinary optimization must retain the parent's effective state and exact original index population");
        L::MaterialOwnershipInfo info;
        require(owners.exclusivelyOwns(*parent,2,~0u,&info) && info.reason==L::MaterialOwnershipRefusal::None,
            "complete optimized root with actual inherited shader/material ancestry must retain owner2 proof");
        require(!owners.exclusivelyOwns(*parent->getChild(0),2,~0u,&info)
            && info.reason==L::MaterialOwnershipRefusal::MaterialNotFound,
            "omitting parent state must fail exact material ownership rather than weakening the ancestry proof");
        L::EffectiveMaterialOwners ordinary;ordinary.observe(*parent,1);
        require(!ordinary.exclusivelyOwns(*parent,2,~0u,&info)
            && info.reason==L::MaterialOwnershipRefusal::OrdinaryOwner && info.observedOwners==1,
            "ordinary-only population must not become a certified L1 contributor");
        osg::ref_ptr<osg::Group> unknown=osg::clone(parent.get(),osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES);
        unknown->addChild(new osg::Node);
        require(!owners.exclusivelyOwns(*unknown,2,~0u,&info)
            && info.reason==L::MaterialOwnershipRefusal::ChildUnknown,
            "unknown non-geometry ownership must fail closed without recertifying the immutable known parent");
        require(owners.exclusivelyOwns(*parent,2),"unknown child rejection cannot poison the original approved ownership ledger");
        owners.observe(*parent,1);
        require(!owners.exclusivelyOwns(*parent,2,~0u,&info)
            && info.reason==L::MaterialOwnershipRefusal::MixedOwner && info.observedOwners==3,
            "shared ordinary/occluder material ownership must remain refused after optimization");
        std::cout<<"CHAIN_PARENT_OWNERSHIP actual_shader_visitor=1 ordinary_optimizer=1 inherited_state_retained=1"
            <<" complete_ancestry_owner2=1 omitted_ancestor_refused=1 ordinary_owner_refused=1 mixed_owner_refused=1"
            <<" unknown_owner_refused=1"
            <<" original_indices="<<after.indices<<" control_submissions="<<after.submissions
            <<" runtime_population_substituted=1\n";
    }
    struct FileMutation
    {
        std::filesystem::path path;
        std::string original;
        bool active=false;
        explicit FileMutation(std::filesystem::path value) : path(std::move(value))
        {
            require(std::filesystem::file_size(path)<=1024*1024,"bounded private shader dependency required");
            std::ifstream source(path,std::ios::binary); require(bool(source),"private shader dependency unavailable");
            original.assign(std::istreambuf_iterator<char>(source),std::istreambuf_iterator<char>());
        }
        void publish()
        {
            active=true;
            std::ofstream output(path,std::ios::binary|std::ios::trunc);
            output<<original<<"\n// PRIVATE PACKAGE QUALIFICATION DEPENDENCY MUTATION\n";
            output.close(); require(bool(output),"private dependency mutation could not be written");
            std::filesystem::last_write_time(path,std::filesystem::file_time_type::clock::now()+std::chrono::seconds(1));
        }
        void restore()
        {
            std::ofstream output(path,std::ios::binary|std::ios::trunc); output<<original;
            output.close(); require(bool(output),"private dependency could not be restored");
            active=false;
        }
        ~FileMutation()
        { if (active) {std::ofstream output(path,std::ios::binary|std::ios::trunc); output<<original;} }
    };
    struct Pixels final : osg::Camera::DrawCallback
    {
        mutable std::array<unsigned char,128*128*4> data{};
        mutable GLenum error=GL_NO_ERROR;
        mutable std::uint64_t reads=0;
        mutable std::uint64_t beforeOriginal=0,original=0;
        void operator()(osg::RenderInfo&) const override
        { original=controlDraws()-beforeOriginal; glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,data.data()); error=glGetError(); ++reads; }
    };
    struct MainBegin final : osg::Camera::DrawCallback
    {
        explicit MainBegin(Pixels* value) : pixels(value) {}
        osg::ref_ptr<Pixels> pixels;
        void operator()(osg::RenderInfo&) const override { pixels->beforeOriginal=controlDraws(); }
    };
    struct WarmOrdinaryDraw final : osg::Drawable::DrawCallback
    {
        osg::ref_ptr<osg::Drawable::DrawCallback> delegate;
        osg::Camera* mainCamera=nullptr;
        std::uint64_t* draws=nullptr;
        WarmOrdinaryDraw(osg::Drawable::DrawCallback* original,osg::Camera* camera,std::uint64_t& count)
            : delegate(original),mainCamera(camera),draws(&count) {}
        void drawImplementation(osg::RenderInfo& info,const osg::Drawable* drawable) const override
        {
            if(info.getCurrentCamera()==mainCamera) ++*draws;
            if(delegate) delegate->drawImplementation(info,drawable); else drawable->drawImplementation(info);
        }
    };
    struct BeforeDrawOnce final : osg::Camera::DrawCallback
    {
        explicit BeforeDrawOnce(std::function<void()> value) : operation(std::move(value)) {}
        std::function<void()> operation;
        mutable bool invoked=false;
        void operator()(osg::RenderInfo&) const override
        { if(!invoked) {invoked=true;operation();} }
    };
    unsigned rgb(const std::array<unsigned char,128*128*4>& pixels)
    {
        unsigned result=0;
        for (unsigned i=0;i<pixels.size();i+=4) result+=pixels[i]>4 || pixels[i+1]>4 || pixels[i+2]>4;
        return result;
    }
    struct SecondaryReceipt
    {
        unsigned mask=0;
        std::uint64_t beforeDerived=0,beforeOriginal=0,derived=0,original=0;
        unsigned pixels=0,pixelReads=0;
        GLenum error=GL_NO_ERROR;
        bool drawn=false;
    };
    struct SecondaryDraw final : osg::Camera::DrawCallback
    {
        SecondaryDraw(std::shared_ptr<SecondaryReceipt> value,bool begin) : receipt(std::move(value)),before(begin) {}
        std::shared_ptr<SecondaryReceipt> receipt;
        bool before;
        void operator()(osg::RenderInfo&) const override
        {
            if (before) { receipt->beforeDerived=consumed(receipt->mask); receipt->beforeOriginal=controlDraws(); }
            else
            {
                receipt->derived=consumed(receipt->mask)-receipt->beforeDerived;
                receipt->original=controlDraws()-receipt->beforeOriginal;
                std::array<unsigned char,128*128*4> pixels{};
                glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                receipt->pixels=rgb(pixels); ++receipt->pixelReads; receipt->error=glGetError(); receipt->drawn=true;
            }
        }
    };
    struct ShadowReceipt
    {
        unsigned mask=0;
        std::uint64_t beforeDerived=0,beforeOriginal=0,derived=0,original=0;
        std::uint64_t depthPixels=0,reads=0,drawn=0;
        GLenum error=GL_NO_ERROR;
    };
    struct ShadowDraw final : osg::Camera::DrawCallback
    {
        ShadowDraw(std::shared_ptr<ShadowReceipt> value,bool begin) : receipt(std::move(value)),before(begin) {}
        std::shared_ptr<ShadowReceipt> receipt;
        bool before;
        void operator()(osg::RenderInfo&) const override
        {
            if (before) { receipt->beforeDerived=consumed(receipt->mask); receipt->beforeOriginal=controlDraws(); }
            else
            {
                receipt->derived+=consumed(receipt->mask)-receipt->beforeDerived;
                receipt->original+=controlDraws()-receipt->beforeOriginal;
                GLint viewport[4]{}; glGetIntegerv(GL_VIEWPORT,viewport);
                require(viewport[2]>0 && viewport[3]>0 && viewport[2]<=4096 && viewport[3]<=4096,"bounded real shadow target viewport required");
                std::vector<float> depth(std::size_t(viewport[2])*std::size_t(viewport[3]));
                GLfloat cleared=1; glGetFloatv(GL_DEPTH_CLEAR_VALUE,&cleared);
                GLint alignment=4,rowLength=0; glGetIntegerv(GL_PACK_ALIGNMENT,&alignment); glGetIntegerv(GL_PACK_ROW_LENGTH,&rowLength);
                glPixelStorei(GL_PACK_ALIGNMENT,1); glPixelStorei(GL_PACK_ROW_LENGTH,0);
                glReadPixels(viewport[0],viewport[1],viewport[2],viewport[3],GL_DEPTH_COMPONENT,GL_FLOAT,depth.data());
                glPixelStorei(GL_PACK_ALIGNMENT,alignment); glPixelStorei(GL_PACK_ROW_LENGTH,rowLength);
                receipt->error=glGetError();
                for (float value:depth) receipt->depthPixels+=std::isfinite(value) && std::abs(value-cleared)>1e-6f;
                ++receipt->reads; ++receipt->drawn;
            }
        }
    };
    std::map<osg::Camera*,osg::observer_ptr<osg::Camera>> shadowCameras(osg::Camera& main)
    {
        std::map<osg::Camera*,osg::observer_ptr<osg::Camera>> result;
        auto* renderer=dynamic_cast<osgViewer::Renderer*>(main.getRenderer());
        require(renderer!=nullptr,"production main camera renderer required for actual shadow stages");
        std::set<osgUtil::RenderStage*> seen;
        auto visit=[&](auto&& self,osgUtil::RenderStage* stage)->void
        {
            if (!stage || !seen.insert(stage).second) return;
            require(seen.size()<=64,"bounded actual nested render stages required");
            unsigned cascade=0;
            if (auto* camera=stage->getCamera(); camera && camera->getName()=="ShadowCamera"
                && camera->getUserValue("p8g4CascadeIndex",cascade)) result.emplace(camera,camera);
            for (const auto& [order,child]:stage->getPreRenderList()) self(self,child.get());
            for (const auto& [order,child]:stage->getPostRenderList()) self(self,child.get());
        };
        for (unsigned i=0;i<2;++i) if (auto* view=renderer->getSceneView(i)) visit(visit,view->getRenderStage());
        return result;
    }
    std::uint64_t built(unsigned mask)
    { return (mask&A::Lod ? L::Counters::built.load() : 0) + (mask&A::Hlod ? SceneUtil::HlodCounters::built.load() : 0); }
    std::uint64_t consumed(unsigned mask)
    { return (mask&A::Lod ? L::Counters::derivedDraws.load() : 0) + (mask&A::Hlod ? SceneUtil::HlodCounters::consumedDraws.load() : 0); }
    std::uint64_t ready(unsigned mask)
    { return (mask&A::Lod ? A::Counters::lod.ready.load() : 0) + (mask&A::Hlod ? A::Counters::hlod.ready.load() : 0); }
    void pagingDiagnostics(unsigned mask,const char* phaseName)
    {
        constexpr const char* gates[]={"graph","material","transform","layout","capacity","no_reduction","source","occluder","population"};
        constexpr const char* refusals[]={"none","graph","material","transform","layout","capacity","no_reduction"};
        for (unsigned bit:{A::Lod,A::Hlod}) if (mask&bit)
        {
            const auto& lane=A::lane(bit);
            std::cout<<"CHAIN_PAGING mask="<<bit<<" phase="<<phaseName<<" source_requested="<<lane.sourceRequested.load()
                <<" source_certified="<<lane.sourceCertified.load()<<" source_rejected="<<lane.sourceRejected.load()
                <<" snapshot_denied="<<lane.snapshotDenied.load()<<" outer_cpu_denied="<<lane.outerCpuDenied.load()
                <<" private_cpu_denied="<<lane.privateCpuDenied.load()<<" private_gpu_denied="<<lane.privateGpuDenied.load()
                <<" candidate_admitted="<<lane.candidateAdmitted.load()<<" material_conflict_instances="<<lane.materialConflictInstances.load()
                <<" build_attempts="<<lane.buildAttempts.load()<<" build_refused="<<lane.buildRefused.load()<<" ready="<<lane.ready.load()
                <<" built="<<built(bit)<<" drawn="<<consumed(bit)<<" attempted_mask="<<A::Counters::attemptedMask.load()
                <<" completed_mask="<<A::Counters::completedMask.load()<<" terminal_mask="<<A::Counters::terminalMask.load()
                <<" transient_mask="<<A::Counters::transientMask.load()<<" retry_attempts="<<A::Counters::retryAttempts.load()
                <<" retry_deferred="<<A::Counters::retryDeferred.load()<<" retry_exhausted="<<A::Counters::retryExhausted.load()<<'\n';
            for (unsigned i=0;i<static_cast<unsigned>(A::Gate::Count);++i)
                std::cout<<"CHAIN_PAGING_GATE mask="<<bit<<" name="<<gates[i]<<" count="<<lane.refusals[i].load()<<'\n';
            for (unsigned i=1;i<static_cast<unsigned>(L::Detail::Count);++i)
                if (const auto count=lane.details[i].load()) std::cout<<"CHAIN_PAGING_DETAIL mask="<<bit
                    <<" name="<<L::detailName(static_cast<L::Detail>(i))<<" count="<<count<<'\n';
        }
        if (mask&A::Lod)
            for (unsigned i=0;i<static_cast<unsigned>(L::Refusal::Count);++i)
                std::cout<<"CHAIN_LOD_REFUSAL name="<<refusals[i]<<" count="<<L::Counters::refused[i].load()<<'\n';
    }
    void sampleGpu()
    {
        Debug::V3GpuMemory::sampleIfDue(false,false,7000,7800);
        const auto sample=Debug::V3GpuMemory::admissionSample();
        require(sample.sampledAtMs > 0 && sample.valid && sample.pressure==Debug::V3GpuMemory::PressureState::Comfortable
            && sample.headroomBytes>R::AssetGpuBudget::HeadroomReserve,"actual adapter headroom must qualify; fixture cannot bypass production GPU admission");
    }
    osg::ref_ptr<osg::Node> upgrade(MWRender::ObjectPaging& paging,R::ResourceSystem& resources,
        std::atomic_bool& abort,std::uint64_t stress=0,bool* needsRetry=nullptr)
    {
        R::SpeculativeScope scope(resources.speculativeBudget(),&R::ResourceSystem::speculativeSample,&resources,R::SpeculativePriority::NearFuture);
        std::unique_ptr<R::SpeculativeScope::Stage> extra;
        bool privateCheckpoint=false;
        struct PrivateHookClear
        { MWRender::ObjectPaging& paging; bool enabled; ~PrivateHookClear() {if(enabled) paging.setFixtureBeforePrivate({});} } clear{paging,stress!=0};
        if (stress) paging.setFixtureBeforePrivate([&]{
            require(!privateCheckpoint,"private stress checkpoint must occur once after real source snapshots");
            privateCheckpoint=true;
            if ((R::AssetPolicy::mask()&A::Mask)==A::Lod)
            {
                const auto future=resources.speculativeBudget()->stats().futureBytes;
                constexpr auto ceiling=256*R::SpeculativeBudget::MiB;
                require(future+64*1024<ceiling,"bounded L1 stress must start below the actual configured ceiling");
                extra=std::make_unique<R::SpeculativeScope::Stage>(ceiling-future-64*1024,0);
            }
            else extra=std::make_unique<R::SpeculativeScope::Stage>(stress,0);
        });
        A::OptionalUpgradeScope optional(&abort);
        auto result=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true);
        if (needsRetry) *needsRetry=optional.needsRetry();
        if (stress) require(privateCheckpoint && optional.needsRetry(),"exact control/private admission denial after completed snapshots must reach the production optional owner retry signal");
        return result;
    }
    void qualityContracts()
    {
        A::Quality quality; A::Outcome deferred; deferred.defer(A::Lod); deferred.finish(A::Lod);
        require(!quality.satisfies(A::Lod,10),"unattempted quality cannot impersonate a built tier");
        for (unsigned i=0;i<A::MaximumAttempts;++i)
        {
            quality=quality.after(deferred,i*10000);
            require(quality.satisfies(A::Lod,i*10000+1),"transient failure must defer a busy retry");
            if (i+1<A::MaximumAttempts) require(!quality.satisfies(A::Lod,i*10000+A::RetryDelayMilliseconds),"finite delayed retry must become available");
        }
        require((quality.exhausted&A::Lod) && quality.satisfies(A::Lod,100000),"same source generation must exhaust after exactly three attempts");
        A::Quality original; original.completed=A::Hlod; original.attempted=A::Hlod;
        require((original.after(deferred,1).completed&A::Hlod)!=0,"retry of one lane must conserve the other lane's outcome");
        A::Outcome submitted; submitted.completed=A::Hlod; submitted.readiness=std::make_shared<L::Readiness>(); submitted.finish(A::Hlod);
        const auto pending=A::Quality{}.after(submitted,10);
        require(!(pending.settled()&A::Hlod) && pending.satisfies(A::Hlod,11)
            && !pending.satisfies(A::Hlod,10+A::RetryDelayMilliseconds),"CPU/ICO submission must defer then retry when exact GL readiness never arrives");
        auto expired=pending.withoutOwner();
        require(!expired.completed && expired.attempts[1]==1 && (expired.transient&A::Hlod)
            && expired.satisfies(A::Hlod,11) && !expired.satisfies(A::Hlod,10+A::RetryDelayMilliseconds),
            "eviction of an unready owner must conserve finite attempts and backoff");
        for (unsigned i=1;i<A::MaximumAttempts;++i) expired=expired.after(submitted,i*10000).withoutOwner();
        require((expired.exhausted&A::Hlod) && expired.attempts[1]==A::MaximumAttempts,
            "failed/expired ICO readiness must exhaust after exactly three generation attempts");
        std::cout<<"CHAIN_QUALITY attempts="<<unsigned(quality.attempts[0])<<" exhausted="<<quality.exhausted<<" delay_ms="<<A::RetryDelayMilliseconds<<'\n';
        std::cout<<"CHAIN_ICO pending_settled=0 finite_failed_attempts="<<unsigned(expired.attempts[1])<<" eviction_retains_backoff=1\n";
    }
}

int main(int argc,char** argv) try
{
    std::cout<<std::unitbuf;
    require(argc>=6,"usage: p9-asset-paging-chain mask captured-openmw.cfg packageRoot activePlugin mesh1 [mesh2 ...] --resources package/resources --shaders private-package-shaders --settings captured-settings.cfg [--retry] [--source-probe]");
    const unsigned mask=static_cast<unsigned>(std::stoul(argv[1]));
    require(mask==A::Lod || mask==A::Hlod,"chain consumption fixture isolates L1 or H1; combined runtime acceptance requires user captures");
    const Inputs inputs(argc,argv);
    require(!inputs.warmControl || mask==A::Lod,"warmed exact ordinary-control fixture is L1-only");
    require(!inputs.textureCombination || mask==A::Lod,"incremental combination qualification is L1 plus texture preparation only");
    const auto processMask=mask | (inputs.textureCombination ? R::AssetPolicy::TexturePreparation : 0u);
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK",std::to_string(processMask).c_str());
#else
    setenv("OPENMW_P9_ASSET_MASK",std::to_string(processMask).c_str(),1);
#endif
    const bool retry=inputs.retry;
    Settings::SettingsFileParser settings;
    settings.loadSettingsFile(inputs.package/"defaults.bin",Settings::Manager::mDefaultSettings,true,false);
    Settings::StaticValues::initDefaults();
    settings.loadSettingsFile(inputs.settings,Settings::Manager::mUserSettings,false,false);
    Settings::StaticValues::init();
    // Preserve captured renderer/optimizer settings exactly. Only the bounded
    // reference population, framing and deterministic race checkpoints differ.
    require(Settings::shadows().mEnableShadows && Settings::shadows().mObjectShadows,
        "package qualification must exercise the captured enabled object shadows");
    Files::PathContainer directories; std::vector<std::string> archives,content;
    std::ifstream config(std::filesystem::u8path(argv[2])); require(bool(config),"read-only loadout config unavailable");
    for (std::string line;std::getline(config,line);)
    {
        if (line.starts_with("data="))
        { auto value=line.substr(5); if (value.size()>1 && value.front()=='"' && value.back()=='"') value=value.substr(1,value.size()-2); const auto path=std::filesystem::u8path(value); if (std::filesystem::is_directory(path)) directories.push_back(path); }
        if (line.starts_with("fallback-archive=")) archives.push_back(line.substr(17));
        if (line.starts_with("content=")) content.push_back(line.substr(8));
    }
    require(!directories.empty(),"loadout has no source directories");
    // Engine::setDataDirs prepends packaged resource VFS at lowest priority;
    // captured loose data retain their exact winning order above it.
    require(std::filesystem::is_directory(inputs.resources/"vfs"),"actual packaged resource VFS required");
    directories.insert(directories.begin(),inputs.resources/"vfs");
    phase("load","begin");
    VFS::Manager vfs; VFS::registerArchives(&vfs,Files::Collections(directories),archives,true,nullptr); vfs.buildIndex();
    const std::string plugin=argv[4]; require(std::find(content.begin(),content.end(),plugin)!=content.end(),"static source plugin must be active in the loadout");
    std::map<VFS::Path::Normalized,ESM::RefId> statics;
    std::map<VFS::Path::Normalized,VFS::Path::Normalized> recordModels;
    ESM::ESMReader records; records.open(vfs.get(VFS::Path::Normalized(plugin)),std::filesystem::u8path(plugin));
    unsigned scanned=0;
    while (records.hasMoreRecs())
    {
        require(++scanned<=250000,"bounded static record scan"); const auto name=records.getRecName(); records.getRecHeader();
        if (name.toInt()!=ESM::REC_STAT) {records.skipRecord();continue;}
        ESM::Static value; bool deleted=false; value.load(records,deleted);
        const VFS::Path::Normalized model("meshes/"+value.mModel.getNormalized().value());
        if (deleted) {statics.erase(model);recordModels.erase(model);}
        else
        {
            statics.insert_or_assign(model,value.mId);
            recordModels.insert_or_assign(model,VFS::Path::Normalized(value.mModel.getNormalized().value()));
        }
    }
    phase("load","done");
    R::ResourceSystem resources(&vfs,100,nullptr);
    R::SpeculativeBudget::Config limits; limits.transientLimit=R::AssetPolicy::CpuTransientLimit;
    limits.maximumJobs=4; limits.reservedNearFutureJobs=1;
    if(inputs.experimentBudget)
    {
        limits.experimentLimit=256*R::SpeculativeBudget::MiB;
        limits.experimentScratchLimit=128*R::SpeculativeBudget::MiB;
        limits.experimentMaximumJobs=1;
    }
    resources.enableOpenGlSpeculativeBudget({},limits);
    for (unsigned attempt=0;attempt<30 && !R::SpeculativeBudget::newOwnerWorkUsable(R::ResourceSystem::speculativeSample(&resources),R::SpeculativePriority::NearFuture);++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(R::SpeculativeBudget::newOwnerWorkUsable(R::ResourceSystem::speculativeSample(&resources),R::SpeculativePriority::NearFuture),"actual host memory sample must qualify production speculative budget");
    auto* scenes=resources.getSceneManager(); scenes->setShaderPath(inputs.shaders);
    scenes->setAutoUseNormalMaps(Settings::shaders().mAutoUseObjectNormalMaps);
    scenes->setNormalMapPattern(Settings::shaders().mNormalMapPattern);
    scenes->setNormalHeightMapPattern(Settings::shaders().mNormalHeightMapPattern);
    scenes->setAutoUseSpecularMaps(Settings::shaders().mAutoUseObjectSpecularMaps);
    scenes->setSpecularMapPattern(Settings::shaders().mSpecularMapPattern);
    scenes->setFilterSettings(Settings::general().mTextureMagFilter,Settings::general().mTextureMinFilter,
        Settings::general().mTextureMipmap,static_cast<float>(Settings::general().mAnisotropy));
    scenes->setAdjustCoverageForAlphaTest(Settings::shaders().mAdjustCoverageForAlphaTest);
    // The captured AA=0 path does not request an MSAA intermediate target.
    // Fail instead of silently qualifying a different alpha coverage policy.
    require(Settings::video().mAntialiasing==0,"this package fixture requires its captured non-MSAA renderer policy");
    scenes->setConvertAlphaTestToAlphaToCoverage(false);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->width=128; traits->height=128; traits->pbuffer=true; traits->doubleBuffer=false; traits->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits); require(context.valid(),"real GL context required");
    osgViewer::Viewer viewer; viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    osg::ref_ptr<osg::Camera> retainedCamera=viewer.getCamera(); auto* camera=retainedCamera.get(); camera->setGraphicsContext(context); camera->setViewport(0,0,128,128);
    camera->setName(Constants::SceneCamera);
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    auto cullingMode=osg::Camera::DEFAULT_CULLING | osg::Camera::FAR_PLANE_CULLING;
    if (!Settings::camera().mSmallFeatureCulling) cullingMode&=~osg::CullStack::SMALL_FEATURE_CULLING;
    else { camera->setSmallFeatureCullingPixelSize(Settings::camera().mSmallFeatureCullingPixelSize); cullingMode|=osg::CullStack::SMALL_FEATURE_CULLING; }
    camera->setCullingMode(cullingMode);
    osg::ref_ptr<osg::Group> sceneRoot=new osg::Group;
    osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico=new osgUtil::IncrementalCompileOperation;
    viewer.setIncrementalCompileOperation(ico); scenes->setIncrementalCompileOperation(ico);
    viewer.setSceneData(sceneRoot); phase("context","begin"); viewer.realize();
    require(context->makeCurrent(),"real GL initialization must make its actual context current");
    osg::ref_ptr<SceneUtil::GetGLExtensionsOperation> extensionOperation=new SceneUtil::GetGLExtensionsOperation;
    (*extensionOperation)(context.get());
    osg::ref_ptr<SceneUtil::SelectDepthFormatOperation> depthOperation=new SceneUtil::SelectDepthFormatOperation;
    (*depthOperation)(context.get());
    GLint textureUnits=0; glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&textureUnits);
    require(textureUnits>3,"actual GL sampler capacity must support captured object/shadow roles");
    scenes->getShaderManager().setMaxTextureUnits(textureUnits);
    osg::ref_ptr<SceneUtil::LightManager> lighting=new SceneUtil::LightManager(SceneUtil::LightSettings{
        .mClusteredLighting=Settings::shaders().mClusteredLighting,
        .mMaxLights=Settings::shaders().mMaxLights,
        .mMaximumLightDistance=Settings::shaders().mMaximumLightDistance,
        .mLightFadeStart=Settings::shaders().mLightFadeStart,
        .mLightRadiusMultiplier=Settings::shaders().mLightRadiusMultiplier,
    },&resources);
    scenes->setSupportsClusteredLighting(lighting->isClusteredSupported());
    // Same capability normalization performed by RenderingManager.
    Settings::shaders().mClusteredLighting.set(lighting->getClusteredLighting());
    lighting->setLightingMask(MWRender::Mask_Lighting); lighting->setNodeMask(MWRender::Mask_Scene);
    unsigned shadowMask=MWRender::Mask_Scene;
    if (Settings::shadows().mActorShadows) shadowMask|=MWRender::Mask_Actor;
    if (Settings::shadows().mPlayerShadows) shadowMask|=MWRender::Mask_Player;
    const unsigned indoorMask=shadowMask;
    if (Settings::shadows().mObjectShadows) shadowMask|=MWRender::Mask_Object|MWRender::Mask_Static;
    if (Settings::shadows().mTerrainShadows) shadowMask|=MWRender::Mask_Terrain;
    SceneUtil::ShadowManager shadows(lighting,sceneRoot,shadowMask,indoorMask,
        MWRender::Mask_Terrain|MWRender::Mask_Object|MWRender::Mask_Static,Settings::shadows(),scenes->getShaderManager());
    auto defines=rendererDefines(*lighting,shadows);
    // PostProcessor's effective material globals follow the real captured
    // winning effect files and actual context capabilities. This qualifies
    // object rendering policy, not the full screen effect canvas itself.
    auto* extensions=context->getState()->get<osg::GLExtensions>();
    require(extensions!=nullptr,"actual GL extension table required for captured effect policy");
    if (!extensions->glDisablei && extensions->glDisableIndexedEXT) extensions->glDisablei=extensions->glDisableIndexedEXT;
    const bool normalSupport=extensions->glDisablei!=nullptr;
    const int glslVersion=static_cast<int>(extensions->glslLanguageVersion*100);
    const bool effectUbo=extensions->isUniformBufferObjectSupported && glslVersion>=330;
    unsigned effectRequested=0,effectValid=0,effectMissing=0,effectInvalid=0,effectUnsupported=0;
    bool effectNormals=false,effectLights=false;
    if (Settings::postProcessing().mEnabled)
        for (const auto& name:Settings::postProcessing().mChain.get())
        {
            if (name.empty()) continue;
            require(++effectRequested<=64,"bounded captured effect chain required");
            const auto path=Fx::Technique::makeFileName(name);
            Fx::Technique technique(vfs,*resources.getImageManager(),path,name,128,128,effectUbo,normalSupport);
            technique.compile();
            const bool missing=technique.getStatus()==Fx::Technique::Status::File_Not_exists;
            const bool supported=technique.getGLSLVersion()<=glslVersion;
            const bool valid=technique.isValid() && supported;
            effectMissing+=missing; effectInvalid+=!missing && !technique.isValid(); effectUnsupported+=technique.isValid() && !supported;
            effectValid+=valid; if (valid) { effectNormals|=technique.getNormals(); effectLights|=technique.getLights(); }
            std::cout<<"CHAIN_EFFECT name="<<std::quoted(name)<<" valid="<<valid<<" missing="<<missing
                <<" unsupported="<<(!supported)<<" normals="<<(valid && technique.getNormals())
                <<" winning="<<std::quoted(missing ? std::string() : vfs.getArchive(path))<<'\n';
        }
    scenes->setSupportsNormalsRT(normalSupport);
    lighting->setCollectPPLights(effectLights);
    scenes->getShaderManager().reserveGlobalTextureUnits(Shader::ShaderManager::Slot::OpaqueDepthTexture);
    defines["distorionRTRatio"]=std::to_string(.25f);
    if (normalSupport) defines["disableNormals"]=effectNormals ? "0" : "1";
    scenes->getShaderManager().setGlobalDefines(defines);
    shadows.setupShaders(scenes->getShaderManager());
    shadows.enableOutdoorMode();
    viewer.setLightingMode(osgViewer::View::NO_LIGHT);
    auto* lightingState=lighting->getOrCreateStateSet();
    lightingState->setMode(GL_CULL_FACE,osg::StateAttribute::ON);
    lightingState->setMode(GL_NORMALIZE,osg::StateAttribute::ON);
    osg::ref_ptr<SceneUtil::Material> defaultMaterial=new SceneUtil::Material;
    defaultMaterial->updateStateSet(lightingState); lightingState->setAttribute(defaultMaterial);
    lightingState->addUniform(new osg::Uniform("distortionStrength",0.f));
    lightingState->addUniform(new osg::Uniform("alpha",1.f));
    lightingState->addUniform(new osg::Uniform("actorFade",1.f));
    scenes->setUpNormalsRTForStateSet(lightingState,true);
    // Reserve the same captured sky sampler slot before any object program is
    // requested. Framing/fog/sun are a bounded scene population substitution;
    // the renderer's shader and sampler allocation policy is retained.
    if (Settings::fog().mSkyBlending)
        scenes->getShaderManager().reserveGlobalTextureUnits(Shader::ShaderManager::Slot::SkyTexture);
    const double nearDistance=Settings::camera().mNearClip;
    const double farDistance=Settings::camera().mViewingDistance;
    const double fieldOfView=Settings::camera().mFieldOfView;
    const osg::Matrixd cullProjection=osg::Matrixd::perspective(fieldOfView,1,nearDistance,farDistance);
    const osg::Matrixd renderProjection=SceneUtil::AutoDepth::isReversed()
        ? SceneUtil::getReversedZProjectionMatrixAsPerspective(fieldOfView,1,nearDistance,farDistance)
        : cullProjection;
    // RenderingManager preserves the ordinary OSG camera frustum for culling
    // and shadow near/far calculations; only the actual GPU uniform is reversed.
    camera->setProjectionMatrix(cullProjection);
    osg::ref_ptr<SceneUtil::PerViewUniformStateUpdater> perView=new SceneUtil::PerViewUniformStateUpdater(scenes);
    perView->setProjectionMatrix(osg::Matrixf(renderProjection)); sceneRoot->addCullCallback(perView);
    if (SceneUtil::AutoDepth::isReversed())
    {
        auto* rootState=sceneRoot->getOrCreateStateSet();
        rootState->setAttributeAndModes(new SceneUtil::AutoDepth,osg::StateAttribute::ON);
        rootState->setAttributeAndModes(new osg::ClipControl(osg::ClipControl::LOWER_LEFT,osg::ClipControl::ZERO_TO_ONE),osg::StateAttribute::ON);
    }
    camera->setClearColor({0,0,0,0}); SceneUtil::setCameraClearDepth(camera);
    camera->setClearMask(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT);
    auto* state=camera->getOrCreateStateSet(); state->setAttributeAndModes(new SceneUtil::AutoDepth,osg::StateAttribute::ON);
    state->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf(renderProjection)));
    state->addUniform(new osg::Uniform("screenRes",osg::Vec2(128,128))); state->addUniform(new osg::Uniform("near",float(nearDistance))); state->addUniform(new osg::Uniform("far",float(farDistance)));
    state->addUniform(new osg::Uniform("fogColor",osg::Vec4(0,0,0,1))); state->addUniform(new osg::Uniform("fogEnd",float(farDistance))); state->addUniform(new osg::Uniform("fogStart",float(farDistance-1)));
    osg::ref_ptr<SceneUtil::Light> sun=new SceneUtil::Light; sun->setPosition({-.3f,-.7f,.5f,0}); sun->setDiffuse({.7f,.7f,.7f,1}); sun->setAmbient({.5f,.5f,.5f,1}); sun->setSpecular({.15f,.15f,.15f,1}); lighting->setSunlight(sun);
    osg::ref_ptr<Pixels> pixels=new Pixels; camera->setPreDrawCallback(new MainBegin(pixels)); camera->setFinalDrawCallback(pixels);
    require(!ico->getContextSet().empty(),"actual ICO context registration required before optional chunk work");
    phase("context","done");
    std::cout<<"CHAIN_PACKAGE mask="<<mask<<" packaged_shaders=1 captured_settings=1 define_substitutions=0 stock_certificate_injection=0"
        <<" package_root="<<std::quoted(inputs.package.string())<<" resources="<<std::quoted(inputs.resources.string())
        <<" shaders="<<std::quoted(inputs.shaders.string())<<" settings="<<std::quoted(inputs.settings.string())<<'\n';
    std::cout<<"CHAIN_DEFINES forcePPL="<<defines.at("forcePPL")<<" clamp="<<defines.at("clamp")<<" preLightEnv="<<defines.at("preLightEnv")
        <<" lightingMethodClustered="<<defines.at("lightingMethodClustered")<<" useGPUShader4="<<defines.at("useGPUShader4")
        <<" shadows_enabled="<<defines.at("shadows_enabled")<<" shadow_maps="<<static_cast<int>(Settings::shadows().mNumberOfShadowMaps)
        <<" shadow_resolution="<<static_cast<int>(Settings::shadows().mShadowMapResolution)
        <<" world_batching="<<static_cast<int>(Settings::cells().mV38WorldBatchingMode)<<" batch_optimizer="<<static_cast<int>(Settings::cells().mV39BatchOptimizerMode)
        <<" auto_normal="<<static_cast<bool>(Settings::shaders().mAutoUseObjectNormalMaps)<<" auto_specular="<<static_cast<bool>(Settings::shaders().mAutoUseObjectSpecularMaps)
        <<" occlusion="<<static_cast<bool>(Settings::camera().mOcclusionCulling)<<" occlusion_statics="<<static_cast<bool>(Settings::camera().mOcclusionCullingStatics)
        <<" occluder_min_radius="<<static_cast<float>(Settings::camera().mOcclusionOccluderMinRadius)
        <<" camera_cull_projection_unreversed=1 gpu_projection_reversed="<<SceneUtil::AutoDepth::isReversed()
        <<" source_defaults=0 captured_render_settings=1 viewport_population_substituted=1\n";
    std::cout<<"CHAIN_POSTPROCESS enabled="<<static_cast<bool>(Settings::postProcessing().mEnabled)
        <<" requested="<<effectRequested<<" valid="<<effectValid<<" missing="<<effectMissing<<" invalid="<<effectInvalid
        <<" unsupported="<<effectUnsupported<<" normal_support="<<normalSupport<<" normals="<<effectNormals<<" lights="<<effectLights
        <<" disableNormals="<<defines.at("disableNormals")<<" glsl="<<glslVersion<<" ubo="<<effectUbo
        <<" shader_global_policy_mirrored=1 output_canvas_qualified=0\n";
    std::vector<MWRender::ObjectPaging::FixtureReference> population;
    unsigned sourceNormal=0,sourceSpecular=0,sourceTangent7=0,sourcePbr=0;
    unsigned probeStatics=0,probeLodCertificates=0,probeHlodCertificates=0,probeLodPopulation=0,probeHlodPopulation=0;
    phase("certify","begin");
    for (const auto& requestedModel:inputs.models)
    {
        const auto model=VFS::Path::toNormalized(std::string_view(requestedModel));
        if (inputs.sourceProbe && !statics.contains(model))
        { std::cout<<"CHAIN_SOURCE model="<<model.value()<<" missing_static=1 source_only=1\n"; continue; }
        require(statics.contains(model),"fixture model must have a real live STAT record in the active loadout");
        ++probeStatics;
        auto original=scenes->getTemplate(model,false);
        // OSG has no const accept overload; this visitor only reads layout,
        // material ancestry and the production shader certificate.
        Features features; const_cast<osg::Node&>(*original).accept(features);
        sourceNormal+=features.normal; sourceSpecular+=features.specular; sourceTangent7+=features.tangent7; sourcePbr+=features.pbr;
        L::RefusalInfo l1Info,h1Info;
        const auto l1=L::certify(*original,&l1Info);
        const bool h1Ancestry=SceneUtil::certifySelectiveHlodTemplate(*original,&h1Info);
        const bool h1=h1Ancestry && SceneUtil::qualifySelectiveHlodTemplate(*original,true,&h1Info);
        const bool occluderOwner=Settings::camera().mOcclusionCulling && Settings::camera().mOcclusionCullingStatics
            && original->getBound().radius()>=Settings::camera().mOcclusionOccluderMinRadius;
        probeLodCertificates+=l1==L::Refusal::None; probeHlodCertificates+=h1;
        probeLodPopulation+=l1==L::Refusal::None && !occluderOwner;
        probeHlodPopulation+=h1 && !occluderOwner;
        std::cout<<"CHAIN_SOURCE model="<<model.value()<<" static_id="<<statics.at(model).toString()<<" plugin="<<plugin
            <<" winning="<<vfs.getArchive(model)<<" l1_certificate_ok="<<(l1==L::Refusal::None)<<" l1_refusal="<<static_cast<unsigned>(l1)<<" h1_certificate_ok="<<h1<<" l1_peak_bytes="<<L::cookPeakBytes(*original)
            <<" normal_geometries="<<features.normal<<" specular_geometries="<<features.specular<<" tangent7_geometries="<<features.tangent7<<" pbr_geometries="<<features.pbr
            <<" bound_radius="<<original->getBound().radius()<<" source_occluder_owner="<<occluderOwner
            <<" source_only="<<inputs.sourceProbe<<'\n';
        std::cout<<"CHAIN_SOURCE_DETAIL model="<<model.value()<<" l1_detail="<<L::detailName(l1==L::Refusal::None ? L::Detail::None : l1Info.detail)
            <<" l1_node="<<std::quoted(l1Info.node ? l1Info.node->getName() : std::string())
            <<" l1_program="<<std::quoted(l1Info.program ? l1Info.program->getName() : std::string())
            <<" h1_ancestry="<<h1Ancestry<<" h1_detail="<<L::detailName(h1 ? L::Detail::None : h1Info.detail)
            <<" h1_node="<<std::quoted(h1Info.node ? h1Info.node->getName() : std::string())
            <<" h1_program="<<std::quoted(h1Info.program ? h1Info.program->getName() : std::string())<<'\n';
        if (!inputs.sourceProbe)
            require(mask==A::Lod ? l1==L::Refusal::None : h1,"real SceneManager source must pass the original certificate before CopyOp");
        // The production getModel contract returns STAT's relative model. Its
        // correctMeshPath stage supplies "meshes/"; preserve the actual record
        // field here rather than passing the already resolved VFS key twice.
        population.push_back({nullptr,recordModels.at(model),ESM::REC_STAT,osg::Vec3f(float(population.size())*original->getBound().radius()*3.f,0,0),1.f,false});
    }
    phase("certify","done");
    if (inputs.sourceProbe)
    {
        std::cout<<"CHAIN_PROBE source_only=1 requested="<<inputs.models.size()<<" real_statics="<<probeStatics
            <<" lod_certificates="<<probeLodCertificates<<" hlod_certificates="<<probeHlodCertificates
            <<" lod_source_population="<<probeLodPopulation<<" hlod_source_population="<<probeHlodPopulation
            <<" built=0 drawn=0 cook_substituted=0 runtime_qualification=0\n";
        viewer.setSceneData(nullptr); viewer.setIncrementalCompileOperation(nullptr); scenes->setIncrementalCompileOperation(nullptr);
        viewer.stopThreading(); resources.releaseGLObjects(context->getState()); context->close(); R::AssetGpuBudget::discardAfterContextsClosed();
        return 0;
    }
    require(!population.empty() && (mask!=A::Hlod || population.size()>=2),"bounded real static source family required");
    if(mask==A::Lod) parentStateOwnership(*scenes,*scenes->getTemplate(VFS::Path::toNormalized(std::string_view(inputs.models.front())),false));
    require(!inputs.mixed || mask==A::Lod,"family-cohesive mixed qualification is L1-only; H1 retains spatial ownership policy");
    if (inputs.mixed)
    {
        const auto ordinaryModel=VFS::Path::toNormalized(std::string_view(inputs.ordinaryModel));
        require(statics.contains(ordinaryModel),"mixed ordinary population must have a live STAT in the same actual plugin");
        const auto ordinaryTemplate=scenes->getTemplate(ordinaryModel,false);
        require(ordinaryTemplate.valid(),"actual ordinary SceneManager template required");
        const auto sourcePopulation=population;
        population.clear();
        for (unsigned bucket=0;bucket<2;++bucket)
            for (unsigned i=0;i<sourcePopulation.size();++i)
            {
                auto reference=sourcePopulation[i];
                reference.position={bucket ? 1100.f : -1100.f,float(i)*600.f,0.f};
                population.push_back(reference);
            }
        // This explicit owner is part of the substituted reference population,
        // never a rewritten material/template or a disabled MSOC policy.
        population.push_back({nullptr,recordModels.at(ordinaryModel),ESM::REC_STAT,{0,0,0},1.f,true});
        std::cout<<"CHAIN_MIXED_INPUT candidate_instances="<<population.size()-1<<" spatial_buckets=2 ordinary_instances=1"
            <<" ordinary_model="<<ordinaryModel.value()<<" actual_stat_sources=1 material_substitution=0\n";
    }
    qualityContracts();
    MWRender::ObjectPaging paging(scenes,ESM::Cell::sDefaultWorldspaceId); paging.setAssetMainCamera(camera); paging.setFixturePopulation(population);
    Registered pagingRegistration(resources,&paging);
    std::atomic_bool abort{false};
    const auto beforeBuilt=built(mask);
    osg::ref_ptr<osg::Node> ordinary;
    phase("required_chunk","begin");
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); ordinary=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    phase("required_chunk","done");
    Selectors weak; ordinary->accept(weak); require(weak.values.empty() && built(mask)==beforeBuilt,"required readiness/control must not fabricate optional asset quality");
    osg::ref_ptr<osg::Node> mixedStrongControl;
    ControlTopology mixedBaseline;
    if (inputs.mixed || mask==A::Lod)
    {
        auto originalPopulation=population;
        for (auto& reference:originalPopulation) reference.occluderOwner=true;
        MWRender::ObjectPaging baseline(scenes,ESM::Cell::sDefaultWorldspaceId);
        baseline.setAssetMainCamera(camera); baseline.setFixturePopulation(originalPopulation);
        Registered baselineRegistration(resources,&baseline);
        sampleGpu(); mixedStrongControl=upgrade(baseline,resources,abort);
        Selectors baselineSelectors; mixedStrongControl->accept(baselineSelectors);
        require(baselineSelectors.values.empty() && built(mask)==beforeBuilt,"exact strong optional baseline must keep every original instance");
        mixedStrongControl->accept(mixedBaseline);
        require(mixedBaseline.submissions && mixedBaseline.indices,"real strong control topology must contain actual submissions and indices");
    }
    const GeometrySnapshot weakSnapshot(*ordinary);
    std::vector<std::pair<osg::ref_ptr<const osg::Node>,GeometrySnapshot>> sourceSnapshots;
    std::uint64_t contributorSubmissions=0;
    if (mask==A::Lod)
        for (const auto& reference:population)
        {
            auto source=scenes->getTemplate(VFS::Path::Normalized("meshes/"+reference.model.value()),false);
            sourceSnapshots.emplace_back(source,GeometrySnapshot(*const_cast<osg::Node*>(source.get())));
            ControlTopology input; const_cast<osg::Node*>(source.get())->accept(input); contributorSubmissions+=input.submissions;
        }
    if (retry)
    {
        phase("stressed_upgrade","begin");
        const auto privateDenied=(mask&A::Lod ? A::Counters::lod.privateCpuDenied.load() : A::Counters::hlod.privateCpuDenied.load());
        const auto outerDenied=A::Counters::lod.outerCpuDenied.load();
        const auto sourceCertified=(mask&A::Lod ? A::Counters::lod.sourceCertified.load() : A::Counters::hlod.sourceCertified.load());
        const auto snapshotDenied=(mask&A::Lod ? A::Counters::lod.snapshotDenied.load() : A::Counters::hlod.snapshotDenied.load());
        const auto admitted=(mask&A::Lod ? A::Counters::lod.candidateAdmitted.load() : A::Counters::hlod.candidateAdmitted.load());
        auto deferred=upgrade(paging,resources,abort,128*R::SpeculativeBudget::MiB);
        require(built(mask)==beforeBuilt,"bounded CPU overlap must refuse additional control/private work before cooking");
        require(mask==A::Lod ? A::Counters::lod.outerCpuDenied.load()>outerDenied
            : A::Counters::hlod.privateCpuDenied.load()>privateDenied,"actual production control/private CPU gate must report refusal");
        require((mask&A::Lod ? A::Counters::lod.sourceCertified.load() : A::Counters::hlod.sourceCertified.load())>sourceCertified
            && (mask&A::Lod ? A::Counters::lod.snapshotDenied.load() : A::Counters::hlod.snapshotDenied.load())==snapshotDenied
            && (mask&A::Lod ? A::Counters::lod.candidateAdmitted.load() : A::Counters::hlod.candidateAdmitted.load())==admitted,
            "control/private denial must follow successful actual source snapshots and precede every candidate admission");
        bool cachedRetry=false;
        auto backoff=upgrade(paging,resources,abort,0,&cachedRetry);
        require(backoff==deferred && built(mask)==beforeBuilt && cachedRetry,
            "same-generation cooldown observation must retain ordinary fallback and optional retry without recooking");
        std::this_thread::sleep_for(std::chrono::milliseconds(A::RetryDelayMilliseconds+100));
        std::cout<<"CHAIN_TRANSIENT same_generation=1 private_cpu_denied="<<(mask==A::Hlod)
            <<" control_cpu_denied="<<(mask==A::Lod)<<" fallback_during_delay=1 optional_owner_retry_signal=1 actual_snapshot_completed=1 private_clone_admitted=0 fixed_transient_limit_mib=256\n";
        phase("stressed_upgrade","done");
    }
    sampleGpu(); const auto beforeReady=ready(mask), beforeDraw=consumed(mask);
    const auto ownersBefore=hostOwners(resources.speculativeBudget()->stats());
    const auto beforePrivateCopies=L::Counters::privateArrayCopies.load(),beforePrivateVbos=L::Counters::privateVertexBuffers.load();
    const auto beforePostCandidates=L::Counters::postOptimizedCandidates.load(),beforeNewIndices=L::Counters::newIndexGpuBytes.load();
    const auto beforeGpuDebt=R::AssetGpuBudget::liveBytes.load();
    std::uint64_t warmControls=0,warmOrdinaryDraws=0;
    if(inputs.warmControl)
        paging.setFixtureBeforeLodCook([&](osg::Node& control) {
            phase("ordinary_warm_before_cook","begin");
            const auto beforeCook=built(mask),beforeConsumed=consumed(mask),beforeReads=pixels->reads;
            Selectors noTiers;control.accept(noTiers);require(noTiers.values.empty(),"ordinary warm must precede tier publication");
            Geometries geometry;control.accept(geometry);require(!geometry.values.empty(),"exact post-optimizer ordinary control geometry required");
            const auto view=camera->getViewMatrix();
            struct Restore
            {
                osg::Camera* camera; osg::Group* lighting; osg::Node* control; osg::Matrixd view;
                std::vector<std::pair<osg::ref_ptr<osg::Geometry>,osg::ref_ptr<osg::Drawable::DrawCallback>>> callbacks;
                ~Restore() {for(auto& [geometry,callback]:callbacks) geometry->setDrawCallback(callback);lighting->removeChild(control);camera->setViewMatrix(view);}
            } restore{camera,lighting,&control,view,{}};
            for(auto* leaf:geometry.values)
            {
                restore.callbacks.emplace_back(leaf,leaf->getDrawCallback());
                leaf->setDrawCallback(new WarmOrdinaryDraw(leaf->getDrawCallback(),camera,warmOrdinaryDraws));
            }
            const auto bound=control.getBound();
            // A merged same-material cohort can span separated instances, so
            // its bounding sphere contains substantial empty space. Use the
            // existing close shadow-proof framing for this ordinary warm
            // proof; keep the captured projection and visible RGB threshold.
            const double distance=std::max(2.*double(bound.radius()),double(bound.radius())+1.);
            camera->setViewMatrixAsLookAt(osg::Vec3d(bound.center())+osg::Vec3d(0,-distance,0),osg::Vec3d(bound.center()),{0,0,1});
            lighting->addChild(&control);
            const auto beforeWarmDraws=warmOrdinaryDraws;
            for(unsigned frame=0;frame<3;++frame) viewer.frame();
            std::cout<<"CHAIN_WARM_RENDER ordinary_draws="<<warmOrdinaryDraws-beforeWarmDraws
                <<" pixel_reads="<<pixels->reads-beforeReads<<" rgb="<<rgb(pixels->data)<<" gl_error="<<pixels->error
                <<" bound_radius="<<bound.radius()<<" view_distance="<<distance<<" control_mask="<<control.getNodeMask()
                <<" geometries="<<geometry.values.size()<<" viewer_done="<<viewer.done()<<'\n';
            require(warmOrdinaryDraws>beforeWarmDraws && pixels->reads>beforeReads && rgb(pixels->data)>16 && pixels->error==GL_NO_ERROR,
                "exact ordinary cohort must draw visible RGB with clean GL before optional cooking");
            Programs programs;control.accept(programs);require(!programs.values.empty(),"warmed exact effective ordinary object programs required");
            for(auto* program:programs.values)
            {
                const auto* linked=program->getPCP(*context->getState());
                require(linked && linked->isLinked() && !linked->needsLink(),"ordinary material must finish linking before same-cohort cook");
            }
            require(built(mask)==beforeCook && consumed(mask)==beforeConsumed,"ordinary warming cannot count derived readiness or draws");
            ++warmControls;
            std::cout<<"CHAIN_WARM_CONTROL exact_post_optimizer_owner=1 ordinary_draws="<<warmOrdinaryDraws-beforeWarmDraws
                <<" linked_programs="<<programs.values.size()<<" rgb="<<rgb(pixels->data)
                <<" material_substitution=0 derived_before_cook=0 gl_error=0\n";
            phase("ordinary_warm_before_cook","done");
        });
    phase("cook","begin");
    auto candidate=upgrade(paging,resources,abort); Selectors selected; candidate->accept(selected);
    paging.setFixtureBeforeLodCook({});
    phase("cook","done");
    pagingDiagnostics(mask,"cook");
    require(candidate!=ordinary && !selected.values.empty() && built(mask)>beforeBuilt,"actual getChunk must replace weak cached quality with built optional tiers");
    require(!inputs.warmControl || (warmControls>0 && warmOrdinaryDraws>0),"requested warmed ordinary cohort must actually reach the same production cook");
    if (inputs.mixed || mask==A::Lod)
    {
        ControlTopology retained; candidate->accept(retained);
        require(retained.indices==mixedBaseline.indices && retained.submissions==mixedBaseline.submissions,
            "post-optimizer candidate must preserve exact ordinary control index population and original strong merge submissions");
        require(L::Counters::postOptimizedCandidates>beforePostCandidates
            && L::Counters::privateArrayCopies==beforePrivateCopies && L::Counters::privateVertexBuffers==beforePrivateVbos,
            "L1-only actual producer must qualify post-optimizer controls without private array or vertex buffer copies");
        const auto newIndexBytes=L::Counters::newIndexGpuBytes.load()-beforeNewIndices;
        require(newIndexBytes>0 && R::AssetGpuBudget::liveBytes.load()-beforeGpuDebt==newIndexBytes,
            "L1-only actual GPU debt must equal exclusively new index resources");
        weakSnapshot.unchanged(*ordinary,"L1 producer must not decorate/mutate its still-owned weak ordinary geometry");
        for (const auto& [source,snapshot]:sourceSnapshots)
            snapshot.unchanged(*const_cast<osg::Node*>(source.get()),"L1 producer must not mutate shared source template callbacks, user data, arrays, indices or VBOs");
        if (inputs.mixed) require(contributorSubmissions>mixedBaseline.submissions,
            "actual repeated approved same-material cohort must exercise ordinary draw merging before L1 cooking");
        std::cout<<"CHAIN_POST_OPTIMIZER baseline_submissions="<<mixedBaseline.submissions
            <<" retained_submissions="<<retained.submissions<<" exact_control_indices=1 private_array_copies=0 private_vertex_buffers=0"
            <<" gpu_debt_bytes="<<newIndexBytes<<" new_index_bytes="<<newIndexBytes
            <<" contributor_submissions="<<contributorSubmissions<<" merged_input_population="<<inputs.mixed
            <<" weak_geometry_unchanged=1 source_geometry_unchanged=1 actual_producer=1\n";
        if (inputs.textureCombination) std::cout<<"CHAIN_L1_TEXTURE_COMBINATION process_mask="<<R::AssetPolicy::mask()
            <<" incremental_control_admission=1 post_optimizer=1 private_array_copies=0\n";
        if (inputs.mixed) std::cout<<"CHAIN_MIXED_CONTROL spatial_buckets=2 ordinary_preserved=1 baseline_submissions="<<mixedBaseline.submissions
            <<" retained_submissions="<<retained.submissions<<" baseline_indices="<<mixedBaseline.indices
            <<" retained_indices="<<retained.indices<<" extra_control_submissions=0 post_optimizer=1\n";
    }
    require(ready(mask)==beforeReady,"CPU candidate cannot claim GL completion");
    bool pendingRetry=false;
    const auto pendingBuilt=built(mask);
    auto pendingCache=upgrade(paging,resources,abort,0,&pendingRetry);
    require(pendingCache==candidate && built(mask)==pendingBuilt && pendingRetry,
        "submitted-but-unready ICO cache observation must retain the candidate and optional retry without a new cook");
    require(hostOwners(resources.speculativeBudget()->stats())>ownersBefore,"actual private candidate must retain a host ownership charge after preparation stages unwind");
    if(inputs.experimentBudget)
    {
        const auto stats=resources.speculativeBudget()->stats();
        require(stats.experimentRetainedBytes>0 && stats.experimentSourcePins>0 && stats.experimentSourcePinEstimateBytes>0,
            "actual derived output must retain its experimental charge and borrowed-source metadata pin");
        require(!stats.experimentFutureBytes && !stats.experimentJobs && stats.experimentPeakTotalBytes<=limits.experimentLimit
            && stats.experimentPeakFutureBytes<=limits.experimentScratchLimit && stats.futureBytes<=limits.transientLimit,
            "published derived ownership must remain within intersecting child and common ceilings after scratch release");
        std::cout<<"CHAIN_EXPERIMENT_BUDGET retained="<<stats.experimentRetainedBytes<<" future="<<stats.experimentFutureBytes
            <<" source_pins="<<stats.experimentSourcePins<<" borrowed_estimate="<<stats.experimentSourcePinEstimateBytes
            <<" peak_total="<<stats.experimentPeakTotalBytes<<" peak_scratch="<<stats.experimentPeakFutureBytes
            <<" child_limit="<<limits.experimentLimit<<" scratch_limit="<<limits.experimentScratchLimit
            <<" common_limit="<<limits.transientLimit<<" double_root_charge=0 live_owner_held=1\n";
    }
    // Receipts belong to the production private control and are installed by
    // its builder before immutable MaterialGuard capture. The fixture never
    // changes a geometry callback after cooking.
    lighting->addChild(candidate);
    const auto bound=candidate->getBound(); double error=0; for (auto* selector:selected.values) error=std::max(error,double(selector->error()));
    const double distance=std::max(double(bound.radius())*4.,double(bound.radius())+error*std::abs(camera->getProjectionMatrix()(1,1))*64./.6);
    camera->setViewMatrixAsLookAt(osg::Vec3d(bound.center())+osg::Vec3d(0,-distance,0),osg::Vec3d(bound.center()),{0,0,1});
    // First cull cannot consume a tier whose actual queued GL work has not run.
    // Hold only the queued ICO operation for one actual main draw. This tests
    // pending fallback deterministically without writing a readiness flag or
    // replacing any compile operator/completion callback.
    context->remove(ico.get());
    phase("pending_main_draw","begin");
    viewer.advance(); viewer.eventTraversal(); viewer.updateTraversal(); viewer.renderingTraversals();
    phase("pending_main_draw","done");
    require(consumed(mask)==beforeDraw && pixels->original>0,"pending production ICO tier must render ordinary geometry on its first main view");
    const auto controlPixels=pixels->data;
    context->add(ico.get());
    phase("ico_completion","begin");
    for (unsigned frame=0;frame<90 && ready(mask)==beforeReady;++frame) viewer.frame();
    phase("ico_completion","done");
    require(ready(mask)>beforeReady,"real ObjectPaging CompileCompleted must certify the queued GL objects");
    const auto drawStart=consumed(mask); phase("derived_draw","begin"); viewer.frame(); viewer.frame(); phase("derived_draw","done");
    require(consumed(mask)>drawStart,"actual main camera must consume production-generated geometry");
    if(inputs.warmControl) std::cout<<"CHAIN_WARM_CONSUMPTION controls="<<warmControls<<" ordinary_draws_before_cook="<<warmOrdinaryDraws
        <<" compile_ready=1 actual_derived_draw=1 exact_same_cohort=1\n";
    bool readyRetry=true;
    auto readyCache=upgrade(paging,resources,abort,0,&readyRetry);
    require(readyCache==candidate && !readyRetry && built(mask)==pendingBuilt,
        "ready cached quality must terminate the optional retry signal without rebuilding");
    std::cout<<"CHAIN_RETRY_OBSERVATIONS cooldown_signalled="<<retry
        <<" pending_ico_signalled=1 ready_stops_retry=1 cache_observation_recooks=0\n";
    {
        MWRender::ObjectPaging coalesced(scenes,ESM::Cell::sDefaultWorldspaceId);
        coalesced.setAssetMainCamera(camera); coalesced.setFixturePopulation(population);
        Registered coalescedRegistration(resources,&coalesced);
        osg::ref_ptr<osg::Node> weakOwner;
        { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
            weakOwner=coalesced.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
        Pause inflight; coalesced.setFixtureBeforePublish([&]{inflight();});
        std::exception_ptr error; osg::ref_ptr<osg::Node> strongOwner;
        sampleGpu();
        std::thread worker([&]{try { strongOwner=upgrade(coalesced,resources,abort); } catch (...) {error=std::current_exception();} });
        Join workerJoin{worker,inflight}; inflight.wait();
        const auto observedBuilt=built(mask),observedAttempts=mask==A::Lod
            ? A::Counters::lod.buildAttempts.load() : A::Counters::hlod.buildAttempts.load();
        bool coalescedRetry=false;
        auto observed=upgrade(coalesced,resources,abort,0,&coalescedRetry);
        require(observed==weakOwner && coalescedRetry && built(mask)==observedBuilt
            && (mask==A::Lod ? A::Counters::lod.buildAttempts.load() : A::Counters::hlod.buildAttempts.load())==observedAttempts,
            "coalesced inflight observation must return exact weak owner and retry signal without a new attempt");
        inflight.resume(); worker.join(); coalesced.setFixtureBeforePublish({});
        if (error) std::rethrow_exception(error);
        require(strongOwner!=weakOwner,"inflight peer must publish its actual stronger replacement");
        std::cout<<"CHAIN_RETRY_COALESCED inflight_signalled=1 exact_weak_owner=1 observer_build_attempts=0 peer_published=1\n";
    }
    Programs programs;
    for (auto* selector:selected.values) {selector->original()->accept(programs);selector->derived()->accept(programs);}
    require(!programs.values.empty(),"actual shipped candidate object program required");
    unsigned pbrPrograms=0;
    for (auto* program:programs.values)
    {
        require(Shader::AssetGeometry::matches(*program),"actual assembled asset shader certificate required");
        auto proof=Shader::AssetGeometry::capture(*program); require(bool(proof),"actual compiled asset program recipe required");
        pbrPrograms+=proof->family()==Shader::AssetGeometry::Family::Pbr;
        const auto* linked=program->getPCP(*context->getState()); require(linked && linked->isLinked(),"actual production object shader must link");
    }
    require(pbrPrograms>0 && sourcePbr>0,"assembled promoted PBR object family must actually qualify and link");
    const unsigned sharedStreams=mask==A::Lod ? indexOnlyStreams(selected) : 0;
    require(mask!=A::Lod || sharedStreams>0,"actual L1 tiers must conserve immutable vertex/material streams");
    if (mask==A::Lod) std::cout<<"CHAIN_POST_OPTIMIZER_BUFFERS shared_stream_geometries="<<sharedStreams
        <<" shared_all_arrays_vbos=1 unchanged_primitive_ebo_identity=1 established_control_vbos=1 actual_main_draw=1\n";
    if (mask==A::Lod)
    {
        auto source=selected.values.front()->original();
        osg::ref_ptr<osg::Node> copy=osg::clone(source,osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES);
        Geometries leaves;copy->accept(leaves);require(!leaves.values.empty(),"actual private control clone required for VBO negative");
        for (auto* geometry:leaves.values)
        {
            require(L::isDelegatingDrawCallback(geometry->getDrawCallback()),"VBO negative strips only the known receipt from its own private copy");
            geometry->setDrawCallback(nullptr);geometry->setUserDataContainer(nullptr);
            geometry->setUseVertexBufferObjects(false);geometry->setUseDisplayList(true);
        }
        const auto debt=R::AssetGpuBudget::liveBytes.load();
        R::SpeculativeScope scope(resources.speculativeBudget(),&R::ResourceSystem::speculativeSample,&resources,R::SpeculativePriority::NearFuture);
        SceneUtil::PagingWorkScope optional(&abort,SceneUtil::PagingWorkScope::Phase::OptionalOptimization);
        const auto displayList=L::build(*copy,"actual-post-control|display-list-negative",camera,true);
        require(!displayList.node && displayList.failureStep==L::BuildStep::VertexOwnership && R::AssetGpuBudget::liveBytes==debt,
            "display-list control must refuse index-only GPU accounting before new GPU debt");
        for (auto* geometry:leaves.values) {geometry->setUseVertexBufferObjects(true);geometry->setUseDisplayList(false);}
        auto vertices=osg::clone(leaves.values.front()->getVertexArray(),osg::CopyOp::DEEP_COPY_ALL);
        // Geometry::setVertexArray installs a VBO when its VBO mode is on.
        // Remove it only after attachment, on this privately cloned array.
        leaves.values.front()->setVertexArray(vertices);vertices->setVertexBufferObject(nullptr);
        require(leaves.values.front()->getUseVertexBufferObjects() && !leaves.values.front()->getUseDisplayList()
            && leaves.values.front()->getVertexArray()==vertices && !vertices->getVertexBufferObject(),
            "missing-VBO negative must actually remove the privately copied control array's VBO before calling the production gate");
        const auto missing=L::build(*copy,"actual-post-control|missing-vbo-negative",camera,true);
        std::cout<<"CHAIN_VBO_NEGATIVE_OBSERVED step="<<L::buildStepName(missing.failureStep)
            <<" built="<<bool(missing.node)<<" source_vbo_missing="<<!vertices->getVertexBufferObject()
            <<" new_gpu_debt="<<R::AssetGpuBudget::liveBytes.load()-debt<<'\n';
        require(!missing.node && missing.failureStep==L::BuildStep::VertexOwnership && R::AssetGpuBudget::liveBytes==debt,
            "missing ordinary VBO must refuse index-only debt without mutating source arrays");
        std::cout<<"CHAIN_POST_OPTIMIZER_VBO_REFUSAL display_list_refused=1 missing_vbo_refused=1 new_gpu_debt=0 private_copy_only=1\n";
    }
    require(!sourceNormal || sourceTangent7>0,"actual normal-map family must contain its tangent7 stream");
    require(mask!=A::Hlod || (sourceNormal>0 && sourceTangent7>0),"H1 package qualification requires actual winning normal-map and tangent7 coverage");
    std::cout<<"CHAIN_SHADER_FEATURES pbr_programs="<<pbrPrograms<<" pbr_geometries="<<sourcePbr
        <<" normal_geometries="<<sourceNormal<<" specular_geometries="<<sourceSpecular<<" tangent7_geometries="<<sourceTangent7
        <<" immutable_index_streams="<<sharedStreams<<" actual_program_linked=1 normal_map_coverage="<<(sourceNormal>0 && sourceTangent7>0)
        <<" required_h1_normal_map="<<(mask==A::Hlod)<<'\n';
    unsigned controlRgb=0,tierRgb=0; for (unsigned i=0;i<controlPixels.size();i+=4)
    { controlRgb+=controlPixels[i]>4 || controlPixels[i+1]>4 || controlPixels[i+2]>4; tierRgb+=pixels->data[i]>4 || pixels->data[i+1]>4 || pixels->data[i+2]>4; }
    require(controlRgb>16 && tierRgb>16 && pixels->error==GL_NO_ERROR,"real stock shaded object pixels and clean GL required");
    if (mask==A::Lod) require(L::Counters::derivedIndices<L::Counters::originalIndices,"real L1 main draw must carry fewer indices");
    if (mask==A::Hlod) require(SceneUtil::HlodCounters::proxySubmissions<SceneUtil::HlodCounters::controlSubmissions,"real H1 must reduce strong optimized residual submissions");
    std::cout<<"CHAIN_CONSUMED mask="<<mask<<" ready="<<ready(mask)-beforeReady<<" draws="<<consumed(mask)-drawStart
        <<" control_rgb="<<controlRgb<<" tier_rgb="<<tierRgb<<" original_indices="<<L::Counters::originalIndices
        <<" derived_indices="<<L::Counters::derivedIndices<<" control_submissions="<<SceneUtil::HlodCounters::controlSubmissions
        <<" proxy_submissions="<<SceneUtil::HlodCounters::proxySubmissions<<" gpu_peak_debt="<<R::AssetGpuBudget::peakBytes<<'\n';
    if (inputs.mixed)
        std::cout<<"CHAIN_MIXED_CONSUMED mixed_ordinary_candidate=1 real_multi_bucket_cook=1 actual_main_draw=1 fewer_indices=1 control_batching_preserved=1 gl_error=0\n";
    // Discover only production-created shadow cameras in actual render stages.
    // Their real PRE_RENDER envelopes then prove no optional tier draw
    // occurs inside the shadow pass; no synthetic ShadowTraversalScope is used.
    const osg::Matrixd capturedView=camera->getViewMatrix();
    // A multi-bucket chunk's far proof view is deliberately distant. The
    // captured renderer limits shadows to4096 units, so that view may contain
    // no in-range caster. Frame the same real population more closely for the
    // independent shadow proof while keeping its conservative coarse threshold.
    const double shadowProofDistance=std::max(double(bound.radius())*2.,
        double(bound.radius())+error*std::abs(camera->getProjectionMatrix()(1,1))*64./.6);
    camera->setViewMatrixAsLookAt(osg::Vec3d(bound.center())+osg::Vec3d(0,-shadowProofDistance,0),osg::Vec3d(bound.center()),{0,0,1});
    viewer.frame();viewer.frame();
    std::cout<<"CHAIN_SHADOW_FRAMING original_view_distance="<<distance<<" shadow_view_distance="<<shadowProofDistance
        <<" population_radius="<<bound.radius()<<" maximum_shadow_distance="<<Settings::shadows().mMaximumShadowMapDistance
        <<" population_unchanged=1 projection_unchanged=1 shader_policy_unchanged=1\n";
    const auto actualShadows=shadowCameras(*camera);
    require(!actualShadows.empty(),"enabled production shadow technique must create actual caster stages");
    std::vector<std::shared_ptr<ShadowReceipt>> shadowReceipts;
    for (const auto& [key,observed]:actualShadows)
    {
        osg::ref_ptr<osg::Camera> actual; if (!observed.lock(actual)) continue;
        auto receipt=std::make_shared<ShadowReceipt>(); receipt->mask=mask;
        actual->addPreDrawCallback(new ShadowDraw(receipt,true)); actual->addPostDrawCallback(new ShadowDraw(receipt,false));
        shadowReceipts.push_back(receipt);
    }
    const auto beforeShadowMain=consumed(mask); viewer.frame(); viewer.frame();
    std::uint64_t shadowDerived=0,shadowOriginal=0,shadowDepth=0,shadowReads=0,shadowDrawn=0;
    for (const auto& receipt:shadowReceipts)
    {
        require(!receipt->derived && receipt->error==GL_NO_ERROR,"real shadow pass must preserve original representation and clean GL");
        shadowDerived+=receipt->derived; shadowOriginal+=receipt->original; shadowDepth+=receipt->depthPixels; shadowReads+=receipt->reads; shadowDrawn+=receipt->drawn;
    }
    std::cout<<"CHAIN_SHADOW_DIAGNOSTIC cameras="<<actualShadows.size()<<" hooked="<<shadowReceipts.size()
        <<" derived_draws="<<shadowDerived<<" original_draws="<<shadowOriginal<<" depth_pixels="<<shadowDepth
        <<" pixel_reads="<<shadowReads<<" drawn="<<shadowDrawn<<" main_derived_draws="<<consumed(mask)-beforeShadowMain<<'\n';
    require(shadowOriginal>0 && shadowDepth>0 && shadowReads>0 && shadowDrawn>0 && consumed(mask)>beforeShadowMain,
        "actual original shadow depth and concurrent reduced main-view geometry are both required");
    std::cout<<"CHAIN_SHADOW real_shadow_cull=1 derived_draws="<<shadowDerived<<" original_draws="<<shadowOriginal
        <<" depth_pixels="<<shadowDepth<<" pixel_reads="<<shadowReads<<" drawn="<<shadowDrawn<<" main_derived_draws="<<consumed(mask)-beforeShadowMain<<" gl_error=0\n";
    camera->setViewMatrix(capturedView);viewer.frame();
    // A real nested secondary draw must preserve the main Viewer/camera and
    // service its registered ICO. Isolate consumption within the secondary's
    // pre/post callbacks while the main draw independently continues afterward.
    auto secondaryReceipt=std::make_shared<SecondaryReceipt>(); secondaryReceipt->mask=mask;
    osg::ref_ptr<osg::Camera> secondary=new osg::Camera;
    secondary->setReferenceFrame(osg::Transform::ABSOLUTE_RF); secondary->setRenderOrder(osg::Camera::PRE_RENDER);
    secondary->setProjectionMatrix(camera->getProjectionMatrix()); secondary->setViewMatrix(camera->getViewMatrix());
    secondary->setViewport(0,0,128,128); secondary->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    secondary->setCullingMode(camera->getCullingMode()); secondary->setSmallFeatureCullingPixelSize(camera->getSmallFeatureCullingPixelSize());
    SceneUtil::setCameraClearDepth(secondary);
    secondary->setClearColor({0,0,0,0}); secondary->setClearMask(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    secondary->setPreDrawCallback(new SecondaryDraw(secondaryReceipt,true)); secondary->setPostDrawCallback(new SecondaryDraw(secondaryReceipt,false));
    require(sceneRoot->getNumChildren()==1,"production shadow root must be the single bounded scene owner before secondary view");
    osg::ref_ptr<osg::Node> shadowRoot=sceneRoot->getChild(0);
    secondary->addChild(shadowRoot); sceneRoot->addChild(secondary); viewer.frame();
    std::cout<<"CHAIN_SECONDARY derived_draws="<<secondaryReceipt->derived<<" original_draws="<<secondaryReceipt->original
        <<" rgb="<<secondaryReceipt->pixels<<" pixel_reads="<<secondaryReceipt->pixelReads<<" gl_error="<<secondaryReceipt->error<<" drawn="<<secondaryReceipt->drawn<<'\n';
    require(secondaryReceipt->drawn && !secondaryReceipt->derived && secondaryReceipt->original>0
        && secondaryReceipt->pixels>16 && secondaryReceipt->error==GL_NO_ERROR,"actual secondary draw must consume visible original geometry without a derived draw");
    sceneRoot->removeChild(secondary); secondary->removeChild(shadowRoot);
    candidate->releaseGLObjects(context->getState()); const auto beforeRelease=consumed(mask),beforeReleaseReads=pixels->reads; viewer.frame();
    std::cout<<"CHAIN_RELEASE derived_draws="<<consumed(mask)-beforeRelease<<" original_draws="<<pixels->original
        <<" rgb="<<rgb(pixels->data)<<" pixel_reads="<<pixels->reads-beforeReleaseReads<<" gl_error="<<pixels->error<<'\n';
    require(consumed(mask)==beforeRelease && pixels->original>0 && pixels->reads>beforeReleaseReads && rgb(pixels->data)>16 && pixels->error==GL_NO_ERROR,
        "context release must draw visible original geometry without optional consumption");
    // Immutable original-source controller rejection must survive paging CopyOp.
    auto negativePopulation=population;
    // This is an all-controller negative. With multiple requested material
    // families, changing only the first reference must not forbid an unrelated
    // still-eligible family from cooking.
    for(auto& reference:negativePopulation) if(!reference.occluderOwner)
    {
        auto templateNode=scenes->getTemplate(VFS::Path::Normalized("meshes/"+reference.model.value()),false);
        auto callbackSource=osg::clone(templateNode.get(),osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES);
        callbackSource->setCullCallback(new Callback);reference.original=callbackSource;
    }
    MWRender::ObjectPaging negative(scenes,ESM::Cell::sDefaultWorldspaceId); negative.setAssetMainCamera(camera); negative.setFixturePopulation(negativePopulation);
    Registered negativeRegistration(resources,&negative);
    const auto rejectionBefore=(mask&A::Lod ? A::Counters::lod.sourceRejected.load() : A::Counters::hlod.sourceRejected.load());
    const auto negativeBuilt=built(mask); sampleGpu(); auto refused=upgrade(negative,resources,abort);
    require((mask&A::Lod ? A::Counters::lod.sourceRejected.load() : A::Counters::hlod.sourceRejected.load())>rejectionBefore,"controller source must be refused before CopyOp strips its behavior");
    if (mask==A::Lod) require(built(mask)==negativeBuilt,"controller-only original must retain ordinary graph");
    negativePopulation=population; for (auto& ref:negativePopulation) ref.occluderOwner=true;
    MWRender::ObjectPaging owners(scenes,ESM::Cell::sDefaultWorldspaceId); owners.setAssetMainCamera(camera); owners.setFixturePopulation(negativePopulation);
    Registered ownerRegistration(resources,&owners);
    const auto ownerBuilt=built(mask); const auto ownerBefore=(mask&A::Lod ? A::Counters::lod.refusals[unsigned(A::Gate::Occluder)].load() : A::Counters::hlod.refusals[unsigned(A::Gate::Occluder)].load());
    auto unchanged=upgrade(owners,resources,abort);
    require(built(mask)==ownerBuilt && (mask&A::Lod ? A::Counters::lod.refusals[unsigned(A::Gate::Occluder)].load() : A::Counters::hlod.refusals[unsigned(A::Gate::Occluder)].load())>ownerBefore,"MSOC owners must keep original production topology");
    std::cout<<"CHAIN_NEGATIVE controller_original=1 occluder_owner=1 secondary_original=1 release_original=1\n";
    if (retry)
    {
        MWRender::ObjectPaging waiting(scenes,ESM::Cell::sDefaultWorldspaceId);
        waiting.setAssetMainCamera(camera); waiting.setFixturePopulation(population);
        Registered waitingRegistration(resources,&waiting);
        osg::ref_ptr<osg::Node> waitingControl;
        { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
            waitingControl=waiting.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
        const auto attemptsBefore=mask==A::Lod ? A::Counters::lod.buildAttempts.load() : A::Counters::hlod.buildAttempts.load();
        const auto exhaustedBefore=A::Counters::retryExhausted.load();
        for (unsigned attempt=0;attempt<A::MaximumAttempts;++attempt)
        {
            upgrade(waiting,resources,abort,128*R::SpeculativeBudget::MiB);
            if (attempt+1<A::MaximumAttempts)
                std::this_thread::sleep_for(std::chrono::milliseconds(A::RetryDelayMilliseconds+100));
        }
        bool waitingRetry=false;
        auto waitingFallback=upgrade(waiting,resources,abort,0,&waitingRetry);
        Selectors waitingSelectors; waitingFallback->accept(waitingSelectors);
        require(waitingRetry && waitingFallback==waitingControl && waitingSelectors.values.empty()
            && A::Counters::retryExhausted==exhaustedBefore
            && (mask==A::Lod ? A::Counters::lod.buildAttempts.load() : A::Counters::hlod.buildAttempts.load())==attemptsBefore,
            "three unadmitted construction waits keep exact ordinary fallback and renewed same-generation eligibility");
        std::this_thread::sleep_for(std::chrono::milliseconds(A::RetryDelayMilliseconds+100));
        sampleGpu(); auto renewed=upgrade(waiting,resources,abort);
        Selectors renewedSelectors;renewed->accept(renewedSelectors);
        require(renewed!=waitingControl && !renewedSelectors.values.empty()
            && (mask==A::Lod ? A::Counters::lod.buildAttempts.load() : A::Counters::hlod.buildAttempts.load())>attemptsBefore,
            "actual same-generation production owner must cook after capacity returns without a VFS/shader reset");
        std::cout<<"CHAIN_RETRY_ADMISSION_WAIT observations="<<A::MaximumAttempts
            <<" construction_attempts_before_resume=0 ordinary_preserved=1 renewed_actual_cook=1 generation_reset=0\n";
    }
    if (inputs.mixed)
    {
        auto sharedPopulation=population;
        sharedPopulation.pop_back(); // Replace only the distinct ordinary reference.
        const auto sharedCandidates=sharedPopulation;
        std::set<VFS::Path::Normalized> sharedModels;
        for(const auto& candidate:sharedCandidates) if(sharedModels.insert(candidate.model).second)
        {
            auto ordinaryShared=candidate;ordinaryShared.occluderOwner=true;
            ordinaryShared.position={0,float(sharedModels.size()-1)*600.f,0};sharedPopulation.push_back(ordinaryShared);
        }
        MWRender::ObjectPaging shared(scenes,ESM::Cell::sDefaultWorldspaceId);
        shared.setAssetMainCamera(camera); shared.setFixturePopulation(sharedPopulation);
        Registered sharedRegistration(resources,&shared);
        const auto beforeSharedBuilt=built(mask),beforeSharedRefusal=L::Counters::materialOwnerShared.load();
        const auto beforeSharedAttempts=A::Counters::lod.buildAttempts.load();
        bool terminalRetry=true;
        sampleGpu(); auto sharedFallback=upgrade(shared,resources,abort,0,&terminalRetry);
        Selectors sharedSelectors; sharedFallback->accept(sharedSelectors);
        require(!terminalRetry && sharedSelectors.values.empty() && built(mask)==beforeSharedBuilt && A::Counters::lod.buildAttempts==beforeSharedAttempts
            && L::Counters::materialOwnerShared>beforeSharedRefusal,
            "genuine material family shared with an ordinary owner must still refuse every candidate before cooking");
        for (auto& reference:sharedPopulation) reference.occluderOwner=true;
        MWRender::ObjectPaging sharedBaseline(scenes,ESM::Cell::sDefaultWorldspaceId);
        sharedBaseline.setAssetMainCamera(camera); sharedBaseline.setFixturePopulation(sharedPopulation);
        Registered sharedBaselineRegistration(resources,&sharedBaseline);
        auto exactSharedControl=upgrade(sharedBaseline,resources,abort);
        ControlTopology fallbackTopology,baselineTopology;
        sharedFallback->accept(fallbackTopology); exactSharedControl->accept(baselineTopology);
        require(fallbackTopology.submissions==baselineTopology.submissions
            && fallbackTopology.indices==baselineTopology.indices && fallbackTopology.submissions>0,
            "ordinary shared-family rejection must preserve the exact strong original control submissions and indices");
        std::cout<<"CHAIN_MIXED_SHARED_REFUSAL ordinary_conflict_refused=1 build_attempted=0"
            <<" baseline_submissions="<<baselineTopology.submissions<<" fallback_submissions="<<fallbackTopology.submissions
            <<" exact_indices=1 control_preserved=1 terminal_stops_retry=1\n";
        auto unsupportedPopulation=population;unsupportedPopulation.pop_back();
        const auto supportedCandidates=unsupportedPopulation;
        std::set<VFS::Path::Normalized> unsupportedModels;
        for(const auto& candidate:supportedCandidates) if(unsupportedModels.insert(candidate.model).second)
        {
            auto unsupported=candidate;unsupported.type=ESM::REC_MISC;
            unsupported.position={0,float(unsupportedModels.size()-1)*600.f,0};unsupportedPopulation.push_back(unsupported);
        }
        MWRender::ObjectPaging unsupportedPaging(scenes,ESM::Cell::sDefaultWorldspaceId);
        unsupportedPaging.setAssetMainCamera(camera);unsupportedPaging.setFixturePopulation(unsupportedPopulation);
        Registered unsupportedRegistration(resources,&unsupportedPaging);
        const auto beforeUnsupported=built(mask),beforeUnsupportedAttempts=A::Counters::lod.buildAttempts.load();
        bool unsupportedRetry=true;
        auto unsupportedControl=upgrade(unsupportedPaging,resources,abort,0,&unsupportedRetry);
        Selectors unsupportedSelectors;unsupportedControl->accept(unsupportedSelectors);
        require(unsupportedSelectors.values.empty() && !unsupportedRetry && built(mask)==beforeUnsupported
            && A::Counters::lod.buildAttempts==beforeUnsupportedAttempts,
            "unsupported contributor using the same cached template/material must refuse the entire optimized family before cooking");
        std::uint64_t inputsBeforeMerge=0;
        for (auto& reference:unsupportedPopulation)
        {
            ControlTopology input;auto source=scenes->getTemplate(VFS::Path::Normalized("meshes/"+reference.model.value()),false);
            const_cast<osg::Node*>(source.get())->accept(input);inputsBeforeMerge+=input.submissions;reference.occluderOwner=true;
        }
        MWRender::ObjectPaging unsupportedBaseline(scenes,ESM::Cell::sDefaultWorldspaceId);
        unsupportedBaseline.setAssetMainCamera(camera);unsupportedBaseline.setFixturePopulation(unsupportedPopulation);
        Registered unsupportedBaselineRegistration(resources,&unsupportedBaseline);
        auto baselineOwner=upgrade(unsupportedBaseline,resources,abort);
        ControlTopology unsupportedTopology,unsupportedBaselineTopology;
        unsupportedControl->accept(unsupportedTopology);baselineOwner->accept(unsupportedBaselineTopology);
        require(inputsBeforeMerge>unsupportedTopology.submissions && unsupportedTopology.submissions==unsupportedBaselineTopology.submissions
            && unsupportedTopology.indices==unsupportedBaselineTopology.indices,
            "unsupported same-material actual merged control must preserve exact ordinary batching and indices");
        std::cout<<"CHAIN_MIXED_UNAPPROVED_REFUSAL unsupported_record_kind=1 same_template_same_material=1 whole_merged_draw_refused=1"
            <<" build_attempted=0 exact_control_batching=1 exact_control_indices=1 contributor_submissions="<<inputsBeforeMerge
            <<" baseline_submissions="<<unsupportedBaselineTopology.submissions<<" merged_input_population=1\n";
    }
    if (mask==A::Lod)
    {
        // Exercise the real nonmerge CopyOp branch that returns the template
        // Drawable. Only this negative test overrides the in-memory merge
        // policy; restore it before any subsequent qualification/cohort test.
        struct RestoreMerge
        {
            int batching=Settings::cells().mV38WorldBatchingMode;
            float factor=Settings::terrain().mObjectPagingMergeFactor;
            ~RestoreMerge() {Settings::cells().mV38WorldBatchingMode.set(batching);Settings::terrain().mObjectPagingMergeFactor.set(factor);}
        } restore;
        Settings::cells().mV38WorldBatchingMode.set(0);Settings::terrain().mObjectPagingMergeFactor.set(std::numeric_limits<float>::min());
        const auto source=scenes->getTemplate(VFS::Path::Normalized("meshes/"+population.front().model.value()),false);
        const GeometrySnapshot sourceBefore(*const_cast<osg::Node*>(source.get()));
        MWRender::ObjectPaging borrowed(scenes,ESM::Cell::sDefaultWorldspaceId);borrowed.setAssetMainCamera(camera);
        auto one=population.front();one.position={0,0,0};borrowed.setFixturePopulation({one});
        Registered borrowedRegistration(resources,&borrowed);
        osg::ref_ptr<osg::Node> weakOwner;
        {SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness);weakOwner=borrowed.getChunk(1,{0,0},0,0,false,{0,0,0},true);}
        const GeometrySnapshot weakBefore(*weakOwner);
        const auto beforeBorrowedBuilt=built(mask),beforeBorrowedDebt=R::AssetGpuBudget::liveBytes.load();
        auto borrowedControl=upgrade(borrowed,resources,abort);
        Geometries borrowedLeaves;borrowedControl->accept(borrowedLeaves);
        require(std::any_of(borrowedLeaves.values.begin(),borrowedLeaves.values.end(),[&](const auto* geometry) {
            return std::any_of(sourceBefore.values.begin(),sourceBefore.values.end(),[&](const auto& state) {return geometry==state.geometry;});}),
            "nonmerge negative must exercise the actual borrowed template Drawable");
        Selectors borrowedSelectors;borrowedControl->accept(borrowedSelectors);
        require(borrowedSelectors.values.empty() && built(mask)==beforeBorrowedBuilt && R::AssetGpuBudget::liveBytes==beforeBorrowedDebt,
            "borrowed template Drawable must keep ordinary fallback with no tier/new GPU debt");
        sourceBefore.unchanged(*const_cast<osg::Node*>(source.get()),"nonmerge path must preserve template callback/userdata/indices/VBO identity");
        weakBefore.unchanged(*weakOwner,"nonmerge path must preserve still-owned weak callback/userdata/indices/VBO identity");
        std::cout<<"CHAIN_NONMERGE_SHARED_TEMPLATE actual_borrowed_drawable=1 template_unchanged=1 weak_unchanged=1 no_derived_tier=1 new_gpu_debt=0 negative_only_merge_override=1\n";
    }
    // Clear only the real cache owner, retaining the quality side table exactly
    // as expiry does. Required reconstruction must not inherit completion from
    // the evicted graph; an actual optional upgrade must cook/install again.
    using CacheBase=R::GenericResourceManager<MWRender::ChunkId>;
    phase("eviction","begin");
    paging.CacheBase::clearCache();
    osg::ref_ptr<osg::Node> rebuiltOrdinary;
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); rebuiltOrdinary=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    Selectors evicted; rebuiltOrdinary->accept(evicted); require(evicted.values.empty(),"cache-only eviction must reconstruct ordinary required topology");
    const auto rebuiltBefore=built(mask); sampleGpu(); auto rebuilt=upgrade(paging,resources,abort);
    require(rebuilt!=rebuiltOrdinary && built(mask)>rebuiltBefore,"evicted completion must not settle an ordinary rebuilt graph");
    std::cout<<"CHAIN_EVICTION cache_only=1 required_ordinary=1 optional_rebuilt=1\n";
    phase("eviction","done");

    // Complete this exact rebuilt owner before the independent publication
    // test evicts it. An unready eviction correctly retains its cooldown; it
    // cannot serve as the already-completed setup for a fresh upgrade race.
    phase("publication_ready","begin");
    lighting->removeChild(candidate); lighting->addChild(rebuilt);
    const auto rebuiltReady=ready(mask),rebuiltDraw=consumed(mask);
    for (unsigned frame=0;frame<90 && ready(mask)==rebuiltReady;++frame) viewer.frame();
    viewer.frame();
    Selectors rebuiltSelectors; rebuilt->accept(rebuiltSelectors);
    unsigned preparedSelectors=0;
    for (auto* selector:rebuiltSelectors.values) preparedSelectors+=selector->compilationReady();
    std::cout<<"CHAIN_SETUP_DIAGNOSTIC ready_before="<<rebuiltReady<<" ready_now="<<ready(mask)
        <<" draw_before="<<rebuiltDraw<<" draw_now="<<consumed(mask)<<" selectors="<<rebuiltSelectors.values.size()
        <<" prepared_selectors="<<preparedSelectors<<" ico_contexts="<<ico->getContextSet().size()
        <<" ico_pending="<<ico->getToCompile().size()<<" ico_completed="<<ico->getCompiled().size()<<'\n';
    unsigned icoOperations=0;
    for (const auto& operation:context->getOperationsQueue()) icoOperations+=operation.get()==ico.get();
    Programs rebuiltPrograms; rebuilt->accept(rebuiltPrograms); unsigned linkedPrograms=0,needsLinkPrograms=0;
    for (auto* program:rebuiltPrograms.values)
    { const auto* status=program->getPCP(*context->getState()); linkedPrograms+=status && status->isLinked(); needsLinkPrograms+=!status || status->needsLink(); }
    std::cout<<"CHAIN_CONTEXT_DIAGNOSTIC ico_operations="<<icoOperations<<" context_realized="<<context->isRealized()
        <<" context_cameras="<<context->getCameras().size()<<" main_camera="<<(viewer.getCamera()==camera)
        <<" camera_context="<<(camera->getGraphicsContext()==context)<<" scene_root="<<(viewer.getSceneData()==sceneRoot)
        <<" programs="<<rebuiltPrograms.values.size()<<" linked_programs="<<linkedPrograms<<" programs_need_link="<<needsLinkPrograms<<'\n';
    for (const auto& compiled:ico->getToCompile())
    {
        std::size_t operations=0;
        for (const auto& [owner,list]:compiled->_compileMap) operations+=list._compileOps.size();
        std::cout<<"CHAIN_ICO_DIAGNOSTIC exact_rebuilt="<<(compiled->_subgraphToCompile==rebuilt)
            <<" maps="<<compiled->_compileMap.size()<<" operations="<<operations<<" compiled="<<compiled->compiled()<<'\n';
    }
    require(ready(mask)>rebuiltReady && consumed(mask)>rebuiltDraw,
        "rebuilt publication setup must complete actual ICO and consume its own main tier");
    lighting->removeChild(rebuilt); lighting->addChild(candidate);
    std::cout<<"CHAIN_PUBLICATION_READY ready="<<ready(mask)-rebuiltReady<<" draws="<<consumed(mask)-rebuiltDraw<<'\n';
    phase("publication_ready","done");

    // Force a stale advisory node across a newer publication. getChunk's
    // authoritative node/quality pair must return the newly published owner.
    phase("publication","begin");
    paging.CacheBase::clearCache();
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); rebuiltOrdinary=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    Pause publication; paging.setFixtureBeforeCache([&]{publication();});
    osg::ref_ptr<osg::Node> raced; std::exception_ptr raceError;
    std::thread demand([&]{try {raced=paging.getChunk(1,{0,0},0,0,false,{0,0,0},false);} catch(...) {raceError=std::current_exception();}});
    Join demandJoin{demand,publication};
    publication.wait(); sampleGpu(); auto published=upgrade(paging,resources,abort); publication.resume(); demand.join(); paging.setFixtureBeforeCache({});
    if (raceError) std::rethrow_exception(raceError);
    require(published!=rebuiltOrdinary && raced==published,"old advisory node cannot be returned with newer strong quality metadata");
    std::cout<<"CHAIN_PUBLICATION paired_owner=1 stale_advisory_refused=1\n";
    phase("publication","done");

    // An old-VFS caller pauses before cache invalidation while a real new index
    // generation publishes its required owner. It must cancel without deleting
    // that fresh owner, even though its initial advisory pointer was old.
    phase("epoch","begin");
    Pause epoch; paging.setFixtureBeforeCache([&]{epoch();}); std::atomic_bool staleCancelled{false}; std::exception_ptr staleError;
    std::thread stale([&]{try {paging.getChunk(1,{0,0},0,0,false,{0,0,0},false);} catch(const SceneUtil::PagingWorkCancelled&) {staleCancelled=true;} catch(...) {staleError=std::current_exception();}});
    Join staleJoin{stale,epoch};
    epoch.wait(); vfs.buildIndex();
    osg::ref_ptr<osg::Node> fresh;
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); fresh=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    epoch.resume(); stale.join(); paging.setFixtureBeforeCache({}); if (staleError) std::rethrow_exception(staleError);
    require(staleCancelled && paging.getChunk(1,{0,0},0,0,false,{0,0,0},false)==fresh,"stale source epoch cannot remove the freshly published cache owner");
    std::cout<<"CHAIN_EPOCH cancelled_old=1 fresh_owner_preserved=1\n";
    phase("epoch","done");

    // Hold a fully built, queued private candidate immediately before cache
    // publication. A new source epoch wins with ordinary required topology;
    // the losing candidate's CompileSet still retains CPU storage after its
    // worker/outer stages finish. Its host receipt must survive until ICO drops
    // the exact graph, without a global CPU retain.
    phase("queued_loser","begin");
    for (unsigned step=0;step<30;++step) viewer.frame();
    const auto heldBefore=hostOwners(resources.speculativeBudget()->stats());
    Pause queued; paging.setFixtureBeforePublish([&]{queued();}); std::atomic_bool queuedCancelled{false}; std::exception_ptr queuedError;
    context->remove(ico.get()); sampleGpu();
    std::thread producer([&]{try {upgrade(paging,resources,abort);} catch(const SceneUtil::PagingWorkCancelled&) {queuedCancelled=true;} catch(...) {queuedError=std::current_exception();}});
    Join producerJoin{producer,queued};
    queued.wait(); vfs.buildIndex();
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); fresh=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    queued.resume(); producer.join(); paging.setFixtureBeforePublish({}); if (queuedError) std::rethrow_exception(queuedError);
    const auto held=hostOwners(resources.speculativeBudget()->stats());
    require(queuedCancelled && held>heldBefore && resources.speculativeBudget()->stats().futureBytes==0,
        "losing queued candidate must retain host ownership after worker stages unwind");
    context->add(ico.get()); for (unsigned step=0;step<90 && hostOwners(resources.speculativeBudget()->stats())>=held;++step) viewer.frame();
    const auto drained=hostOwners(resources.speculativeBudget()->stats()); require(drained<held,"actual ICO graph release must retire the uncached host ownership charge");
    std::cout<<"CHAIN_QUEUED_LOSER host_before="<<heldBefore<<" host_held="<<held<<" host_after_ico="<<drained<<" worker_future=0\n";
    phase("queued_loser","done");
    // Return to the exact ready owner that previously produced reduced main
    // draws. Changes below are genuine adversarial material/source mutations,
    // never certificate or readiness injection.
    phase("shader_mutation","begin");
    lighting->removeChild(candidate); lighting->addChild(rebuilt);
    const auto beforeMutationReadyDraw=consumed(mask); viewer.frame(); viewer.frame();
    require(consumed(mask)>beforeMutationReadyDraw && pixels->error==GL_NO_ERROR && rgb(pixels->data)>16,
        "shader negatives require the same published owner to draw its reduced representation first");
    Selectors mutationSelectors; rebuilt->accept(mutationSelectors);
    require(!mutationSelectors.values.empty(),"real reduced owner required for child program mutation");
    struct ChildMaterial
    { osg::Node* owner; osg::ref_ptr<osg::StateSet> state; osg::ref_ptr<osg::Program> program; };
    std::vector<ChildMaterial> changed;
    for (auto* selector:mutationSelectors.values)
    {
        ProgramState child; selector->derived()->accept(child);
        require(child.owner && child.state && child.program,"actual derived child material program required in every selector");
        changed.push_back({child.owner,child.state,child.program});
        auto substituted=osg::clone(child.state.get(),osg::CopyOp::SHALLOW_COPY);
        osg::ref_ptr<osg::Program> childProgram=Shader::ShaderManager::cloneProgram(child.program);
        require(childProgram && childProgram!=child.program,"child mutation requires a different actual program owner");
        substituted->setAttributeAndModes(childProgram,osg::StateAttribute::ON);
        child.owner->setStateSet(substituted);
    }
    const auto beforeChild=consumed(mask),beforeChildReads=pixels->reads;
    viewer.frame();
    const auto childOriginal=pixels->original;
    const auto childRgb=rgb(pixels->data);
    require(consumed(mask)==beforeChild && childOriginal>0 && childRgb>16 && pixels->reads>beforeChildReads && pixels->error==GL_NO_ERROR,
        "first draw after child Program replacement must render the visible original and no derived geometry");
    std::cout<<"CHAIN_CHILD_PROGRAM changed=1 mutated_children="<<changed.size()<<" before_derived_draws="<<beforeChild-beforeMutationReadyDraw
        <<" derived_draws="<<consumed(mask)-beforeChild<<" original_draws="<<childOriginal<<" rgb="<<childRgb
        <<" pixel_reads="<<pixels->reads-beforeChildReads<<" fallback_original=1 gl_error=0\n";
    for (const auto& child:changed) child.owner->setStateSet(child.state);
    const auto recoveryStart=consumed(mask); viewer.frame(); viewer.frame();
    require(consumed(mask)>recoveryStart,"restored exact child material must recover its already-prepared derived representation");
    if(mask==A::Lod)
    {
        // The actual main cull first selects the current prepared tier. Mutate
        // its source or indices only in the camera's pre-draw callback, after
        // capture/selection. Both the temporal recipe check and the real LOD
        // callback must reject that stale tier on this very draw.
        struct RecipeMutation
        {
            osg::ref_ptr<osg::Geometry> source,tier;
            std::vector<const osg::StateSet*> inherited;
            unsigned unit=0;
            osg::ref_ptr<osg::Array> uv;
            osg::ref_ptr<osg::StateSet> material;
            osg::ref_ptr<osg::PrimitiveSet> indices;
        };
        std::vector<RecipeMutation> recipes;
        for(auto* selector:mutationSelectors.values)
        {
            const auto sources=L::originalDraws(*selector->original()),tiers=L::originalDraws(*selector->derived());
            require(!sources.empty() && sources.size()==tiers.size(),"exact real L1 source/tier pairing required for draw recipe regression");
            for(std::size_t i=0;i<sources.size();++i)
            {
                RecipeMutation value; value.source=sources[i].geometry; value.tier=tiers[i].geometry;
                for(const auto& node:tiers[i].path) if(node->getStateSet()) value.inherited.push_back(node->getStateSet());
                while(value.unit<value.source->getNumTexCoordArrays() && !value.source->getTexCoordArray(value.unit)) ++value.unit;
                require(value.unit<value.source->getNumTexCoordArrays(),"actual original UV stream required for draw recipe regression");
                value.uv=value.source->getTexCoordArray(value.unit);value.material=value.source->getStateSet();value.indices=value.tier->getPrimitiveSet(0);
                require(L::temporalDrawRecipeCurrent(*value.tier,value.inherited),"exact real tier recipe must be current at capture");
                recipes.push_back(std::move(value));
            }
        }
        constexpr const char* kinds[]{"source_uv","source_material","derived_indices"};
        for(unsigned kind=0;kind<3;++kind)
        {
            osg::ref_ptr<BeforeDrawOnce> mutation=new BeforeDrawOnce([&]{
                for(auto& value:recipes)
                {
                    if(kind==0) value.source->setTexCoordArray(value.unit,osg::clone(value.uv.get(),osg::CopyOp::DEEP_COPY_ALL));
                    else if(kind==1) value.source->setStateSet(value.material
                        ? new osg::StateSet(*value.material,osg::CopyOp::SHALLOW_COPY) : new osg::StateSet);
                    else value.tier->setPrimitiveSet(0,osg::clone(value.indices.get(),osg::CopyOp::DEEP_COPY_ALL));
                }
                for(const auto& value:recipes)
                    require(!L::temporalDrawRecipeCurrent(*value.tier,value.inherited),"changed post-capture recipe must refuse temporal coverage before its draw");
            });
            const auto beforeRecipeDraw=consumed(mask),beforeRecipeFallback=L::Counters::materialDrawFallbacks.load(),beforeRecipeReads=pixels->reads;
            camera->addPreDrawCallback(mutation);viewer.frame();camera->removePreDrawCallback(mutation);
            require(mutation->invoked && consumed(mask)==beforeRecipeDraw && L::Counters::materialDrawFallbacks>beforeRecipeFallback
                && pixels->original>0 && pixels->reads>beforeRecipeReads && rgb(pixels->data)>16 && pixels->error==GL_NO_ERROR,
                "post-cull recipe mutation must replay visible original geometry and submit no stale derived indices");
            std::cout<<"CHAIN_TEMPORAL_RECIPE_MUTATION kind="<<kinds[kind]<<" captured_current=1 changed_after_cull=1 temporal_recipe_refused=1"
                <<" derived_draws="<<consumed(mask)-beforeRecipeDraw<<" original_draws="<<pixels->original
                <<" callback_fallbacks="<<L::Counters::materialDrawFallbacks-beforeRecipeFallback<<" rgb="<<rgb(pixels->data)<<" gl_error=0\n";
            for(auto& value:recipes)
            {value.source->setTexCoordArray(value.unit,value.uv);value.source->setStateSet(value.material);value.tier->setPrimitiveSet(0,value.indices);}
            for(const auto& value:recipes) require(L::temporalDrawRecipeCurrent(*value.tier,value.inherited),"restoring exact recipe must recover its immutable guard");
            const auto beforeRecipeRestore=consumed(mask);viewer.frame();
            require(consumed(mask)>beforeRecipeRestore && pixels->error==GL_NO_ERROR,"restored exact recipe must recover actual reduced main draws");
        }
    }
    {
        // A ShaderManager refresh updates the cached template's exact Program
        // recipe, while every previously captured derived guard must remain
        // stale. Paging must bind its own quality to that shader generation.
        auto& manager=scenes->getShaderManager();
        auto originalDefines=manager.getGlobalDefines();
        MWRender::ObjectPaging generations(scenes,ESM::Cell::sDefaultWorldspaceId);
        generations.setAssetMainCamera(camera); generations.setFixturePopulation(population);
        Registered generationRegistration(resources,&generations);
        sampleGpu(); auto initial=upgrade(generations,resources,abort);
        Selectors initialSelectors; initial->accept(initialSelectors);
        require(!initialSelectors.values.empty(),"shader generation regression requires actual initially cooked selectors");
        lighting->removeChild(rebuilt); lighting->addChild(initial);
        const auto allReady=[](const Selectors& selectors) {
            return std::all_of(selectors.values.begin(),selectors.values.end(),[](const auto* value){return value->compilationReady();});
        };
        for (unsigned frame=0;frame<90 && !allReady(initialSelectors);++frame) viewer.frame();
        const auto initialDraw=consumed(mask); viewer.frame(); viewer.frame();
        require(allReady(initialSelectors) && consumed(mask)>initialDraw,"initial cached shader cohort must actually complete ICO and draw");
        Programs initialPrograms; initialSelectors.values.front()->original()->accept(initialPrograms);
        require(!initialPrograms.values.empty(),"actual source Program required for shader generation snapshot");
        auto oldSnapshot=Shader::AssetGeometry::capture(**initialPrograms.values.begin());
        require(bool(oldSnapshot),"current exact Program recipe must be capturable before globals change");
        const auto originalGeneration=manager.assetGeneration(),unchangedBuilt=built(mask);
        manager.setGlobalDefines(originalDefines);
        bool unchangedRetry=true;
        auto unchanged=upgrade(generations,resources,abort,0,&unchangedRetry);
        const auto unchangedDraw=consumed(mask); viewer.frame();
        require(manager.assetGeneration()==originalGeneration && unchanged==initial && !unchangedRetry
            && built(mask)==unchangedBuilt && consumed(mask)>unchangedDraw,
            "identical global defines must preserve epoch, prepared guard and cached owner without recooking");
        Pause shaderEpoch; generations.setFixtureBeforeCache([&]{shaderEpoch();});
        std::atomic_bool oldShaderCancelled{false}; std::exception_ptr oldShaderError;
        std::thread oldShader([&]{try {generations.getChunk(1,{0,0},0,0,false,{0,0,0},false);}
            catch(const SceneUtil::PagingWorkCancelled&) {oldShaderCancelled=true;}
            catch(...) {oldShaderError=std::current_exception();} });
        Join oldShaderJoin{oldShader,shaderEpoch}; shaderEpoch.wait();
        auto changedDefines=originalDefines;
        changedDefines["clamp"]=originalDefines.at("clamp")=="1" ? "0" : "1";
        manager.setGlobalDefines(changedDefines);
        require(manager.assetGeneration()==originalGeneration+1
            && Shader::AssetGeometry::current(*oldSnapshot,**initialPrograms.values.begin())==Shader::AssetGeometry::Refusal::Epoch,
            "reviewed globals must advance exact shader generation and reject the old immutable proof");
        for (auto* program:initialPrograms.values)
            require(Shader::AssetGeometry::matches(*program),"the actual cached source Program must acquire a fresh reviewed recipe");
        osg::ref_ptr<osg::Node> newWeak;
        { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
            newWeak=generations.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
        shaderEpoch.resume(); oldShader.join(); generations.setFixtureBeforeCache({});
        if (oldShaderError) std::rethrow_exception(oldShaderError);
        require(oldShaderCancelled && generations.getChunk(1,{0,0},0,0,false,{0,0,0},false)==newWeak,
            "stale shader cohort caller must cancel without removing the newly published weak owner");
        const auto oldDraw=consumed(mask); viewer.frame();
        require(consumed(mask)==oldDraw && pixels->original>0 && rgb(pixels->data)>16 && pixels->error==GL_NO_ERROR,
            "old prepared guard must draw the visible original after approved shader regeneration");
        const auto freshBuilt=built(mask); sampleGpu(); auto fresh=upgrade(generations,resources,abort);
        Selectors freshSelectors; fresh->accept(freshSelectors);
        require(fresh!=initial && fresh!=newWeak && !freshSelectors.values.empty() && built(mask)>freshBuilt,
            "new shader cohort must cook a fresh exact representation instead of settling the old cached guard");
        lighting->removeChild(initial); lighting->addChild(fresh);
        for (unsigned frame=0;frame<90 && !allReady(freshSelectors);++frame) viewer.frame();
        const auto freshDraw=consumed(mask); viewer.frame(); viewer.frame();
        require(allReady(freshSelectors) && consumed(mask)>freshDraw && pixels->error==GL_NO_ERROR && rgb(pixels->data)>16,
            "fresh reviewed shader cohort must complete actual ICO and consume its new reduced representation");
        manager.setGlobalDefines(originalDefines);
        sampleGpu(); auto restored=upgrade(generations,resources,abort);
        Selectors restoredSelectors; restored->accept(restoredSelectors);
        require(restored!=fresh && !restoredSelectors.values.empty(),"restoring exact globals requires a fresh guard, never blessing an old snapshot");
        lighting->removeChild(fresh); lighting->addChild(restored);
        for (unsigned frame=0;frame<90 && !allReady(restoredSelectors);++frame) viewer.frame();
        const auto restoredDraw=consumed(mask); viewer.frame(); viewer.frame();
        require(allReady(restoredSelectors) && consumed(mask)>restoredDraw && pixels->error==GL_NO_ERROR,
            "restored exact shader policy must recover through fresh cooking and readiness");
        require(Shader::AssetGeometry::current(*oldSnapshot,**initialPrograms.values.begin())==Shader::AssetGeometry::Refusal::Epoch,
            "restoring reviewed source must not revalidate the pre-change immutable snapshot");
        rebuilt=restored; // Subsequent dependency mutation tests this ready owner.
        std::cout<<"CHAIN_SHADER_COHORT unchanged_no_generation=1 unchanged_no_recook=1 old_guard_original=1"
            <<" stale_caller_cancelled=1 fresh_owner_preserved=1 source_recertified=1 new_cohort_recooked=1"
            <<" fresh_actual_draw=1 restored_actual_draw=1 old_snapshot_still_epoch=1 gl_error=0\n";
    }
    Programs mutationPrograms; rebuilt->accept(mutationPrograms);
    std::map<osg::Shader*,std::string> shaderSources;
    for (auto* program:mutationPrograms.values)
        for (unsigned i=0;i<program->getNumShaders();++i)
            shaderSources.emplace(program->getShader(i),program->getShader(i)->getShaderSource());
    require(!shaderSources.empty(),"real dependency mutation requires the actual compiled program sources");
    FileMutation dependency(inputs.shaders/"lib/core/vertex.glsl");
    dependency.publish();
    auto& shaderManager=scenes->getShaderManager();
    shaderManager.triggerShaderReload();
    const bool invalidated=Shader::AssetGeometry::validate(*changed.front().program)!=Shader::AssetGeometry::Refusal::None;
    shaderManager.update(viewer);
    unsigned sourceChanges=0;
    for (const auto& [shader,source]:shaderSources) sourceChanges+=shader->getShaderSource()!=source;
    require(invalidated && sourceChanges>0,"real reload must invalidate the old recipe and consume the changed dependency source");
    const auto beforeSource=consumed(mask),beforeSourceReads=pixels->reads;
    viewer.frame();
    const auto sourceOriginal=pixels->original;
    const auto sourceRgb=rgb(pixels->data);
    require(consumed(mask)==beforeSource && sourceOriginal>0 && sourceRgb>16 && pixels->reads>beforeSourceReads && pixels->error==GL_NO_ERROR,
        "first draw after dependency hot reload must render the visible original without derived consumption");
    std::cout<<"CHAIN_SHADER_MUTATION published=1 fallback_original=1 derived_draws="<<consumed(mask)-beforeSource
        <<" original_draws="<<sourceOriginal<<" rgb="<<sourceRgb<<" pixel_reads="<<pixels->reads-beforeSourceReads
        <<" before_derived_draws="<<beforeSource-recoveryStart<<" source_changed="<<sourceChanges
        <<" trigger_invalidated="<<invalidated<<" actual_reload=1 gl_error=0\n";
    dependency.restore();
    phase("shader_mutation","done");
    viewer.setSceneData(nullptr); viewer.setIncrementalCompileOperation(nullptr); scenes->setIncrementalCompileOperation(nullptr);
    viewer.stopThreading(); resources.releaseGLObjects(context->getState()); context->close(); R::AssetGpuBudget::discardAfterContextsClosed();
    require(resources.speculativeBudget()->stats().futureBytes==0,"production preparation stages must release bounded CPU overlap");
    if(inputs.experimentBudget)
    {
        const auto stats=resources.speculativeBudget()->stats();
        require(!stats.experimentFutureBytes && !stats.experimentJobs
            && stats.experimentPeakTotalBytes<=limits.experimentLimit && stats.experimentPeakFutureBytes<=limits.experimentScratchLimit,
            "full actual replay/invalidation fixture must not leak experimental scratch or exceed either child ceiling");
        std::cout<<"CHAIN_EXPERIMENT_SETTLED retained="<<stats.experimentRetainedBytes<<" future="<<stats.experimentFutureBytes
            <<" jobs="<<stats.experimentJobs<<" source_pins="<<stats.experimentSourcePins
            <<" retained_owners_still_referenced=1 scratch_released=1\n";
    }
    std::cout<<"CHAIN_PASS source_population_substituted=1 source_loader_substituted=0 cook_substituted=0 readiness_substituted=0 cpu_limit="<<R::AssetPolicy::CpuTransientLimit<<'\n';
    return 0;
}
catch (const std::exception& error) { std::cerr<<"CHAIN_FAILURE "<<error.what()<<'\n'; return 1; }
