#ifndef OPENMW_COMPONENTS_SCENEUTIL_TEXTURETYPE_H
#define OPENMW_COMPONENTS_SCENEUTIL_TEXTURETYPE_H

#include <osg/StateAttribute>
#include <osg/StateSet>
#include <osg/Texture>

namespace SceneUtil
{
    // The type bound to the given texture used by the ShaderVisitor to distinguish between them
    class TextureType : public osg::StateAttribute
    {
    public:
        TextureType() = default;

        TextureType(const std::string& name) { setName(name); }

        TextureType(const TextureType& copy, const osg::CopyOp& copyop = osg::CopyOp::SHALLOW_COPY)
            : StateAttribute(copy, copyop)
        {
        }

        static const osg::StateAttribute::Type AttributeType = static_cast<osg::StateAttribute::Type>(69);
        META_StateAttribute(SceneUtil, TextureType, AttributeType)

        bool isTextureAttribute() const override { return true; }

        int compare(const osg::StateAttribute& sa) const override
        {
            COMPARE_StateAttribute_Types(TextureType, sa);
            COMPARE_StateAttribute_Parameter(_name);
            return 0;
        }
    };

    // Shared by native ShaderVisitor and neutral material capture. Keep this
    // small semantic helper independent of scene loading and GlowUpdater.
    inline const std::string& getTextureType(const osg::StateSet& stateset, const osg::Texture& texture,
        unsigned int texUnit)
    {
        const osg::StateAttribute* type = stateset.getTextureAttribute(texUnit, TextureType::AttributeType);
        if (type)
            return static_cast<const TextureType*>(type)->getName();
        return texture.getName();
    }
}
#endif
