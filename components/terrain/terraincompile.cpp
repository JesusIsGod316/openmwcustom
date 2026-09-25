#include "terraincompile.hpp"

#include "terraindrawable.hpp"

#include <components/resource/p4compileops.hpp>

#include <osg/Array>
#include <osg/BufferObject>
#include <osg/PrimitiveSet>
#include <osg/StateSet>

#include <set>

namespace Terrain
{
    void buildStagedTerrainCompileMap(TerrainDrawable& geometry,
        osgUtil::IncrementalCompileOperation::CompileSet& compileSet,
        osgUtil::IncrementalCompileOperation::ContextSet& contexts,
        osg::Object* markerObject)
    {
        if (contexts.empty())
            return;

        osgUtil::StateToCompile stateToCompile(
            osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES, markerObject);

        if (geometry.getStateSet())
            stateToCompile.apply(*geometry.getStateSet());

        for (const osg::ref_ptr<osg::StateSet>& pass : geometry.getPasses())
            if (pass)
                stateToCompile.apply(*pass);

        std::set<osg::BufferObject*> buffers;
        osg::Geometry::ArrayList arrays;
        if (geometry.getArrayList(arrays))
        {
            for (const osg::ref_ptr<osg::Array>& array : arrays)
                if (array && array->getBufferObject())
                    buffers.insert(array->getBufferObject());
        }

        osg::Geometry::DrawElementsList drawElements;
        if (geometry.getDrawElementsList(drawElements))
        {
            for (osg::DrawElements* elements : drawElements)
                if (elements && elements->getBufferObject())
                    buffers.insert(elements->getBufferObject());
        }

        for (osg::GraphicsContext* context : contexts)
        {
            if (!context)
                continue;

            ++compileSet._numberCompileListsToCompile;
            auto& list = compileSet._compileMap[context];

            for (osg::Texture* texture : stateToCompile._textures)
                list.add(texture);
            for (osg::Program* program : stateToCompile._programs)
                list.add(program);
            for (osg::BufferObject* buffer : buffers)
                list.add(new Resource::P4CompileBufferOp(buffer));

            list.add(new Resource::P4CompileGeometryFinalizeOp(&geometry));
        }
    }
}
