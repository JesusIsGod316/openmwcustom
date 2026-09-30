#include <apps/openmw/mwrender/temporaldynamic.hpp>
#include <apps/openmw/mwrender/temporalmotion.hpp>
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
    bool transparent = false, bool shaderCutout = false)
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
    osg::ref_ptr<osgUtil::StateGraph> graph = new osgUtil::StateGraph;
    graph->setStateSet(state); graph->addLeaf(leaf);
    osg::ref_ptr<osgUtil::RenderStage> bin = new osgUtil::RenderStage;
    bin->getStateGraphList().push_back(graph);
    auto frame = std::make_shared<MWRender::TemporalDynamicFrame>(); frame->capture(*bin);
    return frame;
}
int main() try
{
    require(SceneUtil::temporalDynamicMotionEnabled(), "fixture must enable the independent dynamic producer");
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

    osg::ref_ptr<osg::Geometry> excessive = new osg::Geometry;
    excessive->setVertexArray(new osg::Vec3Array(400000));
    const auto rejected = capture(*excessive,osg::Matrix::identity());
    require(rejected->surfaces.empty() && rejected->bytes == 0 && rejected->unsupported == 1,
        "oversized speculative pose copied outside the fixed admission ceiling");
    pass.releaseGLObjects(state); context->releaseContext(); context->close(true);
    std::cout << "PASS: production rigid transforms, real CPU rig/morph previous/current poses, immutable capture, gap reset, transparent/shader-cutout fallback, depth visibility and bounded memory\n";
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
