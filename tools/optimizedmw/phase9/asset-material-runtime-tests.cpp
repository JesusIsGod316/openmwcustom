#include <components/resource/imagemanager.hpp>
#include <components/sceneutil/generatedstaticlod.hpp>
#include <components/sceneutil/selectivehlod.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/sceneutil/shadowproxygroup.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/material.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/shader/assetgeometrycontract.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/shader/shadervisitor.hpp>
#include <components/vfs/manager.hpp>
#include <osg/Material>
#include <osg/Texture2D>
#include <osg/ClipControl>
#include <osgViewer/Viewer>
#include <osgViewer/Renderer>
#include <osgUtil/GLObjectsVisitor>
#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace L=SceneUtil::GeneratedStaticLod;
namespace
{
    void require(bool value,const char* reason) {if(!value) throw std::runtime_error(reason);}
    std::size_t rgbPixels(const std::array<unsigned char,128*128*4>& data)
    {
        std::size_t count=0;
        for(std::size_t i=0;i<data.size();i+=4) if(data[i]>20 || data[i+1]>20 || data[i+2]>20) ++count;
        return count;
    }
    struct Pixels final:osg::Camera::DrawCallback
    {
        mutable std::array<unsigned char,128*128*4> data{};
        mutable GLenum error=GL_NO_ERROR;
        mutable GLenum beforeRead=GL_NO_ERROR,readError=GL_NO_ERROR;
        mutable unsigned reads=0;
        void operator()(osg::RenderInfo&) const override
        {beforeRead=glGetError();glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,data.data());readError=glGetError();error=beforeRead!=GL_NO_ERROR?beforeRead:readError;++reads;}
    };
    struct Mutation final:osg::Camera::DrawCallback
    {
        std::function<void()> action;
        mutable bool executed=false;
        explicit Mutation(std::function<void()> value):action(std::move(value)) {}
        void operator()(osg::RenderInfo&) const override {if(!executed) {action();executed=true;}}
    };
    struct Shadow final:osg::NodeCallback
    {
        void operator()(osg::Node* node,osg::NodeVisitor* visitor) override
        {SceneUtil::ShadowTraversalScope scope;traverse(node,visitor);}
    };
    std::vector<osg::Geometry*> geometries(osg::Node& root)
    {
        auto paths=L::originalDraws(root);std::vector<osg::Geometry*> result;
        for(const auto& path:paths) result.push_back(path.geometry);return result;
    }
    osg::ref_ptr<osg::Program> adversarialProgram()
    {
        osg::ref_ptr<osg::Program> result=new osg::Program;
        result->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvoid main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}\n"));
        result->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nvoid main(){gl_FragColor=vec4(0.75,0.2,0.85,1.0);}\n"));
        return result;
    }
    osg::ref_ptr<osg::Geometry> grid(float left,unsigned red,unsigned blue)
    {
        constexpr unsigned side=20;
        osg::ref_ptr<osg::Geometry> result=new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array,normals=new osg::Vec3Array;
        osg::ref_ptr<osg::Vec2Array> uv=new osg::Vec2Array;
        for(unsigned y=0;y<=side;++y) for(unsigned x=0;x<=side;++x)
        {vertices->push_back({left+float(x)/side*.85f,-.85f+float(y)/side*1.7f,0});normals->push_back({0,0,1});uv->push_back({float(x)/side,float(y)/side});}
        osg::ref_ptr<osg::DrawElementsUInt> indices=new osg::DrawElementsUInt(GL_TRIANGLES);
        for(unsigned y=0;y<side;++y) for(unsigned x=0;x<side;++x)
        {const unsigned a=y*(side+1)+x;for(unsigned i:{a,a+1,a+side+2,a,a+side+2,a+side+1}) indices->push_back(i);}
        result->setVertexArray(vertices);result->setNormalArray(normals,osg::Array::BIND_PER_VERTEX);
        result->setTexCoordArray(0,uv,osg::Array::BIND_PER_VERTEX);result->addPrimitiveSet(indices);
        result->setUseDisplayList(false);result->setUseVertexBufferObjects(true);result->setDataVariance(osg::Object::STATIC);
        osg::ref_ptr<osg::Image> image=new osg::Image;image->allocateImage(256,256,1,GL_RGBA,GL_UNSIGNED_BYTE);
        image->setDataVariance(osg::Object::STATIC);image->setInternalTextureFormat(GL_RGBA8);
        for(unsigned y=0;y<256;++y) for(unsigned x=0;x<256;++x)
        {auto* p=image->data(x,y);p[0]=static_cast<unsigned char>(red);p[1]=100;p[2]=static_cast<unsigned char>(blue);p[3]=255;}
        osg::ref_ptr<osg::Texture2D> texture=new osg::Texture2D(image);texture->setName("diffuseMap");texture->setDataVariance(osg::Object::STATIC);
        texture->setUnRefImageDataAfterApply(false);texture->setResizeNonPowerOfTwoHint(false);
        texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
        texture->setWrap(osg::Texture::WRAP_S,osg::Texture::REPEAT);texture->setWrap(osg::Texture::WRAP_T,osg::Texture::REPEAT);
        auto* state=result->getOrCreateStateSet();state->setDataVariance(osg::Object::STATIC);
        // Same typed material producer as NIF/RenderingManager. A bare OSG
        // Geometry has no material.* uniforms and would shade as zero in the
        // shipped object programs, even though a transparent clear has alpha.
        osg::ref_ptr<SceneUtil::Material> material=new SceneUtil::Material;
        state->setAttribute(material);material->setStateSet(state);
        state->setTextureAttribute(0,texture);state->setTextureAttribute(0,new SceneUtil::TextureType("diffuseMap"));
        for(const char* name:{"texMat0"}) state->addUniform(new osg::Uniform(name,osg::Matrixf::identity()));
        return result;
    }
    struct Pair
    {
        osg::ref_ptr<osg::Group> source;
        osg::ref_ptr<osg::Node> control,derived;
        osg::ref_ptr<L::Group> selector;
        std::shared_ptr<L::Readiness> readiness;
        bool hlod=false;
        std::uint64_t consumed() const {return hlod?SceneUtil::HlodCounters::consumedDraws.load():L::Counters::derivedDraws.load();}
        std::uint64_t drawFallbacks() const {return hlod?SceneUtil::HlodCounters::materialDrawFallbacks.load():L::Counters::materialDrawFallbacks.load();}
    };
    struct Fixture
    {
        VFS::Manager vfs;
        Resource::ImageManager images{&vfs,100};
        Shader::ShaderManager shaders;
        osg::ref_ptr<SceneUtil::LightManager> light=new SceneUtil::LightManager;
        osg::ref_ptr<osg::Group> scene=new osg::Group;
        osg::ref_ptr<osg::Camera> camera=new osg::Camera;
        osg::ref_ptr<Pixels> pixels=new Pixels;
        osgViewer::Viewer viewer;
        std::filesystem::path shaderPath;
        explicit Fixture(const std::filesystem::path& path):shaderPath(path)
        {
            shaders.setShaderPath(shaderPath);
            auto defines=Shader::getDefaultDefines();
            for(const auto& [name,value]:SceneUtil::ShadowManager::getShadowsDisabledDefines()) defines[name]=value;
            for(const auto& [name,value]:light->getLightDefines()) defines[name]=value;
            defines["reverseZ"]=SceneUtil::AutoDepth::isReversed() ? "1" : "0";
            shaders.setGlobalDefines(defines);
            osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
            traits->width=128;traits->height=128;traits->windowDecoration=false;traits->doubleBuffer=false;traits->pbuffer=true;
            auto context=osg::GraphicsContext::createGraphicsContext(traits);
            require(context,"real OpenGL context required; no skip is a passing qualification");
            camera->setGraphicsContext(context);camera->setViewport(0,0,128,128);
            camera->setProjectionMatrixAsPerspective(35,1,.1,1000);camera->setViewMatrixAsLookAt({0,0,8},{0,0,0},{0,1,0});
            camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
            camera->setCullingMode(osg::CullSettings::NO_CULLING);camera->setClearColor({0,0,0,1});camera->setClearMask(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            camera->setDrawBuffer(GL_FRONT);camera->setReadBuffer(GL_FRONT);camera->setFinalDrawCallback(pixels);
            auto* state=scene->getOrCreateStateSet();state->setDataVariance(osg::Object::DYNAMIC);
            const auto renderProjection=SceneUtil::AutoDepth::isReversed()
                ? SceneUtil::getReversedZProjectionMatrixAsPerspective(35,1,.1,1000)
                : camera->getProjectionMatrix();
            state->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf(renderProjection)));
            if(SceneUtil::AutoDepth::isReversed())
            {
                state->setAttributeAndModes(new SceneUtil::AutoDepth,osg::StateAttribute::ON);
                state->setAttributeAndModes(new osg::ClipControl(osg::ClipControl::LOWER_LEFT,osg::ClipControl::ZERO_TO_ONE),osg::StateAttribute::ON);
            }
            SceneUtil::setCameraClearDepth(camera);
            state->addUniform(new osg::Uniform("fixtureFrame",0.f));
            state->addUniform(new osg::Uniform("alpha",1.f));state->addUniform(new osg::Uniform("actorFade",1.f));
            state->addUniform(new osg::Uniform("distortionStrength",0.f));
            state->addUniform(new osg::Uniform("near",.1f));state->addUniform(new osg::Uniform("far",1000.f));
            state->addUniform(new osg::Uniform("fogColor",osg::Vec4(0,0,0,1)));state->addUniform(new osg::Uniform("fogStart",999.f));state->addUniform(new osg::Uniform("fogEnd",1000.f));
            osg::ref_ptr<SceneUtil::Light> sun=new SceneUtil::Light;
            sun->setPosition({0,0,1,0});sun->setAmbient({.6f,.6f,.6f,1});sun->setDiffuse({.8f,.8f,.8f,1});sun->setSpecular({0,0,0,1});light->setSunlight(sun);
            scene->addChild(light);viewer.setCamera(camera);viewer.setLightingMode(osgViewer::View::NO_LIGHT);
            viewer.setSceneData(scene);viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);viewer.realize();
        }
        Pair make(bool hlod)
        {
            Pair pair;pair.hlod=hlod;pair.source=new osg::Group;pair.source->setDataVariance(osg::Object::STATIC);
            pair.source->addChild(grid(-.92f,190,30));pair.source->addChild(grid(.07f,30,190));
            Shader::ShaderVisitor visitor(shaders,images,"objects");pair.source->accept(visitor);
            for(auto* geometry:geometries(*pair.source))
            {
                const auto* program=dynamic_cast<const osg::Program*>(geometry->getStateSet()->getAttribute(osg::StateAttribute::PROGRAM));
                require(program && Shader::AssetGeometry::matches(*program),"actual packaged ShaderManager program family must qualify");
            }
            pair.control=osg::clone(pair.source.get(),osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES|osg::CopyOp::DEEP_COPY_PRIMITIVES|osg::CopyOp::DEEP_COPY_STATESETS);
            if(hlod)
            {
                SceneUtil::HlodBuildOptions options;options.reserveGpu=false;
                const auto result=SceneUtil::buildSelectiveHlod(*pair.source,*pair.control,options);
                if(!result) std::cerr<<"H1_REASON "<<result.rejection<<'\n';
                require(bool(result) && result.proxySubmissions<result.controlSubmissions,"independent qualified H1 must reduce real differing-texture submissions");
                pair.derived=result.representation;pair.readiness=std::make_shared<L::Readiness>();
                pair.selector=new L::Group(*pair.control,*pair.derived,0,pair.readiness,camera,true);
            }
            else
            {
                const auto result=L::build(*pair.control,"asset-material-runtime-grid",camera,false);
                require(result.node && result.derivedIndices<result.originalIndices,"L1 must build a genuine reduced index population");
                pair.selector=result.node;pair.derived=result.node->derived();pair.readiness=result.readiness;
            }
            return pair;
        }
        void show(osg::Node& node) {light->removeChildren(0,light->getNumChildren());light->addChild(&node);}
        void frame()
        {
            const auto reads=pixels->reads;scene->getStateSet()->getUniform("fixtureFrame")->set(float(reads));viewer.frame();
            if(pixels->reads<=reads || pixels->error!=GL_NO_ERROR)
                std::cerr<<"ASSET_MATERIAL_FRAME reads="<<pixels->reads<<" previous="<<reads<<" before_read_gl="<<pixels->beforeRead
                    <<" read_gl="<<pixels->readError<<" l1_draws="<<L::Counters::derivedDraws<<" h1_draws="<<SceneUtil::HlodCounters::consumedDraws
                    <<" original_draws="<<L::Counters::controlDraws<<" rgb="<<rgbPixels(pixels->data)<<'\n';
            require(pixels->reads>reads && pixels->error==GL_NO_ERROR,"actual rendered pixel receipt and no GL error required");
        }
        auto ordinary(Pair& pair) {show(*pair.control);frame();return pixels->data;}
        void prepare(Pair& pair)
        {
            show(*pair.selector);pair.readiness->ready=false;frame();
            // Compile both exact stock/PBR representations on the actual owner.
            osgUtil::GLObjectsVisitor compile(osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES|osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS);
            auto* context=camera->getGraphicsContext();require(context->makeCurrent(),"owner context unavailable");
            compile.setState(context->getState());pair.selector->accept(compile);context->releaseContext();pair.readiness->ready=true;
            const auto before=pair.consumed();frame();require(pair.consumed()>before,"ready candidate must actually submit derived geometry");
            if(rgbPixels(pixels->data)<=1000)
                std::cerr<<"ASSET_MATERIAL_RGB lane="<<(pair.hlod?"H1":"L1")<<" rgb_pixels="<<rgbPixels(pixels->data)
                    <<" derived_delta="<<(pair.consumed()-before)<<" original_draws="<<L::Counters::controlDraws
                    <<" gl_error="<<pixels->error<<" reversed="<<SceneUtil::AutoDepth::isReversed()<<'\n';
            require(rgbPixels(pixels->data)>1000,"positive RGB geometry required");
        }
    };
    void replaceProgram(osg::Geometry& geometry,osg::Program& program)
    {
        osg::ref_ptr<osg::StateSet> state=new osg::StateSet(*geometry.getStateSet(),osg::CopyOp::SHALLOW_COPY);
        state->setAttribute(&program);geometry.setStateSet(state);
    }
    void parity(const std::array<unsigned char,128*128*4>& a,const std::array<unsigned char,128*128*4>& b)
    {require(a==b,"fallback must exactly match ordinary control pixels, including cleared candidate material");}
    void cases(Fixture& fixture,bool hlod)
    {
        auto pair=fixture.make(hlod);fixture.prepare(pair);
        const auto baseline=fixture.ordinary(pair);fixture.show(*pair.selector);fixture.frame();
        const auto coarse=pair.consumed();require(coarse>0,"positive consumption cannot be zero");
        // Source/control leaf mutation before cull must select the original.
        auto malicious=adversarialProgram();replaceProgram(*geometries(*pair.control).front(),*malicious);
        const auto expected=fixture.ordinary(pair);fixture.show(*pair.selector);const auto before=pair.consumed();fixture.frame();
        require(pair.consumed()==before,"source mutation before cull must not submit derived geometry");parity(expected,fixture.pixels->data);
        // Each draw-time case starts with a genuinely ready positive candidate.
        for(bool derived:{false,true})
        {
            pair=fixture.make(hlod);fixture.prepare(pair);const auto control=fixture.ordinary(pair);fixture.show(*pair.selector);
            auto targets=geometries(derived?*pair.derived:*pair.control);
            std::vector<osg::ref_ptr<osg::Geometry>> retained;for(auto* target:targets) retained.emplace_back(target);
            osg::ref_ptr<Mutation> mutation=new Mutation([retained,malicious]{for(const auto& target:retained) replaceProgram(*target,*malicious);});
            fixture.camera->setPreDrawCallback(mutation);const auto start=pair.consumed(),fallbacks=pair.drawFallbacks();fixture.frame();fixture.camera->setPreDrawCallback(nullptr);
            require(mutation->executed && pair.drawFallbacks()>fallbacks,"ordered mutation after cull must exercise actual draw-time fallback");
            require(pair.consumed()==start,"mutated candidate must never execute stale derived submission");
            const auto fallbackPixels=fixture.pixels->data;
            const auto actualControl=derived?control:fixture.ordinary(pair);parity(actualControl,fallbackPixels);
        }
        // Removed source texture/material must not inherit the stale candidate.
        pair=fixture.make(hlod);fixture.prepare(pair);fixture.show(*pair.selector);
        auto target=geometries(*pair.control).front();
        osg::ref_ptr<Mutation> removal=new Mutation([target=osg::ref_ptr<osg::Geometry>(target)]{
            osg::ref_ptr<osg::StateSet> state=new osg::StateSet(*target->getStateSet(),osg::CopyOp::SHALLOW_COPY);
            state->removeTextureAttribute(0,osg::StateAttribute::TEXTURE);target->setStateSet(state);
        });
        fixture.camera->setPreDrawCallback(removal);const auto refused=pair.drawFallbacks();fixture.frame();fixture.camera->setPreDrawCallback(nullptr);
        require(pair.drawFallbacks()>refused,"source role removal must trigger replay");const auto removed=fixture.pixels->data;parity(fixture.ordinary(pair),removed);
        // Effective ancestor Program OVERRIDE must also force the original.
        pair=fixture.make(hlod);fixture.prepare(pair);auto* sourceProgram=dynamic_cast<osg::Program*>(geometries(*pair.source).front()->getStateSet()->getAttribute(osg::StateAttribute::PROGRAM));
        Shader::ShaderManager other;other.setShaderPath(fixture.shaderPath);auto globals=fixture.shaders.getGlobalDefines();other.setGlobalDefines(globals);
        auto otherGeometry=grid(-.92f,190,30);Shader::ShaderVisitor otherVisitor(other,fixture.images,"objects");otherGeometry->accept(otherVisitor);
        osg::ref_ptr<osg::Program> alternate=dynamic_cast<osg::Program*>(otherGeometry->getStateSet()->getAttribute(osg::StateAttribute::PROGRAM));
        require(alternate && alternate!=sourceProgram && Shader::AssetGeometry::matches(*alternate),"ancestor negative needs a distinct actual certified program");
        fixture.scene->getStateSet()->setAttribute(alternate,osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        const auto overrideBefore=pair.consumed();fixture.show(*pair.selector);fixture.frame();
        require(pair.consumed()==overrideBefore,"different certified ancestor override must decline candidate");
        const auto ancestorPixels=fixture.pixels->data;parity(fixture.ordinary(pair),ancestorPixels);
        fixture.scene->getStateSet()->removeAttribute(osg::StateAttribute::PROGRAM);
        // The renderer's known reversed-depth uniform has the ordinary cull
        // frustum. Arbitrary XY/depth edits must still take the real control.
        pair=fixture.make(hlod);fixture.prepare(pair);
        auto* sceneState=fixture.scene->getStateSet();auto* projectionUniform=sceneState->getUniform("projectionMatrix");
        osg::Matrixf properProjection;require(projectionUniform->get(properProjection),"fixture projection uniform kind");
        const auto properCull=fixture.camera->getProjectionMatrix();
        const auto offset=osg::Matrixd::translate(.012,-.01,0);
        fixture.camera->setProjectionMatrix(properCull*offset);projectionUniform->set(osg::Matrixf(osg::Matrixd(properProjection)*offset));
        fixture.show(*pair.selector);const auto offsetBefore=pair.consumed();fixture.frame();
        require(pair.consumed()>offsetBefore,"exact renderer XY projection offset must retain derived engagement");
        fixture.camera->setProjectionMatrix(properCull);projectionUniform->set(properProjection);
        for(unsigned column:{0u,2u})
        {
            auto changed=properProjection;changed(column,column)+=.05f;projectionUniform->set(changed);
            fixture.show(*pair.selector);const auto projectionBefore=pair.consumed(),projectionOriginal=L::Counters::controlDraws.load();fixture.frame();
            require(pair.consumed()==projectionBefore && L::Counters::controlDraws>projectionOriginal,"arbitrary inherited projection must submit retained control");
            const auto declined=fixture.pixels->data;parity(fixture.ordinary(pair),declined);
            projectionUniform->set(properProjection);
        }
        if(SceneUtil::AutoDepth::isReversed())
        {
            osg::ref_ptr<osg::StateAttribute> properClip=sceneState->getAttribute(osg::StateAttribute::CLIPCONTROL);
            require(properClip,"reversed fixture exact clip control required");
            for(bool missing:{true,false})
            {
                if(missing) sceneState->removeAttribute(osg::StateAttribute::CLIPCONTROL);
                else sceneState->setAttributeAndModes(new osg::ClipControl(osg::ClipControl::LOWER_LEFT,osg::ClipControl::NEGATIVE_ONE_TO_ONE),osg::StateAttribute::ON);
                fixture.show(*pair.selector);const auto clipBefore=pair.consumed(),clipOriginal=L::Counters::controlDraws.load();fixture.frame();
                require(pair.consumed()==clipBefore && L::Counters::controlDraws>clipOriginal,"unknown or incompatible clip control must submit retained control");
                const auto declined=fixture.pixels->data;parity(fixture.ordinary(pair),declined);
                sceneState->setAttributeAndModes(properClip,osg::StateAttribute::ON);
            }
        }
        // Main-camera identity and exact shadow traversal always use original.
        pair=fixture.make(hlod);fixture.prepare(pair);
        osg::ref_ptr<osg::GraphicsContext> ownerContext=fixture.camera->getGraphicsContext();
        fixture.camera->setGraphicsContext(nullptr); // runOperations visits every registered camera, including an old master.
        osg::ref_ptr<osg::Camera> secondary=new osg::Camera;
        secondary->setViewport(0,0,128,128);secondary->setViewMatrix(fixture.camera->getViewMatrix());secondary->setProjectionMatrix(fixture.camera->getProjectionMatrix());
        secondary->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);secondary->setCullingMode(osg::CullSettings::NO_CULLING);
        secondary->setClearColor(fixture.camera->getClearColor());secondary->setClearMask(fixture.camera->getClearMask());
        secondary->setClearDepth(fixture.camera->getClearDepth());
        secondary->setDrawBuffer(GL_FRONT);secondary->setReadBuffer(GL_FRONT);secondary->addChild(fixture.scene);
        secondary->setGraphicsContext(ownerContext);secondary->setFinalDrawCallback(fixture.pixels);
        secondary->setPreDrawCallback(nullptr);
        secondary->setRenderer(new osgViewer::Renderer(secondary));
        require(ownerContext->getCameras().size()==1 && ownerContext->getCameras().front()==secondary.get(),"secondary qualification requires one actual graphics camera owner");
        fixture.viewer.setCamera(secondary);const auto secondaryBefore=pair.consumed(),originalBefore=L::Counters::controlDraws.load();fixture.frame();
        if(pair.consumed()!=secondaryBefore || L::Counters::controlDraws<=originalBefore)
            std::cerr<<"ASSET_MATERIAL_SECONDARY lane="<<(hlod?"H1":"L1")<<" derived_delta="<<(pair.consumed()-secondaryBefore)
                <<" original_delta="<<(L::Counters::controlDraws.load()-originalBefore)<<" reads="<<fixture.pixels->reads<<'\n';
        require(pair.consumed()==secondaryBefore && L::Counters::controlDraws>originalBefore,"secondary view must draw retained original");
        require(rgbPixels(fixture.pixels->data)>1000,"secondary view must render actual original RGB pixels");
        secondary->setGraphicsContext(nullptr);fixture.camera->setGraphicsContext(ownerContext);fixture.viewer.setCamera(fixture.camera);
        require(ownerContext->getCameras().size()==1 && ownerContext->getCameras().front()==fixture.camera.get(),"main restoration requires one actual graphics camera owner");
        fixture.scene->setCullCallback(new Shadow);
        const auto shadowBefore=pair.consumed(),shadowOriginal=L::Counters::controlDraws.load();fixture.frame();fixture.scene->setCullCallback(nullptr);
        require(pair.consumed()==shadowBefore && L::Counters::controlDraws>shadowOriginal,"shadow traversal must draw retained original");
        require(rgbPixels(fixture.pixels->data)>1000,"shadow traversal scope must render actual original RGB pixels");
        std::cout<<"ASSET_MATERIAL_CASE lane="<<(hlod?"H1":"L1")<<" positive=1 source_cull=1 source_draw=1 derived_draw=1 removed_role=1 secondary_original=1 shadow_original=1 gl_error=0\n";
    }
    void admission(Fixture& fixture)
    {
        auto pair=fixture.make(false);auto paths=L::originalDraws(*pair.control);auto tier=L::originalDraws(*pair.derived).front();
        osg::ref_ptr<osg::Group> huge=new osg::Group;
        osg::ref_ptr<osg::Group> child=new osg::Group;
        for(unsigned i=0;i<1025;++i) huge->addChild(child);
        auto input=paths.front();input.path.insert(input.path.begin(),huge.get());
        require(!L::MaterialGuard::captureDraws({&input,1},tier),"oversized child metadata must decline before materialization");
        input=paths.front();input.geometry->getOrCreateStateSet()->addUniform(new osg::Uniform(osg::Uniform::FLOAT_MAT4,"oversized",4097));
        require(!L::MaterialGuard::captureDraws({&input,1},tier),"oversized uniform metadata must decline before cloning");
        pair=fixture.make(false);paths=L::originalDraws(*pair.control);tier=L::originalDraws(*pair.derived).front();
        auto guard=L::MaterialGuard::captureDraws({&paths.front(),1},tier);
        require(guard && guard->current(),"baseline stream lifetime guard must qualify");
        auto* original=dynamic_cast<osg::DrawElementsUInt*>(paths.front().geometry->getPrimitiveSet(0));require(original,"fixture original primitive kind");
        paths.front().geometry->setPrimitiveSet(0,new osg::DrawElementsUInt(*original));
        require(!guard->current(),"replaced source primitive with identical layout must lose lifetime identity");
        pair=fixture.make(false);paths=L::originalDraws(*pair.control);tier=L::originalDraws(*pair.derived).front();
        guard=L::MaterialGuard::captureDraws({&paths.front(),1},tier);require(guard && guard->current(),"baseline image lifetime guard must qualify");
        auto* texture=dynamic_cast<osg::Texture2D*>(paths.front().geometry->getStateSet()->getTextureAttribute(0,osg::StateAttribute::TEXTURE));
        require(texture && texture->getImage(),"fixture source texture");
        texture->setImage(new osg::Image(*texture->getImage(),osg::CopyOp::DEEP_COPY_ALL));
        require(!guard->current(),"replaced source image with identical layout must lose lifetime identity");
        std::cout<<"ASSET_MATERIAL_CAPACITY children_denied=1 uniform_denied=1 stream_replacement=1 image_replacement=1 caps_preserved=1\n";
    }
}
int main(int argc,char** argv) try
{
    require((argc==3 || (argc==4 && std::string_view(argv[3])=="--reverse-z")) && std::string_view(argv[1])=="--shader-dir",
        "usage: p9-asset-material-runtime --shader-dir exact-shader-tree [--reverse-z]");
    const bool reversed=argc==4;SceneUtil::AutoDepth::setReversed(reversed);
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK","3");
#else
    setenv("OPENMW_P9_ASSET_MASK","3",1);
#endif
    Settings::SettingsFileParser parser;parser.loadSettingsFile(std::filesystem::path(P9_SOURCE)/"files/settings-default.cfg",Settings::Manager::mDefaultSettings);
    Settings::StaticValues::initDefaults();Settings::Manager::mUserSettings=Settings::Manager::mDefaultSettings;Settings::StaticValues::init();
    Fixture fixture(std::filesystem::u8path(argv[2]));admission(fixture);cases(fixture,false);cases(fixture,true);
    std::cout<<"ASSET_MATERIAL_PROJECTION reversed="<<reversed<<" arbitrary_projection_refused=1 clipcontrol_refused="<<reversed<<" gl_error=0\n";
    std::cout<<"ASSET_MATERIAL_RUNTIME {\"l1\":true,\"h1\":true,\"source_mutation\":true,\"derived_mutation\":true,\"cull_fallback\":true,\"draw_fallback\":true,\"control_preserved\":true,\"gl_error\":0}\n";
    std::cout<<"ASSET_MATERIAL_PASS l1=1 h1=1 source_mutation=1 derived_mutation=1 cull_fallback=1 draw_fallback=1 control_preserved=1 gl_error=0\n";
    return 0;
}
catch(const std::exception& error) {std::cerr<<"ASSET_MATERIAL_FAIL "<<error.what()<<'\n';return 1;}
