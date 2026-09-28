#ifndef OPENMW_RESOURCE_PREPAREDTERRAINTEXTURE_H
#define OPENMW_RESOURCE_PREPAREDTERRAINTEXTURE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <osg/Texture2D>
#include <osg/State>
#include <osg/GraphicsContext>
#include <osg/observer_ptr>
#include <osgUtil/IncrementalCompileOperation>

namespace Resource
{
    // A narrow producer-owned texture type, not a generic "GL object exists"
    // readiness test. Only Terrain::TextureManager opts into it at startup.
    // Draw-time apply() is never bypassed. The memo skips redundant *precompile*
    // calls only after this exact resource revision was submitted in this context.
    // It does NOT claim GPU completion or cover composite/render-target textures.
    class PreparedTerrainTexture final : public osg::Texture2D
    {
    public:
        PreparedTerrainTexture() = default;
        explicit PreparedTerrainTexture(osg::Image* image) : osg::Texture2D(image) {}
        PreparedTerrainTexture(const PreparedTerrainTexture& other,
            const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY)
            : osg::Texture2D(other, copy) {} // clones never inherit preparation proofs
        META_StateAttribute(Resource, PreparedTerrainTexture, TEXTURE);

        bool preparationUnchanged(const osg::State& state) const
        {
            const unsigned context = state.getContextID();
            if (context >= mPrepared.size() || !state.getGraphicsContext()) return false;
            const auto current = signature(context);
            const auto& previous = mPrepared[context];
            return current && previous && previous->image.valid() && previous->object.valid()
                && previous->context.valid() && previous->context.get() == state.getGraphicsContext()
                && previous->image.get() == _image.get()
                && previous->object.get() == getTextureObject(context)
                && previous->signature == *current;
        }

        void rememberPreparation(const osg::State& state) const
        {
            const unsigned context = state.getContextID();
            if (context >= mPrepared.size() || !state.getGraphicsContext()) return;
            if (const auto current = signature(context))
                mPrepared[context] = Record{_image.get(), getTextureObject(context), const_cast<osg::GraphicsContext*>(state.getGraphicsContext()), *current};
            else
                mPrepared[context].reset();
        }

        void resizeGLObjectBuffers(unsigned size) override
        {
            osg::Texture2D::resizeGLObjectBuffers(size);
            mPrepared.resize(size);
        }

        void releaseGLObjects(osg::State* state = nullptr) const override
        {
            if (state) mPrepared[state->getContextID()].reset();
            else mPrepared.setAllElementsTo(std::nullopt);
            osg::Texture2D::releaseGLObjects(state);
        }

    private:
        struct Signature
        {
            std::array<std::int64_t, 36> integers{};
            std::array<float, 5> floats{};
            osg::Vec4d border;
            osg::Vec4i swizzle;
            friend bool operator==(const Signature&, const Signature&) = default;
        };
        struct Record
        {
            osg::observer_ptr<osg::Image> image;
            osg::observer_ptr<osg::Texture::TextureObject> object;
            osg::observer_ptr<osg::GraphicsContext> context;
            Signature signature;
        };
        std::optional<Signature> signature(unsigned context) const
        {
            const auto* object = getTextureObject(context);
            if (!object || !object->isAllocated() || !object->id() || !_image || !_image->data()
                || getSubloadCallback() || getUpdateCallback() || getEventCallback() || getReadPBuffer()
                || getDataVariance() == osg::Object::DYNAMIC
                || _image->getDataVariance() == osg::Object::DYNAMIC || _image->requiresUpdateCall()
                || isDirty(context) || _texParametersDirtyList[context] || _texMipmapGenerationDirtyList[context])
                return std::nullopt;
            const auto& p = object->_profile;
            Signature s;
            s.integers = {_wrap_s, _wrap_t, _wrap_r, _min_filter, _mag_filter,
                _useHardwareMipMapGeneration, _unrefImageDataAfterApply, _clientStorageHint,
                _resizeNonPowerOfTwoHint, _borderWidth, _internalFormatMode, _internalFormatType,
                _internalFormat, _sourceFormat, _sourceType, _use_shadow_comparison,
                _shadow_compare_func, _shadow_texture_mode, _textureWidth, _textureHeight, _numMipmapLevels,
                _image->getModifiedCount(), _image->s(), _image->t(), _image->r(), _image->getPixelFormat(),
                _image->getDataType(), _image->getPacking(), _image->getRowLength(),
                p._target, p._numMipmapLevels, p._internalFormat, p._width, p._height, p._depth, p._border};
            s.floats = {_maxAnisotropy, _minlod, _maxlod, _lodbias, _shadow_ambient};
            s.border = _borderColor;
            s.swizzle = _swizzle;
            return s;
        }
        mutable osg::buffered_object<std::optional<Record>> mPrepared;
    };

    class PreparedTerrainTextureCompileOp final
        : public osgUtil::IncrementalCompileOperation::CompileTextureOp
    {
    public:
        struct Stats { std::uint64_t requested, reused, completed; };
        static Stats stats() noexcept
        { return {sRequested.load(std::memory_order_relaxed), sReused.load(std::memory_order_relaxed),
            sCompleted.load(std::memory_order_relaxed)}; }
        explicit PreparedTerrainTextureCompileOp(PreparedTerrainTexture* texture) : CompileTextureOp(texture) {}
        bool reusable(const osgUtil::IncrementalCompileOperation::CompileInfo& info) const
        {
            // A force-download draw may have caller-defined side effects; preserve it.
            return !info.incrementalCompileOperation->getForceTextureDownloadGeometry()
                && static_cast<const PreparedTerrainTexture*>(_texture.get())
                    ->preparationUnchanged(*info.getState());
        }
        double estimatedTimeForCompile(osgUtil::IncrementalCompileOperation::CompileInfo& info) const override
        { return reusable(info) ? 0.000001 : CompileTextureOp::estimatedTimeForCompile(info); }
        bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
        {
            sRequested.fetch_add(1, std::memory_order_relaxed);
            if (reusable(info))
            {
                sReused.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            const bool result = CompileTextureOp::compile(info);
            if (result)
            {
                static_cast<PreparedTerrainTexture*>(_texture.get())->rememberPreparation(*info.getState());
                sCompleted.fetch_add(1, std::memory_order_relaxed);
            }
            return result;
        }
    private:
        inline static std::atomic<std::uint64_t> sRequested{0}, sReused{0}, sCompleted{0};
    };
}
#endif
