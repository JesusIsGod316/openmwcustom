#ifndef OPENMW_COMPONENTS_RESOURCE_P4COMPILEOPS_H
#define OPENMW_COMPONENTS_RESOURCE_P4COMPILEOPS_H

#include <osg/BufferObject>
#include <osg/Geometry>
#include <osg/ref_ptr>

#include <osgUtil/IncrementalCompileOperation>

namespace Resource
{
    class P4CompileBufferOp final : public osgUtil::IncrementalCompileOperation::CompileOp
    {
    public:
        explicit P4CompileBufferOp(osg::BufferObject* buffer)
            : mBuffer(buffer)
        {
        }

        double estimatedTimeForCompile(
            osgUtil::IncrementalCompileOperation::CompileInfo&) const override
        {
            return 0.0;
        }

        bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
        {
            if (!mBuffer || !info.getState())
                return true;

            osg::GLBufferObject* glBuffer = mBuffer->getOrCreateGLBufferObject(info.getState()->getContextID());
            if (glBuffer && glBuffer->isDirty())
                glBuffer->compileBuffer();
            return true;
        }

    private:
        osg::ref_ptr<osg::BufferObject> mBuffer;
    };

    class P4CompileGeometryFinalizeOp final : public osgUtil::IncrementalCompileOperation::CompileOp
    {
    public:
        explicit P4CompileGeometryFinalizeOp(osg::Geometry* geometry)
            : mGeometry(geometry)
        {
        }

        double estimatedTimeForCompile(
            osgUtil::IncrementalCompileOperation::CompileInfo&) const override
        {
            return 0.00005;
        }

        bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
        {
            if (mGeometry)
            {
                // Deliberately bypass any subclass override that recompiles
                // StateSets. Buffer objects have already been staged.
                mGeometry->osg::Geometry::compileGLObjects(info);
            }
            return true;
        }

    private:
        osg::ref_ptr<osg::Geometry> mGeometry;
    };
}

#endif
