/* -*-c++-*- OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield
 *
 * This library is open source and may be redistributed and/or modified under
 * the terms of the OpenSceneGraph Public License (OSGPL) version 0.0 or
 * (at your option) any later version.  The full license is in LICENSE file
 * included with this distribution, and on the openscenegraph.org website.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * OpenSceneGraph Public License for more details.
*/

// Adapted from OpenSceneGraph 3.6.5 src/osgViewer/Renderer.cpp.
// Included in the diagnostic Renderer class; installed only on exact OSG 3.6.5.
// Preserve stock queues, update/traversal/statistics/publication ordering.
void cull() override
{
    if (!Debug::HitchTrace::requested()) { osgViewer::Renderer::cull(); return; }
    Debug::HitchTrace::Scope envelope("renderer.cull", _camera.valid() ? _camera->getName() : std::string_view{});
    if (_done || _graphicsThreadDoesCull) return;
    osgUtil::SceneView* sceneView;
    { Debug::HitchTrace::Scope acquisition("renderer.sceneview_acquire", Debug::HitchTrace::CpuTiming::Scoped);
      sceneView = _availableQueue.takeFront(); }
    if (sceneView)
    {
        { Debug::HitchTrace::Scope update("renderer.sceneview_update", Debug::HitchTrace::CpuTiming::Scoped);
          updateSceneView(sceneView); }
        osg::Stats* stats = sceneView->getCamera()->getStats();
        const osg::FrameStamp* fs = sceneView->getFrameStamp();
        unsigned int frameNumber = fs ? fs->getFrameNumber() : 0;
        osg::Timer_t beforeCullTick = osg::Timer::instance()->tick();
        { Debug::HitchTrace::Scope traversal("renderer.sceneview_traversal", {}, {}, frameNumber,
              Debug::HitchTrace::CpuTiming::Scoped);
          sceneView->inheritCullSettings(*(sceneView->getCamera()));
          sceneView->cull(); }
        osg::Timer_t afterCullTick = osg::Timer::instance()->tick();
        {
            Debug::HitchTrace::Scope statistics("renderer.sceneview_stats");
            if (stats && stats->collectStats("rendering"))
            {
                stats->setAttribute(frameNumber, "Cull traversal begin time", osg::Timer::instance()->delta_s(_startTick, beforeCullTick));
                stats->setAttribute(frameNumber, "Cull traversal end time", osg::Timer::instance()->delta_s(_startTick, afterCullTick));
                stats->setAttribute(frameNumber, "Cull traversal time taken", osg::Timer::instance()->delta_s(beforeCullTick, afterCullTick));
            }
            if (stats && stats->collectStats("scene")) collectSceneViewStats(frameNumber, sceneView, stats);
        }
        { Debug::HitchTrace::Scope publish("renderer.sceneview_publish"); _drawQueue.add(sceneView); }
    }
}
static void collectSceneViewStats(unsigned int frameNumber, osgUtil::SceneView* sceneView, osg::Stats* stats)
{
    osgUtil::Statistics sceneStats;
    sceneView->getStats(sceneStats);

    stats->setAttribute(frameNumber, "Visible vertex count", static_cast<double>(sceneStats._vertexCount));
    stats->setAttribute(frameNumber, "Visible number of drawables", static_cast<double>(sceneStats.numDrawables));
    stats->setAttribute(frameNumber, "Visible number of fast drawables", static_cast<double>(sceneStats.numFastDrawables));
    stats->setAttribute(frameNumber, "Visible number of lights", static_cast<double>(sceneStats.nlights));
    stats->setAttribute(frameNumber, "Visible number of render bins", static_cast<double>(sceneStats.nbins));
    stats->setAttribute(frameNumber, "Visible depth", static_cast<double>(sceneStats.depth));
    stats->setAttribute(frameNumber, "Number of StateGraphs", static_cast<double>(sceneStats.numStateGraphs));
    stats->setAttribute(frameNumber, "Visible number of impostors", static_cast<double>(sceneStats.nimpostor));
    stats->setAttribute(frameNumber, "Number of ordered leaves", static_cast<double>(sceneStats.numOrderedLeaves));

    unsigned int totalNumPrimitiveSets = 0;
    const osgUtil::Statistics::PrimitiveValueMap& pvm = sceneStats.getPrimitiveValueMap();
    for(osgUtil::Statistics::PrimitiveValueMap::const_iterator pvm_itr = pvm.begin();
        pvm_itr != pvm.end();
        ++pvm_itr)
    {
        totalNumPrimitiveSets += pvm_itr->second.first;
    }
    stats->setAttribute(frameNumber, "Visible number of PrimitiveSets", static_cast<double>(totalNumPrimitiveSets));

    osgUtil::Statistics::PrimitiveCountMap& pcm = sceneStats.getPrimitiveCountMap();
    stats->setAttribute(frameNumber, "Visible number of GL_POINTS", static_cast<double>(pcm[GL_POINTS]));
    stats->setAttribute(frameNumber, "Visible number of GL_LINES", static_cast<double>(pcm[GL_LINES]));
    stats->setAttribute(frameNumber, "Visible number of GL_LINE_STRIP", static_cast<double>(pcm[GL_LINE_STRIP]));
    stats->setAttribute(frameNumber, "Visible number of GL_LINE_LOOP", static_cast<double>(pcm[GL_LINE_LOOP]));
    stats->setAttribute(frameNumber, "Visible number of GL_TRIANGLES", static_cast<double>(pcm[GL_TRIANGLES]));
    stats->setAttribute(frameNumber, "Visible number of GL_TRIANGLE_STRIP", static_cast<double>(pcm[GL_TRIANGLE_STRIP]));
    stats->setAttribute(frameNumber, "Visible number of GL_TRIANGLE_FAN", static_cast<double>(pcm[GL_TRIANGLE_FAN]));
    stats->setAttribute(frameNumber, "Visible number of GL_QUADS", static_cast<double>(pcm[GL_QUADS]));
    stats->setAttribute(frameNumber, "Visible number of GL_QUAD_STRIP", static_cast<double>(pcm[GL_QUAD_STRIP]));
    stats->setAttribute(frameNumber, "Visible number of GL_POLYGON", static_cast<double>(pcm[GL_POLYGON]));
}

