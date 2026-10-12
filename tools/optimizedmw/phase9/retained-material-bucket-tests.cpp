#define P9_RETAINED_BUCKET_TEST_HOOKS
#include "retained-material-bucket-prototype.hpp"
#include <osgViewer/Renderer>
#include <osgViewer/Viewer>
#include <osgUtil/RenderLeaf>
#include <osgUtil/SceneView>

#include <array>
#include <iostream>
#include <stdexcept>

namespace Bucket = SceneUtil::RetainedMaterialBucketPrototype;
struct CountingCullCallback final : osg::NodeCallback
{
    unsigned calls = 0;
    void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
    { ++calls; traverse(node, visitor); }
};
unsigned checks = 0;
void require(bool condition, const char* message)
{ ++checks; if (!condition) throw std::runtime_error(message); }

unsigned leaves(const osgUtil::StateGraph& graph)
{
    unsigned count = static_cast<unsigned>(graph._leaves.size());
    for (const auto& [state, child] : graph._children) count += leaves(*child);
    return count;
}

void retainBucketFrames(const osgUtil::StateGraph& graph, std::vector<osg::ref_ptr<osg::Drawable>>& held)
{
    for (const auto& leaf : graph._leaves)
        if (dynamic_cast<Bucket::Frame*>(leaf->_drawable.get())) held.emplace_back(leaf->_drawable);
    for (const auto& [state, child] : graph._children) retainBucketFrames(*child, held);
}

struct Pixels final : osg::Camera::DrawCallback
{
    mutable std::array<unsigned char, 48 * 48 * 4> rgba{};
    mutable unsigned activeLeaves = 0;
    bool holdBucketFrames = false;
    mutable std::vector<osg::ref_ptr<osg::Drawable>> heldFrames;
    void operator()(osg::RenderInfo& info) const override
    {
        glReadPixels(0, 0, 48, 48, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        auto* renderer = dynamic_cast<osgViewer::Renderer*>(info.getCurrentCamera()->getRenderer());
        require(renderer && renderer->getSceneView(0), "actual OSG scene-view oracle unavailable");
        // The fixture is explicitly SingleThreaded: Renderer scene-view0 is its active view.
        activeLeaves = leaves(*renderer->getSceneView(0)->getStateGraph());
        if (holdBucketFrames) retainBucketFrames(*renderer->getSceneView(0)->getStateGraph(), heldFrames);
        require(glGetError() == GL_NO_ERROR, "native bucket generated a GL error");
    }
};

osg::ref_ptr<osg::StateSet> material(osg::Vec4 color)
{
    osg::ref_ptr<osg::StateSet> result = new osg::StateSet;
    result->setDataVariance(osg::Object::STATIC);
    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX, Bucket::VertexSource));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT, Bucket::FragmentSource));
    result->setAttribute(program);
    result->setAttributeAndModes(new osg::Depth(osg::Depth::LESS), osg::StateAttribute::ON);
    result->addUniform(new osg::Uniform("materialColor", color));
    return result;
}

osg::ref_ptr<osg::Geometry> triangle(float left, float bottom, float height, osg::StateSet* state)
{
    osg::ref_ptr<osg::Geometry> result = new osg::Geometry;
    result->setDataVariance(osg::Object::STATIC);
    result->setUseDisplayList(false); result->setUseVertexBufferObjects(true); result->setStateSet(state);
    osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
    vertices->push_back({left,bottom,-.1f}); vertices->push_back({left+.4f,bottom,-.1f});
    vertices->push_back({left+.2f,bottom+height,-.1f});
    result->setVertexArray(vertices); result->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    return result;
}

int main() try
{
    osgViewer::Viewer viewer;
    viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded); viewer.setLightingMode(osgViewer::View::NO_LIGHT);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=48; traits->height=48; traits->doubleBuffer=false; traits->windowDecoration=false; traits->pbuffer=true;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context.valid(), "real off-screen GL context required");
    auto* camera=viewer.getCamera(); camera->setGraphicsContext(context); camera->setViewport(0,0,48,48);
    camera->setProjectionMatrix(osg::Matrix::identity()); camera->setViewMatrix(osg::Matrix::identity());
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    camera->setCullingMode(osg::CullSettings::VIEW_FRUSTUM_CULLING); camera->setClearColor({0,0,0,1});
    osg::ref_ptr<Pixels> pixels=new Pixels; camera->setFinalDrawCallback(pixels);
    auto orange=material({1,.25f,.05f,1}), green=material({0,1,0,1});
    orange->setName(std::string(4096,'x'));orange->setUserValue("source_only_metadata",std::string(4096,'y'));
    auto* sourceProgram = static_cast<osg::Program*>(orange->getAttribute(osg::StateAttribute::PROGRAM));
    // OSG applies Program names as GL object labels on this driver. Keep that
    // label legal; large unconsumed user metadata still proves no generic copy.
    sourceProgram->setName("source-only-program");sourceProgram->setUserValue("program_metadata",std::string(4096,'m'));
    sourceProgram->getShader(0)->setUserValue("shader_metadata",std::string(4096,'s'));
    green->addUniform(green->getUniform("materialColor"),osg::StateAttribute::ON|osg::StateAttribute::PROTECTED);
    osg::ref_ptr<osg::Group> source=new osg::Group;
    source->getOrCreateStateSet()->addUniform(new osg::Uniform("viewScale",1.f));
    std::array<osg::ref_ptr<osg::Geometry>,4> children;
    for(unsigned i=0;i<children.size();++i){children[i]=triangle(-.9f+.45f*i,-.75f,1.3f,orange);source->addChild(children[i]);}
    children[0]->setName(std::string(4096,'g'));children[0]->setUserValue("geometry_metadata",std::string(4096,'g'));
    children[0]->getVertexArray()->setUserValue("array_metadata",std::string(4096,'a'));
    children[0]->getPrimitiveSet(0)->setUserValue("primitive_metadata",std::string(4096,'d'));
    children[3]->setNodeMask(2);
    auto work=std::make_shared<Bucket::Work>();
    require(Bucket::build(*source,*camera,work,false)==source.get(), "disabled control allocated a replacement");
    require(Bucket::build(*source,*camera,work,true,1)==source.get(), "capacity failure did not use ordinary control");
    auto candidate=Bucket::build(*source,*camera,work,true);
    auto* bucket=dynamic_cast<Bucket::Group*>(candidate.get());
    require(bucket && work->liveOwnedBytes>0, "native material bucket did not own a bounded payload");
    const auto liveCharge = work->liveOwnedBytes.load();
    require(Bucket::build(*source,*camera,work,true,liveCharge)==source.get()
        && work->liveOwnedBytes==liveCharge,"pending/retiring shared allowance was not bounded before copies");
    // The following ordinary sentinel uses another material and disjoint coverage.
    // It is an independent oracle for ordinary state after the bucket; not a claim
    // that arbitrary depth-tied/order-sensitive external primitives are supported.
    auto sentinel=triangle(-.2f,.65f,.25f,green);
    auto root=[&](osg::Node* population){
        osg::ref_ptr<osg::Group> value=new osg::Group;
        value->getOrCreateStateSet()->addUniform(new osg::Uniform("viewScale",1.f));
        value->addChild(population);value->addChild(sentinel);return value;
    };
    auto controlRoot=root(source), candidateRoot=root(candidate);
    viewer.setSceneData(controlRoot); viewer.realize();require(viewer.isRealized(),"fixture context not realized");
    viewer.frame();
    auto compare=[&](const char* label, bool expectReplacement){
        viewer.setSceneData(controlRoot);viewer.frame();const auto reference=pixels->rgba;const auto ordinaryLeaves=pixels->activeLeaves;
        const auto submitted=work->candidateLeaves.load(), geometries=work->geometrySetupsPreserved.load();
        viewer.setSceneData(candidateRoot);viewer.frame();
        require(pixels->rgba==reference,"ordinary/bucket exact RGBA or following sentinel changed");
        require(pixels->rgba[(41*48+24)*4+1]==255,"following ordinary green sentinel rendered no known output");
        if(expectReplacement){
            if(work->candidateLeaves.load()==submitted)std::cerr<<"inert: refusal="<<work->lastDecline.load()
                <<" cull_calls="<<work->cullCalls.load()<<" material_guards="<<work->materialGuards.load()
                <<" record_guards="<<work->sourceRecordGuards.load()<<" observed_leaves="<<pixels->activeLeaves<<'\n';
            if(work->candidateLeaves.load()==submitted && work->diagnosticDeclinedState){
                const auto* state=work->diagnosticDeclinedState.get();
                std::cerr<<"declined_state variance="<<state->getDataVariance()<<" bin="<<state->getBinName()<<" hint="<<state->getRenderingHint()
                    <<" details="<<state->useRenderBinDetails()<<" defines="<<state->getDefineList().size()<<'\n';
                for(const auto& [mode,value]:state->getModeList())std::cerr<<" mode="<<mode<<" flags="<<value<<'\n';
                for(const auto& [type,value]:state->getAttributeList())std::cerr<<" attr="<<type.first<<" class="<<value.first->className()<<'\n';
                for(const auto& [name,value]:state->getUniformList())std::cerr<<" uniform="<<name<<" type="<<value.first->getType()<<'\n';
            }
            require(work->candidateLeaves.load()>submitted,"replacement path was inert");
            require(pixels->activeLeaves==2,"actual OSG bucket+sentinel render-leaf count is not2");
            require(ordinaryLeaves>pixels->activeLeaves,"ordinary render leaves were not actually removed");
            require(work->geometrySetupsPreserved.load()>geometries,"preserved base Geometry work was not counted");
        }else require(work->candidateLeaves.load()==submitted,"unsupported/invalidated path used stale replacement");
        std::cout<<"CASE "<<label<<" rgba_equal=1 ordinary_leaves="<<ordinaryLeaves<<" candidate_leaves="<<pixels->activeLeaves
            <<" replacement="<<expectReplacement<<" preserved_geometry_setups="<<(work->geometrySetupsPreserved.load()-geometries)
            <<" live_known_bytes="<<work->liveOwnedBytes.load()<<'\n';
    };
    camera->setCullMask(1);compare("three_visible",true);
    camera->setCullMask(~0u);compare("four_visible",true);
    source->getStateSet()->getUniform("viewScale")->set(.6f);compare("ancestor_uniform",true);
    camera->setViewMatrix(osg::Matrix::translate(.08,.02,0));compare("camera_matrix",true);
    // Unknown and mistyped inherited globals remain ordinary, even if their
    // names resemble SceneView globals. A partial color mask is also refused.
    source->getStateSet()->addUniform(new osg::Uniform("osg_Unknown",1.f));compare("unknown_global_fallback",false);
    source->getStateSet()->removeUniform("osg_Unknown");
    source->getStateSet()->addUniform(new osg::Uniform("osg_FrameNumber",1.f));compare("mistyped_global_fallback",false);
    source->getStateSet()->removeUniform("osg_FrameNumber");
    source->getStateSet()->setAttribute(new osg::ColorMask(false,true,true,true));compare("partial_write_fallback",false);
    source->getStateSet()->removeAttribute(osg::StateAttribute::COLORMASK);compare("full_write_resume",true);
    osg::ref_ptr<osg::Camera> foreignCamera=new osg::Camera;
    auto foreign=Bucket::build(*source,*foreignCamera,work,true);
    require(dynamic_cast<Bucket::Group*>(foreign.get()),"foreign-camera fixture did not construct");
    candidateRoot=root(foreign);compare("foreign_camera_fallback",false);
    candidateRoot=root(candidate);foreign=nullptr;
    pixels->holdBucketFrames=true;
    compare("held_frame_slot1",true);compare("held_frame_slot2",true);compare("held_frame_slot3",true);
    require(pixels->heldFrames.size()==3,"bounded frame-slot fixture did not retain three actual bucket consumers");
    require(pixels->heldFrames[0]->getStateSet()->getName().empty()
        && !pixels->heldFrames[0]->getStateSet()->getUserDataContainer(),"source metadata escaped into owned material copies");
    const auto& privatePayload = static_cast<const Bucket::Frame*>(pixels->heldFrames[0].get())->testPayload();
    const auto* privateProgram = static_cast<const osg::Program*>(privatePayload.ownedMaterial->getAttribute(osg::StateAttribute::PROGRAM));
    require(privateProgram != sourceProgram && privateProgram->getName().empty() && !privateProgram->getUserDataContainer()
        && !privateProgram->getShader(0)->getUserDataContainer(),"source program/shader metadata escaped into private recipe");
    for(const auto& record : privatePayload.records){
        require(record.owned.get()!=record.original.get() && record.owned->getName().empty() && !record.owned->getUserDataContainer(),
            "source geometry metadata escaped into owned payload");
        require(record.owned->getVertexArray()!=record.original->getVertexArray() && !record.owned->getVertexArray()->getUserDataContainer(),
            "source array metadata or source mutable vertices escaped into owned payload");
        require(record.owned->getPrimitiveSet(0)!=record.original->getPrimitiveSet(0) && !record.owned->getPrimitiveSet(0)->getUserDataContainer(),
            "source primitive metadata escaped into owned payload");
    }
    const auto busyBefore=work->busyFrames.load();compare("held_frame_pool_fallback",false);
    require(work->busyFrames>busyBefore,"held frame pool did not charge its ordinary fallback");
    pixels->holdBucketFrames=false;pixels->heldFrames.clear();compare("frame_pool_resume",true);
    source->setDataVariance(osg::Object::DYNAMIC);compare("root_variance_fallback",false);
    source->setDataVariance(osg::Object::STATIC);compare("root_variance_resume",true);
    osg::ref_ptr<CountingCullCallback> rootCallback = new CountingCullCallback;
    source->setCullCallback(rootCallback);compare("root_callback_fallback",false);
    require(rootCallback->calls >= 2,"root cull callback was stripped from fallback traversal");
    source->setCullCallback(nullptr);compare("root_callback_resume",true);
    require(work->materialPatches==0 && work->customRestorationCalls==0,"prototype hid material patch or custom restoration work");
    require(work->sourceRecordGuards>0 && work->materialGuards>0,"guard overhead was not charged");
    camera->setComputeNearFarMode(osg::CullSettings::COMPUTE_NEAR_FAR_USING_BOUNDING_VOLUMES);compare("near_far_fallback",false);
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    auto* vertices=static_cast<osg::Vec3Array*>(children[0]->getVertexArray());
    (*vertices)[0].x()+=.1f;vertices->dirty();children[0]->dirtyBound();compare("array_generation_fallback",false);
    candidate=Bucket::build(*source,*camera,work,true);bucket=dynamic_cast<Bucket::Group*>(candidate.get());
    require(bucket,"generation rebuild failed");candidateRoot=root(candidate);compare("array_generation_rebuild",true);
    // Flags change without changing the Uniform pointer/value: stale resolution
    // must fallback rather than freezing the pre-PROTECTED cloned material.
    orange->addUniform(orange->getUniform("materialColor"),osg::StateAttribute::ON|osg::StateAttribute::PROTECTED);
    compare("uniform_flags_fallback",false);
    candidate=Bucket::build(*source,*camera,work,true);bucket=dynamic_cast<Bucket::Group*>(candidate.get());candidateRoot=root(candidate);
    controlRoot->getOrCreateStateSet()->addUniform(new osg::Uniform("materialColor",osg::Vec4(0,0,1,1)),osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
    candidateRoot->setStateSet(controlRoot->getStateSet());compare("protected_uniform_rebuild",true);
    bucket->invalidate();compare("explicit_invalidation_fallback",false);require(work->invalidations==1,"explicit invalidation not recorded");
    // Different camera/pass and unsupported dynamic/order-sensitive state never
    // create a competing consumer. Root is still the original ordinary graph.
    source->getStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
    require(Bucket::build(*source,*camera,work,true)==source.get(),"blend state entered opaque replacement");
    source->getStateSet()->setMode(GL_BLEND,osg::StateAttribute::OFF);
    children[0]->setDataVariance(osg::Object::DYNAMIC);
    require(Bucket::build(*source,*camera,work,true)==source.get(),"dynamic geometry entered immutable payload");
    children[0]->setDataVariance(osg::Object::STATIC);
    orange->setAttributeAndModes(new osg::Depth(osg::Depth::LEQUAL),osg::StateAttribute::ON);
    require(Bucket::build(*source,*camera,work,true)==source.get(),"depth-tie/order-sensitive state was admitted");
    orange->setAttributeAndModes(new osg::Depth(osg::Depth::LESS),osg::StateAttribute::ON);
    sourceProgram->addBindAttribLocation("unexpected",1);
    require(Bucket::build(*source,*camera,work,true)==source.get(),"program binding recipe was silently stripped");
    sourceProgram->removeBindAttribLocation("unexpected");
    sourceProgram->addTransformFeedBackVarying("gl_Position");
    require(Bucket::build(*source,*camera,work,true)==source.get(),"transform-feedback recipe was silently stripped");
    sourceProgram->removeTransformFeedBackVarying("gl_Position");
    sourceProgram->setProgramBinary(new osg::Program::ProgramBinary);
    require(Bucket::build(*source,*camera,work,true)==source.get(),"program binary recipe was silently stripped");
    sourceProgram->setProgramBinary(nullptr);
    sourceProgram->getShader(0)->setShaderBinary(new osg::ShaderBinary);
    require(Bucket::build(*source,*camera,work,true)==source.get(),"shader binary recipe was silently stripped");
    sourceProgram->getShader(0)->setShaderBinary(nullptr);
    viewer.setSceneData(nullptr);viewer.frame();
    candidateRoot=nullptr;candidate=nullptr;bucket=nullptr;
    // SceneViews retain bounded old leaves until their explicit teardown.
    viewer.getCamera()->setRenderer(nullptr);
    require(work->liveOwnedBytes==0,"retired payload still owns native CPU copies after consumers retire");
    std::cout<<"PASS R1 native material bucket: "<<checks<<" checks; ordinary-vs-bucket exact RGBA/pass leaf oracle; "
        <<"per-view render leaves replaced="<<work->replacedOrdinaryLeaves<<", bucket leaves="<<work->candidateLeaves
        <<", base Geometry setups preserved="<<work->geometrySetupsPreserved
        <<". Native-only; no game consumption, stock/PBR eligibility or FPS claim.\n";
    return 0;
}
catch(const std::exception& error){std::cerr<<"FAIL R1 native material bucket: "<<error.what()<<'\n';return 1;}
