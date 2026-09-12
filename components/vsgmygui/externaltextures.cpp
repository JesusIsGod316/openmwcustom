#include "rendermanager.hpp"

#include <cstring>
#include <limits>

namespace VsgMyGui
{
    Texture* RenderManager::setRgba8Texture(
        const std::string& name, std::span<const std::uint8_t> rgba, int width, int height)
    {
        if (name.empty() || width <= 0 || height <= 0)
            return nullptr;
        const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        if (pixelCount > std::numeric_limits<std::size_t>::max() / 4 || rgba.size() != pixelCount * 4)
            return nullptr;

        auto data = vsg::ubvec4Array2D::create(
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height),
            vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        if (!data || !data->dataPointer())
            return nullptr;
        std::memcpy(data->dataPointer(), rgba.data(), rgba.size());

        auto it = mTextures.find(name);
        if (it == mTextures.end())
        {
            auto [created, inserted] = mTextures.emplace(name, Texture(name));
            (void)inserted;
            created->second.setDecoder(mDecoder);
            it = created;
        }
        it->second.setData(std::move(data));
        return &it->second;
    }

    bool RenderManager::removeTexture(const std::string& name) noexcept
    {
        // Collected batches snapshot identity/revision plus strong VSG backing references,
        // so erasing the registry entry cannot invalidate an in-flight or retired overlay graph.
        return mTextures.erase(name) <= 1;
    }
}
