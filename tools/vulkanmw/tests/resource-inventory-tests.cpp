#include <components/render/backend/vsg/pipelineinventory.hpp>
#include <vsg/nodes/StateGroup.h>
#include <iostream>
#include <stdexcept>

class CountedGraph : public vsg::Inherit<vsg::Group, CountedGraph>
{
public:
    mutable unsigned visits = 0;
    void traverse(vsg::ConstVisitor& v) const override { ++visits; vsg::Group::traverse(v); }
};
int main()
{
    try
    {
        const auto require = [](bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); };
        auto pipeline = vsg::GraphicsPipeline::create();
        auto state = vsg::StateGroup::create();
        state->add(vsg::BindGraphicsPipeline::create(pipeline));
        auto asset = CountedGraph::create(); asset->addChild(state);
        auto sealed = RenderVsg::sealPipelineInventory(asset);
        require(asset->visits == 1, "initial asset discovery missing");
        auto placements = vsg::Group::create();
        for (unsigned i = 0; i < 200; ++i) placements->addChild(sealed);
        auto inventory = RenderVsg::PipelineInventoryNode::create(placements);
        require(inventory->pipelines().size() == 1 && inventory->pipelines()[0] == pipeline, "shared pipeline dedup");
        require(asset->visits == 1, "root inventory revisited sealed asset topology");
        // Retained inventories accelerate only inventory discovery. Ordinary
        // compilation/resource visitors must still reach the actual graph.
        struct Walk : vsg::ConstVisitor
        {
            using vsg::ConstVisitor::apply;
            void apply(const vsg::Object& o) override { o.traverse(*this); }
        } walk;
        inventory->accept(walk);
        require(asset->visits > 1, "resource traversal hidden by inventory");
        auto otherPipeline = vsg::GraphicsPipeline::create();
        auto otherState = vsg::StateGroup::create();
        otherState->add(vsg::BindGraphicsPipeline::create(otherPipeline));
        auto replacement = vsg::Group::create(); replacement->addChild(otherState);
        auto newTopology = RenderVsg::sealPipelineInventory(replacement);
        auto updated = vsg::Group::create(); updated->addChild(newTopology);
        auto replaced = RenderVsg::PipelineInventoryNode::create(updated);
        require(replaced->pipelines().size() == 1 && replaced->pipelines()[0] == otherPipeline,
            "removed binding retained in replacement inventory");
        require(inventory->pipelines()[0] == pipeline, "old in-flight snapshot was mutated");
        auto scene = vsg::Group::create(); scene->addChild(asset); scene->addChild(replacement);
        RenderVsg::RetainedPipelineInventory empty;
        RenderVsg::RetainedPipelineInventory first(scene, empty);
        require(first.discovered() == 2 && first.residentCount() == 2, "initial resident inventories");
        const auto visits = asset->visits;
        RenderVsg::RetainedPipelineInventory same(scene, first);
        require(same.discovered() == 0 && asset->visits == visits, "unchanged resident rediscovery");
        auto removed = vsg::Group::create(); removed->addChild(replacement);
        RenderVsg::RetainedPipelineInventory second(removed, same);
        auto removedSet = RenderVsg::PipelineInventoryNode::create(second.root());
        require(second.discovered() == 0 && second.residentCount() == 1 && removedSet->pipelines().size() == 1
            && removedSet->pipelines()[0] == otherPipeline, "removal reference counts");
        auto shared = vsg::Group::create(); shared->addChild(state);
        auto sharedScene = vsg::Group::create(); sharedScene->addChild(asset); sharedScene->addChild(shared);
        RenderVsg::RetainedPipelineInventory sharedFirst(sharedScene, second);
        auto sharedRemoved = vsg::Group::create(); sharedRemoved->addChild(shared); sharedRemoved->addChild(shared);
        RenderVsg::RetainedPipelineInventory sharedSecond(sharedRemoved, sharedFirst);
        auto sharedSet = RenderVsg::PipelineInventoryNode::create(sharedSecond.root());
        require(sharedSecond.residentCount() == 1 && sharedSet->pipelines().size() == 1
            && sharedSet->pipelines()[0] == pipeline, "shared/duplicate references removed too early");
        RenderVsg::RetainedPipelineInventory cleared(vsg::Group::create(), sharedSecond);
        require(cleared.residentCount() == 0
            && RenderVsg::PipelineInventoryNode::create(cleared.root())->pipelines().empty(), "reset retains pipelines");
        require(RenderVsg::PipelineInventoryNode::create(first.root())->pipelines().size() == 2,
            "preparing replacement mutated old snapshot");
        std::cout << "PASS resource inventory: discovery, dedup, traversal, replacement, old snapshot\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
