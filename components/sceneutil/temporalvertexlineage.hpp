#ifndef OPENMW_SCENEUTIL_TEMPORALVERTEXLINEAGE_H
#define OPENMW_SCENEUTIL_TEMPORALVERTEXLINEAGE_H

#include <osg/Geometry>
#include <osg/UserDataContainer>
#include <atomic>

namespace SceneUtil
{
    // A private original/tier pair shares immutable vertex order. The identity
    // certifies this lineage only; different indices are still the current
    // representation's coverage and never imply historical pixel validity.
    class TemporalVertexLineage final : public osg::Object
    {
    public:
        TemporalVertexLineage() : id(++sNext) {}
        explicit TemporalVertexLineage(osg::Array* vertices)
            : id(++sNext), mVertices(vertices), mElements(vertices ? vertices->getNumElements() : 0),
              mModified(vertices ? vertices->getModifiedCount() : 0) {}
        TemporalVertexLineage(const TemporalVertexLineage& value, const osg::CopyOp& op)
            : osg::Object(value,op), id(++sNext), mVertices(value.mVertices),
              mElements(value.mElements), mModified(value.mModified) {}
        META_Object(SceneUtil,TemporalVertexLineage)
        bool matches(const osg::Geometry& geometry) const
        {
            return dynamic_cast<const osg::Vec3Array*>(mVertices.get()) && geometry.getVertexArray()==mVertices
                && mVertices->getNumElements()==mElements && mVertices->getModifiedCount()==mModified;
        }
        const std::uint64_t id;
    private:
        osg::ref_ptr<const osg::Array> mVertices;
        unsigned mElements=0, mModified=0;
        inline static std::atomic_uint64_t sNext{0};
    };
    inline void installTemporalVertexLineage(osg::Geometry& geometry, TemporalVertexLineage* lineage)
    {
        if(const auto* original=geometry.getUserDataContainer())
            geometry.setUserDataContainer(static_cast<osg::UserDataContainer*>(original->clone(osg::CopyOp::SHALLOW_COPY)));
        auto* data=geometry.getOrCreateUserDataContainer();
        for(unsigned i=data->getNumUserObjects();i>0;--i)
            if(dynamic_cast<TemporalVertexLineage*>(data->getUserObject(i-1))) data->removeUserObject(i-1);
        data->addUserObject(lineage);
    }
    inline const TemporalVertexLineage* temporalVertexLineage(const osg::Geometry& geometry)
    {
        const auto* data=geometry.getUserDataContainer();
        if(data) for(unsigned i=0;i<data->getNumUserObjects();++i)
            if(const auto* lineage=dynamic_cast<const TemporalVertexLineage*>(data->getUserObject(i));
                lineage && lineage->matches(geometry)) return lineage;
        return nullptr;
    }
}
#endif
