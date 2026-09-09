#ifndef OPENMW_COMPONENTS_VSGRENDER_VSGUIPIPELINE_H
#define OPENMW_COMPONENTS_VSGRENDER_VSGUIPIPELINE_H

#include <vsg/all.h>

#include <cstdint>

namespace RenderVsg
{
    // 2D screen-space overlay pipeline for the GUI — the rendering substrate for the MyGUI-on-Vulkan port
    // (MW8 P1). MyGUI emits triangles ALREADY in clip space, in a single interleaved vertex stream matching its
    // MyGUI::Vertex: position (3×float) @0, colour (RGBA8 UNORM, packed ColourABGR) @12, uv (2×float) @16 —
    // stride 24. So one vertex binding, three attributes. The vertex shader passes position through with a
    // Vulkan Y-flip (MyGUI targets a GL Y-up NDC); the fragment shader is texture × vertexColour. Alpha-blended,
    // depth test/write OFF, cull NONE — the overlay draws over the 3D scene in submission (painter's) order,
    // exactly MyGUI's layering model. Set 0 binding 0 = combined image sampler (the widget texture / font atlas).
    //
    // The layout also declares a 128-byte vertex push-constant range (projection@0 + modelView@64) that the
    // shader ignores: it exists only so the layout stays push-constant-compatible with the mesh pipeline when the
    // overlay is recorded inside the 3D vsg::View (whose camera pushes those matrices).
    struct UiPipeline
    {
        vsg::ref_ptr<vsg::BindGraphicsPipeline> bindPipeline;
        vsg::ref_ptr<vsg::PipelineLayout> pipelineLayout;
        vsg::ref_ptr<vsg::DescriptorSetLayout> descriptorSetLayout; // set 0: binding 0 combined image sampler
        vsg::ref_ptr<vsg::Sampler> sampler; // linear, clamp-to-edge, no mips
        vsg::ref_ptr<vsg::Data> whiteTexture; // 1×1 white — solid quads show their vertex colour

        explicit operator bool() const { return bindPipeline.valid(); }
    };

    // viewportWidth/Height must match the swapchain pixel extent. Returns an empty UiPipeline if shader
    // compilation fails. VSG compiles the embedded GLSL stages with the same runtime path as the retained
    // compatibility shaders.
    UiPipeline createUiPipeline(uint32_t viewportWidth, uint32_t viewportHeight);

    // Verification node (MW8 P1.0): a few translucent, vertex-coloured quads laid out in clip space in MyGUI's
    // interleaved format, drawn through the UI pipeline as an overlay. Proves the pipeline (position/Y-orientation,
    // vertex colour, blending, sampler) works on MoltenVK before the real MyGUI RenderManager is built on top.
    vsg::ref_ptr<vsg::Node> buildUiTestOverlay(const UiPipeline& ui);
}
#endif
