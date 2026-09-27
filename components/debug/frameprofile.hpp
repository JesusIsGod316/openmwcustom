#ifndef OPENMW_DEBUG_FRAMEPROFILE_H
#define OPENMW_DEBUG_FRAMEPROFILE_H

#include <array>
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace Debug::FrameProfile
{
    // Fixed storage and no logging/allocation in timed regions. Only scopes on
    // this thread belong to this tree; worker durations are reported separately.
    struct Accumulator
    {
        static constexpr unsigned Capacity = 96, Depth = 32, Invalid = ~0u;
        struct Total
        {
            std::string_view name; // call sites must supply static labels
            std::uint64_t inclusive = 0, exclusive = 0, maximum = 0, calls = 0;
        };
        struct Entry { unsigned total; std::uint64_t start, children; };
        std::array<Total, Capacity> totals{};
        std::array<Entry, Depth> stack{};
        unsigned size = 0, depth = 0, dropped = 0;
        bool active = false;

        unsigned enter(std::string_view name, std::uint64_t now)
        {
            if (!active) return Invalid;
            unsigned index = 0;
            while (index < size && totals[index].name != name) ++index;
            if (depth == Depth || (index == size && size == Capacity)) { ++dropped; return Invalid; }
            if (index == size) { totals[size].name = name; ++size; }
            stack[depth] = {index, now, 0};
            return depth++;
        }
        void leave(unsigned token, std::uint64_t now)
        {
            if (token == Invalid) return;
            if (depth == 0 || token != depth - 1 || now < stack[token].start) { ++dropped; return; }
            const auto entry = stack[--depth];
            const auto elapsed = now - entry.start;
            auto& total = totals[entry.total];
            total.inclusive += elapsed;
            total.exclusive += elapsed - std::min(elapsed, entry.children);
            total.maximum = std::max(total.maximum, elapsed);
            ++total.calls;
            if (depth) stack[depth - 1].children += elapsed;
        }
    };
    inline thread_local Accumulator accumulator;
}
#endif
