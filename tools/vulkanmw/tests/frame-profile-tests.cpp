#include <components/debug/frameprofile.hpp>
#include <components/debug/runtimediagnostics.hpp>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
int main()
{
    using Debug::FrameProfile::Accumulator;
    Accumulator a;
    check(a.enter("off", 0) == Accumulator::Invalid && a.size == 0, "disabled path");
    a.active = true;
    const auto root = a.enter("frame", 10);
    auto batch = a.enter("batch", 15);
    auto child = a.enter("leaf", 20);
    a.leave(child, 25);
    a.leave(batch, 30);
    batch = a.enter("batch", 40);
    a.leave(batch, 45);
    a.leave(root, 60);
    check(a.totals[0].inclusive == 50 && a.totals[0].exclusive == 30, "parent subtracts children");
    check(a.totals[1].inclusive == 20 && a.totals[1].exclusive == 15, "repeated batch sums");
    check(a.totals[1].calls == 2 && a.totals[1].maximum == 15, "batch count and maximum");
    std::uint64_t sum = 0;
    for (unsigned i = 0; i < a.size; ++i) sum += a.totals[i].exclusive;
    check(sum == a.totals[0].inclusive && a.depth == 0 && a.dropped == 0, "exclusive accounting closes");
    a = {};
    a.active = true;
    for (unsigned i = 0; i < Accumulator::Depth; ++i) a.enter("recursive", i);
    check(a.enter("overflow", 33) == Accumulator::Invalid && a.dropped == 1, "bounded depth");
    for (unsigned i = Accumulator::Depth; i > 0; --i) a.leave(i - 1, 70 - i);
    check(a.depth == 0 && a.totals[0].calls == Accumulator::Depth, "recursive scope closes");
    auto ring = std::make_unique<Debug::RuntimeDiagnostics::Ring<3>>();
    std::array<Debug::RuntimeDiagnostics::Record, 2> records{};
    records[0].frame = 1;
    records[1].frame = 2;
    check(ring->pushBatch(records) && ring->size() == 2, "batch admitted");
    check(!ring->pushBatch(records) && ring->size() == 2, "batch overflow is atomic");
    Debug::RuntimeDiagnostics::Record record;
    check(ring->pop(record) && record.frame == 1, "batch order first");
    check(ring->pop(record) && record.frame == 2, "batch order second");
    std::cout << "Frame profiling accounting tests passed\n";
}
