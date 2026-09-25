#ifndef OPENMW_COMPONENTS_TERRAIN_TERRAINCOMPILE_H
#define OPENMW_COMPONENTS_TERRAIN_TERRAINCOMPILE_H

#include <osg/ref_ptr>

#include <osgUtil/IncrementalCompileOperation>

namespace osg
{
    class Object;
}

namespace Terrain
{
    class TerrainDrawable;

    void buildStagedTerrainCompileMap(TerrainDrawable& geometry,
        osgUtil::IncrementalCompileOperation::CompileSet& compileSet,
        osgUtil::IncrementalCompileOperation::ContextSet& contexts,
        osg::Object* markerObject);
}

#endif
