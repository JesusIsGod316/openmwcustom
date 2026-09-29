// Compile with the same pinned OSG/Windows headers before the full engine build.
#include <components/sceneutil/drawphasetrace.hpp>
static_assert(sizeof(SceneUtil::DrawPhaseTrace::Capture::Row)>0);
void phase9HeaderCheck(osgViewer::Viewer& viewer)
{
    SceneUtil::DrawPhaseTrace::installBeforeRealize(viewer);
}
