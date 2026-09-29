// Compile with the same pinned OSG/Windows headers before the full engine build.
#include <components/sceneutil/drawphasetrace.hpp>
#include <components/sceneutil/staticgeometryprewarm.hpp>
static_assert(sizeof(SceneUtil::DrawPhaseTrace::Capture::Row)>0);
static_assert(sizeof(SceneUtil::StaticGeometryPrewarm::Result)>0);
void phase9HeaderCheck(osgViewer::Viewer& viewer)
{
    SceneUtil::DrawPhaseTrace::installBeforeRealize(viewer);
}
