// Native feasibility proof only. No engine texture factory enables this class.
// The recipe is procedural in this fixture; real VFS/source revisions, material
// CPU consumers, compressed formats, clones, pools and multi-context retirement
// must be qualified before any production CPU backing retirement is enabled.
#include <osg/DisplaySettings>
#include <osg/GraphicsContext>
#include <osg/Image>
#include <osg/State>
#include <osg/Texture2D>
#include <osgViewer/Viewer>

#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
    unsigned checks = 0;
    void require(bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }
    std::atomic<unsigned> destroyed{0};
    struct TrackedImage final : osg::Image
    {
        ~TrackedImage() override { destroyed.fetch_add(1); }
    };
    osg::ref_ptr<osg::Image> image()
    {
        osg::ref_ptr<osg::Image> result = new TrackedImage;
        result->allocateImage(32, 32, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        result->setInternalTextureFormat(GL_RGBA8);
        result->setDataVariance(osg::Object::STATIC);
        for (unsigned i = 0; i < 32 * 32; ++i)
        {
            result->data()[4 * i] = 0;
            result->data()[4 * i + 1] = 255;
            result->data()[4 * i + 2] = 0;
            result->data()[4 * i + 3] = 255;
        }
        return result;
    }
    struct Recipe
    {
        unsigned reads = 0;
        bool available = true;
        osg::ref_ptr<osg::Image> reopen()
        {
            if (!available) return {};
            ++reads;
            return image();
        }
    };
    class ReconstructableTexture final : public osg::Texture2D
    {
    public:
        ReconstructableTexture(osg::Image* source, std::shared_ptr<Recipe> recipe)
            : osg::Texture2D(source), mRecipe(std::move(recipe))
        {
            setDataVariance(osg::Object::STATIC);
            setResizeNonPowerOfTwoHint(false);
            setFilter(MIN_FILTER, LINEAR);
            setFilter(MAG_FILTER, LINEAR);
            setClientStorageHint(false);
            setUnRefImageDataAfterApply(true);
        }
        void apply(osg::State& state) const override
        {
            if (!getImage() && !getTextureObject(state.getContextID()))
            {
                osg::ref_ptr<osg::Image> source;
                if (mRecipe) source = mRecipe->reopen();
                // This prototype deliberately refuses; a game factory must
                // supply its established ordinary fallback before this seam.
                if (!source || source->s() != 32 || source->t() != 32
                    || source->getPixelFormat() != GL_RGBA || source->getDataType() != GL_UNSIGNED_BYTE)
                    throw std::runtime_error("Unavailable or changed reconstruction recipe");
                const_cast<ReconstructableTexture*>(this)->setImage(source.get());
            }
            osg::Texture2D::apply(state);
        }
    private:
        std::shared_ptr<Recipe> mRecipe;
    };
    void pixels(const osg::Texture2D& texture, osg::State& state)
    {
        auto* object = texture.getTextureObject(state.getContextID());
        require(object && object->id() != 0, "Actual resident GL texture exists");
        glBindTexture(GL_TEXTURE_2D, object->id());
        std::vector<unsigned char> read(32 * 32 * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
        require(glGetError() == GL_NO_ERROR, "Real GPU readback succeeded");
        bool green = true;
        for (unsigned i = 0; i < 32 * 32; ++i)
            green &= read[4 * i] == 0 && read[4 * i + 1] == 255
                && read[4 * i + 2] == 0 && read[4 * i + 3] == 255;
        require(green, "All GPU texels survive CPU backing retirement/reconstruction");
    }
}

int main() try
{
    osgViewer::Viewer anchor;
    osg::DisplaySettings::instance()->setMaxNumberOfGraphicsContexts(1);
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32; traits->height = 32; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(), "Actual private GL context exists");
    auto* state = context->getState();
    state->setMaxTexturePoolSize(0); // The OSG pool can otherwise suppress unref.

    auto controlSource = image();
    osg::ref_ptr<osg::Texture2D> control = new osg::Texture2D(controlSource.get());
    control->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
    control->setResizeNonPowerOfTwoHint(false);
    control->apply(*state);
    controlSource = nullptr;
    require(control->getImage() != nullptr && destroyed == 0, "Ordinary control retains its CPU source");
    pixels(*control, *state);

    auto recipe = std::make_shared<Recipe>();
    auto source = image();
    osg::ref_ptr<ReconstructableTexture> candidate = new ReconstructableTexture(source.get(), recipe);
    source = nullptr;
    candidate->apply(*state);
    require(candidate->getImage() == nullptr && destroyed == 1, "Actual apply releases the unpinned CPU image");
    require(recipe->reads == 0, "First upload does not reconstruct an existing source");
    pixels(*candidate, *state);
    candidate->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
    candidate->apply(*state);
    require(recipe->reads == 0, "Resident parameter change needs no CPU source reload");
    pixels(*candidate, *state);
    candidate->releaseGLObjects(state);
    require(!candidate->getTextureObject(state->getContextID()), "Released allocation requires reconstruction");
    candidate->apply(*state);
    require(recipe->reads == 1 && candidate->getImage() == nullptr && destroyed == 2,
        "Missing allocation reconstructs once and retires its temporary CPU image");
    pixels(*candidate, *state);

    auto reader = image();
    osg::ref_ptr<ReconstructableTexture> pinned = new ReconstructableTexture(reader.get(), recipe);
    pinned->apply(*state);
    require(pinned->getImage() == nullptr && destroyed == 2, "Existing CPU reader still owns its image");
    require(reader->data()[1] == 255 && reader->getTotalSizeInBytesIncludingMipmaps() == 4096,
        "Existing CPU reader retains identical pixels and exact backing charge");
    reader = nullptr;
    require(destroyed == 3, "Backing destruction waits for the last external reader");
    pixels(*pinned, *state);
    recipe->available = false;
    candidate->releaseGLObjects(state);
    bool refused = false;
    try { candidate->apply(*state); }
    catch (const std::runtime_error&) { refused = true; }
    require(refused && recipe->reads == 1, "Closed source refuses reconstruction without reviving authority");

    control->releaseGLObjects(state); pinned->releaseGLObjects(state);
    control = nullptr; pinned = nullptr; candidate = nullptr;
    require(destroyed == 4, "Ordinary source also retires with its real final owner");
    context->releaseContext(); context->close(true);
    std::cout << "PASS texture reconstruction feasibility: " << checks
              << " assertions; actual OSG apply, CPU final owners and GL texels\n"
              << "LIMITATIONS: procedural 4 KiB recipe, one context, texture pool disabled, native only. No VFS/asset/clone/compressed/multi-context coverage, game wiring, driver-memory release, RAM/FPS or visual acceptance.\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "FAIL texture reconstruction feasibility: " << error.what() << '\n';
    return 1;
}
