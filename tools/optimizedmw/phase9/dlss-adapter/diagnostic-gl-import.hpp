#pragma once

#include "source-textures.hpp"
#include "../../../../components/rendercore/diagnosticinputborrow.hpp"
#include <osg/State>
#include <osg/Texture2D>
#include <optional>
#include <new>

namespace P9Dlss
{
    // Developer fixture backend only. This is not linked by OpenMW. It accepts
    // partial-motion diagnostic borrows, never SynchronousInputs/NGX inputs.
    // The caller holds these real OSG source owners inside the metadata borrow.
    // No reference, source FBO attachment, or producer object is retained here.
    using DiagnosticTextureOwners = std::array<osg::ref_ptr<osg::Texture2D>, 3>;
    enum class DiagnosticImportStatus
    {
        Imported, WrongThread, WrongContext, MissingBorrow, MissingTexture,
        ResourceMismatch, FormatMismatch, ExtentMismatch, OverBudget,
        StaleSubmission, ResizeRequiresRetirement, Closed, UnsupportedSampling
    };

    // Issued by the context owner, not refreshed by an importer merely observing
    // the same HGLRC address. The owner must invalidate it before destroying its
    // namespace. A genuinely recreated context gets a new lifetime/epoch.
    class DiagnosticContextLifetime final
    {
    public:
        DiagnosticContextLifetime(unsigned osgContext, std::uint64_t epoch)
            : mNative(wglGetCurrentContext()), mOsg(osgContext), mEpoch(epoch)
        { require(mNative && mEpoch, "Context owner issues a nonzero native lifetime epoch"); }
        DiagnosticContextLifetime(const DiagnosticContextLifetime&) = delete;
        DiagnosticContextLifetime& operator=(const DiagnosticContextLifetime&) = delete;
        bool onThread() const noexcept { return std::this_thread::get_id() == mThread; }
        bool current() const noexcept { return onThread() && mAlive && wglGetCurrentContext() == mNative; }
        bool alive() const noexcept { return onThread() && mAlive; }
        bool invalidate() noexcept { if (!onThread()) return false; mAlive = false; return true; }
        bool matches(const RenderCore::Temporal::DiagnosticDrawIdentity& identity, unsigned osgContext) const noexcept
        {
            return current() && identity.nativeContext == reinterpret_cast<std::uintptr_t>(mNative)
                && identity.osgContext == mOsg && osgContext == mOsg && identity.contextEpoch == mEpoch;
        }
        HGLRC native() const noexcept { return mNative; }
        std::uint64_t epoch() const noexcept { return mEpoch; }
    private:
        const std::thread::id mThread = std::this_thread::get_id();
        const HGLRC mNative;
        const unsigned mOsg;
        const std::uint64_t mEpoch;
        bool mAlive = true;
    };
    enum class DiagnosticImportFault
    {
        None, BackendAfterFramebuffers, BackendAfterSampler, BackendAfterShader,
        BackendAfterProgram, AfterFirstMirror, AfterSecondMirror, AfterThirdMirror,
        AfterFirstCopy, AfterFence
    };
    enum class DiagnosticGlObject { Texture, Framebuffer, Sampler, Program, Shader };
    struct DiagnosticGlAudit
    {
        void* owner = nullptr;
        void (*changed)(void*, DiagnosticGlObject, GLuint, bool) noexcept = nullptr;
        void record(DiagnosticGlObject type, GLuint name, bool created) const noexcept
        { if (changed && name) changed(owner, type, name, created); }
    };

    class DiagnosticGlMirrors final
    {
    public:
        explicit DiagnosticGlMirrors(DiagnosticContextLifetime& lifetime,
            RenderCore::Temporal::DiagnosticBorrowLimits limits = {},
            DiagnosticImportFault fault = DiagnosticImportFault::None, DiagnosticGlAudit audit = {})
            : mLimits(limits), mLifetime(lifetime), mAudit(audit), mFault(fault)
        {
            require(mLifetime.current(), "Diagnostic mirror owner has the current issued namespace lifetime");
            BorrowedGlState restore;
            try { initialize(); }
            catch (...) { destroyBackend(); throw; }
        }
        ~DiagnosticGlMirrors() noexcept
        {
            if (mClosed) return;
            if (!mLifetime.onThread()) std::terminate();
            if (!mLifetime.alive()) { abandonLostContext(); return; }
            if (!mLifetime.current()) std::terminate(); // Never delete into another live namespace.
            try
            {
                BorrowedGlState restore;
                // Unwinding only: GL deletion safely releases this owner's names
                // without a GPU wait. Normal retirement remains fence-qualified.
                destroyImages(); destroyBackend(); mClosed = true;
            }
            catch (...) { std::terminate(); }
        }
        DiagnosticGlMirrors(const DiagnosticGlMirrors&) = delete;
        DiagnosticGlMirrors& operator=(const DiagnosticGlMirrors&) = delete;
        DiagnosticGlMirrors(DiagnosticGlMirrors&&) = delete;
        DiagnosticGlMirrors& operator=(DiagnosticGlMirrors&&) = delete;

        DiagnosticImportStatus import(const RenderCore::Temporal::DiagnosticInputBorrow& borrow,
            const DiagnosticTextureOwners& owners, const osg::State& state)
        {
            using namespace RenderCore::Temporal;
            if (!mLifetime.onThread()) return DiagnosticImportStatus::WrongThread;
            if (mClosed) return DiagnosticImportStatus::Closed;
            if (!mLifetime.current()) return DiagnosticImportStatus::WrongContext;
            const DiagnosticSources* source = borrow.sources();
            if (!source) return DiagnosticImportStatus::MissingBorrow;
            if (!mLifetime.matches(source->identity, state.getContextID()))
                return DiagnosticImportStatus::WrongContext;
            const unsigned width = source->images[0].width, height = source->images[0].height;
            const bool floatColor = source->images[0].format == DiagnosticFormat::Rgba16Float;
            const std::uint64_t bytesPerPixel = floatColor ? 16 : 12;
            const std::uint64_t pixels = std::uint64_t(width) * height;
            if (!width || !height || width > mLimits.maximumWidth || height > mLimits.maximumHeight)
                return DiagnosticImportStatus::ExtentMismatch;
            if (pixels > mLimits.maximumMirrorBytes / bytesPerPixel)
                return DiagnosticImportStatus::OverBudget;
            if (mLast && (source->identity.contextEpoch < mLast->contextEpoch
                || (source->identity.contextEpoch == mLast->contextEpoch
                    && (source->identity.submission <= mLast->submission || source->identity.ticket <= mLast->ticket))))
                return DiagnosticImportStatus::StaleSubmission;

            BorrowedGlState restore;
            restore.active(BorrowedGlState::Texture0);
            std::array<GLuint, 3> names{};
            for (unsigned i = 0; i < owners.size(); ++i)
            {
                if (!owners[i]) return DiagnosticImportStatus::MissingTexture;
                const osg::Texture::TextureObject* object = owners[i]->getTextureObject(state.getContextID());
                if (!object || !object->id() || !glIsTexture(object->id()))
                    return DiagnosticImportStatus::MissingTexture;
                if (object->target() != GL_TEXTURE_2D || object->getTexture() != owners[i].get())
                    return DiagnosticImportStatus::ResourceMismatch;
                names[i] = object->id();
                if (std::uint64_t(names[i]) != source->images[i].id)
                    return DiagnosticImportStatus::ResourceMismatch;
                for (unsigned j = 0; j < i; ++j)
                    if (names[i] == names[j]) return DiagnosticImportStatus::ResourceMismatch;
                glBindTexture(GL_TEXTURE_2D, names[i]);
                GLint actualWidth = 0, actualHeight = 0, actualFormat = 0;
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &actualWidth);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &actualHeight);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &actualFormat);
                if (actualWidth != GLint(width) || actualHeight != GLint(height)
                    || owners[i]->getTextureWidth() != GLint(width) || owners[i]->getTextureHeight() != GLint(height)
                    || source->images[i].width != width || source->images[i].height != height)
                    return DiagnosticImportStatus::ExtentMismatch;
                if (actualFormat != GLint(format(source->images[i].format, i)))
                    return DiagnosticImportStatus::FormatMismatch;
                GLint base = 0, swizzle[4]{};
                glGetTexParameteriv(GL_TEXTURE_2D, 0x813C, &base); // GL_TEXTURE_BASE_LEVEL
                glGetTexParameteriv(GL_TEXTURE_2D, 0x8E46, swizzle); // GL_TEXTURE_SWIZZLE_RGBA
                if (base != 0 || swizzle[0] != GL_RED || swizzle[1] != GL_GREEN
                    || swizzle[2] != GL_BLUE || swizzle[3] != GL_ALPHA)
                    return DiagnosticImportStatus::UnsupportedSampling;
                if (i == 1 && source->images[i].format != DiagnosticFormat::Depth32Float)
                {
                    GLint mode = 0; glGetTexParameteriv(GL_TEXTURE_2D, 0x90EA, &mode); // DEPTH_STENCIL_TEXTURE_MODE
                    if (mode != GL_DEPTH_COMPONENT) return DiagnosticImportStatus::UnsupportedSampling;
                }
            }
            glCheck("Validate actual diagnostic source objects without changing their parameters");

            // Never allocate a second mirror generation. Even a completed old
            // generation must be retired explicitly before an extent/format
            // change. Failed validation above does not alter ordinary output.
            if (mImages[0].gl && (width != mImages[0].width || height != mImages[0].height
                || floatColor != mFloatColor || source->identity.contextEpoch != mContextEpoch))
                return DiagnosticImportStatus::ResizeRequiresRetirement;
            try
            {
                if (!mImages[0].gl) allocate(width, height, floatColor, source->identity.contextEpoch);
                restore.prepareImport();
                restore.bindSampler(0, mShader.importSampler);
                mShader.useProgram(mShader.program);
                for (unsigned i = 0; i < mImages.size(); ++i)
                {
                    mGpu.producerTarget(mImages[i]);
                    glBindTexture(GL_TEXTURE_2D, names[i]);
                    glBegin(GL_TRIANGLE_STRIP);
                    glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(-1, 1); glVertex2f(1, 1);
                    glEnd();
                    if (i == 0) fail(DiagnosticImportFault::AfterFirstCopy);
                }
                glCheck("Submit exact diagnostic texel copies on the borrowed draw context");
                // One pending fence at most. Same-size submission replaces no
                // image storage. No readback/finish/timed wait is added here.
                if (mFence) mDeleteSync(mFence);
                mFence = mFenceSync(0x9117, 0);
                require(mFence != nullptr, "Fence bounded diagnostic mirror submission");
                fail(DiagnosticImportFault::AfterFence);
            }
            catch (...) { destroyImages(); throw; }
            mLast = source->identity;
            ++mCopies;
            return DiagnosticImportStatus::Imported;
        }

        bool retireCompleted()
        {
            if (!mLifetime.current()) return false;
            if (mFence)
            {
                const GLenum result = mClientWaitSync(mFence, 0, 0); // Query only; no flush and no timed GPU wait.
                if (result == 0x911B) return false; // GL_TIMEOUT_EXPIRED
                require(result == 0x911A || result == 0x911C, "Exact diagnostic copy completion before retirement");
                mDeleteSync(mFence); mFence = nullptr;
            }
            if (!mImages[0].gl) return true;
            BorrowedGlState restore;
            destroyImages();
            ++mRetired;
            glCheck("Retire the one completed mirror generation");
            return true;
        }
        bool close()
        {
            if (mClosed) return true;
            if (!retireCompleted()) return false;
            BorrowedGlState restore;
            destroyBackend();
            mClosed = true;
            return true;
        }
        bool abandonLostContext() noexcept
        {
            if (!mLifetime.onThread() || mLifetime.alive()) return false;
            // The authoritative owner invalidated/destroyed the namespace.
            // Forget old names, never call GL against the replacement namespace.
            mAbandonedBytes += mBytes; mBytes = 0;
            mImages = {}; mGpu.framebuffers = {};
            mShader.program = 0; mShader.importSampler = 0; mPendingShader = 0; mFence = nullptr;
            mClosed = true; return true;
        }
        std::uint64_t abandonedBytes() const noexcept { return mAbandonedBytes; }
        const std::array<Texture, 3>& images() const noexcept { return mImages; }
        std::uint64_t bytes() const noexcept { return mBytes; }
        std::uint64_t peakBytes() const noexcept { return mPeak; }
        unsigned generations() const noexcept { return mGenerations; }
        unsigned retiredGenerations() const noexcept { return mRetired; }
        unsigned copies() const noexcept { return mCopies; }

    private:
        void fail(DiagnosticImportFault point)
        { if (mFault == point) { mFault = DiagnosticImportFault::None; throw std::bad_alloc(); } }
        void initialize()
        {
            mGpu.initialize();
            for (const auto name : mGpu.framebuffers) mAudit.record(DiagnosticGlObject::Framebuffer, name, true);
            fail(DiagnosticImportFault::BackendAfterFramebuffers);
            mShader.genSampler(1, &mShader.importSampler);
            require(mShader.importSampler, "Owned diagnostic noncomparison sampler");
            mAudit.record(DiagnosticGlObject::Sampler, mShader.importSampler, true);
            mShader.samplerParameter(mShader.importSampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            mShader.samplerParameter(mShader.importSampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            mShader.samplerParameter(mShader.importSampler, GL_TEXTURE_WRAP_S, 0x812F);
            mShader.samplerParameter(mShader.importSampler, GL_TEXTURE_WRAP_T, 0x812F);
            mShader.samplerParameter(mShader.importSampler, 0x884C, GL_NONE);
            fail(DiagnosticImportFault::BackendAfterSampler);
            mShader.program = mShader.createProgram(); require(mShader.program, "Owned diagnostic import program");
            mAudit.record(DiagnosticGlObject::Program, mShader.program, true);
            const char* vertex = "#version 130\nvoid main(){gl_Position=gl_Vertex;}\n";
            const char* fragment = "#version 130\nuniform sampler2D sourceImage;\nvoid main(){gl_FragColor=texelFetch(sourceImage,ivec2(gl_FragCoord.xy),0);}\n";
            for (const auto item : {std::pair{SourceTextures::VertexShader, vertex}, std::pair{SourceTextures::FragmentShader, fragment}})
            {
                mPendingShader = mShader.createShader(item.first); require(mPendingShader, "Owned diagnostic import shader");
                mAudit.record(DiagnosticGlObject::Shader, mPendingShader, true);
                fail(DiagnosticImportFault::BackendAfterShader);
                mShader.shaderSource(mPendingShader, 1, &item.second, nullptr); mShader.compileShader(mPendingShader);
                GLint ready = 0; mShader.shaderStatus(mPendingShader, SourceTextures::CompileStatus, &ready);
                require(ready != 0, "Compile exact diagnostic import shader");
                mShader.attachShader(mShader.program, mPendingShader);
                mShader.deleteShader(mPendingShader); mAudit.record(DiagnosticGlObject::Shader, mPendingShader, false);
                mPendingShader = 0;
            }
            mShader.linkProgram(mShader.program);
            GLint ready = 0; mShader.programStatus(mShader.program, SourceTextures::LinkStatus, &ready);
            require(ready != 0, "Link exact diagnostic import program");
            fail(DiagnosticImportFault::BackendAfterProgram);
            mShader.useProgram(mShader.program); mShader.uniform1i(mShader.uniformLocation(mShader.program, "sourceImage"), 0);
            glCheck("Create bounded diagnostic import backend");
        }
        void destroyBackend() noexcept
        {
            if (mPendingShader) { mShader.deleteShader(mPendingShader); mAudit.record(DiagnosticGlObject::Shader, mPendingShader, false); mPendingShader = 0; }
            if (mShader.program) { mShader.deleteProgram(mShader.program); mAudit.record(DiagnosticGlObject::Program, mShader.program, false); mShader.program = 0; }
            if (mShader.importSampler) { mShader.deleteSampler(1, &mShader.importSampler); mAudit.record(DiagnosticGlObject::Sampler, mShader.importSampler, false); mShader.importSampler = 0; }
            for (const auto name : mGpu.framebuffers) mAudit.record(DiagnosticGlObject::Framebuffer, name, false);
            mGpu.remove(3, mGpu.framebuffers.data()); mGpu.framebuffers = {};
        }
        void destroyImages() noexcept
        {
            if (mFence) { mDeleteSync(mFence); mFence = nullptr; }
            for (const auto name : mGpu.framebuffers) if (name)
            {
                mGpu.bind(GpuTransfers::Framebuffer, name);
                mGpu.attach(GpuTransfers::Framebuffer, GpuTransfers::ColorAttachment, GL_TEXTURE_2D, 0, 0);
            }
            for (auto& image : mImages) if (image.gl)
            { glDeleteTextures(1, &image.gl); mAudit.record(DiagnosticGlObject::Texture, image.gl, false); image = {}; }
            mBytes = 0;
        }
        static GLenum format(RenderCore::Temporal::DiagnosticFormat value, unsigned lane)
        {
            using Format = RenderCore::Temporal::DiagnosticFormat;
            if (lane == 0) return value == Format::Rgba8 ? GL_RGBA8 : value == Format::Rgba16Float ? 0x881A : 0;
            if (lane == 1) return value == Format::Depth24Stencil8 ? 0x88F0 : value == Format::Depth32FloatStencil8
                ? 0x8CAD : value == Format::Depth32Float ? 0x8CAC : 0;
            return value == Format::Rg16Float ? 0x822F : 0;
        }
        void allocate(unsigned width, unsigned height, bool floatColor, std::uint64_t contextEpoch)
        {
            require(!mImages[0].gl && !mFence, "No second mirror generation during allocation");
            // nullptr must mean allocation only, never offset zero into a
            // foreign upload PBO. Preserve that binding without involving OSG.
            GLint unpack = 0; glGetIntegerv(0x88EF, &unpack); // GL_PIXEL_UNPACK_BUFFER_BINDING
            struct RestoreUnpack
            {
                void (APIENTRY* bind)(GLenum, GLuint);
                GLint value;
                ~RestoreUnpack() { bind(0x88EC, GLuint(value)); }
            } restoreUnpack{mBindBuffer, unpack};
            mBindBuffer(0x88EC, 0); // GL_PIXEL_UNPACK_BUFFER
            const std::array<GLenum, 3> internal{floatColor ? 0x881Au : GLenum(GL_RGBA8), 0x822E, 0x822F};
            const std::array<GLenum, 3> external{GL_RGBA, 0x1903, 0x8227};
            for (unsigned i = 0; i < mImages.size(); ++i)
            {
                auto& image = mImages[i]; image.width = width; image.height = height;
                glGenTextures(1, &image.gl); require(image.gl != 0, "Allocate bounded private diagnostic mirror");
                mAudit.record(DiagnosticGlObject::Texture, image.gl, true);
                glBindTexture(GL_TEXTURE_2D, image.gl);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, internal[i], width, height, 0, external[i], GL_FLOAT, nullptr);
                GLint realized = 0; glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &realized);
                require(realized == GLint(internal[i]), "Mirror realizes its exact sized format");
                fail(i == 0 ? DiagnosticImportFault::AfterFirstMirror : i == 1 ? DiagnosticImportFault::AfterSecondMirror : DiagnosticImportFault::AfterThirdMirror);
            }
            glCheck("Allocate one sized diagnostic mirror generation");
            mFloatColor = floatColor; mContextEpoch = contextEpoch;
            mBytes = std::uint64_t(width) * height * (floatColor ? 16 : 12);
            mPeak = std::max(mPeak, mBytes); ++mGenerations;
        }
        const RenderCore::Temporal::DiagnosticBorrowLimits mLimits;
        DiagnosticContextLifetime& mLifetime;
        const DiagnosticGlAudit mAudit;
        DiagnosticImportFault mFault;
        GpuTransfers mGpu;
        SourceTextures mShader;
        std::array<Texture, 3> mImages;
        std::optional<RenderCore::Temporal::DiagnosticDrawIdentity> mLast;
        using Sync = void*;
        Sync (APIENTRY* mFenceSync)(GLenum, GLbitfield) = function<decltype(mFenceSync)>("glFenceSync");
        GLenum (APIENTRY* mClientWaitSync)(Sync, GLbitfield, std::uint64_t)
            = function<decltype(mClientWaitSync)>("glClientWaitSync");
        void (APIENTRY* mDeleteSync)(Sync) = function<decltype(mDeleteSync)>("glDeleteSync");
        void (APIENTRY* mBindBuffer)(GLenum, GLuint) = function<decltype(mBindBuffer)>("glBindBuffer");
        Sync mFence = nullptr;
        GLuint mPendingShader = 0;
        std::uint64_t mBytes = 0, mPeak = 0, mContextEpoch = 0, mAbandonedBytes = 0;
        unsigned mGenerations = 0, mRetired = 0, mCopies = 0;
        bool mFloatColor = false, mClosed = false;
    };
}
