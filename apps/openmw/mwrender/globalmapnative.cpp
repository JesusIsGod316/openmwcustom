#include "globalmap.hpp"

#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)

#include <MyGUI_RenderManager.h>

#include <components/debug/debuglog.hpp>
#include <components/esm3/globalmap.hpp>
#include <components/files/memorystream.hpp>
#include <components/settings/values.hpp>
#include <components/vsgmygui/rendermanager.hpp>

#include <osg/Image>
#include <osg/Texture2D>
#include <osg/Vec4>

#include <osgDB/Registry>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace MWRender
{
    namespace
    {
        constexpr std::string_view NativeBaseTextureName = "openmw-v4-global-map-base";
        constexpr std::string_view NativeOverlayTextureName = "openmw-v4-global-map-overlay";

        [[nodiscard]] std::uint8_t toByte(float value) noexcept
        {
            return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }

        [[nodiscard]] std::vector<std::uint8_t> imageToRgba8(const osg::Image& image)
        {
            if (image.s() <= 0 || image.t() <= 0)
                return {};
            const std::uint64_t pixels
                = static_cast<std::uint64_t>(image.s()) * static_cast<std::uint64_t>(image.t());
            if (pixels > std::numeric_limits<std::size_t>::max() / 4u)
                return {};

            std::vector<std::uint8_t> result(static_cast<std::size_t>(pixels) * 4u);
            for (int y = 0; y < image.t(); ++y)
            {
                for (int x = 0; x < image.s(); ++x)
                {
                    const osg::Vec4 color = image.getColor(x, y);
                    const std::size_t offset
                        = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.s())
                              + static_cast<std::size_t>(x))
                        * 4u;
                    result[offset + 0] = toByte(color.r());
                    result[offset + 1] = toByte(color.g());
                    result[offset + 2] = toByte(color.b());
                    result[offset + 3] = toByte(color.a());
                }
            }
            return result;
        }

        [[nodiscard]] osg::Vec4 sampleRgba8Bilinear(
            std::span<const std::uint8_t> rgba, int width, int height, float x, float y) noexcept
        {
            x = std::clamp(x, 0.0f, static_cast<float>(width - 1));
            y = std::clamp(y, 0.0f, static_cast<float>(height - 1));
            const int x0 = static_cast<int>(std::floor(x));
            const int y0 = static_cast<int>(std::floor(y));
            const int x1 = std::min(x0 + 1, width - 1);
            const int y1 = std::min(y0 + 1, height - 1);
            const float fx = x - static_cast<float>(x0);
            const float fy = y - static_cast<float>(y0);

            const auto sample = [&](int sx, int sy) {
                const std::size_t offset
                    = (static_cast<std::size_t>(sy) * static_cast<std::size_t>(width)
                          + static_cast<std::size_t>(sx))
                    * 4u;
                return osg::Vec4(static_cast<float>(rgba[offset + 0]) / 255.0f,
                    static_cast<float>(rgba[offset + 1]) / 255.0f,
                    static_cast<float>(rgba[offset + 2]) / 255.0f,
                    static_cast<float>(rgba[offset + 3]) / 255.0f);
            };

            const osg::Vec4 top0 = sample(x0, y0) * (1.0f - fx) + sample(x1, y0) * fx;
            const osg::Vec4 top1 = sample(x0, y1) * (1.0f - fx) + sample(x1, y1) * fx;
            return top0 * (1.0f - fy) + top1 * fy;
        }

        [[nodiscard]] osg::Vec4 sampleImageBilinear(const osg::Image& image, float x, float y) noexcept
        {
            x = std::clamp(x, 0.0f, static_cast<float>(image.s() - 1));
            y = std::clamp(y, 0.0f, static_cast<float>(image.t() - 1));
            const int x0 = static_cast<int>(std::floor(x));
            const int y0 = static_cast<int>(std::floor(y));
            const int x1 = std::min(x0 + 1, image.s() - 1);
            const int y1 = std::min(y0 + 1, image.t() - 1);
            const float fx = x - static_cast<float>(x0);
            const float fy = y - static_cast<float>(y0);
            const osg::Vec4 row0 = image.getColor(x0, y0) * (1.0f - fx) + image.getColor(x1, y0) * fx;
            const osg::Vec4 row1 = image.getColor(x0, y1) * (1.0f - fx) + image.getColor(x1, y1) * fx;
            return row0 * (1.0f - fy) + row1 * fy;
        }

        [[nodiscard]] float landMask(const osg::Image* alphaImage, int x, int y) noexcept
        {
            if (!alphaImage || x < 0 || y < 0 || x >= alphaImage->s() || y >= alphaImage->t())
                return 1.0f;
            return std::clamp(alphaImage->getColor(x, y).a(), 0.0f, 1.0f);
        }
    }

    bool GlobalMap::publishNativeTextures()
    {
        ensureLoaded();
        if (!mBaseTexture || !mBaseTexture->getImage() || !mOverlayImage || mWidth <= 0 || mHeight <= 0)
            return false;

        auto* const renderer = dynamic_cast<VsgMyGui::RenderManager*>(&MyGUI::RenderManager::getInstance());
        if (!renderer)
            return false;

        if (!mNativeBasePublished)
        {
            const osg::Image* const baseImage = mBaseTexture->getImage();
            std::vector<std::uint8_t> rgba = imageToRgba8(*baseImage);
            if (rgba.empty()
                || !renderer->setRgba8Texture(
                    std::string(NativeBaseTextureName), rgba, baseImage->s(), baseImage->t()))
                return false;
            mNativeBasePublished = true;
        }

        if (mNativePublishedOverlayRevision != mNativeOverlayRevision)
        {
            std::vector<std::uint8_t> rgba = imageToRgba8(*mOverlayImage);
            if (rgba.empty()
                || !renderer->setRgba8Texture(
                    std::string(NativeOverlayTextureName), rgba, mOverlayImage->s(), mOverlayImage->t()))
                return false;
            mNativePublishedOverlayRevision = mNativeOverlayRevision;
        }
        return true;
    }

    std::string_view GlobalMap::nativeBaseTextureName() const noexcept
    {
        return mNativeBasePublished ? NativeBaseTextureName : std::string_view{};
    }

    std::string_view GlobalMap::nativeOverlayTextureName() const noexcept
    {
        return mNativePublishedOverlayRevision == mNativeOverlayRevision ? NativeOverlayTextureName
                                                                         : std::string_view{};
    }

    bool GlobalMap::exploreCellNative(int cellX, int cellY, std::span<const std::uint8_t> localMapRgba,
        int sourceWidth, int sourceHeight)
    {
        ensureLoaded();
        if (!mOverlayImage || !mOverlayImage->data() || sourceWidth <= 0 || sourceHeight <= 0
            || cellX < mMinX || cellX > mMaxX || cellY < mMinY || cellY > mMaxY)
            return false;

        const std::uint64_t sourcePixels
            = static_cast<std::uint64_t>(sourceWidth) * static_cast<std::uint64_t>(sourceHeight);
        if (sourcePixels > std::numeric_limits<std::size_t>::max() / 4u
            || localMapRgba.size() != static_cast<std::size_t>(sourcePixels) * 4u)
            return false;

        const int cellSize = Settings::map().mGlobalMapCellSize;
        if (cellSize <= 0)
            return false;
        const int originX = (cellX - mMinX) * cellSize;
        const int originY = (cellY - mMinY) * cellSize;
        const osg::Image* const alphaImage = mAlphaTexture ? mAlphaTexture->getImage() : nullptr;

        // Both the legacy RTT texture and the retained VSG readback use the map
        // texture's bottom-left/Y-up convention. Resampling therefore keeps Y
        // increasing with world-cell Y; MyGUI performs the same final UV flip as
        // the established global-map image path.
        for (int y = 0; y < cellSize; ++y)
        {
            const float sourceY
                = ((static_cast<float>(y) + 0.5f) / static_cast<float>(cellSize)) * sourceHeight - 0.5f;
            for (int x = 0; x < cellSize; ++x)
            {
                const float sourceX
                    = ((static_cast<float>(x) + 0.5f) / static_cast<float>(cellSize)) * sourceWidth - 0.5f;
                osg::Vec4 color = sampleRgba8Bilinear(localMapRgba, sourceWidth, sourceHeight, sourceX, sourceY);
                const int destinationX = originX + x;
                const int destinationY = originY + y;
                color.a() *= landMask(alphaImage, destinationX, destinationY);
                mOverlayImage->setColor(color, destinationX, destinationY);
            }
        }
        mOverlayImage->dirty();
        if (mNativeOverlayRevision == std::numeric_limits<std::uint64_t>::max())
            return false;
        ++mNativeOverlayRevision;
        return publishNativeTextures();
    }

    void GlobalMap::clearNative()
    {
        ensureLoaded();
        if (!mOverlayImage || !mOverlayImage->data())
            return;
        std::memset(mOverlayImage->data(), 0, mOverlayImage->getTotalSizeInBytes());
        mOverlayImage->dirty();
        mPendingImageDest.clear();
        if (mNativeOverlayRevision != std::numeric_limits<std::uint64_t>::max())
            ++mNativeOverlayRevision;
        (void)publishNativeTextures();
    }

    void GlobalMap::readNative(ESM::GlobalMap& map)
    {
        ensureLoaded();
        if (!mOverlayImage || !mOverlayImage->data())
            return;

        const ESM::GlobalMap::Bounds& bounds = map.mBounds;
        if (bounds.mMaxX - bounds.mMinX < 0 || bounds.mMaxY - bounds.mMinY < 0)
            return;
        if (bounds.mMinX > bounds.mMaxX || bounds.mMinY > bounds.mMaxY)
            throw std::runtime_error("invalid map bounds");
        if (map.mImageData.empty())
            return;

        osgDB::ReaderWriter* const reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
        if (!reader)
        {
            Log(Debug::Error) << "Error: Can't read native global map overlay: no png readerwriter found";
            return;
        }
        Files::IMemStream stream(map.mImageData.data(), map.mImageData.size());
        osgDB::ReaderWriter::ReadResult decoded = reader->readImage(stream);
        if (!decoded.success())
        {
            Log(Debug::Error) << "Error: Can't read native global map overlay: " << decoded.message() << " code "
                              << decoded.status();
            return;
        }
        osg::ref_ptr<osg::Image> source = decoded.getImage();
        if (!source || source->s() <= 0 || source->t() <= 0)
            return;

        const int sourceCellsX = bounds.mMaxX - bounds.mMinX + 1;
        const int sourceCellsY = bounds.mMaxY - bounds.mMinY + 1;
        const int sourceCellSize = source->s() / sourceCellsX;
        if (sourceCellSize <= 0 || source->t() / sourceCellsY != sourceCellSize
            || sourceCellSize * sourceCellsX != source->s() || sourceCellSize * sourceCellsY != source->t())
            throw std::runtime_error("cell size must be quadratic");

        std::memset(mOverlayImage->data(), 0, mOverlayImage->getTotalSizeInBytes());
        const int destinationCellSize = Settings::map().mGlobalMapCellSize;
        const osg::Image* const alphaImage = mAlphaTexture ? mAlphaTexture->getImage() : nullptr;
        const int firstX = std::max(bounds.mMinX, mMinX);
        const int lastX = std::min(bounds.mMaxX, mMaxX);
        const int firstY = std::max(bounds.mMinY, mMinY);
        const int lastY = std::min(bounds.mMaxY, mMaxY);

        for (int cellY = firstY; cellY <= lastY; ++cellY)
        {
            for (int cellX = firstX; cellX <= lastX; ++cellX)
            {
                const int sourceOriginX = (cellX - bounds.mMinX) * sourceCellSize;
                const int sourceOriginY = (cellY - bounds.mMinY) * sourceCellSize;
                const int destinationOriginX = (cellX - mMinX) * destinationCellSize;
                const int destinationOriginY = (cellY - mMinY) * destinationCellSize;
                for (int y = 0; y < destinationCellSize; ++y)
                {
                    const float sourceY = sourceOriginY
                        + ((static_cast<float>(y) + 0.5f) / static_cast<float>(destinationCellSize))
                            * sourceCellSize
                        - 0.5f;
                    for (int x = 0; x < destinationCellSize; ++x)
                    {
                        const float sourceX = sourceOriginX
                            + ((static_cast<float>(x) + 0.5f) / static_cast<float>(destinationCellSize))
                                * sourceCellSize
                            - 0.5f;
                        osg::Vec4 color = sampleImageBilinear(*source, sourceX, sourceY);
                        const int destinationX = destinationOriginX + x;
                        const int destinationY = destinationOriginY + y;
                        color.a() *= landMask(alphaImage, destinationX, destinationY);
                        mOverlayImage->setColor(color, destinationX, destinationY);
                    }
                }
            }
        }

        mOverlayImage->dirty();
        mPendingImageDest.clear();
        if (mNativeOverlayRevision == std::numeric_limits<std::uint64_t>::max())
            throw std::runtime_error("native global-map overlay revision exhausted");
        ++mNativeOverlayRevision;
        (void)publishNativeTextures();
    }
}

#endif
