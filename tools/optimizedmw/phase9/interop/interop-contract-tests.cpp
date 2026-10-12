#include "interop-contract.hpp"

#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    template<class F> void rejected(F f, const char* message)
    {
        try { f(); } catch (const std::logic_error&) { return; }
        throw std::runtime_error(message);
    }
}

int main() try
{
    using namespace Phase9Interop;
    Slot first(1), second(1);
    const auto a = first.submit(1), b = second.submit(2);
    rejected([&] { first.submit(3); }, "in-flight reuse accepted");
    rejected([&] { first.retire(); }, "resize released in-flight image");
    rejected([&] { first.consume(a, true, false); }, "Vulkan fence substituted for GL consumption");
    rejected([&] { first.consume(b, true, true); }, "other slot sequence accepted");
    rejected([&] { first.consume({ 2, 1 }, true, true); }, "other generation accepted");
    first.consume(a, true, true);
    rejected([&] { first.consume(a, true, true); }, "duplicate semaphore consumer accepted");
    const auto c = first.submit(3);
    rejected([&] { first.consume(a, true, true); }, "stale submission token accepted");
    first.consume(c, true, true);
    second.consume(b, true, true);
    first.retire(); second.retire();
    rejected([&] { first.submit(4); }, "retired generation reused");
    Slot resized(2);
    auto resizedToken = resized.submit(4);
    rejected([&] { resized.consume(c, true, true); }, "old resize generation consumed");
    resized.consume(resizedToken, true, true); resized.retire();
    std::array<unsigned char, 16> zero{}, id{};
    id[5] = 17;
    require(!equalNonzeroId(zero, zero.data()), "all-zero device UUID accepted");
    require(equalNonzeroId(id, id.data()), "matching nonzero UUID rejected");
    require(!equalNonzeroId(id, zero.data()), "mismatched device UUID accepted");
    require(validNodeMask(1) && validNodeMask(4) && !validNodeMask(0) && !validNodeMask(3),
        "linked-device node mask validation failed");
    require(glPattern(1) != glPattern(2) && glPattern(2) != vkPattern(2),
        "stale or unchanged pixels could satisfy the round trip");
    require(spatialSequence(1, 0, 0, 37, 19) == 1 && spatialSequence(1, 36, 18, 37, 19) == (1 ^ 3),
        "non-square quadrant addressing changed");
    for (unsigned i = 0; i < 16; ++i)
        require(glPattern(i) != vkPattern(i), "one API's unchanged image could satisfy its return verification");
    std::cout << "Two-slot retirement, resize tokens and device identity contract passed\n";
    return 0;
}
catch (const std::exception& e)
{
    std::cerr << e.what() << '\n';
    return 1;
}
