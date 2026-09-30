#ifndef OPENMW_PHASE9_INTEROP_CONTRACT_H
#define OPENMW_PHASE9_INTEROP_CONTRACT_H

#include <array>
#include <cstdint>
#include <stdexcept>

namespace Phase9Interop
{
    // This bounded fixture proves two independently owned slots, not an engine
    // ring size or DLSS resource lifetime. Tokens cannot alias across resize.
    struct Token
    {
        std::uint64_t generation;
        std::uint64_t sequence;
        friend bool operator==(const Token&, const Token&) = default;
    };

    class Slot
    {
        enum class State { External, Submitted, Consumed, Retired };
        State mState = State::External;
        Token mToken;
    public:
        explicit Slot(std::uint64_t generation) : mToken{ generation, 0 } {}
        Token submit(std::uint64_t sequence)
        {
            if ((mState != State::External && mState != State::Consumed) || sequence <= mToken.sequence)
                throw std::logic_error("interop slot reuse before both API consumers retired");
            mToken.sequence = sequence;
            mState = State::Submitted;
            return mToken;
        }
        void consume(Token token, bool vulkanFenceComplete, bool glFenceComplete)
        {
            if (mState != State::Submitted || token != mToken || !vulkanFenceComplete || !glFenceComplete)
                throw std::logic_error("interop consumer has stale token or incomplete retirement proof");
            mState = State::Consumed;
        }
        void retire()
        {
            if (mState != State::External && mState != State::Consumed)
                throw std::logic_error("interop resize attempted while a slot is in flight");
            mState = State::Retired;
        }
    };

    template<std::size_t Size>
    bool equalNonzeroId(const std::array<unsigned char, Size>& a, const unsigned char* b)
    {
        bool nonzero = false;
        for (std::size_t i = 0; i < Size; ++i)
        {
            nonzero |= a[i] != 0;
            if (a[i] != b[i])
                return false;
        }
        return nonzero;
    }

    inline bool validNodeMask(std::uint32_t mask)
    {
        return mask != 0 && (mask & (mask - 1)) == 0;
    }

    using Pixel = std::array<unsigned char, 4>;
    inline std::uint64_t spatialSequence(std::uint64_t sequence, unsigned x, unsigned y, unsigned width, unsigned height)
    {
        return sequence ^ ((x >= width / 2 ? 1u : 0u) | (y >= height / 2 ? 2u : 0u));
    }
    inline Pixel glPattern(std::uint64_t sequence)
    {
        return { static_cast<unsigned char>((sequence & 1) ? 255 : 0),
            static_cast<unsigned char>((sequence & 2) ? 255 : 0), 0, 255 };
    }
    inline Pixel vkPattern(std::uint64_t sequence)
    {
        return { static_cast<unsigned char>((sequence & 2) ? 255 : 0),
            static_cast<unsigned char>((sequence & 1) ? 255 : 0), 255, 255 };
    }
}

#endif
