#include <apps/openmw/mwrender/temporaldynamic.hpp>
#include <apps/openmw/mwrender/temporalmotion.hpp>
#include <components/sceneutil/temporalhistorylookup.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <osg/FrameStamp>
#include <osg/GLExtensions>
#include <osg/GraphicsContext>
#include <osg/Image>
#include <osg/Shader>
#include <osgUtil/RenderStage>
#include <osgViewer/Viewer>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#ifdef P9_ASSET_PRODUCER_FIXTURE
#include <components/shader/shadermanager.hpp>
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/settings/settings.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <filesystem>
#include <regex>
#include <cstdlib>
#endif

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string source(const char* path)
{
    std::ifstream input(path); require(input.good(), "production shader missing");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
osg::ref_ptr<osg::Geometry> triangle()
{
    osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
    osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
    positions->push_back({-1,-1,0}); positions->push_back({3,-1,0}); positions->push_back({-1,3,0});
    geometry->setVertexArray(positions);
    osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
    for (unsigned i = 0; i < 3; ++i) normals->push_back({0,0,1});
    geometry->setNormalArray(normals,osg::Array::BIND_PER_VERTEX);
    geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    geometry->setUseDisplayList(false); geometry->setUseVertexBufferObjects(true);
    return geometry;
}
std::shared_ptr<MWRender::TemporalDynamicFrame> capture(osg::Geometry& geometry, osg::Matrixd modelView,
    bool transparent = false, bool shaderCutout = false,
    const std::vector<osg::ref_ptr<osg::StateSet>>& ancestors = {})
{
    osg::ref_ptr<osg::RefMatrix> projection = new osg::RefMatrix;
    osg::ref_ptr<osg::RefMatrix> view = new osg::RefMatrix(modelView);
    osg::ref_ptr<osgUtil::RenderLeaf> leaf = new osgUtil::RenderLeaf(&geometry,projection,view);
    osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
    if (transparent) state->setMode(GL_BLEND,osg::StateAttribute::ON);
    if (shaderCutout)
    {
        state->setMode(GL_ALPHA_TEST,osg::StateAttribute::OFF | osg::StateAttribute::PROTECTED);
        state->setAttribute(new osg::AlphaFunc(osg::AlphaFunc::GREATER,.5f));
    }
    std::vector<osg::ref_ptr<osgUtil::StateGraph>> graphs;
    osg::ref_ptr<osgUtil::StateGraph> graph = new osgUtil::StateGraph; graphs.push_back(graph);
    for (const auto& ancestor : ancestors)
    { graph = new osgUtil::StateGraph(graph,ancestor); graphs.push_back(graph); }
    graph = new osgUtil::StateGraph(graph,state); graphs.push_back(graph);
    if (geometry.getStateSet())
    { graph = new osgUtil::StateGraph(graph,geometry.getStateSet()); graphs.push_back(graph); }
    graph->addLeaf(leaf);
    osg::ref_ptr<osgUtil::RenderStage> bin = new osgUtil::RenderStage;
    bin->getStateGraphList().push_back(graph);
    auto frame = std::make_shared<MWRender::TemporalDynamicFrame>(); frame->capture(*bin);
    return frame;
}
#ifdef P9_ASSET_PRODUCER_FIXTURE
Shader::ShaderManager::DefineMap opaqueDefines(const std::filesystem::path& root)
{
    auto result = Shader::getDefaultDefines();
    const std::regex token(R"(@([A-Za-z_][A-Za-z_0-9]*))"); unsigned files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file()) continue;
        require(++files <= 256,"bounded exact temporal shader inputs");
        std::ifstream input(entry.path()); require(bool(input),"exact temporal shader input unavailable");
        const std::string text{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
        for (auto it=std::sregex_iterator(text.begin(),text.end(),token);it!=std::sregex_iterator();++it)
            result.emplace((*it)[1].str(),"0");
    }
    result["diffuseMap"] = result["normalMap"] = result["specularMap"] = "1";
    result["diffuseMapUV"] = "0"; result["normalMapUV"] = "1"; result["specularMapUV"] = "2";
    result["forcePPL"] = result["lightingMethodClustered"] = result["shadows_enabled"] = "1";
    result["shadowMaps"] = "0,1,2"; result["alphaFunc"] = "519";
    return result;
}
const Shader::AssetGeometry::Samplers opaqueSamplers{{"diffuseMap",0},{"normalMap",1},{"specularMap",2}};
#endif
int main(int argc,char** argv) try
{
    const char* historyLookup = std::getenv("OPENMW_TEMPORAL_HISTORY_LOOKUP_FIXTURE");
    SceneUtil::TemporalHistoryLookup::enabled = historyLookup && std::string_view(historyLookup)=="1";
    require(SceneUtil::temporalDynamicMotionEnabled(), "fixture must enable the independent dynamic producer");
#ifdef P9_ASSET_PRODUCER_FIXTURE
    require(argc == 4,"usage: p9-temporal-asset-producer stockShaderRoot stagedPbrShaderRoot defaultsCfg");
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK","0");
#else
    setenv("OPENMW_P9_ASSET_MASK","0",1);
#endif
    Settings::SettingsFileParser settings;
    settings.loadSettingsFile(std::filesystem::u8path(argv[3]),Settings::Manager::mDefaultSettings);
    Settings::StaticValues::initDefaults(); Settings::Manager::mUserSettings = Settings::Manager::mDefaultSettings;
    Settings::StaticValues::init();
    const auto stock = std::filesystem::u8path(argv[1]), pbr = std::filesystem::u8path(argv[2]);
    require(std::filesystem::is_directory(stock) && std::filesystem::is_directory(pbr),"both exact temporal shader families mandatory");
#else
    (void)argc; (void)argv;
#endif
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32; traits->height = 32; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(), "real GL context missing");
    auto* state = context->getState();
    osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp; state->setFrameStamp(stamp);
    osg::RenderInfo info(state,nullptr);
    auto program = [](const char* vertex,const char* fragment) {
        osg::ref_ptr<osg::Program> value = new osg::Program;
        value->addShader(new osg::Shader(osg::Shader::VERTEX,source(vertex)));
        value->addShader(new osg::Shader(osg::Shader::FRAGMENT,source(fragment)));
        return value;
    };
    auto cameraProgram = program(P9_VERTEX,P9_FRAGMENT);
    auto dynamicProgram = program(P9_DYNAMIC_VERTEX,P9_DYNAMIC_FRAGMENT);
    MWRender::TemporalMotion pass(cameraProgram,dynamicProgram);
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->allocateImage(16,16,1,GL_RED,GL_FLOAT);
    std::fill(reinterpret_cast<float*>(image->data()),reinterpret_cast<float*>(image->data())+256,.5f);
    osg::ref_ptr<osg::Texture2D> depth = new osg::Texture2D(image);
    depth->setTextureSize(16,16); depth->setInternalFormat(GL_R32F); depth->setResizeNonPowerOfTwoHint(false);
    depth->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
    depth->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
    MWRender::TemporalCamera camera;
    camera.projection = new osg::RefMatrix; camera.renderWidth = camera.renderHeight = 16;
    camera.outputWidth = camera.outputHeight = 16;
    auto fullscreen = triangle();
    auto draw = [&](unsigned frame,float expected,unsigned submitted) {
        camera.frame = frame; stamp->setFrameNumber(frame);
        auto* motion = pass.render(info,camera,depth,*fullscreen);
        require(motion, "production dynamic motion pass failed");
        const auto status = pass.status(state->getContextID());
        require(status.dynamicSurfaces == submitted && !status.denseDynamicMotion,
            "dynamic coverage was lost or incomplete coverage claimed complete");
        state->applyTextureAttribute(0,motion);
        std::array<float,512> values{}; glGetTexImage(GL_TEXTURE_2D,0,GL_RG,GL_FLOAT,values.data());
        const unsigned center = (8*16+8)*2;
        if (std::abs(values[center]-expected) > .003f || std::abs(values[center+1]) > .003f)
        {
            std::cerr << "frame=" << frame << " actual=" << values[center] << ',' << values[center+1]
                << " expected=" << expected << ",0\n";
            throw std::runtime_error("actual current/previous actor/rigid motion pixel mismatch");
        }
        require(glGetError() == GL_NO_ERROR, "dynamic overlay generated GL errors");
    };

    // Actual production RigGeometry + Skeleton CPU skinning, not a mock pose.
    osg::ref_ptr<SceneUtil::Skeleton> skeleton = new SceneUtil::Skeleton;
    osg::ref_ptr<osg::MatrixTransform> bone = new osg::MatrixTransform; bone->setName("bone");
    skeleton->addChild(bone);
    osg::ref_ptr<SceneUtil::RigGeometry> rig = new SceneUtil::RigGeometry;
    auto rigSource = triangle();
    osg::ref_ptr<SceneUtil::TemporalMotionIdentity> templateIdentity = new SceneUtil::TemporalMotionIdentity;
    SceneUtil::installTemporalMotionIdentity(*rigSource,templateIdentity);
    rig->setSourceGeometry(rigSource);
    SceneUtil::RigGeometry::BoneInfo boneInfo;
    boneInfo.mName = "bone"; boneInfo.mBoundSphere.set({0,0,0},4);
    rig->setBoneInfo({boneInfo});
    rig->setInfluences(std::vector<SceneUtil::RigGeometry::BoneWeights>(3,{{0,1.0f}}));
    skeleton->addChild(rig);
    osg::NodePath path{skeleton,rig};
    auto* rigFirst = rig->evaluateGeometry(1,path); require(rigFirst, "production rig evaluation unavailable");
    osg::ref_ptr<SceneUtil::RigGeometry> anotherRig = new SceneUtil::RigGeometry(*rig,osg::CopyOp::SHALLOW_COPY);
    skeleton->addChild(anotherRig);
    auto* anotherGeometry = anotherRig->evaluateGeometry(1,osg::NodePath{skeleton,anotherRig});
    require(anotherGeometry && SceneUtil::temporalMotionIdentity(*rigFirst)
        != SceneUtil::temporalMotionIdentity(*anotherGeometry), "cloned actor inherited another instance identity");
    require(SceneUtil::temporalMotionIdentity(*rigSource) == templateIdentity.get(),
        "actor identity installation mutated the shared source userdata");
    camera.dynamic = capture(*rigFirst,osg::Matrix::identity()); draw(1,0,1);
    bone->setMatrix(osg::Matrix::translate(.125f,0,0));
    auto* rigSecond = rig->evaluateGeometry(2,path); require(rigSecond, "production rig update unavailable");
    require(SceneUtil::temporalMotionIdentity(*rigFirst) == SceneUtil::temporalMotionIdentity(*rigSecond),
        "actor identity followed private frame parity instead of the rig instance");
    camera.dynamic = capture(*rigSecond,osg::Matrix::identity());
    // Mutating the next CPU pose before consuming the captured frame must not
    // change that draw's immutable positions or its submitted previous pose.
    bone->setMatrix(osg::Matrix::translate(.5f,0,0)); rig->evaluateGeometry(3,path);
    draw(2,-1,1);

    // Actual production MorphGeometry: moving vertices with a static camera.
    osg::ref_ptr<SceneUtil::MorphGeometry> morph = new SceneUtil::MorphGeometry;
    auto morphSource = triangle();
    morph->setSourceGeometry(morphSource);
    morph->addMorphTarget(static_cast<osg::Vec3Array*>(morphSource->getVertexArray()),1);
    osg::ref_ptr<osg::Vec3Array> offsets = new osg::Vec3Array;
    for (unsigned i = 0; i < 3; ++i) offsets->push_back(osg::Vec3(.125f,0,0));
    morph->addMorphTarget(offsets,0);
    camera.dynamic = capture(*morph->evaluateGeometry(4),osg::Matrix::identity());
    ++camera.worldEpoch; draw(4,0,1);
    morph->getMorphTarget(1).setWeight(1); morph->dirty();
    camera.dynamic = capture(*morph->evaluateGeometry(5),osg::Matrix::identity()); draw(5,-1,1);

    // Rigid model transform motion is distinct from camera depth reprojection.
    auto rigid = triangle();
    camera.dynamic = capture(*rigid,osg::Matrix::identity()); ++camera.worldEpoch; draw(6,0,1);
    camera.dynamic = capture(*rigid,osg::Matrix::translate(.125,0,0)); draw(7,-1,1);
    camera.dynamic = capture(*rigid,osg::Matrix::translate(.25,0,0)); draw(9,0,1); // skipped frame
    camera.dynamic = capture(*rigid,osg::Matrix::translate(.375,0,0),true);
    require(camera.dynamic->unsupported == 1, "transparent geometry was silently treated as opaque");
    draw(10,0,0);
    camera.dynamic = capture(*rigid,osg::Matrix::translate(.5,0,0)); draw(11,0,1); // newly visible history
    std::fill(reinterpret_cast<float*>(image->data()),reinterpret_cast<float*>(image->data())+256,.25f); image->dirty();
    camera.dynamic = capture(*rigid,osg::Matrix::translate(.625,0,0)); draw(12,0,1); // foreground occludes overlay
    camera.dynamic = capture(*rigid,osg::Matrix::identity(),false,true);
    require(camera.dynamic->surfaces.empty() && camera.dynamic->unsupported == 1,
        "shader cutout with disabled fixed-function alpha test was silently treated as opaque");
    draw(13,0,0);
    auto patches = triangle(); patches->getPrimitiveSet(0)->setMode(0x000E); // GL_PATCHES
    auto patchCapture = capture(*patches,osg::Matrix::identity());
    require(patchCapture->surfaces.empty() && patchCapture->unsupported == 1,
        "patch primitives were admitted without their tessellation contract");
    struct CustomGeometry : osg::Geometry
    {
        explicit CustomGeometry(const osg::Geometry& geometry) : osg::Geometry(geometry) {}
    };
    osg::ref_ptr<CustomGeometry> custom = new CustomGeometry(*rigid);
    auto customCapture = capture(*custom,osg::Matrix::identity());
    require(customCapture->surfaces.empty() && customCapture->unsupported == 1,
        "unknown Geometry subclass was qualified by its inherited class name");
    struct CustomTriangles : osg::DrawArrays { CustomTriangles() : osg::DrawArrays(GL_TRIANGLES,0,3) {} };
    auto customPrimitive = triangle(); customPrimitive->setPrimitiveSet(0,new CustomTriangles);
    auto primitiveCapture = capture(*customPrimitive,osg::Matrix::identity());
    require(primitiveCapture->surfaces.empty() && primitiveCapture->unsupported == 1,
        "unknown primitive subclass was copied without its draw ownership contract");
    auto badIndices = triangle();
    osg::ref_ptr<osg::DrawElementsUInt> indices = new osg::DrawElementsUInt(GL_TRIANGLES);
    indices->push_back(0); indices->push_back(1); indices->push_back(999);
    badIndices->setPrimitiveSet(0,indices);
    auto invalidCapture = capture(*badIndices,osg::Matrix::identity());
    require(invalidCapture->surfaces.empty() && invalidCapture->unsupported == 1,
        "out-of-range primitive vertex index was admitted");
    auto excessPrimitives = triangle();
    for (unsigned i = 0; i < 64; ++i) excessPrimitives->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    require(capture(*excessPrimitives,osg::Matrix::identity())->surfaces.empty(),
        "primitive container population exceeded the bounded pose snapshot cap");

    osg::ref_ptr<osg::Geometry> excessive = new osg::Geometry;
    excessive->setVertexArray(new osg::Vec3Array(400000));
    const auto rejected = capture(*excessive,osg::Matrix::identity());
    require(rejected->surfaces.empty() && rejected->bytes == 0 && rejected->unsupported == 1,
        "oversized speculative pose copied outside the fixed admission ceiling");
    // An unproved lineage is never temporal material authorization.
    auto originalTier=triangle();
    osg::ref_ptr<osg::Geometry> derivedTier=new osg::Geometry(*originalTier,osg::CopyOp::SHALLOW_COPY);
    osg::ref_ptr<SceneUtil::TemporalVertexLineage> lineage=new SceneUtil::TemporalVertexLineage(originalTier->getVertexArray());
    SceneUtil::installTemporalVertexLineage(*originalTier,lineage);
    SceneUtil::installTemporalVertexLineage(*derivedTier,lineage);
    osg::ref_ptr<osg::DrawElementsUInt> changedTopology=new osg::DrawElementsUInt(GL_TRIANGLES);
    for(unsigned index:{0,1,2,0,1,2}) changedTopology->push_back(index);
    derivedTier->setPrimitiveSet(0,changedTopology);
    require(capture(*originalTier,osg::Matrix::identity())->surfaces.empty()
        && capture(*derivedTier,osg::Matrix::identity())->surfaces.empty(),"lineage without an exact opaque producer was accepted");
#ifdef P9_ASSET_PRODUCER_FIXTURE
    std::fill(reinterpret_cast<float*>(image->data()),reinterpret_cast<float*>(image->data())+256,.5f); image->dirty();
    unsigned assetFrame = 30;
    const auto metadataBefore = Shader::AssetGeometry::liveMetadataBytes();
    for (const auto& [root,family] : {std::pair{stock,Shader::AssetGeometry::Family::Stock},std::pair{pbr,Shader::AssetGeometry::Family::Pbr}})
    {
        Shader::ShaderManager manager; manager.setShaderPath(root); auto globals=opaqueDefines(root); manager.setGlobalDefines(globals);
        auto actual=manager.getProgram("compatibility/objects",{},nullptr,opaqueSamplers);
        require(actual && Shader::AssetGeometry::matches(*actual),"actual stock/PBR temporal provenance unavailable");
        require(!Shader::IndirectProjection::matches(*actual),"temporal-only coverage widened indirect authorization");
        auto canonical=triangle(); canonical->setDataVariance(osg::Object::STATIC);
        canonical->getOrCreateStateSet()->setAttribute(actual);
        canonical->getStateSet()->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf::identity()));
        osg::ref_ptr<SceneUtil::TemporalVertexLineage> identity=new SceneUtil::TemporalVertexLineage(canonical->getVertexArray());
        SceneUtil::installTemporalVertexLineage(*canonical,identity);
        require(SceneUtil::GeneratedStaticLod::installOriginalDrawReceipts(*canonical),"actual delegating original receipt unavailable");
        osg::ref_ptr<osg::Geometry> reduced=new osg::Geometry(*canonical,osg::CopyOp::SHALLOW_COPY);
        osg::ref_ptr<osg::DrawElementsUInt> tierIndices=new osg::DrawElementsUInt(GL_TRIANGLES);
        for(unsigned index:{0,1,2,0,1,2}) tierIndices->push_back(index); reduced->setPrimitiveSet(0,tierIndices);
        SceneUtil::installTemporalVertexLineage(*reduced,identity);
        camera.dynamic=capture(*canonical,osg::Matrix::identity()); ++camera.worldEpoch;
        require(camera.dynamic->surfaces.size()==1,"approved canonical stock/PBR source was not captured");
        draw(assetFrame++,0,1); const auto prior=camera.dynamic;
        camera.dynamic=capture(*reduced,osg::Matrix::translate(.125,0,0));
        require(camera.dynamic->surfaces.size()==1 && camera.dynamic->surfaces[0].topology!=prior->surfaces[0].topology
            && camera.dynamic->surfaces[0].instance==prior->surfaces[0].instance
            && camera.dynamic->surfaces[0].compatibleHistory(prior->surfaces[0]),"index-only tier lost canonical previous-pose identity");
        draw(assetFrame++,-1,1); const auto tier=camera.dynamic;
        camera.dynamic=capture(*canonical,osg::Matrix::translate(.25,0,0));
        require(camera.dynamic->surfaces.size()==1 && camera.dynamic->surfaces[0].compatibleHistory(tier->surfaces[0]),
            "returning canonical tier lost valid vertex history"); draw(assetFrame++,-1,1);
        const auto live=camera.dynamic;
        auto* shader=actual->getShader(0); const auto originalSource=shader->getShaderSource();
        shader->setShaderSource(originalSource+"\n// unreviewed temporal producer");
        require(!live->surfaces[0].materialCurrent() && capture(*reduced,osg::Matrix::identity())->surfaces.empty(),
            "changed source was accepted by temporal proof"); shader->setShaderSource(originalSource);
        actual->addBindAttribLocation("unreviewed",13);
        require(!live->surfaces[0].materialCurrent() && capture(*reduced,osg::Matrix::identity())->surfaces.empty(),
            "changed actual linking binding was accepted"); actual->removeBindAttribLocation("unreviewed");
        canonical->getStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
        require(!live->surfaces[0].materialCurrent() && capture(*reduced,osg::Matrix::identity())->surfaces.empty(),
            "captured effective material mutation kept temporal authorization");
        camera.dynamic=live; draw(assetFrame++,0,0);
        canonical->getStateSet()->removeMode(GL_BLEND);
        struct UnprovedCallback final : osg::Drawable::DrawCallback
        {void drawImplementation(osg::RenderInfo&,const osg::Drawable*) const override {}};
        const auto callback=reduced->getDrawCallback(); reduced->setDrawCallback(new UnprovedCallback);
        require(capture(*reduced,osg::Matrix::identity())->surfaces.empty()
            && !tier->surfaces[0].materialCurrent(),"replaced unsupported draw callback acquired temporal coverage");
        reduced->setDrawCallback(callback);
        tierIndices->dirty();
        require(!tier->surfaces[0].materialCurrent(),"mutated current tier indices kept captured coverage");
        osg::ref_ptr<osg::Program> impostor=new osg::Program;
        osg::ref_ptr<osg::Shader> named=new osg::Shader(osg::Shader::VERTEX,"void main(){}"); named->setName("objects.vert");
        impostor->addShader(named);
        osg::ref_ptr<osg::StateSet> parent=new osg::StateSet;
        parent->setAttribute(actual,osg::StateAttribute::OVERRIDE);
        reduced->getStateSet()->setAttribute(impostor);
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.size()==1,
            "ignored child Program defeated actual parent override");
        reduced->getStateSet()->setAttribute(impostor,osg::StateAttribute::PROTECTED);
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.empty(),
            "protected names-only impostor Program was accepted"); reduced->getStateSet()->setAttribute(actual);
        parent->removeAttribute(osg::StateAttribute::PROGRAM);
        parent->setMode(GL_BLEND,osg::StateAttribute::OFF|osg::StateAttribute::OVERRIDE);
        reduced->getStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON);
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.size()==1,
            "ignored child blend ON defeated opaque parent override");
        reduced->getStateSet()->setMode(GL_BLEND,osg::StateAttribute::ON|osg::StateAttribute::PROTECTED);
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.empty(),"protected transparency was accepted");
        parent->setMode(GL_BLEND,osg::StateAttribute::ON|osg::StateAttribute::OVERRIDE);
        reduced->getStateSet()->setMode(GL_BLEND,osg::StateAttribute::OFF);
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.empty(),"ignored child opaque mode hid actual blending");
        reduced->getStateSet()->setMode(GL_BLEND,osg::StateAttribute::OFF|osg::StateAttribute::PROTECTED);
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.size()==1,"protected opaque mode was not resolved");
        parent->removeMode(GL_BLEND); reduced->getStateSet()->removeMode(GL_BLEND);
        parent->setAttribute(new osg::AlphaFunc(osg::AlphaFunc::ALWAYS,0.f),osg::StateAttribute::OVERRIDE);
        reduced->getStateSet()->setAttribute(new osg::AlphaFunc(osg::AlphaFunc::GREATER,.5f));
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.size()==1,"ignored child AlphaFunc was applied");
        reduced->getStateSet()->setAttribute(new osg::AlphaFunc(osg::AlphaFunc::GREATER,.5f),osg::StateAttribute::PROTECTED);
        require(capture(*reduced,osg::Matrix::identity(),false,false,{parent})->surfaces.empty(),"protected shader cutout was accepted");
        reduced->getStateSet()->removeAttribute(osg::StateAttribute::ALPHAFUNC); parent->removeAttribute(osg::StateAttribute::ALPHAFUNC);
        const auto stale=capture(*reduced,osg::Matrix::identity()); require(stale->surfaces.size()==1,"epoch fixture setup");
        globals["clamp"]=globals["clamp"]=="1"?"0":"1"; manager.setGlobalDefines(globals);
        require(!stale->surfaces[0].materialCurrent() && capture(*reduced,osg::Matrix::identity())->surfaces.size()==1,
            "fresh exact producer or stale immutable snapshot epoch handling failed");
        globals["alphaFunc"]="516"; manager.setGlobalDefines(globals);
        require(Shader::AssetGeometry::matches(*actual) && capture(*reduced,osg::Matrix::identity())->surfaces.empty(),
            "shader-owned cutout was mistaken for opaque source provenance");
        globals["alphaFunc"]="519"; manager.setGlobalDefines(globals);
        globals["alphaToCoverage"]="1"; manager.setGlobalDefines(globals);
        require(Shader::AssetGeometry::matches(*actual) && capture(*reduced,osg::Matrix::identity())->surfaces.empty(),
            "shader-owned alpha-to-coverage was accepted"); globals["alphaToCoverage"]="0"; manager.setGlobalDefines(globals);
        globals["parallax"]="1"; manager.setGlobalDefines(globals);
        require(Shader::AssetGeometry::matches(*actual) && capture(*reduced,osg::Matrix::identity())->surfaces.empty(),
            "unproved parallax feature acquired opaque temporal coverage"); globals["parallax"]="0"; manager.setGlobalDefines(globals);
        parent->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf::scale(2,1,1)),osg::StateAttribute::OVERRIDE);
        camera.dynamic=capture(*canonical,osg::Matrix::identity(),false,false,{parent}); ++camera.worldEpoch;
        require(camera.dynamic->surfaces.size()==1 && (*camera.dynamic->surfaces[0].projection)(0,0)==2,
            "actual shader GPU projection was replaced by OSG culling projection"); draw(assetFrame++,0,1);
        camera.dynamic=capture(*reduced,osg::Matrix::translate(.125,0,0),false,false,{parent}); draw(assetFrame++,-2,1);
        const auto captured=camera.dynamic;
        parent->getUniform("projectionMatrix")->set(osg::Matrixf::scale(3,1,1));
        require((*captured->surfaces[0].projection)(0,0)==2 && !captured->surfaces[0].materialCurrent()
            && captured->surfaces[0].materialCurrent(false),
            "current projection mutation was accepted or frozen previous history was invalidated");
        camera.dynamic=captured; draw(assetFrame++,0,0);
        const auto currentProjection=capture(*canonical,osg::Matrix::translate(.25,0,0),false,false,{parent});
        require(currentProjection->surfaces.size()==1
            && currentProjection->surfaces[0].compatibleHistory(captured->surfaces[0]),
            "normal next-frame projection change lost immutable previous-pose history");
        auto* ownedProjection=canonical->getStateSet()->getUniform("projectionMatrix");
        osg::ref_ptr<osg::Uniform> savedProjection=ownedProjection;
        canonical->getStateSet()->removeUniform("projectionMatrix"); parent->removeUniform("projectionMatrix");
        require(capture(*canonical,osg::Matrix::identity())->surfaces.empty(),
            "program-backed source without actual projection uniform was accepted");
        canonical->getStateSet()->addUniform(savedProjection);
        reduced->getVertexArray()->dirty();
        require(!captured->surfaces[0].compatibleHistory(prior->surfaces[0])
            && capture(*reduced,osg::Matrix::identity())->surfaces.empty(),"stale canonical vertex lineage was accepted");
        std::cout << "TEMPORAL_ASSET_FAMILY family=" << Shader::AssetGeometry::familyName(family)
            << " source=1 tier_switch=1 tier_return=1 motion_pixels=1 previous_pose=1 effective_program=1 blend_override=1"
            << " alpha_override=1 shader_cutout=1 source_mutation=1 binding_mutation=1 material_mutation=1 callback=1"
            << " epoch=1 projection=1 projection_mutation=1 previous_projection=1 missing_projection=1 stale_lineage=1 indirect_unchanged=1\n";
    }
    camera.dynamic.reset(); ++camera.worldEpoch; draw(assetFrame++,0,0);
    require(Shader::AssetGeometry::liveMetadataBytes()==metadataBefore,"temporal producer snapshots retained metadata after release");
    std::cout << "TEMPORAL_ASSET_PASS stock=1 pbr=1 index_lineage=1 real_motion_pixels=1 opaque_only=1 current_material=1"
        << " bounded=1 no_dense_claim=1 no_dlss_claim=1 indirect_unchanged=1 temporal_only=1\n";
#endif
    if (SceneUtil::TemporalHistoryLookup::enabled.load())
    {
        require(SceneUtil::TemporalHistoryLookup::indexedLookups.load()>0
            && SceneUtil::TemporalHistoryLookup::compatibilityComparisons.load()>0,
            "indexed fixture did not consume actual previous-pose history");
        std::cout << "TEMPORAL_HISTORY_PIXELS_PASS indexed_lookups="
            << SceneUtil::TemporalHistoryLookup::indexedLookups.load()
            << " comparisons=" << SceneUtil::TemporalHistoryLookup::compatibilityComparisons.load() << "\n";
    }
    pass.releaseGLObjects(state); context->releaseContext(); context->close(true);
    std::cout << "PASS: production rigid transforms, real CPU rig/morph previous/current poses, unproved lineage refusal, immutable capture, gap reset, transparent/shader-cutout fallback, depth visibility and bounded memory\n";
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
