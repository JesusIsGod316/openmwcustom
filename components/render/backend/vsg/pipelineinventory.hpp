#ifndef OPENMW_RENDER_VSG_PIPELINEINVENTORY_H
#define OPENMW_RENDER_VSG_PIPELINEINVENTORY_H

#include <vsg/nodes/Group.h>
#include <vsg/core/ConstVisitor.h>
#include <vsg/app/RecordTraversal.h>
#include <vsg/state/GraphicsPipeline.h>
#include <unordered_set>
#include <unordered_map>
#include <vector>

namespace RenderVsg
{
    // Sealed draw topology. Array contents and placement may change, but adding
    // or replacing pipeline bindings requires a new inventory node. Built once
    // at resource realization, freed with that resource, never a global cache.
    // Auditors inspect the live Vulkan handles in this compact inventory for
    // EACH view; no cached "passed" flag survives release/recompile/view reuse.
    class PipelineInventoryNode final : public vsg::Inherit<vsg::Node, PipelineInventoryNode>
    {
    public:
        // Caller owns sealed topology and has already composed its exact
        // pipeline set. Values are handles, never cached per-view QC results.
        PipelineInventoryNode(vsg::ref_ptr<vsg::Node> child,
            std::vector<vsg::ref_ptr<vsg::GraphicsPipeline>> pipelines)
            : mChild(std::move(child)), mPipelines(std::move(pipelines)) {}
        explicit PipelineInventoryNode(vsg::ref_ptr<vsg::Node> child) : mChild(std::move(child))
        {
            struct Gather final : vsg::ConstVisitor
            {
                std::vector<vsg::ref_ptr<vsg::GraphicsPipeline>> pipelines;
                std::unordered_set<const vsg::Object*> visited;
                using vsg::ConstVisitor::apply;
                void apply(const vsg::Object& o) override
                {
                    if (!visited.insert(&o).second) return;
                    if (const auto* inventory = dynamic_cast<const PipelineInventoryNode*>(&o))
                    {
                        for (const auto& pipeline : inventory->pipelines())
                            if (visited.insert(pipeline.get()).second) pipelines.push_back(pipeline);
                        return;
                    }
                    o.traverse(*this);
                }
                void apply(const vsg::BindGraphicsPipeline& bind) override
                {
                    if (visited.insert(bind.pipeline.get()).second) pipelines.emplace_back(bind.pipeline);
                }
            } gather;
            mChild->accept(gather); mPipelines = std::move(gather.pipelines);
        }
        void traverse(vsg::Visitor& v) override { mChild->accept(v); }
        void traverse(vsg::ConstVisitor& v) const override { mChild->accept(v); }
        void traverse(vsg::RecordTraversal& v) const override { mChild->accept(v); }
        // Gather's local class calls this before the enclosing class is complete.
        // Keep the return type explicit; GCC cannot deduce a later auto return here.
        const std::vector<vsg::ref_ptr<vsg::GraphicsPipeline>>& pipelines() const { return mPipelines; }
    private:
        vsg::ref_ptr<vsg::Node> mChild;
        std::vector<vsg::ref_ptr<vsg::GraphicsPipeline>> mPipelines;
    };

    inline vsg::ref_ptr<vsg::Group> sealPipelineInventory(vsg::ref_ptr<vsg::Group> resource)
    {
        auto root = vsg::Group::create();
        root->addChild(PipelineInventoryNode::create(resource));
        return root;
    }

    // For immutable resident roots: discover only added/replaced residents and
    // adjust pipeline reference counts for removals. Never traverse a retained
    // resident just because a different object's transform changed. The next
    // snapshot is prepared before scene publication; the old one is untouched.
    class RetainedPipelineInventory
    {
    public:
        RetainedPipelineInventory() = default;
        RetainedPipelineInventory(vsg::ref_ptr<vsg::Group> topology, const RetainedPipelineInventory& previous)
            : mPipelines(previous.mPipelines)
        {
            mResidents.reserve(topology->children.size());
            for (const auto& child : topology->children)
            {
                if (mResidents.contains(child.get())) continue;
                const auto old = previous.mResidents.find(child.get());
                if (old != previous.mResidents.end())
                    mResidents.emplace(child.get(), old->second);
                else
                {
                    auto inventory = PipelineInventoryNode::create(child);
                    for (const auto& pipeline : inventory->pipelines())
                    {
                        auto& count = mPipelines[pipeline.get()];
                        count.pipeline = pipeline;
                        ++count.references;
                    }
                    mResidents.emplace(child.get(), std::move(inventory));
                    ++mDiscovered;
                }
            }
            for (const auto& [node, inventory] : previous.mResidents)
            {
                if (mResidents.contains(node)) continue;
                for (const auto& pipeline : inventory->pipelines())
                {
                    const auto count = mPipelines.find(pipeline.get());
                    if (--count->second.references == 0) mPipelines.erase(count);
                }
            }
            std::vector<vsg::ref_ptr<vsg::GraphicsPipeline>> pipelines;
            pipelines.reserve(mPipelines.size());
            for (const auto& [key, count] : mPipelines) pipelines.push_back(count.pipeline);
            mRoot = vsg::Group::create();
            mRoot->addChild(PipelineInventoryNode::create(std::move(topology), std::move(pipelines)));
        }
        const auto& root() const noexcept { return mRoot; }
        std::size_t discovered() const noexcept { return mDiscovered; }
        std::size_t residentCount() const noexcept { return mResidents.size(); }
    private:
        struct Count
        {
            vsg::ref_ptr<vsg::GraphicsPipeline> pipeline;
            std::size_t references = 0;
        };
        std::unordered_map<const vsg::Node*, vsg::ref_ptr<PipelineInventoryNode>> mResidents;
        std::unordered_map<const vsg::GraphicsPipeline*, Count> mPipelines;
        vsg::ref_ptr<vsg::Group> mRoot;
        std::size_t mDiscovered = 0;
    };
}
#endif
