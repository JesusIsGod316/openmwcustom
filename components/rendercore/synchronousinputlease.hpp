#ifndef OPENMW_RENDERCORE_SYNCHRONOUSINPUTLEASE_H
#define OPENMW_RENDERCORE_SYNCHRONOUSINPUTLEASE_H

#include <array>
#include <cstdint>
#include <optional>

namespace RenderCore::Temporal
{
    // Draw-owner-only, no-wait lease. This is not a fence or an asynchronous
    // image owner: the producer must obey publish/retire while borrowed, and
    // the consumer releases only after submitting the same-context import.
    // Same-context GL ordering then protects those reads from the next write.
    struct SynchronousInputs
    {
        std::uint64_t ticket = 0, frame = 0, view = 0, context = 0, generation = 0;
        std::array<std::uint64_t, 3> images{};
        unsigned width = 0, height = 0;
        bool producerSubmitted = false, motionComplete = false, colorBeforeHud = false;
        bool motionInRenderPixels = false, depthInverted = false;
    };
    struct SynchronousRequest
    {
        std::uint64_t ticket = 0, frame = 0, view = 0, context = 0, generation = 0;
        unsigned width = 0, height = 0;
    };
    class SynchronousInputLease
    {
    public:
        struct Token { std::uint64_t serial = 0; };
        bool publish(const SynchronousInputs& inputs)
        {
            if (mBorrowed || !inputs.ticket || !inputs.view || !inputs.context || !inputs.generation
                || !inputs.width || !inputs.height || !inputs.producerSubmitted)
                return false;
            for (unsigned i = 0; i < inputs.images.size(); ++i)
            {
                if (!inputs.images[i]) return false;
                for (unsigned j = 0; j < i; ++j)
                    if (inputs.images[i] == inputs.images[j]) return false;
            }
            if (mInputs && (inputs.ticket <= mInputs->ticket || inputs.generation < mInputs->generation))
                return false;
            mInputs = inputs;
            ++mSerial;
            return true;
        }
        std::optional<Token> acquire(const SynchronousRequest& request)
        {
            if (mBorrowed || !mInputs) return std::nullopt;
            const auto& value = *mInputs;
            if (request.ticket != value.ticket || request.frame != value.frame || request.view != value.view
                || request.context != value.context || request.generation != value.generation
                || request.width != value.width || request.height != value.height
                || !value.motionComplete || !value.colorBeforeHud || !value.motionInRenderPixels)
                return std::nullopt;
            mBorrowed = true;
            ++mSerial;
            return Token{mSerial};
        }
        bool accepts(Token token) const { return mBorrowed && token.serial && token.serial == mSerial && mInputs; }
        const SynchronousInputs* inputs(Token token) const { return accepts(token) ? &*mInputs : nullptr; }
        bool release(Token token)
        {
            if (!accepts(token)) return false;
            mBorrowed = false;
            return true;
        }
        bool retire()
        {
            if (mBorrowed) return false;
            mInputs.reset();
            ++mSerial;
            return true;
        }
        bool borrowed() const { return mBorrowed; }
    private:
        std::optional<SynchronousInputs> mInputs;
        std::uint64_t mSerial = 0;
        bool mBorrowed = false;
    };
}
#endif
