#include "../../../components/rendercore/diagnosticframeidentity.hpp"
#include <osg/Texture2D>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace RenderCore::Temporal
{
    struct DiagnosticFrameIdentityTestAccess
    {
        static void exhaust(DiagnosticIdentitySource& source) { source.mSerial.store(UINT64_MAX - 1); }
    };
}
using namespace RenderCore::Temporal;
namespace
{
    unsigned checks = 0;
    void require(bool value, const char* text) { ++checks; if (!value) throw std::runtime_error(text); }
}
int main()
{
    try
    {
        DiagnosticIdentitySource bounded;
        require(bounded.issue() == 1 && bounded.issue() == 2, "Actual event allocator advances monotonically");
        DiagnosticFrameIdentityTestAccess::exhaust(bounded);
        require(bounded.issue() == UINT64_MAX && bounded.issue() == 0 && bounded.issue() == 0,
            "Exhausted identity allocator never wraps or refreshes a previous event");
        osg::ref_ptr<osg::Texture2D> scene = new osg::Texture2D, depth = new osg::Texture2D;
        osg::ref_ptr<osg::Texture2D> otherScene = new osg::Texture2D, otherDepth = new osg::Texture2D;
        require(!DiagnosticTargetGroup::issue(nullptr, depth) && !DiagnosticTargetGroup::issue(scene, nullptr)
            && !DiagnosticTargetGroup::issue(scene, scene), "Missing or aliased source owners refuse generation issuance");
        auto original = DiagnosticTargetGroup::issue(scene, depth);
        require(original && original->generation() && original->matches(scene, depth), "Target event pins exact real CPU owners");
        require(!original->matches(otherScene, depth) && !original->matches(scene, otherDepth),
            "Matching dimensions/names cannot substitute another owner");
        const auto publisher = issueDiagnosticPublisherIdentity();
        DiagnosticAcquiredIdentity first{publisher, issueDiagnosticAcquiredCapture(), original};
        DiagnosticAcquiredIdentity second{publisher, issueDiagnosticAcquiredCapture(), original};
        require(first.matches(scene, depth) && second.capture > first.capture && second.publisher == first.publisher,
            "Distinct finalized captures retain one actual publisher identity across slot reuse");
        auto next = DiagnosticTargetGroup::issue(otherScene, otherDepth);
        require(next && next->generation() > original->generation() && original->matches(scene, depth),
            "Target replacement preserves a delayed prior capture instead of mutating its owners");
        DiagnosticAcquiredIdentity third{publisher, issueDiagnosticAcquiredCapture(), next};
        require(third.matches(otherScene, otherDepth) && !first.matches(otherScene, otherDepth),
            "Captured resource groups remain tied to their exact publication event");
        std::thread release([&] { original->invalidate(); });
        release.join();
        require(!first.matches(scene, depth) && !second.matches(scene, depth) && third.matches(otherScene, otherDepth),
            "Explicit graph release invalidates all old shared captures without invalidating a replacement");
        auto republished = DiagnosticTargetGroup::issue(scene, depth, DiagnosticTargetGroup::Event::OwnerRepublishedAfterRelease);
        require(republished && republished->generation() > original->generation()
            && republished->event() == DiagnosticTargetGroup::Event::OwnerRepublishedAfterRelease
            && !original->valid(), "Only an explicit source-owner republication issues a new CPU publication generation");
        DiagnosticAcquiredIdentity incomplete{publisher, 0, republished};
        require(!incomplete.matches(scene, depth), "Missing finalization/capture serial cannot claim acquired input ownership");
        require(!DiagnosticAcquiredIdentity{0, issueDiagnosticAcquiredCapture(), republished}.matches(scene, depth),
            "Missing publisher refuses acquired input ownership");
        const auto historyA = issueDiagnosticHistoryOwner(), historyB = issueDiagnosticHistoryOwner();
        require(historyA && historyB > historyA, "Actual distinct History owner creation is separate from frame/ticket/reset fields");
        const auto* retainedScene = republished->scene();
        const auto* retainedDepth = republished->depth();
        scene = nullptr; depth = nullptr;
        require(republished->matches(retainedScene, retainedDepth), "Captured group retains source owners after source containers release them");
        std::cout << "PASS diagnostic frame identity: " << checks
            << " assertions; actual CPU owner/capture semantics only, no GL allocation/native/game proof\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
