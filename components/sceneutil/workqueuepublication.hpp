#ifndef OPENMW_COMPONENTS_SCENEUTIL_WORKQUEUEPUBLICATION_H
#define OPENMW_COMPONENTS_SCENEUTIL_WORKQUEUEPUBLICATION_H

#include <cstdint>

namespace SceneUtil
{
    // Process-wide diagnostic allocation failures after successful enqueue;
    // never a claim that the actual task failed. No queue layout/API changed.
    std::uint64_t workQueuePublicationDiagnosticFailures() noexcept;
#ifdef OPENMW_WORKQUEUE_PUBLICATION_TEST_HOOKS
    namespace WorkQueuePublicationTestHooks
    {
        inline void (*beforePublication)() = nullptr;
        inline void (*afterPublicationDiagnostic)() = nullptr;
    }
#endif
}

#endif
