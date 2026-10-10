#ifndef OPENMW_RENDER_VSG_GPUDEFORMATION_H
#define OPENMW_RENDER_VSG_GPUDEFORMATION_H

#include <components/rendercore/gpudeformationinput.hpp>
#include <vsg/core/Array.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/VertexIndexDraw.h>
#include <vsg/vk/Device.h>

namespace RenderVsg
{
    // Each fence-safe actor resident owns its output/palette generation.
    // Authored inputs stay immutable. Compute precedes ALL rendering views;
    // unchanged inputs owe neither an upload nor a dispatch. Debt is cleared
    // only after successful submission, never merely after CPU publication.
    class GpuDeformation
    {
    public:
        static std::shared_ptr<GpuDeformation> create(vsg::Device& device,
            const RenderCore::GpuDeformationInput& input, std::string& diagnostic);
        bool update(const RenderCore::GpuDeformationInput& input);
        vsg::ref_ptr<vsg::VertexIndexDraw> bind(const vsg::VertexIndexDraw& draw) const;
        bool dirty() const noexcept { return mDirty; }
        void submitted() noexcept { mDirty = false; }
        vsg::ref_ptr<vsg::Group> commands() const { return mCommands; }
        vsg::ref_ptr<vsg::BufferInfo> positions() const { return mPositions; }
        vsg::ref_ptr<vsg::BufferInfo> normals() const { return mNormals; }
        RenderCore::ModelNodeIndex node() const noexcept { return mInput.node; }

    private:
        RenderCore::GpuDeformationInput mInput;
        vsg::ref_ptr<vsg::vec4Array> mPose;
        vsg::ref_ptr<vsg::BufferInfo> mPositions, mNormals;
        vsg::ref_ptr<vsg::Group> mCommands;
        bool mDirty = true;
    };
}
#endif
