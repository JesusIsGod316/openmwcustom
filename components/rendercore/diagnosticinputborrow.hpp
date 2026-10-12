#ifndef OPENMW_RENDERCORE_DIAGNOSTICINPUTBORROW_H
#define OPENMW_RENDERCORE_DIAGNOSTICINPUTBORROW_H

#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace RenderCore::Temporal
{
    // Deliberately separate from Inputs and SynchronousInputLease. These
    // diagnostic sources have no reconstruction output, exposure, complete
    // motion flag or evaluation entry point. The metadata never authorizes
    // NGX evaluation or proves that actual GL resources passed inspection.
    enum class DiagnosticFormat : std::uint8_t
    {
        Unknown, Rgba8, Rgba16Float, Depth24Stencil8, Depth32FloatStencil8, Depth32Float, Rg16Float
    };
    enum class DiagnosticMotionCoverage : std::uint8_t { Unknown, CameraStatic, PartialOpaque };
    enum class DiagnosticMotionSpace : std::uint8_t { Unknown, CurrentToPreviousRenderPixels };
    enum class DiagnosticColorStage : std::uint8_t { Unknown, SceneBeforePostFx };
    enum class DiagnosticDepthRange : std::uint8_t { Unknown, NegativeOneToOne, ZeroToOne };

    struct DiagnosticDrawIdentity
    {
        std::uint64_t submission = 0, ticket = 0, frame = 0, view = 0;
        std::uint64_t nativeContext = 0, contextEpoch = 0, resourceEpoch = 0, motionRevision = 0;
        unsigned osgContext = 0; // OSG context zero is valid; native identity is separately mandatory.
        // Optional acquired-producer stream identity. All zero retains the
        // original diagnostic control. Registration belongs to the real
        // producer/History/target owner; these fields certify no GL storage.
        std::uint64_t publisher = 0, historyOwner = 0, capture = 0, sourceGroup = 0;
        unsigned sourceSlot = 0;
        friend bool operator==(const DiagnosticDrawIdentity&, const DiagnosticDrawIdentity&) = default;
    };
    struct DiagnosticImage
    {
        std::uint64_t id = 0, generation = 0, frame = 0, view = 0, nativeContext = 0, contextEpoch = 0;
        unsigned width = 0, height = 0;
        DiagnosticFormat format = DiagnosticFormat::Unknown;
    };
    struct DiagnosticSources
    {
        DiagnosticDrawIdentity identity;
        std::array<DiagnosticImage, 3> images; // Scene color, sampled depth, partial motion.
        DiagnosticColorStage colorStage = DiagnosticColorStage::Unknown;
        DiagnosticMotionCoverage coverage = DiagnosticMotionCoverage::Unknown;
        DiagnosticMotionSpace motionSpace = DiagnosticMotionSpace::Unknown;
        DiagnosticDepthRange depthRange = DiagnosticDepthRange::Unknown;
        bool depthInverted = false, producerSubmitted = false;
    };
    struct DiagnosticBorrowRequest
    {
        DiagnosticDrawIdentity identity;
        unsigned width = 0, height = 0;
    };
    struct DiagnosticBorrowLimits
    {
        unsigned maximumWidth = 1920, maximumHeight = 1080;
        // Only a metadata preflight for private mirrors, not allocation or
        // VRAM admission. A backend must charge its actual bounded resources.
        std::uint64_t maximumMirrorBytes = 32ull * 1024 * 1024;
    };
    enum class DiagnosticBorrowStatus
    {
        Observed, WrongThread, Busy, MissingPublication, InvalidIdentity, StalePublication,
        MissingImage, MismatchedImageOwner, InvalidExtent, OverBudget, UnsupportedFormat,
        AliasedImages, UnsubmittedProducer, UnsupportedMeaning, MismatchedRequest
    };

    class DiagnosticInputBorrowOwner;
    class DiagnosticInputBorrow final
    {
    public:
        DiagnosticInputBorrow(const DiagnosticInputBorrow&) = delete;
        DiagnosticInputBorrow(DiagnosticInputBorrow&&) = delete;
        DiagnosticInputBorrow& operator=(const DiagnosticInputBorrow&) = delete;
        DiagnosticInputBorrow& operator=(DiagnosticInputBorrow&&) = delete;
        // Only valid on the draw owner and inside its callback. Keeping this
        // pointer/reference after the callback violates the scope contract.
        const DiagnosticSources* sources() const noexcept;
    private:
        friend class DiagnosticInputBorrowOwner;
        DiagnosticInputBorrow(const DiagnosticInputBorrowOwner& owner, std::uint64_t serial)
            : mOwner(owner), mSerial(serial) {}
        const DiagnosticInputBorrowOwner& mOwner;
        const std::uint64_t mSerial;
    };

    // Construct on the actual draw owner, not an update/cull thread. No GL,
    // GPU wait, OS resource query, SDK dependency or dynamic image allocation
    // occurs here. Calls from other threads fail before touching mutable state.
    class DiagnosticInputBorrowOwner final
    {
    public:
        explicit DiagnosticInputBorrowOwner(DiagnosticBorrowLimits limits = {}) : mLimits(limits) {}
        ~DiagnosticInputBorrowOwner() { if (mBorrowed) std::terminate(); }
        DiagnosticInputBorrowOwner(const DiagnosticInputBorrowOwner&) = delete;
        DiagnosticInputBorrowOwner(DiagnosticInputBorrowOwner&&) = delete;
        DiagnosticInputBorrowOwner& operator=(const DiagnosticInputBorrowOwner&) = delete;
        DiagnosticInputBorrowOwner& operator=(DiagnosticInputBorrowOwner&&) = delete;

        DiagnosticBorrowStatus publish(const DiagnosticSources& sources)
        {
            if (!onOwnerThread()) return DiagnosticBorrowStatus::WrongThread;
            if (mBorrowed) return DiagnosticBorrowStatus::Busy;
            const auto status = validate(sources);
            if (status != DiagnosticBorrowStatus::Observed) return status;
            const auto& next = sources.identity;
            if (mLastIdentity)
            {
                const auto& last = *mLastIdentity;
                if (next.contextEpoch < last.contextEpoch
                    || (next.contextEpoch == last.contextEpoch
                        && (next.nativeContext != last.nativeContext || next.osgContext != last.osgContext
                            || next.submission <= last.submission || next.ticket <= last.ticket
                            || next.frame < last.frame
                            || next.resourceEpoch < last.resourceEpoch || next.motionRevision < last.motionRevision)))
                    return DiagnosticBorrowStatus::StalePublication;
            }
            mSources = sources;
            mLastIdentity = next; // Retirement cannot make a stale descriptor fresh.
            ++mSerial;
            return DiagnosticBorrowStatus::Observed;
        }

        template <class Consumer>
        DiagnosticBorrowStatus withBorrow(const DiagnosticBorrowRequest& request, Consumer&& consumer)
        {
            static_assert(std::is_void_v<std::invoke_result_t<Consumer, const DiagnosticInputBorrow&>>,
                "Diagnostic consumers do not export a borrow or reconstruction result");
            if (!onOwnerThread()) return DiagnosticBorrowStatus::WrongThread;
            if (mBorrowed) return DiagnosticBorrowStatus::Busy;
            if (!mSources) return DiagnosticBorrowStatus::MissingPublication;
            if (request.identity != mSources->identity || request.width != mSources->images[0].width
                || request.height != mSources->images[0].height)
                return DiagnosticBorrowStatus::MismatchedRequest;
            mBorrowed = true;
            ++mSerial;
            struct Release
            {
                bool& borrowed;
                ~Release() { borrowed = false; }
            } release{mBorrowed};
            const DiagnosticInputBorrow borrow(*this, mSerial);
            std::forward<Consumer>(consumer)(borrow);
            // This says only that a metadata callback ran. It does not mean
            // an import submitted, completed, or produced correct pixels.
            return DiagnosticBorrowStatus::Observed;
        }

        bool retire() noexcept
        {
            if (!onOwnerThread() || mBorrowed) return false;
            mSources.reset();
            ++mSerial;
            return true;
        }
        bool borrowed() const noexcept { return onOwnerThread() && mBorrowed; }

    private:
        friend class DiagnosticInputBorrow;
        bool onOwnerThread() const noexcept { return std::this_thread::get_id() == mThread; }
        const DiagnosticSources* sources(std::uint64_t serial) const noexcept
        {
            if (!onOwnerThread() || !mBorrowed || serial != mSerial || !mSources) return nullptr;
            return &*mSources;
        }
        DiagnosticBorrowStatus validate(const DiagnosticSources& sources) const noexcept
        {
            const auto& id = sources.identity;
            if (!id.submission || !id.ticket || !id.view || !id.nativeContext || !id.contextEpoch
                || !id.resourceEpoch || !id.motionRevision)
                return DiagnosticBorrowStatus::InvalidIdentity;
            if (!sources.producerSubmitted) return DiagnosticBorrowStatus::UnsubmittedProducer;
            if (sources.colorStage != DiagnosticColorStage::SceneBeforePostFx
                || (sources.coverage != DiagnosticMotionCoverage::CameraStatic
                    && sources.coverage != DiagnosticMotionCoverage::PartialOpaque)
                || sources.motionSpace != DiagnosticMotionSpace::CurrentToPreviousRenderPixels
                || (sources.depthRange != DiagnosticDepthRange::NegativeOneToOne
                    && sources.depthRange != DiagnosticDepthRange::ZeroToOne))
                return DiagnosticBorrowStatus::UnsupportedMeaning;
            const unsigned width = sources.images[0].width, height = sources.images[0].height;
            if (!width || !height || width > mLimits.maximumWidth || height > mLimits.maximumHeight)
                return DiagnosticBorrowStatus::InvalidExtent;
            for (unsigned i = 0; i < sources.images.size(); ++i)
            {
                const auto& image = sources.images[i];
                if (!image.id || !image.generation) return DiagnosticBorrowStatus::MissingImage;
                if (image.frame != id.frame || image.view != id.view || image.nativeContext != id.nativeContext
                    || image.contextEpoch != id.contextEpoch)
                    return DiagnosticBorrowStatus::MismatchedImageOwner;
                if (image.width != width || image.height != height) return DiagnosticBorrowStatus::InvalidExtent;
                for (unsigned j = 0; j < i; ++j)
                    if (image.id == sources.images[j].id) return DiagnosticBorrowStatus::AliasedImages;
            }
            const auto color = sources.images[0].format, depth = sources.images[1].format;
            if ((color != DiagnosticFormat::Rgba8 && color != DiagnosticFormat::Rgba16Float)
                || (depth != DiagnosticFormat::Depth24Stencil8 && depth != DiagnosticFormat::Depth32FloatStencil8
                    && depth != DiagnosticFormat::Depth32Float)
                || sources.images[2].format != DiagnosticFormat::Rg16Float)
                return DiagnosticBorrowStatus::UnsupportedFormat;
            const std::uint64_t bytesPerPixel = color == DiagnosticFormat::Rgba8 ? 12 : 16;
            const std::uint64_t pixels = std::uint64_t(width) * height;
            if (pixels > mLimits.maximumMirrorBytes / bytesPerPixel) return DiagnosticBorrowStatus::OverBudget;
            return DiagnosticBorrowStatus::Observed;
        }
        const std::thread::id mThread = std::this_thread::get_id();
        const DiagnosticBorrowLimits mLimits;
        std::optional<DiagnosticSources> mSources;
        std::optional<DiagnosticDrawIdentity> mLastIdentity;
        std::uint64_t mSerial = 0;
        bool mBorrowed = false;
    };
    inline const DiagnosticSources* DiagnosticInputBorrow::sources() const noexcept { return mOwner.sources(mSerial); }
}
#endif
