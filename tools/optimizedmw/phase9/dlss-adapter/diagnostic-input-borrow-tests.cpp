#include "../../../../components/rendercore/diagnosticinputborrow.hpp"
#include "../../../../components/rendercore/synchronousinputlease.hpp"
#include <iostream>
#include <stdexcept>
#include <type_traits>

using namespace RenderCore::Temporal;
namespace
{
    unsigned checks = 0;
    void require(bool value, const char* text)
    {
        ++checks;
        if (!value) throw std::runtime_error(text);
    }
    DiagnosticSources source()
    {
        DiagnosticSources value;
        value.identity = {1, 7, 0, 1, 17, 2, 4, 3, 0};
        value.images = {{{11, 4, 0, 1, 17, 2, 1280, 720, DiagnosticFormat::Rgba8},
                         {12, 4, 0, 1, 17, 2, 1280, 720, DiagnosticFormat::Depth24Stencil8},
                         {13, 3, 0, 1, 17, 2, 1280, 720, DiagnosticFormat::Rg16Float}}};
        value.colorStage = DiagnosticColorStage::SceneBeforePostFx;
        value.coverage = DiagnosticMotionCoverage::CameraStatic;
        value.motionSpace = DiagnosticMotionSpace::CurrentToPreviousRenderPixels;
        value.depthRange = DiagnosticDepthRange::NegativeOneToOne;
        value.producerSubmitted = true;
        return value;
    }
    DiagnosticBorrowRequest request(const DiagnosticSources& value)
    {
        return {value.identity, value.images[0].width, value.images[0].height};
    }
    void advance(DiagnosticSources& value)
    {
        ++value.identity.submission;
        ++value.identity.ticket;
        ++value.identity.frame;
        for (auto& image : value.images) image.frame = value.identity.frame;
    }
    void sourceRejected(const DiagnosticSources& value, DiagnosticBorrowStatus expected, const char* label)
    {
        DiagnosticInputBorrowOwner owner;
        require(owner.publish(value) == expected, label);
        unsigned called = 0;
        require(owner.withBorrow(request(value), [&](const auto&) { ++called; })
                == DiagnosticBorrowStatus::MissingPublication,
            "A refused publication became available to a consumer");
        require(called == 0 && !owner.borrowed(), "Refusal touched an ordinary fallback callback");
    }
}

// The complete source API cannot accept a diagnostic view or publication.
static_assert(!std::is_default_constructible_v<DiagnosticInputBorrow>);
static_assert(!std::is_copy_constructible_v<DiagnosticInputBorrow>);
static_assert(!std::is_move_constructible_v<DiagnosticInputBorrow>);
static_assert(!std::is_copy_constructible_v<DiagnosticInputBorrowOwner>);
static_assert(!std::is_move_constructible_v<DiagnosticInputBorrowOwner>);
static_assert(!std::is_constructible_v<SynchronousInputs, DiagnosticSources>);
static_assert(!std::is_convertible_v<DiagnosticSources, SynchronousInputs>);
static_assert(!std::is_constructible_v<SynchronousInputs, DiagnosticInputBorrow>);
static_assert(!std::is_convertible_v<DiagnosticInputBorrow, SynchronousInputs>);
static_assert(!std::is_constructible_v<SynchronousRequest, DiagnosticBorrowRequest>);
static_assert(!std::is_invocable_v<decltype(&SynchronousInputLease::publish),
    SynchronousInputLease&, const DiagnosticSources&>);
static_assert(!std::is_invocable_v<decltype(&SynchronousInputLease::acquire),
    SynchronousInputLease&, const DiagnosticBorrowRequest&>);
static_assert(std::is_same_v<decltype(std::declval<const DiagnosticInputBorrow&>().sources()),
    const DiagnosticSources*>);
static_assert(sizeof(DiagnosticInputBorrowOwner) <= 1024, "The metadata owner stays bounded");

int main() try
{
    auto inputs = source();
    const auto expected = request(inputs);
    DiagnosticInputBorrowOwner owner;
    unsigned calls = 0;
    require(owner.withBorrow(expected, [&](const auto&) { ++calls; })
            == DiagnosticBorrowStatus::MissingPublication,
        "Unpublished diagnostic inputs were borrowed");
    require(calls == 0, "Missing publication invoked a consumer");
    require(owner.publish(inputs) == DiagnosticBorrowStatus::Observed, "Valid partial diagnostic publication");
    require(owner.withBorrow(expected, [&](const DiagnosticInputBorrow& borrow) {
        ++calls;
        require(owner.borrowed() && borrow.sources(), "Scoped view is unavailable on its draw owner");
        require(borrow.sources()->identity == inputs.identity && borrow.sources()->images[2].id == 13,
            "Diagnostic view changed its exact frame or source");
        require(borrow.sources()->coverage == DiagnosticMotionCoverage::CameraStatic,
            "Partial motion was upgraded to completeness");
        require(owner.withBorrow(expected, [&](const auto&) { ++calls; }) == DiagnosticBorrowStatus::Busy,
            "Nested import accessed a held publication");
        auto next = inputs; advance(next);
        require(owner.publish(next) == DiagnosticBorrowStatus::Busy && !owner.retire(),
            "Borrowed metadata was overwritten or retired");
    }) == DiagnosticBorrowStatus::Observed, "Exact diagnostic borrow refused");
    require(calls == 1 && !owner.borrowed(), "Callback count or automatic release failed");
    require(owner.withBorrow(expected, [&](const auto& borrow) {
        require(borrow.sources() != nullptr, "Same-frame later diagnostic borrow unavailable");
    }) == DiagnosticBorrowStatus::Observed, "Same-frame repeated diagnostic observation refused");

    for (unsigned field = 0; field < 11; ++field)
    {
        auto bad = expected;
        switch (field)
        {
            case 0: ++bad.identity.submission; break;
            case 1: ++bad.identity.ticket; break;
            case 2: ++bad.identity.frame; break;
            case 3: ++bad.identity.view; break;
            case 4: ++bad.identity.nativeContext; break;
            case 5: ++bad.identity.contextEpoch; break;
            case 6: ++bad.identity.resourceEpoch; break;
            case 7: ++bad.identity.motionRevision; break;
            case 8: ++bad.identity.osgContext; break;
            case 9: ++bad.width; break;
            case 10: ++bad.height; break;
        }
        require(owner.withBorrow(bad, [&](const auto&) { ++calls; }) == DiagnosticBorrowStatus::MismatchedRequest,
            "Mismatched exact draw request borrowed inputs");
    }
    require(calls == 1 && !owner.borrowed(), "Refused requests changed the ordinary fallback");

    bool caught = false;
    try
    {
        owner.withBorrow(expected, [&](const auto&) { throw std::runtime_error("Intentional consumer failure"); });
    }
    catch (const std::runtime_error&) { caught = true; }
    require(caught && !owner.borrowed(), "Consumer exception did not release its scope");
    require(owner.withBorrow(expected, [&](const auto&) {}) == DiagnosticBorrowStatus::Observed,
        "Consumer exception poisoned future observation");

    std::thread wrongThread([&] {
        require(owner.publish(inputs) == DiagnosticBorrowStatus::WrongThread, "Foreign thread published metadata");
        require(owner.withBorrow(expected, [&](const auto&) { ++calls; }) == DiagnosticBorrowStatus::WrongThread,
            "Foreign thread borrowed frame inputs");
        require(!owner.retire() && !owner.borrowed(), "Foreign thread changed or inspected owner state");
    });
    wrongThread.join();
    require(owner.withBorrow(expected, [&](const auto& borrow) {
        bool refused = false;
        std::thread other([&] { refused = borrow.sources() == nullptr; });
        other.join();
        require(refused && borrow.sources(), "Foreign-thread view exposed frame data or damaged owner view");
    }) == DiagnosticBorrowStatus::Observed, "Owner view was unavailable after thread rejection");

    for (unsigned field = 0; field < 7; ++field)
    {
        auto bad = inputs;
        switch (field)
        {
            case 0: bad.identity.submission = 0; break;
            case 1: bad.identity.ticket = 0; break;
            case 2: bad.identity.view = 0; break;
            case 3: bad.identity.nativeContext = 0; break;
            case 4: bad.identity.contextEpoch = 0; break;
            case 5: bad.identity.resourceEpoch = 0; break;
            case 6: bad.identity.motionRevision = 0; break;
        }
        sourceRejected(bad, DiagnosticBorrowStatus::InvalidIdentity, "Incomplete draw identity accepted");
    }
    for (unsigned image = 0; image < 3; ++image)
    {
        for (unsigned field = 0; field < 8; ++field)
        {
            auto bad = inputs;
            auto& changed = bad.images[image];
            switch (field)
            {
                case 0: changed.id = 0; break;
                case 1: changed.generation = 0; break;
                case 2: ++changed.frame; break;
                case 3: ++changed.view; break;
                case 4: ++changed.nativeContext; break;
                case 5: ++changed.contextEpoch; break;
                case 6: ++changed.width; break;
                case 7: ++changed.height; break;
            }
            const auto status = field < 2 ? DiagnosticBorrowStatus::MissingImage
                : field < 6 ? DiagnosticBorrowStatus::MismatchedImageOwner : DiagnosticBorrowStatus::InvalidExtent;
            sourceRejected(bad, status, "Unmatched source image accepted");
        }
        auto aliased = inputs;
        aliased.images[image].id = aliased.images[(image + 1) % 3].id;
        sourceRejected(aliased, DiagnosticBorrowStatus::AliasedImages, "Aliased source images accepted");
        auto unknown = inputs; unknown.images[image].format = DiagnosticFormat::Unknown;
        sourceRejected(unknown, DiagnosticBorrowStatus::UnsupportedFormat, "Unknown source format accepted");
    }
    for (unsigned meaning = 0; meaning < 4; ++meaning)
    {
        auto bad = inputs;
        if (meaning == 0) bad.colorStage = DiagnosticColorStage::Unknown;
        else if (meaning == 1) bad.coverage = DiagnosticMotionCoverage::Unknown;
        else if (meaning == 2) bad.motionSpace = DiagnosticMotionSpace::Unknown;
        else bad.depthRange = DiagnosticDepthRange::Unknown;
        sourceRejected(bad, DiagnosticBorrowStatus::UnsupportedMeaning, "Unknown diagnostic input meaning accepted");
    }
    auto unsubmitted = inputs; unsubmitted.producerSubmitted = false;
    sourceRejected(unsubmitted, DiagnosticBorrowStatus::UnsubmittedProducer, "Unsubmitted source accepted");
    auto oversized = inputs;
    for (auto& image : oversized.images) image.width = 1921;
    sourceRejected(oversized, DiagnosticBorrowStatus::InvalidExtent, "Probe extent limit bypassed");

    const std::uint64_t bytes = 1280ull * 720 * 12;
    DiagnosticInputBorrowOwner exactBudget({1920, 1080, bytes});
    require(exactBudget.publish(inputs) == DiagnosticBorrowStatus::Observed, "Exact byte budget was declined");
    DiagnosticInputBorrowOwner belowBudget({1920, 1080, bytes - 1});
    require(belowBudget.publish(inputs) == DiagnosticBorrowStatus::OverBudget, "One byte below bound admitted");
    DiagnosticInputBorrowOwner overflowBudget({0xffffffffu, 0xffffffffu, 32ull * 1024 * 1024});
    auto huge = inputs;
    for (auto& image : huge.images) image.width = image.height = 0xffffffffu;
    require(overflowBudget.publish(huge) == DiagnosticBorrowStatus::OverBudget, "Extent arithmetic wrapped budget");

    require(owner.retire(), "Idle diagnostic retirement refused");
    require(owner.withBorrow(expected, [&](const auto&) { ++calls; }) == DiagnosticBorrowStatus::MissingPublication,
        "Retired sources became available");
    require(owner.publish(inputs) == DiagnosticBorrowStatus::StalePublication,
        "Retirement permitted stale same-frame republication");
    auto next = inputs; advance(next);
    next.coverage = DiagnosticMotionCoverage::PartialOpaque;
    require(owner.publish(next) == DiagnosticBorrowStatus::Observed, "Next partial opaque frame publication refused");
    require(owner.withBorrow(request(next), [&](const auto& borrow) {
        require(borrow.sources()->coverage == DiagnosticMotionCoverage::PartialOpaque, "Partial coverage lost");
    }) == DiagnosticBorrowStatus::Observed, "Next partial frame borrow failed");

    for (unsigned change = 0; change < 5; ++change)
    {
        auto stale = next; advance(stale);
        if (change == 0) --stale.identity.contextEpoch;
        else if (change == 1) ++stale.identity.nativeContext;
        else if (change == 2) ++stale.identity.osgContext;
        else if (change == 3) --stale.identity.resourceEpoch;
        else --stale.identity.motionRevision;
        for (auto& image : stale.images)
        {
            image.nativeContext = stale.identity.nativeContext;
            image.contextEpoch = stale.identity.contextEpoch;
        }
        require(owner.publish(stale) == DiagnosticBorrowStatus::StalePublication, "Stale lifetime/generation accepted");
    }
    auto recreated = source();
    recreated.identity.contextEpoch = next.identity.contextEpoch + 1;
    for (auto& image : recreated.images) image.contextEpoch = recreated.identity.contextEpoch;
    require(owner.publish(recreated) == DiagnosticBorrowStatus::Observed,
        "Actual new context epoch could not restart submission tickets");
    require(owner.withBorrow(request(next), [&](const auto&) { ++calls; }) == DiagnosticBorrowStatus::MismatchedRequest,
        "Old descriptor matched reused GL names in a new context lifetime");

    for (const auto depth : {DiagnosticFormat::Depth24Stencil8, DiagnosticFormat::Depth32FloatStencil8,
             DiagnosticFormat::Depth32Float})
        for (const auto color : {DiagnosticFormat::Rgba8, DiagnosticFormat::Rgba16Float})
            for (const bool reversed : {false, true})
            {
                DiagnosticInputBorrowOwner formats;
                auto value = source(); value.images[0].format = color; value.images[1].format = depth;
                value.depthInverted = reversed;
                value.depthRange = reversed ? DiagnosticDepthRange::ZeroToOne : DiagnosticDepthRange::NegativeOneToOne;
                require(formats.publish(value) == DiagnosticBorrowStatus::Observed, "Supported diagnostic format refused");
                require(formats.withBorrow(request(value), [&](const auto& borrow) {
                    require(borrow.sources()->depthInverted == reversed, "Probe changed depth convention");
                }) == DiagnosticBorrowStatus::Observed, "Supported diagnostic format borrow refused");
            }
    require(calls == 1 && owner.retire(), "Refusals invoked fallback work or blocked final retirement");
    std::cout << "Diagnostic input borrow checks passed: " << checks << '\n';
    std::cout << "Metadata-only proof; GL import, game engagement and NGX evaluation remain unproved.\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << "Diagnostic input borrow failed: " << error.what() << '\n';
    return 1;
}
