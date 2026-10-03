#include <components/files/collections.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/nif/niffile.hpp>
#include <components/nifosg/nifloader.hpp>
#include <components/resource/bgsmfilemanager.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/optimizer.hpp>
#include <components/sceneutil/selectivehlod.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texmat.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/shader/shadervisitor.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <osg/Geometry>
#include <osg/MatrixTransform>
#include <osg/Texture2D>
#include <osgViewer/Viewer>
#include <osgDB/SharedStateManager>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <set>

namespace
{
    void require(bool condition,const char* reason) {if(!condition) throw std::runtime_error(reason);}
    constexpr auto PrivateCopyFlags=osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES
        |osg::CopyOp::DEEP_COPY_ARRAYS|osg::CopyOp::DEEP_COPY_PRIMITIVES;
    osg::ref_ptr<osg::Node> privateClone(const osg::Node& node) {return osg::clone(&node,osg::CopyOp(PrivateCopyFlags));}
    struct PagingCopy final:osg::CopyOp
    {
        PagingCopy():osg::CopyOp(PrivateCopyFlags){}
        osg::Node* operator()(const osg::Node* node) const override
        {
            if(!node->getNodeMask()) return nullptr;
            auto* cloned=static_cast<osg::Node*>(node->clone(*this));
            cloned->setName("");cloned->setUserDataContainer(nullptr);cloned->setDataVariance(osg::Object::STATIC);return cloned;
        }
        osg::Drawable* operator()(const osg::Drawable* drawable) const override
        {
            if(!drawable->getNodeMask()) return nullptr;
            auto* cloned=static_cast<osg::Drawable*>(drawable->clone(*this));
            cloned->setName("");cloned->setUserDataContainer(nullptr);cloned->setDataVariance(osg::Object::STATIC);return cloned;
        }
    };
    struct Draws final:osg::Drawable::DrawCallback
    {
        std::shared_ptr<unsigned> count;
        osg::ref_ptr<osg::Drawable::DrawCallback> delegate;
        void drawImplementation(osg::RenderInfo& info,const osg::Drawable* drawable) const override
        {++*count;if(delegate) delegate->drawImplementation(info,drawable);else drawable->drawImplementation(info);}
    };
    struct Instrument final:osg::NodeVisitor
    {
        std::shared_ptr<unsigned> count=std::make_shared<unsigned>(0);
        Instrument():osg::NodeVisitor(TRAVERSE_ALL_CHILDREN){}
        void apply(osg::Geometry& geometry) override
        {
            // Corpus control has no callbacks. Proxy receipt stays delegated
            // through the production callback, so instrumentation cannot invent
            // consumption by merely counting a replacement traversal.
            osg::ref_ptr<Draws> draws=new Draws;draws->count=count;draws->delegate=geometry.getDrawCallback();
            geometry.setDrawCallback(draws);
        }
    };
    struct Samplers final:osg::NodeVisitor
    {
        Samplers():osg::NodeVisitor(TRAVERSE_ALL_CHILDREN){}
        void apply(osg::Node& node) override
        {
            if(auto* state=node.getStateSet())
                for(auto& unit:state->getTextureAttributeList())
                    for(auto& [type,pair]:unit) if(auto* texture=pair.first->asTexture())
                    {
                        texture->setMaxAnisotropy(16.f);
                        if(texture->getImage(0)) std::cout<<"CORPUS_TEXTURE name="<<texture->getName()<<" file="<<texture->getImage(0)->getFileName()<<'\n';
                    }
            traverse(node);
        }
    };
    struct Pixels final:osg::Camera::DrawCallback
    {
        mutable std::array<unsigned char,128*128*4> data{};
        mutable GLenum error=GL_NO_ERROR;
        void operator()(osg::RenderInfo&) const override
        {glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,data.data());error=glGetError();}
    };
    struct Asset {std::string name;osg::ref_ptr<osg::Node> node;};
    struct Explain final:osg::NodeVisitor
    {
        Explain():osg::NodeVisitor(TRAVERSE_ALL_CHILDREN){setNodeMaskOverride(~0u);}
        void apply(osg::Node& node) override
        {
            unsigned record=~0u;const bool recordKnown=node.getUserValue("recordIndex",record);
            std::cout<<"CORPUS_GRAPH class="<<node.className()<<" mask="<<node.getNodeMask()<<" variance="<<node.getDataVariance()
                <<" update="<<(node.getUpdateCallback()!=nullptr)<<" cull="<<(node.getCullCallback()!=nullptr)
                <<" sphere_callback="<<(node.getComputeBoundingSphereCallback()!=nullptr)
                <<" state_variance="<<(node.getStateSet()?node.getStateSet()->getDataVariance():-1)
                <<" record="<<(recordKnown?std::to_string(record):"unknown")
                <<" children="<<(node.asGroup()?node.asGroup()->getNumChildren():0)<<" name="<<node.getName()<<'\n';
            if(const auto* drawable=node.asDrawable()) std::cout<<"CORPUS_DRAWABLE type="<<typeid(*drawable).name()
                <<" draw_callback="<<(drawable->getDrawCallback()!=nullptr)<<" box_callback="<<(drawable->getComputeBoundingBoxCallback()!=nullptr)<<'\n';
            if(const auto* transform=dynamic_cast<const osg::MatrixTransform*>(&node))
            {
                const auto& m=transform->getMatrix();
                std::cout<<"CORPUS_TRANSFORM scales="<<osg::Vec3d(m(0,0),m(0,1),m(0,2)).length()<<','
                    <<osg::Vec3d(m(1,0),m(1,1),m(1,2)).length()<<','<<osg::Vec3d(m(2,0),m(2,1),m(2,2)).length()<<'\n';
            }
            traverse(node);
        }
    };
}

int main(int argc,char** argv) try
{
    require(argc>=6,"usage: p9-selective-hlod-corpus --probe|--draw|--static-draw openmw.cfg sourceRoot [activePlugin] mesh1 mesh2 [...bounded candidate names]");
    const bool staticRecords=std::string(argv[1])=="--static-draw";
    const bool draw=std::string(argv[1])=="--draw" || staticRecords;
    const int firstMesh=staticRecords?5:4;
    require(argc>=firstMesh+2,"two bounded corpus models required");
    Files::PathContainer directories;std::vector<std::string> archives,content;
    std::ifstream config(std::filesystem::u8path(argv[2]));require(bool(config),"read-only loadout config unavailable");
    for(std::string line;std::getline(config,line);)
    {
        if(line.starts_with("data="))
        {
            std::string path=line.substr(5);if(path.size()>1 && path.front()=='"' && path.back()=='"') path=path.substr(1,path.size()-2);
            const auto directory=std::filesystem::u8path(path);
            if(std::filesystem::is_directory(directory)) directories.push_back(directory);
            else std::cout<<"CORPUS_ABSENT_DIRECTORY "<<path<<'\n';
        }
        if(line.starts_with("fallback-archive=")) archives.push_back(line.substr(17));
        if(line.starts_with("content=")) content.push_back(line.substr(8));
    }
    require(!directories.empty(),"loadout has no source directories");
    VFS::Manager vfs;VFS::registerArchives(&vfs,Files::Collections(directories),archives,true,nullptr);vfs.buildIndex();
    std::set<std::string> staticModels;
    if(staticRecords)
    {
        const std::string plugin=argv[4];
        require(std::find(content.begin(),content.end(),plugin)!=content.end(),"static fixture plugin must be active in the read-only loadout");
        const VFS::Path::Normalized pluginPath(plugin);ESM::ESMReader records;records.open(vfs.get(pluginPath),std::filesystem::u8path(plugin));
        unsigned scanned=0,statics=0;
        while(records.hasMoreRecs())
        {
            require(++scanned<=250000,"bounded static corpus record scan");const auto name=records.getRecName();records.getRecHeader();
            if(name.toInt()!=ESM::REC_STAT) {records.skipRecord();continue;}
            ESM::Static value;bool deleted=false;value.load(records,deleted);++statics;if(deleted) continue;
            const VFS::Path::Normalized model("meshes/"+value.mModel.getNormalized().value());staticModels.insert(model.value());
            for(int arg=firstMesh;arg<argc;++arg) if(model==VFS::Path::Normalized(argv[arg]))
                std::cout<<"CORPUS_STATIC id="<<value.mId.toString()<<" model="<<model.value()<<" active_plugin="<<plugin<<'\n';
        }
        std::cout<<"CORPUS_STATIC_SCAN records="<<scanned<<" static_records="<<statics<<'\n';
    }
    Resource::ImageManager images(&vfs,0);Resource::BgsmFileManager materials(&vfs,0);
    Shader::ShaderManager shaders;shaders.setShaderPath(std::filesystem::u8path(argv[3])/"files/shaders");
    auto defines=Shader::getDefaultDefines();
    for(const auto& [name,value]:SceneUtil::ShadowManager::getShadowsDisabledDefines()) defines[name]=value;
    defines["maxLights"]="8";defines["lightingMethodClustered"]="0";defines["simpleLighting"]="0";
    shaders.setGlobalDefines(defines);
    Shader::ShaderVisitor visitor(shaders,images,"objects");
    visitor.setAutoUseNormalMaps(true);visitor.setNormalMapPattern("_n");visitor.setNormalHeightMapPattern("_nh");
    visitor.setAutoUseSpecularMaps(true);visitor.setSpecularMapPattern("_s");
    std::vector<Asset> assets;
    for(int arg=firstMesh;arg<argc && arg<firstMesh+64;++arg)
    {
        const VFS::Path::Normalized path=VFS::Path::toNormalized(std::string_view(argv[arg]));
        try
        {
            if(staticRecords) require(staticModels.contains(path.value()),"model has no live STATIC record in the selected active plugin");
            Nif::NIFFile file(path);Nif::Reader reader(file,nullptr);reader.parse(vfs.get(path));
            auto node=NifOsg::Loader::load(file,&images,&materials);
            const bool certified=SceneUtil::certifySelectiveHlodTemplate(*node);
            std::cout<<"CORPUS_SOURCE "<<path.value()<<" winning="<<vfs.getArchive(path)<<" original_certificate="<<certified<<'\n';
            if(!certified) {Explain explanation;node->accept(explanation);continue;}
            node->accept(visitor);Samplers samplers;node->accept(samplers);assets.push_back({path.value(),node});
        }
        catch(const std::exception& error) {std::cout<<"CORPUS_REFUSED "<<path.value()<<" reason="<<error.what()<<'\n';}
    }
    SceneUtil::HlodBuildOptions options;options.reserveGpu=false;
    osg::ref_ptr<osg::Node> chosenControl,chosenProxy;
    SceneUtil::HlodBuildResult chosen;
    for(unsigned a=0;a<assets.size();++a) for(unsigned b=a+1;b<assets.size();++b)
    {
        osg::ref_ptr<osg::Group> source=new osg::Group;
        for(const auto* asset : {&assets[a],&assets[b]})
        {
            osg::ref_ptr<osg::MatrixTransform> instance=new osg::MatrixTransform;
            instance->setDataVariance(osg::Object::STATIC);
            instance->setMatrix(osg::Matrix::translate(asset==&assets[a]?0.:asset->node->getBound().radius()*3.,0,0));
            instance->addChild(privateClone(*asset->node));source->addChild(instance);
        }
        osg::ref_ptr<osg::Node> control=osg::clone(source.get(),PagingCopy{});
        osg::ref_ptr<osgDB::SharedStateManager> shared=new osgDB::SharedStateManager;
        shared->share(control);
        SceneUtil::Optimizer optimizer;
        optimizer.optimize(control,SceneUtil::Optimizer::FLATTEN_STATIC_TRANSFORMS|SceneUtil::Optimizer::REMOVE_REDUNDANT_NODES
            |SceneUtil::Optimizer::MERGE_GEOMETRY|SceneUtil::Optimizer::MERGE_GEODES|SceneUtil::Optimizer::SHARE_DUPLICATE_STATE);
        auto result=SceneUtil::buildSelectiveHlod(*source,*control,options);
        std::cout<<"CORPUS_PAIR "<<assets[a].name<<" + "<<assets[b].name<<" control_draws="<<result.controlSubmissions
            <<" proxy_draws="<<result.proxySubmissions<<" charts="<<result.distinctDiffuseImages<<" reason="<<result.rejection<<'\n';
        if(result && !chosenProxy) {chosen=result;chosenProxy=result.representation;chosenControl=control;}
    }
    require(chosenProxy.valid(),"no qualified real diffuse-family cluster: H1 engagement remains unproved");
    if(!draw) return 0;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY();traits->width=128;traits->height=128;traits->pbuffer=true;traits->doubleBuffer=false;traits->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(),"real GL context required");
    osgViewer::Viewer viewer;viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    auto* camera=viewer.getCamera();camera->setGraphicsContext(context);camera->setViewport(0,0,128,128);
    const auto bounds=chosenControl->getBound();const float radius=bounds.radius()*1.25f;
    camera->setProjectionMatrixAsOrtho(-radius,radius,-radius,radius,1,8*radius);
    camera->setViewMatrixAsLookAt(bounds.center()+osg::Vec3(0,-3*radius,radius),bounds.center(),osg::Vec3(0,0,1));
    camera->setClearColor({0,0,0,0});camera->getOrCreateStateSet()->setMode(GL_BLEND,osg::StateAttribute::OFF);
    // Standard required per-view uniforms. No material or geometry changes.
    auto* state=camera->getOrCreateStateSet();state->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf(camera->getProjectionMatrix())));
    state->addUniform(new osg::Uniform("screenRes",osg::Vec2(128,128)));state->addUniform(new osg::Uniform("near",1.f));state->addUniform(new osg::Uniform("far",8*radius));
    SceneUtil::Material defaultMaterial;defaultMaterial.setStateSet(state);SceneUtil::initTexMatForStateSet(*state);
    state->addUniform(new osg::Uniform("alpha",1.f));state->addUniform(new osg::Uniform("actorFade",1.f));
    state->addUniform(new osg::Uniform("sun.position",osg::Vec4(0,0,1,0)));state->addUniform(new osg::Uniform("sun.diffuse",osg::Vec4(1,1,1,1)));
    state->addUniform(new osg::Uniform("sun.ambient",osg::Vec4(.8f,.8f,.8f,1)));state->addUniform(new osg::Uniform("sun.specular",osg::Vec4(0,0,0,0)));
    state->addUniform(new osg::Uniform("PointLightCount",0));
    state->addUniform(new osg::Uniform("fog.color",osg::Vec4(0,0,0,1)));state->addUniform(new osg::Uniform("fog.end",8*radius));state->addUniform(new osg::Uniform("fog.start",7*radius));state->addUniform(new osg::Uniform("fog.depth",-1.f));
    osg::ref_ptr<Pixels> pixels=new Pixels;camera->setFinalDrawCallback(pixels);
    Instrument controlDraws,proxyDraws;chosenControl->accept(controlDraws);chosenProxy->accept(proxyDraws);
    viewer.setSceneData(chosenControl);viewer.realize();viewer.frame();viewer.frame();const auto controlPixels=pixels->data;
    viewer.setSceneData(chosenProxy);viewer.frame();viewer.frame();
    require(*controlDraws.count/2==chosen.controlSubmissions && *proxyDraws.count/2==chosen.proxySubmissions,"real NIF/OSG/shipped-PBR consumed draw reduction");
    unsigned nonempty=0,outside=0;std::uint64_t error=0;
    for(unsigned i=0;i<controlPixels.size();++i)
    {if(i%4!=3 && controlPixels[i])++nonempty;const unsigned difference=std::abs(int(controlPixels[i])-int(pixels->data[i]));error+=difference;if(difference>24)++outside;}
    require(nonempty>128,"actual object pixels must be rendered");
    require(outside<controlPixels.size()/20,"real corpus atlas quality tolerance");
    std::cout<<"H1_REAL_CONSUMPTION control_draws="<<*controlDraws.count/2<<" proxy_draws="<<*proxyDraws.count/2
        <<" atlas_bytes="<<chosen.atlasBytes<<" nonempty_channels="<<nonempty<<" absolute_channel_error="<<error<<" outside_24="<<outside<<'\n';
    require(pixels->error==GL_NO_ERROR,"real corpus GL validation");
    return 0;
}
catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
