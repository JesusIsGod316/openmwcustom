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
            const bool profiling = Debug::FrameProfile::accumulator.active;
            std::vector<double> wallMs(profiling ? commandGraphs.size() : 0);
            mWorkers.forEach(commandGraphs.size(), [&](std::size_t index) {
                const auto start = profiling ? Debug::GameplayDiagnostics::Clock::now()
                    : Debug::GameplayDiagnostics::Clock::time_point{};
                commandGraphs[index]->record(recorded, frame, databasePager);
                if (profiling) wallMs[index] = std::chrono::duration<double, std::milli>(
                    Debug::GameplayDiagnostics::Clock::now() - start).count();
            });
            if (profiling)
                for (std::size_t i = 0; i < wallMs.size(); ++i)
                    Debug::GameplayDiagnostics::recordEvent("profile_record_graph", {
                        {"index", std::to_string(i)}, {"order", std::to_string(commandGraphs[i]->submitOrder)},
                        {"wall_ms", std::to_string(wallMs[i])}});
            return VK_SUCCESS;
        }

    private:
        RenderCore::BoundedParallelFor mWorkers;
    };
}
#endif
