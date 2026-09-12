#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGSUBMISSION_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGSUBMISSION_H

#include "framecompletion.hpp"

#include <vsg/app/Viewer.h>
#include <vsg/app/Presentation.h>
#include <vsg/vk/Fence.h>
#include <vsg/vk/Queue.h>
#include <vsg/vk/Swapchain.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <vector>

namespace RenderVsg
{
    // VSG 1.1.15 Viewer::assignRecordAndSubmitTaskAndPresentation() constructs
    // RecordAndSubmitTask with this hard-coded buffer count.
    inline constexpr std::size_t VsgRecordAndSubmitRingSize = 3;

    struct VsgSubmitPresentResult
    {
        VkResult submit = VK_SUCCESS;
        VkResult present = VK_SUCCESS;

        [[nodiscard]] bool success() const noexcept
        {
            return submit == VK_SUCCESS && (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR);
        }
    };

    // VSG 1.1.15 does not mark Presentation with VSG_DECLSPEC, so calling
    // Presentation::present() directly fails to link against its Windows shared
    // library. Keep the checked path by reproducing that pinned implementation
    // through exported/public Queue, Window, Swapchain and Semaphore APIs.
    [[nodiscard]] inline VkResult presentChecked(vsg::Presentation& presentation)
    {
        if (!presentation.queue)
            return VK_ERROR_INITIALIZATION_FAILED;

        std::vector<VkSemaphore> semaphores;
        semaphores.reserve(presentation.waitSemaphores.size() + presentation.windows.size());
        for (const auto& semaphore : presentation.waitSemaphores)
        {
            if (!semaphore)
                return VK_ERROR_INITIALIZATION_FAILED;
            semaphores.push_back(semaphore->vk());
        }

        std::vector<VkSwapchainKHR> swapchains;
        std::vector<std::uint32_t> imageIndices;
        swapchains.reserve(presentation.windows.size());
        imageIndices.reserve(presentation.windows.size());
        for (const auto& window : presentation.windows)
        {
            if (!window)
                return VK_ERROR_INITIALIZATION_FAILED;
            const std::size_t imageIndex = window->imageIndex();
            if (!window->visible() || imageIndex >= window->numFrames())
                continue;
            const vsg::ref_ptr<vsg::Swapchain> swapchain = window->getOrCreateSwapchain();
            const vsg::ref_ptr<vsg::Semaphore>& renderFinished = window->frame(imageIndex).renderFinishedSemaphore;
            if (!swapchain || !renderFinished)
                return VK_ERROR_INITIALIZATION_FAILED;
            swapchains.push_back(swapchain->vk());
            imageIndices.push_back(static_cast<std::uint32_t>(imageIndex));
            semaphores.push_back(renderFinished->vk());
        }
        if (swapchains.empty())
            return VK_SUCCESS;

        VkPresentInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = static_cast<std::uint32_t>(semaphores.size());
        info.pWaitSemaphores = semaphores.data();
        info.swapchainCount = static_cast<std::uint32_t>(swapchains.size());
        info.pSwapchains = swapchains.data();
        info.pImageIndices = imageIndices.data();
        return presentation.queue->present(info);
    }

    // Viewer::recordAndSubmit() and Viewer::present() intentionally discard the
    // VkResult returned by VSG 1.1.15 tasks. The production semantic backend
    // needs truthful failure propagation, so its single-threaded path uses this
    // checked equivalent. A future threaded host must add result collection to
    // VSG's worker barrier instead of calling this helper concurrently.
    [[nodiscard]] inline VsgSubmitPresentResult submitAndPresentChecked(vsg::Viewer& viewer)
    {
        VsgSubmitPresentResult result;
        if (viewer.recordAndSubmitTasks.size() != 1 || viewer.presentations.size() != 1)
        {
            // Avoid an untrackable partial success if a later task fails. The
            // current production-shaped host owns exactly one swapchain/task;
            // multiview submission will need a transaction result per task.
            result.submit = VK_ERROR_INITIALIZATION_FAILED;
            return result;
        }
        vsg::FrameStamp* frameStamp = viewer.getFrameStamp();
        if (!frameStamp)
        {
            result.submit = VK_ERROR_INITIALIZATION_FAILED;
            return result;
        }

        for (const auto& task : viewer.recordAndSubmitTasks)
        {
            if (!task)
            {
                result.submit = VK_ERROR_INITIALIZATION_FAILED;
                return result;
            }
            for (auto& commandGraph : task->commandGraphs)
                commandGraph->reset();
            result.submit = task->submit(vsg::ref_ptr<vsg::FrameStamp>(frameStamp));
            if (result.submit != VK_SUCCESS)
                return result;
        }
        for (const auto& presentation : viewer.presentations)
        {
            if (!presentation)
            {
                result.present = VK_ERROR_INITIALIZATION_FAILED;
                return result;
            }
            result.present = presentChecked(*presentation);
            if (result.present != VK_SUCCESS && result.present != VK_SUBOPTIMAL_KHR)
                return result;
        }
        return result;
    }

    struct VsgCompletionPoll
    {
        VkResult result = VK_SUCCESS;
        std::optional<RenderCore::FrameId> completedThrough;
    };

    // Exact VSG 1.1.15 fence bridge. pollBeforeRecordAndSubmit() must run after
    // Viewer::advanceToNextFrame() and before RecordAndSubmitTask::start() can
    // reset the ring slot selected by that advance. A hidden/minimized frame can
    // advance VSG's task ring without submitting, so the current ring slot is not
    // necessarily the oldest logical submission. Detect reuse by fence identity
    // and retire every completed logical frame before VSG is allowed to reset
    // that fence. No device-wide idle is required during normal mutation/streaming.
    class VsgSubmissionCompletion
    {
    public:
        explicit VsgSubmissionCompletion(std::size_t maximumFramesInFlight = VsgRecordAndSubmitRingSize)
            : mTracker(maximumFramesInFlight)
        {
        }

        [[nodiscard]] VsgCompletionPoll pollBeforeRecordAndSubmit(
            vsg::Viewer& viewer, std::uint64_t reuseWaitTimeout = std::numeric_limits<std::uint64_t>::max())
        {
            VsgCompletionPoll poll = pollReady();
            if (poll.result != VK_SUCCESS)
                return poll;

            bool reusesTrackedFence = false;
            for (const auto& task : viewer.recordAndSubmitTasks)
            {
                const vsg::ref_ptr<vsg::Fence> current = task ? task->fence(0) : nullptr;
                if (!current)
                    continue;
                for (const Submission& submission : mSubmissions)
                {
                    if (std::find(submission.fences.begin(), submission.fences.end(), current)
                        != submission.fences.end())
                    {
                        reusesTrackedFence = true;
                        break;
                    }
                }
                if (reusesTrackedFence)
                    break;
            }

            if (!reusesTrackedFence)
                return poll;

            // The selected VSG ring slot still represents a tracked logical
            // submission. Wait for that exact slot before RecordAndSubmitTask::start()
            // resets/reuses its Fence object. Queue order guarantees earlier
            // submissions are also complete; pollReady() validates that rather
            // than assuming it.
            poll.result = viewer.waitForFences(0, reuseWaitTimeout);
            if (poll.result != VK_SUCCESS)
                return poll;
            VsgCompletionPoll afterWait = pollReady();
            if (afterWait.completedThrough)
                poll.completedThrough = afterWait.completedThrough;
            poll.result = afterWait.result;
            if (poll.result != VK_SUCCESS)
                return poll;

            // A waited current fence must no longer be represented by any
            // tracked submission. If it is, allowing VSG to reset it would make
            // the logical completion timeline alias a newer submission.
            for (const auto& task : viewer.recordAndSubmitTasks)
            {
                const vsg::ref_ptr<vsg::Fence> current = task ? task->fence(0) : nullptr;
                if (!current)
                    continue;
                for (const Submission& submission : mSubmissions)
                {
                    if (std::find(submission.fences.begin(), submission.fences.end(), current)
                        != submission.fences.end())
                    {
                        poll.result = VK_ERROR_UNKNOWN;
                        return poll;
                    }
                }
            }
            return poll;
        }

        [[nodiscard]] bool canRegisterSubmission(RenderCore::FrameId frame) const noexcept
        {
            return mTracker.canSubmit(frame);
        }

        // Call immediately after Viewer::recordAndSubmit() for a frame that will
        // be treated as submitted by the semantic renderer.
        [[nodiscard]] bool registerSubmission(vsg::Viewer& viewer, RenderCore::FrameId frame)
        {
            Submission submission;
            submission.frame = frame;
            submission.fences.reserve(viewer.recordAndSubmitTasks.size());
            for (const auto& task : viewer.recordAndSubmitTasks)
                submission.fences.push_back(task ? task->fence(0) : nullptr);
            mSubmissions.push_back(std::move(submission));
            if (!mTracker.submit(frame))
            {
                mSubmissions.pop_back();
                return false;
            }
            return true;
        }

        [[nodiscard]] const FrameCompletionTracker& tracker() const noexcept { return mTracker; }

    private:
        [[nodiscard]] VsgCompletionPoll pollReady()
        {
            VsgCompletionPoll poll;
            while (!mSubmissions.empty())
            {
                bool complete = true;
                for (const auto& fence : mSubmissions.front().fences)
                {
                    if (!fence || !fence->hasDependencies())
                        continue;
                    const VkResult status = fence->status();
                    if (status == VK_NOT_READY)
                    {
                        complete = false;
                        break;
                    }
                    if (status != VK_SUCCESS)
                    {
                        poll.result = status;
                        return poll;
                    }
                }
                if (!complete)
                    break;
                const RenderCore::FrameId frame = mSubmissions.front().frame;
                if (!mTracker.completeThrough(frame))
                {
                    poll.result = VK_ERROR_UNKNOWN;
                    return poll;
                }
                poll.completedThrough = frame;
                mSubmissions.pop_front();
            }
            return poll;
        }
        struct Submission
        {
            RenderCore::FrameId frame;
            std::vector<vsg::ref_ptr<vsg::Fence>> fences;
        };

        FrameCompletionTracker mTracker;
        std::deque<Submission> mSubmissions;
    };

    // Compile first and publish the CompileResult to every affected VSG task.
    // The caller attaches the object to the live root only after this succeeds.
    [[nodiscard]] inline vsg::CompileResult compileForViewer(vsg::Viewer& viewer, vsg::ref_ptr<vsg::Object> object)
    {
        if (!viewer.compileManager || !object)
            return {};
        vsg::CompileResult result = viewer.compileManager->compile(std::move(object));
        if (result && result.requiresViewerUpdate(&viewer))
            vsg::updateViewer(viewer, result);
        return result;
    }
}

#endif
