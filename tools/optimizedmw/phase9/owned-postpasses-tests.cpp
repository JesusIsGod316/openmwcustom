#include <apps/openmw/mwrender/depthclear.hpp>
#include <apps/openmw/mwrender/distortion.hpp>
#include <apps/openmw/mwrender/transparentpass.hpp>
#include <components/stereo/multiview.hpp>
#include <osg/Camera>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/Shader>
#include <osg/Viewport>
#include <osg/observer_ptr>
#include <osgUtil/RenderLeaf>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>
#include <osgViewer/Viewer>
#include <array>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace
{
    struct Unsupported : std::runtime_error { using std::runtime_error::runtime_error; };
    void require(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    constexpr osg::StateAttribute::OverrideValue Protected
        = osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE | osg::StateAttribute::PROTECTED;

    struct Target
    {
        unsigned width = 0, height = 0;
        osg::ref_ptr<osg::Texture2D> color, depth;
        osg::ref_ptr<osg::FrameBufferObject> framebuffer;
    };

    osg::ref_ptr<osg::Texture2D> texture(unsigned width, unsigned height, bool depth)
    {
        osg::ref_ptr<osg::Texture2D> result = new osg::Texture2D;
        result->setTextureSize(width, height);
        result->setResizeNonPowerOfTwoHint(false);
        result->setInternalFormat(depth ? GL_DEPTH24_STENCIL8 : GL_RGBA8);
        result->setSourceFormat(depth ? GL_DEPTH_STENCIL_EXT : GL_RGBA);
        result->setSourceType(depth ? GL_UNSIGNED_INT_24_8_EXT : GL_UNSIGNED_BYTE);
        result->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
        result->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
        return result;
    }

    Target target(unsigned width, unsigned height, bool color, bool depth,
        osg::Texture2D* sharedColor = nullptr)
    {
        Target result;
        result.width = width; result.height = height;
        result.framebuffer = new osg::FrameBufferObject;
        if (color)
        {
            result.color = sharedColor ? sharedColor : texture(width,height,false).get();
            result.framebuffer->setAttachment(osg::Camera::COLOR_BUFFER0,osg::FrameBufferAttachment(result.color));
        }
        if (depth)
        {
            result.depth = texture(width,height,true);
            result.framebuffer->setAttachment(osg::Camera::PACKED_DEPTH_STENCIL_BUFFER,
                osg::FrameBufferAttachment(result.depth));
        }
        return result;
    }

    void bind(osg::State& state, const Target& value)
    {
        value.framebuffer->apply(state);
        glDrawBuffer(value.color ? GL_COLOR_ATTACHMENT0_EXT : GL_NONE);
        glReadBuffer(value.color ? GL_COLOR_ATTACHMENT0_EXT : GL_NONE);
        require(state.get<osg::GLExtensions>()->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT)
            == GL_FRAMEBUFFER_COMPLETE_EXT,"fixture framebuffer incomplete");
        glViewport(0,0,value.width,value.height);
    }

    void clear(osg::State& state, const Target& value, const osg::Vec4& color, float depth)
    {
        bind(state,value);
        glColorMask(true,true,true,true); glDepthMask(true); glStencilMask(~0u);
        glClearColor(color.r(),color.g(),color.b(),color.a()); glClearDepth(depth); glClearStencil(7);
        GLbitfield mask = value.color ? GL_COLOR_BUFFER_BIT : 0;
        if (value.depth) mask |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        glClear(mask); glClearStencil(0);
        state.haveAppliedAttribute(osg::StateAttribute::COLORMASK);
        state.haveAppliedAttribute(osg::StateAttribute::DEPTH);
    }

    void colorPixels(osg::State& state, const Target& value, const osg::Vec4& expected)
    {
        bind(state,value);
        std::vector<unsigned char> pixels(value.width * value.height * 4);
        glReadPixels(0,0,value.width,value.height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
        for (std::size_t i = 0; i < pixels.size(); ++i)
            require(std::abs(static_cast<int>(pixels[i]) - static_cast<int>(std::lround(expected[i%4]*255))) <= 1,
                "owned postpass color differs from submitted resource generation");
    }

    void depthPixels(osg::State& state, const Target& value, float expected, unsigned char expectedStencil)
    {
        bind(state,value);
        std::vector<float> pixels(value.width * value.height);
        glReadPixels(0,0,value.width,value.height,GL_DEPTH_COMPONENT,GL_FLOAT,pixels.data());
        for (float actual : pixels)
            require(std::abs(actual - expected) < .00001f,"owned postpass depth/blit differs from submission");
        std::vector<unsigned char> stencil(pixels.size());
        glReadPixels(0,0,value.width,value.height,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,stencil.data());
        for (auto actual : stencil)
            if (actual != expectedStencil)
            {
                std::cerr << "stencil actual=" << static_cast<unsigned>(actual)
                    << " expected=" << static_cast<unsigned>(expectedStencil)
                    << " fbo=" << value.framebuffer->getHandle(state.getContextID()) << '\n';
                throw std::runtime_error("owned postpass unexpectedly cleared/copied stencil");
            }
    }

    void restored(osg::State& state, const Target& primary)
    {
        GLint draw = 0, read = 0;
        std::array<GLint,4> viewport{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT,&draw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT,&read);
        glGetIntegerv(GL_VIEWPORT,viewport.data());
        const auto handle = primary.framebuffer->getHandle(state.getContextID());
        require(draw == static_cast<GLint>(handle) && read == static_cast<GLint>(handle),
            "owned postpass did not restore submitted primary framebuffer");
        require(viewport == std::array<GLint,4>{0,0,static_cast<GLint>(primary.width),static_cast<GLint>(primary.height)},
            "owned postpass did not restore submitted viewport");
        require(glGetError() == GL_NO_ERROR,"owned postpass generated GL errors");
    }

    osg::ref_ptr<osg::Program> program(bool depthOnly)
    {
        osg::ref_ptr<osg::Program> result = new osg::Program;
        result->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\nvoid main(){gl_Position=gl_Vertex;}\n"));
        result->addShader(new osg::Shader(osg::Shader::FRAGMENT,depthOnly
            ? "#version 120\nuniform float drawDepth;void main(){gl_FragDepth=drawDepth;}\n"
            : "#version 120\nuniform float drawDepth;uniform vec4 drawColor;"
              "void main(){gl_FragDepth=drawDepth;gl_FragColor=drawColor;}\n"));
        return result;
    }

    struct Bin
    {
        osg::ref_ptr<osgUtil::RenderStage> stage = new osgUtil::RenderStage;
        osg::ref_ptr<osgUtil::StateGraph> root = new osgUtil::StateGraph;
        osg::ref_ptr<osgUtil::StateGraph> graph;
        osg::ref_ptr<osg::Geometry> geometry = new osg::Geometry;
        osg::ref_ptr<osgUtil::RenderLeaf> leaf;
        osgUtil::RenderBin* bin = nullptr;
        osg::ref_ptr<osg::StateSet> stateSet = new osg::StateSet;
        osg::ref_ptr<osg::StateSet> base = new osg::StateSet;

        Bin(osg::Program* shader, const Target& primary, const osg::Vec4& color, float depth)
        {
            geometry->setUseDisplayList(false); geometry->setUseVertexBufferObjects(true);
            geometry->setDataVariance(osg::Object::STATIC);
            osg::ref_ptr<osg::Vec3Array> vertices = new osg::Vec3Array;
            vertices->push_back({-1,-1,0}); vertices->push_back({3,-1,0}); vertices->push_back({-1,3,0});
            geometry->setVertexArray(vertices);
            geometry->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
            stateSet->setAttributeAndModes(shader);
            stateSet->addUniform(new osg::Uniform("drawColor",color));
            stateSet->addUniform(new osg::Uniform("drawDepth",depth));
            base->setAttributeAndModes(new osg::Viewport(0,0,primary.width,primary.height));
            base->setAttributeAndModes(new osg::ColorMask(true,true,true,true));
            base->setMode(GL_DEPTH_TEST,osg::StateAttribute::ON);
            base->setMode(GL_BLEND,osg::StateAttribute::OFF);
            base->setMode(GL_CULL_FACE,osg::StateAttribute::OFF);
            base->setMode(GL_SCISSOR_TEST,osg::StateAttribute::OFF);
            base->setMode(GL_STENCIL_TEST,osg::StateAttribute::OFF);
            base->setMode(GL_ALPHA_TEST,osg::StateAttribute::OFF);
            base->setAttributeAndModes(new SceneUtil::AutoDepth);
            stage->setFrameBufferObject(primary.framebuffer);
            stage->setViewport(new osg::Viewport(0,0,primary.width,primary.height));
            bin = stage->find_or_insert(1,"RenderBin");
            graph = new osgUtil::StateGraph(root,stateSet);
            leaf = new osgUtil::RenderLeaf(geometry,new osg::RefMatrix,new osg::RefMatrix);
            graph->addLeaf(leaf);
            bin->getRenderLeafList().push_back(leaf);
        }

        void draw(osgUtil::RenderBin::DrawCallback& callback, osg::RenderInfo& info)
        {
            auto& state = *info.getState();
            state.pushStateSet(base); state.apply();
            osgUtil::RenderLeaf* previous = nullptr;
            callback.drawImplementation(bin,info,previous);
            // Preserve the callback's resulting actual bindings/viewport for
            // assertions. StateGraph changes only unwind after those checks.
        }
    };

    void unwind(osg::State& state)
    {
        state.popAllStateSets(); state.apply();
    }

    void callbackRetains(std::initializer_list<Target*> targets)
    {
        std::vector<osg::observer_ptr<osg::FrameBufferObject>> observers;
        for (auto* value : targets) observers.emplace_back(value->framebuffer);
        for (auto* value : targets) value->framebuffer = nullptr;
        std::size_t i = 0;
        for (auto* value : targets)
        {
            require(observers[i].valid(),"owned callback did not retain a replaced framebuffer generation");
            value->framebuffer = observers[i++].get();
        }
    }

    MWRender::DepthClearCallback::Resources forbiddenDepthProvider(osg::RenderInfo&)
    {
        throw std::runtime_error("owned depth-clear consulted legacy camera userdata provider");
    }
    osg::ref_ptr<osg::FrameBufferObject> forbiddenPrimaryProvider(osg::RenderInfo&)
    {
        throw std::runtime_error("owned distortion consulted legacy camera userdata provider");
    }

    void generation(osg::State& state, osg::RenderInfo& info, unsigned sequence, unsigned frame,
        unsigned width, unsigned height, osg::Program* colorProgram, osg::StateSet* depthOnly)
    {
        const osg::Vec4 sentinel(.125f,.875f,.25f,1.f);
        const osg::Vec4 initial(.05f,.1f,.15f,1.f);
        const osg::Vec4 submitted(.25f,.5f,.25f + .125f*sequence,1.f);
        auto primary = target(width,height,true,true);
        auto firstPerson = target(width,height,true,true,primary.color);
        auto opaque = target(width,height,false,true);
        auto distortion = target(std::max(1u,width/4),std::max(1u,height/4),true,false);
        auto replacement = target(width+3,height+2,true,true);
        clear(state,replacement,sentinel,.9f);

        {
            osg::ref_ptr<MWRender::DepthClearCallback> depthSource = new MWRender::DepthClearCallback(forbiddenDepthProvider);
            auto depth = depthSource->ownedFrame(firstPerson.framebuffer,opaque.framebuffer,primary.framebuffer);
            // Reset all source handles before draw. Only callback-owned references
            // identify this submission; frame parity and live userdata are absent.
            depthSource = nullptr;
            callbackRetains({&firstPerson,&opaque,&primary});
            clear(state,primary,initial,.8f); clear(state,opaque,initial,.8f);
            clear(state,firstPerson,initial,.8f);
            bind(state,primary);
            Bin first(colorProgram,primary,submitted,.25f);
            first.draw(*depth,info); restored(state,primary); unwind(state);
            colorPixels(state,primary,submitted);
            depthPixels(state,primary,.8f,7);
            depthPixels(state,firstPerson,.25f,0);
            depthPixels(state,opaque,.25f,7);
            colorPixels(state,replacement,sentinel); depthPixels(state,replacement,.9f,7);
        }

        for (const bool postPass : {false,true})
        {
            osg::ref_ptr<MWRender::TransparentDepthBinCallback> source
                = new MWRender::TransparentDepthBinCallback(depthOnly,postPass);
            const auto parity = frame%2;
            source->mFbo[parity] = primary.framebuffer; source->mOpaqueFbo[parity] = opaque.framebuffer;
            auto owned = source->ownedFrame(parity);
            source->mFbo[parity] = replacement.framebuffer;
            source->mOpaqueFbo[parity] = replacement.framebuffer;
            source = nullptr;
            callbackRetains({&primary,&opaque});
            clear(state,primary,initial,.6f); clear(state,opaque,initial,.8f); bind(state,primary);
            Bin transparent(colorProgram,primary,submitted,.25f);
            transparent.draw(*owned,info); restored(state,primary); unwind(state);
            colorPixels(state,primary,submitted); depthPixels(state,primary,.25f,7);
            depthPixels(state,opaque,postPass ? .25f : .6f,7);
            colorPixels(state,replacement,sentinel); depthPixels(state,replacement,.9f,7);
        }

        osg::ref_ptr<MWRender::DistortionCallback> distortionSource
            = new MWRender::DistortionCallback(forbiddenPrimaryProvider);
        distortionSource->setFBO(distortion.framebuffer,frame%2);
        distortionSource->setOriginalFBO(primary.framebuffer,frame%2);
        auto ownedDistortion = distortionSource->ownedFrame(distortion.framebuffer,primary.framebuffer,primary.framebuffer);
        distortionSource->setFBO(replacement.framebuffer,frame%2);
        distortionSource->setOriginalFBO(replacement.framebuffer,frame%2);
        distortionSource = nullptr;
        callbackRetains({&distortion,&primary});
        clear(state,primary,initial,.6f); clear(state,distortion,initial,.6f); bind(state,primary);
        Bin distorted(colorProgram,primary,submitted,.25f);
        // Distortion targets intentionally have no depth attachment.
        distorted.base->setMode(GL_DEPTH_TEST,osg::StateAttribute::OFF);
        distorted.draw(*ownedDistortion,info); restored(state,primary); unwind(state);
        colorPixels(state,distortion,submitted);
        colorPixels(state,primary,initial); depthPixels(state,primary,.6f,7);
        colorPixels(state,replacement,sentinel); depthPixels(state,replacement,.9f,7);
        require(glGetError() == GL_NO_ERROR,"pixel validation generated GL errors");
        std::cout << "PASS: owned postpass sequence=" << sequence << " repeated_or_skipped_frame=" << frame
            << " extent=" << width << 'x' << height << " depthclear+transparent_blit+transparent_overlay+distortion\n";
    }
}

int main() try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = traits->height = 64; traits->doubleBuffer = false; traits->windowDecoration = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    if (!context || !context->realize() || !context->makeCurrent())
        throw Unsupported("real GL context unavailable");
    auto* state = context->getState();
    auto* ext = state->get<osg::GLExtensions>();
    if (!ext || !ext->isFrameBufferObjectSupported || !ext->glCheckFramebufferStatus || !ext->glBlitFramebuffer
        || osg::getGLVersionNumber() < 3.f)
        throw Unsupported("OpenGL 3 framebuffer/depth-stencil/blit contract unavailable");
    // Stencil readback is one byte per pixel, including non-square extents.
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp; state->setFrameStamp(stamp);
    osg::ref_ptr<osg::Camera> camera = new osg::Camera;
    require(camera->getUserData() == nullptr,"fixture must not supply engine userdata");
    osg::RenderInfo info(state,nullptr); info.pushCamera(camera);
    osg::ref_ptr<osg::Program> colorProgram = program(false), depthProgram = program(true);
    osg::ref_ptr<osg::StateSet> depthOnly = new osg::StateSet;
    depthOnly->setAttributeAndModes(depthProgram,Protected);
    depthOnly->setAttributeAndModes(new osg::ColorMask(false,false,false,false),Protected);
    depthOnly->setAttributeAndModes(new SceneUtil::AutoDepth,Protected);
    depthOnly->addUniform(new osg::Uniform("drawDepth",.25f));
    const std::array<unsigned,4> frames{1,1,4,5}, widths{16,37,8,16}, heights{16,19,8,16};
    for (unsigned i=0;i<frames.size();++i)
    {
        stamp->setFrameNumber(frames[i]);
        generation(*state,info,i,frames[i],widths[i],heights[i],colorProgram,depthOnly);
    }
    info.popCamera(); unwind(*state); context->releaseContext(); context->close(true);
    std::cout << "PASS: actual production owned postpass GL branches, immutable retained FBO generations, every color/depth/stencil pixel, repeated IDs and resize, no legacy provider calls\n";
}
catch (const Unsupported& error) { std::cout << "SKIP: " << error.what() << '\n'; return 77; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
