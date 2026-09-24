#ifndef OPENMW_MWRENDER_V4MATERIALVALUESTAMP_H
#define OPENMW_MWRENDER_V4MATERIALVALUESTAMP_H

#include <components/sceneutil/material.hpp>
#include <components/sceneutil/texturetype.hpp>
#include <osg/AlphaFunc>
#include <osg/BlendEquation>
#include <osg/BlendFunc>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/FrontFace>
#include <osg/PolygonMode>
#include <osg/Stencil>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <array>
#include <typeinfo>
#include <vector>

namespace MWRender::v4_effect_detail
{
    // Exact value guard for the inputs consumed by captureMaterial. No dirty
    // counts: controllers and mods may mutate uniform arrays or attributes
    // directly. Unknown subclasses take the uncached compatibility path.
    class MaterialValueStamp
    {
    public:
        bool capture(const osg::StateSet& state)
        {
            if (state.getTextureAttributeList().size() > 32) return false;
            mStructure = new osg::StateSet(state, osg::CopyOp::SHALLOW_COPY);
            if (!attribute<SceneUtil::Material>(state, osg::StateAttribute::MATERIAL)
                || !attribute<osg::AlphaFunc>(state, osg::StateAttribute::ALPHAFUNC)
                || !attribute<osg::BlendFunc>(state, osg::StateAttribute::BLENDFUNC)
                || !attribute<osg::BlendEquation>(state, osg::StateAttribute::BLENDEQUATION)
                || !attribute<osg::CullFace>(state, osg::StateAttribute::CULLFACE)
                || !attribute<osg::FrontFace>(state, osg::StateAttribute::FRONTFACE)
                || !attribute<osg::Depth>(state, osg::StateAttribute::DEPTH)
                || !attribute<osg::Stencil>(state, osg::StateAttribute::STENCIL)
                || !attribute<osg::PolygonMode>(state, osg::StateAttribute::POLYGONMODE)) return false;
            for (std::size_t i = 0; i < UniformNames.size(); ++i)
                if (const auto* uniform = state.getUniform(UniformNames[i]))
                {
                    if (typeid(*uniform) != typeid(osg::Uniform) || uniform->getNumElements() > 16) return false;
                    mUniforms[i] = new osg::Uniform(*uniform, osg::CopyOp::DEEP_COPY_ALL);
                }
            for (unsigned i = 0; i < state.getTextureAttributeList().size(); ++i)
            {
                const auto* attr = state.getTextureAttribute(i, osg::StateAttribute::TEXTURE);
                if (!attr) continue;
                if (typeid(*attr) != typeid(osg::Texture2D)) return false;
                const auto* role = state.getTextureAttribute(i, SceneUtil::TextureType::AttributeType);
                if (role && typeid(*role) != typeid(SceneUtil::TextureType)) return false;
                const auto& texture = static_cast<const osg::Texture2D&>(*attr);
                // Preview sentinels and other generated textures stay on the
                // full path; they carry signatures beyond normal VFS images.
                const auto* image = texture.getImage();
                if (!image || image->getFileName().empty()) return false;
                mTextures.push_back(TextureStamp{ i, texture.getName(),
                    SceneUtil::getTextureType(state, texture, i), image->getFileName(),
                    image->s(), image->t(), image->getNumMipmapLevels(),
                    texture.getFilter(osg::Texture::MIN_FILTER), texture.getFilter(osg::Texture::MAG_FILTER),
                    texture.getWrap(osg::Texture::WRAP_S), texture.getWrap(osg::Texture::WRAP_T),
                    texture.getMaxAnisotropy() });
            }
            return true;
        }

        bool current(const osg::StateSet& state) const
        {
            const auto& old = *mStructure;
            if (state.getModeList() != old.getModeList() || state.getAttributeList() != old.getAttributeList()
                || state.getTextureAttributeList() != old.getTextureAttributeList()
                || state.getDefineList() != old.getDefineList() || state.getBinName() != old.getBinName()
                || state.getUniformList() != old.getUniformList()) return false;
            for (const auto& pair : mAttributes)
                if (pair.first->compare(*pair.second) != 0) return false;
            for (std::size_t i = 0; i < UniformNames.size(); ++i)
            {
                const auto* now = state.getUniform(UniformNames[i]);
                const auto* prior = mUniforms[i].get();
                if (bool(now) != bool(prior)) return false;
                if (now && (now->getType() != prior->getType() || now->getNumElements() != prior->getNumElements()
                    || now->compareData(*prior) != 0)) return false;
            }
            for (const auto& stamp : mTextures)
            {
                const auto& texture = *static_cast<const osg::Texture2D*>(
                    state.getTextureAttribute(stamp.unit, osg::StateAttribute::TEXTURE));
                const auto* image = texture.getImage();
                if (!image || image->getFileName() != stamp.file || image->s() != stamp.width
                    || image->t() != stamp.height || image->getNumMipmapLevels() != stamp.mips
                    || texture.getName() != stamp.name
                    || SceneUtil::getTextureType(state, texture, stamp.unit) != stamp.role
                    || texture.getFilter(osg::Texture::MIN_FILTER) != stamp.minFilter
                    || texture.getFilter(osg::Texture::MAG_FILTER) != stamp.magFilter
                    || texture.getWrap(osg::Texture::WRAP_S) != stamp.wrapS
                    || texture.getWrap(osg::Texture::WRAP_T) != stamp.wrapT
                    || texture.getMaxAnisotropy() != stamp.anisotropy) return false;
            }
            return true;
        }

    private:
        template <class T> bool attribute(const osg::StateSet& state, osg::StateAttribute::Type type)
        {
            const auto* source = state.getAttribute(type);
            if (!source) return true;
            if (typeid(*source) != typeid(T)) return false;
            mAttributes.emplace_back(source, new T(static_cast<const T&>(*source), osg::CopyOp::SHALLOW_COPY));
            return true;
        }
        inline static constexpr std::array UniformNames{
            "sun.ambient", "alpha", "fog.depth", "fog.color", "envMapColor", "bumpMapMatrix", "envMapLumaBias" };
        struct TextureStamp
        {
            unsigned unit;
            std::string name, role, file;
            int width, height;
            unsigned mips;
            osg::Texture::FilterMode minFilter, magFilter;
            osg::Texture::WrapMode wrapS, wrapT;
            float anisotropy;
        };
        osg::ref_ptr<osg::StateSet> mStructure;
        std::vector<std::pair<const osg::StateAttribute*, osg::ref_ptr<osg::StateAttribute>>> mAttributes;
        std::array<osg::ref_ptr<osg::Uniform>, UniformNames.size()> mUniforms;
        std::vector<TextureStamp> mTextures;
    };
}
#endif
