#ifndef OPENMW_RENDER_VSG_PARALLELSHADOWRECORD_H
#define OPENMW_RENDER_VSG_PARALLELSHADOWRECORD_H

#include "vsgsubmission.hpp"
#include <components/rendercore/boundedparallelfor.hpp>
#include <vsg/app/RecordTraversal.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/nodes/Switch.h>
#include <vsg/vk/State.h>

namespace RenderVsg
{
    // Keep the compiled shadow graph visible to VSG's resource/compile visitors.
    // Only recording is partitioned, after the owner has fitted every camera.
    // One whole cascade per job: each owns traversal, bins, command pools and
    // view-dependent data; shared scene/resource objects remain read-only.
    class ParallelShadowRecordGraph final
        : public vsg::Inherit<vsg::CommandGraph, ParallelShadowRecordGraph>
    {
    public:
        ParallelShadowRecordGraph(const vsg::CommandGraph& source, vsg::ref_ptr<vsg::Switch> selection)
            : Inherit(source.device, source.queueFamily), mSelection(std::move(selection)), mWorkers(2, 2, 1)
        {
            window = source.window;
            presentFamily = source.presentFamily;
            submitOrder = source.submitOrder;
            maxSlots = source.maxSlots;
            instrumentation = source.instrumentation;
            children = source.children;
        }

        void record(vsg::ref_ptr<vsg::RecordedCommandBuffers> recorded,
            vsg::ref_ptr<vsg::FrameStamp> frame, vsg::ref_ptr<vsg::DatabasePager> pager = {}) override
        {
            // Unknown instrumentation and noncanonical graphs keep stock VSG.
            if (!recorded || (instrumentation && !dynamic_cast<SubmitTaskDiagnostics*>(instrumentation.get()))
                || !prepare())
            {
                vsg::CommandGraph::record(recorded, frame, pager);
                return;
            }
            if (window && !window->visible()) return;
            const auto parent = getOrCreateRecordTraversal();
            std::vector<std::size_t> active;
            for (std::size_t i = 0; i < mJobs.size(); ++i)
            {
                auto& job = mJobs[i];
                if (job.cascade && !(mSelection->children[*job.cascade].mask
                        & (parent->traversalMask | parent->overrideMask))) continue;
                job.graph->maxSlots = maxSlots;
                const auto traversal = job.graph->getOrCreateRecordTraversal();
                // Mirror pinned VSG's nested CommandGraph inheritance, including
                // inherited eye/LOD matrices, not the shadow camera viewpoint.
                traversal->state->inherit(*parent->state);
                traversal->traversalMask = parent->traversalMask;
                traversal->overrideMask = parent->overrideMask;
                traversal->intensityMinimum = parent->intensityMinimum;
                active.push_back(i);
            }
            // Give each job a unique submit-order slot. This preserves
            // prefix/cascade/suffix order regardless of worker completion and
            // keeps shadows between water views and the main view. CommandGraph
            // publishes through VSG's internally synchronized collector API.
            const bool profiling = Debug::GameplayDiagnostics::enabled()
                && Debug::GameplayDiagnostics::frameProfileEnabled();
            std::vector<double> elapsed(profiling ? active.size() : 0);
            mWorkers.forEach(active.size(), [&](std::size_t index) {
                const auto start = profiling ? Debug::GameplayDiagnostics::Clock::now()
                    : Debug::GameplayDiagnostics::Clock::time_point{};
                mJobs[active[index]].graph->record(recorded, frame, pager);
                if (profiling) elapsed[index] = std::chrono::duration<double, std::milli>(
                    Debug::GameplayDiagnostics::Clock::now() - start).count();
            });
            if (profiling)
                for (std::size_t i = 0; i < active.size(); ++i)
                    Debug::GameplayDiagnostics::recordEvent("profile_record_shadow", {
                        {"job", std::to_string(active[i])},
                        {"semantic_frame", std::to_string(frame ? frame->frameCount : 0)},
                        {"cascade", mJobs[active[i]].cascade ? std::to_string(*mJobs[active[i]].cascade) : "wrapper"},
                        {"wall_ms", std::to_string(elapsed[i])}}, true);
        }

    private:
        struct Job
        {
            vsg::ref_ptr<vsg::CommandGraph> graph;
            std::optional<std::size_t> cascade;
        };
        bool prepare()
        {
            if (!mSelection || mSelection->children.size() < 2 || mSelection->children.size() > 8)
                return false;
            if (std::count(children.begin(), children.end(), mSelection) != 1) return false;
            for (const auto& child : mSelection->children)
                if (!child.node.cast<vsg::RenderGraph>()) return false;
            if (mLayout != children || mCascadeNodes.size() != mSelection->children.size()
                || !std::equal(mCascadeNodes.begin(), mCascadeNodes.end(), mSelection->children.begin(),
                    [](const auto& node, const auto& child) { return node == child.node; }))
            {
                mJobs.clear(); mLayout = children; mCascadeNodes.clear();
                const auto add = [&](vsg::ref_ptr<vsg::Node> node, std::optional<std::size_t> cascade) {
                    auto graph = vsg::CommandGraph::create(device, queueFamily);
                    // Reserve [-100, -91] for shadow recording. Water views
                    // occupy -400/-300 and the main view is 0.
                    graph->submitOrder = -100 + static_cast<int>(mJobs.size());
                    graph->addChild(node);
                    mJobs.push_back({std::move(graph), cascade});
                };
                for (const auto& child : children)
                    if (child == mSelection)
                        for (std::size_t i = 0; i < mSelection->children.size(); ++i)
                        {
                            mCascadeNodes.push_back(mSelection->children[i].node);
                            add(mSelection->children[i].node, i);
                        }
                    else add(child, {});
            }
            return true;
        }
        vsg::ref_ptr<vsg::Switch> mSelection;
        vsg::Group::Children mLayout, mCascadeNodes;
        std::vector<Job> mJobs;
        RenderCore::BoundedParallelFor mWorkers;
    };
}
#endif
