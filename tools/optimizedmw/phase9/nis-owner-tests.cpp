#include <apps/openmw/mwrender/nisscaler.hpp>
#include <osg/GLExtensions>
#include <osg/GraphicsContext>
#include <osg/Image>
#include <osg/RenderInfo>
#include <osgViewer/Viewer>
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <vector>

void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
osg::ref_ptr<osg::Texture2D> solid(unsigned size,const osg::Vec4f& color)
{
    osg::ref_ptr<osg::Image> image = new osg::Image;
    image->allocateImage(size,size,1,GL_RGBA,GL_FLOAT);
    for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x) image->setColor(color,x,y);
    osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
    texture->setInternalFormat(GL_RGBA8);
    texture->setTextureSize(size,size);
    texture->setResizeNonPowerOfTwoHint(false);
    texture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
    texture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::LINEAR);
    texture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
    texture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
    return texture;
}
int main() try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = traits->height = 32; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(),"real GL context missing");
    osg::State& state = *context->getState();
    osg::RenderInfo info(&state,nullptr);
    auto* ext = state.get<osg::GLExtensions>();
    require(ext != nullptr,"GL extension dispatch missing");

    // The exact production unavailable path must return nullptr so canvas
    // presentation can keep using its retained source through bilinear draw.
    auto compute = ext->glDispatchCompute;
    ext->glDispatchCompute = nullptr;
    MWRender::NisScaler unavailable;
    auto oldInput = solid(16,{.25f,.5f,.75f,1.f});
    require(!unavailable.dispatch(info,oldInput,16,16,32,32,.2f),"missing compute did not select NIS fallback");
    ext->glDispatchCompute = compute;
    require(!unavailable.dispatch(info,oldInput,16,16,32,32,.2f),"unavailable context was retried as valid");
    require(unavailable.dispatch(info,oldInput,16,16,16,16,.2f) == oldInput,
        "native resolution should return the captured input unchanged");
    if (ext->glslLanguageVersion < 4.3f || !compute || !ext->glBindImageTexture || !ext->glMemoryBarrier)
    {
        std::cout << "SKIP: unavailable fallback passed; compute NIS pixels need GL4.3\n";
        return 77;
    }

    MWRender::NisScaler sharedDrawScaler;
    struct Submission { osg::ref_ptr<osg::Texture2D> input; unsigned inputSize, outputSize; float sharpness; };
    Submission delayed{oldInput,16,32,.2f};
    // A later cull/resource generation replaces the source while delayed still
    // retains the old texture, dimensions and sharpness. The same draw owner
    // then consumes each submission serially, exactly as canvas uses NIS.
    Submission next{solid(8,{.75f,.25f,.5f,1.f}),8,16,.8f};
    oldInput = next.input;
    auto draw = [&](const Submission& submission,const osg::Vec4f& expected) {
        auto* output = sharedDrawScaler.dispatch(info,submission.input,submission.inputSize,submission.inputSize,
            submission.outputSize,submission.outputSize,submission.sharpness);
        require(output != nullptr,"actual production NIS shader/dispatch failed");
        require(output->getTextureWidth() == static_cast<int>(submission.outputSize)
            && output->getTextureHeight() == static_cast<int>(submission.outputSize),"NIS output generation extent mismatch");
        state.applyTextureAttribute(0,output);
        // CPU texture readback is a separate consumer from production's
        // sampler fetch and requires the texture-update barrier bit.
        ext->glMemoryBarrier(0x0100);
        std::vector<float> pixels(submission.outputSize*submission.outputSize*4);
        glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_FLOAT,pixels.data());
        for (std::size_t i = 0; i < pixels.size(); ++i)
            require(std::abs(pixels[i]-expected[i%4]) < .015f,"NIS consumed another submission's input or stale output");
        require(glGetError() == GL_NO_ERROR,"actual NIS generated GL errors");
    };
    draw(delayed,{.25f,.5f,.75f,1.f});
    draw(next,{.75f,.25f,.5f,1.f});
    draw(delayed,{.25f,.5f,.75f,1.f});
    require(delayed.input->getTextureWidth() == 16,"later source generation mutated the retained input");
    require(!sharedDrawScaler.dispatch(info,next.input,8,8,1,1,.5f),"invalid NIS scale did not select fallback");
    std::cout << "PASS: production NIS pixels, retained input/settings, serialized output resize and unavailable/invalid-scale fallback\n";
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
