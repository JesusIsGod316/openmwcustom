// Match the failing game include boundary before any other platform headers.
#include <components/debug/runtimeprocessmemory.hpp>
#include <components/misc/hostmemory.hpp>
#if defined(near) || defined(far) || defined(_WINDOWS_) || defined(_INC_WINDOWS)
#error "Public process-memory header leaked the Windows SDK"
#endif

#if defined(OPENMW_TEST_KEYWORDS) && !defined(OPENMW_TEST_KEYWORDS_AFTER)
#include "test-keywords.hpp"
#endif
#include <components/debug/runtimediagnostics.hpp>
#include <components/debug/gameplaydiagnostics.hpp>
#include <components/resource/hostmemorybudget.hpp>
#if defined(OPENMW_TEST_KEYWORDS) && defined(OPENMW_TEST_KEYWORDS_AFTER)
#include "test-keywords.hpp"
#endif

// This is the exact parameter/body pattern rejected in stereomanager.hpp.
// It tests the shared boundary without requiring OSG or a graphical device.
struct ClipRange
{
    float mNear = 0, mFar = 0;
    void updateSettings(float near, float far)
    {
        mNear = near;
        mFar = far;
    }
};

#if defined(OPENMW_TEST_KEYWORDS)
#ifndef emit
#error "The diagnostic header must not undefine Qt emit"
#endif
#ifndef signals
#error "The diagnostic header must not undefine Qt signals"
#endif
#ifndef slots
#error "The diagnostic header must not undefine Qt slots"
#endif
struct QtKeywordConsumer
{
    bool called = false;
public slots:
    void receive() { called = true; }
signals:
    void unusedSignal();
};
#endif

int main()
{
    ClipRange range;
    range.updateSettings(1.0f, 2.0f);
    if (range.mNear != 1.0f || range.mFar != 2.0f) return 1;
#if defined(OPENMW_TEST_KEYWORDS)
    QtKeywordConsumer consumer;
    emit consumer.receive();
    if (!consumer.called) return 2;
#endif
    // Reference and call the out-of-line implementation to test linkage too.
    auto* probe = &Debug::RuntimeDiagnostics::sampleProcessMemory;
    probe();
    const auto hostMemory = Misc::queryHostMemoryStatus();
    if (hostMemory.physicalValid && hostMemory.physicalAvailable > hostMemory.physicalTotal) return 3;
    Debug::RuntimeDiagnostics::recordEvent("header_test", "consumer", {}, {{"value", 1}});
    Debug::GameplayDiagnostics::recordEvent("header_test", {{"value", "1"}}, true);
    return 0;
}
