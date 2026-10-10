#include "gpudeformation.hpp"

#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/Dispatch.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/ComputePipeline.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/Buffer.h>
#include <vsg/vk/Context.h>
#include <vsg/vk/PhysicalDevice.h>

#include <bit>
#include <limits>

namespace RenderVsg
{
    namespace
    {
        // VertexIndexDraw's stock combined-data compiler requires every range
        // to own CPU data. GPU outputs have no CPU mirror and must never be
        // included in that staging allocation. Otherwise normal compilation
        // would overwrite/reallocate the retained compute output.
        class DeformedDraw final : public vsg::Inherit<vsg::VertexIndexDraw, DeformedDraw>
        {
        public:
            explicit DeformedDraw(const vsg::VertexIndexDraw& source) : Inherit(source) {}
            void compile(vsg::Context& context) override
            {
                vsg::BufferInfoList cpu;
                bool copy = indices->requiresCopy(context.deviceID);
                for (const auto& array : arrays)
                    if (array->data) { cpu.push_back(array); copy |= array->requiresCopy(context.deviceID); }
                cpu.push_back(indices);
                if (copy) vsg::createBufferAndTransferData(context, cpu,
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_SHARING_MODE_EXCLUSIVE);
                vsg::assignVulkanArrayData(context.deviceID, arrays, _vulkanData[context.deviceID]);
                indexType = vsg::computeIndexType(indices->data);
            }
        };

        // Adapted mechanism, not a second donor scene: Clockwork-style arbitrary
        // influence ranges and native output streams, RTX-style retained pose
        // equality/debt. Math is the existing OpenMW ActorProgram contract.
        constexpr const char* Shader = R"glsl(#version 450
layout(local_size_x=64) in;
layout(std430,binding=0) readonly buffer Source { vec4 source[]; };
layout(std430,binding=1) readonly buffer Ranges { uvec2 ranges[]; };
layout(std430,binding=2) readonly buffer Shares { uvec2 shares[]; };
layout(std430,binding=3) readonly buffer Offsets { vec4 offsets[]; };
layout(std430,binding=4) readonly buffer Pose { vec4 pose[]; };
layout(std430,binding=5) writeonly buffer Positions { float positions[]; };
layout(std430,binding=6) writeonly buffer Normals { float normals[]; };
layout(std430,binding=7) readonly buffer Spec { uvec4 spec; };
mat4 matrix(uint i) { return mat4(pose[i],pose[i+1],pose[i+2],pose[i+3]); }
void main() {
    uint v=gl_GlobalInvocationID.x;
    if(v>=spec.x) return;
    vec3 p=source[2*v].xyz, n=source[2*v+1].xyz;
    if(spec.y==2) {
        for(uint t=0;t<spec.z;++t) p+=offsets[t*spec.x+v].xyz*pose[t].x;
    } else if(ranges[v].y!=0) {
        mat4 blended=mat4(0);
        for(uint j=0;j<ranges[v].y;++j) {
            uvec2 share=shares[ranges[v].x+j];
            blended+=matrix(4+4*share.x)*uintBitsToFloat(share.y);
        }
        // RigGeometry restores affine w; unnormalized authored weights are
        // intentional. A vertex with no influences retains its source exactly.
        blended[0].w=0; blended[1].w=0; blended[2].w=0; blended[3].w=1;
        mat4 transform=matrix(0)*blended;
        p=(transform*vec4(p,1)).xyz;
        if(spec.w!=0) n=mat3(transform)*n;
    }
    for(uint c=0;c<3;++c) { positions[3*v+c]=p[c]; normals[3*v+c]=n[c]; }
}
)glsl";

        std::vector<vsg::vec4> poseWords(const RenderCore::GpuDeformationInput& input)
        {
            std::vector<vsg::vec4> result;
            if (input.morphs)
                for (const float weight : input.weights) result.emplace_back(weight, 0.f, 0.f, 0.f);
            else
                for (const auto& matrix : input.palette)
                    for (int column = 0; column < 4; ++column)
                        result.emplace_back(matrix[column][0], matrix[column][1], matrix[column][2], matrix[column][3]);
            return result;
        }
    }

    std::shared_ptr<GpuDeformation> GpuDeformation::create(vsg::Device& device,
        const RenderCore::GpuDeformationInput& input, std::string& diagnostic)
    {
        diagnostic.clear();
        const auto reject = [&](const char* reason) -> std::shared_ptr<GpuDeformation> {
            diagnostic = reason; return {};
        };
        if (!input.source || input.source->positions.empty() || (!input.skin && !input.morphs))
            return reject("GPU deformation requires bound native source streams");
        const auto& limits = device.getPhysicalDevice()->getProperties().limits;
        const auto vertices = input.source->positions.size();
        if (limits.maxPerStageDescriptorStorageBuffers < 8 || limits.maxDescriptorSetStorageBuffers < 8
            || vertices > std::numeric_limits<std::uint32_t>::max()
            || (vertices + 63) / 64 > limits.maxComputeWorkGroupCount[0])
            return reject("GPU deformation device limits require CPU fallback");
        if (!RenderCore::validMeshPayload(*input.source)) return reject("GPU deformation invalid source");
        std::uint64_t sharesCount = 0;
        if (input.morphs)
        {
            if (!RenderCore::validMorphPayload(*input.morphs, vertices)
                || input.weights.size() != input.morphs->targets.size()) return reject("GPU morph binding mismatch");
        }
        else
        {
            if (!RenderCore::validSkinPayload(*input.skin, vertices)
                || input.palette.size() != input.skin->bones.size() + 1) return reject("GPU skin binding mismatch");
            for (const auto& influences : input.skin->vertexInfluences) sharesCount += influences.size();
        }
        const auto words = poseWords(input);
        const std::uint64_t offsetsCount = input.morphs ? std::uint64_t(input.morphs->targets.size()) * vertices : 1;
        // Large/unusual assets retain CPU behavior rather than overflowing
        // indices, device ranges, or creating unbounded donor scratch buffers.
        const std::uint64_t largest = std::max({vertices * 32ull, vertices * 8ull, sharesCount * 8ull,
            offsetsCount * 16ull, std::uint64_t(words.size()) * 16ull});
        if (words.empty() || sharesCount > std::numeric_limits<std::uint32_t>::max()
            || largest > std::min<std::uint64_t>(limits.maxStorageBufferRange, 64u * 1024u * 1024u))
            return reject("GPU deformation buffer budget requires CPU fallback");
        for (const auto& word : words)
            for (int c = 0; c < 4; ++c) if (!std::isfinite(word[c])) return reject("GPU deformation non-finite pose");

        auto result = std::make_shared<GpuDeformation>();
        result->mInput = input;
        auto source = vsg::vec4Array::create(vertices * 2);
        for (std::size_t v = 0; v < vertices; ++v)
        {
            const auto p = input.source->positions[v];
            const auto n = input.source->normals.empty() ? glm::vec3(0,0,1) : input.source->normals[v];
            (*source)[2*v] = vsg::vec4(p.x,p.y,p.z,1); (*source)[2*v+1] = vsg::vec4(n.x,n.y,n.z,0);
        }
        auto ranges = vsg::uivec2Array::create(vertices);
        auto shares = vsg::uivec2Array::create(std::max<std::uint64_t>(1, sharesCount));
        std::uint32_t first = 0;
        for (std::size_t v = 0; v < vertices; ++v)
        {
            const auto count = input.morphs ? 0u : static_cast<std::uint32_t>(input.skin->vertexInfluences[v].size());
            (*ranges)[v] = {first,count};
            if (!input.morphs)
                for (const auto& influence : input.skin->vertexInfluences[v])
                    (*shares)[first++] = {influence.boneIndex, std::bit_cast<std::uint32_t>(influence.weight)};
        }
        auto offsets = vsg::vec4Array::create(offsetsCount);
        if (input.morphs)
            for (std::size_t t = 0; t < input.morphs->targets.size(); ++t)
                for (std::size_t v = 0; v < vertices; ++v)
                {
                    const auto p = input.morphs->targets[t].positionOffsets[v];
                    (*offsets)[t*vertices+v] = vsg::vec4(p.x,p.y,p.z,0);
                }
        result->mPose = vsg::vec4Array::create(words.size());
        std::copy(words.begin(), words.end(), result->mPose->begin());
        result->mPose->properties.dataVariance = vsg::DYNAMIC_DATA_TRANSFER_AFTER_RECORD;
        const VkDeviceSize bytes = vertices * 3 * sizeof(float);
        const auto allocate = [&] {
            auto buffer = vsg::createBufferAndMemory(&device, bytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_SHARING_MODE_EXCLUSIVE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            return buffer ? vsg::BufferInfo::create(buffer, 0, bytes) : vsg::ref_ptr<vsg::BufferInfo>{};
        };
        result->mPositions = allocate(); result->mNormals = allocate();
        if (!result->mPositions || !result->mNormals) return reject("GPU deformation allocation requires CPU fallback");
        auto spec = vsg::uivec4Value::create(vsg::uivec4(static_cast<std::uint32_t>(vertices), input.morphs ? 2u : 1u,
            input.morphs ? static_cast<std::uint32_t>(input.weights.size()) : 0u, input.source->normals.empty() ? 0u : 1u));
        vsg::DescriptorSetLayoutBindings bindings;
        for (std::uint32_t b = 0; b < 8; ++b)
            bindings.push_back({b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        auto layout = vsg::DescriptorSetLayout::create(bindings);
        auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{layout}, vsg::PushConstantRanges{});
        auto pipeline = vsg::ComputePipeline::create(pipelineLayout,
            vsg::ShaderStage::create(VK_SHADER_STAGE_COMPUTE_BIT, "main", Shader));
        auto set = vsg::DescriptorSet::create(layout, vsg::Descriptors{
            vsg::DescriptorBuffer::create(source,0,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(ranges,1,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(shares,2,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(offsets,3,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(result->mPose,4,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(vsg::BufferInfoList{result->mPositions},5,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(vsg::BufferInfoList{result->mNormals},6,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(spec,7,0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)});
        auto dispatch = vsg::StateGroup::create();
        dispatch->add(vsg::BindComputePipeline::create(pipeline));
        dispatch->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,set));
        dispatch->addChild(vsg::Dispatch::create(static_cast<std::uint32_t>((vertices+63)/64),1,1));
        result->mCommands = vsg::Group::create();
        const auto barrier = [&](VkAccessFlags from, VkAccessFlags to, VkPipelineStageFlags src, VkPipelineStageFlags dst) {
            return vsg::PipelineBarrier::create(src,dst,0,
                vsg::BufferMemoryBarrier::create(from,to,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
                    result->mPositions->buffer,0,bytes),
                vsg::BufferMemoryBarrier::create(from,to,VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,
                    result->mNormals->buffer,0,bytes));
        };
        result->mCommands->addChild(barrier(VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT));
        result->mCommands->addChild(dispatch);
        result->mCommands->addChild(barrier(VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT));
        return result;
    }

    bool GpuDeformation::update(const RenderCore::GpuDeformationInput& input)
    {
        if (input.node != mInput.node || input.source != mInput.source || input.skin != mInput.skin || input.morphs != mInput.morphs)
            return false;
        const auto words = poseWords(input);
        if (words.size() != mPose->size()) return false;
        for (const auto& word : words) for (int c=0;c<4;++c) if (!std::isfinite(word[c])) return false;
        if (!std::equal(words.begin(),words.end(),mPose->begin()))
        {
            std::copy(words.begin(),words.end(),mPose->begin());
            mPose->dirty(); mDirty = true;
        }
        return true;
    }

    vsg::ref_ptr<vsg::VertexIndexDraw> GpuDeformation::bind(const vsg::VertexIndexDraw& draw) const
    {
        if (draw.arrays.size()<2 || !draw.indices || !draw.arrays[0]->data || !draw.arrays[1]->data
            || draw.arrays[0]->data->valueCount()!=mInput.source->positions.size()
            || draw.arrays[1]->data->valueCount()!=mInput.source->positions.size()) return {};
        auto result = DeformedDraw::create(draw);
        result->arrays[0]=mPositions; result->arrays[1]=mNormals;
        return result;
    }
}
