#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGSUBMISSION_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGSUBMISSION_H

#include "framecompletion.hpp"
#include <components/debug/gameplaydiagnostics.hpp>

#include <vsg/app/Presentation.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/core/ConstVisitor.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/ViewDependentState.h>
#include <vsg/vk/Framebuffer.h>
#include <vsg/vk/Context.h>
#include <vsg/vk/Fence.h>
#include <vsg/vk/Queue.h>
#include <vsg/vk/Swapchain.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace RenderVsg
{
    // VSG 1.1.15 Viewer::assignRecordAndSubmitTaskAndPresentation() constructs
    // RecordAndSubmitTask with this hard-coded buffer count.
    inline constexpr std::size_t VsgRecordAndSubmitRingSize = 3;

    struct GraphicsPipelineAudit
    {
        std::size_t bindings = 0;
        std::vector<std::string> unrealized;

        [[nodiscard]] bool valid() const noexcept { return unrealized.empty(); }
    };

    class GraphicsPipelineViewValidator final : public vsg::ConstVisitor
    {
    public:
        explicit GraphicsPipelineViewValidator(std::uint32_t viewId)
            : mViewId(viewId)
        {
        }

        using vsg::ConstVisitor::apply;

        void apply(const vsg::Object& object) override
        {
            object.traverse(*this);
        }

        void apply(const vsg::BindGraphicsPipeline& bind) override
        {
            ++mAudit.bindings;
            const void* identity = bind.pipeline.get();
            if ((!bind.pipeline || bind.pipeline->validated_vk(mViewId) == VK_NULL_HANDLE)
                && mReported.insert(identity).second)
            {
                std::string family = "unlabelled";
                std::string source;
                if (bind.pipeline)
                {
                    (void)bind.pipeline->getValue("openmw.pipeline.family", family);
                    (void)bind.pipeline->getValue("openmw.pipeline.source", source);
                }
                std::ostringstream message;
                message << family;
                if (!source.empty())
                    message << " [" << source << ']';
                message << " pipeline=" << identity << " view=" << mViewId;
                mAudit.unrealized.push_back(message.str());
            }
            bind.traverse(*this);
        }

        [[nodiscard]] GraphicsPipelineAudit take() { return std::move(mAudit); }

    private:
        std::uint32_t mViewId;
        GraphicsPipelineAudit mAudit;
        std::unordered_set<const void*> mReported;
    };

    [[nodiscard]] inline GraphicsPipelineAudit auditGraphicsPipelinesForView(
        const vsg::Object& object, const vsg::View& view)
    {
        GraphicsPipelineViewValidator validator(view.viewID);
        object.accept(validator);
        return validator.take();
    }

    [[nodiscard]] inline bool graphicsPipelinesRealizedForView(
        const vsg::Object& object, const vsg::View& view)
    {
        return auditGraphicsPipelinesForView(object, view).valid();
    }

    struct VsgSubmitPresentResult
    {
        VkResult submit = VK_SUCCESS;
        VkResult present = VK_SUCCESS;

        [[nodiscard]] bool success() const noexcept
        {
            // Once queue submission succeeded, OUT_OF_DATE/full-screen-loss are
            // swapchain transition results rather than semantic-frame failures.
            // Treat the rendered frame as consumed so frame IDs/lifetimes remain
            // monotonic; the next extent/acquire pass rebuilds the swapchain.
            return submit == VK_SUCCESS
                && (present == VK_SUCCESS || present == VK_SUBOPTIMAL_KHR
                    || present == VK_ERROR_OUT_OF_DATE_KHR
                    || present == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT);
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
            if (imageIndex >= window->numFrames())
                continue;
            const vsg::ref_ptr<vsg::Swapchain> swapchain = window->getOrCreateSwapchain();
            const vsg::ref_ptr<vsg::Semaphore>& renderFinished = window->frame(imageIndex).renderFinishedSemaphore;
            if (!swapchain || !renderFinished)
                return VK_ERROR_INITIALIZATION_FAILED;
            swapchains.push_back(swapchain->vk());
            imageIndices.push_back(static_cast<std::uint32_t>(imageIndex));
            semaphores.push_back(renderFinished->vk());
        }
        // A successful frame advance in the production host owns one acquired
        // swapchain image. If that invariant is lost, fail closed rather than
        // reporting a presentation that never consumed renderFinished.
        if (swapchains.empty())
            return VK_ERROR_INITIALIZATION_FAILED;

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
            {
                Debug::GameplayDiagnostics::Stage stage("submit_task");
                result.submit = task->submit(vsg::ref_ptr<vsg::FrameStamp>(frameStamp));
            }
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
            {
                Debug::GameplayDiagnostics::Stage stage("submit_present_queue");
                result.present = presentChecked(*presentation);
            }
            if (result.present != VK_SUCCESS && result.present != VK_SUBOPTIMAL_KHR
                && result.present != VK_ERROR_OUT_OF_DATE_KHR
                && result.present != VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
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

    inline void updateViewerAfterCompile(vsg::Viewer& viewer, const vsg::CompileResult& result)
    {
        if (!result)
            return;
        if (result.requiresViewerUpdate(&viewer))
            vsg::updateViewer(viewer, result);

        // VSG 1.1.15 updateTasks grows only top-level CommandGraph::maxSlots.
        // Shadow pre-render graphs keep the limits collected at startup. After
        // a GUI-only startup (slots 0/1), later material set 1 lives in slot 2
        // and is silently omitted from shadow draws unless this limit grows.
        // The old water/sky PushConstants happened to occupy slot 2 and masked
        // this defect. Uniform buffers correctly use slot 1 instead.
        const char* control = std::getenv("OPENMW_V4_STALE_SHADOW_SLOTS");
        if (control && std::string_view(control) == "1")
            return; // Isolated regression control, never enabled by the launcher.
        for (const auto& [view, details] : result.views)
        {
            if (view && view->viewDependentState && view->viewDependentState->preRenderCommandGraph)
                view->viewDependentState->preRenderCommandGraph->maxSlots.update(result.maxSlots);
        }
    }

    // Compile first and publish the CompileResult to every affected VSG task.
    // The caller attaches the object to the live root only after this succeeds.
    [[nodiscard]] inline vsg::CompileResult compileForViewer(vsg::Viewer& viewer, vsg::ref_ptr<vsg::Object> object)
    {
        if (!viewer.compileManager || !object)
            return {};
        // Context::viewDependentState is a raw pointer in VSG 1.1.15. Never
        // dereference it after its owning View has gone away. Keep selected
        // Views alive through compilation and reject stale registrations rather
        // than silently publishing an incompletely prepared scene.
        std::vector<vsg::ref_ptr<vsg::View>> liveViews;
        bool invalidContext = false;
        vsg::CompileResult result = viewer.compileManager->compile(std::move(object),
            [&](vsg::Context& context) {
                auto view = context.view.ref_ptr();
                if (context.viewDependentState
                    && (!view || context.viewDependentState != view->viewDependentState.get()))
                {
                    invalidContext = true;
                    return false;
                }
                if (view)
                    liveViews.push_back(std::move(view));
                return true;
            });
        if (invalidContext)
        {
            result.result = VK_ERROR_INITIALIZATION_FAILED;
            result.message = "Stale VSG view compilation context after view retirement";
            return result;
        }
        updateViewerAfterCompile(viewer, result);
        return result;
    }

    // Main-view-only publications such as MyGUI must not materialize their
    // graphics pipelines against reflection, refraction, shadow or auxiliary
    // render passes that will never record them. CompileManager owns all live
    // contexts, so select the exact VSG View identity before publication.
    [[nodiscard]] inline vsg::CompileResult compileForViewerView(
        vsg::Viewer& viewer, const vsg::View& view, vsg::ref_ptr<vsg::Object> object)
    {
        if (!viewer.compileManager || !object)
            return {};
        const std::uint32_t viewId = view.viewID;
        bool matchedContext = false;
        vsg::CompileResult result = viewer.compileManager->compile(std::move(object),
            [&view, &matchedContext](vsg::Context& context) {
                const bool matches = context.viewID == view.viewID && context.view.ref_ptr().get() == &view;
                matchedContext = matchedContext || matches;
                return matches;
            });
        // VSG 1.1.15 reports success even when a selector matches zero contexts.
        // A no-op is not successful preparation of a renderable view.
        if (!matchedContext)
        {
            result.result = VK_ERROR_INITIALIZATION_FAILED;
            result.message = "No registered compile context for VSG view " + std::to_string(viewId);
            return result;
        }
        updateViewerAfterCompile(viewer, result);
        return result;
    }

    // A framebuffer/view introduced after Viewer::compile() is absent from the
    // CompileManager's persistent contexts. Register it before compiling the
    // graph; otherwise CompileTraversal skips that View and VSG's unchecked
    // GraphicsPipeline::vk(viewID) dereferences an empty implementation during
    // the first record traversal.
    [[nodiscard]] inline vsg::CompileResult compileForNewFramebufferView(vsg::Viewer& viewer,
        vsg::Framebuffer& framebuffer, vsg::ref_ptr<vsg::View> view, vsg::ref_ptr<vsg::Object> object)
    {
        if (!viewer.compileManager || !view || !object)
            return {};
        viewer.compileManager->add(framebuffer, view);
        return compileForViewerView(viewer, *view, std::move(object));
    }
}

#endif
