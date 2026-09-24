#ifndef OPENMW_RENDER_VSG_PARALLELRECORDTASK_H
#define OPENMW_RENDER_VSG_PARALLELRECORDTASK_H

#include "vsgsubmission.hpp"
#include <components/rendercore/boundedparallelfor.hpp>

namespace RenderVsg
{
    // Only independent views may be assigned separate command graphs here.
    // They own their traversal, bins, command pools and view-dependent data;
    // their shared scene is read-only until every recorder has joined. GPU
    // execution remains one ordered queue submission and one completion ring.
    class ParallelRecordTask : public vsg::Inherit<vsg::RecordAndSubmitTask, ParallelRecordTask>
    {
    public:
        explicit ParallelRecordTask(const vsg::RecordAndSubmitTask& source)
            : Inherit(source.device.get(), static_cast<std::uint32_t>(VsgRecordAndSubmitRingSize))
            , mWorkers(2, 2, 1)
        {
            if (source.index() != VsgRecordAndSubmitRingSize)
                throw std::invalid_argument("parallel recording task must be installed before the first frame");
            windows = source.windows;
            waitSemaphores = source.waitSemaphores;
            signalSemaphores = source.signalSemaphores;
            commandGraphs = source.commandGraphs;
            transferTask = source.transferTask;
            earlyTransferConsumerCompletedSemaphore = source.earlyTransferConsumerCompletedSemaphore;
            lateTransferConsumerCompletedSemaphore = source.lateTransferConsumerCompletedSemaphore;
            queue = source.queue;
            databasePager = source.databasePager;
            instrumentation = source.instrumentation;
        }

        VkResult record(vsg::ref_ptr<vsg::RecordedCommandBuffers> recorded,
            vsg::ref_ptr<vsg::FrameStamp> frame) override
        {
            // External instrumentation may require a single calling thread.
            // Preserve it rather than sharing an unknown profiler across workers.
            if (instrumentation && !dynamic_cast<SubmitTaskDiagnostics*>(instrumentation.get()))
                return vsg::RecordAndSubmitTask::record(recorded, frame);
            Debug::GameplayDiagnostics::Stage stage("submit_record");
            mWorkers.forEach(commandGraphs.size(), [&](std::size_t index) {
                commandGraphs[index]->record(recorded, frame, databasePager);
            });
            return VK_SUCCESS;
        }

    private:
        RenderCore::BoundedParallelFor mWorkers;
    };
}
#endif
