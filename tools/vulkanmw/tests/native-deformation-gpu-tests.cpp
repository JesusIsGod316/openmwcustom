#include <components/render/backend/vsg/gpudeformation.hpp>
#include <components/render/backend/vsg/vsgsubmission.hpp>
#include <vsg/all.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <stdexcept>
#include <cstring>

namespace
{
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    class CopyResults final : public vsg::Inherit<vsg::Command, CopyResults>
    {
    public:
        vsg::ref_ptr<vsg::BufferInfo> positions, normals;
        vsg::ref_ptr<vsg::Buffer> destination;
        void record(vsg::CommandBuffer& cb) const override
        {
            VkBufferCopy copy{positions->offset,0,positions->range};
            vkCmdCopyBuffer(cb,positions->buffer->vk(cb.deviceID),destination->vk(cb.deviceID),1,&copy);
            copy = {normals->offset,positions->range,normals->range};
            vkCmdCopyBuffer(cb,normals->buffer->vk(cb.deviceID),destination->vk(cb.deviceID),1,&copy);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT};
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        }
    };
    void run(vsg::ref_ptr<vsg::Device> device, bool morph, bool normals)
    {
        using namespace RenderCore;
        auto source = std::make_shared<MeshPayload>();
        source->indices = {0,1,2};
        for (unsigned v=0;v<257;++v)
        {
            source->positions.emplace_back(float(v)*.02f,float(v%3),-.25f);
            if (normals) source->normals.emplace_back(0.1f,0.3f,0.7f);
        }
        GpuDeformationInput input; input.node=ModelNodeIndex{3}; input.source=source;
        std::shared_ptr<SkinPayload> skin;
        if (morph)
        {
            auto targets=std::make_shared<MorphPayload>();
            for (unsigned t=0;t<3;++t)
            {
                MorphTarget target;
                target.positionOffsets.assign(257,glm::vec3(.1f*float(t+1),-.2f,0.5f));
                targets->targets.push_back(std::move(target));
            }
            input.morphs=targets; input.weights={.5f,-.25f,1.2f};
        }
        else
        {
            skin=std::make_shared<SkinPayload>();
            for (unsigned b=0;b<6;++b) { SkinBoneBinding bone; bone.name="bone"+std::to_string(b); skin->bones.push_back(bone); }
            skin->vertexInfluences.resize(257);
            // More than four weights, unnormalized and zero-influence vertices.
            for (unsigned v=1;v<257;++v)
                for (unsigned b=0;b<6;++b) skin->vertexInfluences[v].push_back({b,.12f+float(b)*.01f});
            input.skin=skin;
            input.palette.push_back(glm::scale(glm::translate(glm::mat4(1),{.3f,.4f,.5f}),{1.f,2.f,.5f}));
            for (unsigned b=0;b<6;++b)
                input.palette.push_back(glm::rotate(glm::translate(glm::mat4(1),{float(b)*.1f,.1f,0.f}),float(b)*.07f,glm::vec3(0,0,1)));
        }
        std::string reason;
        auto deformer=RenderVsg::GpuDeformation::create(*device,input,reason);
        require(bool(deformer),reason.c_str());
        const auto bytes=deformer->positions()->range;
        auto destination=vsg::createBufferAndMemory(device,bytes*2,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_SHARING_MODE_EXCLUSIVE,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        auto copy=CopyResults::create(); copy->positions=deformer->positions(); copy->normals=deformer->normals(); copy->destination=destination;
        auto holder=vsg::Group::create(); holder->addChild(deformer->commands()); holder->addChild(copy);
        const auto family=device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT);
        auto graph=vsg::CommandGraph::create(device,family); graph->addChild(holder);
        auto viewer=vsg::Viewer::create(); viewer->assignRecordAndSubmitTaskAndPresentation({graph});
        require(bool(viewer->compile()),"native deformation compilation");
        // Test production draw compilation with GPU-only ranges. It must not
        // dereference data==nullptr or allocate a replacement CPU vertex buffer.
        auto original=vsg::VertexIndexDraw::create();
        original->assignArrays({vsg::vec3Array::create(257),vsg::vec3Array::create(257)});
        original->assignIndices(vsg::uintArray::create(3)); original->indexCount=3; original->instanceCount=1;
        auto draw=deformer->bind(*original); require(bool(draw),"bind native output");
        auto context=vsg::Context::create(device); draw->compile(*context);
        require(draw->arrays[0]==deformer->positions() && draw->arrays[1]==deformer->normals(),"draw reallocated outputs");
        for (unsigned frame=0;frame<8;++frame)
        {
            if (frame==2 || frame==5)
            {
                if (morph) input.weights[0]+=.2f;
                else input.palette[1][3][0]+=.7f;
                require(deformer->update(input)&&deformer->dirty(),"changed native input did not dispatch");
                // Skipped publication cannot settle a not-yet-submitted pose.
                require(deformer->update(input)&&deformer->dirty(),"unsubmitted deformation debt lost");
            }
            else require(deformer->update(input),"update current native binding");
            if (frame!=0 && frame!=2 && frame!=5) require(!deformer->dirty(),"unchanged pose dispatch");
            holder->children.clear();
            if (deformer->dirty()) holder->addChild(deformer->commands());
            holder->addChild(copy);
            require(viewer->advanceToNextFrame(double(frame+1)),"advance GPU deformation fixture"); viewer->update();
            auto task=viewer->recordAndSubmitTasks.front(); graph->reset();
            require(RenderVsg::submitTaskChecked(*task,vsg::ref_ptr<vsg::FrameStamp>(viewer->getFrameStamp()))==VK_SUCCESS,"submit native deformation");
            deformer->submitted(); viewer->deviceWaitIdle();
            void* mapped=nullptr;
            require(destination->getDeviceMemory()->map(destination->getMemoryOffset(),bytes*2,0,&mapped)==VK_SUCCESS,"map native output");
            const auto* values=static_cast<const float*>(mapped);
            for (unsigned v=0;v<257;++v)
            {
                auto expected=source->positions[v]; auto normal=normals?source->normals[v]:glm::vec3(0,0,1);
                if (morph)
                    for (std::size_t t=0;t<input.weights.size();++t) expected+=input.morphs->targets[t].positionOffsets[v]*input.weights[t];
                else if(v!=0)
                {
                    glm::mat4 blend(0);
                    for (auto influence:skin->vertexInfluences[v]) blend+=input.palette[1+influence.boneIndex]*influence.weight;
                    for(int c=0;c<4;++c) blend[c][3]=c==3?1.f:0.f;
                    const auto transform=input.palette[0]*blend;
                    expected=glm::vec3(transform*glm::vec4(expected,1));
                    if(normals) normal=glm::mat3(transform)*normal;
                }
                for(unsigned c=0;c<3;++c)
                {
                    require(std::abs(values[3*v+c]-expected[c])<2e-5f,"GPU position differs from native CPU semantics");
                    require(std::abs(values[257*3+3*v+c]-normal[c])<2e-5f,"GPU normal differs from native CPU semantics");
                }
            }
            destination->getDeviceMemory()->unmap();
        }
        auto invalid=input; invalid.node=ModelNodeIndex{4}; require(!deformer->update(invalid),"stale binding accepted");
        if(morph) invalid.weights[0]=std::numeric_limits<float>::quiet_NaN();
        else invalid.palette[0][0][0]=std::numeric_limits<float>::quiet_NaN();
        invalid.node=input.node; require(!deformer->update(invalid)&&!deformer->dirty(),"invalid update changed live debt");
        std::cout<<"PASS actual GPU "<<(morph?"morph":"skin")<<" normals="<<normals<<" frames=8 tail=257 unchanged=5\n";
    }
}
int main()
{
    try
    {
        auto instance=vsg::Instance::create(vsg::Names{},vsg::Names{"VK_LAYER_KHRONOS_validation"},VK_API_VERSION_1_2);
        auto physical=instance->getPhysicalDevices(); require(!physical.empty(),"no Vulkan device");
        auto selected=physical.front(); const auto family=selected->getQueueFamily(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT);
        auto device=vsg::Device::create(selected,vsg::QueueSettings{{family,{1.f}}},vsg::Names{},vsg::Names{});
        for(bool morph:{false,true}) for(bool normals:{false,true}) run(device,morph,normals);
        return 0;
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
