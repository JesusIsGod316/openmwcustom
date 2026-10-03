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
#include <components/sceneutil/glcalltrace.hpp>
#include "uploadpool.hpp"

namespace Resource
{
    // A narrow producer-owned texture type, not a generic "GL object exists"
    // readiness test. Only Terrain::TextureManager opts into it at startup.
    // Draw-time apply() is never bypassed. The memo skips redundant *precompile*
    // calls only after this exact resource revision was submitted in this context.
    // It does NOT claim GPU completion, residency or a nonblocking future apply.
    // A producer may explicitly opt a render target into the same proof. Its
    // dimensions/format/allocation replace the source image revision in the key.
    class PreparedTerrainTexture : public osg::Texture2D
    {
    public:
        PreparedTerrainTexture() = default;
        explicit PreparedTerrainTexture(osg::Image* image) : osg::Texture2D(image) {}
        PreparedTerrainTexture(const PreparedTerrainTexture& other,
            const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY)
            : osg::Texture2D(other, copy), mRenderTarget(other.mRenderTarget),
              mPreparationMemoEnabled(other.mPreparationMemoEnabled) {} // clones never inherit preparation proofs
        META_StateAttribute(Resource, PreparedTerrainTexture, TEXTURE);
        void setPreparationRenderTarget(bool enabled) { mRenderTarget = enabled; mPrepared.setAllElementsTo(std::nullopt); }
        // Diagnostic-only producer wrappers prove the actual apply owner without
        // changing ordinary ICO reuse behavior in an instrumented CONTROL run.
        void setPreparationMemoEnabled(bool enabled)
        { mPreparationMemoEnabled = enabled; mPrepared.setAllElementsTo(std::nullopt); }
        void apply(osg::State& state) const override
        {
            if (getSubloadCallback())
            {
                osg::Texture2D::apply(state);
                return;
            }
            // Direct ICO/FBO applies do not update OSG's cached last attribute.
            // This scope is exact producer ownership, not cached-attribute
            // inference. Callback-managed uploads deliberately remain unknown.
            SceneUtil::GLCallTrace::ScopedTextureApply ownership(state,*this);
            if (UploadPool::requested() && _image && !getTextureObject(state.getContextID())
                && !_image->getPixelBufferObject() && !getUnRefImageDataAfterApply() && !getReadPBuffer()
                && getBorderWidth()==0 && !getResizeNonPowerOfTwoHint()
                && !getUseHardwareMipMapGeneration() && UploadPool::eligible(*_image))
            {
                GLsizei width=0,height=0,levels=0;
                computeInternalFormat();
                computeRequiredTextureDimensions(state,*_image,width,height,levels);
                if (width==_image->s() && height==_image->t()
                    && levels==static_cast<GLsizei>(_image->getNumMipmapLevels())
                    && (_internalFormat==GL_ALPHA || _internalFormat==GL_ALPHA8))
                {
                    auto* pool=UploadPool::find(state,true);
                    auto* slot=pool ? pool->acquire(state,_image->getTotalSizeInBytesIncludingMipmaps()) : nullptr;
                    if (slot)
                    {
                        _textureWidth=width; _textureHeight=height; _numMipmapLevels=levels;
                        auto* object=generateAndAssignTextureObject(state.getContextID(),GL_TEXTURE_2D,
                            levels,_internalFormat,width,height,1,0);
                        // Recycled allocated objects may have immutable storage.
                        // Ordinary OSG subload owns that case and its metadata.
                        if(object->isAllocated())
                        {
                            _modifiedCount[state.getContextID()]=_image->getModifiedCount()-1;
                            osg::Texture2D::apply(state); return;
                        }
                        object->bind(); applyTexParameters(GL_TEXTURE_2D,state);
                        if (UploadPool::upload(state,*pool,*slot,*_image,_internalFormat))
                        {
                            object->setAllocated(levels,_internalFormat,width,height,1,0);
                            _modifiedCount[state.getContextID()]=_image->getModifiedCount();
                        }
                        else
                        {
                            object->release(); _textureObjectBuffer[state.getContextID()]=nullptr;
                        } // required ordinary upload remains authoritative
                    }
                }
            }
            osg::Texture2D::apply(state);
        }

        bool preparationUnchanged(const osg::State& state) const
        {
            if (!mPreparationMemoEnabled || !preparationContextEligible(state)) return false;
            const unsigned context = state.getContextID();
            if (context >= mPrepared.size() || !state.getGraphicsContext()) return false;
            const auto current = signature(context);
            const auto& previous = mPrepared[context];
            return current && previous && (_image ? previous->image.valid() : mRenderTarget) && previous->object.valid()
                && previous->context.valid() && previous->context.get() == state.getGraphicsContext()
                && previous->image.get() == _image.get()
                && previous->object.get() == getTextureObject(context)
                && previous->signature == *current;
        }

        void rememberPreparation(const osg::State& state) const
        {
            if (!mPreparationMemoEnabled || !preparationContextEligible(state)) return;
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
            if (state && UploadPool::requested()) UploadPool::release(*state);
            if (state && state->getContextID() < mPrepared.size()) mPrepared[state->getContextID()].reset();
            else mPrepared.setAllElementsTo(std::nullopt);
            osg::Texture2D::releaseGLObjects(state);
        }

        virtual std::uint64_t retainedSourceBytes() const noexcept
        { return _image ? _image->getTotalDataSize() : 0; }

        // Pure producer/context admission, separate from draw-owned GL object
        // and memo state. A context-specific wrapper can decline optional work
        // before a descriptor occupies preparation credit indefinitely.
        virtual bool preparationAdmissionEligible(const osg::State&) const noexcept { return true; }

        // Draw-context dispatch recheck. Unlike producer admission, a narrow
        // wrapper may inspect its draw-owned context arrays here to decline
        // late generation requests before speculative fallback GL is invoked.
        virtual bool preparationDispatchEligible(const osg::State& state) const
        { return preparationAdmissionEligible(state); }

        // Called by the composite producer only after a successful layer draw
        // and caller-state restoration. Precompile apply alone is not use.
        virtual void recordProducerDrawUse(osg::State&, bool) const noexcept {}

    protected:
        // A generated image wrapper may use an explicit original-image control
        // in an incompatible context. That path must never inherit the outer
        // texture's preparation proof or pretend the supplied image was applied.
        virtual bool preparationContextEligible(const osg::State&) const { return true; }

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
            if (!object || !object->isAllocated() || !object->id()
                || (_image ? !_image->data() : !mRenderTarget || _textureWidth <= 0 || _textureHeight <= 0)
                || getSubloadCallback() || getUpdateCallback() || getEventCallback() || getReadPBuffer()
                || getDataVariance() == osg::Object::DYNAMIC
                || (_image && (_image->getDataVariance() == osg::Object::DYNAMIC || _image->requiresUpdateCall()))
                || isDirty(context) || _texParametersDirtyList[context] || _texMipmapGenerationDirtyList[context])
                return std::nullopt;
            const auto& p = object->_profile;
            Signature s;
            s.integers = {_wrap_s, _wrap_t, _wrap_r, _min_filter, _mag_filter,
                _useHardwareMipMapGeneration, _unrefImageDataAfterApply, _clientStorageHint,
                _resizeNonPowerOfTwoHint, _borderWidth, _internalFormatMode, _internalFormatType,
                _internalFormat, _sourceFormat, _sourceType, _use_shadow_comparison,
                _shadow_compare_func, _shadow_texture_mode, _textureWidth, _textureHeight, _numMipmapLevels,
                _image ? _image->getModifiedCount() : 0, _image ? _image->s() : 0, _image ? _image->t() : 0,
                _image ? _image->r() : 0, _image ? _image->getPixelFormat() : 0,
                _image ? _image->getDataType() : 0, _image ? _image->getPacking() : 0, _image ? _image->getRowLength() : 0,
                p._target, p._numMipmapLevels, p._internalFormat, p._width, p._height, p._depth, p._border};
            s.floats = {_maxAnisotropy, _minlod, _maxlod, _lodbias, _shadow_ambient};
            s.border = _borderColor;
            s.swizzle = _swizzle;
            return s;
        }
        mutable osg::buffered_object<std::optional<Record>> mPrepared;
        bool mRenderTarget = false;
        bool mPreparationMemoEnabled = true;
    };

    inline std::uint64_t retainedTerrainTextureBytes(const PreparedTerrainTexture& texture) noexcept
    { return texture.retainedSourceBytes(); }

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
