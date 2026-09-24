#ifndef OPENMW_RENDER_VSG_PIPELINEINVENTORY_H
#define OPENMW_RENDER_VSG_PIPELINEINVENTORY_H

#include <vsg/nodes/Group.h>
#include <vsg/core/ConstVisitor.h>
#include <vsg/app/RecordTraversal.h>
#include <vsg/state/GraphicsPipeline.h>
#include <unordered_set>

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
        const auto& pipelines() const { return mPipelines; }
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
}
#endif
