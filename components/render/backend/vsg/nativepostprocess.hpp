#ifndef OPENMW_COMPONENTS_RENDER_VSG_NATIVEPOSTPROCESS_H
#define OPENMW_COMPONENTS_RENDER_VSG_NATIVEPOSTPROCESS_H
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Node.h>
#include <vsg/state/ImageView.h>
namespace RenderVsg
{
    enum class NativePostProcessMode { Copy, EdgeAA, Depth };
    // Linear HDR scene color + reversed D32 depth -> output color. The GUI is
    // composed afterwards. This is not an .omwfx translator or Rafael preset.
    vsg::ref_ptr<vsg::Node> createNativePostProcess(vsg::ref_ptr<vsg::ImageView> color,
        vsg::ref_ptr<vsg::ImageView> depth, NativePostProcessMode mode);
}
#endif
