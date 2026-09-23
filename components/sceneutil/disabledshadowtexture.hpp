#ifndef OPENMW_COMPONENTS_SCENEUTIL_DISABLEDSHADOWTEXTURE_H
#define OPENMW_COMPONENTS_SCENEUTIL_DISABLEDSHADOWTEXTURE_H

#include <osg/Image>
#include <osg/Texture2D>

#include <cmath>
#include <cstring>
#include <limits>

namespace SceneUtil
{
    // Engine-owned pass dependency, not an authored texture. Keep construction
    // shared with capture tests so an image-backed shadow sentinel is not
    // confused with the preview's separate, image-less depth sentinel.
    inline constexpr char DisabledShadowTextureName[] = "openmw.shadow-disabled.depth-sentinel";

    [[nodiscard]] inline osg::ref_ptr<osg::Texture2D> makeDisabledShadowTexture()
    {
        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(1, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT);
        const float depth = std::numeric_limits<float>::infinity();
        std::memcpy(image->data(), &depth, sizeof(depth));
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setName(DisabledShadowTextureName);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture->setShadowComparison(true);
        texture->setShadowCompareFunc(osg::Texture::ALWAYS);
        return texture;
    }

    // Do not accept arbitrary generated/depth images, an authored file bearing
    // the marker name, or a changed comparison texture. The caller additionally
    // restricts this exception to native character previews.
    [[nodiscard]] inline bool isDisabledShadowTexture(const osg::Texture2D& texture)
    {
        if (texture.getName() != DisabledShadowTextureName || !texture.getShadowComparison()
            || texture.getShadowCompareFunc() != osg::Texture::ALWAYS)
            return false;
        const osg::Image* image = texture.getImage();
        if (!image || !image->getFileName().empty() || image->s() != 1 || image->t() != 1 || image->r() != 1
            || image->getPixelFormat() != GL_DEPTH_COMPONENT || image->getDataType() != GL_FLOAT
            || !image->data() || image->getTotalSizeInBytes() < sizeof(float))
            return false;
        float depth = 0.0f;
        std::memcpy(&depth, image->data(), sizeof(depth));
        return std::isinf(depth) && depth > 0.0f;
    }
}

#endif
