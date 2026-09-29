#include <components/render/backend/vsg/pipelineauditgate.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>

int main()
{
    RenderVsg::PipelineAuditGate gate;
    constexpr std::uint64_t mainViews = 0x1234;
    constexpr std::uint64_t waterViews = 0x5678;

    assert(gate.needsAudit(mainViews));
    gate.accept(mainViews);
    assert(gate.primed());
    assert(!gate.needsAudit(mainViews));
    assert(gate.needsAudit(mainViews, true));

    gate.invalidate();
    assert(gate.needsAudit(mainViews));
    const auto changed = gate.topologySerial();
    gate.accept(mainViews);
    assert(gate.acceptedSerial() == changed);
    assert(!gate.needsAudit(mainViews));

    assert(gate.needsAudit(waterViews));
    gate.accept(waterViews);
    assert(!gate.needsAudit(waterViews));
    assert(gate.needsAudit(mainViews));

    std::cout << "VulkanMW P2 pipeline audit generation gate tests passed\n";
}
